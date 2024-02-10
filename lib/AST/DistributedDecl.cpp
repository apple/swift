//===--- Decl.cpp - Swift Language Decl ASTs ------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file handles lookups related to distributed actor decls.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/DistributedDecl.h"
#include "swift/AST/AccessRequests.h"
#include "swift/AST/AccessScope.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ForeignAsyncConvention.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParseRequests.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ResilienceExpansion.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/SwiftNameTranslation.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Parse/Lexer.h" // FIXME: Bad dependency
#include "clang/Lex/MacroInfo.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"
#include "swift/Basic/StringExtras.h"

#include "clang/Basic/CharInfo.h"
#include "clang/Basic/Module.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclObjC.h"

#include <algorithm>

using namespace swift;

/******************************************************************************/
/************** Distributed Actor System Associated Types *********************/
/******************************************************************************/

// TODO(distributed): make into a request
Type swift::getConcreteReplacementForProtocolActorSystemType(ValueDecl *member) {
  auto &C = member->getASTContext();
  auto *DC = member->getDeclContext();
  auto DA = C.getDistributedActorDecl();

  // === When declared inside an actor, we can get the type directly
  if (auto classDecl = DC->getSelfClassDecl()) {
    return getDistributedActorSystemType(classDecl);
  }

  /// === Maybe the value is declared in a protocol?
  if (auto protocol = DC->getSelfProtocolDecl()) {
    GenericSignature signature;
    if (auto *genericContext = member->getAsGenericContext()) {
      signature = genericContext->getGenericSignature();
    } else {
      signature = DC->getGenericSignatureOfContext();
    }

    auto ActorSystemAssocType =
        DA->getAssociatedType(C.Id_ActorSystem)->getDeclaredInterfaceType();

    // Note that this may be null, e.g. if we're a distributed func inside
    // a protocol that did not declare a specific actor system requirement.
    return signature->getConcreteType(ActorSystemAssocType);
  }

  llvm_unreachable("Unable to fetch ActorSystem type!");
}

Type swift::getSerializationRequirementTypesForMember(
    ValueDecl *member,
    llvm::SmallPtrSet<ProtocolDecl *, 2> &serializationRequirements) {
  auto &C = member->getASTContext();
  auto *DC = member->getDeclContext();
  auto DA = C.getDistributedActorDecl();

  // === When declared inside an actor, we can get the type directly
  if (auto classDecl = DC->getSelfClassDecl()) {
    return getDistributedSerializationRequirementType(classDecl, C.getDistributedActorDecl());
  }

  auto SerReqAssocType = DA->getAssociatedType(C.Id_SerializationRequirement)
      ->getDeclaredInterfaceType();

  if (DC->getSelfProtocolDecl()) {
    GenericSignature signature;
    if (auto *genericContext = member->getAsGenericContext()) {
      signature = genericContext->getGenericSignature();
    } else {
      signature = DC->getGenericSignatureOfContext();
    }

    // Also store all `SerializationRequirement : SomeProtocol` requirements
    for (auto proto: signature->getRequiredProtocols(SerReqAssocType)) {
      serializationRequirements.insert(proto);
    }

    // Note that this may be null, e.g. if we're a distributed func inside
    // a protocol that did not declare a specific actor system requirement.
    return signature->getConcreteType(SerReqAssocType);
  }

  llvm_unreachable("Unable to fetch ActorSystem type!");
}

Type swift::getDistributedActorSystemType(NominalTypeDecl *actor) {
  assert(!dyn_cast<ProtocolDecl>(actor) &&
         "Use getConcreteReplacementForProtocolActorSystemType instead to get"
         "the concrete ActorSystem, if bound, for this DistributedActor "
         "constrained ProtocolDecl!");
  assert(actor->isDistributedActor());
  auto &C = actor->getASTContext();

  auto DA = C.getDistributedActorDecl();
  if (!DA)
    return ErrorType::get(C); // FIXME(distributed): just use Type()

  // Dig out the actor system type.
  auto module = actor->getParentModule();
  Type selfType = actor->getSelfInterfaceType();
  auto conformance = module->lookupConformance(selfType, DA);
  return conformance.getTypeWitnessByName(selfType, C.Id_ActorSystem);
}

Type swift::getDistributedActorIDType(NominalTypeDecl *actor) {
  auto &C = actor->getASTContext();
  return C.getAssociatedTypeOfDistributedSystemOfActor(actor, C.Id_ActorID);
}

Type swift::getDistributedActorSystemActorIDType(NominalTypeDecl *system) {
  assert(!system->isDistributedActor());
  auto &ctx = system->getASTContext();

  auto DAS = ctx.getDistributedActorSystemDecl();
  if (!DAS)
    return Type();

  // Dig out the serialization requirement type.
  auto module = system->getParentModule();
  Type selfType = system->getSelfInterfaceType();
  auto conformance = module->lookupConformance(selfType, DAS);
  return conformance.getTypeWitnessByName(selfType, ctx.Id_ActorID);
}

