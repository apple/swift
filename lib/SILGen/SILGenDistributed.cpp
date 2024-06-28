//===--- SILGenDistributed.cpp - SILGen for distributed -------------------===//
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

#include "ArgumentSource.h"
#include "Conversion.h"
#include "ExecutorBreadcrumb.h"
#include "Initialization.h"
#include "LValue.h"
#include "RValue.h"
#include "SILGenFunction.h"
#include "SILGenFunctionBuilder.h"
#include "Scope.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/DistributedDecl.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/SILOptimizer/Utils/DistributedActor.h"

using namespace swift;
using namespace Lowering;

// MARK: utility functions

/// Emit a reference to a specific stored property of the actor.
static SILValue emitActorPropertyReference(
    SILGenFunction &SGF, SILLocation loc, SILValue actorSelf,
    VarDecl *property) {
  assert(property);
  Type formalType = SGF.F.mapTypeIntoContext(property->getInterfaceType());
  SILType loweredType = SGF.getLoweredType(formalType).getAddressType();
  return SGF.B.createRefElementAddr(loc, actorSelf, property, loweredType);
}

/// Perform an initializing store to the given property using the value
/// \param actorSelf the value representing `self` for the actor instance.
/// \param prop the property to be initialized.
/// \param value the value to use when initializing the property.
static void initializeProperty(SILGenFunction &SGF, SILLocation loc,
                               SILValue actorSelf,
                               VarDecl* prop, SILValue value,
                               IsTake_t isTake) {
  Type formalType = SGF.F.mapTypeIntoContext(prop->getInterfaceType());
  SILType loweredType = SGF.getLoweredType(formalType);

  auto fieldAddr = emitActorPropertyReference(SGF, loc, actorSelf, prop);

  if (loweredType.isAddressOnly(SGF.F)) {
    SGF.B.createCopyAddr(loc, value, fieldAddr, isTake, IsInitialization);
  } else {
    if (value->getType().isAddress()) {
      SGF.emitSemanticLoadInto(loc, value, SGF.F.getTypeLowering(value->getType()),
          fieldAddr, SGF.getTypeLowering(loweredType), isTake, IsInitialization);
    } else {
      value = SGF.B.emitCopyValueOperation(loc, value);
      SGF.B.emitStoreValueOperation(
        loc, value, fieldAddr, StoreOwnershipQualifier::Init);
    }
  }
}

/******************************************************************************/
/******************* COMMON (DISTRIBUTED) SIL PATTERNS ************************/
/******************************************************************************/

/// Emit the following branch SIL instruction:
/// \verbatim
/// if __isRemoteActor(self) {
///   <isRemoteBB>
/// } else {
///   <isLocalBB>
/// }
/// \endverbatim
void SILGenFunction::emitDistributedIfRemoteBranch(SILLocation Loc,
                                                   ManagedValue selfValue,
                                                   Type selfTy,
                                                   SILBasicBlock *isRemoteBB,
                                                   SILBasicBlock *isLocalBB) {
  ASTContext &ctx = getASTContext();

  FuncDecl *isRemoteFn = ctx.getIsRemoteDistributedActor();
  assert(isRemoteFn && "Could not find 'is remote' function, is the "
                       "'Distributed' module available?");

  auto conformances = SGM.M.getSwiftModule()->collectExistentialConformances(
      selfTy->getCanonicalType(), ctx.getAnyObjectType());

  ManagedValue selfAnyObject = B.createInitExistentialRef(
      Loc,
      /*existentialType=*/getLoweredType(ctx.getAnyObjectType()),
      /*formalConcreteType=*/selfTy->getCanonicalType(),
      selfValue, conformances);
  auto result = emitApplyOfLibraryIntrinsic(
      Loc, isRemoteFn, SubstitutionMap(), {selfAnyObject}, SGFContext());

  SILValue isRemoteResult = std::move(result).forwardAsSingleValue(*this, Loc);
  SILValue isRemoteResultUnwrapped =
      emitUnwrapIntegerResult(Loc, isRemoteResult);

  B.createCondBranch(Loc, isRemoteResultUnwrapped, isRemoteBB, isLocalBB);
}

// ==== ------------------------------------------------------------------------
// MARK: local instance initialization

