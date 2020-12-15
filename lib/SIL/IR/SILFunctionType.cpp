//===--- SILFunctionType.cpp - Giving SIL types to AST functions ----------===//
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
// This file defines the native Swift ownership transfer conventions
// and works in concert with the importer to give the correct
// conventions to imported functions and types.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "libsil"
#include "swift/AST/AnyFunctionRef.h"
#include "swift/AST/CanTypeVisitor.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/ForeignInfo.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/Module.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Analysis/DomainSpecific/CocoaConventions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;
using namespace swift::Lowering;

SILType SILFunctionType::substInterfaceType(SILModule &M,
                                            SILType interfaceType,
                                            TypeExpansionContext context) const {
  // Apply pattern substitutions first, then invocation substitutions.
  if (auto subs = getPatternSubstitutions())
    interfaceType = interfaceType.subst(M, subs, context);
  if (auto subs = getInvocationSubstitutions())
    interfaceType = interfaceType.subst(M, subs, context);
  return interfaceType;
}

CanSILFunctionType SILFunctionType::getUnsubstitutedType(SILModule &M) const {
  auto mutableThis = const_cast<SILFunctionType*>(this);

  // If we have no substitutions, there's nothing to do.
  if (!hasPatternSubstitutions() && !hasInvocationSubstitutions())
    return CanSILFunctionType(mutableThis);

  // Otherwise, substitute the component types.

  SmallVector<SILParameterInfo, 4> params;
  SmallVector<SILYieldInfo, 4> yields;
  SmallVector<SILResultInfo, 4> results;
  Optional<SILResultInfo> errorResult;

  auto subs = getCombinedSubstitutions();
  auto substComponentType = [&](CanType type) {
    if (!type->hasTypeParameter()) return type;
    return SILType::getPrimitiveObjectType(type)
             .subst(M, subs).getASTType();
  };
  
  for (auto param : getParameters()) {
    params.push_back(param.map(substComponentType));
  }
  
  for (auto yield : getYields()) {
    yields.push_back(yield.map(substComponentType));
  }
  
  for (auto result : getResults()) {
    results.push_back(result.map(substComponentType));
  }
  
  if (auto error = getOptionalErrorResult()) {
    errorResult = error->map(substComponentType);
  }

  auto signature = isPolymorphic() ? getInvocationGenericSignature()
                                   : CanGenericSignature();
  return SILFunctionType::get(signature,
                              getExtInfo(),
                              getCoroutineKind(),
                              getCalleeConvention(),
                              params, yields, results, errorResult,
                              SubstitutionMap(),
                              SubstitutionMap(),
                              mutableThis->getASTContext(),
                              getWitnessMethodConformanceOrInvalid());
}

CanType SILParameterInfo::getArgumentType(SILModule &M,
                                          const SILFunctionType *t,
                                          TypeExpansionContext context) const {
  // TODO: We should always require a function type.
  if (t)
    return t
        ->substInterfaceType(
            M, SILType::getPrimitiveAddressType(getInterfaceType()), context)
        .getASTType();

  return getInterfaceType();
}

CanType SILResultInfo::getReturnValueType(SILModule &M,
                                          const SILFunctionType *t,
                                          TypeExpansionContext context) const {
  // TODO: We should always require a function type.
  if (t)
    return t
        ->substInterfaceType(
            M, SILType::getPrimitiveAddressType(getInterfaceType()), context)
        .getASTType();

  return getInterfaceType();
}

SILType
SILFunctionType::getDirectFormalResultsType(SILModule &M,
                                            TypeExpansionContext context) {
  CanType type;
  if (getNumDirectFormalResults() == 0) {
    type = getASTContext().TheEmptyTupleType;
  } else if (getNumDirectFormalResults() == 1) {
    type = getSingleDirectFormalResult().getReturnValueType(M, this, context);
  } else {
    auto &cache = getMutableFormalResultsCache();
    if (cache) {
      type = cache;
    } else {
      SmallVector<TupleTypeElt, 4> elts;
      for (auto result : getResults())
        if (!result.isFormalIndirect())
          elts.push_back(result.getReturnValueType(M, this, context));
      type = CanType(TupleType::get(elts, getASTContext()));
      cache = type;
    }
  }
  return SILType::getPrimitiveObjectType(type);
}

SILType SILFunctionType::getAllResultsInterfaceType() {
  CanType type;
  if (getNumResults() == 0) {
    type = getASTContext().TheEmptyTupleType;
  } else if (getNumResults() == 1) {
    type = getResults()[0].getInterfaceType();
  } else {
    auto &cache = getMutableAllResultsCache();
    if (cache) {
      type = cache;
    } else {
      SmallVector<TupleTypeElt, 4> elts;
      for (auto result : getResults())
        elts.push_back(result.getInterfaceType());
      type = CanType(TupleType::get(elts, getASTContext()));
      cache = type;
    }
  }
  return SILType::getPrimitiveObjectType(type);
}

SILType SILFunctionType::getAllResultsSubstType(SILModule &M,
                                                TypeExpansionContext context) {
  return substInterfaceType(M, getAllResultsInterfaceType(), context);
}

SILType SILFunctionType::getFormalCSemanticResult(SILModule &M) {
  assert(getLanguage() == SILFunctionLanguage::C);
  assert(getNumResults() <= 1);
  return getDirectFormalResultsType(M, TypeExpansionContext::minimal());
}

CanType
SILFunctionType::getSelfInstanceType(SILModule &M,
                                     TypeExpansionContext context) const {
  auto selfTy = getSelfParameter().getArgumentType(M, this, context);

  // If this is a static method, get the instance type.
  if (auto metaTy = dyn_cast<AnyMetatypeType>(selfTy))
    return metaTy.getInstanceType();

  return selfTy;
}

ClassDecl *
SILFunctionType::getWitnessMethodClass(SILModule &M,
                                       TypeExpansionContext context) const {
  // TODO: When witnesses use substituted types, we'd get this from the
  // substitution map.
  auto selfTy = getSelfInstanceType(M, context);
  auto genericSig = getSubstGenericSignature();
  if (auto paramTy = dyn_cast<GenericTypeParamType>(selfTy)) {
    assert(paramTy->getDepth() == 0 && paramTy->getIndex() == 0);
    auto superclass = genericSig->getSuperclassBound(paramTy);
    if (superclass)
      return superclass->getClassOrBoundGenericClass();
  }

  return nullptr;
}

IndexSubset *
SILFunctionType::getDifferentiabilityParameterIndices() {
  assert(isDifferentiable() && "Must be a differentiable function");
  SmallVector<unsigned, 8> paramIndices;
  for (auto paramAndIndex : enumerate(getParameters()))
    if (paramAndIndex.value().getDifferentiability() !=
        SILParameterDifferentiability::NotDifferentiable)
      paramIndices.push_back(paramAndIndex.index());
  return IndexSubset::get(getASTContext(), getNumParameters(), paramIndices);
}

IndexSubset *SILFunctionType::getDifferentiabilityResultIndices() {
  assert(isDifferentiable() && "Must be a differentiable function");
  SmallVector<unsigned, 8> resultIndices;
  // Check formal results.
  for (auto resultAndIndex : enumerate(getResults()))
    if (resultAndIndex.value().getDifferentiability() !=
        SILResultDifferentiability::NotDifferentiable)
      resultIndices.push_back(resultAndIndex.index());
  // Check `inout` parameters.
  for (auto inoutParamAndIndex : enumerate(getIndirectMutatingParameters()))
    // FIXME(TF-1305): The `getResults().empty()` condition is a hack.
    //
    // Currently, an `inout` parameter can either be:
    // 1. Both a differentiability parameter and a differentiability result.
    // 2. `@noDerivative`: neither a differentiability parameter nor a
    //    differentiability result.
    // However, there is no way to represent an `inout` parameter that:
    // 3. Is a differentiability result but not a differentiability parameter.
    // 4. Is a differentiability parameter but not a differentiability result.
    //    This case is not currently expressible and does not yet have clear use
    //    cases, so supporting it is a non-goal.
    //
    // See TF-1305 for solution ideas. For now, `@noDerivative` `inout`
    // parameters are not treated as differentiability results, unless the
    // original function has no formal results, in which case all `inout`
    // parameters are treated as differentiability results.
    if (getResults().empty() ||
        inoutParamAndIndex.value().getDifferentiability() !=
            SILParameterDifferentiability::NotDifferentiable)
      resultIndices.push_back(getNumResults() + inoutParamAndIndex.index());
  auto numSemanticResults =
      getNumResults() + getNumIndirectMutatingParameters();
  return IndexSubset::get(getASTContext(), numSemanticResults, resultIndices);
}

CanSILFunctionType
SILFunctionType::getWithDifferentiability(DifferentiabilityKind kind,
                                          IndexSubset *parameterIndices,
                                          IndexSubset *resultIndices) {
  assert(kind != DifferentiabilityKind::NonDifferentiable &&
         "Differentiability kind must be normal or linear");
  SmallVector<SILParameterInfo, 8> newParameters;
  for (auto paramAndIndex : enumerate(getParameters())) {
    auto &param = paramAndIndex.value();
    unsigned index = paramAndIndex.index();
    newParameters.push_back(param.getWithDifferentiability(
        index < parameterIndices->getCapacity() &&
                parameterIndices->contains(index)
            ? SILParameterDifferentiability::DifferentiableOrNotApplicable
            : SILParameterDifferentiability::NotDifferentiable));
  }
  SmallVector<SILResultInfo, 8> newResults;
  for (auto resultAndIndex : enumerate(getResults())) {
    auto &result = resultAndIndex.value();
    unsigned index = resultAndIndex.index();
    newResults.push_back(result.getWithDifferentiability(
        index < resultIndices->getCapacity() && resultIndices->contains(index)
            ? SILResultDifferentiability::DifferentiableOrNotApplicable
            : SILResultDifferentiability::NotDifferentiable));
  }
  auto newExtInfo =
      getExtInfo().intoBuilder().withDifferentiabilityKind(kind).build();
  return get(getInvocationGenericSignature(), newExtInfo, getCoroutineKind(),
             getCalleeConvention(), newParameters, getYields(), newResults,
             getOptionalErrorResult(), getPatternSubstitutions(),
             getInvocationSubstitutions(), getASTContext(),
             getWitnessMethodConformanceOrInvalid());
}

CanSILFunctionType SILFunctionType::getWithoutDifferentiability() {
  if (!isDifferentiable())
    return CanSILFunctionType(this);
  auto nondiffExtInfo =
      getExtInfo()
          .intoBuilder()
          .withDifferentiabilityKind(DifferentiabilityKind::NonDifferentiable)
          .build();
  SmallVector<SILParameterInfo, 8> newParams;
  for (auto &param : getParameters())
    newParams.push_back(param.getWithDifferentiability(
        SILParameterDifferentiability::DifferentiableOrNotApplicable));
  SmallVector<SILResultInfo, 8> newResults;
  for (auto &result : getResults())
    newResults.push_back(result.getWithDifferentiability(
        SILResultDifferentiability::DifferentiableOrNotApplicable));
  return SILFunctionType::get(
      getInvocationGenericSignature(), nondiffExtInfo, getCoroutineKind(),
      getCalleeConvention(), newParams, getYields(), newResults,
      getOptionalErrorResult(), getPatternSubstitutions(),
      getInvocationSubstitutions(), getASTContext());
}

/// Collects the differentiability parameters of the given original function
/// type in `diffParams`.
static void
getDifferentiabilityParameters(SILFunctionType *originalFnTy,
                               IndexSubset *parameterIndices,
                               SmallVectorImpl<SILParameterInfo> &diffParams) {
  // Returns true if `index` is a differentiability parameter index.
  auto isDiffParamIndex = [&](unsigned index) -> bool {
    return index < parameterIndices->getCapacity() &&
           parameterIndices->contains(index);
  };
  // Calculate differentiability parameter infos.
  for (auto valueAndIndex : enumerate(originalFnTy->getParameters()))
    if (isDiffParamIndex(valueAndIndex.index()))
      diffParams.push_back(valueAndIndex.value());
}

/// Collects the semantic results of the given function type in
/// `originalResults`. The semantic results are formal results followed by
/// `inout` parameters, in type order.
static void
getSemanticResults(SILFunctionType *functionType, IndexSubset *parameterIndices,
                   IndexSubset *&inoutParameterIndices,
                   SmallVectorImpl<SILResultInfo> &originalResults) {
  auto &C = functionType->getASTContext();
  SmallVector<unsigned, 4> inoutParamIndices;
  // Collect original formal results.
  originalResults.append(functionType->getResults().begin(),
                         functionType->getResults().end());
  // Collect original `inout` parameters.
  for (auto i : range(functionType->getNumParameters())) {
    auto param = functionType->getParameters()[i];
    if (!param.isIndirectInOut())
      continue;
    inoutParamIndices.push_back(i);
    originalResults.push_back(
        SILResultInfo(param.getInterfaceType(), ResultConvention::Indirect));
  }
  inoutParameterIndices =
      IndexSubset::get(C, parameterIndices->getCapacity(), inoutParamIndices);
}

/// Returns the differential type for the given original function type,
/// parameter indices, and result index.
static CanSILFunctionType getAutoDiffDifferentialType(
    SILFunctionType *originalFnTy, IndexSubset *parameterIndices,
    IndexSubset *resultIndices, LookupConformanceFn lookupConformance,
    TypeConverter &TC) {
  // Given the tangent type and the corresponding original parameter's
  // convention, returns the tangent parameter's convention.
  auto getTangentParameterConvention =
      [&](CanType tanType,
          ParameterConvention origParamConv) -> ParameterConvention {
    tanType =
        tanType->getCanonicalType(originalFnTy->getSubstGenericSignature());
    AbstractionPattern pattern(originalFnTy->getSubstGenericSignature(),
                               tanType);
    auto &tl =
        TC.getTypeLowering(pattern, tanType, TypeExpansionContext::minimal());
    // When the tangent type is address only, we must ensure that the tangent
    // parameter's convention is indirect.
    if (tl.isAddressOnly() && !isIndirectFormalParameter(origParamConv)) {
      switch (origParamConv) {
      case ParameterConvention::Direct_Guaranteed:
        return ParameterConvention::Indirect_In_Guaranteed;
      case ParameterConvention::Direct_Owned:
      case ParameterConvention::Direct_Unowned:
        return ParameterConvention::Indirect_In;
      default:
        llvm_unreachable("unhandled parameter convention");
      }
    }
    return origParamConv;
  };

  // Given the tangent type and the corresponding original result's convention,
  // returns the tangent result's convention.
  auto getTangentResultConvention =
      [&](CanType tanType,
          ResultConvention origResConv) -> ResultConvention {
    tanType =
        tanType->getCanonicalType(originalFnTy->getSubstGenericSignature());
    AbstractionPattern pattern(originalFnTy->getSubstGenericSignature(),
                               tanType);
    auto &tl =
        TC.getTypeLowering(pattern, tanType, TypeExpansionContext::minimal());
    // When the tangent type is address only, we must ensure that the tangent
    // result's convention is indirect.
    if (tl.isAddressOnly() && !isIndirectFormalResult(origResConv)) {
      switch (origResConv) {
      case ResultConvention::Unowned:
      case ResultConvention::Owned:
        return ResultConvention::Indirect;
      default:
        llvm_unreachable("unhandled result convention");
      }
    }
    return origResConv;
  };

  auto &ctx = originalFnTy->getASTContext();
  SmallVector<GenericTypeParamType *, 4> substGenericParams;
  SmallVector<Requirement, 4> substRequirements;
  SmallVector<Type, 4> substReplacements;
  SmallVector<ProtocolConformanceRef, 4> substConformances;

  IndexSubset *inoutParamIndices;
  SmallVector<SILResultInfo, 2> originalResults;
  getSemanticResults(originalFnTy, parameterIndices, inoutParamIndices,
                     originalResults);

  SmallVector<SILParameterInfo, 4> diffParams;
  getDifferentiabilityParameters(originalFnTy, parameterIndices, diffParams);
  SmallVector<SILParameterInfo, 8> differentialParams;
  for (auto &param : diffParams) {
    auto paramTan =
        param.getInterfaceType()->getAutoDiffTangentSpace(lookupConformance);
    assert(paramTan && "Parameter type does not have a tangent space?");
    auto paramTanType = paramTan->getCanonicalType();
    auto paramConv = getTangentParameterConvention(paramTanType,
                                                   param.getConvention());
    if (!paramTanType->hasArchetype() && !paramTanType->hasTypeParameter()) {
      differentialParams.push_back(
          {paramTan->getCanonicalType(), paramConv});
    } else {
      auto gpIndex = substGenericParams.size();
      auto gpType = CanGenericTypeParamType::get(0, gpIndex, ctx);
      substGenericParams.push_back(gpType);
      substReplacements.push_back(paramTanType);
      differentialParams.push_back({gpType, paramConv});
    }
  }
  SmallVector<SILResultInfo, 1> differentialResults;
  for (auto resultIndex : resultIndices->getIndices()) {
    // Handle formal original result.
    if (resultIndex < originalFnTy->getNumResults()) {
      auto &result = originalResults[resultIndex];
      auto resultTan =
          result.getInterfaceType()->getAutoDiffTangentSpace(lookupConformance);
      assert(resultTan && "Result type does not have a tangent space?");
      auto resultTanType = resultTan->getCanonicalType();
      auto resultConv =
          getTangentResultConvention(resultTanType, result.getConvention());
      if (!resultTanType->hasArchetype() &&
          !resultTanType->hasTypeParameter()) {
        differentialResults.push_back(
            {resultTan->getCanonicalType(), resultConv});
      } else {
        auto gpIndex = substGenericParams.size();
        auto gpType = CanGenericTypeParamType::get(0, gpIndex, ctx);
        substGenericParams.push_back(gpType);
        substReplacements.push_back(resultTanType);
        differentialResults.push_back({gpType, resultConv});
      }
      continue;
    }
    // Handle original `inout` parameter.
    auto inoutParamIndex = resultIndex - originalFnTy->getNumResults();
    auto inoutParamIt = std::next(
        originalFnTy->getIndirectMutatingParameters().begin(), inoutParamIndex);
    auto paramIndex =
        std::distance(originalFnTy->getParameters().begin(), &*inoutParamIt);
    // If the original `inout` parameter is a differentiability parameter, then
    // it already has a corresponding differential parameter. Skip adding a
    // corresponding differential result.
    if (parameterIndices->contains(paramIndex))
      continue;
    auto inoutParam = originalFnTy->getParameters()[paramIndex];
    auto paramTan = inoutParam.getInterfaceType()->getAutoDiffTangentSpace(
        lookupConformance);
    assert(paramTan && "Parameter type does not have a tangent space?");
    differentialResults.push_back(
        {paramTan->getCanonicalType(), ResultConvention::Indirect});
  }

  SubstitutionMap substitutions;
  if (!substGenericParams.empty()) {
    auto genericSig =
        GenericSignature::get(substGenericParams, substRequirements)
            .getCanonicalSignature();
    substitutions =
        SubstitutionMap::get(genericSig, llvm::makeArrayRef(substReplacements),
                             llvm::makeArrayRef(substConformances));
  }
  return SILFunctionType::get(
      GenericSignature(), SILFunctionType::ExtInfo(), SILCoroutineKind::None,
      ParameterConvention::Direct_Guaranteed, differentialParams, {},
      differentialResults, None, substitutions,
      /*invocationSubstitutions*/ SubstitutionMap(), ctx);
}