Type swift::getDistributedActorSystemResultHandlerType(
    NominalTypeDecl *system) {
  assert(!system->isDistributedActor());
  auto &ctx = system->getASTContext();

  auto DAS = ctx.getDistributedActorSystemDecl();
  if (!DAS)
    return Type();

  // Dig out the serialization requirement type.
  auto module = system->getParentModule();
  Type selfType = system->getSelfInterfaceType();
  auto conformance = module->lookupConformance(selfType, DAS);
  return conformance.getTypeWitnessByName(selfType, ctx.Id_ResultHandler);
}

Type swift::getDistributedActorSystemInvocationEncoderType(NominalTypeDecl *system) {
  assert(!system->isDistributedActor());
  auto &ctx = system->getASTContext();

  auto DAS = ctx.getDistributedActorSystemDecl();
  if (!DAS)
    return Type();

  // Dig out the serialization requirement type.
  auto module = system->getParentModule();
  Type selfType = system->getSelfInterfaceType();
  auto conformance = module->lookupConformance(selfType, DAS);
  return conformance.getTypeWitnessByName(selfType, ctx.Id_InvocationEncoder);
}

Type swift::getDistributedActorSystemInvocationDecoderType(NominalTypeDecl *system) {
  assert(!system->isDistributedActor());
  auto &ctx = system->getASTContext();

  auto DAS = ctx.getDistributedActorSystemDecl();
  if (!DAS)
    return Type();

  // Dig out the serialization requirement type.
  auto module = system->getParentModule();
  Type selfType = system->getSelfInterfaceType();
  auto conformance = module->lookupConformance(selfType, DAS);
  return conformance.getTypeWitnessByName(selfType, ctx.Id_InvocationDecoder);
}

Type swift::getDistributedSerializationRequirementType(
    NominalTypeDecl *nominal, ProtocolDecl *protocol) {
  assert(nominal);
  assert(protocol);
  auto &ctx = nominal->getASTContext();

  // Dig out the serialization requirement type.
  auto module = nominal->getParentModule();
  Type selfType = nominal->getSelfInterfaceType();
  auto conformance = module->lookupConformance(selfType, protocol);
  if (conformance.isInvalid())
    return Type();

  return conformance.getTypeWitnessByName(selfType, ctx.Id_SerializationRequirement);
}

AbstractFunctionDecl *
swift::getAssociatedDistributedInvocationDecoderDecodeNextArgumentFunction(
    ValueDecl *thunk) {
  assert(thunk);
  auto &C = thunk->getASTContext();

  auto *actor = thunk->getDeclContext()->getSelfNominalTypeDecl();
  if (!actor)
    return nullptr;
  if (!actor->isDistributedActor())
    return nullptr;

  auto systemTy = getConcreteReplacementForProtocolActorSystemType(thunk);
  if (!systemTy)
    return nullptr;

  auto decoderTy =
      getDistributedActorSystemInvocationDecoderType(
          systemTy->getAnyNominal());
  if (!decoderTy)
    return nullptr;

  return C.getDecodeNextArgumentOnDistributedInvocationDecoder(
      decoderTy->getAnyNominal());
}

Type ASTContext::getAssociatedTypeOfDistributedSystemOfActor(
    NominalTypeDecl *actor, Identifier member) {
  auto &ctx = actor->getASTContext();

  auto actorProtocol = ctx.getProtocol(KnownProtocolKind::DistributedActor);
  if (!actorProtocol)
    return ErrorType::get(ctx);

  AssociatedTypeDecl *actorSystemDecl =
      actorProtocol->getAssociatedType(ctx.Id_ActorSystem);
  if (!actorSystemDecl)
    return Type();

  auto actorSystemProtocol = ctx.getDistributedActorSystemDecl();
  if (!actorSystemProtocol)
    return Type();

  AssociatedTypeDecl *assocTypeDecl =
      actorSystemProtocol->getAssociatedType(member);
  if (!assocTypeDecl)
    return Type();

  auto module = actor->getParentModule();

  // In case of protocol, let's find a concrete `ActorSystem`
  if (auto *protocol = dyn_cast<ProtocolDecl>(actor)) {
    auto signature = protocol->getGenericSignatureOfContext();

    auto systemTy =
        signature->getConcreteType(actorSystemDecl->getDeclaredInterfaceType());
    if (!systemTy)
      return Type();

    auto conformance = module->lookupConformance(systemTy, actorSystemProtocol);
    if (conformance.isInvalid())
      return Type();

    return conformance.getTypeWitnessByName(systemTy, member);
  }

  Type selfType = actor->getSelfInterfaceType();
  auto conformance = module->lookupConformance(selfType, actorProtocol);
  Type dependentType = actorProtocol->getSelfInterfaceType();
  dependentType = DependentMemberType::get(dependentType, actorSystemDecl);
  dependentType = DependentMemberType::get(dependentType, assocTypeDecl);

  return dependentType.subst(SubstitutionMap::getProtocolSubstitutions(
      actorProtocol, selfType, conformance));
}

/******************************************************************************/
/******** Functions on DistributedActorSystem and friends *********************/
/******************************************************************************/


FuncDecl*
ASTContext::getDistributedActorArgumentDecodingMethod(NominalTypeDecl *actor) {
  if (!actor->isDistributedActor())
    return nullptr;

  return evaluateOrDefault(
      evaluator, GetDistributedActorArgumentDecodingMethodRequest{actor}, nullptr);
}