static SILArgument *findFirstDistributedActorSystemArg(SILFunction &F) {
  auto *module = F.getModule().getSwiftModule();
  auto &C = F.getASTContext();

  auto *DAS = C.getDistributedActorSystemDecl();
  Type systemTy = DAS->getDeclaredInterfaceType();

  for (auto arg : F.getArguments()) {
    // TODO(distributed): also be able to locate a generic system
    Type argTy = arg->getType().getASTType();
    auto argDecl = arg->getDecl();

    auto conformsToSystem =
        module->lookupConformance(argDecl->getInterfaceType(), DAS);

    // Is it a protocol that conforms to DistributedActorSystem?
    if (argTy->isEqual(systemTy) || conformsToSystem) {
      return arg;
    }

    // Is it some specific DistributedActorSystem?
    auto result = module->lookupConformance(argTy, DAS);
    if (!result.isInvalid()) {
      return arg;
    }
  }

#ifndef NDEBUG
  llvm_unreachable("Missing required DistributedActorSystem argument!");
#endif

  return nullptr;
}

/// For the initialization of a local distributed actor instance, emits code to
/// initialize the instance's stored property corresponding to the system.
static void emitActorSystemInit(SILGenFunction &SGF,
                                ConstructorDecl *ctor,
                                SILLocation loc,
                                ManagedValue actorSelf,
                                SILValue systemValue) {
  assert(ctor->isImplicit() && "unexpected explicit dist actor init");
  assert(ctor->isDesignatedInit());

  auto *dc = ctor->getDeclContext();
  auto classDecl = dc->getSelfClassDecl();

  // By construction, automatically generated distributed actor ctors have
  // exactly one ActorSystem-conforming argument to the constructor,
  // so we grab the first one from the params.
  VarDecl *var = classDecl->getDistributedActorSystemProperty();

  initializeProperty(SGF, loc, actorSelf.getValue(), var, systemValue, IsNotTake);
}

/// Emits the distributed actor's identity (`id`) initialization.
///
/// Specifically, it performs:
/// \verbatim
///     self.id = system.assignID(Self.self)
/// \endverbatim
void SILGenFunction::emitDistActorIdentityInit(ConstructorDecl *ctor,
                                               SILLocation loc,
                                               SILValue borrowedSelfArg,
                                               SILValue actorSystem) {
  assert(ctor->isDesignatedInit());

  auto &C = ctor->getASTContext();
  
  auto *dc = ctor->getDeclContext();
  auto classDecl = dc->getSelfClassDecl();

  assert(classDecl->isDistributedActor());

  // --- prepare `Self.self` metatype
  auto *selfTyDecl = ctor->getParent()->getSelfNominalTypeDecl();
  auto selfTy = F.mapTypeIntoContext(selfTyDecl->getDeclaredInterfaceType());
  auto selfMetatype = getLoweredType(MetatypeType::get(selfTy));
  SILValue selfMetatypeValue = B.createMetatype(loc, selfMetatype);

  // --- create a temporary storage for the result of the call
  // it will be deallocated automatically as we exit this scope
  VarDecl *var = classDecl->getDistributedActorIDProperty();
  auto resultTy = getLoweredType(F.mapTypeIntoContext(var->getInterfaceType()));
  auto temp = emitTemporaryAllocation(loc, resultTy);

  // --- emit the call itself.
  emitDistributedActorSystemWitnessCall(
      B, loc, C.Id_assignID,
      actorSystem, getLoweredType(selfTy),
      { temp, selfMetatypeValue });

  // --- initialize the property.
  initializeProperty(*this, loc, borrowedSelfArg, var, temp, IsTake);
}

// TODO(distributed): rename to DistributedActorID
InitializeDistActorIdentity::InitializeDistActorIdentity(ConstructorDecl *ctor,
                                       ManagedValue actorSelf)
                                       : ctor(ctor),
                                         actorSelf(actorSelf) {
  systemVar = ctor->getDeclContext()
                  ->getSelfClassDecl()
                  ->getDistributedActorSystemProperty();
  assert(systemVar);
}

void InitializeDistActorIdentity::emit(SILGenFunction &SGF, CleanupLocation loc,
                              ForUnwind_t forUnwind) {

  // If we're unwinding, that must mean we're in the case where the
  // evaluating the expression being assigned to the actorSystem has
  // thrown an error. In that case, we cannot initialize the identity,
  // since there is no actorSystem.
  if (forUnwind == IsForUnwind)
    return;
    

  // Save the current clean-up depth
  auto baseDepth = SGF.getCleanupsDepth();
  {
    loc.markAutoGenerated();
    auto borrowedSelf = actorSelf.borrow(SGF, loc);

    // load the actorSystem value
    Type formalType = SGF.F.mapTypeIntoContext(systemVar->getInterfaceType());
    SILType loweredType = SGF.getLoweredType(formalType).getAddressType();
    auto ref =
      SGF.B.createRefElementAddr(loc, borrowedSelf, systemVar, loweredType);

    SGFContext ctx;
    auto systemVal =
      SGF.emitLoad(loc, ref.getValue(),
                   SGF.getTypeLowering(loweredType), ctx, IsNotTake);

    // Important that we mark the location as auto-generated, since the id
    // is a @_compilerInitialized field.
    SGF.emitDistActorIdentityInit(ctor, loc,
                                borrowedSelf.getValue(), systemVal.getValue());
  }

  // Emit any active clean-ups we just pushed.
  while (SGF.getTopCleanup() != baseDepth)
    SGF.Cleanups.popAndEmitCleanup(SGF.getTopCleanup(), loc, forUnwind);

}

