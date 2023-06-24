//===--- TypeCheckDistributed.cpp - Distributed ---------------------------===//
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
//
// This file implements type checking support for Swift's concurrency model.
//
//===----------------------------------------------------------------------===//
#include "TypeCheckConcurrency.h"
#include "TypeCheckDistributed.h"
#include "TypeChecker.h"
#include "swift/Strings.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/DistributedDecl.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeVisitor.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/Basic/Defer.h"

using namespace swift;

// ==== ------------------------------------------------------------------------

bool swift::ensureDistributedModuleLoaded(Decl *decl) {
  auto &C = decl->getASTContext();
  auto moduleAvailable = evaluateOrDefault(
      C.evaluator, DistributedModuleIsAvailableRequest{decl}, false);
  return moduleAvailable;
}

bool
DistributedModuleIsAvailableRequest::evaluate(Evaluator &evaluator,
                                              Decl *decl) const {
  auto &C = decl->getASTContext();

  if (C.getLoadedModule(C.Id_Distributed))
    return true;

  // seems we're missing the Distributed module, ask to import it explicitly
  decl->diagnose(diag::distributed_actor_needs_explicit_distributed_import);
  return false;
}

/******************************************************************************/
/************ LOCATING AD-HOC PROTOCOL REQUIREMENT IMPLS **********************/
/******************************************************************************/

static AbstractFunctionDecl *findDistributedAdHocRequirement(
    NominalTypeDecl *decl, Identifier identifier,
    std::function<bool(AbstractFunctionDecl *)> matchFn) {
  auto &C = decl->getASTContext();

  // It would be nice to check if this is a DistributedActorSystem
  // "conforming" type, but we can't do this as we invoke this function WHILE
  // deciding if the type conforms or not;

  // Not via `ensureDistributedModuleLoaded` to avoid generating a warning,
  // we won't be emitting the offending decl after all.
  if (!C.getLoadedModule(C.Id_Distributed)) {
    return nullptr;
  }

  for (auto value : decl->lookupDirect(identifier)) {
    auto func = dyn_cast<AbstractFunctionDecl>(value);
    if (func && matchFn(func))
      return func;
  }

  return nullptr;
}

AbstractFunctionDecl *
GetDistributedActorSystemRemoteCallFunctionRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *decl, bool isVoidReturn) const {
  auto &C = decl->getASTContext();
  auto callId = isVoidReturn ? C.Id_remoteCallVoid : C.Id_remoteCall;

  return findDistributedAdHocRequirement(
      decl, callId, [isVoidReturn](AbstractFunctionDecl *func) {
        return func->isDistributedActorSystemRemoteCall(isVoidReturn);
      });
}

AbstractFunctionDecl *
GetDistributedTargetInvocationEncoderRecordArgumentFunctionRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *decl) const {
  auto &C = decl->getASTContext();

  return findDistributedAdHocRequirement(
      decl, C.Id_recordArgument, [](AbstractFunctionDecl *func) {
        return func->isDistributedTargetInvocationEncoderRecordArgument();
      });
}

AbstractFunctionDecl *
GetDistributedTargetInvocationEncoderRecordReturnTypeFunctionRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *decl) const {
  auto &C = decl->getASTContext();

  return findDistributedAdHocRequirement(
      decl, C.Id_recordReturnType, [](AbstractFunctionDecl *func) {
        return func->isDistributedTargetInvocationEncoderRecordReturnType();
      });
}

AbstractFunctionDecl *
GetDistributedTargetInvocationEncoderRecordErrorTypeFunctionRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *decl) const {
  auto &C = decl->getASTContext();
  return findDistributedAdHocRequirement(
      decl, C.Id_recordErrorType, [](AbstractFunctionDecl *func) {
        return func->isDistributedTargetInvocationEncoderRecordErrorType();
      });
}

AbstractFunctionDecl *
GetDistributedTargetInvocationDecoderDecodeNextArgumentFunctionRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *decl) const {
  auto &C = decl->getASTContext();
  return findDistributedAdHocRequirement(
      decl, C.Id_decodeNextArgument, [](AbstractFunctionDecl *func) {
        return func->isDistributedTargetInvocationDecoderDecodeNextArgument();
      });
}

AbstractFunctionDecl *
GetDistributedTargetInvocationResultHandlerOnReturnFunctionRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *decl) const {
  auto &C = decl->getASTContext();
  return findDistributedAdHocRequirement(
      decl, C.Id_onReturn, [](AbstractFunctionDecl *func) {
        return func->isDistributedTargetInvocationResultHandlerOnReturn();
      });
}