/// Returns the pullback type for the given original function type, parameter
/// indices, and result index.
static CanSILFunctionType getAutoDiffPullbackType(
    SILFunctionType *originalFnTy, IndexSubset *parameterIndices,
    IndexSubset *resultIndices, LookupConformanceFn lookupConformance,
    TypeConverter &TC) {
  auto &ctx = originalFnTy->getASTContext();
  SmallVector<GenericTypeParamType *, 4> substGenericParams;
  SmallVector<Requirement, 4> substRequirements;
  SmallVector<Type, 4> substReplacements;
  SmallVector<ProtocolConformanceRef, 4> substConformances;

  IndexSubset *inoutParamIndices;
  SmallVector<SILResultInfo, 2> originalResults;
  getSemanticResults(originalFnTy, parameterIndices, inoutParamIndices,
                     originalResults);

  // Given a type, returns its formal SIL parameter info.
  auto getTangentParameterConventionForOriginalResult =
      [&](CanType tanType,
          ResultConvention origResConv) -> ParameterConvention {
    tanType =
        tanType->getCanonicalType(originalFnTy->getSubstGenericSignature());
    AbstractionPattern pattern(originalFnTy->getSubstGenericSignature(),
                               tanType);
    auto &tl =
        TC.getTypeLowering(pattern, tanType, TypeExpansionContext::minimal());
    ParameterConvention conv;
    switch (origResConv) {
    case ResultConvention::Unowned:
    case ResultConvention::UnownedInnerPointer:
    case ResultConvention::Owned:
    case ResultConvention::Autoreleased:
      if (tl.isAddressOnly()) {
        conv = ParameterConvention::Indirect_In_Guaranteed;
      } else {
        conv = tl.isTrivial() ? ParameterConvention::Direct_Unowned
                              : ParameterConvention::Direct_Guaranteed;
      }
      break;
    case ResultConvention::Indirect:
      conv = ParameterConvention::Indirect_In_Guaranteed;
      break;
    }
    return conv;
  };

  // Given a type, returns its formal SIL result info.
  auto getTangentResultConventionForOriginalParameter =
      [&](CanType tanType,
          ParameterConvention origParamConv) -> ResultConvention {
    tanType =
        tanType->getCanonicalType(originalFnTy->getSubstGenericSignature());
    AbstractionPattern pattern(originalFnTy->getSubstGenericSignature(),
                               tanType);
    auto &tl =
        TC.getTypeLowering(pattern, tanType, TypeExpansionContext::minimal());
    ResultConvention conv;
    switch (origParamConv) {
    case ParameterConvention::Direct_Owned:
    case ParameterConvention::Direct_Guaranteed:
    case ParameterConvention::Direct_Unowned:
      if (tl.isAddressOnly()) {
        conv = ResultConvention::Indirect;
      } else {
        conv = tl.isTrivial() ? ResultConvention::Unowned
                              : ResultConvention::Owned;
      }
      break;
    case ParameterConvention::Indirect_In:
    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Indirect_In_Constant:
    case ParameterConvention::Indirect_In_Guaranteed:
    case ParameterConvention::Indirect_InoutAliasable:
      conv = ResultConvention::Indirect;
      break;
    }
    return conv;
  };

  // Collect pullback parameters.
  SmallVector<SILParameterInfo, 1> pullbackParams;
  for (auto resultIndex : resultIndices->getIndices()) {
    // Handle formal original result.
    if (resultIndex < originalFnTy->getNumResults()) {
      auto &origRes = originalResults[resultIndex];
      auto resultTan = origRes.getInterfaceType()->getAutoDiffTangentSpace(
          lookupConformance);
      assert(resultTan && "Result type does not have a tangent space?");
      auto resultTanType = resultTan->getCanonicalType();
      auto paramTanConvention = getTangentParameterConventionForOriginalResult(
          resultTanType, origRes.getConvention());
      if (!resultTanType->hasArchetype() &&
          !resultTanType->hasTypeParameter()) {
        auto resultTanType = resultTan->getCanonicalType();
        pullbackParams.push_back({resultTanType, paramTanConvention});
      } else {
        auto gpIndex = substGenericParams.size();
        auto gpType = CanGenericTypeParamType::get(0, gpIndex, ctx);
        substGenericParams.push_back(gpType);
        substReplacements.push_back(resultTanType);
        pullbackParams.push_back({gpType, paramTanConvention});
      }
      continue;
    }
    // Handle original `inout` parameter.
    auto inoutParamIndex = resultIndex - originalFnTy->getNumResults();
    auto inoutParamIt = std::next(
        originalFnTy->getIndirectMutatingParameters().begin(), inoutParamIndex);
    auto paramIndex =
        std::distance(originalFnTy->getParameters().begin(), &*inoutParamIt);
    auto inoutParam = originalFnTy->getParameters()[paramIndex];
    auto paramTan = inoutParam.getInterfaceType()->getAutoDiffTangentSpace(
        lookupConformance);
    assert(paramTan && "Parameter type does not have a tangent space?");
    // The pullback parameter convention depends on whether the original `inout`
    // paramater is a differentiability parameter.
    // - If yes, the pullback parameter convention is `@inout`.
    // - If no, the pullback parameter convention is `@in_guaranteed`.
    bool isWrtInoutParameter = parameterIndices->contains(paramIndex);
    auto paramTanConvention = isWrtInoutParameter
                                  ? inoutParam.getConvention()
                                  : ParameterConvention::Indirect_In_Guaranteed;
    auto paramTanType = paramTan->getCanonicalType();
    if (!paramTanType->hasArchetype() && !paramTanType->hasTypeParameter()) {
      pullbackParams.push_back(
          SILParameterInfo(paramTanType, paramTanConvention));
    } else {
      auto gpIndex = substGenericParams.size();
      auto gpType = CanGenericTypeParamType::get(0, gpIndex, ctx);
      substGenericParams.push_back(gpType);
      substReplacements.push_back(paramTanType);
      pullbackParams.push_back({gpType, paramTanConvention});
    }
  }

  // Collect pullback results.
  SmallVector<SILParameterInfo, 4> diffParams;
  getDifferentiabilityParameters(originalFnTy, parameterIndices, diffParams);
  SmallVector<SILResultInfo, 8> pullbackResults;
  for (auto &param : diffParams) {
    // Skip `inout` parameters, which semantically behave as original results
    // and always appear as pullback parameters.
    if (param.isIndirectInOut())
      continue;
    auto paramTan =
        param.getInterfaceType()->getAutoDiffTangentSpace(lookupConformance);
    assert(paramTan && "Parameter type does not have a tangent space?");
    auto paramTanType = paramTan->getCanonicalType();
    auto resultTanConvention = getTangentResultConventionForOriginalParameter(
        paramTanType, param.getConvention());
    if (!paramTanType->hasArchetype() && !paramTanType->hasTypeParameter()) {
      pullbackResults.push_back({paramTanType, resultTanConvention});
    } else {
      auto gpIndex = substGenericParams.size();
      auto gpType = CanGenericTypeParamType::get(0, gpIndex, ctx);
      substGenericParams.push_back(gpType);
      substReplacements.push_back(paramTanType);
      pullbackResults.push_back({gpType, resultTanConvention});
    }
  }
  SubstitutionMap substitutions;
  if (!substGenericParams.empty()) {
    auto genericSig =
        GenericSignature::get(substGenericParams, substRequirements)
            .getCanonicalSignature();
    substitutions =
        SubstitutionMap::get(genericSig, llvm::makeArrayRef(substReplacements),
                             llvm::makeArrayRef(substConformances));
  }
  return SILFunctionType::get(
      GenericSignature(), SILFunctionType::ExtInfo(), SILCoroutineKind::None,
      ParameterConvention::Direct_Guaranteed, pullbackParams, {},
      pullbackResults, None, substitutions,
      /*invocationSubstitutions*/ SubstitutionMap(), ctx);
}

/// Constrains the `original` function type according to differentiability
/// requirements:
/// - All differentiability parameters are constrained to conform to
///   `Differentiable`.
/// - The invocation generic signature is replaced by the
///   `constrainedInvocationGenSig` argument.
static SILFunctionType *getConstrainedAutoDiffOriginalFunctionType(
    SILFunctionType *original, IndexSubset *parameterIndices,
    LookupConformanceFn lookupConformance,
    CanGenericSignature constrainedInvocationGenSig) {
  auto originalInvocationGenSig = original->getInvocationGenericSignature();
  if (!originalInvocationGenSig) {
    assert(!constrainedInvocationGenSig ||
           constrainedInvocationGenSig->areAllParamsConcrete() &&
               "derivative function cannot have invocation generic signature "
               "when original function doesn't");
    return original;
  }

  assert(!original->getPatternSubstitutions() &&
         "cannot constrain substituted function type");
  if (!constrainedInvocationGenSig)
    constrainedInvocationGenSig = originalInvocationGenSig;
  if (!constrainedInvocationGenSig)
    return original;
  constrainedInvocationGenSig =
      autodiff::getConstrainedDerivativeGenericSignature(
          original, parameterIndices, constrainedInvocationGenSig,
          lookupConformance)
          .getCanonicalSignature();

  SmallVector<SILParameterInfo, 4> newParameters;
  newParameters.reserve(original->getNumParameters());
  for (auto &param : original->getParameters()) {
    newParameters.push_back(
        param.getWithInterfaceType(param.getInterfaceType()->getCanonicalType(
            constrainedInvocationGenSig)));
  }

  SmallVector<SILResultInfo, 4> newResults;
  newResults.reserve(original->getNumResults());
  for (auto &result : original->getResults()) {
    newResults.push_back(
        result.getWithInterfaceType(result.getInterfaceType()->getCanonicalType(
            constrainedInvocationGenSig)));
  }
  return SILFunctionType::get(
      constrainedInvocationGenSig->areAllParamsConcrete()
          ? GenericSignature()
          : constrainedInvocationGenSig,
      original->getExtInfo(), original->getCoroutineKind(),
      original->getCalleeConvention(), newParameters, original->getYields(),
      newResults, original->getOptionalErrorResult(),
      /*patternSubstitutions*/ SubstitutionMap(),
      /*invocationSubstitutions*/ SubstitutionMap(), original->getASTContext(),
      original->getWitnessMethodConformanceOrInvalid());
}

CanSILFunctionType SILFunctionType::getAutoDiffDerivativeFunctionType(
    IndexSubset *parameterIndices, IndexSubset *resultIndices,
    AutoDiffDerivativeFunctionKind kind, TypeConverter &TC,
    LookupConformanceFn lookupConformance,
    CanGenericSignature derivativeFnInvocationGenSig,
    bool isReabstractionThunk) {
  assert(parameterIndices);
  assert(!parameterIndices->isEmpty() && "Parameter indices must not be empty");
  assert(resultIndices);
  assert(!resultIndices->isEmpty() && "Result indices must not be empty");
  auto &ctx = getASTContext();

  // Look up result in cache.
  SILAutoDiffDerivativeFunctionKey key{this,
                                       parameterIndices,
                                       resultIndices,
                                       kind,
                                       derivativeFnInvocationGenSig,
                                       isReabstractionThunk};
  auto insertion =
      ctx.SILAutoDiffDerivativeFunctions.try_emplace(key, CanSILFunctionType());
  auto &cachedResult = insertion.first->getSecond();
  if (!insertion.second)
    return cachedResult;

  SILFunctionType *constrainedOriginalFnTy =
      getConstrainedAutoDiffOriginalFunctionType(this, parameterIndices,
                                                 lookupConformance,
                                                 derivativeFnInvocationGenSig);
  // Compute closure type.
  CanSILFunctionType closureType;
  switch (kind) {
  case AutoDiffDerivativeFunctionKind::JVP:
    closureType =
        getAutoDiffDifferentialType(constrainedOriginalFnTy, parameterIndices,
                                    resultIndices, lookupConformance, TC);
    break;
  case AutoDiffDerivativeFunctionKind::VJP:
    closureType =
        getAutoDiffPullbackType(constrainedOriginalFnTy, parameterIndices,
                                resultIndices, lookupConformance, TC);
    break;
  }
  // Compute the derivative function parameters.
  SmallVector<SILParameterInfo, 4> newParameters;
  newParameters.reserve(constrainedOriginalFnTy->getNumParameters());
  for (auto &param : constrainedOriginalFnTy->getParameters()) {
    newParameters.push_back(param);
  }
  // Reabstraction thunks have a function-typed parameter (the function to
  // reabstract) as their last parameter. Reabstraction thunk JVPs/VJPs have a
  // `@differentiable` function-typed last parameter instead.
  if (isReabstractionThunk) {
    assert(!parameterIndices->contains(getNumParameters() - 1) &&
           "Function-typed parameter should not be wrt");
    auto fnParam = newParameters.back();
    auto fnParamType = dyn_cast<SILFunctionType>(fnParam.getInterfaceType());
    assert(fnParamType);
    auto diffFnType = fnParamType->getWithDifferentiability(
        DifferentiabilityKind::Normal, parameterIndices, resultIndices);
    newParameters.back() = fnParam.getWithInterfaceType(diffFnType);
  }

  // Compute the derivative function results.
  SmallVector<SILResultInfo, 4> newResults;
  newResults.reserve(getNumResults() + 1);
  for (auto &result : constrainedOriginalFnTy->getResults()) {
    newResults.push_back(result);
  }
  newResults.push_back({closureType, ResultConvention::Owned});

  // Compute the derivative function ExtInfo.
  // If original function is `@convention(c)`, the derivative function should
  // have `@convention(thin)`. IRGen does not support `@convention(c)` functions
  // with multiple results.
  auto extInfo = constrainedOriginalFnTy->getExtInfo();
  if (getRepresentation() == SILFunctionTypeRepresentation::CFunctionPointer)
    extInfo = extInfo.withRepresentation(SILFunctionTypeRepresentation::Thin);

  // Put everything together to get the derivative function type. Then, store in
  // cache and return.
  cachedResult = SILFunctionType::get(
      constrainedOriginalFnTy->getSubstGenericSignature(), extInfo,
      constrainedOriginalFnTy->getCoroutineKind(),
      constrainedOriginalFnTy->getCalleeConvention(), newParameters,
      constrainedOriginalFnTy->getYields(), newResults,
      constrainedOriginalFnTy->getOptionalErrorResult(),
      /*patternSubstitutions*/ SubstitutionMap(),
      /*invocationSubstitutions*/ SubstitutionMap(),
      constrainedOriginalFnTy->getASTContext(),
      constrainedOriginalFnTy->getWitnessMethodConformanceOrInvalid());
  return cachedResult;
}

CanSILFunctionType SILFunctionType::getAutoDiffTransposeFunctionType(
    IndexSubset *parameterIndices, Lowering::TypeConverter &TC,
    LookupConformanceFn lookupConformance,
    CanGenericSignature transposeFnGenSig) {
  // Get the "constrained" transpose function generic signature.
  if (!transposeFnGenSig)
    transposeFnGenSig = getSubstGenericSignature();
  transposeFnGenSig = autodiff::getConstrainedDerivativeGenericSignature(
                          this, parameterIndices, transposeFnGenSig,
                          lookupConformance, /*isLinear*/ true)
                          .getCanonicalSignature();

  // Given a type, returns its formal SIL parameter info.
  auto getParameterInfoForOriginalResult =
      [&](const SILResultInfo &result) -> SILParameterInfo {
    AbstractionPattern pattern(transposeFnGenSig, result.getInterfaceType());
    auto &tl = TC.getTypeLowering(pattern, result.getInterfaceType(),
                                  TypeExpansionContext::minimal());
    ParameterConvention newConv;
    switch (result.getConvention()) {
    case ResultConvention::Owned:
    case ResultConvention::Autoreleased:
      newConv = tl.isTrivial() ? ParameterConvention::Direct_Unowned
                               : ParameterConvention::Direct_Guaranteed;
      break;
    case ResultConvention::Unowned:
    case ResultConvention::UnownedInnerPointer:
      newConv = ParameterConvention::Direct_Unowned;
      break;
    case ResultConvention::Indirect:
      newConv = ParameterConvention::Indirect_In_Guaranteed;
      break;
    }
    return {result.getInterfaceType(), newConv};
  };

  // Given a type, returns its formal SIL result info.
  auto getResultInfoForOriginalParameter =
      [&](const SILParameterInfo &param) -> SILResultInfo {
    AbstractionPattern pattern(transposeFnGenSig, param.getInterfaceType());
    auto &tl = TC.getTypeLowering(pattern, param.getInterfaceType(),
                                  TypeExpansionContext::minimal());
    ResultConvention newConv;
    switch (param.getConvention()) {
    case ParameterConvention::Direct_Owned:
    case ParameterConvention::Direct_Guaranteed:
    case ParameterConvention::Direct_Unowned:
      newConv =
          tl.isTrivial() ? ResultConvention::Unowned : ResultConvention::Owned;
      break;
    case ParameterConvention::Indirect_In:
    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Indirect_In_Constant:
    case ParameterConvention::Indirect_In_Guaranteed:
    case ParameterConvention::Indirect_InoutAliasable:
      newConv = ResultConvention::Indirect;
      break;
    }
    return {param.getInterfaceType(), newConv};
  };

  SmallVector<SILParameterInfo, 4> newParameters;
  SmallVector<SILResultInfo, 4> newResults;
  for (auto pair : llvm::enumerate(getParameters())) {
    auto index = pair.index();
    auto param = pair.value();
    if (parameterIndices->contains(index))
      newResults.push_back(getResultInfoForOriginalParameter(param));
    else
      newParameters.push_back(param);
  }
  for (auto &res : getResults())
    newParameters.push_back(getParameterInfoForOriginalResult(res));
  return SILFunctionType::get(
      getInvocationGenericSignature(), getExtInfo(), getCoroutineKind(),
      getCalleeConvention(), newParameters, getYields(), newResults,
      getOptionalErrorResult(), getPatternSubstitutions(),
      /*invocationSubstitutions*/ {}, getASTContext());
}

static CanType getKnownType(Optional<CanType> &cacheSlot, ASTContext &C,
                            StringRef moduleName, StringRef typeName) {
  if (!cacheSlot) {
    cacheSlot = ([&] {
      ModuleDecl *mod = C.getLoadedModule(C.getIdentifier(moduleName));
      if (!mod)
        return CanType();

      // Do a general qualified lookup instead of a direct lookupValue because
      // some of the types we want are reexported through overlays and
      // lookupValue would only give us types actually declared in the overlays
      // themselves.
      SmallVector<ValueDecl *, 2> decls;
      mod->lookupQualified(mod, DeclNameRef(C.getIdentifier(typeName)),
                           NL_QualifiedDefault, decls);
      if (decls.size() != 1)
        return CanType();

      const auto *typeDecl = dyn_cast<TypeDecl>(decls.front());
      if (!typeDecl)
        return CanType();

      return typeDecl->getDeclaredInterfaceType()->getCanonicalType();
    })();
  }
  CanType t = *cacheSlot;

  // It is possible that we won't find a bridging type (e.g. String) when we're
  // parsing the stdlib itself.
  if (t) {
    LLVM_DEBUG(llvm::dbgs() << "Bridging type " << moduleName << '.' << typeName
                            << " mapped to ";
               if (t)
                 t->print(llvm::dbgs());
               else
                 llvm::dbgs() << "<null>";
               llvm::dbgs() << '\n');
  }
  return t;
}

#define BRIDGING_KNOWN_TYPE(BridgedModule,BridgedType) \
  CanType TypeConverter::get##BridgedType##Type() {         \
    return getKnownType(BridgedType##Ty, Context, \
                        #BridgedModule, #BridgedType);      \
  }
#include "swift/SIL/BridgedTypes.def"

/// Adjust a function type to have a slightly different type.
CanAnyFunctionType
Lowering::adjustFunctionType(CanAnyFunctionType t,
                             AnyFunctionType::ExtInfo extInfo) {
  if (t->getExtInfo().isEqualTo(extInfo, useClangTypes(t)))
    return t;
  return CanAnyFunctionType(t->withExtInfo(extInfo));
}

/// Adjust a function type to have a slightly different type.
CanSILFunctionType
Lowering::adjustFunctionType(CanSILFunctionType type,
                             SILFunctionType::ExtInfo extInfo,
                             ParameterConvention callee,
                             ProtocolConformanceRef witnessMethodConformance) {
  if (type->getExtInfo().isEqualTo(extInfo, useClangTypes(type)) &&
      type->getCalleeConvention() == callee &&
      type->getWitnessMethodConformanceOrInvalid() == witnessMethodConformance)
    return type;

  return SILFunctionType::get(type->getInvocationGenericSignature(),
                              extInfo, type->getCoroutineKind(), callee,
                              type->getParameters(), type->getYields(),
                              type->getResults(),
                              type->getOptionalErrorResult(),
                              type->getPatternSubstitutions(),
                              type->getInvocationSubstitutions(),
                              type->getASTContext(),
                              witnessMethodConformance);
}

CanSILFunctionType
SILFunctionType::getWithRepresentation(Representation repr) {
  return getWithExtInfo(getExtInfo().withRepresentation(repr));
}

CanSILFunctionType SILFunctionType::getWithExtInfo(ExtInfo newExt) {
  auto oldExt = getExtInfo();
  if (newExt.isEqualTo(oldExt, useClangTypes(this)))
    return CanSILFunctionType(this);

  auto calleeConvention =
    (newExt.hasContext()
       ? (oldExt.hasContext()
            ? getCalleeConvention()
            : Lowering::DefaultThickCalleeConvention)
       : ParameterConvention::Direct_Unowned);

  return get(getInvocationGenericSignature(), newExt, getCoroutineKind(),
             calleeConvention, getParameters(), getYields(), getResults(),
             getOptionalErrorResult(), getPatternSubstitutions(),
             getInvocationSubstitutions(), getASTContext(),
             getWitnessMethodConformanceOrInvalid());
}

namespace {

enum class ConventionsKind : uint8_t {
  Default = 0,
  DefaultBlock = 1,
  ObjCMethod = 2,
  CFunctionType = 3,
  CFunction = 4,
  ObjCSelectorFamily = 5,
  Deallocator = 6,
  Capture = 7,
  CXXMethod = 8,
};

class Conventions {
  ConventionsKind kind;

protected:
  virtual ~Conventions() = default;

public:
  Conventions(ConventionsKind k) : kind(k) {}

  ConventionsKind getKind() const { return kind; }

  virtual ParameterConvention
  getIndirectParameter(unsigned index,
                       const AbstractionPattern &type,
                       const TypeLowering &substTL) const = 0;
  virtual ParameterConvention
  getDirectParameter(unsigned index,
                     const AbstractionPattern &type,
                     const TypeLowering &substTL) const = 0;
  virtual ParameterConvention getCallee() const = 0;
  virtual ResultConvention getResult(const TypeLowering &resultTL) const = 0;
  virtual ParameterConvention
  getIndirectSelfParameter(const AbstractionPattern &type) const = 0;
  virtual ParameterConvention
  getDirectSelfParameter(const AbstractionPattern &type) const = 0;

  // Helpers that branch based on a value ownership.
  ParameterConvention getIndirect(ValueOwnership ownership, bool forSelf,
                                  unsigned index,
                                  const AbstractionPattern &type,
                                  const TypeLowering &substTL) const {
    switch (ownership) {
    case ValueOwnership::Default:
      if (forSelf)
        return getIndirectSelfParameter(type);
      return getIndirectParameter(index, type, substTL);
    case ValueOwnership::InOut:
      return ParameterConvention::Indirect_Inout;
    case ValueOwnership::Shared:
      return ParameterConvention::Indirect_In_Guaranteed;
    case ValueOwnership::Owned:
      return ParameterConvention::Indirect_In;
    }
    llvm_unreachable("unhandled ownership");
  }

  ParameterConvention getDirect(ValueOwnership ownership, bool forSelf,
                                unsigned index, const AbstractionPattern &type,
                                const TypeLowering &substTL) const {
    switch (ownership) {
    case ValueOwnership::Default:
      if (forSelf)
        return getDirectSelfParameter(type);
      return getDirectParameter(index, type, substTL);
    case ValueOwnership::InOut:
      return ParameterConvention::Indirect_Inout;
    case ValueOwnership::Shared:
      return ParameterConvention::Direct_Guaranteed;
    case ValueOwnership::Owned:
      return ParameterConvention::Direct_Owned;
    }
    llvm_unreachable("unhandled ownership");
  }
};

/// A structure for building the substituted generic signature of a lowered type.
///
/// Where the abstraction pattern for a lowered type involves substitutable types, we extract those positions
/// out into generic arguments. This signature only needs to consider the general calling convention,
/// so it can reduce away protocol and base class constraints aside from
/// `AnyObject`. We want similar-shaped generic function types to remain
/// canonically equivalent, like `(T, U) -> ()`, `(T, T) -> ()`,
/// `(U, T) -> ()` or `(T, T.A) -> ()` when given substitutions that produce
/// the same function types, so we also introduce a new generic argument for
/// each position where we see a dependent type, and canonicalize the order in
/// which we see independent generic arguments.
class SubstFunctionTypeCollector {
public:
  TypeConverter &TC;
  TypeExpansionContext Expansion;
  CanGenericSignature GenericSig;
  bool Enabled;
  
