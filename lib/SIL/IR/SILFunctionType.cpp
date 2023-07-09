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
#include "swift/AST/Module.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/AbstractionPatternGenerators.h"
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
  llvm::Optional<SILResultInfo> errorResult;

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
    if (resultIndices.empty() ||
        inoutParamAndIndex.value().getDifferentiability() !=
        SILParameterDifferentiability::NotDifferentiable)
      resultIndices.push_back(getNumResults() + inoutParamAndIndex.index());

  auto numSemanticResults =
      getNumResults() + getNumIndirectMutatingParameters();
  return IndexSubset::get(getASTContext(), numSemanticResults, resultIndices);
}

CanSILFunctionType SILFunctionType::getDifferentiableComponentType(
    NormalDifferentiableFunctionTypeComponent component, SILModule &module) {
  assert(getDifferentiabilityKind() == DifferentiabilityKind::Reverse &&
         "Must be a `@differentiable(reverse)` function");
  auto originalFnTy = getWithoutDifferentiability();
  if (auto derivativeKind = component.getAsDerivativeFunctionKind()) {
    return originalFnTy->getAutoDiffDerivativeFunctionType(
        getDifferentiabilityParameterIndices(),
        getDifferentiabilityResultIndices(), *derivativeKind, module.Types,
        LookUpConformanceInModule(module.getSwiftModule()));
  }
  return originalFnTy;
}

CanSILFunctionType SILFunctionType::getLinearComponentType(
    LinearDifferentiableFunctionTypeComponent component, SILModule &module) {
  assert(getDifferentiabilityKind() == DifferentiabilityKind::Linear &&
         "Must be a `@differentiable(linear)` function");
  auto originalFnTy = getWithoutDifferentiability();
  switch (component) {
  case LinearDifferentiableFunctionTypeComponent::Original:
    return originalFnTy;
  case LinearDifferentiableFunctionTypeComponent::Transpose:
    return originalFnTy->getAutoDiffTransposeFunctionType(
        getDifferentiabilityParameterIndices(), module.Types,
        LookUpConformanceInModule(module.getSwiftModule()));
  }
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
                   SmallVectorImpl<SILResultInfo> &originalResults) {
  // Collect original formal results.
  originalResults.append(functionType->getResults().begin(),
                         functionType->getResults().end());

  // Collect original `inout` parameters.
  for (auto i : range(functionType->getNumParameters())) {
    auto param = functionType->getParameters()[i];
    if (!param.isIndirectMutating())
      continue;
    if (param.getDifferentiability() != SILParameterDifferentiability::NotDifferentiable)
      originalResults.emplace_back(param.getInterfaceType(), ResultConvention::Indirect);
  }
}

static CanGenericSignature buildDifferentiableGenericSignature(CanGenericSignature sig,
                                                               CanType tanType,
                                                               CanType origTypeOfAbstraction) {
  if (!sig)
    return sig;

  llvm::DenseSet<CanType> types;

  auto &ctx = tanType->getASTContext();

  (void) tanType.findIf([&](Type t) -> bool {
    if (auto *dmt = t->getAs<DependentMemberType>()) {
      if (dmt->getName() == ctx.Id_TangentVector)
        types.insert(dmt->getBase()->getCanonicalType());
    }

    return false;
  });

  SmallVector<Requirement, 2> reqs;
  auto *proto = ctx.getProtocol(KnownProtocolKind::Differentiable);
  assert(proto != nullptr);

  for (auto type : types) {
    if (!sig->requiresProtocol(type, proto)) {
      reqs.push_back(Requirement(RequirementKind::Conformance, type,
                                 proto->getDeclaredInterfaceType()));
    }
  }

  if (origTypeOfAbstraction) {
    (void) origTypeOfAbstraction.findIf([&](Type t) -> bool {
      if (auto *at = t->getAs<ArchetypeType>()) {
        auto interfaceTy = at->getInterfaceType();
        auto genericParams = sig.getGenericParams();

        // The GSB used to drop requirements which reference non-existent
        // generic parameters, whereas the RequirementMachine asserts now.
        // Filter these requirements out explicitly to preserve the old
        // behavior.
        if (std::find_if(genericParams.begin(), genericParams.end(),
                         [interfaceTy](CanGenericTypeParamType t) -> bool {
                           return t->isEqual(interfaceTy->getRootGenericParam());
                         }) != genericParams.end()) {
          types.insert(interfaceTy->getCanonicalType());

          for (auto *proto : at->getConformsTo()) {
            reqs.push_back(Requirement(RequirementKind::Conformance,
                                       interfaceTy,
                                       proto->getDeclaredInterfaceType()));
          }

          // The GSB would add conformance requirements if a nested type
          // requirement involving a resolved DependentMemberType was added;
          // eg, if you start with <T> and add T.[P]A == Int, it would also
          // add the conformance requirement T : P.
          //
          // This was not an intended behavior on the part of the GSB, and the
          // logic here is a complete mess, so just simulate the old behavior
          // here.
          auto parentTy = interfaceTy;
          while (parentTy) {
            if (auto memberTy = parentTy->getAs<DependentMemberType>()) {
              parentTy = memberTy->getBase();
              if (auto *assocTy = memberTy->getAssocType()) {
                reqs.push_back(Requirement(RequirementKind::Conformance,
                                           parentTy,
                                           assocTy->getProtocol()->getDeclaredInterfaceType()));
              }
            } else
              parentTy = Type();
          }
        }
      }
      return false;
    });
  }

  return buildGenericSignature(ctx, sig, {}, reqs).getCanonicalSignature();
}

/// Given an original type, computes its tangent type for the purpose of
/// building a linear map using this type.  When the original type is an
/// archetype or contains a type parameter, appends a new generic parameter and
/// a corresponding replacement type to the given containers.
static CanType getAutoDiffTangentTypeForLinearMap(
  Type originalType,
  LookupConformanceFn lookupConformance,
  SmallVectorImpl<GenericTypeParamType *> &substGenericParams,
  SmallVectorImpl<Type> &substReplacements,
  ASTContext &context
) {
  auto maybeTanType = originalType->getAutoDiffTangentSpace(lookupConformance);
  assert(maybeTanType && "Type does not have a tangent space?");
  auto tanType = maybeTanType->getCanonicalType();
  // If concrete, the tangent type is concrete.
  if (!tanType->hasArchetype() && !tanType->hasTypeParameter())
    return tanType;
  // Otherwise, the tangent type is a new generic parameter substituted for the
  // tangent type.
  auto gpIndex = substGenericParams.size();
  auto gpType = CanGenericTypeParamType::get(/*isParameterPack*/ false,
                                             0, gpIndex, context);
  substGenericParams.push_back(gpType);
  substReplacements.push_back(tanType);
  return gpType;
}