// ==== ------------------------------------------------------------------------

/// Add Fix-It text for the given protocol type to inherit DistributedActor.
void swift::diagnoseDistributedFunctionInNonDistributedActorProtocol(
    const ProtocolDecl *proto, InFlightDiagnostic &diag) {
  if (proto->getInherited().empty()) {
    SourceLoc fixItLoc = proto->getBraces().Start;
    diag.fixItInsert(fixItLoc, ": DistributedActor");
  } else {
    // Similar to how Sendable FitIts do this, we insert at the end of
    // the inherited types.
    ASTContext &ctx = proto->getASTContext();
    SourceLoc fixItLoc = proto->getInherited().back().getSourceRange().End;
    fixItLoc = Lexer::getLocForEndOfToken(ctx.SourceMgr, fixItLoc);
    diag.fixItInsert(fixItLoc, ", DistributedActor");
  }
}


/// Add Fix-It text for the given nominal type to adopt Codable.
///
/// Useful when 'Codable' is the 'SerializationRequirement' and a non-Codable
/// function parameter or return value type is detected.
void swift::addCodableFixIt(
    const NominalTypeDecl *nominal, InFlightDiagnostic &diag) {
  if (nominal->getInherited().empty()) {
    SourceLoc fixItLoc = nominal->getBraces().Start;
    diag.fixItInsert(fixItLoc, ": Codable");
  } else {
    ASTContext &ctx = nominal->getASTContext();
    SourceLoc fixItLoc = nominal->getInherited().back().getSourceRange().End;
    fixItLoc = Lexer::getLocForEndOfToken(ctx.SourceMgr, fixItLoc);
    diag.fixItInsert(fixItLoc, ", Codable");
  }
}

// ==== ------------------------------------------------------------------------

bool IsDistributedActorRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *nominal) const {
  // Protocols are actors if they inherit from `DistributedActor`.
  if (auto protocol = dyn_cast<ProtocolDecl>(nominal)) {
    auto &ctx = protocol->getASTContext();
    auto *distributedActorProtocol = ctx.getDistributedActorDecl();
    return (protocol == distributedActorProtocol ||
            protocol->inheritsFrom(distributedActorProtocol));
  }

  // Class declarations are 'distributed actors' if they are declared with
  // 'distributed actor'
  auto classDecl = dyn_cast<ClassDecl>(nominal);
  if(!classDecl)
    return false;

  return classDecl->isExplicitDistributedActor();
}

// ==== ------------------------------------------------------------------------

static bool checkAdHocRequirementAccessControl(
    NominalTypeDecl *decl,
    ProtocolDecl *proto,
    AbstractFunctionDecl *func) {
  if (!func)
    return true;

  // === check access control
  // TODO(distributed): this is for ad-hoc requirements and is likely too naive
  if (func->getEffectiveAccess() == decl->getEffectiveAccess()) {
    return false;
  }

  func->diagnose(diag::witness_not_accessible_type,
                 diag::RequirementKind::Func,
                 func->getName(),
                 /*isSetter=*/false,
                 /*requiredAccess=*/AccessLevel::Public,
                 AccessLevel::Public,
                 proto->getName());
      return true;
}