NominalTypeDecl*
ASTContext::getDistributedActorInvocationDecoder(NominalTypeDecl *actor) {
  if (!actor->isDistributedActor())
    return nullptr;

  return evaluateOrDefault(
      evaluator, GetDistributedActorInvocationDecoderRequest{actor}, nullptr);
}

bool
swift::getDistributedSerializationRequirements(
    NominalTypeDecl *nominal,
    ProtocolDecl *protocol,
    llvm::SmallPtrSetImpl<ProtocolDecl *> &requirementProtos) {
  auto existentialRequirementTy =
      getDistributedSerializationRequirementType(nominal, protocol);
  if (existentialRequirementTy->hasError()) {
    return false;
  }

  if (existentialRequirementTy->isAny())
    return true; // we're done here, any means there are no requirements

  auto *serialReqType = existentialRequirementTy->getAs<ExistentialType>();
  if (!serialReqType || serialReqType->hasError()) {
    return false;
  }

  auto layout = serialReqType->getExistentialLayout();
  for (auto p : layout.getProtocols()) {
    requirementProtos.insert(p);
  }

  return true;
}

bool swift::checkDistributedSerializationRequirementIsExactlyCodable(
    ASTContext &C,
    Type type) {
  if (!type)
    return false;

  if (type->hasError())
    return false;

  auto encodable = C.getProtocol(KnownProtocolKind::Encodable);
  auto decodable = C.getProtocol(KnownProtocolKind::Decodable);

  auto layout = type->getExistentialLayout();
  auto protocols = layout.getProtocols();

  if (protocols.size() != 2)
    return false;

  return std::count(protocols.begin(), protocols.end(), encodable) == 1 &&
      std::count(protocols.begin(), protocols.end(), decodable) == 1;
}

/******************************************************************************/
/********************* Ad-hoc protocol requirement checks *********************/
/******************************************************************************/

bool AbstractFunctionDecl::isDistributedActorSystemRemoteCallRequirement(
    bool withResult) const {
  auto &ctx = getASTContext();
  auto *DC = getDeclContext();

  auto expectedName =
      withResult ? DeclName(ctx, ctx.Id_remoteCall,
                            {ctx.Id_on, ctx.Id_target, ctx.Id_invocation,
                             ctx.Id_throwing, ctx.Id_returning})
                 : DeclName(ctx, ctx.Id_remoteCallVoid,
                            {ctx.Id_on, ctx.Id_target, ctx.Id_invocation,
                             ctx.Id_throwing});

  if (getName() != expectedName)
    return false;

  if (!DC->isTypeContext() || !isGeneric())
    return false;

  auto declaredIn = DC->getSelfProtocolDecl();
  if (!(declaredIn && declaredIn == ctx.getDistributedActorSystemDecl()))
    return false;

  auto genericParams = getGenericParams();
  if (genericParams->size() != (withResult ? 3 : 2))
    return false;

  if (!hasThrows() || !hasAsync())
    return false;

  return true;
}

