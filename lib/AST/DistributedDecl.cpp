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
#include "swift/Basic/Assertions.h"
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
/******************* Distributed Actor Conformances ***************************/
/******************************************************************************/

bool swift::canSynthesizeDistributedActorCodableConformance(NominalTypeDecl *actor) {
  auto &C = actor->getASTContext();

  if (!actor->isDistributedActor())
    return false;

  return evaluateOrDefault(
      C.evaluator,
      CanSynthesizeDistributedActorCodableConformanceRequest{actor},
      false);
}

ExtensionDecl *
swift::findDistributedActorAsActorExtension(
    ProtocolDecl *distributedActorProto, ModuleDecl *module) {
  ASTContext &C = distributedActorProto->getASTContext();
  auto name = C.getIdentifier("__actorUnownedExecutor");
  auto results = distributedActorProto->lookupDirect(
      name, SourceLoc(),
      NominalTypeDecl::LookupDirectFlags::IncludeAttrImplements);
  for (auto result : results) {
    if (auto var = dyn_cast<VarDecl>(result)) {
      return dyn_cast<ExtensionDecl>(var->getDeclContext());
    }
  }

  return nullptr;
}

bool swift::isDistributedActorAsLocalActorComputedProperty(VarDecl *var) {
  auto &C = var->getASTContext();
  return var->getName() == C.Id_asLocalActor &&
         var->getDeclContext()->getSelfProtocolDecl() &&
         var->getDeclContext()->getSelfProtocolDecl()->isSpecificProtocol(
             KnownProtocolKind::DistributedActor);
}

VarDecl *
swift::getDistributedActorAsLocalActorComputedProperty(ModuleDecl *module) {
  auto &C = module->getASTContext();
  auto DA = C.getDistributedActorDecl();
  auto extension = findDistributedActorAsActorExtension(DA, module);

  if (!extension)
    return nullptr;

  for (auto decl : extension->getMembers()) {
    if (auto var = dyn_cast<VarDecl>(decl)) {
      if (isDistributedActorAsLocalActorComputedProperty(var)) {
        return var;
      }
    }
  }

  return nullptr;
}

ProtocolConformanceRef
swift::getDistributedActorAsActorConformanceRef(ASTContext &C) {
  auto distributedActorAsActorConformance =
      getDistributedActorAsActorConformance(C);

  auto actorProto = C.getProtocol(KnownProtocolKind::Actor);
  return ProtocolConformanceRef(actorProto, distributedActorAsActorConformance);
}
NormalProtocolConformance *
swift::getDistributedActorAsActorConformance(ASTContext &C) {
  auto distributedActorProtocol = C.getProtocol(KnownProtocolKind::DistributedActor);

  return evaluateOrDefault(
      C.evaluator,
      GetDistributedActorAsActorConformanceRequest{distributedActorProtocol},
      nullptr);
}

/******************************************************************************/
/************** Distributed Actor System Associated Types *********************/
/******************************************************************************/