/// Returns the differential type for the given original function type,
/// parameter indices, and result index.
static CanSILFunctionType getAutoDiffDifferentialType(
    SILFunctionType *originalFnTy, IndexSubset *parameterIndices,
    IndexSubset *resultIndices, LookupConformanceFn lookupConformance,
    CanType origTypeOfAbstraction,
    TypeConverter &TC) {
  // Given the tangent type and the corresponding original parameter's
  // convention, returns the tangent parameter's convention.
  auto getTangentParameterConvention =
      [&](CanType tanType,
          ParameterConvention origParamConv) -> ParameterConvention {
    auto sig = buildDifferentiableGenericSignature(
      originalFnTy->getSubstGenericSignature(), tanType, origTypeOfAbstraction);

    tanType = tanType->getReducedType(sig);
    AbstractionPattern pattern(sig, tanType);
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
    auto sig = buildDifferentiableGenericSignature(
      originalFnTy->getSubstGenericSignature(), tanType, origTypeOfAbstraction);

    tanType = tanType->getReducedType(sig);
    AbstractionPattern pattern(sig, tanType);
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

  SmallVector<SILResultInfo, 2> originalResults;
  getSemanticResults(originalFnTy, parameterIndices, originalResults);

  SmallVector<SILParameterInfo, 4> diffParams;
  getDifferentiabilityParameters(originalFnTy, parameterIndices, diffParams);
  SmallVector<SILParameterInfo, 8> differentialParams;
  for (auto &param : diffParams) {
    auto paramTanType = getAutoDiffTangentTypeForLinearMap(
        param.getInterfaceType(), lookupConformance,
        substGenericParams, substReplacements, ctx);
    auto paramConv = getTangentParameterConvention(
        // FIXME(rdar://82549134): Use `resultTanType` to compute it instead.
        param.getInterfaceType()
            ->getAutoDiffTangentSpace(lookupConformance)
            ->getCanonicalType(),
        param.getConvention());
    differentialParams.push_back({paramTanType, paramConv});
  }
  SmallVector<SILResultInfo, 1> differentialResults;
  for (auto resultIndex : resultIndices->getIndices()) {
    // Handle formal original result.
    if (resultIndex < originalFnTy->getNumResults()) {
      auto &result = originalResults[resultIndex];
      auto resultTanType = getAutoDiffTangentTypeForLinearMap(
          result.getInterfaceType(), lookupConformance,
          substGenericParams, substReplacements, ctx);
      auto resultConv = getTangentResultConvention(
          // FIXME(rdar://82549134): Use `resultTanType` to compute it instead.
          result.getInterfaceType()
              ->getAutoDiffTangentSpace(lookupConformance)
              ->getCanonicalType(),
          result.getConvention());
      differentialResults.push_back({resultTanType, resultConv});
      continue;
    }
    // Handle original `inout` parameters.
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
    auto inoutParamTanType = getAutoDiffTangentTypeForLinearMap(
        inoutParam.getInterfaceType(), lookupConformance,
        substGenericParams, substReplacements, ctx);
    differentialResults.push_back(
        {inoutParamTanType, ResultConvention::Indirect});
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
      differentialResults, llvm::None, substitutions,
      /*invocationSubstitutions*/ SubstitutionMap(), ctx);
}

/// Returns the pullback type for the given original function type, parameter
/// indices, and result index.
static CanSILFunctionType getAutoDiffPullbackType(
    SILFunctionType *originalFnTy, IndexSubset *parameterIndices,
    IndexSubset *resultIndices, LookupConformanceFn lookupConformance,
    CanType origTypeOfAbstraction, TypeConverter &TC) {
  auto &ctx = originalFnTy->getASTContext();
  SmallVector<GenericTypeParamType *, 4> substGenericParams;
  SmallVector<Requirement, 4> substRequirements;
  SmallVector<Type, 4> substReplacements;
  SmallVector<ProtocolConformanceRef, 4> substConformances;

  SmallVector<SILResultInfo, 2> originalResults;
  getSemanticResults(originalFnTy, parameterIndices, originalResults);

  // Given a type, returns its formal SIL parameter info.
  auto getTangentParameterConventionForOriginalResult =
      [&](CanType tanType,
          ResultConvention origResConv) -> ParameterConvention {
    auto sig = buildDifferentiableGenericSignature(
      originalFnTy->getSubstGenericSignature(), tanType, origTypeOfAbstraction);

    tanType = tanType->getReducedType(sig);
    AbstractionPattern pattern(sig, tanType);
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
    case ResultConvention::Pack:
      conv = ParameterConvention::Pack_Guaranteed;
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
    auto sig = buildDifferentiableGenericSignature(
      originalFnTy->getSubstGenericSignature(), tanType, origTypeOfAbstraction);

    tanType = tanType->getReducedType(sig);
    AbstractionPattern pattern(sig, tanType);
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
    case ParameterConvention::Pack_Owned:
    case ParameterConvention::Pack_Guaranteed:
    case ParameterConvention::Pack_Inout:
      conv = ResultConvention::Pack;
      break;
    case ParameterConvention::Indirect_In:
    case ParameterConvention::Indirect_Inout:
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
      auto resultTanType = getAutoDiffTangentTypeForLinearMap(
          origRes.getInterfaceType(), lookupConformance,
          substGenericParams, substReplacements, ctx);
      auto paramConv = getTangentParameterConventionForOriginalResult(
          // FIXME(rdar://82549134): Use `resultTanType` to compute it instead.
          origRes.getInterfaceType()
              ->getAutoDiffTangentSpace(lookupConformance)
              ->getCanonicalType(),
          origRes.getConvention());
      pullbackParams.push_back({resultTanType, paramConv});
      continue;
    }
    // Handle `inout` parameters.
    auto inoutParamIndex = resultIndex - originalFnTy->getNumResults();
    auto inoutParamIt = std::next(
        originalFnTy->getIndirectMutatingParameters().begin(), inoutParamIndex);
    auto paramIndex =
        std::distance(originalFnTy->getParameters().begin(), &*inoutParamIt);
    auto inoutParam = originalFnTy->getParameters()[paramIndex];
    // The pullback parameter convention depends on whether the original `inout`
    // parameter is a differentiability parameter.
    // - If yes, the pullback parameter convention is `@inout`.
    // - If no, the pullback parameter convention is `@in_guaranteed`.
    auto inoutParamTanType = getAutoDiffTangentTypeForLinearMap(
        inoutParam.getInterfaceType(), lookupConformance,
        substGenericParams, substReplacements, ctx);
    bool isWrtInoutParameter = parameterIndices->contains(paramIndex);
    auto paramTanConvention = isWrtInoutParameter
        ? inoutParam.getConvention()
        : ParameterConvention::Indirect_In_Guaranteed;
    pullbackParams.push_back({inoutParamTanType, paramTanConvention});
  }

  // Collect pullback results.
  SmallVector<SILParameterInfo, 4> diffParams;
  getDifferentiabilityParameters(originalFnTy, parameterIndices, diffParams);
  SmallVector<SILResultInfo, 8> pullbackResults;
  for (auto &param : diffParams) {
    // Skip `inout` parameters, which semantically behave as original results
    // and always appear as pullback parameters.
    if (param.isIndirectMutating())
      continue;
    auto paramTanType = getAutoDiffTangentTypeForLinearMap(
        param.getInterfaceType(), lookupConformance,
        substGenericParams, substReplacements, ctx);
    auto resultTanConvention = getTangentResultConventionForOriginalParameter(
        // FIXME(rdar://82549134): Use `resultTanType` to compute it instead.
        param.getInterfaceType()
            ->getAutoDiffTangentSpace(lookupConformance)
            ->getCanonicalType(),
        param.getConvention());
    pullbackResults.push_back({paramTanType, resultTanConvention});
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
      pullbackResults, llvm::None, substitutions,
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
        param.getWithInterfaceType(param.getInterfaceType()->getReducedType(
            constrainedInvocationGenSig)));
  }

  SmallVector<SILResultInfo, 4> newResults;
  newResults.reserve(original->getNumResults());
  for (auto &result : original->getResults()) {
    newResults.push_back(
        result.getWithInterfaceType(result.getInterfaceType()->getReducedType(
            constrainedInvocationGenSig)));
  }
  return SILFunctionType::get(
      constrainedInvocationGenSig->areAllParamsConcrete()
          ? GenericSignature()
          : constrainedInvocationGenSig,
      original->getExtInfo(), original->getCoroutineKind(),
      original->getCalleeConvention(), newParameters, original->getYields(),
      newResults, original->getOptionalErrorResult(),
      original->getPatternSubstitutions(),
      /*invocationSubstitutions*/ SubstitutionMap(), original->getASTContext(),
      original->getWitnessMethodConformanceOrInvalid());
}

CanSILFunctionType SILFunctionType::getAutoDiffDerivativeFunctionType(
    IndexSubset *parameterIndices, IndexSubset *resultIndices,
    AutoDiffDerivativeFunctionKind kind, TypeConverter &TC,
    LookupConformanceFn lookupConformance,
    CanGenericSignature derivativeFnInvocationGenSig,
    bool isReabstractionThunk,
    CanType origTypeOfAbstraction) {
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
                                    resultIndices, lookupConformance,
                                    origTypeOfAbstraction, TC);
    break;
  case AutoDiffDerivativeFunctionKind::VJP:
    closureType =
        getAutoDiffPullbackType(constrainedOriginalFnTy, parameterIndices,
                                resultIndices, lookupConformance,
                                origTypeOfAbstraction, TC);
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
        DifferentiabilityKind::Reverse, parameterIndices, resultIndices);
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
      constrainedOriginalFnTy->getInvocationGenericSignature(), extInfo,
      constrainedOriginalFnTy->getCoroutineKind(),
      constrainedOriginalFnTy->getCalleeConvention(), newParameters,
      constrainedOriginalFnTy->getYields(), newResults,
      constrainedOriginalFnTy->getOptionalErrorResult(),
      constrainedOriginalFnTy->getPatternSubstitutions(),
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
    case ResultConvention::Pack:
      newConv = ParameterConvention::Pack_Guaranteed;
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
    case ParameterConvention::Pack_Owned:
    case ParameterConvention::Pack_Guaranteed:
    case ParameterConvention::Pack_Inout:
      newConv = ResultConvention::Pack;
      break;
    case ParameterConvention::Indirect_In:
    case ParameterConvention::Indirect_Inout:
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