bool swift::checkDistributedActorSystemAdHocProtocolRequirements(
    ASTContext &C,
    ProtocolDecl *Proto,
    NormalProtocolConformance *Conformance,
    Type Adoptee,
    bool diagnose) {
  auto decl = Adoptee->getAnyNominal();
  auto anyMissingAdHocRequirements = false;

  // ==== ----------------------------------------------------------------------
  // Check the ad-hoc requirements of 'DistributedActorSystem":
  if (Proto->isSpecificProtocol(KnownProtocolKind::DistributedActorSystem)) {
    // - remoteCall
    auto remoteCallDecl =
        C.getRemoteCallOnDistributedActorSystem(decl, /*isVoidReturn=*/false);
    if (!remoteCallDecl && diagnose) {
      auto identifier = C.Id_remoteCall;
      decl->diagnose(
          diag::distributed_actor_system_conformance_missing_adhoc_requirement,
          decl->getDescriptiveKind(), decl->getName(), identifier);
      decl->diagnose(
          diag::note_distributed_actor_system_conformance_missing_adhoc_requirement,
          Proto->getName(), identifier,
          "func remoteCall<Act, Err, Res>(\n"
          "    on actor: Act,\n"
          "    target: RemoteCallTarget,\n"
          "    invocation: inout InvocationEncoder,\n"
          "    throwing: Err.Type,\n"
          "    returning: Res.Type\n"
          ") async throws -> Res\n"
          "  where Act: DistributedActor,\n"
          "        Act.ID == ActorID,\n"
          "        Err: Error,\n"
          "        Res: SerializationRequirement\n");
      anyMissingAdHocRequirements = true;
    }
    if (checkAdHocRequirementAccessControl(decl, Proto, remoteCallDecl))
      anyMissingAdHocRequirements = true;

    // - remoteCallVoid
    auto remoteCallVoidDecl =
        C.getRemoteCallOnDistributedActorSystem(decl, /*isVoidReturn=*/true);
    if (!remoteCallVoidDecl && diagnose) {
      auto identifier = C.Id_remoteCallVoid;
      decl->diagnose(
          diag::distributed_actor_system_conformance_missing_adhoc_requirement,
          decl->getDescriptiveKind(), decl->getName(), identifier);
      decl->diagnose(
          diag::note_distributed_actor_system_conformance_missing_adhoc_requirement,
          Proto->getName(), identifier,
          "func remoteCallVoid<Act, Err>(\n"
          "    on actor: Act,\n"
          "    target: RemoteCallTarget,\n"
          "    invocation: inout InvocationEncoder,\n"
          "    throwing: Err.Type\n"
          ") async throws\n"
          "  where Act: DistributedActor,\n"
          "        Act.ID == ActorID,\n"
          "        Err: Error\n");
      anyMissingAdHocRequirements = true;
    }
    if (checkAdHocRequirementAccessControl(decl, Proto, remoteCallVoidDecl))
      anyMissingAdHocRequirements = true;

    return anyMissingAdHocRequirements;
  }

  // ==== ----------------------------------------------------------------------
  // Check the ad-hoc requirements of 'DistributedTargetInvocationEncoder'
  if (Proto->isSpecificProtocol(KnownProtocolKind::DistributedTargetInvocationEncoder)) {
    // - recordArgument
    auto recordArgumentDecl = C.getRecordArgumentOnDistributedInvocationEncoder(decl);
    if (!recordArgumentDecl) {
      auto identifier = C.Id_recordArgument;
      decl->diagnose(
          diag::distributed_actor_system_conformance_missing_adhoc_requirement,
          decl->getDescriptiveKind(), decl->getName(), identifier);
      decl->diagnose(diag::note_distributed_actor_system_conformance_missing_adhoc_requirement,
                     Proto->getName(), identifier,
                     "mutating func recordArgument<Value: SerializationRequirement>(_ argument: RemoteCallArgument<Value>) throws\n");
      anyMissingAdHocRequirements = true;
    }
    if (checkAdHocRequirementAccessControl(decl, Proto, recordArgumentDecl))
      anyMissingAdHocRequirements = true;

    // - recordReturnType
    auto recordReturnTypeDecl = C.getRecordReturnTypeOnDistributedInvocationEncoder(decl);
    if (!recordReturnTypeDecl) {
      auto identifier = C.Id_recordReturnType;
      decl->diagnose(
          diag::distributed_actor_system_conformance_missing_adhoc_requirement,
          decl->getDescriptiveKind(), decl->getName(), identifier);
      decl->diagnose(diag::note_distributed_actor_system_conformance_missing_adhoc_requirement,
                     Proto->getName(), identifier,
                     "mutating func recordReturnType<Res: SerializationRequirement>(_ resultType: Res.Type) throws\n");
      anyMissingAdHocRequirements = true;
    }
    if (checkAdHocRequirementAccessControl(decl, Proto, recordReturnTypeDecl))
      anyMissingAdHocRequirements = true;

    return anyMissingAdHocRequirements;
  }

  // ==== ----------------------------------------------------------------------
  // Check the ad-hoc requirements of 'DistributedTargetInvocationDecoder'
  if (Proto->isSpecificProtocol(KnownProtocolKind::DistributedTargetInvocationDecoder)) {
    // - decodeNextArgument
    auto decodeNextArgumentDecl = C.getDecodeNextArgumentOnDistributedInvocationDecoder(decl);
    if (!decodeNextArgumentDecl) {
      auto identifier = C.Id_decodeNextArgument;
      decl->diagnose(
          diag::distributed_actor_system_conformance_missing_adhoc_requirement,
          decl->getDescriptiveKind(), decl->getName(), identifier);
      decl->diagnose(diag::note_distributed_actor_system_conformance_missing_adhoc_requirement,
                     Proto->getName(), identifier,
                     "mutating func decodeNextArgument<Argument: SerializationRequirement>() throws -> Argument\n");
      anyMissingAdHocRequirements = true;
    }
    if (checkAdHocRequirementAccessControl(decl, Proto, decodeNextArgumentDecl))
      anyMissingAdHocRequirements = true;

    return anyMissingAdHocRequirements;
  }

  // === -----------------------------------------------------------------------
  // Check the ad-hoc requirements of 'DistributedTargetInvocationResultHandler'
  if (Proto->isSpecificProtocol(KnownProtocolKind::DistributedTargetInvocationResultHandler)) {
    // - onReturn
    auto onReturnDecl = C.getOnReturnOnDistributedTargetInvocationResultHandler(decl);
    if (!onReturnDecl) {
      auto identifier = C.Id_onReturn;
      decl->diagnose(
          diag::distributed_actor_system_conformance_missing_adhoc_requirement,
          decl->getDescriptiveKind(), decl->getName(), identifier);
      decl->diagnose(
          diag::note_distributed_actor_system_conformance_missing_adhoc_requirement,
          Proto->getName(), identifier,
          "func onReturn<Success: SerializationRequirement>(value: "
          "Success) async throws\n");
      anyMissingAdHocRequirements = true;
    }
    if (checkAdHocRequirementAccessControl(decl, Proto, onReturnDecl))
      anyMissingAdHocRequirements = true;

    return anyMissingAdHocRequirements;
  }

  assert(!anyMissingAdHocRequirements &&
         "Should have returned in appropriate type checking block earlier!");
  return false;
}