  SmallVector<GenericTypeParamType *, 4> substGenericParams;
  SmallVector<Requirement, 4> substRequirements;
  SmallVector<Type, 4> substReplacements;
  SmallVector<ProtocolConformanceRef, 4> substConformances;
  
  SubstFunctionTypeCollector(TypeConverter &TC, TypeExpansionContext context,
                             CanGenericSignature genericSig, bool enabled)
    : TC(TC), Expansion(context), GenericSig(genericSig), Enabled(enabled) {
    }
  SubstFunctionTypeCollector(const SubstFunctionTypeCollector &) = delete;
  
  // Add a substitution for a fresh type variable, with the given replacement
  // type and layout constraint.
  CanType addSubstitution(LayoutConstraint layout,
                          CanType substType,
                          ArchetypeType *upperBound,
                      ArrayRef<ProtocolConformanceRef> substTypeConformances) {
    auto paramIndex = substGenericParams.size();
    auto param = CanGenericTypeParamType::get(0, paramIndex, TC.Context);
    
    // Expand the bound type according to the expansion context.
    if (Expansion.shouldLookThroughOpaqueTypeArchetypes()
        && substType->hasOpaqueArchetype()) {
      substType = substOpaqueTypesWithUnderlyingTypes(substType, Expansion);
    }
    
    substGenericParams.push_back(param);
    substReplacements.push_back(substType);
    
    LayoutConstraint upperBoundLayout;
    Type upperBoundSuperclass;
    ArrayRef<ProtocolDecl*> upperBoundConformances;
    
    // If the parameter is in a position with upper bound constraints, such
    // as a generic nominal type with type constraints on its arguments, then
    // preserve the constraints from that upper bound.
    if (upperBound) {
      upperBoundSuperclass = upperBound->getSuperclass();
      upperBoundConformances = upperBound->getConformsTo();
      upperBoundLayout = upperBound->getLayoutConstraint();
    }
    
    if (upperBoundSuperclass) {
      upperBoundSuperclass = upperBoundSuperclass->mapTypeOutOfContext();
      substRequirements.push_back(
       Requirement(RequirementKind::Superclass, param, upperBoundSuperclass));
    }
    
    // Preserve the layout constraint, if any, on the archetype in the
    // generic signature, generalizing away some constraints that
    // shouldn't affect ABI substitutability.
    if (layout) {
      switch (layout->getKind()) {
      // Keep these layout constraints as is.
      case LayoutConstraintKind::RefCountedObject:
      case LayoutConstraintKind::TrivialOfAtMostSize:
        break;
      
      case LayoutConstraintKind::UnknownLayout:
      case LayoutConstraintKind::Trivial:
        // These constraints don't really constrain the ABI, so we can
        // eliminate them.
        layout = LayoutConstraint();
        break;
    
      // Replace these specific constraints with one of the more general
      // constraints above.
      case LayoutConstraintKind::NativeClass:
      case LayoutConstraintKind::Class:
      case LayoutConstraintKind::NativeRefCountedObject:
        // These can all be generalized to RefCountedObject.
        layout = LayoutConstraint::getLayoutConstraint(
                                   LayoutConstraintKind::RefCountedObject);
        break;
          
      case LayoutConstraintKind::TrivialOfExactSize:
        // Generalize to TrivialOfAtMostSize.
        layout = LayoutConstraint::getLayoutConstraint(
           LayoutConstraintKind::TrivialOfAtMostSize,
           layout->getTrivialSizeInBits(),
           layout->getAlignmentInBits(),
           TC.Context);
        break;
      }
      
      if (layout) {
        // Pick the more specific of the upper bound layout and the layout
        // we chose above.
        if (upperBoundLayout) {
          layout = layout.merge(upperBoundLayout);
        }
        
        substRequirements.push_back(
                      Requirement(RequirementKind::Layout, param, layout));
      }
    }
    
    for (unsigned i : indices(upperBoundConformances)) {
      auto proto = upperBoundConformances[i];
      auto conformance = substTypeConformances[i];
      substRequirements.push_back(Requirement(RequirementKind::Conformance, param,
                                              proto->getDeclaredInterfaceType()));
      substConformances.push_back(conformance);
    }
    
    return param;
  }
  
  /// Given the destructured original abstraction pattern and substituted type for a destructured
  /// parameter or result, introduce substituted generic parameters and requirements as needed for
  /// the lowered type, and return the substituted type in terms of the substituted generic signature.
  CanType getSubstitutedInterfaceType(AbstractionPattern origType,
                                      CanType substType) {
    if (!Enabled)
      return substType;
    
    // Replace every dependent type we see with a fresh type variable in
    // the substituted signature, substituted by the corresponding concrete
    // type.

    // The entire original context could be a generic parameter.
    if (origType.isTypeParameter() ||
        origType.isOpaqueFunctionOrOpaqueDerivativeFunction()) {
      return addSubstitution(origType.getLayoutConstraint(), substType,
                             nullptr, {});
    }
    
    auto origContextType = origType.getType();
    
    // If the substituted type is a subclass of the abstraction pattern
    // type, build substitutions for any type parameters in it. This only
    // comes up when lowering override types for vtable entries.
    auto areDifferentClasses = [](Type a, Type b) -> bool {
      if (auto dynA = a->getAs<DynamicSelfType>()) {
        a = dynA->getSelfType();
      }
      if (auto dynB = b->getAs<DynamicSelfType>()) {
        b = dynB->getSelfType();
      }
      if (auto aClass = a->getClassOrBoundGenericClass()) {
        if (auto bClass = b->getClassOrBoundGenericClass()) {
          return aClass != bClass;
        }
      }
      
      return false;
    };
    
    bool substituteBindingsInSubstType = false;
    if (areDifferentClasses(substType, origContextType)) {
      substituteBindingsInSubstType = true;
    }
    if (auto substMeta = dyn_cast<MetatypeType>(substType)) {
      if (auto origMeta = dyn_cast<MetatypeType>(origContextType)) {
        if (areDifferentClasses(substMeta->getInstanceType(),
                                origMeta->getInstanceType())) {
          substituteBindingsInSubstType = true;
        }
      }
    }

    CanGenericSignature origSig = origType.getGenericSignature();
    if (substituteBindingsInSubstType) {
      origContextType = substType;
      origSig = TC.getCurGenericSignature();
      assert((!substType->hasTypeParameter() || origSig) &&
             "lowering mismatched interface types in a context without "
             "a generic signature");
    }
    
    if (!origContextType->hasTypeParameter()
        && !origContextType->hasArchetype()) {
      // If the abstraction pattern doesn't have substitutable positions, nor
      // should the concrete type.
      assert(!substType->hasTypeParameter()
             && !substType->hasArchetype());
      return substType;
    }
    
    // Extract structural substitutions.
    if (origContextType->hasTypeParameter()) {
      origContextType = origSig->getGenericEnvironment()
        ->mapTypeIntoContext(origContextType)
        ->getCanonicalType();
    }

    auto result = origContextType
      ->substituteBindingsTo(substType,
        [&](ArchetypeType *archetype,
            CanType binding,
            ArchetypeType *upperBound,
            ArrayRef<ProtocolConformanceRef> bindingConformances) -> CanType {
          return addSubstitution(archetype->getLayoutConstraint(),
                                 binding,
                                 upperBound,
                                 bindingConformances);
        });
    
    assert(result && "substType was not bindable to abstraction pattern type?");
    return result;
  }
};

/// A visitor for breaking down formal result types into a SILResultInfo
/// and possibly some number of indirect-out SILParameterInfos,
/// matching the abstraction patterns of the original type.
class DestructureResults {
  TypeConverter &TC;
  const Conventions &Convs;
  SmallVectorImpl<SILResultInfo> &Results;
  TypeExpansionContext context;
  SubstFunctionTypeCollector &Subst;

public:
  DestructureResults(TypeExpansionContext context, TypeConverter &TC,
                     const Conventions &conventions,
                     SmallVectorImpl<SILResultInfo> &results,
                     SubstFunctionTypeCollector &subst)
      : TC(TC), Convs(conventions), Results(results), context(context),
        Subst(subst) {}

  void destructure(AbstractionPattern origType, CanType substType) {
    // Recur into tuples.
    if (origType.isTuple()) {
      auto substTupleType = cast<TupleType>(substType);
      for (auto eltIndex : indices(substTupleType.getElementTypes())) {
        AbstractionPattern origEltType =
          origType.getTupleElementType(eltIndex);
        CanType substEltType = substTupleType.getElementType(eltIndex);
        destructure(origEltType, substEltType);
      }
      return;
    }
    
    auto substInterfaceType = Subst.getSubstitutedInterfaceType(origType,
                                                                substType);
    
    auto &substResultTLForConvention = TC.getTypeLowering(
        origType, substInterfaceType, TypeExpansionContext::minimal());
    auto &substResultTL = TC.getTypeLowering(origType, substInterfaceType,
                                             context);


    // Determine the result convention.
    ResultConvention convention;
    if (isFormallyReturnedIndirectly(origType, substType,
                                     substResultTLForConvention)) {
      convention = ResultConvention::Indirect;
    } else {
      convention = Convs.getResult(substResultTLForConvention);

      // Reduce conventions for trivial types to an unowned convention.
      if (substResultTL.isTrivial()) {
        switch (convention) {
        case ResultConvention::Indirect:
        case ResultConvention::Unowned:
        case ResultConvention::UnownedInnerPointer:
          // Leave these as-is.
          break;

        case ResultConvention::Autoreleased:
        case ResultConvention::Owned:
          // These aren't distinguishable from unowned for trivial types.
          convention = ResultConvention::Unowned;
          break;
        }
      }
    }
    
    SILResultInfo result(substResultTL.getLoweredType().getASTType(),
                         convention);
    Results.push_back(result);
  }

  /// Query whether the original type is returned indirectly for the purpose
  /// of reabstraction given complete lowering information about its
  /// substitution.
  bool isFormallyReturnedIndirectly(AbstractionPattern origType,
                                    CanType substType,
                                    const TypeLowering &substTL) {
    // If the substituted type is returned indirectly, so must the
    // unsubstituted type.
    if ((origType.isTypeParameter()
         && !origType.isConcreteType()
         && !origType.requiresClass())
        || substTL.isAddressOnly()) {
      return true;

    // Functions are always returned directly.
    } else if (origType.isOpaqueFunctionOrOpaqueDerivativeFunction()) {
      return false;

    // If the substitution didn't change the type, then a negative
    // response to the above is determinative as well.
    } else if (origType.getType() == substType &&
               !origType.getType()->hasTypeParameter()) {
      return false;

    // Otherwise, query specifically for the original type.
    } else {
      return SILType::isFormallyReturnedIndirectly(
          origType.getType(), TC, origType.getGenericSignature());
    }
  }
};

static bool isClangTypeMoreIndirectThanSubstType(TypeConverter &TC,
                                                 const clang::Type *clangTy,
                                                 CanType substTy) {
  // A const pointer argument might have been imported as
  // UnsafePointer, COpaquePointer, or a CF foreign class.
  // (An ObjC class type wouldn't be const-qualified.)
  if (clangTy->isPointerType()
      && clangTy->getPointeeType().isConstQualified()) {
    // Peek through optionals.
    if (auto substObjTy = substTy.getOptionalObjectType())
      substTy = substObjTy;

    // Void pointers aren't usefully indirectable.
    if (clangTy->isVoidPointerType())
      return false;

    if (auto eltTy = substTy->getAnyPointerElementType())
      return isClangTypeMoreIndirectThanSubstType(TC,
                    clangTy->getPointeeType().getTypePtr(), CanType(eltTy));

    if (substTy->getAnyNominal() ==
          TC.Context.getOpaquePointerDecl())
      // TODO: We could conceivably have an indirect opaque ** imported
      // as COpaquePointer. That shouldn't ever happen today, though,
      // since we only ever indirect the 'self' parameter of functions
      // imported as methods.
      return false;

    if (clangTy->getPointeeType()->getAs<clang::RecordType>()) {
      // CF type as foreign class
      if (substTy->getClassOrBoundGenericClass() &&
          substTy->getClassOrBoundGenericClass()->getForeignClassKind() ==
            ClassDecl::ForeignKind::CFType) {
        return false;
      }
    }

    // swift_newtypes are always passed directly
    if (auto typedefTy = clangTy->getAs<clang::TypedefType>()) {
      if (typedefTy->getDecl()->getAttr<clang::SwiftNewtypeAttr>())
        return false;
    }

    return true;
  }
  return false;
}

static bool isFormallyPassedIndirectly(TypeConverter &TC,
                                       AbstractionPattern origType,
                                       CanType substType,
                                       const TypeLowering &substTL) {
  // If the C type of the argument is a const pointer, but the Swift type
  // isn't, treat it as indirect.
  if (origType.isClangType()
      && isClangTypeMoreIndirectThanSubstType(TC, origType.getClangType(),
                                              substType)) {
    return true;
  }

  // If the substituted type is passed indirectly, so must the
  // unsubstituted type.
  if ((origType.isTypeParameter() && !origType.isConcreteType()
       && !origType.requiresClass())
      || substTL.isAddressOnly()) {
    return true;

  // If the substitution didn't change the type, then a negative
  // response to the above is determinative as well.
  } else if (origType.getType() == substType &&
             !origType.getType()->hasTypeParameter()) {
    return false;

  // Otherwise, query specifically for the original type.
  } else {
    return SILType::isFormallyPassedIndirectly(
        origType.getType(), TC, origType.getGenericSignature());
  }
}

/// A visitor for turning formal input types into SILParameterInfos, matching
/// the abstraction patterns of the original type.
///
/// If the original abstraction pattern is fully opaque, we must pass the
/// function's parameters and results indirectly, as if the original type were
/// the most general function signature (expressed entirely in generic
/// parameters) which can be substituted to equal the given signature.
///
/// See the comment in AbstractionPattern.h for details.
class DestructureInputs {
  TypeExpansionContext expansion;
  TypeConverter &TC;
  const Conventions &Convs;
  const ForeignInfo &Foreign;
  struct ForeignSelfInfo {
    AbstractionPattern OrigSelfParam;
    AnyFunctionType::CanParam SubstSelfParam;
  };
  Optional<ForeignSelfInfo> ForeignSelf;
  AbstractionPattern TopLevelOrigType = AbstractionPattern::getInvalid();
  SmallVectorImpl<SILParameterInfo> &Inputs;
  SubstFunctionTypeCollector &Subst;
  unsigned NextOrigParamIndex = 0;
public:
  DestructureInputs(TypeExpansionContext expansion, TypeConverter &TC,
                    const Conventions &conventions, const ForeignInfo &foreign,
                    SmallVectorImpl<SILParameterInfo> &inputs,
                    SubstFunctionTypeCollector &subst)
      : expansion(expansion), TC(TC), Convs(conventions), Foreign(foreign),
        Inputs(inputs), Subst(subst) {}

  void destructure(AbstractionPattern origType,
                   CanAnyFunctionType::CanParamArrayRef params,
                   SILExtInfoBuilder extInfoBuilder) {
    visitTopLevelParams(origType, params, extInfoBuilder);
  }

private:
  /// Query whether the original type is address-only given complete
  /// lowering information about its substitution.
  bool isFormallyPassedIndirectly(AbstractionPattern origType,
                                  CanType substType,
                                  const TypeLowering &substTL) {
    return ::isFormallyPassedIndirectly(TC, origType, substType, substTL);
  }

  /// This is a special entry point that allows destructure inputs to handle
  /// self correctly.
  void visitTopLevelParams(AbstractionPattern origType,
                           CanAnyFunctionType::CanParamArrayRef params,
                           SILExtInfoBuilder extInfoBuilder) {
    unsigned numEltTypes = params.size();

    bool hasSelf =
        (extInfoBuilder.hasSelfParam() || Foreign.Self.isImportAsMember());
    unsigned numNonSelfParams = (hasSelf ? numEltTypes - 1 : numEltTypes);
    TopLevelOrigType = origType;
    // If we have a foreign-self, install handleSelf as the handler.
    if (Foreign.Self.isInstance()) {
      assert(hasSelf && numEltTypes > 0);
      ForeignSelf = ForeignSelfInfo{origType.getFunctionParamType(numNonSelfParams),
                                    params[numNonSelfParams]};
    }

    // Add any foreign parameters that are positioned here.
    maybeAddForeignParameters();

    // Process all the non-self parameters.
    for (unsigned i = 0; i != numNonSelfParams; ++i) {
      auto ty = params[i].getParameterType();
      auto eltPattern = origType.getFunctionParamType(i);
      auto flags = params[i].getParameterFlags();

      visit(flags.getValueOwnership(), /*forSelf=*/false, eltPattern, ty,
            flags.isNoDerivative());
    }

    // Process the self parameter.  Note that we implicitly drop self
    // if this is a static foreign-self import.
    if (hasSelf && !Foreign.Self.isImportAsMember()) {
      auto selfParam = params[numNonSelfParams];
      auto ty = selfParam.getParameterType();
      auto eltPattern = origType.getFunctionParamType(numNonSelfParams);
      auto flags = selfParam.getParameterFlags();

      visit(flags.getValueOwnership(), /*forSelf=*/true,
            eltPattern, ty);
    }

    TopLevelOrigType = AbstractionPattern::getInvalid();
    ForeignSelf = None;
  }

  void visit(ValueOwnership ownership, bool forSelf,
             AbstractionPattern origType, CanType substType,
             bool isNonDifferentiable = false) {
    assert(!isa<InOutType>(substType));

    // Tuples get handled specially, in some cases:
    CanTupleType substTupleTy = dyn_cast<TupleType>(substType);
    if (substTupleTy && !origType.isTypeParameter()) {
      assert(origType.getNumTupleElements() == substTupleTy->getNumElements());
      switch (ownership) {
      case ValueOwnership::Default:
      case ValueOwnership::Owned:
      case ValueOwnership::Shared:
        // Expand the tuple.
        for (auto i : indices(substTupleTy.getElementTypes())) {
          auto &elt = substTupleTy->getElement(i);
          auto ownership = elt.getParameterFlags().getValueOwnership();
          assert(ownership == ValueOwnership::Default);
          assert(!elt.isVararg());
          visit(ownership, forSelf,
                origType.getTupleElementType(i),
                CanType(elt.getRawType()));
        }
        return;
      case ValueOwnership::InOut:
        // handled below
        break;
      }
    }

    unsigned origParamIndex = NextOrigParamIndex++;
    
    auto substInterfaceType =
      Subst.getSubstitutedInterfaceType(origType, substType);

    auto &substTLConv = TC.getTypeLowering(origType, substInterfaceType,
                                       TypeExpansionContext::minimal());
    auto &substTL = TC.getTypeLowering(origType, substInterfaceType, expansion);

    ParameterConvention convention;
    if (ownership == ValueOwnership::InOut) {
      convention = ParameterConvention::Indirect_Inout;
    } else if (isFormallyPassedIndirectly(origType, substType, substTLConv)) {
      convention = Convs.getIndirect(ownership, forSelf, origParamIndex,
                                     origType, substTLConv);
      assert(isIndirectFormalParameter(convention));
    } else if (substTL.isTrivial()) {
      convention = ParameterConvention::Direct_Unowned;
    } else {
      convention = Convs.getDirect(ownership, forSelf, origParamIndex, origType,
                                   substTLConv);
      assert(!isIndirectFormalParameter(convention));
    }

    SILParameterInfo param(substTL.getLoweredType().getASTType(), convention);
    if (isNonDifferentiable)
      param = param.getWithDifferentiability(
          SILParameterDifferentiability::NotDifferentiable);
    Inputs.push_back(param);

    maybeAddForeignParameters();
  }

  /// Given that we've just reached an argument index for the
  /// first time, add any foreign parameters.
  void maybeAddForeignParameters() {
    while (maybeAddForeignAsyncParameter() ||
           maybeAddForeignErrorParameter() ||
           maybeAddForeignSelfParameter()) {
      // Continue to see, just in case there are more parameters to add.
    }
  }
  
  bool maybeAddForeignAsyncParameter() {
    if (!Foreign.Async
        || NextOrigParamIndex != Foreign.Async->completionHandlerParamIndex())
      return false;
    
    auto nativeCHTy = Foreign.Async->completionHandlerType();

    // Use the abstraction pattern we're lowering against in order to lower
    // the completion handler type, so we can preserve C/ObjC distinctions that
    // normally get abstracted away by the importer.
    auto completionHandlerNativeOrigTy = TopLevelOrigType
      .getObjCMethodAsyncCompletionHandlerType(nativeCHTy);
    
    // Bridge the Swift completion handler type back to its
    // foreign representation.
    auto foreignCHTy = TC.getLoweredBridgedType(completionHandlerNativeOrigTy,
                                      nativeCHTy,
                                      Bridgeability::Full,
                                      SILFunctionTypeRepresentation::ObjCMethod,
                                      TypeConverter::ForArgument)
      ->getCanonicalType();
    
    auto completionHandlerOrigTy = TopLevelOrigType
      .getObjCMethodAsyncCompletionHandlerType(foreignCHTy);
    auto completionHandlerTy = TC.getLoweredType(completionHandlerOrigTy,
                                                 foreignCHTy, expansion)
      .getASTType();
    Inputs.push_back(SILParameterInfo(completionHandlerTy,
                                      ParameterConvention::Direct_Unowned));
    ++NextOrigParamIndex;
    return true;
  }