bool AbstractFunctionDecl::isDistributedActorSystemRemoteCall(bool isVoidReturn) const {
  auto &C = getASTContext();
  auto module = getParentModule();

  // === Check the name
  auto callId = isVoidReturn ? C.Id_remoteCallVoid : C.Id_remoteCall;
  if (getBaseName() != callId) {
    return false;
  }

  // === Must be declared in a 'DistributedActorSystem' conforming type
  ProtocolDecl *systemProto =
      C.getDistributedActorSystemDecl();

  auto systemNominal = getDeclContext()->getSelfNominalTypeDecl();
  auto distSystemConformance = module->lookupConformance(
      systemNominal->getDeclaredInterfaceType(), systemProto);

  if (distSystemConformance.isInvalid()) {
    return false;
  }

  auto *func = dyn_cast<FuncDecl>(this);
  if (!func) {
    return false;
  }

  // === Structural Checks
  // -- Must be throwing
  if (!hasThrows()) {
    return false;
  }

  // -- Must be async
  if (!hasAsync()) {
    return false;
  }

  // -- Must not be mutating, use classes to implement a system instead
  if (func->isMutating()) {
    return false;
  }

  // === Check generics
  if (!isGeneric()) {
    return false;
  }

  // --- Check number of generic parameters
  auto genericParams = getGenericParams();
  unsigned int expectedGenericParamNum = isVoidReturn ? 2 : 3;

  if (genericParams->size() != expectedGenericParamNum) {
    return false;
  }

  // === Get the SerializationRequirement
  SmallPtrSet<ProtocolDecl*, 2> requirementProtos;
  if (!getDistributedSerializationRequirements(
          systemNominal, systemProto, requirementProtos)) {
    return false;
  }

  // -- Check number of generic requirements
  size_t expectedRequirementsNum = 3;
  size_t serializationRequirementsNum = 0;
  if (!isVoidReturn) {
    serializationRequirementsNum = requirementProtos.size();
    expectedRequirementsNum += serializationRequirementsNum;
  }

  // === Check all parameters
  auto params = getParameters();

  // --- Count of parameters depends on if we're void returning or not
  unsigned int expectedParamNum = isVoidReturn ? 4 : 5;
  if (!params || params->size() != expectedParamNum) {
    return false;
  }

  // --- Check parameter: on: Actor
  auto actorParam = params->get(0);
  if (actorParam->getArgumentName() != C.Id_on) {
    return false;
  }

  // --- Check parameter: target RemoteCallTarget
  auto targetParam = params->get(1);
  if (targetParam->getArgumentName() != C.Id_target) {
    return false;
  }

  // --- Check parameter: invocation: inout InvocationEncoder
  auto invocationParam = params->get(2);
  if (invocationParam->getArgumentName() != C.Id_invocation) {
    return false;
  }
  if (!invocationParam->isInOut()) {
    return false;
  }

  // --- Check parameter: throwing: Err.Type
  auto thrownTypeParam = params->get(3);
  if (thrownTypeParam->getArgumentName() != C.Id_throwing) {
    return false;
  }

  // --- Check parameter: returning: Res.Type
  if (!isVoidReturn) {
    auto returnedTypeParam = params->get(4);
    if (returnedTypeParam->getArgumentName() != C.Id_returning) {
      return false;
    }
  }

  // === Check generic parameters in detail
  // --- Check: Act: DistributedActor,
  //            Act.ID == Self.ActorID
  GenericTypeParamDecl *ActParam = genericParams->getParams()[0];
  auto ActConformance = module->lookupConformance(
      mapTypeIntoContext(ActParam->getDeclaredInterfaceType()),
      C.getProtocol(KnownProtocolKind::DistributedActor));
  if (ActConformance.isInvalid()) {
    return false;
  }

  // --- Check: Err: Error
  GenericTypeParamDecl *ErrParam = genericParams->getParams()[1];
  auto ErrConformance = module->lookupConformance(
      mapTypeIntoContext(ErrParam->getDeclaredInterfaceType()),
      C.getProtocol(KnownProtocolKind::Error));
  if (ErrConformance.isInvalid()) {
    return false;
  }

  // --- Check: Res: SerializationRequirement
  // We could have the `SerializationRequirement = Any` in which case there are
  // no requirements to check on `Res`
  GenericTypeParamDecl *ResParam = nullptr;
  if (!isVoidReturn) {
    ResParam = genericParams->getParams().back();
  }

  auto sig = getGenericSignature();

  SmallVector<Requirement, 2> reqs;
  SmallVector<InverseRequirement, 2> inverseReqs;
  sig->getRequirementsWithInverses(reqs, inverseReqs);
  assert(inverseReqs.empty() && "Non-copyable generics not supported here!");

  if (reqs.size() != expectedRequirementsNum) {
    return false;
  }

  // --- Check the expected requirements
  // conforms_to: Act DistributedActor
  // conforms_to: Err Error
  // --- all the Res requirements ---
  // conforms_to: Res Decodable
  // conforms_to: Res Encodable
  // ...
  // --------------------------------
  // same_type: Act.ID FakeActorSystem.ActorID // LAST one

  // --- Check requirement: conforms_to: Act DistributedActor
  auto actorReq = reqs[0];
  if (actorReq.getKind() != RequirementKind::Conformance) {
    return false;
  }
  if (!actorReq.getProtocolDecl()->isSpecificProtocol(KnownProtocolKind::DistributedActor)) {
    return false;
  }

  // --- Check requirement: conforms_to: Err Error
  auto errorReq = reqs[1];
  if (errorReq.getKind() != RequirementKind::Conformance) {
    return false;
  }
  if (!errorReq.getProtocolDecl()->isSpecificProtocol(KnownProtocolKind::Error)) {
    return false;
  }

  // --- Check requirement: Res either Void or all SerializationRequirements
  if (isVoidReturn) {
    if (auto func = dyn_cast<FuncDecl>(this)) {
      if (!func->getResultInterfaceType()->isVoid()) {
        return false;
      }
    }
  } else if (ResParam) {
    assert(ResParam && "Non void function, yet no Res generic parameter found");
    if (auto func = dyn_cast<FuncDecl>(this)) {
      auto resultType = func->mapTypeIntoContext(func->getResultInterfaceType())
                            ->getMetatypeInstanceType();
      auto resultParamType = func->mapTypeIntoContext(
          ResParam->getDeclaredInterfaceType());
      // The result of the function must be the `Res` generic argument.
      if (!resultType->isEqual(resultParamType)) {
        return false;
      }

      for (auto requirementProto : requirementProtos) {
        auto conformance = module->lookupConformance(resultType, requirementProto);
        if (conformance.isInvalid()) {
          return false;
        }
      }
    } else {
      return false;
    }
  }

  // -- Check requirement: same_type Actor.ID Self.ActorID
  auto actorIdReq = reqs.back();
  if (actorIdReq.getKind() != RequirementKind::SameType) {
    return false;
  }
  auto expectedActorIdTy = getDistributedActorSystemActorIDType(systemNominal);
  if (!actorIdReq.getSecondType()->isEqual(expectedActorIdTy)) {
    return false;
  }

  return true;
}