static bool checkDistributedTargetResultType(
    ModuleDecl *module, ValueDecl *valueDecl,
    const llvm::SmallPtrSetImpl<ProtocolDecl *> &serializationRequirements,
    bool diagnose) {
  auto &C = valueDecl->getASTContext();

  Type resultType;
  if (auto func = dyn_cast<FuncDecl>(valueDecl)) {
    resultType = func->mapTypeIntoContext(func->getResultInterfaceType());
  } else if (auto var = dyn_cast<VarDecl>(valueDecl)) {
    resultType = var->getInterfaceType();
  } else {
    llvm_unreachable("Unsupported distributed target");
  }

  if (resultType->isVoid())
    return false;

  auto isCodableRequirement =
      checkDistributedSerializationRequirementIsExactlyCodable(
          C, serializationRequirements);

  for(auto serializationReq : serializationRequirements) {
    auto conformance =
        TypeChecker::conformsToProtocol(resultType, serializationReq, module);
    if (conformance.isInvalid()) {
      if (diagnose) {
        llvm::StringRef conformanceToSuggest = isCodableRequirement ?
            "Codable" : // Codable is a typealias, easier to diagnose like that
            serializationReq->getNameStr();

        auto diag = valueDecl->diagnose(
            diag::distributed_actor_target_result_not_codable,
            resultType,
            valueDecl->getDescriptiveKind(),
            valueDecl->getBaseIdentifier(),
            conformanceToSuggest
        );

        if (isCodableRequirement) {
          if (auto resultNominalType = resultType->getAnyNominal()) {
            addCodableFixIt(resultNominalType, diag);
          }
        }
      } // end if: diagnose
      
      return true;
    }
  }

  return false;
}

bool swift::checkDistributedActorSystem(const NominalTypeDecl *system) {
  auto nominal = const_cast<NominalTypeDecl *>(system);

  // ==== Ensure the Distributed module is available,
  // without it there's no reason to check the decl in more detail anyway.
  if (!swift::ensureDistributedModuleLoaded(nominal))
    return true;

  auto &C = nominal->getASTContext();
  auto DAS = C.getDistributedActorSystemDecl();

  // === AssociatedTypes
  // --- SerializationRequirement MUST be a protocol TODO(distributed): rdar://91663941
  // we may lift this in the future and allow classes but this requires more
  // work to enable associatedtypes to be constrained to class or protocol,
  // which then will unlock using them as generic constraints in protocols.
  Type requirementTy = getDistributedSerializationRequirementType(nominal, DAS);

  if (auto existentialTy = requirementTy->getAs<ExistentialType>()) {
    requirementTy = existentialTy->getConstraintType();
  }

  if (auto alias = dyn_cast<TypeAliasType>(requirementTy.getPointer())) {
    auto concreteReqTy = alias->getDesugaredType();
    if (auto comp = dyn_cast<ProtocolCompositionType>(concreteReqTy)) {
      // ok, protocol composition is fine as requirement,
      // since special case of just a single protocol
    } else if (auto proto = dyn_cast<ProtocolType>(concreteReqTy)) {
      // ok, protocols is exactly what we want to be used as constraints here
    } else {
      nominal->diagnose(diag::distributed_actor_system_serialization_req_must_be_protocol,
                        requirementTy);
      return true;
    }
  }

  // all good, didn't find any errors
  return false;
}