  bool maybeAddForeignErrorParameter() {
    // A foreign async convention absorbs any error parameter, making it into
    // an argument to the callback.
    if (Foreign.Async)
      return false;
    
    if (!Foreign.Error ||
        NextOrigParamIndex != Foreign.Error->getErrorParameterIndex())
      return false;

    auto foreignErrorTy = TC.getLoweredRValueType(
        expansion, Foreign.Error->getErrorParameterType());

    // Assume the error parameter doesn't have interesting lowering.
    Inputs.push_back(SILParameterInfo(foreignErrorTy,
                                      ParameterConvention::Direct_Unowned));
    ++NextOrigParamIndex;
    return true;
  }

  bool maybeAddForeignSelfParameter() {
    if (!Foreign.Self.isInstance() ||
        NextOrigParamIndex != Foreign.Self.getSelfIndex())
      return false;

    if (ForeignSelf) {
      // This is a "self", but it's not a Swift self, we handle it differently.
      visit(ForeignSelf->SubstSelfParam.getValueOwnership(),
            /*forSelf=*/false,
            ForeignSelf->OrigSelfParam,
            ForeignSelf->SubstSelfParam.getParameterType());
    }
    return true;
  }
};

} // end anonymous namespace

static bool isPseudogeneric(SILDeclRef c) {
  // FIXME: should this be integrated in with the Sema check that prevents
  // illegal use of type arguments in pseudo-generic method bodies?

  // The implicitly-generated native initializer thunks for imported
  // initializers are never pseudo-generic, because they may need
  // to use their type arguments to bridge their value arguments.
  if (!c.isForeign &&
      (c.kind == SILDeclRef::Kind::Allocator ||
       c.kind == SILDeclRef::Kind::Initializer) &&
      c.getDecl()->hasClangNode())
    return false;

  // Otherwise, we have to look at the entity's context.
  DeclContext *dc;
  if (c.hasDecl()) {
    dc = c.getDecl()->getDeclContext();
  } else if (auto closure = c.getAbstractClosureExpr()) {
    dc = closure->getParent();
  } else {
    return false;
  }
  dc = dc->getInnermostTypeContext();
  if (!dc) return false;

  auto classDecl = dc->getSelfClassDecl();
  return (classDecl && classDecl->usesObjCGenericsModel());
}

/// Update the result type given the foreign error convention that we will be
/// using.
void updateResultTypeForForeignInfo(
    const ForeignInfo &foreignInfo, CanGenericSignature genericSig,
    AbstractionPattern &origResultType, CanType &substFormalResultType) {
  // If there's no error or async convention, the return type is unchanged.
  if (!foreignInfo.Async && !foreignInfo.Error) {
    return;
  }
  
  // A foreign async convention means our lowered return type is Void, since
  // the imported semantic return and/or error type map to the completion
  // callback's argument(s).
  auto &C = substFormalResultType->getASTContext();
  if (auto async = foreignInfo.Async) {
    substFormalResultType = TupleType::getEmpty(C);
    origResultType = AbstractionPattern(genericSig, substFormalResultType);
    return;
  }
  
  // Otherwise, adjust the return type to match the foreign error convention.
  auto convention = *foreignInfo.Error;
  switch (convention.getKind()) {
  // These conventions replace the result type.
  case ForeignErrorConvention::ZeroResult:
  case ForeignErrorConvention::NonZeroResult:
    assert(substFormalResultType->isVoid());
    substFormalResultType = convention.getResultType();
    origResultType = AbstractionPattern(genericSig, substFormalResultType);
    return;

  // These conventions wrap the result type in a level of optionality.
  case ForeignErrorConvention::NilResult:
    assert(!substFormalResultType->getOptionalObjectType());
    substFormalResultType =
        OptionalType::get(substFormalResultType)->getCanonicalType();
    origResultType =
        AbstractionPattern::getOptional(origResultType);
    return;

  // These conventions don't require changes to the formal error type.
  case ForeignErrorConvention::ZeroPreservedResult:
  case ForeignErrorConvention::NonNilError:
    return;
  }
  llvm_unreachable("unhandled kind");
}

/// Lower any/all capture context parameters.
///
/// *NOTE* Currently default arg generators can not capture anything.
/// If we ever add that ability, it will be a different capture list
/// from the function to which the argument is attached.
static void
lowerCaptureContextParameters(TypeConverter &TC, SILDeclRef function,
                              CanGenericSignature genericSig,
                              TypeExpansionContext expansion,
                              SmallVectorImpl<SILParameterInfo> &inputs) {

  // NB: The generic signature may be elided from the lowered function type
  // if the function is in a fully-specialized context, but we still need to
  // canonicalize references to the generic parameters that may appear in
  // non-canonical types in that context. We need the original generic
  // signature from the AST for that.
  auto origGenericSig = function.getAnyFunctionRef()->getGenericSignature();
  auto loweredCaptures = TC.getLoweredLocalCaptures(function);

  for (auto capture : loweredCaptures.getCaptures()) {
    if (capture.isDynamicSelfMetadata()) {
      ParameterConvention convention = ParameterConvention::Direct_Unowned;
      auto dynamicSelfInterfaceType =
          loweredCaptures.getDynamicSelfType()->mapTypeOutOfContext();

      auto selfMetatype = MetatypeType::get(dynamicSelfInterfaceType,
                                            MetatypeRepresentation::Thick);

      auto canSelfMetatype = selfMetatype->getCanonicalType(origGenericSig);
      SILParameterInfo param(canSelfMetatype, convention);
      inputs.push_back(param);

      continue;
    }

    if (capture.isOpaqueValue()) {
      OpaqueValueExpr *opaqueValue = capture.getOpaqueValue();
      auto canType = opaqueValue->getType()->mapTypeOutOfContext()
          ->getCanonicalType(origGenericSig);
      auto &loweredTL =
          TC.getTypeLowering(AbstractionPattern(genericSig, canType),
                             canType, expansion);
      auto loweredTy = loweredTL.getLoweredType();

      ParameterConvention convention;
      if (loweredTL.isAddressOnly()) {
        convention = ParameterConvention::Indirect_In;
      } else {
        convention = ParameterConvention::Direct_Owned;
      }
      SILParameterInfo param(loweredTy.getASTType(), convention);
      inputs.push_back(param);

      continue;
    }

    auto *VD = capture.getDecl();
    auto type = VD->getInterfaceType();
    auto canType = type->getCanonicalType(origGenericSig);

    auto &loweredTL =
        TC.getTypeLowering(AbstractionPattern(genericSig, canType), canType,
                           expansion);
    auto loweredTy = loweredTL.getLoweredType();
    switch (TC.getDeclCaptureKind(capture, expansion)) {
    case CaptureKind::Constant: {
      // Constants are captured by value.
      ParameterConvention convention;
      assert (!loweredTL.isAddressOnly());
      if (loweredTL.isTrivial()) {
        convention = ParameterConvention::Direct_Unowned;
      } else {
        convention = ParameterConvention::Direct_Guaranteed;
      }
      SILParameterInfo param(loweredTy.getASTType(), convention);
      inputs.push_back(param);
      break;
    }
    case CaptureKind::Box: {
      // The type in the box is lowered in the minimal context.
      auto minimalLoweredTy =
          TC.getTypeLowering(AbstractionPattern(genericSig, canType), canType,
                             TypeExpansionContext::minimal())
              .getLoweredType();
      // Lvalues are captured as a box that owns the captured value.
      auto boxTy = TC.getInterfaceBoxTypeForCapture(
          VD, minimalLoweredTy.getASTType(),
          /*mutable*/ true);
      auto convention = ParameterConvention::Direct_Guaranteed;
      auto param = SILParameterInfo(boxTy, convention);
      inputs.push_back(param);
      break;
    }
    case CaptureKind::StorageAddress: {
      // Non-escaping lvalues are captured as the address of the value.
      SILType ty = loweredTy.getAddressType();
      auto param =
          SILParameterInfo(ty.getASTType(),
                           ParameterConvention::Indirect_InoutAliasable);
      inputs.push_back(param);
      break;
    }
    case CaptureKind::Immutable: {
      // 'let' constants that are address-only are captured as the address of
      // the value and will be consumed by the closure.
      SILType ty = loweredTy.getAddressType();
      auto param =
          SILParameterInfo(ty.getASTType(),
                           ParameterConvention::Indirect_In_Guaranteed);
      inputs.push_back(param);
      break;
    }
    }
  }
}

static AccessorDecl *getAsCoroutineAccessor(Optional<SILDeclRef> constant) {
  if (!constant || !constant->hasDecl())
    return nullptr;;

  auto accessor = dyn_cast<AccessorDecl>(constant->getDecl());
  if (!accessor || !accessor->isCoroutine())
    return nullptr;

  return accessor;
}

static void destructureYieldsForReadAccessor(TypeConverter &TC,
                                             TypeExpansionContext expansion,
                                             AbstractionPattern origType,
                                             CanType valueType,
                                          SmallVectorImpl<SILYieldInfo> &yields,
                                          SubstFunctionTypeCollector &subst) {
  // Recursively destructure tuples.
  if (origType.isTuple()) {
    auto valueTupleType = cast<TupleType>(valueType);
    for (auto i : indices(valueTupleType.getElementTypes())) {
      auto origEltType = origType.getTupleElementType(i);
      auto valueEltType = valueTupleType.getElementType(i);
      destructureYieldsForReadAccessor(TC, expansion, origEltType, valueEltType,
                                       yields, subst);
    }
    return;
  }

  auto valueInterfaceType =
    subst.getSubstitutedInterfaceType(origType, valueType);
  
  auto &tlConv =
      TC.getTypeLowering(origType, valueInterfaceType,
                         TypeExpansionContext::minimal());
  auto &tl =
      TC.getTypeLowering(origType, valueInterfaceType, expansion);
  auto convention = [&] {
    if (isFormallyPassedIndirectly(TC, origType, valueInterfaceType, tlConv))
      return ParameterConvention::Indirect_In_Guaranteed;
    if (tlConv.isTrivial())
      return ParameterConvention::Direct_Unowned;
    return ParameterConvention::Direct_Guaranteed;
  }();
  
  yields.push_back(SILYieldInfo(tl.getLoweredType().getASTType(),
                                convention));
}

static void destructureYieldsForCoroutine(TypeConverter &TC,
                                          TypeExpansionContext expansion,
                                          Optional<SILDeclRef> origConstant,
                                          Optional<SILDeclRef> constant,
                                          Optional<SubstitutionMap> reqtSubs,
                                          SmallVectorImpl<SILYieldInfo> &yields,
                                          SILCoroutineKind &coroutineKind,
                                          SubstFunctionTypeCollector &subst) {
  assert(coroutineKind == SILCoroutineKind::None);
  assert(yields.empty());

  auto accessor = getAsCoroutineAccessor(constant);
  if (!accessor)
    return;

  auto origAccessor = cast<AccessorDecl>(origConstant->getDecl());

  // Coroutine accessors are implicitly yield-once coroutines, despite
  // their function type.
  coroutineKind = SILCoroutineKind::YieldOnce;

  // Coroutine accessors are always native, so fetch the native
  // abstraction pattern.
  auto origStorage = origAccessor->getStorage();
  auto origType = TC.getAbstractionPattern(origStorage, /*nonobjc*/ true)
                    .getReferenceStorageReferentType();

  auto storage = accessor->getStorage();
  auto valueType = storage->getValueInterfaceType();
  if (reqtSubs) {
    valueType = valueType.subst(*reqtSubs);
  }

  auto canValueType = valueType->getCanonicalType(
    accessor->getGenericSignature());

  // 'modify' yields an inout of the target type.
  if (accessor->getAccessorKind() == AccessorKind::Modify) {
    auto valueInterfaceType = subst.getSubstitutedInterfaceType(origType,
                                                                canValueType);
    auto loweredValueTy =
        TC.getLoweredType(origType, valueInterfaceType, expansion);
    yields.push_back(SILYieldInfo(loweredValueTy.getASTType(),
                                  ParameterConvention::Indirect_Inout));
    return;
  }

  // 'read' yields a borrowed value of the target type, destructuring
  // tuples as necessary.
  assert(accessor->getAccessorKind() == AccessorKind::Read);
  destructureYieldsForReadAccessor(TC, expansion, origType, canValueType,
                                   yields, subst);
}

/// Create the appropriate SIL function type for the given formal type
/// and conventions.
///
/// The lowering of function types is generally sensitive to the
/// declared abstraction pattern.  We want to be able to take
/// advantage of declared type information in order to, say, pass
/// arguments separately and directly; but we also want to be able to
/// call functions from generic code without completely embarrassing
/// performance.  Therefore, different abstraction patterns induce
/// different argument-passing conventions, and we must introduce
/// implicit reabstracting conversions where necessary to map one
/// convention to another.
///
/// However, we actually can't reabstract arbitrary thin function
/// values while still leaving them thin, at least without costly
/// page-mapping tricks. Therefore, the representation must remain
/// consistent across all abstraction patterns.
///
/// We could reabstract block functions in theory, but (1) we don't
/// really need to and (2) doing so would be problematic because
/// stuffing something in an Optional currently forces it to be
/// reabstracted to the most general type, which means that we'd
/// expect the wrong abstraction conventions on bridged block function
/// types.
///
/// Therefore, we only honor abstraction patterns on thick or
/// polymorphic functions.
///
/// FIXME: we shouldn't just drop the original abstraction pattern
/// when we can't reabstract.  Instead, we should introduce
/// dynamic-indirect argument-passing conventions and map opaque
/// archetypes to that, then respect those conventions in IRGen by
/// using runtime call construction.
///
/// \param conventions - conventions as expressed for the original type
static CanSILFunctionType getSILFunctionType(
    TypeConverter &TC, TypeExpansionContext expansionContext,
    AbstractionPattern origType, CanAnyFunctionType substFnInterfaceType,
    SILExtInfoBuilder extInfoBuilder, const Conventions &conventions,
    const ForeignInfo &foreignInfo, Optional<SILDeclRef> origConstant,
    Optional<SILDeclRef> constant, Optional<SubstitutionMap> reqtSubs,
    ProtocolConformanceRef witnessMethodConformance) {
  // Find the generic parameters.
  CanGenericSignature genericSig =
    substFnInterfaceType.getOptGenericSignature();

  Optional<TypeConverter::GenericContextRAII> contextRAII;
  if (genericSig) contextRAII.emplace(TC, genericSig);

  // Per above, only fully honor opaqueness in the abstraction pattern
  // for thick or polymorphic functions.  We don't need to worry about
  // non-opaque patterns because the type-checker forbids non-thick
  // function types from having generic parameters or results.
  if (origType.isTypeParameter() &&
      substFnInterfaceType->getExtInfo().getSILRepresentation()
        != SILFunctionType::Representation::Thick &&
      isa<FunctionType>(substFnInterfaceType)) {
    origType = AbstractionPattern(genericSig,
                                  substFnInterfaceType);
  }

  Optional<SILResultInfo> errorResult;
  assert(
      (!foreignInfo.Error || substFnInterfaceType->getExtInfo().isThrowing())
      && "foreignError was set but function type does not throw?");
  assert(
      (!foreignInfo.Async || substFnInterfaceType->getExtInfo().isAsync())
      && "foreignAsync was set but function type is not async?");
  
  // Map 'async' to the appropriate `@async` modifier.
  bool isAsync = false;
  if (substFnInterfaceType->getExtInfo().isAsync() && !foreignInfo.Async) {
    assert(!origType.isForeign()
           && "using native Swift async for foreign type!");
    isAsync = true;
  }
  
  // Map 'throws' to the appropriate error convention.
  if (substFnInterfaceType->getExtInfo().isThrowing()
      && !foreignInfo.Error
      && !foreignInfo.Async) {
    assert(!origType.isForeign()
           && "using native Swift error convention for foreign type!");
    SILType exnType = SILType::getExceptionType(TC.Context);
    assert(exnType.isObject());
    errorResult = SILResultInfo(exnType.getASTType(),
                                ResultConvention::Owned);
  }

  // Lower the result type.
  AbstractionPattern origResultType = origType.getFunctionResultType();
  CanType substFormalResultType = substFnInterfaceType.getResult();

  // If we have a foreign error and/or async convention, adjust the
  // lowered result type.
  updateResultTypeForForeignInfo(foreignInfo, genericSig, origResultType,
                                 substFormalResultType);

  bool shouldBuildSubstFunctionType = [&]{
    if (!TC.Context.LangOpts.EnableSubstSILFunctionTypesForFunctionValues)
      return false;

    // We always use substituted function types for coroutines that are
    // being lowered in the context of another coroutine, which is to say,
    // for class override thunks.  This is required to make the yields
    // match in abstraction to the base method's yields, which is necessary
    // to make the extracted continuation-function signatures match.
    if (constant != origConstant && getAsCoroutineAccessor(constant))
      return true;

    // We don't currently use substituted function types for generic function
    // type lowering, though we should for generic methods on classes and
    // protocols.
    if (genericSig)
      return false;

    // We only currently use substituted function types for function values,
    // which will have standard thin or thick representation. (Per the previous
    // comment, it would be useful to do so for generic methods on classes and
    // protocols too.)
    auto rep = extInfoBuilder.getRepresentation();
    return (rep == SILFunctionTypeRepresentation::Thick ||
            rep == SILFunctionTypeRepresentation::Thin);
  }();

  SubstFunctionTypeCollector subst(TC, expansionContext, genericSig,
                                   shouldBuildSubstFunctionType);

  // Destructure the input tuple type.
  SmallVector<SILParameterInfo, 8> inputs;
  {
    DestructureInputs destructurer(expansionContext, TC, conventions,
                                   foreignInfo, inputs, subst);
    destructurer.destructure(origType, substFnInterfaceType.getParams(),
                             extInfoBuilder);
  }

  // Destructure the coroutine yields.
  SILCoroutineKind coroutineKind = SILCoroutineKind::None;
  SmallVector<SILYieldInfo, 8> yields;
  destructureYieldsForCoroutine(TC, expansionContext, origConstant, constant,
                                reqtSubs, yields, coroutineKind, subst);
  
  // Destructure the result tuple type.
  SmallVector<SILResultInfo, 8> results;
  {
    DestructureResults destructurer(expansionContext, TC, conventions,
                                    results, subst);
    destructurer.destructure(origResultType, substFormalResultType);
  }

  // Lower the capture context parameters, if any.
  if (constant && constant->getAnyFunctionRef()) {
    auto expansion = TypeExpansionContext::maximal(
        expansionContext.getContext(), expansionContext.isWholeModuleContext());
    if (constant->isSerialized())
      expansion = TypeExpansionContext::minimal();
    lowerCaptureContextParameters(TC, *constant, genericSig, expansion, inputs);
  }
  
  auto calleeConvention = ParameterConvention::Direct_Unowned;
  if (extInfoBuilder.hasContext())
    calleeConvention = conventions.getCallee();

  bool pseudogeneric = genericSig && constant
    ? isPseudogeneric(*constant)
    : false;

  auto silRep = extInfoBuilder.getRepresentation();
  const clang::Type *clangType = extInfoBuilder.getClangTypeInfo().getType();
  if (shouldStoreClangType(silRep) && !clangType) {
    // If we have invalid code in the source like
    //     do { let x = 0; let _ : @convention(c) () -> Int = { x } }
    // we will fail to convert the corresponding SIL function type, as it will
    // have a sil_box_type { Int } parameter reflecting the capture. So we
    // convert the AST type instead.
    // N.B. The `do` is necessary; the code compiles at global scope.
    SmallVector<AnyFunctionType::Param, 4> params;
    for (auto ty : substFnInterfaceType.getParams())
      params.push_back(ty);
    clangType = TC.Context.getClangFunctionType(
        params, substFnInterfaceType.getResult(),
        convertRepresentation(silRep).getValue());
  }
  auto silExtInfo = extInfoBuilder.withClangFunctionType(clangType)
                        .withIsPseudogeneric(pseudogeneric)
                        .withAsync(isAsync)
                        .build();

  // Build the substituted generic signature we extracted.
  SubstitutionMap substitutions;
  if (subst.Enabled) {
    if (!subst.substGenericParams.empty()) {
      auto subSig = GenericSignature::get(subst.substGenericParams,
                                          subst.substRequirements)
                       .getCanonicalSignature();
      substitutions = SubstitutionMap::get(subSig,
                                   llvm::makeArrayRef(subst.substReplacements),
                                   llvm::makeArrayRef(subst.substConformances));
    }
  }
  
  return SILFunctionType::get(genericSig, silExtInfo, coroutineKind,
                              calleeConvention, inputs, yields,
                              results, errorResult,
                              substitutions, SubstitutionMap(),
                              TC.Context, witnessMethodConformance);
}

//===----------------------------------------------------------------------===//
//                        Deallocator SILFunctionTypes
//===----------------------------------------------------------------------===//

namespace {

// The convention for general deallocators.
struct DeallocatorConventions : Conventions {
  DeallocatorConventions() : Conventions(ConventionsKind::Deallocator) {}

  ParameterConvention getIndirectParameter(unsigned index,
                             const AbstractionPattern &type,
                             const TypeLowering &substTL) const override {
    llvm_unreachable("Deallocators do not have indirect parameters");
  }

  ParameterConvention getDirectParameter(unsigned index,
                             const AbstractionPattern &type,
                             const TypeLowering &substTL) const override {
    llvm_unreachable("Deallocators do not have non-self direct parameters");
  }

  ParameterConvention getCallee() const override {
    llvm_unreachable("Deallocators do not have callees");
  }

  ResultConvention getResult(const TypeLowering &tl) const override {
    // TODO: Put an unreachable here?
    return ResultConvention::Owned;
  }

  ParameterConvention
  getDirectSelfParameter(const AbstractionPattern &type) const override {
    // TODO: Investigate whether or not it is
    return ParameterConvention::Direct_Owned;
  }

  ParameterConvention
  getIndirectSelfParameter(const AbstractionPattern &type) const override {
    llvm_unreachable("Deallocators do not have indirect self parameters");
  }