void InitializeDistActorIdentity::dump(SILGenFunction &) const {
#ifndef NDEBUG
  llvm::errs() << "InitializeDistActorIdentity\n"
               << "State: " << getState()
               << "\n";
#endif
}

bool SILGenFunction::shouldReplaceConstantForApplyWithDistributedThunk(
    FuncDecl *func) const {
  auto isDistributedFuncOrAccessor =
      func->isDistributed();
  if (auto acc = dyn_cast<AccessorDecl>(func)) {
    isDistributedFuncOrAccessor =
        acc->getStorage()->isDistributed();
  }

  if (!isDistributedFuncOrAccessor)
    return false;

  // If we are inside a distributed thunk, we want to call the "real" method,
  // in order to avoid infinitely recursively calling the thunk from itself.
  if (F.isDistributed() && F.isThunk())
    return false;

  // If caller and called func are isolated to the same (distributed) actor,
  // (i.e. we are "inside the distributed actor"), there is no need to call
  // the thunk.
  if (isSameActorIsolated(func, FunctionDC))
    return false;

  // In all other situations, we may have to replace the called function,
  // depending on isolation (to be checked in SILGenApply).
  return true;
}

void SILGenFunction::emitDistributedActorImplicitPropertyInits(
    ConstructorDecl *ctor, ManagedValue selfArg) {
  // Only designated initializers should perform this initialization.
  assert(ctor->isDesignatedInit());

  auto loc = SILLocation(ctor);
  loc.markAutoGenerated();

  selfArg = selfArg.borrow(*this, loc);

  // implicit ctors initialize the system and identity from
  // its ActorSystem parameter.
  if (ctor->isImplicit()) {
    SILValue actorSystem = findFirstDistributedActorSystemArg(F);
    emitActorSystemInit(*this, ctor, loc, selfArg, actorSystem);
    emitDistActorIdentityInit(ctor, loc, selfArg.getValue(), actorSystem);
    return;
  }

  // for explicit ctors, store (but do not push) a clean-up that will
  // initialize the identity in whichever scope it's pushed to.
  DistActorCtorContext = InitializeDistActorIdentity(ctor, selfArg);
}

void SILGenFunction::emitDistributedActorReady(
    SILLocation loc, ConstructorDecl *ctor, ManagedValue actorSelf) {

  // Only designated initializers get the lifecycle handling injected
  assert(ctor->isDesignatedInit());

  auto *dc = ctor->getDeclContext();
  auto classDecl = dc->getSelfClassDecl();

  FullExpr scope(Cleanups, CleanupLocation(loc));
  auto borrowedSelf = actorSelf.borrow(*this, loc);

  // --- load the actor system from the actor instance
  ManagedValue actorSystem;
  SGFContext sgfCxt;
  {
    VarDecl *property = classDecl->getDistributedActorSystemProperty();
    Type formalType = F.mapTypeIntoContext(property->getInterfaceType());
    SILType loweredType = getLoweredType(formalType).getAddressType();
    SILValue actorSystemRef = emitActorPropertyReference(
                                *this, loc, borrowedSelf.getValue(), property);
    actorSystem = emitLoad(loc, actorSystemRef,
                           getTypeLowering(loweredType), sgfCxt, IsNotTake);
  }

  emitActorReadyCall(B, loc, borrowedSelf.getValue(), actorSystem.getValue());
}

// ==== ------------------------------------------------------------------------
// MARK: remote instance initialization

/// emit a call to the distributed actor system's resolve function:
///
/// \verbatim
///     system.resolve(id:as:)
/// \endverbatim
static void createDistributedActorFactory_resolve(
    SILGenFunction &SGF, ASTContext &C, FuncDecl *fd, SILValue idValue,
    SILValue actorSystemValue, Type selfTy, SILValue selfMetatypeValue,
    SILType resultTy, SILBasicBlock *normalBB, SILBasicBlock *errorBB) {
  auto &B = SGF.B;

  auto loc = SILLocation(fd);
  loc.markAutoGenerated();

  // // ---- actually call system.resolve(id: id, as: Self.self)
  emitDistributedActorSystemWitnessCall(
      B, loc, C.Id_resolve, actorSystemValue, SGF.getLoweredType(selfTy),
      { idValue, selfMetatypeValue },
      std::make_pair(normalBB, errorBB));
}