/// Check whether the function is a proper distributed function
///
/// \returns \c true if there was a problem with adding the attribute, \c false
/// otherwise.
bool swift::checkDistributedFunction(AbstractFunctionDecl *func) {
  auto &C = func->getASTContext();
  return evaluateOrDefault(C.evaluator,
                           CheckDistributedFunctionRequest{func},
                           false); // no error if cycle
}

bool CheckDistributedFunctionRequest::evaluate(
    Evaluator &evaluator, AbstractFunctionDecl *func) const {
  if (auto *accessor = dyn_cast<AccessorDecl>(func)) {
    auto *var = cast<VarDecl>(accessor->getStorage());
    assert(var->isDistributed() && accessor->isGetter());
  } else {
    assert(func->isDistributed());
  }

  auto &C = func->getASTContext();
  auto DC = func->getDeclContext();
  auto module = func->getParentModule();

  /// If no distributed module is available, then no reason to even try checks.
  if (!C.getLoadedModule(C.Id_Distributed))
    return true;

  // === All parameters and the result type must conform
  // SerializationRequirement
  llvm::SmallPtrSet<ProtocolDecl *, 2> serializationRequirements;
  if (auto extension = dyn_cast<ExtensionDecl>(DC)) {
    serializationRequirements = extractDistributedSerializationRequirements(
        C, extension->getGenericRequirements());
  } else if (auto actor = dyn_cast<ClassDecl>(DC)) {
    serializationRequirements = getDistributedSerializationRequirementProtocols(
        getDistributedActorSystemType(actor)->getAnyNominal(),
        C.getProtocol(KnownProtocolKind::DistributedActorSystem));
  } else if (isa<ProtocolDecl>(DC)) {
    if (auto seqReqTy =
        getConcreteReplacementForMemberSerializationRequirement(func)) {
      auto seqReqTyDes = seqReqTy->castTo<ExistentialType>()->getConstraintType()->getDesugaredType();
      for (auto req : flattenDistributedSerializationTypeToRequiredProtocols(seqReqTyDes)) {
        serializationRequirements.insert(req);
      }
    }

    // The distributed actor constrained protocol has no serialization requirements
    // or actor system defined, so these will only be enforced, by implementations
    // of DAs conforming to it, skip checks here.
    if (serializationRequirements.empty()) {
      return false;
    }
  } else {
    llvm_unreachable("Distributed function detected in type other than extension, "
                     "distributed actor, or protocol! This should not be possible "
                     ", please file a bug.");
  }

  // If the requirement is exactly `Codable` we diagnose it ia bit nicer.
  auto serializationRequirementIsCodable =
      checkDistributedSerializationRequirementIsExactlyCodable(
          C, serializationRequirements);

  for (auto param : *func->getParameters()) {
    // --- Check parameters for 'Codable' conformance
    auto paramTy = func->mapTypeIntoContext(param->getInterfaceType());

    for (auto req : serializationRequirements) {
      if (TypeChecker::conformsToProtocol(paramTy, req, module).isInvalid()) {
        auto diag = func->diagnose(
            diag::distributed_actor_func_param_not_codable,
            param->getArgumentName().str(), param->getInterfaceType(),
            func->getDescriptiveKind(),
            serializationRequirementIsCodable ? "Codable"
                                              : req->getNameStr());

        if (auto paramNominalTy = paramTy->getAnyNominal()) {
          addCodableFixIt(paramNominalTy, diag);
        } // else, no nominal type to suggest the fixit for, e.g. a closure
        return true;
      }
    }

    // --- Check parameters for various illegal modifiers
    if (param->isInOut()) {
      param->diagnose(
          diag::distributed_actor_func_inout,
          param->getName(),
          func->getDescriptiveKind(), func->getName()
      ).fixItRemove(SourceRange(param->getTypeSourceRangeForDiagnostics().Start,
                                param->getTypeSourceRangeForDiagnostics().Start.getAdvancedLoc(1)));
      // FIXME(distributed): the fixIt should be on param->getSpecifierLoc(), but that Loc is invalid for some reason?
      return true;
    }

    if (param->getSpecifier() == ParamSpecifier::LegacyShared ||
        param->getSpecifier() == ParamSpecifier::LegacyOwned ||
        param->getSpecifier() == ParamSpecifier::Consuming ||
        param->getSpecifier() == ParamSpecifier::Borrowing) {
      param->diagnose(
          diag::distributed_actor_func_unsupported_specifier,
          ParamDecl::getSpecifierSpelling(param->getSpecifier()),
          param->getName(),
          func->getDescriptiveKind(),
          func->getName());
      return true;
    }

    if (param->isVariadic()) {
      param->diagnose(
          diag::distributed_actor_func_variadic,
          param->getName(),
          func->getDescriptiveKind(), func->getName()
      );
    }
  }

  // --- Result type must be either void or a codable type
  if (checkDistributedTargetResultType(module, func, serializationRequirements,
                                       /*diagnose=*/true)) {
    return true;
  }

  return false;
}

