//===--- SILGenBackDeploy.cpp - SILGen for back deployment ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILGenFunction.h"
#include "SILGenFunctionBuilder.h"
#include "Scope.h"
#include "swift/SIL/SILDeclRef.h"

using namespace swift;
using namespace Lowering;

/// Given a value, extracts all elements to `result` from this value if it's a
/// tuple. Otherwise, add this value directly to `result`.
static void extractAllElements(SILValue val, SILLocation loc,
                               SILBuilder &builder,
                               SmallVectorImpl<SILValue> &result) {
  auto &fn = builder.getFunction();
  auto tupleType = val->getType().getAs<TupleType>();
  if (!tupleType) {
    result.push_back(val);
    return;
  }
  if (!fn.hasOwnership()) {
    for (auto i : range(tupleType->getNumElements()))
      result.push_back(builder.createTupleExtract(loc, val, i));
    return;
  }
  if (tupleType->getNumElements() == 0)
    return;
  builder.emitDestructureValueOperation(loc, val, result);
}

static Type getResultInterfaceType(AbstractFunctionDecl *AFD) {
  if (auto *FD = dyn_cast<FuncDecl>(AFD))
    return FD->getResultInterfaceType();

  if (auto *CD = dyn_cast<ConstructorDecl>(AFD))
    return CD->getResultInterfaceType();

  llvm_unreachable("Unhandled AbstractFunctionDecl type");
}

/// Emit the following branch SIL instruction:
/// \verbatim
/// if #available(OSVersion) {
///   <availableBB>
/// } else {
///   <unavailableBB>
/// }
/// \endverbatim
static void emitBackDeployIfAvailableCondition(SILGenFunction &SGF,
                                               AbstractFunctionDecl *AFD,
                                               SILLocation loc,
                                               SILBasicBlock *availableBB,
                                               SILBasicBlock *unavailableBB) {
  auto version = AFD->getBackDeployedBeforeOSVersion(SGF.SGM.getASTContext());
  VersionRange OSVersion = VersionRange::empty();
  if (version.has_value()) {
    OSVersion = VersionRange::allGTE(*version);
  }

  SILValue booleanTestValue;
  if (OSVersion.isEmpty() || OSVersion.isAll()) {
    // If there's no check for the current platform, this condition is
    // trivially true.
    SILType i1 = SILType::getBuiltinIntegerType(1, SGF.getASTContext());
    booleanTestValue = SGF.B.createIntegerLiteral(loc, i1, 1);
  } else {
    booleanTestValue = SGF.emitOSVersionRangeCheck(loc, OSVersion);
  }

  SGF.B.createCondBranch(loc, booleanTestValue, availableBB, unavailableBB);
}

/// Emits a function or method application, forwarding parameters.
static void emitBackDeployForwardApplyAndReturnOrThrow(
    SILGenFunction &SGF, AbstractFunctionDecl *AFD, SILLocation loc,
    SILDeclRef function, SmallVector<SILValue, 8> &params) {
  // Only statically dispatched class methods are supported.
  if (auto classDecl = dyn_cast<ClassDecl>(AFD->getDeclContext())) {
    assert(classDecl->isFinal() || AFD->isFinal() ||
           AFD->hasForcedStaticDispatch());
  }

  TypeExpansionContext TEC = SGF.getTypeExpansionContext();
  auto fnType = SGF.SGM.Types.getConstantOverrideType(TEC, function);
  auto silFnType =
      SILType::getPrimitiveObjectType(fnType).castTo<SILFunctionType>();
  SILFunctionConventions fnConv(silFnType, SGF.SGM.M);

  SILValue functionRef = SGF.emitGlobalFunctionRef(loc, function);
  auto subs = SGF.F.getForwardingSubstitutionMap();
  SmallVector<SILValue, 4> directResults;

  // If the function is a coroutine, we need to use 'begin_apply'.
  if (silFnType->isCoroutine()) {
    assert(!silFnType->hasErrorResult() && "throwing coroutine?");

    // Apply the coroutine, yield the result, and finally branch to either the
    // terminal return or unwind basic block via intermediate basic blocks. The
    // intermediates are needed to avoid forming critical edges.
    SILBasicBlock *resumeBB = SGF.createBasicBlock();
    SILBasicBlock *unwindBB = SGF.createBasicBlock();

    auto *apply = SGF.B.createBeginApply(loc, functionRef, subs, params);
    SmallVector<SILValue, 4> rawResults;
    for (auto result : apply->getAllResults())
      rawResults.push_back(result);

    auto token = rawResults.pop_back_val();
    SGF.B.createYield(loc, rawResults, resumeBB, unwindBB);

    // Emit resume block.
    SGF.B.emitBlock(resumeBB);
    SGF.B.createEndApply(loc, token,
                         SILType::getEmptyTupleType(SGF.getASTContext()));
    SGF.B.createBranch(loc, SGF.ReturnDest.getBlock());

    // Emit unwind block.
    SGF.B.emitBlock(unwindBB);
    SGF.B.createEndApply(loc, token,
                         SILType::getEmptyTupleType(SGF.getASTContext()));
    SGF.B.createBranch(loc, SGF.CoroutineUnwindDest.getBlock());
    return;
  }

  // Use try_apply for functions that throw.
  if (silFnType->hasErrorResult()) {
    // Apply the throwing function and forward the results and the error to the
    // return/throw blocks via intermediate basic blocks. The intermediates
    // are needed to avoid forming critical edges.
    SILBasicBlock *normalBB = SGF.createBasicBlock();
    SILBasicBlock *errorBB = SGF.createBasicBlock();

    SGF.B.createTryApply(loc, functionRef, subs, params, normalBB, errorBB);

    // Emit error block.
    SGF.B.emitBlock(errorBB);
    ManagedValue error =
        SGF.B.createPhi(SGF.F.mapTypeIntoContext(fnConv.getSILErrorType(TEC)),
                        OwnershipKind::Owned);
    SGF.B.createBranch(loc, SGF.ThrowDest.getBlock(), {error});

    // Emit normal block.
    SGF.B.emitBlock(normalBB);
    SILValue result = normalBB->createPhiArgument(
        SGF.F.mapTypeIntoContext(fnConv.getSILResultType(TEC)),
        OwnershipKind::Owned);
    SmallVector<SILValue, 4> directResults;
    extractAllElements(result, loc, SGF.B, directResults);

    SGF.B.createBranch(loc, SGF.ReturnDest.getBlock(), directResults);
    return;
  }

  // The original function is neither throwing nor a coroutine. Apply it and
  // forward its results straight to the return block.
  auto *apply = SGF.B.createApply(loc, functionRef, subs, params);
  extractAllElements(apply, loc, SGF.B, directResults);

  SGF.B.createBranch(loc, SGF.ReturnDest.getBlock(), directResults);
}