bool
AbstractFunctionDecl::isDistributedActorSystemMakeInvocationEncoder() const {
  auto &C = getASTContext();
  auto module = getParentModule();

  if (getBaseIdentifier() != C.Id_makeInvocationEncoder) {
    return false;
  }

  auto *func = dyn_cast<FuncDecl>(this);
  if (!func) {
    return false;
  }
  if (func->getParameters()->size() != 0) {
    return false;
  }
  if (func->hasAsync()) {
    return false;
  }
  if (func->hasThrows()) {
    return false;
  }

  auto returnTy = func->getResultInterfaceType();
  auto conformance = module->lookupConformance(
      returnTy, C.getDistributedTargetInvocationEncoderDecl());
  if (conformance.isInvalid()) {
    return false;
  }

  return true;
}

bool
AbstractFunctionDecl::isDistributedTargetInvocationEncoderRecordGenericSubstitution() const {
  auto &C = getASTContext();

  if (getBaseIdentifier() != C.Id_recordGenericSubstitution) {
    return false;
  }

  auto *fd = dyn_cast<FuncDecl>(this);
  if (!fd) {
    return false;
  }
  if (fd->getParameters()->size() != 1) {
    return false;
  }
  if (fd->hasAsync()) {
    return false;
  }
  if (!fd->hasThrows()) {
    return false;
  }
  // TODO(distributed): more checks

  // A single generic parameter.
  auto genericParamList = fd->getGenericParams();
  if (genericParamList->size() != 1) {
    return false;
  }

  SmallVector<Requirement, 2> reqs;
  SmallVector<InverseRequirement, 2> inverseReqs;
  fd->getGenericSignature()->getRequirementsWithInverses(reqs, inverseReqs);
  assert(inverseReqs.empty() && "Non-copyable generics not supported here!");

  // No requirements on the generic parameter
  if (!reqs.empty())
    return false;

  if (!fd->getResultInterfaceType()->isVoid())
    return false;

  return true;
}

bool
AbstractFunctionDecl::isDistributedTargetInvocationEncoderRecordArgument() const {
  auto &C = getASTContext();
  auto module = getParentModule();

  auto func = dyn_cast<FuncDecl>(this);
  if (!func) {
    return false;
  }

  // === Check base name
  if (getBaseIdentifier() != C.Id_recordArgument) {
    return false;
  }

  // === Must be declared in a 'DistributedTargetInvocationEncoder' conforming type
  ProtocolDecl *encoderProto =
      C.getProtocol(KnownProtocolKind::DistributedTargetInvocationEncoder);

  auto encoderNominal = getDeclContext()->getSelfNominalTypeDecl();
  auto protocolConformance = module->lookupConformance(
      encoderNominal->getDeclaredInterfaceType(), encoderProto);

  if (protocolConformance.isInvalid()) {
    return false;
  }

  // === Check modifiers
  // --- must not be async
    if (hasAsync()) {
      return false;
    }

    // --- must be throwing
    if (!hasThrows()) {
      return false;
    }

    // === Check generics
    if (!isGeneric()) {
      return false;
    }

    // --- must be mutating, if it is defined in a struct
    if (isa<StructDecl>(getDeclContext()) &&
        !func->isMutating()) {
      return false;
    }

    // --- Check number of generic parameters
    auto genericParams = getGenericParams();
    unsigned int expectedGenericParamNum = 1;

    if (genericParams->size() != expectedGenericParamNum) {
      return false;
    }

    // === Get the SerializationRequirement
    SmallPtrSet<ProtocolDecl*, 2> requirementProtos;
    if (!getDistributedSerializationRequirements(
            encoderNominal, encoderProto, requirementProtos)) {
      return false;
    }

    // -- Check number of generic requirements
    size_t serializationRequirementsNum = requirementProtos.size();
    size_t expectedRequirementsNum = serializationRequirementsNum;

    // === Check all parameters
    auto params = getParameters();
    if (params->size() != 1) {
      return false;
    }

  GenericTypeParamDecl *ArgumentParam = genericParams->getParams()[0];

    // --- Check parameter: _ argument
    auto argumentParam = params->get(0);
    if (!argumentParam->getArgumentName().empty()) {
      return false;
    }

    auto argumentTy = argumentParam->getInterfaceType();
    auto argumentInContextTy = mapTypeIntoContext(argumentTy);
    if (argumentInContextTy->getAnyNominal() == C.getRemoteCallArgumentDecl()) {
      auto argGenericParams = argumentInContextTy->getStructOrBoundGenericStruct()
          ->getGenericParams()->getParams();
      if (argGenericParams.size() != 1) {
        return false;
      }

      // the <Value> of the RemoteCallArgument<Value>
      auto remoteCallArgValueGenericTy =
          mapTypeIntoContext(argGenericParams[0]->getDeclaredInterfaceType());
      // expected (the <Value> from the recordArgument<Value>)
      auto expectedGenericParamTy = mapTypeIntoContext(
          ArgumentParam->getDeclaredInterfaceType());

      if (!remoteCallArgValueGenericTy->isEqual(expectedGenericParamTy)) {
            return false;
          }
    } else {
      return false;
    }


    auto sig = getGenericSignature();

    SmallVector<Requirement, 2> reqs;
    SmallVector<InverseRequirement, 2> inverseReqs;
    sig->getRequirementsWithInverses(reqs, inverseReqs);
    assert(inverseReqs.empty() && "Non-copyable generics not supported here!");

    if (reqs.size() != expectedRequirementsNum) {
      return false;
    }

    // --- Check the expected requirements
    // --- all the Argument requirements ---
    // e.g.
    // conforms_to: Argument Decodable
    // conforms_to: Argument Encodable
    // ...

    // === Check result type: Void
    if (!func->getResultInterfaceType()->isVoid()) {
      return false;
    }

    return true;
}

