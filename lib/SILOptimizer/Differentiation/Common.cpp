//===--- Common.cpp - Automatic differentiation common utils --*- C++ -*---===//
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
// Automatic differentiation common utilities.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "differentiation"

#include "swift/SILOptimizer/Differentiation/Common.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/SILOptimizer/Differentiation/ADContext.h"

namespace swift {
namespace autodiff {

raw_ostream &getADDebugStream() { return llvm::dbgs() << "[AD] "; }

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

ApplyInst *getAllocateUninitializedArrayIntrinsicElementAddress(SILValue v) {
  // Find the `pointer_to_address` result, peering through `index_addr`.
  auto *ptai = dyn_cast<PointerToAddressInst>(v);
  if (auto *iai = dyn_cast<IndexAddrInst>(v))
    ptai = dyn_cast<PointerToAddressInst>(iai->getOperand(0));
  if (!ptai)
    return nullptr;
  // Return the `array.uninitialized_intrinsic` application, if it exists.
  if (auto *dti = dyn_cast<DestructureTupleInst>(
          ptai->getOperand()->getDefiningInstruction()))
    return ArraySemanticsCall(dti->getOperand(),
                              semantics::ARRAY_UNINITIALIZED_INTRINSIC);
  return nullptr;
}

DestructureTupleInst *getSingleDestructureTupleUser(SILValue value) {
  bool foundDestructureTupleUser = false;
  if (!value->getType().is<TupleType>())
    return nullptr;
  DestructureTupleInst *result = nullptr;
  for (auto *use : value->getUses()) {
    if (auto *dti = dyn_cast<DestructureTupleInst>(use->getUser())) {
      assert(!foundDestructureTupleUser &&
             "There should only be one `destructure_tuple` user of a tuple");
      foundDestructureTupleUser = true;
      result = dti;
    }
  }
  return result;
}

bool isSemanticMemberAccessor(SILFunction *original) {
  auto *dc = original->getDeclContext();
  if (!dc)
    return false;
  auto *decl = dc->getAsDecl();
  if (!decl)
    return false;
  auto *accessor = dyn_cast<AccessorDecl>(decl);
  if (!accessor)
    return false;
  // Currently, only getters and setters are supported.
  // TODO(https://github.com/apple/swift/issues/55084): Support `modify` accessors.
  if (accessor->getAccessorKind() != AccessorKind::Get &&
      accessor->getAccessorKind() != AccessorKind::Set)
    return false;
  // Accessor must come from a `var` declaration.
  auto *varDecl = dyn_cast<VarDecl>(accessor->getStorage());
  if (!varDecl)
    return false;
  // Return true for stored property accessors.
  if (varDecl->hasStorage() && varDecl->isInstanceMember())
    return true;
  // Return true for properties that have attached property wrappers.
  if (varDecl->hasAttachedPropertyWrapper())
    return true;
  // Otherwise, return false.
  // User-defined accessors can never be supported because they may use custom
  // logic that does not semantically perform a member access.
  return false;
}

bool hasSemanticMemberAccessorCallee(ApplySite applySite) {
  if (auto *FRI = dyn_cast<FunctionRefBaseInst>(applySite.getCallee()))
    if (auto *F = FRI->getReferencedFunctionOrNull())
      return isSemanticMemberAccessor(F);
  return false;
}

void forEachApplyDirectResult(
    FullApplySite applySite,
    llvm::function_ref<void(SILValue)> resultCallback) {
  switch (applySite.getKind()) {
  case FullApplySiteKind::ApplyInst: {
    auto *ai = cast<ApplyInst>(applySite.getInstruction());
    if (!ai->getType().is<TupleType>()) {
      resultCallback(ai);
      return;
    }
    if (auto *dti = getSingleDestructureTupleUser(ai))
      for (auto directResult : dti->getResults())
        resultCallback(directResult);
    break;
  }
  case FullApplySiteKind::BeginApplyInst: {
    auto *bai = cast<BeginApplyInst>(applySite.getInstruction());
    for (auto directResult : bai->getResults())
      resultCallback(directResult);
    break;
  }
  case FullApplySiteKind::TryApplyInst: {
    auto *tai = cast<TryApplyInst>(applySite.getInstruction());
    for (auto *succBB : tai->getSuccessorBlocks())
      for (auto *arg : succBB->getArguments())
        resultCallback(arg);
    break;
  }
  }
}

void collectAllFormalResultsInTypeOrder(SILFunction &function,
                                        SmallVectorImpl<SILValue> &results) {
  SILFunctionConventions convs(function.getLoweredFunctionType(),
                               function.getModule());
  auto indResults = function.getIndirectResults();
  auto *retInst = cast<ReturnInst>(function.findReturnBB()->getTerminator());
  auto retVal = retInst->getOperand();
  SmallVector<SILValue, 8> dirResults;
  if (auto *tupleInst =
          dyn_cast_or_null<TupleInst>(retVal->getDefiningInstruction()))
    dirResults.append(tupleInst->getElements().begin(),
                      tupleInst->getElements().end());
  else
    dirResults.push_back(retVal);
  unsigned indResIdx = 0, dirResIdx = 0;
  for (auto &resInfo : convs.getResults())
    results.push_back(resInfo.isFormalDirect() ? dirResults[dirResIdx++]
                                               : indResults[indResIdx++]);
  // Treat `inout` parameters as semantic results.
  // Append `inout` parameters after formal results.
  for (auto i : range(convs.getNumParameters())) {
    auto paramInfo = convs.getParameters()[i];
    if (!paramInfo.isIndirectMutating())
      continue;
    auto *argument = function.getArgumentsWithoutIndirectResults()[i];
    results.push_back(argument);
  }
}

void collectAllDirectResultsInTypeOrder(SILFunction &function,
                                        SmallVectorImpl<SILValue> &results) {
  SILFunctionConventions convs(function.getLoweredFunctionType(),
                               function.getModule());
  auto *retInst = cast<ReturnInst>(function.findReturnBB()->getTerminator());
  auto retVal = retInst->getOperand();
  if (auto *tupleInst = dyn_cast<TupleInst>(retVal))
    results.append(tupleInst->getElements().begin(),
                   tupleInst->getElements().end());
  else
    results.push_back(retVal);
}

void collectAllActualResultsInTypeOrder(
    ApplyInst *ai, ArrayRef<SILValue> extractedDirectResults,
    SmallVectorImpl<SILValue> &results) {
  auto calleeConvs = ai->getSubstCalleeConv();
  unsigned indResIdx = 0, dirResIdx = 0;
  for (auto &resInfo : calleeConvs.getResults()) {
    results.push_back(resInfo.isFormalDirect()
                          ? extractedDirectResults[dirResIdx++]
                          : ai->getIndirectSILResults()[indResIdx++]);
  }
}

void collectMinimalIndicesForFunctionCall(
    ApplyInst *ai, const AutoDiffConfig &parentConfig,
    const DifferentiableActivityInfo &activityInfo,
    SmallVectorImpl<SILValue> &results, SmallVectorImpl<unsigned> &paramIndices,
    SmallVectorImpl<unsigned> &resultIndices) {
  auto calleeFnTy = ai->getSubstCalleeType();
  auto calleeConvs = ai->getSubstCalleeConv();
  // Parameter indices are indices (in the callee type signature) of parameter
  // arguments that are varied or are arguments.
  // Record all parameter indices in type order.
  unsigned currentParamIdx = 0;
  for (auto applyArg : ai->getArgumentsWithoutIndirectResults()) {
    if (activityInfo.isActive(applyArg, parentConfig))
      paramIndices.push_back(currentParamIdx);
    ++currentParamIdx;
  }
  // Result indices are indices (in the callee type signature) of results that
  // are useful.
  SmallVector<SILValue, 8> directResults;
  forEachApplyDirectResult(ai, [&](SILValue directResult) {
    directResults.push_back(directResult);
  });
  auto indirectResults = ai->getIndirectSILResults();
  // Record all results and result indices in type order.
  results.reserve(calleeFnTy->getNumResults());
  unsigned dirResIdx = 0;
  unsigned indResIdx = calleeConvs.getSILArgIndexOfFirstIndirectResult();
  for (auto &resAndIdx : enumerate(calleeConvs.getResults())) {
    auto &res = resAndIdx.value();
    unsigned idx = resAndIdx.index();
    if (res.isFormalDirect()) {
      results.push_back(directResults[dirResIdx]);
      if (auto dirRes = directResults[dirResIdx])
        if (dirRes && activityInfo.isActive(dirRes, parentConfig))
          resultIndices.push_back(idx);
      ++dirResIdx;
    } else {
      results.push_back(indirectResults[indResIdx]);
      if (activityInfo.isActive(indirectResults[indResIdx], parentConfig))
        resultIndices.push_back(idx);
      ++indResIdx;
    }
  }
  // Record all `inout` parameters as results.
  auto inoutParamResultIndex = calleeFnTy->getNumResults();
  for (auto &paramAndIdx : enumerate(calleeConvs.getParameters())) {
    auto &param = paramAndIdx.value();
    if (!param.isIndirectMutating())
      continue;
    unsigned idx = paramAndIdx.index() + calleeFnTy->getNumIndirectFormalResults();
    auto inoutArg = ai->getArgument(idx);
    results.push_back(inoutArg);
    resultIndices.push_back(inoutParamResultIndex++);
  }
  // Make sure the function call has active results.
#ifndef NDEBUG
  auto numResults = calleeFnTy->getNumResults() +
                    calleeFnTy->getNumIndirectMutatingParameters();
  assert(results.size() == numResults);
  assert(llvm::any_of(results, [&](SILValue result) {
    return activityInfo.isActive(result, parentConfig);
  }));
#endif
}

llvm::Optional<std::pair<SILDebugLocation, SILDebugVariable>>
findDebugLocationAndVariable(SILValue originalValue) {
  if (auto *asi = dyn_cast<AllocStackInst>(originalValue))
    return asi->getVarInfo().transform([&](SILDebugVariable var) {
      return std::make_pair(asi->getDebugLocation(), var);
    });
  for (auto *use : originalValue->getUses()) {
    if (auto *dvi = dyn_cast<DebugValueInst>(use->getUser()))
      return dvi->getVarInfo().transform([&](SILDebugVariable var) {
        // We need to drop `op_deref` here as we're transferring debug info
        // location from debug_value instruction (which describes how to get value)
        // into alloc_stack (which describes the location)
        if (var.DIExpr.startsWithDeref())
          var.DIExpr.eraseElement(var.DIExpr.element_begin());
        return std::make_pair(dvi->getDebugLocation(), var);
      });
  }
  return llvm::None;
}

//===----------------------------------------------------------------------===//
// Diagnostic utilities
//===----------------------------------------------------------------------===//

SILLocation getValidLocation(SILValue v) {
  auto loc = v.getLoc();
  if (loc.isNull() || loc.getSourceLoc().isInvalid())
    loc = v->getFunction()->getLocation();
  return loc;
}

SILLocation getValidLocation(SILInstruction *inst) {
  auto loc = inst->getLoc();
  if (loc.isNull() || loc.getSourceLoc().isInvalid())
    loc = inst->getFunction()->getLocation();
  return loc;
}

//===----------------------------------------------------------------------===//
// Tangent property lookup utilities
//===----------------------------------------------------------------------===//

VarDecl *getTangentStoredProperty(ADContext &context, VarDecl *originalField,
                                  CanType baseType, SILLocation loc,
                                  DifferentiationInvoker invoker) {
  auto &astCtx = context.getASTContext();
  auto tanFieldInfo = evaluateOrDefault(
      astCtx.evaluator, TangentStoredPropertyRequest{originalField, baseType},
      TangentPropertyInfo(nullptr));
  // If no error, return the tangent property.
  if (tanFieldInfo)
    return tanFieldInfo.tangentProperty;
  // Otherwise, diagnose error and return nullptr.
  assert(tanFieldInfo.error);
  auto *parentDC = originalField->getDeclContext();
  assert(parentDC->isTypeContext());
  auto parentDeclName = parentDC->getSelfNominalTypeDecl()->getNameStr();
  auto fieldName = originalField->getNameStr();
  auto sourceLoc = loc.getSourceLoc();
  switch (tanFieldInfo.error->kind) {
  case TangentPropertyInfo::Error::Kind::NoDerivativeOriginalProperty:
    llvm_unreachable(
        "`@noDerivative` stored property accesses should not be "
        "differentiated; activity analysis should not mark as varied");
  case TangentPropertyInfo::Error::Kind::NominalParentNotDifferentiable:
    context.emitNondifferentiabilityError(
        sourceLoc, invoker,
        diag::autodiff_stored_property_parent_not_differentiable,
        parentDeclName, fieldName);
    break;
  case TangentPropertyInfo::Error::Kind::OriginalPropertyNotDifferentiable:
    context.emitNondifferentiabilityError(
        sourceLoc, invoker, diag::autodiff_stored_property_not_differentiable,
        parentDeclName, fieldName, originalField->getInterfaceType());
    break;
  case TangentPropertyInfo::Error::Kind::ParentTangentVectorNotStruct:
    context.emitNondifferentiabilityError(
        sourceLoc, invoker, diag::autodiff_stored_property_tangent_not_struct,
        parentDeclName, fieldName);
    break;
  case TangentPropertyInfo::Error::Kind::TangentPropertyNotFound:
    context.emitNondifferentiabilityError(
        sourceLoc, invoker,
        diag::autodiff_stored_property_no_corresponding_tangent, parentDeclName,
        fieldName);
    break;
  case TangentPropertyInfo::Error::Kind::TangentPropertyWrongType:
    context.emitNondifferentiabilityError(
        sourceLoc, invoker, diag::autodiff_tangent_property_wrong_type,
        parentDeclName, fieldName, tanFieldInfo.error->getType());
    break;
  case TangentPropertyInfo::Error::Kind::TangentPropertyNotStored:
    context.emitNondifferentiabilityError(
        sourceLoc, invoker, diag::autodiff_tangent_property_not_stored,
        parentDeclName, fieldName);
    break;
  }
  return nullptr;
}

VarDecl *getTangentStoredProperty(ADContext &context,
                                  SingleValueInstruction *projectionInst,
                                  CanType baseType,
                                  DifferentiationInvoker invoker) {
  assert(isa<StructExtractInst>(projectionInst) ||
         isa<StructElementAddrInst>(projectionInst) ||
         isa<RefElementAddrInst>(projectionInst));
  Projection proj(projectionInst);
  auto loc = getValidLocation(projectionInst);
  auto *field = proj.getVarDecl(projectionInst->getOperand(0)->getType());
  return getTangentStoredProperty(context, field, baseType,
                                  loc, invoker);
}

//===----------------------------------------------------------------------===//
// Code emission utilities
//===----------------------------------------------------------------------===//

SILValue joinElements(ArrayRef<SILValue> elements, SILBuilder &builder,
                      SILLocation loc) {
  if (elements.size() == 1)
    return elements.front();
  return builder.createTuple(loc, elements);
}

void extractAllElements(SILValue value, SILBuilder &builder,
                        SmallVectorImpl<SILValue> &results) {
  auto tupleType = value->getType().getAs<TupleType>();
  if (!tupleType) {
    results.push_back(value);
    return;
  }
  if (builder.hasOwnership()) {
    auto *dti = builder.createDestructureTuple(value.getLoc(), value);
    results.append(dti->getResults().begin(), dti->getResults().end());
    return;
  }
  for (auto i : range(tupleType->getNumElements()))
    results.push_back(builder.createTupleExtract(value.getLoc(), value, i));
}

SILValue emitMemoryLayoutSize(
    SILBuilder &builder, SILLocation loc, CanType type) {
  auto &ctx = builder.getASTContext();
  auto id = ctx.getIdentifier(getBuiltinName(BuiltinValueKind::Sizeof));
  auto *builtin = cast<FuncDecl>(getBuiltinValueDecl(ctx, id));
  auto metatypeTy = SILType::getPrimitiveObjectType(
      CanMetatypeType::get(type, MetatypeRepresentation::Thin));
  auto metatypeVal = builder.createMetatype(loc, metatypeTy);
  return builder.createBuiltin(
      loc, id, SILType::getBuiltinWordType(ctx),
      SubstitutionMap::get(
          builtin->getGenericSignature(), ArrayRef<Type>{type}, {}),
      {metatypeVal});
}

SILValue emitProjectTopLevelSubcontext(
    SILBuilder &builder, SILLocation loc, SILValue context,
    SILType subcontextType) {
  assert(context->getOwnershipKind() == OwnershipKind::Guaranteed);
  auto &ctx = builder.getASTContext();
  auto id = ctx.getIdentifier(
      getBuiltinName(BuiltinValueKind::AutoDiffProjectTopLevelSubcontext));
  assert(context->getType() == SILType::getNativeObjectType(ctx));
  auto *subcontextAddr = builder.createBuiltin(
      loc, id, SILType::getRawPointerType(ctx), SubstitutionMap(), {context});
  return builder.createPointerToAddress(
      loc, subcontextAddr, subcontextType.getAddressType(), /*isStrict*/ true);
}

//===----------------------------------------------------------------------===//
// Utilities for looking up derivatives of functions
//===----------------------------------------------------------------------===//

/// Returns the AbstractFunctionDecl corresponding to `F`. If there isn't one,
/// returns `nullptr`.
static AbstractFunctionDecl *findAbstractFunctionDecl(SILFunction *F) {
  auto *DC = F->getDeclContext();
  if (!DC)
    return nullptr;
  auto *D = DC->getAsDecl();
  if (!D)
    return nullptr;
  return dyn_cast<AbstractFunctionDecl>(D);
}

SILDifferentiabilityWitness *
getExactDifferentiabilityWitness(SILModule &module, SILFunction *original,
                                 IndexSubset *parameterIndices,
                                 IndexSubset *resultIndices) {
  for (auto *w : module.lookUpDifferentiabilityWitnessesForFunction(
           original->getName())) {
    if (w->getParameterIndices() == parameterIndices &&
        w->getResultIndices() == resultIndices)
      return w;
  }
  return nullptr;
}

llvm::Optional<AutoDiffConfig>
findMinimalDerivativeConfiguration(AbstractFunctionDecl *original,
                                   IndexSubset *parameterIndices,
                                   IndexSubset *&minimalASTParameterIndices) {
  llvm::Optional<AutoDiffConfig> minimalConfig = llvm::None;
  auto configs = original->getDerivativeFunctionConfigurations();
  for (auto &config : configs) {
    auto *silParameterIndices = autodiff::getLoweredParameterIndices(
        config.parameterIndices,
        original->getInterfaceType()->castTo<AnyFunctionType>());

    if (silParameterIndices->getCapacity() < parameterIndices->getCapacity()) {
      assert(original->getCaptureInfo().hasLocalCaptures());
      silParameterIndices =
        silParameterIndices->extendingCapacity(original->getASTContext(),
                                               parameterIndices->getCapacity());
    }

    // If all indices in `parameterIndices` are in `daParameterIndices`, and
    // it has fewer indices than our current candidate and a primitive VJP,
    // then `attr` is our new candidate.
    //
    // NOTE(TF-642): `attr` may come from a un-partial-applied function and
    // have larger capacity than the desired indices. We expect this logic to
    // go away when `partial_apply` supports `@differentiable` callees.
    if (silParameterIndices->isSupersetOf(parameterIndices->extendingCapacity(
            original->getASTContext(), silParameterIndices->getCapacity())) &&
        // fewer parameters than before
        (!minimalConfig ||
         silParameterIndices->getNumIndices() <
             minimalConfig->parameterIndices->getNumIndices())) {
      minimalASTParameterIndices = config.parameterIndices;
      minimalConfig =
          AutoDiffConfig(silParameterIndices, config.resultIndices,
                         autodiff::getDifferentiabilityWitnessGenericSignature(
                             original->getGenericSignature(),
                             config.derivativeGenericSignature));
    }
  }
  return minimalConfig;
}

SILDifferentiabilityWitness *getOrCreateMinimalASTDifferentiabilityWitness(
    SILModule &module, SILFunction *original, DifferentiabilityKind kind,
    IndexSubset *parameterIndices, IndexSubset *resultIndices) {
  // Explicit differentiability witnesses only exist on SIL functions that come
  // from AST functions.
  auto *originalAFD = findAbstractFunctionDecl(original);
  if (!originalAFD)
    return nullptr;

  IndexSubset *minimalASTParameterIndices = nullptr;
  auto minimalConfig = findMinimalDerivativeConfiguration(
      originalAFD, parameterIndices, minimalASTParameterIndices);
  if (!minimalConfig)
    return nullptr;

  std::string originalName = original->getName().str();
  // If original function requires a foreign entry point, use the foreign SIL
  // function to get or create the minimal differentiability witness.
  if (requiresForeignEntryPoint(originalAFD)) {
    originalName = SILDeclRef(originalAFD).asForeign().mangle();
    original = module.lookUpFunction(SILDeclRef(originalAFD).asForeign());
  }

  auto *existingWitness = module.lookUpDifferentiabilityWitness(
      {originalName, kind, *minimalConfig});
  if (existingWitness)
    return existingWitness;

  assert(original->isExternalDeclaration() &&
         "SILGen should create differentiability witnesses for all function "
         "definitions with explicit differentiable attributes");

  return SILDifferentiabilityWitness::createDeclaration(
      module, SILLinkage::PublicExternal, original, kind,
      minimalConfig->parameterIndices, minimalConfig->resultIndices,
      minimalConfig->derivativeGenericSignature);
}

} // end namespace autodiff
} // end namespace swift