/// Check whether the function is a proper distributed computed property
///
/// \param diagnose Whether to emit a diagnostic when a problem is encountered.
///
/// \returns \c true if there was a problem with adding the attribute, \c false
/// otherwise.
bool swift::checkDistributedActorProperty(VarDecl *var, bool diagnose) {
  auto &C = var->getASTContext();
  auto DC = var->getDeclContext();

  // without the distributed module, we can't check any of these.
  if (!ensureDistributedModuleLoaded(var))
    return true;

  /// === Check if the declaration is a valid combination of attributes
  if (var->isStatic()) {
    if (diagnose)
      var->diagnose(diag::distributed_property_cannot_be_static,
                    var->getName());
    // TODO(distributed): fixit, offer removing the static keyword
    return true;
  }

  // it is not a computed property
  if (var->isLet() || var->hasStorageOrWrapsStorage()) {
    if (diagnose)
      var->diagnose(diag::distributed_property_can_only_be_computed,
                    var->getDescriptiveKind(), var->getName());
    return true;
  }

  // distributed properties cannot have setters
  if (var->getWriteImpl() != swift::WriteImplKind::Immutable) {
    if (diagnose)
      var->diagnose(diag::distributed_property_can_only_be_computed_get_only,
                    var->getName());
    return true;
  }

  auto systemVar =
      DC->getSelfNominalTypeDecl()->getDistributedActorSystemProperty();
  auto systemDecl = systemVar->getInterfaceType()->getAnyNominal();

  auto serializationRequirements =
      getDistributedSerializationRequirementProtocols(
          systemDecl,
          C.getProtocol(KnownProtocolKind::DistributedActorSystem));

  auto module = var->getModuleContext();
  if (checkDistributedTargetResultType(module, var, serializationRequirements, diagnose)) {
    return true;
  }

  return false;
}

void swift::checkDistributedActorProperties(const NominalTypeDecl *decl) {
  auto &C = decl->getASTContext();

  if (auto sourceFile = decl->getDeclContext()->getParentSourceFile()) {
    if (sourceFile->Kind == SourceFileKind::Interface) {
      // Don't diagnose properties in swiftinterfaces.
      return;
    }
  } else {
    // Don't diagnose when checking without source file (e.g. from module, importer etc).
    return;
  }

  if (isa<ProtocolDecl>(decl)) {
    // protocols don't matter for stored property checking
    return;
  }

  for (auto member : decl->getMembers()) {
    if (auto prop = dyn_cast<VarDecl>(member)) {
      if (prop->isSynthesized())
        continue;

      auto id = prop->getName();
      if (id == C.Id_actorSystem || id == C.Id_id) {
        prop->diagnose(diag::distributed_actor_user_defined_special_property,
                      id);
      }
    }
  }
}

// ==== ------------------------------------------------------------------------