bool SILGenModule::requiresBackDeploymentThunk(ValueDecl *decl,
                                               ResilienceExpansion expansion) {
  auto &ctx = getASTContext();
  auto backDeployBeforeVersion = decl->getBackDeployedBeforeOSVersion(ctx);
  if (!backDeployBeforeVersion)
    return false;

  switch (expansion) {
  case ResilienceExpansion::Minimal:
    // In a minimal resilience expansion we must always call the back deployment
    // thunk since we can't predict the deployment targets of the modules that
    // might inline the call.
    return true;
  case ResilienceExpansion::Maximal:
    // FIXME: We can skip thunking if we're in the same module.
    break;
  }

  // Use of a back deployment thunk is unnecessary if the deployment target is
  // high enough that the ABI implementation of the back deployed declaration is
  // guaranteed to be available.
  auto deploymentAvailability = AvailabilityContext::forDeploymentTarget(ctx);
  auto declAvailability =
      AvailabilityContext(VersionRange::allGTE(*backDeployBeforeVersion));
  if (deploymentAvailability.isContainedIn(declAvailability))
    return false;

  return true;
}

void SILGenFunction::emitBackDeploymentThunk(SILDeclRef thunk) {
  // Generate code equivalent to:
  //
  //  func X_thunk(...) async throws -> ... {
  //    if #available(...) {
  //      return try await X(...)
  //    } else {
  //      return try await X_fallback(...)
  //    }
  //  }

  assert(thunk.isBackDeploymentThunk());

  auto loc = thunk.getAsRegularLocation();
  loc.markAutoGenerated();
  Scope scope(Cleanups, CleanupLocation(loc));
  auto AFD = cast<AbstractFunctionDecl>(thunk.getDecl());

  F.setGenericEnvironment(SGM.Types.getConstantGenericEnvironment(thunk));

  // Generate the thunk prolog by collecting parameters.
  SmallVector<ManagedValue, 4> params;
  SmallVector<ManagedValue, 4> indirectParams;
  SmallVector<ManagedValue, 4> indirectErrorResults;
  collectThunkParams(loc, params, &indirectParams, &indirectErrorResults);

  // Build up the list of arguments that we're going to invoke the the real
  // function with.
  SmallVector<SILValue, 8> paramsForForwarding;
  for (auto indirectParam : indirectParams) {
    paramsForForwarding.emplace_back(indirectParam.getLValueAddress());
  }
  for (auto indirectErrorResult : indirectErrorResults) {
    paramsForForwarding.emplace_back(indirectErrorResult.getLValueAddress());
  }

  for (auto param : params) {
    // We're going to directly call either the original function or the fallback
    // function with these arguments and then return. Therefore we just forward
    // the arguments instead of handling their ownership conventions.
    paramsForForwarding.emplace_back(param.forward(*this));
  }

  prepareEpilog(AFD,
                getResultInterfaceType(AFD),
                AFD->getEffectiveThrownErrorType(),
                CleanupLocation(AFD));

  SILBasicBlock *availableBB = createBasicBlock("availableBB");
  SILBasicBlock *unavailableBB = createBasicBlock("unavailableBB");

  //  if #available(...) {
  //    <availableBB>
  //  } else {
  //    <unavailableBB>
  //  }
  emitBackDeployIfAvailableCondition(*this, AFD, loc, availableBB,
                                     unavailableBB);

  // <availableBB>:
  //   return (try)? (await)? (self.)?X(...)
  {
    B.emitBlock(availableBB);
    SILDeclRef original =
        thunk.asBackDeploymentKind(SILDeclRef::BackDeploymentKind::None);
    emitBackDeployForwardApplyAndReturnOrThrow(*this, AFD, loc, original,
                                               paramsForForwarding);
  }

  // <unavailableBB>:
  //   return (try)? (await)? (self.)?X_fallback(...)
  {
    B.emitBlock(unavailableBB);
    SILDeclRef fallback =
        thunk.asBackDeploymentKind(SILDeclRef::BackDeploymentKind::Fallback);
    emitBackDeployForwardApplyAndReturnOrThrow(*this, AFD, loc, fallback,
                                               paramsForForwarding);
  }

  emitEpilog(AFD);
}