// TODO(distributed): make into a request
Type swift::getConcreteReplacementForProtocolActorSystemType(
    ValueDecl *anyValue) {
  auto &C = anyValue->getASTContext();

  // FIXME(distributed): clean this up, we want a method that gets us AS type
  // given any value, but is this the best way?
  DeclContext *DC;
  if (auto nominal = dyn_cast<NominalTypeDecl>(anyValue)) {
    DC = nominal;
  } else if (auto extension = dyn_cast<ExtensionDecl>(anyValue)) {
    DC = extension->getExtendedNominal();
  } else {
    DC = anyValue->getDeclContext();
  }
  auto DA = C.getDistributedActorDecl();

  // === When declared inside an actor, we can get the type directly
  if (auto classDecl = DC->getSelfClassDecl()) {
    return getDistributedActorSystemType(classDecl);
  }

  /// === Maybe the value is declared in a protocol?
  if (auto protocol = DC->getSelfProtocolDecl()) {
    GenericSignature signature;
    if (auto *genericContext = anyValue->getAsGenericContext()) {
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
  return getAssociatedTypeOfDistributedSystemOfActor(actor, C.Id_ActorID);
}

static Type getTypeWitnessByName(NominalTypeDecl *type, ProtocolDecl *protocol,
                                 Identifier member) {
  if (!protocol)
    return ErrorType::get(type->getASTContext());

  auto module = type->getParentModule();
  Type selfType = type->getSelfInterfaceType();
  auto conformance = module->lookupConformance(selfType, protocol);
  if (!conformance || conformance.isInvalid())
    return Type();
  return conformance.getTypeWitnessByName(selfType, member);
}

Type swift::getDistributedActorSerializationType(
    DeclContext *actorOrExtension) {
  auto &ctx = actorOrExtension->getASTContext();
  auto resultTy = getAssociatedTypeOfDistributedSystemOfActor(
      actorOrExtension,
      ctx.Id_SerializationRequirement);

  // Protocols are allowed to either not provide a `SerializationRequirement`
  // at all or provide it in a conformance requirement.
  if ((!resultTy || resultTy->hasDependentMember()) &&
      actorOrExtension->getSelfProtocolDecl()) {
    auto sig = actorOrExtension->getGenericSignatureOfContext();

    auto actorProtocol = ctx.getProtocol(KnownProtocolKind::DistributedActor);
    if (!actorProtocol)
      return Type();

    auto serializationTy =
        actorProtocol->getAssociatedType(ctx.Id_SerializationRequirement)
            ->getDeclaredInterfaceType();

    return sig->getExistentialType(serializationTy);
  }

  return resultTy;
}

Type swift::getDistributedActorSystemSerializationType(
    NominalTypeDecl *system) {
  assert(!system->isDistributedActor());
  auto &ctx = system->getASTContext();
  return getTypeWitnessByName(system, ctx.getDistributedActorSystemDecl(),
                              ctx.Id_SerializationRequirement);
}

Type swift::getDistributedActorSystemActorIDType(NominalTypeDecl *system) {
  assert(!system->isDistributedActor());
  auto &ctx = system->getASTContext();
  return getTypeWitnessByName(system, ctx.getDistributedActorSystemDecl(),
                              ctx.Id_ActorID);
}

Type swift::getDistributedActorSystemResultHandlerType(
    NominalTypeDecl *system) {
  assert(!system->isDistributedActor());
  auto &ctx = system->getASTContext();
  return getTypeWitnessByName(system, ctx.getDistributedActorSystemDecl(),
                              ctx.Id_ResultHandler);
}

Type swift::getDistributedActorSystemInvocationEncoderType(NominalTypeDecl *system) {
  assert(!system->isDistributedActor());
  auto &ctx = system->getASTContext();
  return getTypeWitnessByName(system, ctx.getDistributedActorSystemDecl(),
                              ctx.Id_InvocationEncoder);
}

Type swift::getDistributedActorSystemInvocationDecoderType(NominalTypeDecl *system) {
  assert(!system->isDistributedActor());
  auto &ctx = system->getASTContext();
  return getTypeWitnessByName(system, ctx.getDistributedActorSystemDecl(),
                              ctx.Id_InvocationDecoder);
}

Type swift::getDistributedSerializationRequirementType(
    NominalTypeDecl *nominal, ProtocolDecl *protocol) {
  assert(nominal);
  auto &ctx = nominal->getASTContext();

  if (!protocol)
    return Type();

  // Dig out the serialization requirement type.
  auto module = nominal->getParentModule();
  Type selfType = nominal->getSelfInterfaceType();
  auto conformance = module->lookupConformance(selfType, protocol);
  if (conformance.isInvalid())
    return Type();

  return conformance.getTypeWitnessByName(selfType,
                                          ctx.Id_SerializationRequirement);
}

AbstractFunctionDecl *
swift::getAssociatedDistributedInvocationDecoderDecodeNextArgumentFunction(
    ValueDecl *thunk) {
  assert(thunk);
  auto *actor = thunk->getDeclContext()->getSelfNominalTypeDecl();
  if (!actor)
    return nullptr;
  if (!actor->isDistributedActor())
    return nullptr;

  auto systemTy = getConcreteReplacementForProtocolActorSystemType(thunk);
  if (!systemTy || systemTy->is<GenericTypeParamType>())
    return nullptr;

  auto decoderTy =
      getDistributedActorSystemInvocationDecoderType(
          systemTy->getAnyNominal());
  if (!decoderTy)
    return nullptr;

  return getDecodeNextArgumentOnDistributedInvocationDecoder(
      decoderTy->getAnyNominal());
}

Type swift::getAssociatedTypeOfDistributedSystemOfActor(
    DeclContext *actorOrExtension, Identifier member) {
  auto &ctx = actorOrExtension->getASTContext();

  auto actorProtocol = ctx.getProtocol(KnownProtocolKind::DistributedActor);
  if (!actorProtocol)
    return Type();

  AssociatedTypeDecl *actorSystemDecl =
      actorProtocol->getAssociatedType(ctx.Id_ActorSystem);
  if (!actorSystemDecl)
    return Type();

  auto actorSystemProtocol = ctx.getDistributedActorSystemDecl();
  if (!actorSystemProtocol)
    return Type();

  AssociatedTypeDecl *memberTypeDecl =
      actorSystemProtocol->getAssociatedType(member);
  if (!memberTypeDecl)
    return Type();

  Type memberTy = DependentMemberType::get(
    DependentMemberType::get(actorProtocol->getSelfInterfaceType(),
                             actorSystemDecl),
    memberTypeDecl);

  auto sig = actorOrExtension->getGenericSignatureOfContext();

  auto *actorType = actorOrExtension->getSelfNominalTypeDecl();
  if (isa<ProtocolDecl>(actorType))
    return memberTy->getReducedType(sig);

  auto actorConformance =
      actorOrExtension->getParentModule()->lookupConformance(
          actorType->getDeclaredInterfaceType(), actorProtocol);
  if (!actorConformance || actorConformance.isInvalid())
    return Type();

  auto subs = SubstitutionMap::getProtocolSubstitutions(
      actorProtocol, actorType->getDeclaredInterfaceType(), actorConformance);

  memberTy = memberTy.subst(subs);

  // If substitution is still not fully resolved, let's see if we can
  // find a concrete replacement in the generic signature.
  if (memberTy->hasTypeParameter() && sig) {
    if (auto concreteTy = sig->getConcreteType(memberTy))
      return concreteTy;
  }

  return memberTy;
}

/******************************************************************************/
/******** Functions on DistributedActorSystem and friends *********************/
/******************************************************************************/

FuncDecl *
swift::getDistributedActorArgumentDecodingMethod(NominalTypeDecl *actor) {
  auto &ctx = actor->getASTContext();
  return evaluateOrDefault(
      ctx.evaluator,
      GetDistributedActorConcreteArgumentDecodingMethodRequest{actor}, nullptr);
}

NominalTypeDecl *
swift::getDistributedActorInvocationDecoder(NominalTypeDecl *actor) {
  if (!actor->isDistributedActor())
    return nullptr;

  auto &ctx = actor->getASTContext();
  return evaluateOrDefault(ctx.evaluator,
                           GetDistributedActorInvocationDecoderRequest{actor},
                           nullptr);
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

// TODO(distributed): probably can be removed?
llvm::ArrayRef<ValueDecl *>
AbstractFunctionDecl::getDistributedMethodWitnessedProtocolRequirements() const {
  auto mutableThis = const_cast<AbstractFunctionDecl *>(this);

  // Only a 'distributed' decl can witness 'distributed' protocol
  if (!isDistributed()) {
    return llvm::ArrayRef<ValueDecl *>();
  }

  return evaluateOrDefault(
      getASTContext().evaluator,
      GetDistributedMethodWitnessedProtocolRequirements(mutableThis), {});
}

/******************************************************************************/
/********************* Ad-hoc protocol requirement checks *********************/
/******************************************************************************/

bool AbstractFunctionDecl::isDistributedActorSystemRemoteCall(bool isVoidReturn) const {
  auto &C = getASTContext();
  auto module = getParentModule();
  auto *DC = getDeclContext();

  if (!DC->isTypeContext() || !isGeneric())
    return false;

  // === Check the name
  auto callId = isVoidReturn ? C.Id_remoteCallVoid : C.Id_remoteCall;
  if (getBaseName() != callId) {
    return false;
  }

  // === Must be declared in a 'DistributedActorSystem' conforming type
  ProtocolDecl *systemProto =
      C.getDistributedActorSystemDecl();

  auto systemNominal = DC->getSelfNominalTypeDecl();
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

bool ValueDecl::isDistributed() const {
  return getAttrs().hasAttribute<DistributedActorAttr>();
}

bool ValueDecl::isDistributedGetAccessor() const {
  if (auto accessor = dyn_cast<AccessorDecl>(this)) {
    if (accessor->getAccessorKind() == AccessorKind::DistributedGet) {
      return true;
    }
  }

  return false;
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

AbstractFunctionDecl *
swift::getRemoteCallOnDistributedActorSystem(NominalTypeDecl *actorOrSystem,
                                             bool isVoidReturn) {
  assert(actorOrSystem && "distributed actor (or system) decl must be provided");
  const NominalTypeDecl *system = actorOrSystem;
  if (actorOrSystem->isDistributedActor()) {
    if (auto systemTy =
            getConcreteReplacementForProtocolActorSystemType(actorOrSystem)) {
      system = systemTy->getNominalOrBoundGenericNominal();
    }
  }

  auto &ctx = actorOrSystem->getASTContext();

  // If no concrete system was found, return the general protocol:
  if (!system)
    system = ctx.getProtocol(KnownProtocolKind::DistributedActorSystem);

  auto mutableSystem = const_cast<NominalTypeDecl *>(system);
  return evaluateOrDefault(ctx.evaluator,
                           GetDistributedActorSystemRemoteCallFunctionRequest{
                               mutableSystem, /*isVoidReturn=*/isVoidReturn},
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
  if (isDistributedThunk())
    return const_cast<FuncDecl *>(dyn_cast<FuncDecl>(this));

  auto mutableThis = const_cast<AbstractFunctionDecl *>(this);

  // For an accessor, get the Storage (VarDecl) and get the thunk off it.
  //
  // Since only 'get' computed distributed properties are allowed, we know
  // this will be the equivalent 'get' thunk for this AccessorDecl.
  //
  // The AccessorDecl is not marked distributed, but the VarDecl will be.
  if (auto accessor = dyn_cast<AccessorDecl>(mutableThis)) {
    auto Storage = accessor->getStorage();
    return Storage->getDistributedThunk();
  }

  if (!isDistributed())
    return nullptr;

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

FuncDecl *swift::getMakeInvocationEncoderOnDistributedActorSystem(
    AbstractFunctionDecl *thunk) {
  auto &ctx = thunk->getASTContext();
  auto systemTy = getConcreteReplacementForProtocolActorSystemType(thunk);
  assert(systemTy && "No specific ActorSystem type found!");

  auto systemNominal = systemTy->getNominalOrBoundGenericNominal();
  assert(systemNominal && "No system nominal type found!");

  for (auto result :
       systemNominal->lookupDirect(ctx.Id_makeInvocationEncoder)) {
    auto *func = dyn_cast<FuncDecl>(result);
    if (func && func->isDistributedActorSystemMakeInvocationEncoder()) {
      return func;
    }
  }

  return nullptr;
}

FuncDecl *swift::getRecordGenericSubstitutionOnDistributedInvocationEncoder(
    NominalTypeDecl *nominal) {
  if (!nominal)
    return nullptr;

  auto &ctx = nominal->getASTContext();
  for (auto result : nominal->lookupDirect(ctx.Id_recordGenericSubstitution)) {
    auto *func = dyn_cast<FuncDecl>(result);
    if (func &&
        func->isDistributedTargetInvocationEncoderRecordGenericSubstitution()) {
      return func;
    }
  }

  return nullptr;
}

AbstractFunctionDecl *swift::getRecordArgumentOnDistributedInvocationEncoder(
    NominalTypeDecl *nominal) {
  if (!nominal)
    return nullptr;

  return evaluateOrDefault(
      nominal->getASTContext().evaluator,
      GetDistributedTargetInvocationEncoderRecordArgumentFunctionRequest{
          nominal},
      nullptr);
}

AbstractFunctionDecl *swift::getRecordReturnTypeOnDistributedInvocationEncoder(
    NominalTypeDecl *nominal) {
  if (!nominal)
    return nullptr;

  return evaluateOrDefault(
      nominal->getASTContext().evaluator,
      GetDistributedTargetInvocationEncoderRecordReturnTypeFunctionRequest{
          nominal},
      nullptr);
}

AbstractFunctionDecl *swift::getRecordErrorTypeOnDistributedInvocationEncoder(
    NominalTypeDecl *nominal) {
  if (!nominal)
    return nullptr;

  return evaluateOrDefault(
      nominal->getASTContext().evaluator,
      GetDistributedTargetInvocationEncoderRecordErrorTypeFunctionRequest{
          nominal},
      nullptr);
}

AbstractFunctionDecl *
swift::getDecodeNextArgumentOnDistributedInvocationDecoder(
    NominalTypeDecl *nominal) {
  if (!nominal)
    return nullptr;

  return evaluateOrDefault(
      nominal->getASTContext().evaluator,
      GetDistributedTargetInvocationDecoderDecodeNextArgumentFunctionRequest{
          nominal},
      nullptr);
}

AbstractFunctionDecl *
swift::getOnReturnOnDistributedTargetInvocationResultHandler(
    NominalTypeDecl *nominal) {
  if (!nominal)
    return nullptr;

  return evaluateOrDefault(
      nominal->getASTContext().evaluator,
      GetDistributedTargetInvocationResultHandlerOnReturnFunctionRequest{
          nominal},
      nullptr);
}

FuncDecl *swift::getDoneRecordingOnDistributedInvocationEncoder(
    NominalTypeDecl *nominal) {
  auto &ctx = nominal->getASTContext();

  llvm::SmallVector<ValueDecl *, 2> results;
  nominal->lookupQualified(nominal, DeclNameRef(ctx.Id_doneRecording),
                           SourceLoc(), NL_QualifiedDefault, results);
  for (auto result : results) {
    auto *fd = dyn_cast<FuncDecl>(result);
    if (!fd)
      continue;

    if (fd->getParameters()->size() != 0)
      continue;

    if (fd->getResultInterfaceType()->isVoid() && fd->hasThrows() &&
        !fd->hasAsync())
      return fd;
  }

  return nullptr;
}