  static bool classof(const Conventions *C) {
    return C->getKind() == ConventionsKind::Deallocator;
  }
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                      Default Convention FunctionTypes
//===----------------------------------------------------------------------===//

namespace {

enum class NormalParameterConvention { Owned, Guaranteed };

/// The default Swift conventions.
class DefaultConventions : public Conventions {
  NormalParameterConvention normalParameterConvention;

public:
  DefaultConventions(NormalParameterConvention normalParameterConvention)
      : Conventions(ConventionsKind::Default),
        normalParameterConvention(normalParameterConvention) {}

  bool isNormalParameterConventionGuaranteed() const {
    return normalParameterConvention == NormalParameterConvention::Guaranteed;
  }

  ParameterConvention getIndirectParameter(unsigned index,
                            const AbstractionPattern &type,
                            const TypeLowering &substTL) const override {
    if (isNormalParameterConventionGuaranteed()) {
      return ParameterConvention::Indirect_In_Guaranteed;
    }
    return ParameterConvention::Indirect_In;
  }

  ParameterConvention getDirectParameter(unsigned index,
                            const AbstractionPattern &type,
                            const TypeLowering &substTL) const override {
    if (isNormalParameterConventionGuaranteed())
      return ParameterConvention::Direct_Guaranteed;
    return ParameterConvention::Direct_Owned;
  }

  ParameterConvention getCallee() const override {
    return DefaultThickCalleeConvention;
  }

  ResultConvention getResult(const TypeLowering &tl) const override {
    return ResultConvention::Owned;
  }

  ParameterConvention
  getDirectSelfParameter(const AbstractionPattern &type) const override {
    return ParameterConvention::Direct_Guaranteed;
  }

  ParameterConvention
  getIndirectSelfParameter(const AbstractionPattern &type) const override {
    return ParameterConvention::Indirect_In_Guaranteed;
  }

  static bool classof(const Conventions *C) {
    return C->getKind() == ConventionsKind::Default;
  }
};

/// The default conventions for Swift initializing constructors.
///
/// Initializing constructors take all parameters (including) self at +1. This
/// is because:
///
/// 1. We are likely to be initializing fields of self implying that the
///    parameters are likely to be forwarded into memory without further
///    copies.
/// 2. Initializers must take 'self' at +1, since they will return it back
///    at +1, and may chain onto Objective-C initializers that replace the
///    instance.
struct DefaultInitializerConventions : DefaultConventions {
  DefaultInitializerConventions()
      : DefaultConventions(NormalParameterConvention::Owned) {}

  /// Initializers must take 'self' at +1, since they will return it back at +1,
  /// and may chain onto Objective-C initializers that replace the instance.
  ParameterConvention
  getDirectSelfParameter(const AbstractionPattern &type) const override {
    return ParameterConvention::Direct_Owned;
  }
  
  ParameterConvention
  getIndirectSelfParameter(const AbstractionPattern &type) const override {
    return ParameterConvention::Indirect_In;
  }
};

/// The convention used for allocating inits. Allocating inits take their normal
/// parameters at +1 and do not have a self parameter.
struct DefaultAllocatorConventions : DefaultConventions {
  DefaultAllocatorConventions()
      : DefaultConventions(NormalParameterConvention::Owned) {}

  ParameterConvention
  getDirectSelfParameter(const AbstractionPattern &type) const override {
    llvm_unreachable("Allocating inits do not have self parameters");
  }

  ParameterConvention
  getIndirectSelfParameter(const AbstractionPattern &type) const override {
    llvm_unreachable("Allocating inits do not have self parameters");
  }
};

/// The default conventions for Swift setter acccessors.
///
/// These take self at +0, but all other parameters at +1. This is because we
/// assume that setter parameters are likely to be values to be forwarded into
/// memory. Thus by passing in the +1 value, we avoid a potential copy in that
/// case.
struct DefaultSetterConventions : DefaultConventions {
  DefaultSetterConventions()
      : DefaultConventions(NormalParameterConvention::Owned) {}
};

/// The default conventions for ObjC blocks.
struct DefaultBlockConventions : Conventions {
  DefaultBlockConventions() : Conventions(ConventionsKind::DefaultBlock) {}

  ParameterConvention getIndirectParameter(unsigned index,
                            const AbstractionPattern &type,
                            const TypeLowering &substTL) const override {
    llvm_unreachable("indirect block parameters unsupported");
  }

  ParameterConvention getDirectParameter(unsigned index,
                            const AbstractionPattern &type,
                            const TypeLowering &substTL) const override {
    return ParameterConvention::Direct_Unowned;
  }

  ParameterConvention getCallee() const override {
    return ParameterConvention::Direct_Unowned;
  }

  ResultConvention getResult(const TypeLowering &substTL) const override {
    return ResultConvention::Autoreleased;
  }

  ParameterConvention
  getDirectSelfParameter(const AbstractionPattern &type) const override {
    llvm_unreachable("objc blocks do not have a self parameter");
  }

  ParameterConvention
  getIndirectSelfParameter(const AbstractionPattern &type) const override {
    llvm_unreachable("objc blocks do not have a self parameter");
  }

  static bool classof(const Conventions *C) {
    return C->getKind() == ConventionsKind::DefaultBlock;
  }
};

} // end anonymous namespace

static CanSILFunctionType getSILFunctionTypeForAbstractCFunction(
    TypeConverter &TC, AbstractionPattern origType,
    CanAnyFunctionType substType, SILExtInfoBuilder extInfoBuilder,
    Optional<SILDeclRef> constant);

static CanSILFunctionType getNativeSILFunctionType(
    TypeConverter &TC, TypeExpansionContext context,
    AbstractionPattern origType, CanAnyFunctionType substInterfaceType,
    SILExtInfoBuilder extInfoBuilder, Optional<SILDeclRef> origConstant,
    Optional<SILDeclRef> constant, Optional<SubstitutionMap> reqtSubs,
    ProtocolConformanceRef witnessMethodConformance) {
  assert(bool(origConstant) == bool(constant));
  auto getSILFunctionTypeForConventions =
      [&](const Conventions &convs) -> CanSILFunctionType {
    return getSILFunctionType(TC, context, origType, substInterfaceType,
                              extInfoBuilder, convs, ForeignInfo(),
                              origConstant, constant, reqtSubs,
                              witnessMethodConformance);
  };
  switch (extInfoBuilder.getRepresentation()) {
  case SILFunctionType::Representation::Block:
  case SILFunctionType::Representation::CFunctionPointer:
  case SILFunctionTypeRepresentation::CXXMethod:
    return getSILFunctionTypeForAbstractCFunction(
        TC, origType, substInterfaceType, extInfoBuilder, constant);

  case SILFunctionType::Representation::Thin:
  case SILFunctionType::Representation::ObjCMethod:
  case SILFunctionType::Representation::Thick:
  case SILFunctionType::Representation::Method:
  case SILFunctionType::Representation::Closure:
  case SILFunctionType::Representation::WitnessMethod: {
    switch (constant ? constant->kind : SILDeclRef::Kind::Func) {
    case SILDeclRef::Kind::Initializer:
    case SILDeclRef::Kind::EnumElement:
      return getSILFunctionTypeForConventions(DefaultInitializerConventions());
    case SILDeclRef::Kind::Allocator:
      return getSILFunctionTypeForConventions(DefaultAllocatorConventions());
    case SILDeclRef::Kind::Func: {
      // If we have a setter, use the special setter convention. This ensures
      // that we take normal parameters at +1.
      if (constant && constant->isSetter()) {
        return getSILFunctionTypeForConventions(DefaultSetterConventions());
      }
      return getSILFunctionTypeForConventions(
          DefaultConventions(NormalParameterConvention::Guaranteed));
    }
    case SILDeclRef::Kind::Destroyer:
    case SILDeclRef::Kind::GlobalAccessor:
    case SILDeclRef::Kind::DefaultArgGenerator:
    case SILDeclRef::Kind::StoredPropertyInitializer:
    case SILDeclRef::Kind::PropertyWrapperBackingInitializer:
    case SILDeclRef::Kind::IVarInitializer:
    case SILDeclRef::Kind::IVarDestroyer:
      return getSILFunctionTypeForConventions(
          DefaultConventions(NormalParameterConvention::Guaranteed));
    case SILDeclRef::Kind::Deallocator:
      return getSILFunctionTypeForConventions(DeallocatorConventions());
    }
  }
  }

  llvm_unreachable("Unhandled SILDeclRefKind in switch.");
}

CanSILFunctionType swift::getNativeSILFunctionType(
    TypeConverter &TC, TypeExpansionContext context,
    AbstractionPattern origType, CanAnyFunctionType substType,
    SILExtInfo silExtInfo, Optional<SILDeclRef> origConstant,
    Optional<SILDeclRef> substConstant, Optional<SubstitutionMap> reqtSubs,
    ProtocolConformanceRef witnessMethodConformance) {

  return ::getNativeSILFunctionType(
      TC, context, origType, substType, silExtInfo.intoBuilder(), origConstant,
      substConstant, reqtSubs, witnessMethodConformance);
}

//===----------------------------------------------------------------------===//
//                          Foreign SILFunctionTypes
//===----------------------------------------------------------------------===//

static bool isCFTypedef(const TypeLowering &tl, clang::QualType type) {
  // If we imported a C pointer type as a non-trivial type, it was
  // a foreign class type.
  return !tl.isTrivial() && type->isPointerType();
}

/// Given nothing but a formal C parameter type that's passed
/// indirectly, deduce the convention for it.
///
/// Generally, whether the parameter is +1 is handled before this.
static ParameterConvention getIndirectCParameterConvention(clang::QualType type) {
  // Non-trivial C++ types would be Indirect_Inout (at least in Itanium).
  // A trivial const * parameter in C should be considered @in.
  return ParameterConvention::Indirect_In;
}

/// Given a C parameter declaration whose type is passed indirectly,
/// deduce the convention for it.
///
/// Generally, whether the parameter is +1 is handled before this.
static ParameterConvention
getIndirectCParameterConvention(const clang::ParmVarDecl *param) {
  return getIndirectCParameterConvention(param->getType());
}

/// Given nothing but a formal C parameter type that's passed
/// directly, deduce the convention for it.
///
/// Generally, whether the parameter is +1 is handled before this.
static ParameterConvention getDirectCParameterConvention(clang::QualType type) {
  return ParameterConvention::Direct_Unowned;
}

/// Given a C parameter declaration whose type is passed directly,
/// deduce the convention for it.
static ParameterConvention
getDirectCParameterConvention(const clang::ParmVarDecl *param) {
  if (param->hasAttr<clang::NSConsumedAttr>() ||
      param->hasAttr<clang::CFConsumedAttr>())
    return ParameterConvention::Direct_Owned;
  return getDirectCParameterConvention(param->getType());
}

// FIXME: that should be Direct_Guaranteed
const auto ObjCSelfConvention = ParameterConvention::Direct_Unowned;

namespace {

class ObjCMethodConventions : public Conventions {
  const clang::ObjCMethodDecl *Method;

public:
  const clang::ObjCMethodDecl *getMethod() const { return Method; }

  ObjCMethodConventions(const clang::ObjCMethodDecl *method)
    : Conventions(ConventionsKind::ObjCMethod), Method(method) {}

  ParameterConvention getIndirectParameter(unsigned index,
                           const AbstractionPattern &type,
                           const TypeLowering &substTL) const override {
    return getIndirectCParameterConvention(Method->param_begin()[index]);
  }

  ParameterConvention getDirectParameter(unsigned index,
                           const AbstractionPattern &type,
                           const TypeLowering &substTL) const override {
    return getDirectCParameterConvention(Method->param_begin()[index]);
  }

  ParameterConvention getCallee() const override {
    // Always thin.
    return ParameterConvention::Direct_Unowned;
  }

  /// Given that a method returns a CF type, infer its method
  /// family.  Unfortunately, Clang's getMethodFamily() never
  /// considers a method to be in a special family if its result
  /// doesn't satisfy isObjCRetainable().
  clang::ObjCMethodFamily getMethodFamilyForCFResult() const {
    // Trust an explicit attribute.
    if (auto attr = Method->getAttr<clang::ObjCMethodFamilyAttr>()) {
      switch (attr->getFamily()) {
      case clang::ObjCMethodFamilyAttr::OMF_None:
        return clang::OMF_None;
      case clang::ObjCMethodFamilyAttr::OMF_alloc:
        return clang::OMF_alloc;
      case clang::ObjCMethodFamilyAttr::OMF_copy:
        return clang::OMF_copy;
      case clang::ObjCMethodFamilyAttr::OMF_init:
        return clang::OMF_init;
      case clang::ObjCMethodFamilyAttr::OMF_mutableCopy:
        return clang::OMF_mutableCopy;
      case clang::ObjCMethodFamilyAttr::OMF_new:
        return clang::OMF_new;
      }
      llvm_unreachable("bad attribute value");
    }

    return Method->getSelector().getMethodFamily();
  }

  bool isImplicitPlusOneCFResult() const {
    switch (getMethodFamilyForCFResult()) {
    case clang::OMF_None:
    case clang::OMF_dealloc:
    case clang::OMF_finalize:
    case clang::OMF_retain:
    case clang::OMF_release:
    case clang::OMF_autorelease:
    case clang::OMF_retainCount:
    case clang::OMF_self:
    case clang::OMF_initialize:
    case clang::OMF_performSelector:
      return false;

    case clang::OMF_alloc:
    case clang::OMF_new:
    case clang::OMF_mutableCopy:
    case clang::OMF_copy:
      return true;

    case clang::OMF_init:
      return Method->isInstanceMethod();
    }
    llvm_unreachable("bad method family");
  }

  ResultConvention getResult(const TypeLowering &tl) const override {
    // If we imported the result as something trivial, we need to
    // use one of the unowned conventions.
    if (tl.isTrivial()) {
      if (Method->hasAttr<clang::ObjCReturnsInnerPointerAttr>())
        return ResultConvention::UnownedInnerPointer;

      auto type = tl.getLoweredType();
      if (type.unwrapOptionalType().getStructOrBoundGenericStruct()
          == type.getASTContext().getUnmanagedDecl())
        return ResultConvention::UnownedInnerPointer;
      return ResultConvention::Unowned;
    }

    // Otherwise, the return type had better be a retainable object pointer.
    auto resultType = Method->getReturnType();
    assert(resultType->isObjCRetainableType() || isCFTypedef(tl, resultType));

    // If it's retainable for the purposes of ObjC ARC, we can trust
    // the presence of ns_returns_retained, because Clang will add
    // that implicitly based on the method family.
    if (resultType->isObjCRetainableType()) {
      if (Method->hasAttr<clang::NSReturnsRetainedAttr>())
        return ResultConvention::Owned;
      return ResultConvention::Autoreleased;
    }

    // Otherwise, it's a CF return type, which unfortunately means
    // we can't just trust getMethodFamily().  We should really just
    // change that, but that's an annoying change to make to Clang
    // right now.
    assert(isCFTypedef(tl, resultType));

    // Trust the explicit attributes.
    if (Method->hasAttr<clang::CFReturnsRetainedAttr>())
      return ResultConvention::Owned;
    if (Method->hasAttr<clang::CFReturnsNotRetainedAttr>())
      return ResultConvention::Autoreleased;

    // Otherwise, infer based on the method family.
    if (isImplicitPlusOneCFResult())
      return ResultConvention::Owned;
    return ResultConvention::Autoreleased;
  }

  ParameterConvention
  getDirectSelfParameter(const AbstractionPattern &type) const override {
    if (Method->hasAttr<clang::NSConsumesSelfAttr>())
      return ParameterConvention::Direct_Owned;

    // The caller is supposed to take responsibility for ensuring
    // that 'self' survives a method call.
    return ObjCSelfConvention;
  }

  ParameterConvention
  getIndirectSelfParameter(const AbstractionPattern &type) const override {
    llvm_unreachable("objc methods do not support indirect self parameters");
  }

  static bool classof(const Conventions *C) {
    return C->getKind() == ConventionsKind::ObjCMethod;
  }
};

/// Conventions based on a C function type.
class CFunctionTypeConventions : public Conventions {
  const clang::FunctionType *FnType;

  clang::QualType getParamType(unsigned i) const {
    return FnType->castAs<clang::FunctionProtoType>()->getParamType(i);
  }

protected:
  /// Protected constructor for subclasses to override the kind passed to the
  /// super class.
  CFunctionTypeConventions(ConventionsKind kind,
                           const clang::FunctionType *type)
    : Conventions(kind), FnType(type) {}

public:
  CFunctionTypeConventions(const clang::FunctionType *type)
    : Conventions(ConventionsKind::CFunctionType), FnType(type) {}

  ParameterConvention getIndirectParameter(unsigned index,
                            const AbstractionPattern &type,
                           const TypeLowering &substTL) const override {
    return getIndirectCParameterConvention(getParamType(index));
  }

  ParameterConvention getDirectParameter(unsigned index,
                            const AbstractionPattern &type,
                           const TypeLowering &substTL) const override {
    if (cast<clang::FunctionProtoType>(FnType)->isParamConsumed(index))
      return ParameterConvention::Direct_Owned;
    return getDirectCParameterConvention(getParamType(index));
  }

  ParameterConvention getCallee() const override {
    // FIXME: blocks should be Direct_Guaranteed.
    return ParameterConvention::Direct_Unowned;
  }

  ResultConvention getResult(const TypeLowering &tl) const override {
    if (tl.isTrivial())
      return ResultConvention::Unowned;
    if (FnType->getExtInfo().getProducesResult())
      return ResultConvention::Owned;
    return ResultConvention::Autoreleased;
  }

  ParameterConvention
  getDirectSelfParameter(const AbstractionPattern &type) const override {
    llvm_unreachable("c function types do not have a self parameter");
  }

  ParameterConvention
  getIndirectSelfParameter(const AbstractionPattern &type) const override {
    llvm_unreachable("c function types do not have a self parameter");
  }

  static bool classof(const Conventions *C) {
    return C->getKind() == ConventionsKind::CFunctionType;
  }
};

/// Conventions based on C function declarations.
class CFunctionConventions : public CFunctionTypeConventions {
  using super = CFunctionTypeConventions;
  const clang::FunctionDecl *TheDecl;
public:
  CFunctionConventions(const clang::FunctionDecl *decl)
    : CFunctionTypeConventions(ConventionsKind::CFunction,
                               decl->getType()->castAs<clang::FunctionType>()),
      TheDecl(decl) {}

  ParameterConvention getDirectParameter(unsigned index,
                            const AbstractionPattern &type,
                            const TypeLowering &substTL) const override {
    if (auto param = TheDecl->getParamDecl(index))
      if (param->hasAttr<clang::CFConsumedAttr>())
        return ParameterConvention::Direct_Owned;
    return super::getDirectParameter(index, type, substTL);
  }

  ResultConvention getResult(const TypeLowering &tl) const override {
    if (isCFTypedef(tl, TheDecl->getReturnType())) {
      // The CF attributes aren't represented in the type, so we need
      // to check them here.
      if (TheDecl->hasAttr<clang::CFReturnsRetainedAttr>()) {
        return ResultConvention::Owned;
      } else if (TheDecl->hasAttr<clang::CFReturnsNotRetainedAttr>()) {
        // Probably not actually autoreleased.
        return ResultConvention::Autoreleased;

      // The CF Create/Copy rule only applies to functions that return
      // a CF-runtime type; it does not apply to methods, and it does
      // not apply to functions returning ObjC types.
      } else if (clang::ento::coreFoundation::followsCreateRule(TheDecl)) {
        return ResultConvention::Owned;
      } else {
        return ResultConvention::Autoreleased;
      }
    }

    // Otherwise, fall back on the ARC annotations, which are part
    // of the type.
    return super::getResult(tl);
  }

