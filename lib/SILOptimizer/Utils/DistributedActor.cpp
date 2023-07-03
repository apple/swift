//===-- DistributedActor.cpp - SIL utils for distributed actors -*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SILOptimizer/Utils/DistributedActor.h"

#include "swift/AST/Decl.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILLocation.h"

namespace swift {

void emitDistributedActorSystemWitnessCall(
    SILBuilder &B, SILLocation loc, DeclName methodName, SILValue base,
    // types to be passed through to SubstitutionMap:
    SILType actorType,
    // call arguments, except the base which will be passed last
    ArrayRef<SILValue> args,
    llvm::Optional<std::pair<SILBasicBlock *, SILBasicBlock *>> tryTargets) {
  auto &F = B.getFunction();
  auto &M = B.getModule();
  auto &C = F.getASTContext();

  // Dig out the conformance to DistributedActorSystem.
  ProtocolDecl *DAS = C.getDistributedActorSystemDecl();
  assert(DAS);
  auto systemASTType = base->getType().getASTType();
  auto *module = M.getSwiftModule();
  ProtocolConformanceRef systemConfRef;

  // If the base is an existential open it.
  if (systemASTType->isAnyExistentialType()) {
    OpenedArchetypeType *opened;
    systemASTType = systemASTType->openAnyExistentialType(opened,
                                                          F.getGenericSignature())
                        ->getCanonicalType();
    base = B.createOpenExistentialAddr(
        loc, base, F.getLoweredType(systemASTType),
        OpenedExistentialAccess::Immutable);
  }

  if (systemASTType->isTypeParameter() || systemASTType->is<ArchetypeType>()) {
    systemConfRef = ProtocolConformanceRef(DAS);
  } else {
    systemConfRef = module->lookupConformance(systemASTType, DAS);
  }

  assert(!systemConfRef.isInvalid() &&
         "Missing conformance to `DistributedActorSystem`");

  // Dig out the method.
  auto method = cast<FuncDecl>(DAS->getSingleRequirement(methodName));
  auto methodRef = SILDeclRef(method, SILDeclRef::Kind::Func);
  auto methodSILTy =
      M.Types.getConstantInfo(B.getTypeExpansionContext(), methodRef)
          .getSILType();

  auto witnessMethod = B.createWitnessMethod(
      loc, systemASTType, systemConfRef, methodRef, methodSILTy);

  // prepare conformance substitutions
  SubstitutionMap subs;
  {
    auto genericSig = method->getGenericSignature();
    SmallVector<Type, 2> subTypes;
    SmallVector<ProtocolConformanceRef, 2> subConformances;
    subTypes.push_back(systemASTType);
    subConformances.push_back(systemConfRef);
    if (actorType) {
      ProtocolDecl *actorProto = C.getProtocol(
          KnownProtocolKind::DistributedActor);
      assert(actorProto);

      ProtocolConformanceRef conformance;
      auto distributedActorConfRef = module->lookupConformance(
          actorType.getASTType(), actorProto);
      assert(!distributedActorConfRef.isInvalid() &&
             "Missing conformance to `DistributedActor`");
      subTypes.push_back(actorType.getASTType());
      subConformances.push_back(distributedActorConfRef);
    }

    subs = SubstitutionMap::get(genericSig, subTypes, subConformances);
  }

  llvm::Optional<SILValue> temporaryArgumentBuffer;

  // If the self parameter is indirect but the base is a value, put it
  // into a temporary allocation.
  auto methodSILFnTy = methodSILTy.castTo<SILFunctionType>();
  llvm::Optional<SILValue> temporaryActorSystemBuffer;
  if (methodSILFnTy->getSelfParameter().isFormalIndirect() &&
      !base->getType().isAddress()) {
    auto buf = B.createAllocStack(loc, base->getType(), llvm::None);
    base = B.emitCopyValueOperation(loc, base);
    B.emitStoreValueOperation(
        loc, base, buf, StoreOwnershipQualifier::Init);
    temporaryActorSystemBuffer = SILValue(buf);
  }

  // === Call the method.
  // --- Push the arguments
  SmallVector<SILValue, 2> allArgs;
  auto params = methodSILFnTy->getParameters();
  for (size_t i = 0; i < args.size(); ++i) {
    auto arg = args[i];
    if (params[i].isFormalIndirect() &&
        !arg->getType().isAddress() &&
        !dyn_cast<AnyMetatypeType>(arg->getType().getASTType())) {
      auto buf = B.createAllocStack(loc, arg->getType(), llvm::None);
      auto argCopy = B.emitCopyValueOperation(loc, arg);
      B.emitStoreValueOperation(
          loc, argCopy, buf, StoreOwnershipQualifier::Init);
      temporaryArgumentBuffer = SILValue(buf);
      allArgs.push_back(*temporaryArgumentBuffer);
    } else {
      allArgs.push_back(arg);
    }
  }
  // Push the self argument
  auto selfArg = temporaryActorSystemBuffer ? *temporaryActorSystemBuffer : base;
  allArgs.push_back(selfArg);

  SILInstruction *apply;
  if (tryTargets) {
    apply = B.createTryApply(
        loc, witnessMethod, subs, allArgs, tryTargets->first,
        tryTargets->second);
  } else {
    apply = B.createApply(loc, witnessMethod, subs, allArgs);
  }

  // Local function to emit a cleanup after the call.
  auto emitCleanup = [&](llvm::function_ref<void(SILBuilder &builder)> fn) {
    if (tryTargets) {
      {
        SILBuilderWithScope normalBuilder(tryTargets->first, apply);
        fn(normalBuilder);
      }
      {
        SILBuilderWithScope errorBuilder(tryTargets->second, apply);
        fn(errorBuilder);
      }
    } else {
      fn(B);
    }
  };

  // ==== If we had to create a buffers we need to clean them up
  // --- Cleanup id buffer
  if (temporaryArgumentBuffer) {
    emitCleanup([&](SILBuilder & builder) {
      auto value = builder.emitLoadValueOperation(
          loc, *temporaryArgumentBuffer, LoadOwnershipQualifier::Take);
      builder.emitDestroyValueOperation(loc, value);
      builder.createDeallocStack(loc, *temporaryArgumentBuffer);
    });
  }
  // --- Cleanup base buffer
  if (temporaryActorSystemBuffer) {
    emitCleanup([&](SILBuilder & builder) {
      auto value = builder.emitLoadValueOperation(
          loc, *temporaryActorSystemBuffer, LoadOwnershipQualifier::Take);
      builder.emitDestroyValueOperation(loc, value);
      builder.createDeallocStack(loc, *temporaryActorSystemBuffer);
    });
  }
}

void emitActorReadyCall(SILBuilder &B, SILLocation loc, SILValue actor,
                        SILValue actorSystem) {

  auto &F = B.getFunction();
  auto &C = F.getASTContext();
  emitDistributedActorSystemWitnessCall(
      B, loc, C.Id_actorReady, actorSystem,
      actor->getType(), { actor });
}

void emitResignIdentityCall(SILBuilder &B, SILLocation loc,
                            ClassDecl* actorDecl,
                            SILValue actor, SILValue idRef) {
  auto &F = B.getFunction();
  auto &C = F.getASTContext();

  SILValue systemRef = refDistributedActorSystem(B, loc, actorDecl, actor);

  emitDistributedActorSystemWitnessCall(
      B, loc, C.Id_resignID,
      systemRef,
      SILType(),
      { idRef });
}

/// Creates a reference to the distributed actor's \p actorSystem
/// stored property.
SILValue refDistributedActorSystem(SILBuilder &b,
                                   SILLocation loc,
                                   ClassDecl *actDecl,
                                   SILValue actorInstance) {
  assert(actDecl);
  assert(actDecl->isDistributedActor());

  // get the VarDecl corresponding to the actorSystem.
  auto refs = actDecl->lookupDirect(actDecl->getASTContext().Id_actorSystem);
  assert(refs.size() == 1);
  VarDecl *actorSystemVar = dyn_cast<VarDecl>(refs.front());

  return b.createRefElementAddr(loc, actorInstance, actorSystemVar);
}

} // namespace swift