void TypeChecker::checkDistributedActor(SourceFile *SF, NominalTypeDecl *nominal) {
  if (!nominal)
    return;

  // ==== Ensure the Distributed module is available,
  // without it there's no reason to check the decl in more detail anyway.
  if (!swift::ensureDistributedModuleLoaded(nominal))
    return;

  // ==== Constructors
  // --- Get the default initializer
  // If applicable, this will create the default 'init(transport:)' initializer
  (void)nominal->getDefaultInitializer();

  // We check decls for ambiguity more strictly than normal nominal types,
  // because we want to record distributed accessors the same if function they
  // point at (in a remote process) is async or not, as that has no effect on
  // a caller from a different process, so we want to make the remoteCall target
  // identifiers, less fragile against such refactorings.
  //
  // To achieve this, we ban overloads on just "effects" of functions,
  // which are useful in local settings, but really should not be relied
  // on as differenciators in remote calls - the call will always be "async"
  // since it will go through a thunk, and then be asynchronously transferred
  // to the called process.
  llvm::SmallDenseSet<DeclName, 2> diagnosedAmbiguity;

  for (auto member : nominal->getMembers()) {
    // --- Ensure 'distributed func' all thunks
    if (auto *var = dyn_cast<VarDecl>(member)) {
      if (!var->isDistributed())
        continue;

      if (auto thunk = var->getDistributedThunk())
        SF->DelayedFunctions.push_back(thunk);

      continue;
    }

    // --- Ensure 'distributed func' all thunks
    if (auto func = dyn_cast<AbstractFunctionDecl>(member)) {
      if (!func->isDistributed())
        continue;

      if (!isa<ProtocolDecl>(nominal)) {
        auto systemTy = getConcreteReplacementForProtocolActorSystemType(func);
        if (!systemTy || systemTy->hasError()) {
          nominal->diagnose(
              diag::distributed_actor_conformance_missing_system_type,
              nominal->getName());
          return;
        }

        // Check there's no async/no-async overloads, since those are more
        // fragile in distribution than we'd want distributed calls to be.
        // A remote call is always 'async throws', and we can always record
        // an async throws "accessor" (see AccessibleFunction.cpp) as such.
        // This means, if we allowed async/no-async overloads of functions,
        // we'd have to store the precise "it was not throwing" information,
        // but we'll _never_ make use of such because all remote calls are
        // necessarily going to async to the actor in the recipient process,
        // and for the remote caller, they are always as-if-async.
        //
        // By banning such overloads, which may be useful in local APIs,
        // but too fragile in distributed APIs, we allow a remote 'v2' version
        // of an implementation to add or remove `async` to their implementation
        // without breaking calls which were made on previous 'v1' versions of
        // the same interface; Callers are never broken this way, and rollouts
        // are simpler.
        //
        // The restriction on overloads is not a problem for distributed calls,
        // as we don't have a vast swab of APIs which must compatibly get async
        // versions, as that is what the async overloading aimed to address.
        //
        // Note also, that overloading on throws is already illegal anyway.
        if (!diagnosedAmbiguity.contains(func->getName())) {
          auto candidates = nominal->lookupDirect(func->getName());
          if (candidates.size() > 1) {
            auto firstDecl = dyn_cast<AbstractFunctionDecl>(candidates.back());
            for (auto decl : candidates) {
              if (decl == firstDecl) {
                decl->diagnose(
                    diag::distributed_func_cannot_overload_on_async_only,
                    decl->getName());
              } else {
                decl->diagnose(
                    diag::distributed_func_other_ambiguous_overload_here,
                    decl->getName());
              }
            }

            diagnosedAmbiguity.insert(func->getName());
          }
        }
      }

      if (auto thunk = func->getDistributedThunk()) {
        SF->DelayedFunctions.push_back(thunk);
      }
    }
  }

  // ==== Properties
  checkDistributedActorProperties(nominal);
  // --- Synthesize the 'id' property here rather than via derived conformance
  //     because the 'DerivedConformanceDistributedActor' won't trigger for 'id'
  //     because it has a default impl via 'Identifiable' (ObjectIdentifier)
  //     which we do not want.
  // Also, the 'id' var must be added before the 'actorSystem'.
  // See NOTE (id-before-actorSystem) for more details.
  (void)nominal->getDistributedActorIDProperty();
}

void TypeChecker::checkDistributedFunc(FuncDecl *func) {
  if (!func->isDistributed())
    return;

  swift::checkDistributedFunction(func);
}

llvm::SmallPtrSet<ProtocolDecl *, 2>
swift::getDistributedSerializationRequirementProtocols(
    NominalTypeDecl *nominal, ProtocolDecl *protocol) {
  if (!protocol || !nominal) {
    return {};
  }

  auto ty = getDistributedSerializationRequirementType(nominal, protocol);
  if (!ty || ty->hasError()) {
    return {};
  }

  auto serialReqType =
      ty->castTo<ExistentialType>()->getConstraintType()->getDesugaredType();

  // TODO(distributed): check what happens with Any
  return flattenDistributedSerializationTypeToRequiredProtocols(serialReqType);
}

ConstructorDecl*
GetDistributedRemoteCallTargetInitFunctionRequest::evaluate(
    Evaluator &evaluator,
    NominalTypeDecl *nominal) const {
  auto &C = nominal->getASTContext();

  // not via `ensureDistributedModuleLoaded` to avoid generating a warning,
  // we won't be emitting the offending decl after all.
  if (!C.getLoadedModule(C.Id_Distributed))
    return nullptr;

  if (!nominal->getDeclaredInterfaceType()->isEqual(
          C.getRemoteCallTargetType()))
    return nullptr;

  for (auto value : nominal->getMembers()) {
    auto ctor = dyn_cast<ConstructorDecl>(value);
    if (!ctor)
      continue;

    auto params = ctor->getParameters();
    if (params->size() != 1)
      return nullptr;

    // _ identifier
    if (params->get(0)->getArgumentName().empty())
      return ctor;

    return nullptr;
  }

  return nullptr;
}