bool
AbstractFunctionDecl::isDistributedTargetInvocationEncoderRecordReturnType() const {
  auto &C = getASTContext();
  auto module = getParentModule();

  auto func = dyn_cast<FuncDecl>(this);
  if (!func) {
    return false;
  }

  // === Check base name
  if (getBaseIdentifier() != C.Id_recordReturnType) {
    return false;
  }

  // === Must be declared in a 'DistributedTargetInvocationEncoder' conforming type
  ProtocolDecl *encoderProto =
      C.getProtocol(KnownProtocolKind::DistributedTargetInvocationEncoder);

  auto encoderNominal = getDeclContext()->getSelfNominalTypeDecl();
  auto protocolConformance = module->lookupConformance(
      encoderNominal->getDeclaredInterfaceType(), encoderProto);

  if (protocolConformance.isInvalid()) {
    return false;
  }

  // === Check modifiers
  // --- must not be async
  if (hasAsync()) {
    return false;
  }

  // --- must be throwing
  if (!hasThrows()) {
    return false;
  }

  // --- must be mutating, if it is defined in a struct
  if (isa<StructDecl>(getDeclContext()) &&
      !func->isMutating()) {
    return false;
  }

  // === Check generics
  if (!isGeneric()) {
    return false;
  }

  // --- Check number of generic parameters
  auto genericParams = getGenericParams();
  unsigned int expectedGenericParamNum = 1;

  if (genericParams->size() != expectedGenericParamNum) {
    return false;
  }

  // === Get the SerializationRequirement
  SmallPtrSet<ProtocolDecl*, 2> requirementProtos;
  if (!getDistributedSerializationRequirements(
          encoderNominal, encoderProto, requirementProtos)) {
    return false;
  }

  // -- Check number of generic requirements
  size_t serializationRequirementsNum = requirementProtos.size();
  size_t expectedRequirementsNum = serializationRequirementsNum;

  // === Check all parameters
  auto params = getParameters();
  if (params->size() != 1) {
    return false;
  }

  // --- Check parameter: _ argument
  auto argumentParam = params->get(0);
  if (!argumentParam->getArgumentName().is("")) {
    return false;
  }

  // === Check generic parameters in detail
  // --- Check: Argument: SerializationRequirement
  GenericTypeParamDecl *ArgumentParam = genericParams->getParams()[0];

  auto sig = getGenericSignature();

  SmallVector<Requirement, 2> reqs;
  SmallVector<InverseRequirement, 2> inverseReqs;
  sig->getRequirementsWithInverses(reqs, inverseReqs);
  assert(inverseReqs.empty() && "Non-copyable generics not supported here!");

  if (reqs.size() != expectedRequirementsNum) {
    return false;
  }

  // --- Check the expected requirements
  // --- all the Argument requirements ---
  // conforms_to: Argument Decodable
  // conforms_to: Argument Encodable
  // ...

  auto resultType = func->mapTypeIntoContext(argumentParam->getInterfaceType())
                        ->getMetatypeInstanceType();

  auto resultParamType = func->mapTypeIntoContext(
      ArgumentParam->getDeclaredInterfaceType());

  // The result of the function must be the `Res` generic argument.
  if (!resultType->isEqual(resultParamType)) {
    return false;
  }

  for (auto requirementProto : requirementProtos) {
    auto conformance = module->lookupConformance(resultType, requirementProto);
    if (conformance.isInvalid()) {
        return false;
    }
  }

  // === Check result type: Void
  if (!func->getResultInterfaceType()->isVoid()) {
    return false;
  }

  return true;
}