  static bool classof(const Conventions *C) {
    return C->getKind() == ConventionsKind::CFunction;
  }
};

/// Conventions based on C++ method declarations.
class CXXMethodConventions : public CFunctionTypeConventions {
  using super = CFunctionTypeConventions;
  const clang::CXXMethodDecl *TheDecl;

public:
  CXXMethodConventions(const clang::CXXMethodDecl *decl)
      : CFunctionTypeConventions(
            ConventionsKind::CXXMethod,
            decl->getType()->castAs<clang::FunctionType>()),
        TheDecl(decl) {}
  ParameterConvention
  getIndirectSelfParameter(const AbstractionPattern &type) const override {
    if (TheDecl->isConst())
      return ParameterConvention::Indirect_In_Guaranteed;
    return ParameterConvention::Indirect_Inout;
  }
  ResultConvention getResult(const TypeLowering &resultTL) const override {
    if (isa<clang::CXXConstructorDecl>(TheDecl)) {
      // Represent the `this` pointer as an indirectly returned result.
      // This gets us most of the way towards representing the ABI of a
      // constructor correctly, but it's not guaranteed to be entirely correct.
      // C++ constructor ABIs are complicated and can require passing additional
      // "implicit" arguments that depend not only on the signature of the
      // constructor but on the class on which it's defined (e.g. whether that
      // class has a virtual base class).
      // Effectively, we're making an assumption here that there are no implicit
      // arguments and that the return type of the constructor ABI is void (and
      // indeed we have no way to represent anything else here). If this assumed
      // ABI doesn't match the actual ABI, we insert a thunk in IRGen. On some
      // ABIs (e.g. Itanium x64), we get lucky and the ABI for a complete
      // constructor call always matches the ABI we assume here. Even if the
      // actual ABI doesn't match the assumed ABI, we try to get as close as
      // possible to make it easy for LLVM to optimize away the thunk.
      return ResultConvention::Indirect;
    }
    return CFunctionTypeConventions::getResult(resultTL);
  }
  static bool classof(const Conventions *C) {
    return C->getKind() == ConventionsKind::CXXMethod;
  }
};

} // end anonymous namespace

/// Given that we have an imported Clang declaration, deduce the
/// ownership conventions for calling it and build the SILFunctionType.
static CanSILFunctionType getSILFunctionTypeForClangDecl(
    TypeConverter &TC, const clang::Decl *clangDecl,
    CanAnyFunctionType origType, CanAnyFunctionType substInterfaceType,
    SILExtInfoBuilder extInfoBuilder, const ForeignInfo &foreignInfo,
    Optional<SILDeclRef> constant) {
  if (auto method = dyn_cast<clang::ObjCMethodDecl>(clangDecl)) {
    auto origPattern =
      AbstractionPattern::getObjCMethod(origType, method,
                                        foreignInfo.Error,
                                        foreignInfo.Async);
    return getSILFunctionType(
        TC, TypeExpansionContext::minimal(), origPattern, substInterfaceType,
        extInfoBuilder, ObjCMethodConventions(method), foreignInfo, constant,
        constant, None, ProtocolConformanceRef());
  }

  if (auto method = dyn_cast<clang::CXXMethodDecl>(clangDecl)) {
    AbstractionPattern origPattern = method->isOverloadedOperator() ?
        AbstractionPattern::getCXXOperatorMethod(origType, method):
        AbstractionPattern::getCXXMethod(origType, method);
    auto conventions = CXXMethodConventions(method);
    return getSILFunctionType(TC, TypeExpansionContext::minimal(), origPattern,
                              substInterfaceType, extInfoBuilder, conventions,
                              foreignInfo, constant, constant, None,
                              ProtocolConformanceRef());
  }

  if (auto func = dyn_cast<clang::FunctionDecl>(clangDecl)) {
    auto clangType = func->getType().getTypePtr();
    AbstractionPattern origPattern =
      foreignInfo.Self.isImportAsMember()
        ? AbstractionPattern::getCFunctionAsMethod(origType, clangType,
                                                   foreignInfo.Self)
        : AbstractionPattern(origType, clangType);
    return getSILFunctionType(TC, TypeExpansionContext::minimal(), origPattern,
                              substInterfaceType, extInfoBuilder,
                              CFunctionConventions(func), foreignInfo, constant,
                              constant, None, ProtocolConformanceRef());
  }

  llvm_unreachable("call to unknown kind of C function");
}

static CanSILFunctionType getSILFunctionTypeForAbstractCFunction(
    TypeConverter &TC, AbstractionPattern origType,
    CanAnyFunctionType substType, SILExtInfoBuilder extInfoBuilder,
    Optional<SILDeclRef> constant) {
  if (origType.isClangType()) {
    auto clangType = origType.getClangType();
    const clang::FunctionType *fnType;
    if (auto blockPtr = clangType->getAs<clang::BlockPointerType>()) {
      fnType = blockPtr->getPointeeType()->castAs<clang::FunctionType>();
    } else if (auto ptr = clangType->getAs<clang::PointerType>()) {
      fnType = ptr->getPointeeType()->getAs<clang::FunctionType>();
    } else if (auto ref = clangType->getAs<clang::ReferenceType>()) {
      fnType = ref->getPointeeType()->getAs<clang::FunctionType>();
    } else if (auto fn = clangType->getAs<clang::FunctionType>()) {
      fnType = fn;
    } else {
      llvm_unreachable("unexpected type imported as a function type");
    }
    if (fnType) {
      return getSILFunctionType(
          TC, TypeExpansionContext::minimal(), origType, substType,
          extInfoBuilder, CFunctionTypeConventions(fnType), ForeignInfo(),
          constant, constant, None, ProtocolConformanceRef());
    }
  }

  // TODO: Ought to support captures in block funcs.
  return getSILFunctionType(TC, TypeExpansionContext::minimal(), origType,
                            substType, extInfoBuilder,
                            DefaultBlockConventions(), ForeignInfo(), constant,
                            constant, None, ProtocolConformanceRef());
}

/// Try to find a clang method declaration for the given function.
static const clang::Decl *findClangMethod(ValueDecl *method) {
  if (auto *methodFn = dyn_cast<FuncDecl>(method)) {
    if (auto *decl = methodFn->getClangDecl())
      return decl;

    if (auto overridden = methodFn->getOverriddenDecl())
      return findClangMethod(overridden);
  }

  if (auto *constructor = dyn_cast<ConstructorDecl>(method)) {
    if (auto *decl = constructor->getClangDecl())
      return decl;
  }

  return nullptr;
}

//===----------------------------------------------------------------------===//
//                      Selector Family SILFunctionTypes
//===----------------------------------------------------------------------===//

/// Derive the ObjC selector family from an identifier.
///
/// Note that this will never derive the Init family, which is too dangerous
/// to leave to chance. Swift functions starting with "init" are always
/// emitted as if they are part of the "none" family.
static ObjCSelectorFamily getObjCSelectorFamily(ObjCSelector name) {
  auto result = name.getSelectorFamily();

  if (result == ObjCSelectorFamily::Init)
    return ObjCSelectorFamily::None;

  return result;
}

/// Get the ObjC selector family a foreign SILDeclRef belongs to.
static ObjCSelectorFamily getObjCSelectorFamily(SILDeclRef c) {
  assert(c.isForeign);
  switch (c.kind) {
  case SILDeclRef::Kind::Func: {
    if (!c.hasDecl())
      return ObjCSelectorFamily::None;
      
    auto *FD = cast<FuncDecl>(c.getDecl());
    if (auto accessor = dyn_cast<AccessorDecl>(FD)) {
      switch (accessor->getAccessorKind()) {
      case AccessorKind::Get:
      case AccessorKind::Set:
        break;
#define OBJC_ACCESSOR(ID, KEYWORD)
#define ACCESSOR(ID) \
      case AccessorKind::ID:
#include "swift/AST/AccessorKinds.def"
        llvm_unreachable("Unexpected AccessorKind of foreign FuncDecl");
      }
    }

    return getObjCSelectorFamily(FD->getObjCSelector());
  }
  case SILDeclRef::Kind::Initializer:
  case SILDeclRef::Kind::IVarInitializer:
    return ObjCSelectorFamily::Init;

  /// Currently IRGen wraps alloc/init methods into Swift constructors
  /// with Swift conventions.
  case SILDeclRef::Kind::Allocator:
  /// These constants don't correspond to method families we care about yet.
  case SILDeclRef::Kind::Destroyer:
  case SILDeclRef::Kind::Deallocator:
  case SILDeclRef::Kind::IVarDestroyer:
    return ObjCSelectorFamily::None;

  case SILDeclRef::Kind::EnumElement:
  case SILDeclRef::Kind::GlobalAccessor:
  case SILDeclRef::Kind::DefaultArgGenerator:
  case SILDeclRef::Kind::StoredPropertyInitializer:
  case SILDeclRef::Kind::PropertyWrapperBackingInitializer:
    llvm_unreachable("Unexpected Kind of foreign SILDeclRef");
  }

  llvm_unreachable("Unhandled SILDeclRefKind in switch.");
}

namespace {

class ObjCSelectorFamilyConventions : public Conventions {
  ObjCSelectorFamily Family;

public:
  ObjCSelectorFamilyConventions(ObjCSelectorFamily family)
    : Conventions(ConventionsKind::ObjCSelectorFamily), Family(family) {}

  ParameterConvention getIndirectParameter(unsigned index,
                                           const AbstractionPattern &type,
                                 const TypeLowering &substTL) const override {
    return ParameterConvention::Indirect_In;
  }

  ParameterConvention getDirectParameter(unsigned index,
                                         const AbstractionPattern &type,
                                 const TypeLowering &substTL) const override {
    return ParameterConvention::Direct_Unowned;
  }

  ParameterConvention getCallee() const override {
    // Always thin.
    return ParameterConvention::Direct_Unowned;
  }

  ResultConvention getResult(const TypeLowering &tl) const override {
    switch (Family) {
    case ObjCSelectorFamily::Alloc:
    case ObjCSelectorFamily::Copy:
    case ObjCSelectorFamily::Init:
    case ObjCSelectorFamily::MutableCopy:
    case ObjCSelectorFamily::New:
      return ResultConvention::Owned;

    case ObjCSelectorFamily::None:
      // Defaults below.
      break;
    }

    // Get the underlying AST type, potentially stripping off one level of
    // optionality while we do it.
    CanType type = tl.getLoweredType().unwrapOptionalType().getASTType();
    if (type->hasRetainablePointerRepresentation()
        || (type->getSwiftNewtypeUnderlyingType() && !tl.isTrivial()))
      return ResultConvention::Autoreleased;

    return ResultConvention::Unowned;
  }

  ParameterConvention
  getDirectSelfParameter(const AbstractionPattern &type) const override {
    if (Family == ObjCSelectorFamily::Init)
      return ParameterConvention::Direct_Owned;
    return ObjCSelfConvention;
  }

  ParameterConvention
  getIndirectSelfParameter(const AbstractionPattern &type) const override {
    llvm_unreachable("selector family objc function types do not support "
                     "indirect self parameters");
  }