ConstructorDecl*
GetDistributedRemoteCallArgumentInitFunctionRequest::evaluate(
    Evaluator &evaluator,
    NominalTypeDecl *nominal) const {
  auto &C = nominal->getASTContext();

  // not via `ensureDistributedModuleLoaded` to avoid generating a warning,
  // we won't be emitting the offending decl after all.
  if (!C.getLoadedModule(C.Id_Distributed))
    return nullptr;

  if (!nominal->getDeclaredInterfaceType()->isEqual(
          C.getRemoteCallArgumentType()))
    return nullptr;

  for (auto value : nominal->getMembers()) {
    auto ctor = dyn_cast<ConstructorDecl>(value);
    if (!ctor)
      continue;

    auto params = ctor->getParameters();
    if (params->size() != 3)
      return nullptr;

    // --- param: label
    if (!params->get(0)->getArgumentName().is("label"))
      return nullptr;

    // --- param: name
    if (!params->get(1)->getArgumentName().is("name"))
      return nullptr;

    // --- param: value
    if (params->get(2)->getArgumentName() != C.Id_value)
      return nullptr;

    return ctor;
  }

  return nullptr;
}

NominalTypeDecl *
GetDistributedActorInvocationDecoderRequest::evaluate(Evaluator &evaluator,
                                                      NominalTypeDecl *actor) const {
  auto &ctx = actor->getASTContext();
  auto decoderTy =
      ctx.getAssociatedTypeOfDistributedSystemOfActor(actor, ctx.Id_InvocationDecoder);
  return decoderTy->hasError() ? nullptr : decoderTy->getAnyNominal();
}

FuncDecl *
GetDistributedActorArgumentDecodingMethodRequest::evaluate(Evaluator &evaluator,
                                                           NominalTypeDecl *actor) const {
  auto &ctx = actor->getASTContext();

  auto *decoder = ctx.getDistributedActorInvocationDecoder(actor);
  assert(decoder);

  auto decoderTy = decoder->getDeclaredInterfaceType();

  auto members = TypeChecker::lookupMember(actor->getDeclContext(), decoderTy,
                                           DeclNameRef(ctx.Id_decodeNextArgument));

  // typealias SerializationRequirement = any ...
  llvm::SmallPtrSet<ProtocolDecl *, 2> serializationReqs =
      getDistributedSerializationRequirementProtocols(
          actor, ctx.getProtocol(KnownProtocolKind::DistributedActor));

  SmallVector<FuncDecl *, 2> candidates;
  // Looking for `decodeNextArgument<Arg: <SerializationReq>>() throws -> Arg`
  for (auto &member : members) {
    auto *FD = dyn_cast<FuncDecl>(member.getValueDecl());
    if (!FD || FD->hasAsync() || !FD->hasThrows())
      continue;

    auto *params = FD->getParameters();
    // No arguments.
    if (params->size() != 0)
      continue;

    auto genericParamList = FD->getGenericParams();
    // A single generic parameter.
    if (genericParamList->size() != 1)
      continue;

    auto paramTy = genericParamList->getParams()[0]
                       ->getInterfaceType()
                       ->getMetatypeInstanceType();

    // `decodeNextArgument` should return its generic parameter value
    if (!FD->getResultInterfaceType()->isEqual(paramTy))
      continue;

    // Let's find out how many serialization requirements does this method cover
    // e.g. `Codable` is two requirements - `Encodable` and `Decodable`.
    unsigned numSerializationReqsCovered = llvm::count_if(
        FD->getGenericRequirements(), [&](const Requirement &requirement) {
          if (!(requirement.getFirstType()->isEqual(paramTy) &&
                requirement.getKind() == RequirementKind::Conformance))
            return 0;

          return serializationReqs.count(requirement.getProtocolDecl()) ? 1 : 0;
        });

    // If the current method covers all of the serialization requirements,
    // it's a match. Note that it might also have other requirements, but
    // we let that go as long as there are no two candidates that differ
    // only in generic requirements.
    if (numSerializationReqsCovered == serializationReqs.size())
      candidates.push_back(FD);
  }

  // Type-checker should reject any definition of invocation decoder
  // that doesn't have a correct version of `decodeNextArgument` declared.
  assert(candidates.size() == 1);
  return candidates.front();
}