/// Function body of:
/// \verbatim
/// DistributedActor.resolve(
///     id: Self.ID,
///     using system: Self.ActorSystem
/// ) throws -> Self
/// \endverbatim
void SILGenFunction::emitDistributedActorFactory(FuncDecl *fd) { // TODO(distributed): rename
  /// NOTE: this will only be reached if the resolve function is actually
  ///       demanded. For example, by declaring the actor as `public` or
  ///       having at least one call to the resolve function.

  auto &C = getASTContext();
  auto DC = fd->getDeclContext();
  SILLocation loc = fd;

  // ==== Prepare argument references
  // --- Parameter: id
  SILArgument *idArg = F.getArgument(0);

  // --- Parameter: system
  SILArgument *actorSystemArg = F.getArgument(1);

  SILValue selfArgValue = F.getSelfArgument();
  ManagedValue selfArg = ManagedValue::forBorrowedObjectRValue(selfArgValue);

  // type: SpecificDistributedActor.Type
  auto selfArgType = selfArg.getType().getASTType();
  auto selfMetatype = getLoweredType(selfArgType);
  SILValue selfMetatypeValue = B.createMetatype(loc, selfMetatype);

  // type: SpecificDistributedActor
  auto *selfTyDecl = DC->getSelfClassDecl();
  assert(selfTyDecl->isDistributedActor());
  auto selfTy = F.mapTypeIntoContext(selfTyDecl->getDeclaredInterfaceType());
  auto returnTy = getLoweredType(selfTy);

  // ==== Prepare all the basic blocks
  auto returnBB = createBasicBlock("returnBB");
  auto resolvedBB = createBasicBlock("resolvedBB");
  auto makeProxyBB = createBasicBlock("makeProxyBB");
  auto switchBB = createBasicBlock("switchBB");
  auto errorBB = createBasicBlock("errorBB");

  SILFunctionConventions fnConv = F.getConventions();

  // --- get the uninitialized allocation from the runtime system.
  FullExpr scope(Cleanups, CleanupLocation(fd));

  auto optionalReturnTy = SILType::getOptionalType(returnTy);

  // ==== Call `try system.resolve(id: id, as: Self.self)`
  {
    createDistributedActorFactory_resolve(
        *this, C, fd, idArg, actorSystemArg, selfTy, selfMetatypeValue,
        optionalReturnTy, switchBB, errorBB);
  }

  // ==== switch resolved { ... }
  {
    B.emitBlock(switchBB);
    auto resolve =
        switchBB->createPhiArgument(optionalReturnTy, OwnershipKind::Owned);

    auto *switchEnum = B.createSwitchEnum(
        loc, resolve, nullptr,
        {{C.getOptionalSomeDecl(), resolvedBB},
         {std::make_pair(C.getOptionalNoneDecl(), makeProxyBB)}});
    switchEnum->createOptionalSomeResult();
  }

  // ==== Case 'some') return the resolved instance
  {
    B.emitBlock(resolvedBB);

    B.createBranch(loc, returnBB, {resolvedBB->getArgument(0)});
  }

  // ==== Case 'none') Create the remote instance
  {
    B.emitBlock(makeProxyBB);
    // ==== Create 'remote' distributed actor instance

    // --- Call: _distributedActorRemoteInitialize(Self.self)
    auto builtinName = C.getIdentifier(
        getBuiltinName(BuiltinValueKind::InitializeDistributedRemoteActor));
    auto *remote = B.createBuiltin(
        loc, builtinName,
        /*returnTy*/returnTy,
        /*subs*/ {},
        {selfMetatypeValue});

    // ==== Initialize distributed actor properties
    loc.markAutoGenerated();
    auto *dc = fd->getDeclContext();
    auto classDecl = dc->getSelfClassDecl();

    initializeProperty(*this, loc, remote,
                       classDecl->getDistributedActorIDProperty(),
                       idArg,
                       IsNotTake);

    initializeProperty(*this, loc, remote,
                       classDecl->getDistributedActorSystemProperty(),
                       actorSystemArg,
                       IsNotTake);

    // ==== Branch to return the fully initialized remote instance
    B.createBranch(loc, returnBB, {remote});
  }

  // --- Emit return logic
  // return <remote>
  {
    B.emitBlock(returnBB);

    auto local = returnBB->createPhiArgument(returnTy, OwnershipKind::Owned);

    Cleanups.emitCleanupsForReturn(CleanupLocation(loc), NotForUnwind);
    B.createReturn(loc, local);
  }

  // --- Emit rethrow logic
  // throw error
  {
    B.emitBlock(errorBB);

    auto error = errorBB->createPhiArgument(
        fnConv.getSILErrorType(F.getTypeExpansionContext()),
        OwnershipKind::Owned);

    Cleanups.emitCleanupsForReturn(CleanupLocation(loc), IsForUnwind);

    // FIXME: typed throws
    B.createThrow(loc, error);
  }
}