  static bool classof(const Conventions *C) {
    return C->getKind() == ConventionsKind::ObjCSelectorFamily;
  }
};

} // end anonymous namespace

static CanSILFunctionType getSILFunctionTypeForObjCSelectorFamily(
    TypeConverter &TC, ObjCSelectorFamily family, CanAnyFunctionType origType,
    CanAnyFunctionType substInterfaceType, SILExtInfoBuilder extInfoBuilder,
    const ForeignInfo &foreignInfo, Optional<SILDeclRef> constant) {
  return getSILFunctionType(
      TC, TypeExpansionContext::minimal(), AbstractionPattern(origType),
      substInterfaceType, extInfoBuilder, ObjCSelectorFamilyConventions(family),
      foreignInfo, constant, constant,
      /*requirement subs*/ None, ProtocolConformanceRef());
}

static bool isImporterGeneratedAccessor(const clang::Decl *clangDecl,
                                        SILDeclRef constant) {
  // Must be an accessor.
  auto accessor = dyn_cast<AccessorDecl>(constant.getDecl());
  if (!accessor)
    return false;

  // Must be a type member.
  if (constant.getParameterListCount() != 2)
    return false;

  // Must be imported from a function.
  if (!isa<clang::FunctionDecl>(clangDecl))
    return false;

  return true;
}

static CanSILFunctionType getUncachedSILFunctionTypeForConstant(
    TypeConverter &TC, TypeExpansionContext context, SILDeclRef constant,
    TypeConverter::LoweredFormalTypes bridgedTypes) {
  auto silRep = TC.getDeclRefRepresentation(constant);
  assert(silRep != SILFunctionTypeRepresentation::Thick &&
         silRep != SILFunctionTypeRepresentation::Block);

  auto origLoweredInterfaceType = bridgedTypes.Uncurried;
  auto extInfo = origLoweredInterfaceType->getExtInfo();

  auto extInfoBuilder =
      SILExtInfo(extInfo, false).intoBuilder().withRepresentation(silRep);

  if (shouldStoreClangType(silRep)) {
    const clang::Type *clangType = nullptr;
    if (bridgedTypes.Pattern.isClangType()) {
      clangType = bridgedTypes.Pattern.getClangType();
    }
    if (clangType) {
      // According to [NOTE: ClangTypeInfo-contents], we need to wrap a function
      // type in an additional clang::PointerType.
      if (clangType->isFunctionType()) {
        clangType =
            static_cast<ClangImporter *>(TC.Context.getClangModuleLoader())
                ->getClangASTContext()
                .getPointerType(clang::QualType(clangType, 0))
                .getTypePtr();
      }
      extInfoBuilder = extInfoBuilder.withClangFunctionType(clangType);
    }
  }

  if (!constant.isForeign) {
    ProtocolConformanceRef witnessMethodConformance;

    if (silRep == SILFunctionTypeRepresentation::WitnessMethod) {
      auto proto = constant.getDecl()->getDeclContext()->getSelfProtocolDecl();
      witnessMethodConformance = ProtocolConformanceRef(proto);
    }

    return ::getNativeSILFunctionType(
        TC, context, AbstractionPattern(origLoweredInterfaceType),
        origLoweredInterfaceType, extInfoBuilder, constant, constant, None,
        witnessMethodConformance);
  }

  ForeignInfo foreignInfo;

  // If we have a clang decl associated with the Swift decl, derive its
  // ownership conventions.
  if (constant.hasDecl()) {
    auto decl = constant.getDecl();
    if (auto funcDecl = dyn_cast<AbstractFunctionDecl>(decl)) {
      foreignInfo = ForeignInfo{
        funcDecl->getImportAsMemberStatus(),
        funcDecl->getForeignErrorConvention(),
        funcDecl->getForeignAsyncConvention(),
      };
    }

    if (auto clangDecl = findClangMethod(decl)) {
      // The importer generates accessors that are not actually
      // import-as-member but do involve the same gymnastics with the
      // formal type.  That's all that SILFunctionType cares about, so
      // pretend that it's import-as-member.
      if (!foreignInfo.Self.isImportAsMember() &&
          isImporterGeneratedAccessor(clangDecl, constant)) {
        unsigned selfIndex = cast<AccessorDecl>(decl)->isSetter() ? 1 : 0;
        foreignInfo.Self.setSelfIndex(selfIndex);
      }

      return getSILFunctionTypeForClangDecl(
          TC, clangDecl, origLoweredInterfaceType, origLoweredInterfaceType,
          extInfoBuilder, foreignInfo, constant);
    }
  }

  // If the decl belongs to an ObjC method family, use that family's
  // ownership conventions.
  return getSILFunctionTypeForObjCSelectorFamily(
      TC, getObjCSelectorFamily(constant), origLoweredInterfaceType,
      origLoweredInterfaceType, extInfoBuilder, foreignInfo, constant);
}

CanSILFunctionType TypeConverter::getUncachedSILFunctionTypeForConstant(
    TypeExpansionContext context, SILDeclRef constant,
    CanAnyFunctionType origInterfaceType) {
  auto bridgedTypes = getLoweredFormalTypes(constant, origInterfaceType);
  return ::getUncachedSILFunctionTypeForConstant(*this, context, constant,
                                                 bridgedTypes);
}

static bool isClassOrProtocolMethod(ValueDecl *vd) {
  if (!vd->getDeclContext())
    return false;
  Type contextType = vd->getDeclContext()->getDeclaredInterfaceType();
  if (!contextType)
    return false;
  return contextType->getClassOrBoundGenericClass()
    || contextType->isClassExistentialType();
}

SILFunctionTypeRepresentation
TypeConverter::getDeclRefRepresentation(SILDeclRef c) {
  // If this is a foreign thunk, it always has the foreign calling convention.
  if (c.isForeign) {
    if (!c.hasDecl())
      return SILFunctionTypeRepresentation::CFunctionPointer;

    // TODO: Is this correct for operators?
    if (auto method =
            dyn_cast_or_null<clang::CXXMethodDecl>(c.getDecl()->getClangDecl()))
      return isa<clang::CXXConstructorDecl>(method) || method->isStatic() ||
                     method->isOverloadedOperator()
                 ? SILFunctionTypeRepresentation::CFunctionPointer
                 : SILFunctionTypeRepresentation::CXXMethod;

    // For example, if we have a function in a namespace:
    if (c.getDecl()->isImportAsMember())
      return SILFunctionTypeRepresentation::CFunctionPointer;

    if (isClassOrProtocolMethod(c.getDecl()) ||
        c.kind == SILDeclRef::Kind::IVarInitializer ||
        c.kind == SILDeclRef::Kind::IVarDestroyer)
      return SILFunctionTypeRepresentation::ObjCMethod;

    return SILFunctionTypeRepresentation::CFunctionPointer;
  }

  // Anonymous functions currently always have Freestanding CC.
  if (!c.hasDecl())
    return SILFunctionTypeRepresentation::Thin;

  // FIXME: Assert that there is a native entry point
  // available. There's no great way to do this.

  // Protocol witnesses are called using the witness calling convention.
  if (auto proto = dyn_cast<ProtocolDecl>(c.getDecl()->getDeclContext())) {
    // Use the regular method convention for foreign-to-native thunks.
    if (c.isForeignToNativeThunk())
      return SILFunctionTypeRepresentation::Method;
    assert(!c.isNativeToForeignThunk() && "shouldn't be possible");
    return getProtocolWitnessRepresentation(proto);
  }

  switch (c.kind) {
    case SILDeclRef::Kind::GlobalAccessor:
    case SILDeclRef::Kind::DefaultArgGenerator:
    case SILDeclRef::Kind::StoredPropertyInitializer:
    case SILDeclRef::Kind::PropertyWrapperBackingInitializer:
      return SILFunctionTypeRepresentation::Thin;

    case SILDeclRef::Kind::Func:
      if (c.getDecl()->getDeclContext()->isTypeContext())
        return SILFunctionTypeRepresentation::Method;
      return SILFunctionTypeRepresentation::Thin;

    case SILDeclRef::Kind::Destroyer:
    case SILDeclRef::Kind::Deallocator:
    case SILDeclRef::Kind::Allocator:
    case SILDeclRef::Kind::Initializer:
    case SILDeclRef::Kind::EnumElement:
    case SILDeclRef::Kind::IVarInitializer:
    case SILDeclRef::Kind::IVarDestroyer:
      return SILFunctionTypeRepresentation::Method;
  }

  llvm_unreachable("Unhandled SILDeclRefKind in switch.");
}

// Provide the ability to turn off the type converter cache to ease debugging.
static llvm::cl::opt<bool>
    DisableConstantInfoCache("sil-disable-typelowering-constantinfo-cache",
                             llvm::cl::init(false));

const SILConstantInfo &
TypeConverter::getConstantInfo(TypeExpansionContext expansion,
                               SILDeclRef constant) {
  if (!DisableConstantInfoCache) {
    auto found = ConstantTypes.find(std::make_pair(expansion, constant));
    if (found != ConstantTypes.end())
      return *found->second;
  }

  // First, get a function type for the constant.  This creates the
  // right type for a getter or setter.
  auto formalInterfaceType = makeConstantInterfaceType(constant);

  // The lowered type is the formal type, but uncurried and with
  // parameters automatically turned into their bridged equivalents.
  auto bridgedTypes = getLoweredFormalTypes(constant, formalInterfaceType);

  CanAnyFunctionType loweredInterfaceType = bridgedTypes.Uncurried;

  // The SIL type encodes conventions according to the original type.
  CanSILFunctionType silFnType = ::getUncachedSILFunctionTypeForConstant(
      *this, expansion, constant, bridgedTypes);

  // If the constant refers to a derivative function, get the SIL type of the
  // original function and use it to compute the derivative SIL type.
  //
  // This is necessary because the "lowered AST derivative function type" (bc)
  // may differ from the "derivative type of the lowered original function type"
  // (ad):
  //
  // ┌────────────────────┐       lowering      ┌────────────────────┐
  // │ AST orig.  fn type │  ───────(a)──────►  │ SIL orig.  fn type │
  // └────────────────────┘                     └────────────────────┘
  //         │                                                │
  //    (b, Sema)   getAutoDiffDerivativeFunctionType     (d, here)
  //         │                                                │
  //         ▼                                                ▼
  // ┌────────────────────┐       lowering      ┌────────────────────┐
  // │ AST deriv. fn type │  ───────(c)──────►  │ SIL deriv. fn type │
  // └────────────────────┘                     └────────────────────┘
  //
  // (ad) does not always commute with (bc):
  // - (bc) is the result of computing the AST derivative type (Sema), then
  //   lowering it via SILGen. This is the default lowering behavior, but may
  //   break SIL typing invariants because expected lowered derivative types are
  //   computed from lowered original function types.
  // - (ad) is the result of lowering the original function type, then computing
  //   its derivative type. This is the expected lowered derivative type,
  //   preserving SIL typing invariants.
  //
  // Always use (ad) to compute lowered derivative function types.
  if (auto *derivativeId = constant.getDerivativeFunctionIdentifier()) {
    // Get lowered original function type.
    auto origFnConstantInfo = getConstantInfo(
        TypeExpansionContext::minimal(), constant.asAutoDiffOriginalFunction());
    // Use it to compute lowered derivative function type.
    auto *loweredParamIndices = autodiff::getLoweredParameterIndices(
        derivativeId->getParameterIndices(), formalInterfaceType);
    auto numResults =
        origFnConstantInfo.SILFnType->getNumResults() +
        origFnConstantInfo.SILFnType->getNumIndirectMutatingParameters();
    auto *loweredResultIndices = IndexSubset::getDefault(
        M.getASTContext(), numResults, /*includeAll*/ true);
    silFnType = origFnConstantInfo.SILFnType->getAutoDiffDerivativeFunctionType(
        loweredParamIndices, loweredResultIndices, derivativeId->getKind(),
        *this, LookUpConformanceInModule(&M));
  }

  LLVM_DEBUG(llvm::dbgs() << "lowering type for constant ";
             constant.print(llvm::dbgs());
             llvm::dbgs() << "\n  formal type: ";
             formalInterfaceType.print(llvm::dbgs());
             llvm::dbgs() << "\n  lowered AST type: ";
             loweredInterfaceType.print(llvm::dbgs());
             llvm::dbgs() << "\n  SIL type: ";
             silFnType.print(llvm::dbgs());
             llvm::dbgs() << "\n  Expansion context: "
                           << expansion.shouldLookThroughOpaqueTypeArchetypes();
             llvm::dbgs() << "\n");

  auto resultBuf = Context.Allocate(sizeof(SILConstantInfo),
                                    alignof(SILConstantInfo));

  auto result = ::new (resultBuf) SILConstantInfo{formalInterfaceType,
                                                  bridgedTypes.Pattern,
                                                  loweredInterfaceType,
                                                  silFnType};
  if (DisableConstantInfoCache)
    return *result;

  auto inserted =
      ConstantTypes.insert({std::make_pair(expansion, constant), result});
  assert(inserted.second);
  (void)inserted;
  return *result;
}

/// Returns the SILParameterInfo for the given declaration's `self` parameter.
/// `constant` must refer to a method.
SILParameterInfo
TypeConverter::getConstantSelfParameter(TypeExpansionContext context,
                                        SILDeclRef constant) {
  auto ty = getConstantFunctionType(context, constant);

  // In most cases the "self" parameter is lowered as the back parameter.
  // The exception is C functions imported as methods.
  if (!constant.isForeign)
    return ty->getParameters().back();
  if (!constant.hasDecl())
    return ty->getParameters().back();
  auto fn = dyn_cast<AbstractFunctionDecl>(constant.getDecl());
  if (!fn)
    return ty->getParameters().back();
  if (fn->isImportAsStaticMember())
    return SILParameterInfo();
  if (fn->isImportAsInstanceMember())
    return ty->getParameters()[fn->getSelfIndex()];
  return ty->getParameters().back();
}

// This check duplicates TypeConverter::checkForABIDifferences(),
// but on AST types. The issue is we only want to introduce a new
// vtable thunk if the AST type changes, but an abstraction change
// is OK; we don't want a new entry if an @in parameter became
// @guaranteed or whatever.
static bool checkASTTypeForABIDifferences(CanType type1,
                                          CanType type2) {
  return !type1->matches(type2, TypeMatchFlags::AllowABICompatible);
}

// FIXME: This makes me very upset. Can we do without this?
static CanType copyOptionalityFromDerivedToBase(TypeConverter &tc,
                                                CanType derived,
                                                CanType base) {
  // Unwrap optionals, but remember that we did.
  bool derivedWasOptional = false;
  if (auto object = derived.getOptionalObjectType()) {
    derivedWasOptional = true;
    derived = object;
  }
  if (auto object = base.getOptionalObjectType()) {
    base = object;
  }

  // T? +> S = (T +> S)?
  // T? +> S? = (T +> S)?
  if (derivedWasOptional) {
    base = copyOptionalityFromDerivedToBase(tc, derived, base);

    auto optDecl = tc.Context.getOptionalDecl();
    return CanType(BoundGenericEnumType::get(optDecl, Type(), base));
  }

  // (T1, T2, ...) +> (S1, S2, ...) = (T1 +> S1, T2 +> S2, ...)
  if (auto derivedTuple = dyn_cast<TupleType>(derived)) {
    if (auto baseTuple = dyn_cast<TupleType>(base)) {
      assert(derivedTuple->getNumElements() == baseTuple->getNumElements());
      SmallVector<TupleTypeElt, 4> elements;
      for (unsigned i = 0, e = derivedTuple->getNumElements(); i < e; i++) {
        elements.push_back(
          baseTuple->getElement(i).getWithType(
            copyOptionalityFromDerivedToBase(
              tc,
              derivedTuple.getElementType(i),
              baseTuple.getElementType(i))));
      }
      return CanType(TupleType::get(elements, tc.Context));
    }
  }

  // (T1 -> T2) +> (S1 -> S2) = (T1 +> S1) -> (T2 +> S2)
  if (auto derivedFunc = dyn_cast<AnyFunctionType>(derived)) {
    if (auto baseFunc = dyn_cast<AnyFunctionType>(base)) {
      SmallVector<FunctionType::Param, 8> params;

      auto derivedParams = derivedFunc.getParams();
      auto baseParams = baseFunc.getParams();
      assert(derivedParams.size() == baseParams.size());
      for (unsigned i = 0, e = derivedParams.size(); i < e; ++i) {
        assert(derivedParams[i].getParameterFlags() ==
               baseParams[i].getParameterFlags());

        params.emplace_back(
          copyOptionalityFromDerivedToBase(
            tc,
            derivedParams[i].getPlainType(),
            baseParams[i].getPlainType()),
          Identifier(),
          baseParams[i].getParameterFlags());
      }

      auto result = copyOptionalityFromDerivedToBase(tc,
                                                     derivedFunc.getResult(),
                                                     baseFunc.getResult());
      return CanAnyFunctionType::get(baseFunc.getOptGenericSignature(),
                                     llvm::makeArrayRef(params), result,
                                     baseFunc->getExtInfo());
    }
  }

  return base;
}

/// Returns the ConstantInfo corresponding to the VTable thunk for overriding.
/// Will be the same as getConstantInfo if the declaration does not override.
const SILConstantInfo &
TypeConverter::getConstantOverrideInfo(TypeExpansionContext context,
                                       SILDeclRef derived, SILDeclRef base) {
  // Foreign overrides currently don't need reabstraction.
  if (derived.isForeign)
    return getConstantInfo(context, derived);

  auto found = ConstantOverrideTypes.find({derived, base});
  if (found != ConstantOverrideTypes.end())
    return *found->second;

  assert(base.requiresNewVTableEntry() && "base must not be an override");

  // Figure out the generic signature for the class method call. This is the
  // signature of the derived class, with requirements transplanted from
  // the base method. The derived method is allowed to have fewer
  // requirements, in which case the thunk will translate the calling
  // convention appropriately before calling the derived method.
  bool hasGenericRequirementDifference = false;

  auto derivedSig = derived.getDecl()->getAsGenericContext()
                                     ->getGenericSignature();
  auto genericSig = Context.getOverrideGenericSignature(base.getDecl(),
                                                        derived.getDecl());
  if (genericSig) {
    hasGenericRequirementDifference =
      !genericSig->requirementsNotSatisfiedBy(derivedSig).empty();
  }

  auto baseInfo = getConstantInfo(context, base);
  auto derivedInfo = getConstantInfo(context, derived);

  auto params = derivedInfo.FormalType.getParams();
  assert(params.size() == 1);
  auto selfInterfaceTy = params[0].getPlainType()->getMetatypeInstanceType();

  auto overrideInterfaceTy =
    cast<AnyFunctionType>(
      selfInterfaceTy->adjustSuperclassMemberDeclType(
        base.getDecl(), derived.getDecl(), baseInfo.FormalType)
          ->getCanonicalType());

  // Build the formal AST function type for the class method call.
  auto basePattern = AbstractionPattern(baseInfo.LoweredType);

  if (!hasGenericRequirementDifference &&
      !checkASTTypeForABIDifferences(derivedInfo.FormalType,
                                     overrideInterfaceTy)) {

    // The derived method is ABI-compatible with the base method. Let's
    // just use the derived method's formal type.
    basePattern = AbstractionPattern(
      copyOptionalityFromDerivedToBase(
        *this,
        derivedInfo.LoweredType,
        baseInfo.LoweredType));
    overrideInterfaceTy = derivedInfo.FormalType;
  }

  if (genericSig && !genericSig->areAllParamsConcrete()) {
    overrideInterfaceTy =
      cast<AnyFunctionType>(
        GenericFunctionType::get(genericSig,
                                 overrideInterfaceTy->getParams(),
                                 overrideInterfaceTy->getResult(),
                                 overrideInterfaceTy->getExtInfo())
          ->getCanonicalType());
  }

  // Build the lowered AST function type for the class method call.
  auto bridgedTypes = getLoweredFormalTypes(derived, overrideInterfaceTy);

  // Build the SILFunctionType for the class method call.
  CanSILFunctionType fnTy = getNativeSILFunctionType(
      *this, context, basePattern, bridgedTypes.Uncurried,
      derivedInfo.SILFnType->getExtInfo(), base, derived,
      /*reqt subs*/ None, ProtocolConformanceRef());

  // Build the SILConstantInfo and cache it.
  auto resultBuf = Context.Allocate(sizeof(SILConstantInfo),
                                    alignof(SILConstantInfo));
  auto result = ::new (resultBuf) SILConstantInfo{
    overrideInterfaceTy,
    basePattern,
    bridgedTypes.Uncurried,
    fnTy};
  
  auto inserted = ConstantOverrideTypes.insert({{derived, base}, result});
  assert(inserted.second);
  (void)inserted;
  return *result;
}

namespace {

/// Given a lowered SIL type, apply a substitution to it to produce another
/// lowered SIL type which uses the same abstraction conventions.
class SILTypeSubstituter :
    public CanTypeVisitor<SILTypeSubstituter, CanType> {
  TypeConverter &TC;
  TypeSubstitutionFn Subst;
  LookupConformanceFn Conformances;
  // The signature for the original type.
  //
  // Replacement types are lowered with respect to the current
  // context signature.
  CanGenericSignature Sig;

  TypeExpansionContext typeExpansionContext;

  bool shouldSubstituteOpaqueArchetypes;

public:
  SILTypeSubstituter(TypeConverter &TC,
                     TypeExpansionContext context,
                     TypeSubstitutionFn Subst,
                     LookupConformanceFn Conformances,
                     CanGenericSignature Sig,
                     bool shouldSubstituteOpaqueArchetypes)
    : TC(TC),
      Subst(Subst),
      Conformances(Conformances),
      Sig(Sig),
      typeExpansionContext(context),
      shouldSubstituteOpaqueArchetypes(shouldSubstituteOpaqueArchetypes)
  {}

  // SIL type lowering only does special things to tuples and functions.

  // When a function appears inside of another type, we only perform
  // substitutions if it is not polymorphic.
  CanSILFunctionType visitSILFunctionType(CanSILFunctionType origType) {
    return substSILFunctionType(origType, false);
  }

  SubstitutionMap substSubstitutions(SubstitutionMap subs) {
    // Substitute the substitutions.
    SubstOptions options = None;
    if (shouldSubstituteOpaqueArchetypes)
      options |= SubstFlags::SubstituteOpaqueArchetypes;

    // Expand substituted type according to the expansion context.
    auto newSubs = subs.subst(Subst, Conformances, options);

    // If we need to look through opaque types in this context, re-substitute
    // according to the expansion context.
    newSubs = substOpaqueTypes(newSubs);

    return newSubs;
  }

  SubstitutionMap substOpaqueTypes(SubstitutionMap subs) {
    if (!typeExpansionContext.shouldLookThroughOpaqueTypeArchetypes())
      return subs;

    return subs.subst([&](SubstitutableType *s) -> Type {
        return substOpaqueTypesWithUnderlyingTypes(s->getCanonicalType(),
                                                   typeExpansionContext);
      }, [&](CanType dependentType,
             Type conformingReplacementType,
             ProtocolDecl *conformedProtocol) -> ProtocolConformanceRef {
        return substOpaqueTypesWithUnderlyingTypes(
               ProtocolConformanceRef(conformedProtocol),
               conformingReplacementType->getCanonicalType(),
               typeExpansionContext);
      }, SubstFlags::SubstituteOpaqueArchetypes);
  }

  // Substitute a function type.
  CanSILFunctionType substSILFunctionType(CanSILFunctionType origType,
                                          bool isGenericApplication) {
    assert((!isGenericApplication || origType->isPolymorphic()) &&
           "generic application without invocation signature or with "
           "existing arguments");
    assert((!isGenericApplication || !shouldSubstituteOpaqueArchetypes) &&
           "generic application while substituting opaque archetypes");

    // The general substitution rule is that we should only substitute
    // into the free components of the type, i.e. the components that
    // aren't inside a generic signature.  That rule would say:
    //
    // - If there are invocation substitutions, just substitute those;
    //   the other components are necessarily inside the invocation
    //   generic signature.
    //
    // - Otherwise, if there's an invocation generic signature,
    //   substitute nothing.  If we are applying generic arguments,
    //   add the appropriate invocation substitutions.
    //
    // - Otherwise, if there are pattern substitutions, just substitute
    //   those; the other components are inside the patttern generic
    //   signature.
    //
    // - Otherwise, substitute the basic components.
    //
    // There are two caveats here.  The first is that we haven't yet
    // written all the code that would be necessary in order to handle
    // invocation substitutions everywhere, and so we never build those.
    // Instead, we substitute into the pattern substitutions if present,
    // or the components if not, and build a type with no invocation
    // signature.  As a special case, when substituting a coroutine type,
    // we build pattern substitutions instead of substituting the
    // component types in order to preserve the original yield structure,
    // which factors into the continuation function ABI.
    //
    // The second is that this function is also used when substituting
    // opaque archetypes.  In this case, we may need to substitute
    // into component types even within generic signatures.  This is
    // safe because the substitutions used in this case don't change
    // generics, they just narrowly look through certain opaque archetypes.
    // If substitutions are present, we still don't substitute into
    // the basic components, in order to maintain the information about
    // what was abstracted there.

    auto patternSubs = origType->getPatternSubstitutions();

    // If we have an invocation signatture, we generally shouldn't
    // substitute into the pattern substitutions and component types.
    if (auto sig = origType->getInvocationGenericSignature()) {
      // Substitute the invocation substitutions if present.
      if (auto invocationSubs = origType->getInvocationSubstitutions()) {
        assert(!isGenericApplication);
        invocationSubs = substSubstitutions(invocationSubs);
        auto substType =
          origType->withInvocationSubstitutions(invocationSubs);

        // Also do opaque-type substitutions on the pattern substitutions
        // if requested and applicable.
        if (patternSubs) {
          patternSubs = substOpaqueTypes(patternSubs);
          substType = substType->withPatternSubstitutions(patternSubs);
        }

        return substType;
      }

      // Otherwise, we shouldn't substitute any components except
      // when substituting opaque archetypes.

      // If we're doing a generic application, and there are pattern
      // substitutions, substitute into the pattern substitutions; or if
      // it's a coroutine, build pattern substitutions; or else, fall
      // through to substitute the component types as discussed above.
      if (isGenericApplication) {
        if (patternSubs || origType->isCoroutine()) {
          CanSILFunctionType substType = origType;
          if (typeExpansionContext.shouldLookThroughOpaqueTypeArchetypes()) {
            substType =
              origType->substituteOpaqueArchetypes(TC, typeExpansionContext);
          }

          SubstitutionMap subs;
          if (patternSubs) {
            subs = substSubstitutions(patternSubs);
          } else {
            subs = SubstitutionMap::get(sig, Subst, Conformances);
          }
          auto witnessConformance = substWitnessConformance(origType);
          substType = substType->withPatternSpecialization(nullptr, subs,
                                                           witnessConformance);
          if (typeExpansionContext.shouldLookThroughOpaqueTypeArchetypes()) {
            substType =
              substType->substituteOpaqueArchetypes(TC, typeExpansionContext);
          }
          return substType;
        }
        // else fall down to component substitution

      // If we're substituting opaque archetypes, and there are pattern
      // substitutions present, just substitute those and preserve the
      // basic structure in the component types.  Otherwise, fall through
      // to substitute the component types.
      } else if (shouldSubstituteOpaqueArchetypes) {
        if (patternSubs) {
          patternSubs = substOpaqueTypes(patternSubs);
          auto witnessConformance = substWitnessConformance(origType);
          return origType->withPatternSpecialization(sig, patternSubs,
                                                     witnessConformance);
        }
        // else fall down to component substitution

      // Otherwise, don't try to substitute bound components.
      } else {
        auto substType = origType;
        if (patternSubs) {
          patternSubs = substOpaqueTypes(patternSubs);
          auto witnessConformance = substWitnessConformance(origType);
          substType = substType->withPatternSpecialization(sig, patternSubs,
                                                           witnessConformance);
        }
        return substType;
      }

    // Otherwise, if there are pattern substitutions, just substitute
    // into those and don't touch the component types.
    } else if (patternSubs) {
      patternSubs = substSubstitutions(patternSubs);
      auto witnessConformance = substWitnessConformance(origType);
      return origType->withPatternSpecialization(nullptr, patternSubs,
                                                 witnessConformance);
    }

    // Otherwise, we need to substitute component types.

    SmallVector<SILResultInfo, 8> substResults;
    substResults.reserve(origType->getNumResults());
    for (auto origResult : origType->getResults()) {
      substResults.push_back(substInterface(origResult));
    }

    auto substErrorResult = origType->getOptionalErrorResult();
    assert(!substErrorResult ||
           (!substErrorResult->getInterfaceType()->hasTypeParameter() &&
            !substErrorResult->getInterfaceType()->hasArchetype()));

    SmallVector<SILParameterInfo, 8> substParams;
    substParams.reserve(origType->getParameters().size());
    for (auto &origParam : origType->getParameters()) {
      substParams.push_back(substInterface(origParam));
    }

    SmallVector<SILYieldInfo, 8> substYields;
    substYields.reserve(origType->getYields().size());
    for (auto &origYield : origType->getYields()) {
      substYields.push_back(substInterface(origYield));
    }

    auto witnessMethodConformance = substWitnessConformance(origType);

    // The substituted type is no longer generic, so it'd never be
    // pseudogeneric.
    auto extInfo = origType->getExtInfo();
    if (!shouldSubstituteOpaqueArchetypes)
      extInfo = extInfo.intoBuilder().withIsPseudogeneric(false).build();

    auto genericSig = shouldSubstituteOpaqueArchetypes
                        ? origType->getInvocationGenericSignature()
                        : nullptr;

    return SILFunctionType::get(genericSig, extInfo,
                                origType->getCoroutineKind(),
                                origType->getCalleeConvention(), substParams,
                                substYields, substResults, substErrorResult,
                                SubstitutionMap(), SubstitutionMap(),
                                TC.Context, witnessMethodConformance);
  }

  ProtocolConformanceRef substWitnessConformance(CanSILFunctionType origType) {
    auto conformance = origType->getWitnessMethodConformanceOrInvalid();
    if (!conformance) return conformance;

    assert(origType->getExtInfo().hasSelfParam());
    auto selfType = origType->getSelfParameter().getInterfaceType();

    // The Self type can be nested in a few layers of metatypes (etc.).
    while (auto metatypeType = dyn_cast<MetatypeType>(selfType)) {
      auto next = metatypeType.getInstanceType();
      if (next == selfType)
        break;
      selfType = next;
    }

    auto substConformance =
        conformance.subst(selfType, Subst, Conformances);

    // Substitute the underlying conformance of opaque type archetypes if we
    // should look through opaque archetypes.
    if (typeExpansionContext.shouldLookThroughOpaqueTypeArchetypes()) {
      SubstOptions substOptions(None);
      auto substType = selfType.subst(Subst, Conformances, substOptions)
                           ->getCanonicalType();
      if (substType->hasOpaqueArchetype()) {
        substConformance = substOpaqueTypesWithUnderlyingTypes(
            substConformance, substType, typeExpansionContext);
      }
    }

    return substConformance;
  }

  SILType subst(SILType type) {
    return SILType::getPrimitiveType(visit(type.getASTType()),
                                     type.getCategory());
  }

  SILResultInfo substInterface(SILResultInfo orig) {
    return SILResultInfo(visit(orig.getInterfaceType()), orig.getConvention());
  }

  SILYieldInfo substInterface(SILYieldInfo orig) {
    return SILYieldInfo(visit(orig.getInterfaceType()), orig.getConvention());
  }

  SILParameterInfo substInterface(SILParameterInfo orig) {
    return SILParameterInfo(visit(orig.getInterfaceType()),
                            orig.getConvention(), orig.getDifferentiability());
  }

  /// Tuples need to have their component types substituted by these
  /// same rules.
  CanType visitTupleType(CanTupleType origType) {
    // Fast-path the empty tuple.
    if (origType->getNumElements() == 0) return origType;

    SmallVector<TupleTypeElt, 8> substElts;
    substElts.reserve(origType->getNumElements());
    for (auto &origElt : origType->getElements()) {
      auto substEltType = visit(CanType(origElt.getType()));
      substElts.push_back(origElt.getWithType(substEltType));
    }
    return CanType(TupleType::get(substElts, TC.Context));
  }
  // Block storage types need to substitute their capture type by these same
  // rules.
  CanType visitSILBlockStorageType(CanSILBlockStorageType origType) {
    auto substCaptureType = visit(origType->getCaptureType());
    return SILBlockStorageType::get(substCaptureType);
  }

  /// Optionals need to have their object types substituted by these rules.
  CanType visitBoundGenericEnumType(CanBoundGenericEnumType origType) {
    // Only use a special rule if it's Optional.
    if (!origType->getDecl()->isOptionalDecl()) {
      return visitType(origType);
    }

    CanType origObjectType = origType.getGenericArgs()[0];
    CanType substObjectType = visit(origObjectType);
    return CanType(BoundGenericType::get(origType->getDecl(), Type(),
                                         substObjectType));
  }

  /// Any other type would be a valid type in the AST. Just apply the
  /// substitution on the AST level and then lower that.
  CanType visitType(CanType origType) {
    assert(!isa<AnyFunctionType>(origType));
    assert(!isa<LValueType>(origType) && !isa<InOutType>(origType));

    SubstOptions substOptions(None);
    if (shouldSubstituteOpaqueArchetypes)
      substOptions = SubstFlags::SubstituteOpaqueArchetypes |
                     SubstFlags::AllowLoweredTypes;
    auto substType =
        origType.subst(Subst, Conformances, substOptions)->getCanonicalType();

    // If the substitution didn't change anything, we know that the
    // original type was a lowered type, so we're good.
    if (origType == substType) {
      return origType;
    }

    AbstractionPattern abstraction(Sig, origType);
    return TC.getLoweredRValueType(typeExpansionContext, abstraction,
                                   substType);
  }
};

} // end anonymous namespace

SILType SILType::subst(TypeConverter &tc, TypeSubstitutionFn subs,
                       LookupConformanceFn conformances,
                       CanGenericSignature genericSig,
                       bool shouldSubstituteOpaqueArchetypes) const {
  if (!hasArchetype() && !hasTypeParameter() &&
      (!shouldSubstituteOpaqueArchetypes ||
       !getASTType()->hasOpaqueArchetype()))
    return *this;

  SILTypeSubstituter STST(tc, TypeExpansionContext::minimal(), subs,
                          conformances, genericSig,
                          shouldSubstituteOpaqueArchetypes);
  return STST.subst(*this);
}

SILType SILType::subst(SILModule &M, TypeSubstitutionFn subs,
                       LookupConformanceFn conformances,
                       CanGenericSignature genericSig,
                       bool shouldSubstituteOpaqueArchetypes) const {
  return subst(M.Types, subs, conformances, genericSig,
               shouldSubstituteOpaqueArchetypes);
}

SILType SILType::subst(TypeConverter &tc, SubstitutionMap subs) const {
  auto sig = subs.getGenericSignature();
  return subst(tc, QuerySubstitutionMap{subs},
               LookUpConformanceInSubstitutionMap(subs),
               sig.getCanonicalSignature());
}
SILType SILType::subst(SILModule &M, SubstitutionMap subs) const{
  return subst(M.Types, subs);
}

SILType SILType::subst(SILModule &M, SubstitutionMap subs,
                       TypeExpansionContext context) const {
  if (!hasArchetype() && !hasTypeParameter() &&
      !getASTType()->hasOpaqueArchetype())
    return *this;

  // Pass the TypeSubstitutionFn and LookupConformanceFn as arguments so that
  // the llvm::function_ref value's scope spans the STST.subst call since
  // SILTypeSubstituter captures these functions.
  auto result = [&](TypeSubstitutionFn subsFn,
                    LookupConformanceFn conformancesFn) -> SILType {
    SILTypeSubstituter STST(M.Types, context, subsFn, conformancesFn,
                            subs.getGenericSignature().getCanonicalSignature(),
                            false);
    return STST.subst(*this);
  }(QuerySubstitutionMap{subs}, LookUpConformanceInSubstitutionMap(subs));
  return result;
}

/// Apply a substitution to this polymorphic SILFunctionType so that
/// it has the form of the normal SILFunctionType for the substituted
/// type, except using the original conventions.
CanSILFunctionType
SILFunctionType::substGenericArgs(SILModule &silModule, SubstitutionMap subs,
                                  TypeExpansionContext context) {
  if (!isPolymorphic()) {
    return CanSILFunctionType(this);
  }
  
  if (subs.empty()) {
    return CanSILFunctionType(this);
  }

  return substGenericArgs(silModule,
                          QuerySubstitutionMap{subs},
                          LookUpConformanceInSubstitutionMap(subs),
                          context);
}

CanSILFunctionType
SILFunctionType::substGenericArgs(SILModule &silModule,
                                  TypeSubstitutionFn subs,
                                  LookupConformanceFn conformances,
                                  TypeExpansionContext context) {
  if (!isPolymorphic()) return CanSILFunctionType(this);
  SILTypeSubstituter substituter(silModule.Types, context, subs, conformances,
                                 getSubstGenericSignature(),
                                 /*shouldSubstituteOpaqueTypes*/ false);
  return substituter.substSILFunctionType(CanSILFunctionType(this), true);
}

CanSILFunctionType
SILFunctionType::substituteOpaqueArchetypes(TypeConverter &TC,
                                            TypeExpansionContext context) {
  if (!hasOpaqueArchetype() ||
      !context.shouldLookThroughOpaqueTypeArchetypes())
    return CanSILFunctionType(this);

  ReplaceOpaqueTypesWithUnderlyingTypes replacer(
      context.getContext(), context.getResilienceExpansion(),
      context.isWholeModuleContext());

  SILTypeSubstituter substituter(TC, context, replacer, replacer,
                                 getSubstGenericSignature(),
                                 /*shouldSubstituteOpaqueTypes*/ true);
  auto resTy =
    substituter.substSILFunctionType(CanSILFunctionType(this), false);

  return resTy;
}

/// Fast path for bridging types in a function type without uncurrying.
CanAnyFunctionType TypeConverter::getBridgedFunctionType(
    AbstractionPattern pattern, CanAnyFunctionType t, Bridgeability bridging,
    SILFunctionTypeRepresentation rep) {
  // Pull out the generic signature.
  CanGenericSignature genericSig = t.getOptGenericSignature();

  switch (getSILFunctionLanguage(rep)) {
  case SILFunctionLanguage::Swift: {
    // No bridging needed for native functions.
    return t;
  }
  case SILFunctionLanguage::C: {
    SmallVector<AnyFunctionType::Param, 8> params;
    getBridgedParams(rep, pattern, t->getParams(), params, bridging);

    bool suppressOptional = pattern.hasForeignErrorStrippingResultOptionality();
    auto result = getBridgedResultType(rep,
                                       pattern.getFunctionResultType(),
                                       t.getResult(),
                                       bridging,
                                       suppressOptional);

    return CanAnyFunctionType::get(genericSig, llvm::makeArrayRef(params),
                                   result, t->getExtInfo());
  }
  }
  llvm_unreachable("bad calling convention");
}

static AbstractFunctionDecl *getBridgedFunction(SILDeclRef declRef) {
  switch (declRef.kind) {
  case SILDeclRef::Kind::Func:
  case SILDeclRef::Kind::Allocator:
  case SILDeclRef::Kind::Initializer:
    return (declRef.hasDecl()
            ? cast<AbstractFunctionDecl>(declRef.getDecl())
            : nullptr);

  case SILDeclRef::Kind::EnumElement:
  case SILDeclRef::Kind::Destroyer:
  case SILDeclRef::Kind::Deallocator:
  case SILDeclRef::Kind::GlobalAccessor:
  case SILDeclRef::Kind::DefaultArgGenerator:
  case SILDeclRef::Kind::StoredPropertyInitializer:
  case SILDeclRef::Kind::PropertyWrapperBackingInitializer:
  case SILDeclRef::Kind::IVarInitializer:
  case SILDeclRef::Kind::IVarDestroyer:
    return nullptr;
  }
  llvm_unreachable("bad SILDeclRef kind");
}

static AbstractionPattern
getAbstractionPatternForConstant(ASTContext &ctx, SILDeclRef constant,
                                 CanAnyFunctionType fnType,
                                 unsigned numParameterLists) {
  if (!constant.isForeign)
    return AbstractionPattern(fnType);

  auto bridgedFn = getBridgedFunction(constant);
  if (!bridgedFn)
    return AbstractionPattern(fnType);
  const clang::Decl *clangDecl = bridgedFn->getClangDecl();
  if (!clangDecl)
    return AbstractionPattern(fnType);

  // Don't implicitly turn non-optional results to optional if
  // we're going to apply a foreign error convention that checks
  // for nil results.
  if (auto method = dyn_cast<clang::ObjCMethodDecl>(clangDecl)) {
    assert(numParameterLists == 2 && "getting curried ObjC method type?");
    return AbstractionPattern::getCurriedObjCMethod(fnType, method,
                                      bridgedFn->getForeignErrorConvention(),
                                      bridgedFn->getForeignAsyncConvention());
  } else if (auto value = dyn_cast<clang::ValueDecl>(clangDecl)) {
    if (numParameterLists == 1) {
      // C function imported as a function.
      return AbstractionPattern(fnType, value->getType().getTypePtr());
    } else {
      assert(numParameterLists == 2);
      if (auto method = dyn_cast<clang::CXXMethodDecl>(clangDecl)) {
        // C++ method.
        return method->isOverloadedOperator()
                   ? AbstractionPattern::getCurriedCXXOperatorMethod(fnType,
                                                                     bridgedFn)
                   : AbstractionPattern::getCurriedCXXMethod(fnType, bridgedFn);
      } else {
        // C function imported as a method.
        return AbstractionPattern::getCurriedCFunctionAsMethod(fnType,
                                                               bridgedFn);
      }
    }
  }

  return AbstractionPattern(fnType);
}

TypeConverter::LoweredFormalTypes
TypeConverter::getLoweredFormalTypes(SILDeclRef constant,
                                     CanAnyFunctionType fnType) {
  // We always use full bridging when importing a constant because we can
  // directly bridge its arguments and results when calling it.
  auto bridging = Bridgeability::Full;

  unsigned numParameterLists = constant.getParameterListCount();

  // Form an abstraction pattern for bridging purposes.
  AbstractionPattern bridgingFnPattern =
    getAbstractionPatternForConstant(Context, constant, fnType,
                                     numParameterLists);

  auto extInfo = fnType->getExtInfo();
  SILFunctionTypeRepresentation rep = getDeclRefRepresentation(constant);
  assert(rep != SILFunctionType::Representation::Block &&
         "objc blocks cannot be curried");

  // Fast path: no uncurrying required.
  if (numParameterLists == 1) {
    auto bridgedFnType =
        getBridgedFunctionType(bridgingFnPattern, fnType, bridging, rep);
    bridgingFnPattern.rewriteType(bridgingFnPattern.getGenericSignature(),
                                  bridgedFnType);
    return { bridgingFnPattern, bridgedFnType };
  }

  // The dependent generic signature.
  CanGenericSignature genericSig = fnType.getOptGenericSignature();

  // The 'self' parameter.
  assert(fnType.getParams().size() == 1);
  AnyFunctionType::Param selfParam = fnType.getParams()[0];

  // The formal method parameters.
  // If we actually partially-apply this, assume we'll need a thick function.
  fnType = cast<FunctionType>(fnType.getResult());
  auto innerExtInfo =
    fnType->getExtInfo().withRepresentation(FunctionTypeRepresentation::Swift);
  auto methodParams = fnType->getParams();

  auto resultType = fnType.getResult();
  bool suppressOptionalResult =
    bridgingFnPattern.hasForeignErrorStrippingResultOptionality();

  // Bridge input and result types.
  SmallVector<AnyFunctionType::Param, 8> bridgedParams;
  CanType bridgedResultType;

  switch (rep) {
  case SILFunctionTypeRepresentation::Thin:
  case SILFunctionTypeRepresentation::Thick:
  case SILFunctionTypeRepresentation::Method:
  case SILFunctionTypeRepresentation::Closure:
  case SILFunctionTypeRepresentation::WitnessMethod:
    // Native functions don't need bridging.
    bridgedParams.append(methodParams.begin(), methodParams.end());
    bridgedResultType = resultType;
    break;

  case SILFunctionTypeRepresentation::CXXMethod:
  case SILFunctionTypeRepresentation::ObjCMethod:
  case SILFunctionTypeRepresentation::CFunctionPointer: {
    if (rep == SILFunctionTypeRepresentation::ObjCMethod) {
      // The "self" parameter should not get bridged unless it's a metatype.
      if (selfParam.getPlainType()->is<AnyMetatypeType>()) {
        auto selfPattern = bridgingFnPattern.getFunctionParamType(0);
        selfParam = getBridgedParam(rep, selfPattern, selfParam, bridging);
      }
    }

    auto partialFnPattern = bridgingFnPattern.getFunctionResultType();
    for (unsigned i : indices(methodParams)) {
      // C++ operators that are implemented as non-static member functions get
      // imported into Swift as static methods that have an additional
      // parameter for the left-hand-side operand instead of the receiver
      // object. These are inout parameters and don't get bridged.
      // TODO: Undo this if we stop using inout.
      if (auto method = dyn_cast_or_null<clang::CXXMethodDecl>(
              constant.getDecl()->getClangDecl())) {
        if (i==0 && method->isOverloadedOperator()) {
          bridgedParams.push_back(methodParams[0]);
          continue;
        }
      }

      auto paramPattern = partialFnPattern.getFunctionParamType(i);
      auto bridgedParam =
          getBridgedParam(rep, paramPattern, methodParams[i], bridging);
      bridgedParams.push_back(bridgedParam);
    }

    bridgedResultType =
      getBridgedResultType(rep,
                           partialFnPattern.getFunctionResultType(),
                           resultType, bridging, suppressOptionalResult);
    break;
  }

  case SILFunctionTypeRepresentation::Block:
    llvm_unreachable("Cannot uncurry native representation");
  }

  // Build the curried function type.
  auto inner =
    CanFunctionType::get(llvm::makeArrayRef(bridgedParams),
                         bridgedResultType, innerExtInfo);

  auto curried =
    CanAnyFunctionType::get(genericSig, {selfParam}, inner, extInfo);

  // Replace the type in the abstraction pattern with the curried type.
  bridgingFnPattern.rewriteType(genericSig, curried);

  // Build the uncurried function type.
  if (innerExtInfo.isThrowing())
    extInfo = extInfo.withThrows(true);
  if (innerExtInfo.isAsync())
    extInfo = extInfo.withAsync(true);

  // If this is a C++ constructor, don't add the metatype "self" parameter
  // because we'll never use it and it will cause problems in IRGen.
  if (constant.getDecl()->getClangDecl() &&
      isa<clang::CXXConstructorDecl>(constant.getDecl()->getClangDecl())) {
    // But, make sure it is actually a metatype that we're not adding. If
    // changes to the self parameter are made in the future, this logic may
    // need to be updated.
    assert(selfParam.getParameterType()->is<MetatypeType>());
  } else {
    bridgedParams.push_back(selfParam);
  }

  auto uncurried =
    CanAnyFunctionType::get(genericSig,
                            llvm::makeArrayRef(bridgedParams),
                            bridgedResultType,
                            extInfo);

  return { bridgingFnPattern, uncurried };
}

// TODO: We should compare generic signatures. Class and witness methods
// allow variance in "self"-fulfilled parameters; other functions must
// match exactly.
// TODO: More sophisticated param and return ABI compatibility rules could
// diverge.
static bool areABICompatibleParamsOrReturns(SILType a, SILType b,
                                            SILFunction *inFunction) {
  // Address parameters are all ABI-compatible, though the referenced
  // values may not be. Assume whoever's doing this knows what they're
  // doing.
  if (a.isAddress() && b.isAddress())
    return true;

  // Addresses aren't compatible with values.
  // TODO: An exception for pointerish types?
  if (a.isAddress() || b.isAddress())
    return false;

  // Tuples are ABI compatible if their elements are.
  // TODO: Should destructure recursively.
  SmallVector<CanType, 1> aElements, bElements;
  if (auto tup = a.getAs<TupleType>()) {
    auto types = tup.getElementTypes();
    aElements.append(types.begin(), types.end());
  } else {
    aElements.push_back(a.getASTType());
  }
  if (auto tup = b.getAs<TupleType>()) {
    auto types = tup.getElementTypes();
    bElements.append(types.begin(), types.end());
  } else {
    bElements.push_back(b.getASTType());
  }

  if (aElements.size() != bElements.size())
    return false;

  for (unsigned i : indices(aElements)) {
    auto aa = SILType::getPrimitiveObjectType(aElements[i]);
    auto bb = SILType::getPrimitiveObjectType(bElements[i]);
    // Equivalent types are always ABI-compatible.
    if (aa == bb)
      continue;

    // Opaque types are compatible with their substitution.
    if (inFunction) {
      auto opaqueTypesSubsituted = aa;
      auto *dc = inFunction->getDeclContext();
      auto *currentModule = inFunction->getModule().getSwiftModule();
      if (!dc || !dc->isChildContextOf(currentModule))
        dc = currentModule;
      ReplaceOpaqueTypesWithUnderlyingTypes replacer(
          dc, inFunction->getResilienceExpansion(),
          inFunction->getModule().isWholeModule());
      if (aa.getASTType()->hasOpaqueArchetype())
        opaqueTypesSubsituted = aa.subst(inFunction->getModule(), replacer,
                                         replacer, CanGenericSignature(), true);

      auto opaqueTypesSubsituted2 = bb;
      if (bb.getASTType()->hasOpaqueArchetype())
        opaqueTypesSubsituted2 =
            bb.subst(inFunction->getModule(), replacer, replacer,
                     CanGenericSignature(), true);
      if (opaqueTypesSubsituted == opaqueTypesSubsituted2)
        continue;
    }

    // FIXME: If one or both types are dependent, we can't accurately assess
    // whether they're ABI-compatible without a generic context. We can
    // do a better job here when dependent types are related to their
    // generic signatures.
    if (aa.hasTypeParameter() || bb.hasTypeParameter())
      continue;

    // Bridgeable object types are interchangeable.
    if (aa.isBridgeableObjectType() && bb.isBridgeableObjectType())
      continue;

    // Optional and IUO are interchangeable if their elements are.
    auto aObject = aa.getOptionalObjectType();
    auto bObject = bb.getOptionalObjectType();
    if (aObject && bObject &&
        areABICompatibleParamsOrReturns(aObject, bObject, inFunction))
      continue;
    // Optional objects are ABI-interchangeable with non-optionals;
    // None is represented by a null pointer.
    if (aObject && aObject.isBridgeableObjectType() &&
        bb.isBridgeableObjectType())
      continue;
    if (bObject && bObject.isBridgeableObjectType() &&
        aa.isBridgeableObjectType())
      continue;

    // Optional thick metatypes are ABI-interchangeable with non-optionals
    // too.
    if (aObject)
      if (auto aObjMeta = aObject.getAs<MetatypeType>())
        if (auto bMeta = bb.getAs<MetatypeType>())
          if (aObjMeta->getRepresentation() == bMeta->getRepresentation() &&
              bMeta->getRepresentation() != MetatypeRepresentation::Thin)
            continue;
    if (bObject)
      if (auto aMeta = aa.getAs<MetatypeType>())
        if (auto bObjMeta = bObject.getAs<MetatypeType>())
          if (aMeta->getRepresentation() == bObjMeta->getRepresentation() &&
              aMeta->getRepresentation() != MetatypeRepresentation::Thin)
            continue;

    // Function types are interchangeable if they're also ABI-compatible.
    if (auto aFunc = aa.getAs<SILFunctionType>()) {
      if (auto bFunc = bb.getAs<SILFunctionType>()) {
        // *NOTE* We swallow the specific error here for now. We will still get
        // that the function types are incompatible though, just not more
        // specific information.
        return aFunc->isABICompatibleWith(bFunc, *inFunction).isCompatible();
      }
    }

    // Metatypes are interchangeable with metatypes with the same
    // representation.
    if (auto aMeta = aa.getAs<MetatypeType>()) {
      if (auto bMeta = bb.getAs<MetatypeType>()) {
        if (aMeta->getRepresentation() == bMeta->getRepresentation())
          continue;
      }
    }
    // Other types must match exactly.
    return false;
  }

  return true;
}

namespace {
using ABICompatibilityCheckResult =
    SILFunctionType::ABICompatibilityCheckResult;
} // end anonymous namespace

ABICompatibilityCheckResult
SILFunctionType::isABICompatibleWith(CanSILFunctionType other,
                                     SILFunction &context) const {
  // The calling convention and function representation can't be changed.
  if (getRepresentation() != other->getRepresentation())
    return ABICompatibilityCheckResult::DifferentFunctionRepresentations;

  // Check the results.
  if (getNumResults() != other->getNumResults())
    return ABICompatibilityCheckResult::DifferentNumberOfResults;

  for (unsigned i : indices(getResults())) {
    auto result1 = getResults()[i];
    auto result2 = other->getResults()[i];

    if (result1.getConvention() != result2.getConvention())
      return ABICompatibilityCheckResult::DifferentReturnValueConventions;

    if (!areABICompatibleParamsOrReturns(
            result1.getSILStorageType(context.getModule(), this,
                                      context.getTypeExpansionContext()),
            result2.getSILStorageType(context.getModule(), other,
                                      context.getTypeExpansionContext()),
            &context)) {
      return ABICompatibilityCheckResult::ABIIncompatibleReturnValues;
    }
  }

  // Our error result conventions are designed to be ABI compatible
  // with functions lacking error results.  Just make sure that the
  // actual conventions match up.
  if (hasErrorResult() && other->hasErrorResult()) {
    auto error1 = getErrorResult();
    auto error2 = other->getErrorResult();
    if (error1.getConvention() != error2.getConvention())
      return ABICompatibilityCheckResult::DifferentErrorResultConventions;

    if (!areABICompatibleParamsOrReturns(
            error1.getSILStorageType(context.getModule(), this,
                                     context.getTypeExpansionContext()),
            error2.getSILStorageType(context.getModule(), other,
                                     context.getTypeExpansionContext()),
            &context))
      return ABICompatibilityCheckResult::ABIIncompatibleErrorResults;
  }

  // Check the parameters.
  // TODO: Could allow known-empty types to be inserted or removed, but SIL
  // doesn't know what empty types are yet.
  if (getParameters().size() != other->getParameters().size())
    return ABICompatibilityCheckResult::DifferentNumberOfParameters;

  for (unsigned i : indices(getParameters())) {
    auto param1 = getParameters()[i];
    auto param2 = other->getParameters()[i];

    if (param1.getConvention() != param2.getConvention())
      return {ABICompatibilityCheckResult::DifferingParameterConvention, i};
    if (!areABICompatibleParamsOrReturns(
            param1.getSILStorageType(context.getModule(), this,
                                     context.getTypeExpansionContext()),
            param2.getSILStorageType(context.getModule(), other,
                                     context.getTypeExpansionContext()),
            &context))
      return {ABICompatibilityCheckResult::ABIIncompatibleParameterType, i};
  }

  // This needs to be checked last because the result implies everying else has
  // already been checked and this is the only difference.
  if (isNoEscape() != other->isNoEscape() &&
      (getRepresentation() == SILFunctionType::Representation::Thick))
    return ABICompatibilityCheckResult::ABIEscapeToNoEscapeConversion;

  return ABICompatibilityCheckResult::None;
}

StringRef SILFunctionType::ABICompatibilityCheckResult::getMessage() const {
  switch (kind) {
  case innerty::None:
    return "None";
  case innerty::DifferentFunctionRepresentations:
    return "Different function representations";
  case innerty::DifferentNumberOfResults:
    return "Different number of results";
  case innerty::DifferentReturnValueConventions:
    return "Different return value conventions";
  case innerty::ABIIncompatibleReturnValues:
    return "ABI incompatible return values";
  case innerty::DifferentErrorResultConventions:
    return "Different error result conventions";
  case innerty::ABIIncompatibleErrorResults:
    return "ABI incompatible error results";
  case innerty::DifferentNumberOfParameters:
    return "Different number of parameters";

  // These two have to do with specific parameters, so keep the error message
  // non-plural.
  case innerty::DifferingParameterConvention:
    return "Differing parameter convention";
  case innerty::ABIIncompatibleParameterType:
    return "ABI incompatible parameter type.";
  case innerty::ABIEscapeToNoEscapeConversion:
    return "Escape to no escape conversion";
  }
  llvm_unreachable("Covered switch isn't completely covered?!");
}

TypeExpansionContext::TypeExpansionContext(const SILFunction &f)
    : expansion(f.getResilienceExpansion()),
      inContext(f.getModule().getAssociatedContext()),
      isContextWholeModule(f.getModule().isWholeModule()) {}

CanSILFunctionType SILFunction::getLoweredFunctionTypeInContext(
    TypeExpansionContext context) const {
  auto origFunTy = getLoweredFunctionType();
  auto &M = getModule();
  auto funTy = M.Types.getLoweredType(origFunTy , context);
  return cast<SILFunctionType>(funTy.getASTType());
}