bool
AbstractFunctionDecl::isDistributedTargetInvocationEncoderRecordErrorType() const {
    auto &C = getASTContext();
    auto module = getParentModule();

    auto func = dyn_cast<FuncDecl>(this);
    if (!func) {
      return false;
    }

    // === Check base name
    if (getBaseIdentifier() != C.Id_recordErrorType) {
      return false;
    }

    // === Must be declared in a 'DistributedTargetInvocationEncoder' conforming type
    ProtocolDecl *encoderProto =
        C.getProtocol(KnownProtocolKind::DistributedTargetInvocationEncoder);

    auto encoderNominal = getDeclContext()->getSelfNominalTypeDecl();
    auto protocolConformance = module->lookupConformance(
        encoderNominal->getDeclaredInterfaceType(), encoderProto);

    if (protocolConformance.isInvalid()) {
      return false;
    }

    // === Check modifiers
    // --- must not be async
    if (hasAsync()) {
      return false;
    }

    // --- must be throwing
    if (!hasThrows()) {
      return false;
    }

    // --- must be mutating, if it is defined in a struct
    if (isa<StructDecl>(getDeclContext()) &&
        !func->isMutating()) {
      return false;
    }

    // === Check generics
    if (!isGeneric()) {
      return false;
    }

    // --- Check number of generic parameters
    auto genericParams = getGenericParams();
    unsigned int expectedGenericParamNum = 1;

    if (genericParams->size() != expectedGenericParamNum) {
      return false;
    }

    // === Check all parameters
    auto params = getParameters();
    if (params->size() != 1) {
      return false;
    }

    // --- Check parameter: _ errorType
    auto errorTypeParam = params->get(0);
    if (!errorTypeParam->getArgumentName().is("")) {
      return false;
    }

    // --- Check: Argument: SerializationRequirement
    auto sig = getGenericSignature();

    SmallVector<Requirement, 2> reqs;
    SmallVector<InverseRequirement, 2> inverseReqs;
    sig->getRequirementsWithInverses(reqs, inverseReqs);
    assert(inverseReqs.empty() && "Non-copyable generics not supported here!");

    if (reqs.size() != 1) {
      return false;
    }

    // === Check generic parameters in detail
    // --- Check: Err: Error
    GenericTypeParamDecl *ErrParam = genericParams->getParams()[0];
    auto ErrConformance = module->lookupConformance(
        mapTypeIntoContext(ErrParam->getDeclaredInterfaceType()),
        C.getProtocol(KnownProtocolKind::Error));
    if (ErrConformance.isInvalid()) {
      return false;
    }

    // --- Check requirement: conforms_to: Err Error
    auto errorReq = reqs[0];
    if (errorReq.getKind() != RequirementKind::Conformance) {
      return false;
    }
    if (!errorReq.getProtocolDecl()->isSpecificProtocol(KnownProtocolKind::Error)) {
      return false;
    }

    // === Check result type: Void
    if (!func->getResultInterfaceType()->isVoid()) {
      return false;
    }

    return true;
}

bool
AbstractFunctionDecl::isDistributedTargetInvocationDecoderDecodeNextArgument() const {
    auto &C = getASTContext();
    auto module = getParentModule();

    auto func = dyn_cast<FuncDecl>(this);
    if (!func) {
      return false;
    }

    // === Check base name
    if (getBaseIdentifier() != C.Id_decodeNextArgument) {
      return false;
    }

    // === Must be declared in a 'DistributedTargetInvocationEncoder' conforming type
    ProtocolDecl *decoderProto =
        C.getProtocol(KnownProtocolKind::DistributedTargetInvocationDecoder);

    auto decoderNominal = getDeclContext()->getSelfNominalTypeDecl();
    auto protocolConformance = module->lookupConformance(
        decoderNominal->getDeclaredInterfaceType(), decoderProto);

    if (protocolConformance.isInvalid()) {
      return false;
    }

    // === Check modifiers
    // --- must not be async
    if (hasAsync()) {
      return false;
    }

    // --- must be throwing
    if (!hasThrows()) {
      return false;
    }

    // --- must be mutating, if it is defined in a struct
    if (isa<StructDecl>(getDeclContext()) &&
        !func->isMutating()) {
      return false;
    }


    // === Check generics
    if (!isGeneric()) {
      return false;
    }

    // --- Check number of generic parameters
    auto genericParams = getGenericParams();
    unsigned int expectedGenericParamNum = 1;

    if (genericParams->size() != expectedGenericParamNum) {
      return false;
    }

    // === Get the SerializationRequirement
    SmallPtrSet<ProtocolDecl*, 2> requirementProtos;
    if (!getDistributedSerializationRequirements(
            decoderNominal, decoderProto, requirementProtos)) {
      return false;
    }

    // === No parameters
    auto params = getParameters();
    if (params->size() != 0) {
      return false;
    }

    // === Check generic parameters in detail
    // --- Check: Argument: SerializationRequirement
    GenericTypeParamDecl *ArgumentParam = genericParams->getParams()[0];
    auto resultType = func->mapTypeIntoContext(func->getResultInterfaceType())
                          ->getMetatypeInstanceType();
    auto resultParamType = func->mapTypeIntoContext(
        ArgumentParam->getDeclaredInterfaceType());
    // The result of the function must be the `Res` generic argument.
    if (!resultType->isEqual(resultParamType)) {
      return false;
    }

    for (auto requirementProto : requirementProtos) {
      auto conformance =
          module->lookupConformance(resultType, requirementProto);
      if (conformance.isInvalid()) {
          return false;
      }
    }

    return true;
}