// ==== ------------------------------------------------------------------------
// MARK: system.resignID()

void SILGenFunction::emitDistributedActorSystemResignIDCall(
    SILLocation loc, ClassDecl *actorDecl, ManagedValue actorSelf) {
  ASTContext &ctx = getASTContext();
  
  FormalEvaluationScope scope(*this);

  // ==== locate: self.id
  auto idRef = emitActorPropertyReference(
      *this, loc, actorSelf.getValue(), actorDecl->getDistributedActorIDProperty());

  // ==== locate: self.actorSystem
  auto systemRef = emitActorPropertyReference(
      *this, loc, actorSelf.getValue(),
      actorDecl->getDistributedActorSystemProperty());

  // Perform the call.
  emitDistributedActorSystemWitnessCall(
      B, loc, ctx.Id_resignID,
      systemRef,
      SILType(),
      { idRef });
}

void
SILGenFunction::emitConditionalResignIdentityCall(SILLocation loc,
                                                  ClassDecl *actorDecl,
                                                  ManagedValue actorSelf,
                                                  SILBasicBlock *continueBB,
                                                  SILBasicBlock *finishBB) {
  assert(actorDecl->isDistributedActor() &&
         "only distributed actors have actorSystem lifecycle hooks in deinit");
  assert(continueBB && finishBB &&
         "need valid continue and finish basic blocks");

  auto selfTy = F.mapTypeIntoContext(actorDecl->getDeclaredInterfaceType());

  // we only system.resignID if we are a local actor,
  // and thus the address was created by system.assignID.
  auto isRemoteBB = createBasicBlock("isRemoteBB");
  auto isLocalBB = createBasicBlock("isLocalBB");

  // if __isRemoteActor(self) {
  //   ...
  // } else {
  //   ...
  // }
  emitDistributedIfRemoteBranch(loc,
                                actorSelf, selfTy,
                                /*if remote*/isRemoteBB,
                                /*if local*/isLocalBB);

  // if remote, return early; the user defined deinit should not run.
  {
    B.emitBlock(isRemoteBB);
    B.createBranch(loc, finishBB);
  }

  // if local, resign identity.
  {
    B.emitBlock(isLocalBB);

    emitDistributedActorSystemResignIDCall(loc, actorDecl, actorSelf);
    
    B.createBranch(loc, continueBB);
  }
}

/******************************************************************************/
/******************* DISTRIBUTED DEINIT: class memberwise destruction *********/
/******************************************************************************/

void SILGenFunction::emitDistributedActorClassMemberDestruction(
    SILLocation cleanupLoc, ManagedValue selfValue, ClassDecl *cd,
    SILBasicBlock *normalMemberDestroyBB,
    SILBasicBlock *remoteMemberDestroyBB,
    SILBasicBlock *finishBB) {
  auto selfTy = cd->getDeclaredInterfaceType();

  Scope scope(Cleanups, CleanupLocation(cleanupLoc));

  auto isLocalBB = createBasicBlock("isLocalBB");

  // if __isRemoteActor(self) {
  //   ...
  // } else {
  //   ...
  // }
  emitDistributedIfRemoteBranch(cleanupLoc,
                                selfValue, selfTy,
                                /*if remote*/remoteMemberDestroyBB,
                                /*if local*/isLocalBB);

  // // if __isRemoteActor(self)
  // {
  //  // destroy only self.id and self.actorSystem
  // }
  {
    B.emitBlock(remoteMemberDestroyBB);

    for (VarDecl *vd : cd->getStoredProperties()) {
      if (getActorIsolation(vd) == ActorIsolation::ActorInstance)
        continue;

      destroyClassMember(cleanupLoc, selfValue, vd);
    }

    B.createBranch(cleanupLoc, finishBB);
  }

  // // else (local distributed actor)
  // {
  //   <continue normal deinit>
  // }
  {
    B.emitBlock(isLocalBB);

    B.createBranch(cleanupLoc, normalMemberDestroyBB);
  }
}