static CanType getKnownType(llvm::Optional<CanType> &cacheSlot, ASTContext &C,
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
                           SourceLoc(), NL_QualifiedDefault, decls);
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
  virtual ParameterConvention getPackParameter(unsigned index) const = 0;

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

  ParameterConvention getPack(ValueOwnership ownership,
                              unsigned index) const {
    switch (ownership) {
    case ValueOwnership::Default:
      return getPackParameter(index);
    case ValueOwnership::InOut:
      return ParameterConvention::Pack_Inout;
    case ValueOwnership::Shared:
      return ParameterConvention::Pack_Guaranteed;
    case ValueOwnership::Owned:
      return ParameterConvention::Pack_Owned;
    }
    llvm_unreachable("unhandled ownership");
  }

  ParameterConvention getDirect(ValueOwnership ownership, bool forSelf,
                                unsigned index, const AbstractionPattern &type,
                                const TypeLowering &substTL) const {
    switch (ownership) {
    case ValueOwnership::Default: {
      if (forSelf)
        return getDirectSelfParameter(type);
      auto convention = getDirectParameter(index, type, substTL);
      // Nonescaping closures can only be borrowed across calls currently.
      if (convention == ParameterConvention::Direct_Owned) {
        if (auto fnTy = substTL.getLoweredType().getAs<SILFunctionType>()) {
          if (fnTy->isTrivialNoEscape()) {
            return ParameterConvention::Direct_Guaranteed;
          }
        }
      }
      return convention;
    }
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

/// A visitor for breaking down formal result types into a SILResultInfo
/// and possibly some number of indirect-out SILParameterInfos,
/// matching the abstraction patterns of the original type.
class DestructureResults {
  TypeConverter &TC;
  const Conventions &Convs;
  SmallVectorImpl<SILResultInfo> &Results;
  TypeExpansionContext context;

public:
  DestructureResults(TypeExpansionContext context, TypeConverter &TC,
                     const Conventions &conventions,
                     SmallVectorImpl<SILResultInfo> &results)
      : TC(TC), Convs(conventions), Results(results), context(context) {}

  void destructure(AbstractionPattern origType, CanType substType) {
    // Recur into tuples.
    if (origType.isTuple()) {
      origType.forEachTupleElement(substType,
                                   [&](TupleElementGenerator &elt) {
        // If the original element type is not a pack expansion, just
        // pull off the next substituted element type.
        if (!elt.isOrigPackExpansion()) {
          destructure(elt.getOrigType(), elt.getSubstTypes()[0]);
          return;
        }

        // If the original element type is a pack expansion, build a
        // lowered pack type for the substituted components it expands to.
        auto origExpansionType = elt.getOrigType();
        bool indirect = origExpansionType.arePackElementsPassedIndirectly(TC);

        SmallVector<CanType, 4> packElts;
        for (auto substEltType : elt.getSubstTypes()) {
          auto origComponentType
            = origExpansionType.getPackExpansionComponentType(substEltType);
          CanType loweredEltTy =
            TC.getLoweredRValueType(context, origComponentType, substEltType);
          packElts.push_back(loweredEltTy);
        };

        SILPackType::ExtInfo extInfo(indirect);
        auto packType = SILPackType::get(TC.Context, extInfo, packElts);
        SILResultInfo result(packType, ResultConvention::Pack);
        Results.push_back(result);
      });
      return;
    }

    auto &substResultTLForConvention = TC.getTypeLowering(
        origType, substType, TypeExpansionContext::minimal());
    auto &substResultTL = TC.getTypeLowering(origType, substType,
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

        case ResultConvention::Pack:
          llvm_unreachable("pack convention for non-pack");

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

    if (substTy->isOpaquePointer())
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
      if (typedefTy->getDecl()->getAttr<clang::SwiftNewTypeAttr>())
        return false;
    }

    return true;
  }

  // Pass C++ const reference types indirectly. Right now there's no way to
  // express immutable borrowed params, so we have to have this hack.
  // Eventually, we should just express these correctly: rdar://89647503
  if (importer::isCxxConstReferenceType(clangTy))
    return true;

  return false;
}

static bool isFormallyPassedIndirectly(TypeConverter &TC,
                                       AbstractionPattern origType,
                                       CanType substType,
                                       const TypeLowering &substTL) {
  // If this is a native Swift class that's passed directly to C/C++, treat it
  // as indirect.
  if (origType.isClangType()) {
    if (auto *classDecl = substType->lookThroughAllOptionalTypes()
                              ->getClassOrBoundGenericClass()) {
      if (!classDecl->isForeignReferenceType()) {
        if (origType.getClangType()
                ->getUnqualifiedDesugaredType()
                ->getAsCXXRecordDecl())
          return true;
      }
    }
  }

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
  llvm::Optional<ForeignSelfInfo> ForeignSelf;
  AbstractionPattern TopLevelOrigType = AbstractionPattern::getInvalid();
  SmallVectorImpl<SILParameterInfo> &Inputs;
  unsigned NextOrigParamIndex = 0;

public:
  DestructureInputs(TypeExpansionContext expansion, TypeConverter &TC,
                    const Conventions &conventions, const ForeignInfo &foreign,
                    SmallVectorImpl<SILParameterInfo> &inputs)
      : expansion(expansion), TC(TC), Convs(conventions), Foreign(foreign),
        Inputs(inputs) {}

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

  /// Destructure the top-level parameters.  There are two things
  /// we have to handle differently here from the normal recursive
  /// walk into parameter types that expands tuples:
  ///   - self, especially if it's a foreign-imported self, because
  ///     it has different conventions and the foreign self needs to be
  ///     inserted in the right place
  ///   - the possibility of an opaque abstraction pattern, because
  ///     a significant amount of the structure of the parameter list
  ///     is still preserved under opaque abstraction
  void visitTopLevelParams(AbstractionPattern origType,
                           CanAnyFunctionType::CanParamArrayRef params,
                           SILExtInfoBuilder extInfoBuilder) {
    // If we're working with an opaque abstraction pattern, we never
    // have to worry about pack expansions, so we can go 1-1 with the
    // substituted parameters.
    unsigned numOrigParams =
      origType.isTypeParameter()
        ? params.size()
        : origType.getNumFunctionParams();

    // If we're importing a freestanding foreign function as a member
    // function, the formal types (subst and orig) will conspire to
    // pretend that there is a self parameter in the position Swift
    // expects it: the end of the parameter lists.  In the lowered type,
    // we need to put this in its proper place, which for static methods
    // generally means dropping it entirely.
    bool hasForeignSelf = Foreign.self.isImportAsMember();

    // Is there a self parameter in the formal parameter lists?
    bool hasSelf =
      (extInfoBuilder.hasSelfParam() || hasForeignSelf);

    TopLevelOrigType = origType;
    // If we have a foreign self parameter, set up the ForeignSelfInfo
    // for the use of maybeAddForeignParameters.
    if (Foreign.self.isInstance()) {
      assert(hasSelf && numOrigParams > 0);
      ForeignSelf = ForeignSelfInfo{
        origType.getFunctionParamType(numOrigParams - 1),
        params.back()
      };
    }

    // Add any foreign parameters that are positioned at the start
    // of the sequence.  visit() will add foreign parameters that are
    // positioned after any parameters it adds.
    maybeAddForeignParameters();

    // Process all the non-self parameters.
    origType.forEachFunctionParam(params.drop_back(hasSelf ? 1 : 0),
                                  /*ignore final orig param*/ hasSelf,
                                  [&](FunctionParamGenerator &param) {
      // If the parameter is not a pack expansion, just pull off the
      // next parameter and destructure it in parallel with the abstraction
      // pattern for the type.
      if (!param.isOrigPackExpansion()) {
        visit(param.getOrigType(), param.getSubstParams()[0],
              /*forSelf*/false);
        return;
      }

      // Otherwise, collect the substituted components into a pack.
      auto origExpansionType = param.getOrigType();
      SmallVector<CanType, 8> packElts;
      for (auto substParam : param.getSubstParams()) {
        auto substParamType = substParam.getParameterType();
        auto origParamType =
          origExpansionType.getPackExpansionComponentType(substParamType);
        auto loweredParamTy = TC.getLoweredRValueType(expansion,
                                              origParamType, substParamType);
        packElts.push_back(loweredParamTy);
      }

      bool indirect = origExpansionType.arePackElementsPassedIndirectly(TC);
      SILPackType::ExtInfo extInfo(/*address*/ indirect);
      auto packTy = SILPackType::get(TC.Context, extInfo, packElts);

      auto origFlags = param.getOrigFlags();
      addPackParameter(packTy, origFlags.getValueOwnership(),
                       origFlags.isNoDerivative());
    });

    // Process the self parameter.  But if we have a formal foreign self
    // parameter, we should have processed it earlier in a call to
    // maybeAddForeignParameters().
    if (hasSelf && !hasForeignSelf) {
      auto origParamType = origType.getFunctionParamType(numOrigParams - 1);
      auto substParam = params.back();
      visit(origParamType, substParam, /*forSelf*/true);
    }

    TopLevelOrigType = AbstractionPattern::getInvalid();
    ForeignSelf = llvm::None;
  }

  void visit(AbstractionPattern origType, AnyFunctionType::Param substParam,
             bool forSelf) {
    // FIXME: we should really be using the flags from the original
    // parameter here, right?
    auto flags = substParam.getParameterFlags();

    auto substType = substParam.getParameterType()->getCanonicalType();

    // If we see a pack expansion here, that should only happen because
    // we're lowering a function type with a parameter expansion in an
    // opaque context.  We can't actually support this configuration,
    // but we need to make sure we don't crash at this level of the API
    // so we can diagnose it later.
    if (isa<PackExpansionType>(substType)) {
      bool indirect = true;
      SILPackType::ExtInfo extInfo(/*address*/ indirect);
      auto packTy = SILPackType::get(TC.Context, extInfo, {substType});
      return addPackParameter(packTy, flags.getValueOwnership(),
                              flags.isNoDerivative());
    }

    visit(flags.getValueOwnership(), forSelf, origType, substType,
          flags.isNoDerivative());
  }

  void visit(ValueOwnership ownership, bool forSelf,
             AbstractionPattern origType, CanType substType,
             bool isNonDifferentiable) {
    assert(!isa<InOutType>(substType));

    // Tuples get expanded unless they're inout.
    if (origType.isTuple() && ownership != ValueOwnership::InOut) {
      expandTuple(ownership, forSelf, origType, substType,
                  isNonDifferentiable);
      return;
    }

    unsigned origParamIndex = NextOrigParamIndex++;
    
    auto &substTLConv = TC.getTypeLowering(origType, substType,
                                       TypeExpansionContext::minimal());
    auto &substTL = TC.getTypeLowering(origType, substType, expansion);

    CanType loweredType = substTL.getLoweredType().getASTType();

    ParameterConvention convention;
    if (ownership == ValueOwnership::InOut) {
      convention = ParameterConvention::Indirect_Inout;
    } else if (isFormallyPassedIndirectly(origType, substType, substTLConv)) {
      convention = Convs.getIndirect(ownership, forSelf, origParamIndex,
                                     origType, substTLConv);
      assert(isIndirectFormalParameter(convention));
    } else if (substTL.isTrivial() ||
               // Foreign reference types are passed trivially.
               (substType->getClassOrBoundGenericClass() &&
                substType->isForeignReferenceType())) {
      convention = ParameterConvention::Direct_Unowned;
    } else {
      // If we are no implicit copy, our ownership is always Owned.
      convention = Convs.getDirect(ownership, forSelf, origParamIndex, origType,
                                   substTLConv);
      assert(!isIndirectFormalParameter(convention));
    }

    addParameter(loweredType, convention, isNonDifferentiable);
  }

  /// Recursively expand a tuple type into separate parameters.
  void expandTuple(ValueOwnership ownership, bool forSelf,
                   AbstractionPattern origType, CanType substType,
                   bool isNonDifferentiable) {
    assert(ownership != ValueOwnership::InOut);
    assert(origType.isTuple());

    origType.forEachTupleElement(substType, [&](TupleElementGenerator &elt) {
      if (!elt.isOrigPackExpansion()) {
        visit(ownership, forSelf, elt.getOrigType(), elt.getSubstTypes()[0],
              isNonDifferentiable);
        return;
      }

      auto origExpansionType = elt.getOrigType();

      SmallVector<CanType, 8> packElts;
      for (auto substEltType : elt.getSubstTypes()) {
        auto origComponentType
          = origExpansionType.getPackExpansionComponentType(substEltType);
        auto loweredEltTy =
          TC.getLoweredRValueType(expansion, origComponentType, substEltType);
        packElts.push_back(loweredEltTy);
      };

      bool indirect = origExpansionType.arePackElementsPassedIndirectly(TC);
      SILPackType::ExtInfo extInfo(/*address*/ indirect);
      auto packTy = SILPackType::get(TC.Context, extInfo, packElts);

      addPackParameter(packTy, ownership, isNonDifferentiable);
    });
  }

  /// Add a parameter that we derived from deconstructing the
  /// formal type.
  void addParameter(CanType loweredType, ParameterConvention convention,
                    bool isNonDifferentiable) {
    SILParameterInfo param(loweredType, convention);
    if (isNonDifferentiable)
      param = param.getWithDifferentiability(
          SILParameterDifferentiability::NotDifferentiable);
    Inputs.push_back(param);

    maybeAddForeignParameters();
  }

  void addPackParameter(CanSILPackType packTy, ValueOwnership ownership,
                        bool isNonDifferentiable) {
    unsigned origParamIndex = NextOrigParamIndex++;
    auto convention = Convs.getPack(ownership, origParamIndex);
    addParameter(packTy, convention, isNonDifferentiable);
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
    if (!Foreign.async ||
        NextOrigParamIndex != Foreign.async->completionHandlerParamIndex())
      return false;

    CanType foreignCHTy =
        TopLevelOrigType.getObjCMethodAsyncCompletionHandlerForeignType(
            Foreign.async.value(), TC);
    auto completionHandlerOrigTy = TopLevelOrigType.getObjCMethodAsyncCompletionHandlerType(foreignCHTy);
    auto completionHandlerTy = TC.getLoweredType(completionHandlerOrigTy,
                                                 foreignCHTy, expansion)
      .getASTType();
    Inputs.push_back(SILParameterInfo(completionHandlerTy,
                                      ParameterConvention::Direct_Unowned));
    ++NextOrigParamIndex;
    return true;
  }

  bool maybeAddForeignErrorParameter() {
    if (!Foreign.error ||
        NextOrigParamIndex != Foreign.error->getErrorParameterIndex())
      return false;

    auto foreignErrorTy = TC.getLoweredRValueType(
        expansion, Foreign.error->getErrorParameterType());

    // Assume the error parameter doesn't have interesting lowering.
    Inputs.push_back(SILParameterInfo(foreignErrorTy,
                                      ParameterConvention::Direct_Unowned));
    ++NextOrigParamIndex;
    return true;
  }

  bool maybeAddForeignSelfParameter() {
    if (!Foreign.self.isInstance() ||
        NextOrigParamIndex != Foreign.self.getSelfIndex())
      return false;

    if (ForeignSelf) {
      // This is a "self", but it's not a Swift self, we handle it differently.
      visit(ForeignSelf->SubstSelfParam.getValueOwnership(),
            /*forSelf=*/false, ForeignSelf->OrigSelfParam,
            ForeignSelf->SubstSelfParam.getParameterType(), false);
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
  return (classDecl && classDecl->isTypeErasedGenericClass());
}

/// Update the result type given the foreign error convention that we will be
/// using.
void updateResultTypeForForeignInfo(
    const ForeignInfo &foreignInfo, CanGenericSignature genericSig,
    AbstractionPattern &origResultType, CanType &substFormalResultType) {
  // If there's no error or async convention, the return type is unchanged.
  if (!foreignInfo.async && !foreignInfo.error) {
    return;
  }

  // A foreign async convention without an error convention means our lowered
  // return type is Void, since the imported semantic return map to the
  // completion callback's argument(s).
  if (!foreignInfo.error) {
    auto &C = substFormalResultType->getASTContext();
    substFormalResultType = TupleType::getEmpty(C);
    origResultType = AbstractionPattern(genericSig, substFormalResultType);
    return;
  }

  // Otherwise, adjust the return type to match the foreign error convention.
  auto convention = *foreignInfo.error;
  switch (convention.getKind()) {
  // These conventions replace the result type.
  case ForeignErrorConvention::ZeroResult:
  case ForeignErrorConvention::NonZeroResult:
    assert(substFormalResultType->isVoid() || foreignInfo.async);
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

      auto canSelfMetatype = selfMetatype->getReducedType(origGenericSig);
      SILParameterInfo param(canSelfMetatype, convention);
      inputs.push_back(param);

      continue;
    }

    if (capture.isOpaqueValue()) {
      OpaqueValueExpr *opaqueValue = capture.getOpaqueValue();
      auto canType = opaqueValue->getType()->mapTypeOutOfContext()
          ->getReducedType(origGenericSig);
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
    auto canType = type->getReducedType(origGenericSig);

    // If we're capturing a parameter pack, wrap it in a tuple.
    if (isa<PackExpansionType>(canType)) {
      assert(!cast<ParamDecl>(VD)->supportsMutation() &&
             "Cannot capture a pack as an lvalue");

      SmallVector<TupleTypeElt, 1> elts;
      elts.push_back(canType);
      canType = CanTupleType(TupleType::get(elts, TC.Context));
    }

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
    case CaptureKind::ImmutableBox: {
      // The type in the box is lowered in the minimal context.
      auto minimalLoweredTy =
          TC.getTypeLowering(AbstractionPattern(genericSig, canType), canType,
                             TypeExpansionContext::minimal())
              .getLoweredType();
      // Lvalues are captured as a box that owns the captured value.
      auto boxTy =
          TC.getInterfaceBoxTypeForCapture(VD, minimalLoweredTy.getASTType(),
                                           /*mutable*/ false);
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

static AccessorDecl *
getAsCoroutineAccessor(llvm::Optional<SILDeclRef> constant) {
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
                                         SmallVectorImpl<SILYieldInfo> &yields){
  // Recursively destructure tuples.
  if (origType.isTuple()) {
    auto valueTupleType = cast<TupleType>(valueType);
    for (auto i : indices(valueTupleType.getElementTypes())) {
      auto origEltType = origType.getTupleElementType(i);
      auto valueEltType = valueTupleType.getElementType(i);
      destructureYieldsForReadAccessor(TC, expansion, origEltType, valueEltType,
                                       yields);
    }
    return;
  }

  auto &tlConv =
      TC.getTypeLowering(origType, valueType,
                         TypeExpansionContext::minimal());
  auto &tl =
      TC.getTypeLowering(origType, valueType, expansion);
  auto convention = [&] {
    if (isFormallyPassedIndirectly(TC, origType, valueType, tlConv))
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
                                          llvm::Optional<SILDeclRef> constant,
                                          AbstractionPattern origType,
                                          CanType canValueType,
                                          SmallVectorImpl<SILYieldInfo> &yields,
                                          SILCoroutineKind &coroutineKind) {
  auto accessor = getAsCoroutineAccessor(constant);
  if (!accessor)
    return;

  // 'modify' yields an inout of the target type.
  if (accessor->getAccessorKind() == AccessorKind::Modify) {
    auto loweredValueTy =
        TC.getLoweredType(origType, canValueType, expansion);
    yields.push_back(SILYieldInfo(loweredValueTy.getASTType(),
                                  ParameterConvention::Indirect_Inout));
  } else {
    // 'read' yields a borrowed value of the target type, destructuring
    // tuples as necessary.
    assert(accessor->getAccessorKind() == AccessorKind::Read);
    destructureYieldsForReadAccessor(TC, expansion, origType, canValueType,
                                     yields);
  }
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
    const ForeignInfo &foreignInfo, llvm::Optional<SILDeclRef> origConstant,
    llvm::Optional<SILDeclRef> constant,
    llvm::Optional<SubstitutionMap> reqtSubs,
    ProtocolConformanceRef witnessMethodConformance) {
  // Find the generic parameters.
  CanGenericSignature genericSig =
    substFnInterfaceType.getOptGenericSignature();

  llvm::Optional<TypeConverter::GenericContextRAII> contextRAII;
  if (genericSig) contextRAII.emplace(TC, genericSig);

  // Per above, only fully honor opaqueness in the abstraction pattern
  // for thick or polymorphic functions.  We don't need to worry about
  // non-opaque patterns because the type-checker forbids non-thick
  // function types from having generic parameters or results.
  if (!constant &&
      origType.isTypeParameter() &&
      substFnInterfaceType->getExtInfo().getSILRepresentation()
        != SILFunctionType::Representation::Thick &&
      isa<FunctionType>(substFnInterfaceType)) {
    origType = AbstractionPattern(genericSig,
                                  substFnInterfaceType);
  }

  llvm::Optional<SILResultInfo> errorResult;
  assert(
      (!foreignInfo.error || substFnInterfaceType->getExtInfo().isThrowing()) &&
      "foreignError was set but function type does not throw?");
  assert((!foreignInfo.async || substFnInterfaceType->getExtInfo().isAsync()) &&
         "foreignAsync was set but function type is not async?");

  // Map '@Sendable' to the appropriate `@Sendable` modifier.
  bool isSendable = substFnInterfaceType->getExtInfo().isSendable();

  // Map 'async' to the appropriate `@async` modifier.
  bool isAsync = false;
  if (substFnInterfaceType->getExtInfo().isAsync() && !foreignInfo.async) {
    assert(!origType.isForeign()
           && "using native Swift async for foreign type!");
    isAsync = true;
  }

  // Map 'throws' to the appropriate error convention.
  // Give the type an error argument whether the substituted type semantically
  // `throws` or if the abstraction pattern specifies a Swift function type
  // that also throws. This prevents the need for a second possibly-thunking
  // conversion when using a non-throwing function in more abstract throwing
  // context.
  bool isThrowing = substFnInterfaceType->getExtInfo().isThrowing();
  if (auto origFnType = origType.getAs<AnyFunctionType>()) {
    isThrowing |= origFnType->getExtInfo().isThrowing();
  }
  if (isThrowing && !foreignInfo.error &&
      !foreignInfo.async) {
    assert(!origType.isForeign()
           && "using native Swift error convention for foreign type!");
    SILType exnType = SILType::getExceptionType(TC.Context);
    assert(exnType.isObject());
    errorResult = SILResultInfo(exnType.getASTType(),
                                ResultConvention::Owned);
  }
  
  // Get the yield type for an accessor coroutine.
  SILCoroutineKind coroutineKind = SILCoroutineKind::None;
  AbstractionPattern coroutineOrigYieldType = AbstractionPattern::getInvalid();
  CanType coroutineSubstYieldType;
  
  if (auto accessor = getAsCoroutineAccessor(constant)) {
    auto origAccessor = cast<AccessorDecl>(origConstant->getDecl());
    coroutineKind = SILCoroutineKind::YieldOnce;
    
    // Coroutine accessors are always native, so fetch the native
    // abstraction pattern.
    auto origStorage = origAccessor->getStorage();
    coroutineOrigYieldType = TC.getAbstractionPattern(origStorage,
                                                      /*nonobjc*/ true)
                               .getReferenceStorageReferentType();

    auto storage = accessor->getStorage();
    auto valueType = storage->getValueInterfaceType();

    if (reqtSubs) {
      valueType = valueType.subst(*reqtSubs);
      coroutineSubstYieldType = valueType->getReducedType(
          genericSig);
    } else {
      coroutineSubstYieldType = valueType->getReducedType(
          accessor->getGenericSignature());
    }
  }

  bool shouldBuildSubstFunctionType = [&]{
    if (TC.Context.LangOpts.DisableSubstSILFunctionTypes)
      return false;

    // If there is no genericity in the abstraction pattern we're lowering
    // against, we don't need to introduce substitutions into the lowered
    // type.
    if (!origType.isTypeParameterOrOpaqueArchetype()
        && !origType.isOpaqueFunctionOrOpaqueDerivativeFunction()
        && !origType.getType()->hasArchetype()
        && !origType.getType()->hasOpaqueArchetype()
        && !origType.getType()->hasTypeParameter()
        && !isa<GenericFunctionType>(origType.getType())) {
      return false;
    }

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
  
  SubstitutionMap substFunctionTypeSubs;
  
  if (shouldBuildSubstFunctionType) {
    // Generalize the generic signature in the abstraction pattern, so that
    // abstraction patterns with the same general shape produce equivalent
    // lowered function types.
    AbstractionPattern origSubstPat = AbstractionPattern::getInvalid();
    AbstractionPattern substYieldType = AbstractionPattern::getInvalid();
    std::tie(origSubstPat, substFunctionTypeSubs, substYieldType)
      = origType.getSubstFunctionTypePattern(substFnInterfaceType, TC,
                                             coroutineOrigYieldType,
                                             coroutineSubstYieldType);

    // We'll lower the abstraction pattern type against itself, and then apply
    // those substitutions to form the substituted lowered function type.
    origType = origSubstPat;
    substFnInterfaceType = cast<AnyFunctionType>(origType.getType());
    if (substYieldType.isValid()) {
      coroutineOrigYieldType = substYieldType;
      coroutineSubstYieldType = substYieldType.getType();
    }
  }

  // Lower the result type.
  AbstractionPattern origResultType = origType.getFunctionResultType();
  CanType substFormalResultType = substFnInterfaceType.getResult();

  // If we have a foreign error and/or async convention, adjust the
  // lowered result type.
  updateResultTypeForForeignInfo(foreignInfo, genericSig, origResultType,
                                 substFormalResultType);

  // Destructure the input tuple type.
  SmallVector<SILParameterInfo, 8> inputs;
  {
    DestructureInputs destructurer(expansionContext, TC, conventions,
                                   foreignInfo, inputs);
    destructurer.destructure(origType, substFnInterfaceType.getParams(),
                             extInfoBuilder);
  }

  // Destructure the coroutine yields.
  SmallVector<SILYieldInfo, 8> yields;
  destructureYieldsForCoroutine(TC, expansionContext, constant,
                                coroutineOrigYieldType, coroutineSubstYieldType,
                                yields, coroutineKind);
  
  // Destructure the result tuple type.
  SmallVector<SILResultInfo, 8> results;
  {
    DestructureResults destructurer(expansionContext, TC, conventions,
                                    results);
    destructurer.destructure(origResultType, substFormalResultType);
  }

  // Lower the capture context parameters, if any.
  if (constant && constant->getAnyFunctionRef()) {
    // Lower in the context of the closure. Since the set of captures is a
    // private contract between the closure and its enclosing context, we
    // don't need to keep its capture types opaque.
    lowerCaptureContextParameters(TC, *constant, genericSig,
                                  TC.getCaptureTypeExpansionContext(*constant),
                                  inputs);
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
        convertRepresentation(silRep).value());
  }
  auto silExtInfo = extInfoBuilder.withClangFunctionType(clangType)
                        .withIsPseudogeneric(pseudogeneric)
                        .withConcurrent(isSendable)
                        .withAsync(isAsync)
                        .build();

  return SILFunctionType::get(genericSig, silExtInfo, coroutineKind,
                              calleeConvention, inputs, yields,
                              results, errorResult,
                              substFunctionTypeSubs, SubstitutionMap(),
                              TC.Context, witnessMethodConformance);
}

static CanSILFunctionType getSILFunctionTypeForInitAccessor(
    TypeConverter &TC, TypeExpansionContext context,
    AbstractionPattern origType, CanAnyFunctionType substAccessorType,
    SILExtInfoBuilder extInfoBuilder, const Conventions &conventions,
    SILDeclRef accessorRef) {
  auto *accessor = cast<AccessorDecl>(accessorRef.getDecl());

  CanGenericSignature genericSig = substAccessorType.getOptGenericSignature();

  llvm::Optional<TypeConverter::GenericContextRAII> contextRAII;
  if (genericSig)
    contextRAII.emplace(TC, genericSig);

  SmallVector<SILParameterInfo, 8> inputs;

  // First compute `initialValue` input.
  {
    ForeignInfo foreignInfo;
    DestructureInputs destructurer(context, TC, conventions, foreignInfo,
                                   inputs);
    destructurer.destructure(
        origType, substAccessorType.getParams(),
        extInfoBuilder.withRepresentation(SILFunctionTypeRepresentation::Thin));
  }

  // Drop `self` parameter.
  inputs.pop_back();

  // `accesses(...)` appear as `inout` parameters because they could be
  // read from and modified.
  if (auto *attr = accessor->getAttrs().getAttribute<AccessesAttr>()) {
    for (auto *property : attr->getPropertyDecls(accessor)) {
      inputs.push_back(
          SILParameterInfo(property->getInterfaceType()->getCanonicalType(),
                           ParameterConvention::Indirect_Inout));
    }
  }

  SmallVector<SILResultInfo, 8> results;

  // `initializes(...)` appear as `@out` result because they are initialized
  // by the accessor.
  if (auto *attr = accessor->getAttrs().getAttribute<InitializesAttr>()) {
    for (auto *property : attr->getPropertyDecls(accessor)) {
      results.push_back(
          SILResultInfo(property->getInterfaceType()->getCanonicalType(),
                        ResultConvention::Indirect));
    }
  }

  auto calleeConvention = ParameterConvention::Direct_Unowned;
  if (extInfoBuilder.hasContext())
    calleeConvention = conventions.getCallee();

  // Map '@Sendable' to the appropriate `@Sendable` modifier.
  auto silExtInfo =
      SILExtInfoBuilder()
          .withRepresentation(SILFunctionTypeRepresentation::Thin)
          .withConcurrent(substAccessorType->getExtInfo().isSendable())
          .build();

  return SILFunctionType::get(
      /*genericSig=*/genericSig, silExtInfo, SILCoroutineKind::None,
      calleeConvention, inputs,
      /*yields=*/{}, results, /*errorResult=*/llvm::None,
      /*patternSubs=*/SubstitutionMap(),
      /*invocationSubs=*/SubstitutionMap(), TC.Context);
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

  ParameterConvention getPackParameter(unsigned index) const override {
    llvm_unreachable("Deallocators do not have pack parameters");
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
    return ParameterConvention::Indirect_In;
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
  ResultConvention resultConvention;

public:
  DefaultConventions(
      NormalParameterConvention normalParameterConvention,
      ResultConvention resultConvention = ResultConvention::Owned)
      : Conventions(ConventionsKind::Default),
        normalParameterConvention(normalParameterConvention),
        resultConvention(resultConvention) {}

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

  ParameterConvention getPackParameter(unsigned index) const override {
    if (isNormalParameterConventionGuaranteed())
      return ParameterConvention::Pack_Guaranteed;
    return ParameterConvention::Pack_Owned;
  }

  ParameterConvention getCallee() const override {
    return DefaultThickCalleeConvention;
  }

  ResultConvention getResult(const TypeLowering &tl) const override {
    return resultConvention;
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

/// The default conventions for Swift setter accessories.
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

  ParameterConvention getPackParameter(unsigned index) const override {
    llvm_unreachable("objc blocks do not have pack parameters");
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
    llvm::Optional<SILDeclRef> constant);

static CanSILFunctionType getNativeSILFunctionType(
    TypeConverter &TC, TypeExpansionContext context,
    AbstractionPattern origType, CanAnyFunctionType substInterfaceType,
    SILExtInfoBuilder extInfoBuilder, llvm::Optional<SILDeclRef> origConstant,
    llvm::Optional<SILDeclRef> constant,
    llvm::Optional<SubstitutionMap> reqtSubs,
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
    switch (origConstant ? origConstant->kind : SILDeclRef::Kind::Func) {
    case SILDeclRef::Kind::Initializer:
    case SILDeclRef::Kind::EnumElement:
      return getSILFunctionTypeForConventions(DefaultInitializerConventions());
    case SILDeclRef::Kind::Allocator:
      return getSILFunctionTypeForConventions(DefaultAllocatorConventions());
    case SILDeclRef::Kind::Func: {
      // If we have a setter, use the special setter convention. This ensures
      // that we take normal parameters at +1.
      if (constant) {
        if (constant->isSetter()) {
          return getSILFunctionTypeForConventions(DefaultSetterConventions());
        } else if (constant->isInitAccessor()) {
          return getSILFunctionTypeForInitAccessor(
              TC, context, origType, substInterfaceType, extInfoBuilder,
              DefaultSetterConventions(), *constant);
        }
      }
      return getSILFunctionTypeForConventions(
          DefaultConventions(NormalParameterConvention::Guaranteed));
    }
    case SILDeclRef::Kind::Destroyer:
    case SILDeclRef::Kind::GlobalAccessor:
    case SILDeclRef::Kind::DefaultArgGenerator:
    case SILDeclRef::Kind::StoredPropertyInitializer:
    case SILDeclRef::Kind::PropertyWrapperBackingInitializer:
    case SILDeclRef::Kind::PropertyWrapperInitFromProjectedValue:
    case SILDeclRef::Kind::IVarInitializer:
    case SILDeclRef::Kind::IVarDestroyer:
      return getSILFunctionTypeForConventions(
          DefaultConventions(NormalParameterConvention::Guaranteed));
    case SILDeclRef::Kind::Deallocator:
      return getSILFunctionTypeForConventions(DeallocatorConventions());

    case SILDeclRef::Kind::AsyncEntryPoint:
      return getSILFunctionTypeForConventions(
          DefaultConventions(NormalParameterConvention::Guaranteed));
    case SILDeclRef::Kind::RuntimeAttributeGenerator:
      return getSILFunctionTypeForConventions(DefaultConventions(
          NormalParameterConvention::Guaranteed, ResultConvention::Indirect));
    case SILDeclRef::Kind::EntryPoint:
      llvm_unreachable("Handled by getSILFunctionTypeForAbstractCFunction");
    }
  }
  }

  llvm_unreachable("Unhandled SILDeclRefKind in switch.");
}

CanSILFunctionType swift::getNativeSILFunctionType(
    TypeConverter &TC, TypeExpansionContext context,
    AbstractionPattern origType, CanAnyFunctionType substType,
    SILExtInfo silExtInfo, llvm::Optional<SILDeclRef> origConstant,
    llvm::Optional<SILDeclRef> substConstant,
    llvm::Optional<SubstitutionMap> reqtSubs,
    ProtocolConformanceRef witnessMethodConformance) {

  return ::getNativeSILFunctionType(
      TC, context, origType, substType, silExtInfo.intoBuilder(), origConstant,
      substConstant, reqtSubs, witnessMethodConformance);
}

namespace {
struct LocalArchetypeRequirementCollector {
  const ASTContext &Context;
  unsigned Depth;

  /// The lists of new parameters and requirements to add to the signature.
  SmallVector<GenericTypeParamType *, 2> Params;
  SmallVector<Requirement, 2> Requirements;

  /// The list of contextual types from the original function to use as
  /// substitutions for the new parameters; parallel to Params.
  SmallVector<Type, 4> ParamSubs;

  /// A mapping of local archetypes to the corresponding type parameters
  /// created for them.
  llvm::DenseMap<CanType, Type> ParamsForLocalArchetypes;

  /// The set of element environments we've processed.
  llvm::SmallPtrSet<GenericEnvironment*, 4> ElementEnvs;

  LocalArchetypeRequirementCollector(const ASTContext &ctx, unsigned depth)
    : Context(ctx), Depth(depth) {}

  void collect(CanLocalArchetypeType archetype) {
    if (auto openedExistential = dyn_cast<OpenedArchetypeType>(archetype)) {
      collect(openedExistential);
    } else {
      collect(cast<ElementArchetypeType>(archetype));
    }
  }

  void collect(CanOpenedArchetypeType archetype) {
    auto param = addParameter(archetype);

    assert(archetype->isRoot());
    auto constraint = archetype->getExistentialType();
    if (auto existential = constraint->getAs<ExistentialType>())
      constraint = existential->getConstraintType();

    addRequirement(RequirementKind::Conformance, param, constraint);
  }

  void collect(CanElementArchetypeType archetype) {
    size_t startingIndex = Params.size();

    // Check whether we've already handled this environment.
    // This can happen with opened-element environments because
    // they can open multiple archetypes.
    auto env = archetype->getGenericEnvironment();
    if (!ElementEnvs.insert(env).second) return;

    // Add a parameter for each of the opened elements in this environment.
    auto sig = env->getGenericSignature();
    auto elementParams = sig.getInnermostGenericParams();
#ifndef NDEBUG
    unsigned nextIndex = 0;
#endif
    for (auto elementParam : elementParams) {
      assert(elementParam->getIndex() == nextIndex++);
      auto elementArchetype = env->mapTypeIntoContext(elementParam);
      addParameter(cast<LocalArchetypeType>(CanType(elementArchetype)));
    }

    // Clone the element requirements.

    // The element parameters should all have the same depth, and their
    // index values are dense and in order from 0..<N in that depth.
    // We asserted that above, and we'll use it below to map them to
    // their new parameters in Params.
    auto elementDepth = elementParams.front()->getDepth();

    // Helper function: does the given type refer to an opened element
    // parameter from the opened element generic signature?
    auto refersToElementType = [=](Type type) {
      return type.findIf([&](Type t) {
        if (auto *param = t->getAs<GenericTypeParamType>())
          return (param->getDepth() == elementDepth);
        return false;
      });
    };
    // Helper function: replace references to opened element parameters
    // with one of the type parameters we just created for this
    // environment.
    auto rewriteElementType = [=](Type type) {
      return type.transformRec([&](Type t) -> llvm::Optional<Type> {
        if (auto *param = t->getAs<GenericTypeParamType>()) {
          if (param->getDepth() == elementDepth)
            return Type(Params[startingIndex + param->getIndex()]);
        }
        return llvm::None;
      });
    };

    for (auto req : sig.getRequirements()) {
      switch (req.getKind()) {
      case RequirementKind::SameShape:
        // These never involve element types.
        break;
      case RequirementKind::Conformance:
        if (refersToElementType(req.getFirstType())) {
          addRequirement(RequirementKind::Conformance,
                         rewriteElementType(req.getFirstType()),
                         req.getSecondType());
        }
        break;
      case RequirementKind::Superclass:
      case RequirementKind::SameType:
        if (refersToElementType(req.getFirstType()) ||
            refersToElementType(req.getSecondType())) {
          addRequirement(req.getKind(),
                         rewriteElementType(req.getFirstType()),
                         rewriteElementType(req.getSecondType()));
        }
        break;
        break;
      case RequirementKind::Layout:
        if (refersToElementType(req.getFirstType())) {
          addRequirement(RequirementKind::Layout,
                         rewriteElementType(req.getFirstType()),
                         req.getLayoutConstraint());
        }
        break;
      }
    }
  }

  GenericTypeParamType *addParameter(CanLocalArchetypeType localArchetype) {
    auto *param = GenericTypeParamType::get(/*pack*/ false, Depth,
                                            Params.size(), Context);
    Params.push_back(param);
    ParamSubs.push_back(localArchetype);
    ParamsForLocalArchetypes.insert(std::make_pair(localArchetype, param));
    return param;
  }

  template <class... Args>
  void addRequirement(Args &&... args) {
    Requirements.emplace_back(std::forward<Args>(args)...);
  }
};
} // end anonymous namespace

/// Build a generic signature and environment for a re-abstraction thunk.
///
/// Most thunks share the generic environment with their original function.
/// The one exception is if the thunk type involves an open existential,
/// in which case we "promote" the opened existential to a new generic parameter.
///
/// \param localArchetypes - the list of local archetypes to promote
///   into the signature, if any
/// \param inheritGenericSig - whether to inherit the generic signature from the
/// parent function.
/// \param genericEnv - the new generic environment
/// \param contextSubs - map non-local archetypes from the original function
///    to archetypes in the thunk
/// \param interfaceSubs - map interface types to old archetypes
static CanGenericSignature
buildThunkSignature(SILFunction *fn,
                    bool inheritGenericSig,
                    ArrayRef<CanLocalArchetypeType> localArchetypes,
                    GenericEnvironment *&genericEnv,
                    SubstitutionMap &contextSubs,
                    SubstitutionMap &interfaceSubs,
                    llvm::DenseMap<ArchetypeType*, Type> &contextLocalArchetypes) {
  auto *mod = fn->getModule().getSwiftModule();
  auto &ctx = mod->getASTContext();

  // If there are no local archetypes, we just inherit the generic
  // environment from the parent function.
  if (localArchetypes.empty()) {
    auto genericSig =
      fn->getLoweredFunctionType()->getInvocationGenericSignature();
    genericEnv = fn->getGenericEnvironment();
    interfaceSubs = fn->getForwardingSubstitutionMap();
    contextSubs = interfaceSubs;
    return genericSig;
  }

  // Add the existing generic signature.
  unsigned depth = 0;
  GenericSignature baseGenericSig;
  if (inheritGenericSig) {
    if (auto genericSig =
          fn->getLoweredFunctionType()->getInvocationGenericSignature()) {
      baseGenericSig = genericSig;
      depth = genericSig.getGenericParams().back()->getDepth() + 1;
    }
  }

  // Add new generic parameters to replace the local archetypes.
  LocalArchetypeRequirementCollector collector(ctx, depth);

  for (auto archetype : localArchetypes) {
    collector.collect(archetype);
  }

  auto genericSig = buildGenericSignature(ctx, baseGenericSig,
                                          collector.Params,
                                          collector.Requirements);
  genericEnv = genericSig.getGenericEnvironment();

  // Map the local archetypes to their new parameter types.
  for (auto localArchetype : localArchetypes) {
    auto param =
      collector.ParamsForLocalArchetypes.find(localArchetype)->second;
    auto thunkArchetype = genericEnv->mapTypeIntoContext(param);
    contextLocalArchetypes.insert(std::make_pair(localArchetype,
                                                 thunkArchetype));
  }

  // Calculate substitutions to map the caller's archetypes to the thunk's
  // archetypes.
  if (auto calleeGenericSig = fn->getLoweredFunctionType()
          ->getInvocationGenericSignature()) {
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
      if (auto param = dyn_cast<GenericTypeParamType>(type)) {
        if (param->getDepth() == depth) {
          return collector.ParamSubs[param->getIndex()];
        }
      }
      return fn->mapTypeIntoContext(type);
    },
    MakeAbstractConformanceForGenericType());

  return genericSig.getCanonicalSignature();
}

/// Build the type of a function transformation thunk.
CanSILFunctionType swift::buildSILFunctionThunkType(
    SILFunction *fn, CanSILFunctionType &sourceType,
    CanSILFunctionType &expectedType, CanType &inputSubstType,
    CanType &outputSubstType, GenericEnvironment *&genericEnv,
    SubstitutionMap &interfaceSubs, CanType &dynamicSelfType,
    bool withoutActuallyEscaping,
    llvm::Optional<DifferentiationThunkKind> differentiationThunkKind) {
  // We shouldn't be thunking generic types here, and substituted function types
  // ought to have their substitutions applied before we get here.
  assert(!expectedType->isPolymorphic() &&
         !expectedType->getCombinedSubstitutions());
  assert(!sourceType->isPolymorphic() &&
         !sourceType->getCombinedSubstitutions());

  // This may inherit @noescape from the expectedType. The @noescape attribute
  // is only stripped when using this type to materialize a new decl.
  auto extInfoBuilder = expectedType->getExtInfo().intoBuilder();
  if (!differentiationThunkKind ||
      *differentiationThunkKind == DifferentiationThunkKind::Reabstraction ||
      extInfoBuilder.hasContext()) {
    // Can't build a reabstraction thunk without context, so we require
    // ownership semantics on the result type.
    assert(expectedType->getExtInfo().hasContext());

    extInfoBuilder = extInfoBuilder.withRepresentation(
          SILFunctionType::Representation::Thin);
  }

  if (withoutActuallyEscaping)
    extInfoBuilder = extInfoBuilder.withNoEscape(false);

  // Does the thunk type involve archetypes other than local archetypes?
  bool hasArchetypes = false;
  // Does the thunk type involve a local archetype type?
  SmallVector<CanLocalArchetypeType, 8> localArchetypes;
  auto archetypeVisitor = [&](CanType t) {
    if (auto archetypeTy = dyn_cast<ArchetypeType>(t)) {
      if (auto opened = dyn_cast<LocalArchetypeType>(archetypeTy)) {
        auto root = opened.getRoot();
        if (llvm::find(localArchetypes, root) == localArchetypes.end())
          localArchetypes.push_back(root);
      } else {
        hasArchetypes = true;
      }
    }
  };

  // Use the generic signature from the context if the thunk involves
  // generic parameters.
  CanGenericSignature genericSig;
  SubstitutionMap contextSubs;
  llvm::DenseMap<ArchetypeType*, Type> contextLocalArchetypes;

  if (expectedType->hasArchetype() || sourceType->hasArchetype()) {
    expectedType.visit(archetypeVisitor);
    sourceType.visit(archetypeVisitor);

    genericSig = buildThunkSignature(fn,
                                     hasArchetypes,
                                     localArchetypes,
                                     genericEnv,
                                     contextSubs,
                                     interfaceSubs,
                                     contextLocalArchetypes);
  }

  auto substTypeHelper = [&](SubstitutableType *type) -> Type {
    // If it's a local archetype, do an ad-hoc mapping through the
    // map produced by buildThunkSignature.
    if (auto *archetype = dyn_cast<LocalArchetypeType>(type)) {
      assert(!contextLocalArchetypes.empty());

      // Decline to map non-root archetypes; subst() will come back
      // to us later and ask about the root.
      if (!archetype->isRoot())
        return Type();

      auto it = contextLocalArchetypes.find(archetype);
      assert(it != contextLocalArchetypes.end());
      return it->second;
    }

    // Otherwise, use the context substitutions.
    return Type(type).subst(contextSubs);
  };
  auto substConformanceHelper =
    LookUpConformanceInSubstitutionMap(contextSubs);

  // Utility function to apply contextSubs, and also replace the
  // opened existential with the new archetype.
  auto substFormalTypeIntoThunkContext =
      [&](CanType t) -> CanType {
    return t.subst(substTypeHelper, substConformanceHelper)
               ->getCanonicalType();
  };
  auto substLoweredTypeIntoThunkContext =
      [&](CanSILFunctionType t) -> CanSILFunctionType {
    return SILType::getPrimitiveObjectType(t)
             .subst(fn->getModule(), substTypeHelper, substConformanceHelper)
             .castTo<SILFunctionType>();
  };

  sourceType = substLoweredTypeIntoThunkContext(sourceType);
  expectedType = substLoweredTypeIntoThunkContext(expectedType);

  bool hasDynamicSelf = false;

  if (inputSubstType) {
    inputSubstType = substFormalTypeIntoThunkContext(inputSubstType);
    hasDynamicSelf |= inputSubstType->hasDynamicSelfType();
  }

  if (outputSubstType) {
    outputSubstType = substFormalTypeIntoThunkContext(outputSubstType);
    hasDynamicSelf |= outputSubstType->hasDynamicSelfType();
  }

  hasDynamicSelf |= sourceType->hasDynamicSelfType();
  hasDynamicSelf |= expectedType->hasDynamicSelfType();

  // If our parent function was pseudogeneric, this thunk must also be
  // pseudogeneric, since we have no way to pass generic parameters.
  if (genericSig)
    if (fn->getLoweredFunctionType()->isPseudogeneric())
      extInfoBuilder = extInfoBuilder.withIsPseudogeneric();

  // Add the function type as the parameter.
  auto contextConvention =
      fn->getTypeLowering(sourceType).isTrivial()
          ? ParameterConvention::Direct_Unowned
          : ParameterConvention::Direct_Guaranteed;
  SmallVector<SILParameterInfo, 4> params;
  params.append(expectedType->getParameters().begin(),
                expectedType->getParameters().end());

  if (!differentiationThunkKind ||
      *differentiationThunkKind == DifferentiationThunkKind::Reabstraction) {
    params.push_back({sourceType,
                      sourceType->getExtInfo().hasContext()
                        ? contextConvention
                        : ParameterConvention::Direct_Unowned});
  }

  // If this thunk involves DynamicSelfType in any way, add a capture for it
  // in case we need to recover metadata.
  if (hasDynamicSelf) {
    dynamicSelfType = fn->getDynamicSelfMetadata()->getType().getASTType();
    if (!isa<MetatypeType>(dynamicSelfType)) {
      dynamicSelfType = CanMetatypeType::get(dynamicSelfType,
                                             MetatypeRepresentation::Thick);
    }
    params.push_back({dynamicSelfType, ParameterConvention::Direct_Unowned});
  }

  auto mapTypeOutOfContext = [&](CanType type) -> CanType {
    return type->mapTypeOutOfContext()->getCanonicalType();
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

  llvm::Optional<SILResultInfo> interfaceErrorResult;
  if (expectedType->hasErrorResult()) {
    auto errorResult = expectedType->getErrorResult();
    interfaceErrorResult = errorResult.map(mapTypeOutOfContext);;
  }

  // The type of the thunk function.
  return SILFunctionType::get(
      genericSig, extInfoBuilder.build(), expectedType->getCoroutineKind(),
      ParameterConvention::Direct_Unowned, interfaceParams, interfaceYields,
      interfaceResults, interfaceErrorResult,
      expectedType->getPatternSubstitutions(), SubstitutionMap(),
      fn->getASTContext());
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
  if (importer::isCxxConstReferenceType(type.getTypePtr()))
    return ParameterConvention::Indirect_In_Guaranteed;
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
  if (auto *cxxRecord = type->getAsCXXRecordDecl()) {
    // Directly passed non-trivially destroyed C++ record is consumed by the
    // callee.
    if (!cxxRecord->hasTrivialDestructor())
      return ParameterConvention::Direct_Owned;
  }
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

  ParameterConvention getPackParameter(unsigned index) const override {
    llvm_unreachable("objc methods do not have pack parameters");
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
      if (type.unwrapOptionalType().getASTType()->isUnmanaged())
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
    if (type.isClangType()) {
      if (type.getClangType()
              ->getUnqualifiedDesugaredType()
              ->getAsCXXRecordDecl()) {
        auto t = substTL.getLoweredType().getASTType();
        if (auto *classDecl = t.getPointer()
                                  ->lookThroughAllOptionalTypes()
                                  ->getClassOrBoundGenericClass()) {
          if (!classDecl->isForeignReferenceType()) {
            assert(!classDecl->hasClangNode() &&
                   "unexpected imported class type in C function");
            assert(!classDecl->isGeneric());
            return ParameterConvention::Indirect_In_Guaranteed;
          }
        }
      }
    }
    return getIndirectCParameterConvention(getParamType(index));
  }

  ParameterConvention getDirectParameter(unsigned index,
                            const AbstractionPattern &type,
                           const TypeLowering &substTL) const override {
    if (cast<clang::FunctionProtoType>(FnType)->isParamConsumed(index))
      return ParameterConvention::Direct_Owned;
    return getDirectCParameterConvention(getParamType(index));
  }

  ParameterConvention getPackParameter(unsigned index) const override {
    llvm_unreachable("C functions do not have pack parameters");
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
    if (tl.getLoweredType().isForeignReferenceType())
      return ResultConvention::Unowned;
    if (FnType->getReturnType()
            ->getUnqualifiedDesugaredType()
            ->getAsCXXRecordDecl()) {
      auto t = tl.getLoweredType().getASTType();
      if (auto *classDecl = t.getPointer()
                                ->lookThroughAllOptionalTypes()
                                ->getClassOrBoundGenericClass()) {
        assert(!classDecl->hasClangNode() &&
               "unexpected imported class type in C function");
        assert(!classDecl->isGeneric());
        return ResultConvention::Owned;
      }
    }

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

  ParameterConvention getPackParameter(unsigned index) const override {
    llvm_unreachable("C functions do not have pack parameters");
  }

  ResultConvention getResult(const TypeLowering &tl) const override {
    // C++ constructors return indirectly.
    // TODO: this may be different depending on the ABI, so we may have to
    // check with clang here.
    if (isa<clang::CXXConstructorDecl>(TheDecl)) {
      return ResultConvention::Indirect;
    }

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
      } else if (tl.getLoweredType().isForeignReferenceType()) {
        return ResultConvention::Unowned;
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
  bool isMutating;

public:
  CXXMethodConventions(const clang::CXXMethodDecl *decl, bool isMutating)
      : CFunctionTypeConventions(
            ConventionsKind::CXXMethod,
            decl->getType()->castAs<clang::FunctionType>()),
        TheDecl(decl), isMutating(isMutating) {}
  ParameterConvention
  getIndirectSelfParameter(const AbstractionPattern &type) const override {
    if (isMutating)
      return ParameterConvention::Indirect_Inout;
    return ParameterConvention::Indirect_In_Guaranteed;
  }

  ParameterConvention
  getIndirectParameter(unsigned int index, const AbstractionPattern &type,
                       const TypeLowering &substTL) const override {
    // `self` is the last parameter.
    if (index == TheDecl->getNumParams()) {
      return getIndirectSelfParameter(type);
    }
    return super::getIndirectParameter(index, type, substTL);
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
    llvm::Optional<SILDeclRef> constant) {
  if (auto method = dyn_cast<clang::ObjCMethodDecl>(clangDecl)) {
    auto origPattern = AbstractionPattern::getObjCMethod(
        origType, method, foreignInfo.error, foreignInfo.async);
    return getSILFunctionType(
        TC, TypeExpansionContext::minimal(), origPattern, substInterfaceType,
        extInfoBuilder, ObjCMethodConventions(method), foreignInfo, constant,
        constant, llvm::None, ProtocolConformanceRef());
  }

  if (auto method = dyn_cast<clang::CXXMethodDecl>(clangDecl)) {
    // Static methods and ctors should be lowered like plane functions
    // (case below).
    if (!isa<clang::CXXConstructorDecl>(method) || method->isStatic()) {
      AbstractionPattern origPattern = AbstractionPattern::getCXXMethod(origType, method,
                                                                        foreignInfo.self);
      bool isMutating =
          TC.Context.getClangModuleLoader()->isCXXMethodMutating(method);
      auto conventions = CXXMethodConventions(method, isMutating);
      return getSILFunctionType(TC, TypeExpansionContext::minimal(),
                                origPattern, substInterfaceType, extInfoBuilder,
                                conventions, foreignInfo, constant, constant,
                                llvm::None, ProtocolConformanceRef());
    }
  }

  if (auto func = dyn_cast<clang::FunctionDecl>(clangDecl)) {
    auto clangType = func->getType().getTypePtr();
    AbstractionPattern origPattern =
        foreignInfo.self.isImportAsMember()
            ? AbstractionPattern::getCFunctionAsMethod(origType, clangType,
                                                       foreignInfo.self)
            : AbstractionPattern(origType, clangType);
    return getSILFunctionType(TC, TypeExpansionContext::minimal(), origPattern,
                              substInterfaceType, extInfoBuilder,
                              CFunctionConventions(func), foreignInfo, constant,
                              constant, llvm::None, ProtocolConformanceRef());
  }

  llvm_unreachable("call to unknown kind of C function");
}

static CanSILFunctionType getSILFunctionTypeForAbstractCFunction(
    TypeConverter &TC, AbstractionPattern origType,
    CanAnyFunctionType substType, SILExtInfoBuilder extInfoBuilder,
    llvm::Optional<SILDeclRef> constant) {
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
          constant, constant, llvm::None, ProtocolConformanceRef());
    }
  }

  // TODO: Ought to support captures in block funcs.
  return getSILFunctionType(TC, TypeExpansionContext::minimal(), origType,
                            substType, extInfoBuilder,
                            DefaultBlockConventions(), ForeignInfo(), constant,
                            constant, llvm::None, ProtocolConformanceRef());
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
  case SILDeclRef::Kind::RuntimeAttributeGenerator:
  case SILDeclRef::Kind::StoredPropertyInitializer:
  case SILDeclRef::Kind::PropertyWrapperBackingInitializer:
  case SILDeclRef::Kind::PropertyWrapperInitFromProjectedValue:
  case SILDeclRef::Kind::EntryPoint:
  case SILDeclRef::Kind::AsyncEntryPoint:
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

  ParameterConvention getPackParameter(unsigned index) const override {
    llvm_unreachable("objc methods do not have pack parameters");
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
    const ForeignInfo &foreignInfo, llvm::Optional<SILDeclRef> constant) {
  return getSILFunctionType(
      TC, TypeExpansionContext::minimal(), AbstractionPattern(origType),
      substInterfaceType, extInfoBuilder, ObjCSelectorFamilyConventions(family),
      foreignInfo, constant, constant,
      /*requirement subs*/ llvm::None, ProtocolConformanceRef());
}

static bool isImporterGeneratedAccessor(const clang::Decl *clangDecl,
                                        SILDeclRef constant) {
  // Must be an accessor.
  auto accessor = dyn_cast<AccessorDecl>(constant.getDecl());
  if (!accessor)
    return false;

  // Must be a type member.
  if (!accessor->hasImplicitSelfDecl())
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
    
    // Does this constant have a preferred abstraction pattern set?
    AbstractionPattern origType = [&]{
      if (auto abstraction = TC.getConstantAbstractionPattern(constant)) {
        return *abstraction;
      } else {
        return AbstractionPattern(origLoweredInterfaceType);
      }
    }();

    return ::getNativeSILFunctionType(
        TC, context, origType, origLoweredInterfaceType, extInfoBuilder,
        constant, constant, llvm::None, witnessMethodConformance);
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
      if (!foreignInfo.self.isImportAsMember() &&
          isImporterGeneratedAccessor(clangDecl, constant)) {
        unsigned selfIndex = cast<AccessorDecl>(decl)->isSetter() ? 1 : 0;
        foreignInfo.self.setSelfIndex(selfIndex);
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

static bool isObjCMethod(ValueDecl *vd) {
  if (!vd->getDeclContext())
    return false;

  Type contextType = vd->getDeclContext()->getDeclaredInterfaceType();
  if (!contextType)
    return false;

  bool isRefCountedClass = contextType->getClassOrBoundGenericClass() &&
                           !contextType->isForeignReferenceType();
  return isRefCountedClass || contextType->isClassExistentialType();
}

SILFunctionTypeRepresentation
TypeConverter::getDeclRefRepresentation(SILDeclRef c) {
  // If this is a foreign thunk, it always has the foreign calling convention.
  if (c.isForeign) {
    if (!c.hasDecl())
      return SILFunctionTypeRepresentation::CFunctionPointer;

    if (auto method =
            dyn_cast_or_null<clang::CXXMethodDecl>(c.getDecl()->getClangDecl()))
      return isa<clang::CXXConstructorDecl>(method) || method->isStatic()
                 ? SILFunctionTypeRepresentation::CFunctionPointer
                 : SILFunctionTypeRepresentation::CXXMethod;


    // For example, if we have a function in a namespace:
    if (c.getDecl()->isImportAsMember())
      return SILFunctionTypeRepresentation::CFunctionPointer;

    if (isObjCMethod(c.getDecl()) ||
        c.kind == SILDeclRef::Kind::IVarInitializer ||
        c.kind == SILDeclRef::Kind::IVarDestroyer)
      return SILFunctionTypeRepresentation::ObjCMethod;

    return SILFunctionTypeRepresentation::CFunctionPointer;
  }

  // Anonymous functions currently always have Freestanding CC.
  if (c.getAbstractClosureExpr())
    return SILFunctionTypeRepresentation::Thin;

  // FIXME: Assert that there is a native entry point
  // available. There's no great way to do this.

  // Protocol witnesses are called using the witness calling convention.
  if (c.hasDecl()) {
    if (auto proto = dyn_cast<ProtocolDecl>(c.getDecl()->getDeclContext())) {
      // Use the regular method convention for foreign-to-native thunks.
      if (c.isForeignToNativeThunk())
        return SILFunctionTypeRepresentation::Method;
      assert(!c.isNativeToForeignThunk() && "shouldn't be possible");
      return getProtocolWitnessRepresentation(proto);
    }
  }

  switch (c.kind) {
    case SILDeclRef::Kind::GlobalAccessor:
    case SILDeclRef::Kind::DefaultArgGenerator:
    case SILDeclRef::Kind::RuntimeAttributeGenerator:
    case SILDeclRef::Kind::StoredPropertyInitializer:
    case SILDeclRef::Kind::PropertyWrapperBackingInitializer:
    case SILDeclRef::Kind::PropertyWrapperInitFromProjectedValue:
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

    case SILDeclRef::Kind::AsyncEntryPoint:
      return SILFunctionTypeRepresentation::Thin;
    case SILDeclRef::Kind::EntryPoint:
      return SILFunctionTypeRepresentation::CFunctionPointer;
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


  auto derivedSig = derived.getDecl()->getAsGenericContext()
                                     ->getGenericSignature();
  auto genericSig = Context.getOverrideGenericSignature(base.getDecl(),
                                                        derived.getDecl());
  // Figure out the generic signature for the class method call. This is the
  // signature of the derived class, with requirements transplanted from
  // the base method. The derived method is allowed to have fewer
  // requirements, in which case the thunk will translate the calling
  // convention appropriately before calling the derived method.
  bool hasGenericRequirementDifference =
      !genericSig.requirementsNotSatisfiedBy(derivedSig).empty();

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
      /*reqt subs*/ llvm::None, ProtocolConformanceRef());

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
  case SILDeclRef::Kind::RuntimeAttributeGenerator:
  case SILDeclRef::Kind::StoredPropertyInitializer:
  case SILDeclRef::Kind::PropertyWrapperBackingInitializer:
  case SILDeclRef::Kind::PropertyWrapperInitFromProjectedValue:
  case SILDeclRef::Kind::IVarInitializer:
  case SILDeclRef::Kind::IVarDestroyer:
  case SILDeclRef::Kind::EntryPoint:
  case SILDeclRef::Kind::AsyncEntryPoint:
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
        return AbstractionPattern::getCurriedCXXMethod(fnType, bridgedFn);
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

  // Distributed thunks are always `async throws`
  if (constant.isDistributedThunk()) {
    extInfo = extInfo.withAsync(true).withThrows(true);
  }

  // If this is a C++ constructor, don't add the metatype "self" parameter
  // because we'll never use it and it will cause problems in IRGen.
  if (isa_and_nonnull<clang::CXXConstructorDecl>(
          constant.getDecl()->getClangDecl())) {
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
//
// Note: all cases recognized here must be handled in the SILOptimizer's
// castValueToABICompatibleType().
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
      auto opaqueTypesSubstituted = aa;
      auto *dc = inFunction->getDeclContext();
      auto *currentModule = inFunction->getModule().getSwiftModule();
      if (!dc || !dc->isChildContextOf(currentModule))
        dc = currentModule;
      ReplaceOpaqueTypesWithUnderlyingTypes replacer(
          dc, inFunction->getResilienceExpansion(),
          inFunction->getModule().isWholeModule());
      if (aa.getASTType()->hasOpaqueArchetype())
        opaqueTypesSubstituted = aa.subst(inFunction->getModule(), replacer,
                                         replacer, CanGenericSignature(),
                                         SubstFlags::SubstituteOpaqueArchetypes);

      auto opaqueTypesSubstituted2 = bb;
      if (bb.getASTType()->hasOpaqueArchetype())
        opaqueTypesSubstituted2 =
            bb.subst(inFunction->getModule(), replacer, replacer,
                     CanGenericSignature(),
                     SubstFlags::SubstituteOpaqueArchetypes);
      if (opaqueTypesSubstituted == opaqueTypesSubstituted2)
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

  // `() async -> ()` is not compatible with `() async -> @error Error` and
  // vice versa.
  if (hasErrorResult() != other->hasErrorResult() && isAsync()) {
    return ABICompatibilityCheckResult::DifferentErrorResultConventions;
  }

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