bool
AbstractFunctionDecl::isDistributedTargetInvocationResultHandlerOnReturn() const {
    auto &C = getASTContext();
    auto module = getParentModule();

    auto func = dyn_cast<FuncDecl>(this);
    if (!func) {
      return false;
    }

    // === Check base name
    if (getBaseIdentifier() != C.Id_onReturn) {
      return false;
    }

    // === Must be declared in a 'DistributedTargetInvocationEncoder' conforming type
    ProtocolDecl *decoderProto =
        C.getProtocol(KnownProtocolKind::DistributedTargetInvocationResultHandler);

    auto decoderNominal = getDeclContext()->getSelfNominalTypeDecl();
    auto protocolConformance = module->lookupConformance(
        decoderNominal->getDeclaredInterfaceType(), decoderProto);

    if (protocolConformance.isInvalid()) {
      return false;
    }

    // === Check modifiers
    // --- must be async
    if (!hasAsync()) {
      return false;
    }

    // --- must be throwing
    if (!hasThrows()) {
      return false;
    }

    // --- must not be mutating
    if (func->isMutating()) {
      return false;
    }

    // === Check generics
    if (!isGeneric()) {
      return false;
    }

    // --- Check number of generic parameters
    auto genericParams = getGenericParams();
    unsigned int expectedGenericParamNum = 1;

    if (genericParams->size() != expectedGenericParamNum) {
      return false;
    }

    // === Get the SerializationRequirement
    SmallPtrSet<ProtocolDecl *, 2> requirementProtos;
    if (!getDistributedSerializationRequirements(decoderNominal, decoderProto,
                                                 requirementProtos)) {
      return false;
    }

    // === Check all parameters
    auto params = getParameters();
    if (params->size() != 1) {
      return false;
    }

    // === Check parameter: value: Res
    auto valueParam = params->get(0);
    if (!valueParam->getArgumentName().is("value")) {
      return false;
    }

    // === Check generic parameters in detail
    // --- Check: Argument: SerializationRequirement
    GenericTypeParamDecl *ArgumentParam = genericParams->getParams()[0];
    auto argumentType = func->mapTypeIntoContext(
        valueParam->getInterfaceType()->getMetatypeInstanceType());
    auto resultParamType = func->mapTypeIntoContext(
        ArgumentParam->getDeclaredInterfaceType());
    // The result of the function must be the `Res` generic argument.
    if (!argumentType->isEqual(resultParamType)) {
      return false;
    }

    for (auto requirementProto : requirementProtos) {
      auto conformance =
          module->lookupConformance(argumentType, requirementProto);
      if (conformance.isInvalid()) {
        return false;
      }
    }

    if (!func->getResultInterfaceType()->isVoid()) {
      return false;
    }

    return true;
}

/******************************************************************************/
/********************** Distributed Functions *********************************/
/******************************************************************************/

bool AbstractFunctionDecl::isDistributed() const {
  return getAttrs().hasAttribute<DistributedActorAttr>();
}

bool AbstractStorageDecl::isDistributed() const {
  return getAttrs().hasAttribute<DistributedActorAttr>();
}

ConstructorDecl *
NominalTypeDecl::getDistributedRemoteCallTargetInitFunction() const {
  auto mutableThis = const_cast<NominalTypeDecl *>(this);
  return evaluateOrDefault(
      getASTContext().evaluator,
      GetDistributedRemoteCallTargetInitFunctionRequest(mutableThis), nullptr);
}

ConstructorDecl *
NominalTypeDecl::getDistributedRemoteCallArgumentInitFunction() const {
  auto mutableThis = const_cast<NominalTypeDecl *>(this);
  return evaluateOrDefault(
      getASTContext().evaluator,
      GetDistributedRemoteCallArgumentInitFunctionRequest(mutableThis),
      nullptr);
}

AbstractFunctionDecl *ASTContext::getRemoteCallOnDistributedActorSystem(
    NominalTypeDecl *actorOrSystem, bool isVoidReturn) const {
  assert(actorOrSystem && "distributed actor (or system) decl must be provided");
  const NominalTypeDecl *system = actorOrSystem;
  if (actorOrSystem->isDistributedActor()) {
    if (auto systemTy =
            getConcreteReplacementForProtocolActorSystemType(actorOrSystem)) {
      system = systemTy->getNominalOrBoundGenericNominal();
    }
  }

  // If no concrete system was found, return the general protocol:
  if (!system)
    system = getProtocol(KnownProtocolKind::DistributedActorSystem);

  auto mutableSystem = const_cast<NominalTypeDecl *>(system);
  return evaluateOrDefault(
      system->getASTContext().evaluator,
      GetDistributedActorSystemRemoteCallFunctionRequest{mutableSystem, /*isVoidReturn=*/isVoidReturn},
      nullptr);
}

/******************************************************************************/
/********************** Distributed Actor Properties **************************/
/******************************************************************************/

FuncDecl *AbstractStorageDecl::getDistributedThunk() const {
  if (!isDistributed())
    return nullptr;

  auto mutableThis = const_cast<AbstractStorageDecl *>(this);
  return evaluateOrDefault(getASTContext().evaluator,
                           GetDistributedThunkRequest{mutableThis}, nullptr);
}

FuncDecl*
AbstractFunctionDecl::getDistributedThunk() const {
  if (!isDistributed())
    return nullptr;

  auto mutableThis = const_cast<AbstractFunctionDecl *>(this);
  return evaluateOrDefault(
      getASTContext().evaluator,
      GetDistributedThunkRequest{mutableThis},
      nullptr);
}

VarDecl*
NominalTypeDecl::getDistributedActorSystemProperty() const {
  if (!isDistributedActor())
    return nullptr;

  auto mutableThis = const_cast<NominalTypeDecl *>(this);
  return evaluateOrDefault(
      getASTContext().evaluator,
      GetDistributedActorSystemPropertyRequest{mutableThis},
      nullptr);
}

VarDecl*
NominalTypeDecl::getDistributedActorIDProperty() const {
  if (!isDistributedActor())
    return nullptr;

  auto mutableThis = const_cast<NominalTypeDecl *>(this);
  return evaluateOrDefault(
      getASTContext().evaluator,
      GetDistributedActorIDPropertyRequest{mutableThis},
      nullptr);
}
