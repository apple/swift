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
#include "swift/AST/ImportCache.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"
#include "swift/AST/ASTPrinter.h"

using namespace swift;

// ==== ------------------------------------------------------------------------

bool swift::ensureDistributedModuleLoaded(const ValueDecl *decl) {
  auto &C = decl->getASTContext();
  auto moduleAvailable = evaluateOrDefault(
      C.evaluator, DistributedModuleIsAvailableRequest{decl}, false);
  return moduleAvailable;
}

bool
DistributedModuleIsAvailableRequest::evaluate(Evaluator &evaluator,
                                              const ValueDecl *decl) const {
  auto &C = decl->getASTContext();

  auto DistributedModule = C.getLoadedModule(C.Id_Distributed);
  if (!DistributedModule) {
    decl->diagnose(diag::distributed_decl_needs_explicit_distributed_import,
                   decl)
        .fixItAddImport("Distributed");
    return false;
  }

  auto &importCache = C.getImportCache();
  if (importCache.isImportedBy(DistributedModule, decl->getDeclContext())) {
    return true;
  }

  // seems we're missing the Distributed module, ask to import it explicitly
  decl->diagnose(diag::distributed_decl_needs_explicit_distributed_import,
                 decl);
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

  llvm::SmallVector<ValueDecl *, 2> results;
  decl->lookupQualified(decl, DeclNameRef(identifier),
                        SourceLoc(), NL_QualifiedDefault, results);
  for (auto value : results) {
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
    SourceLoc fixItLoc = proto->getInherited().getEndLoc();
    diag.fixItInsertAfter(fixItLoc, ", DistributedActor");
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
    SourceLoc fixItLoc = nominal->getInherited().getEndLoc();
    diag.fixItInsertAfter(fixItLoc, ", Codable");
  }
}

// ==== ------------------------------------------------------------------------

bool IsDistributedActorRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *nominal) const {
  // Protocols are actors if they inherit from `DistributedActor`.
  if (auto protocol = dyn_cast<ProtocolDecl>(nominal)) {
    auto &ctx = protocol->getASTContext();
    auto *distributedActorProtocol = ctx.getDistributedActorDecl();
    if (!distributedActorProtocol)
      return false;

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
  if (func->getEffectiveAccess() == decl->getEffectiveAccess()) {
    return false;
  }

  func->diagnose(diag::witness_not_accessible_type, diag::RequirementKind::Func,
                 func, /*isSetter=*/false,
                 /*requiredAccess=*/AccessLevel::Public, AccessLevel::Public,
                 proto);
      return true;
}

static bool diagnoseMissingAdHocProtocolRequirement(ASTContext &C, Identifier identifier, NominalTypeDecl *decl) {
  assert(decl);
  auto FixitLocation = decl->getBraces().Start;

  // Prepare the indent (same as `printRequirementStub`)
  StringRef ExtraIndent;
  StringRef CurrentIndent =
      Lexer::getIndentationForLine(C.SourceMgr, decl->getStartLoc(), &ExtraIndent);

  llvm::SmallString<128> Text;
  llvm::raw_svector_ostream OS(Text);
  ExtraIndentStreamPrinter Printer(OS, CurrentIndent);

  Printer.printNewline();
  Printer.printIndent();
  Printer << (decl->getFormalAccess() == AccessLevel::Public ? "public " : "");

  if (identifier == C.Id_remoteCall) {
    Printer << "func remoteCall<Act, Err, Res>("
               "on actor: Act, "
               "target: RemoteCallTarget, "
               "invocation: inout InvocationEncoder, "
               "throwing: Err.Type, "
               "returning: Res.Type) "
               "async throws -> Res "
               "where Act: DistributedActor, "
               "Act.ID == ActorID, "
               "Err: Error, "
               "Res: SerializationRequirement";
  } else if (identifier == C.Id_remoteCallVoid) {
    Printer << "func remoteCallVoid<Act, Err>("
               "on actor: Act, "
               "target: RemoteCallTarget, "
               "invocation: inout InvocationEncoder, "
               "throwing: Err.Type"
               ") async throws "
               "where Act: DistributedActor, "
               "Act.ID == ActorID, "
               "Err: Error";
  } else if (identifier == C.Id_recordArgument) {
    Printer << "mutating func recordArgument<Value: SerializationRequirement>(_ argument: RemoteCallArgument<Value>) throws";
  } else if (identifier == C.Id_recordReturnType) {
    Printer << "mutating func recordReturnType<Res: SerializationRequirement>(_ resultType: Res.Type) throws";
  } else if (identifier == C.Id_decodeNextArgument) {
    Printer << "mutating func decodeNextArgument<Argument: SerializationRequirement>() throws -> Argument";
  } else if (identifier == C.Id_onReturn) {
    Printer << "func onReturn<Success: SerializationRequirement>(value: Success) async throws";
  } else {
    llvm_unreachable("Unknown identifier for diagnosing ad-hoc missing requirement.");
  }

  /// Print the "{ <#code#> }" placeholder body
  Printer << " {\n";
  Printer << ExtraIndent << getCodePlaceholder();
  Printer.printNewline();
  Printer.printIndent();
  Printer << "}\n";

  decl->diagnose(
      diag::distributed_actor_system_conformance_missing_adhoc_requirement,
      decl, identifier);
  decl->diagnose(diag::missing_witnesses_general)
      .fixItInsertAfter(FixitLocation, Text.str());

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
        getRemoteCallOnDistributedActorSystem(decl, /*isVoidReturn=*/false);
    if (!remoteCallDecl && diagnose) {
      anyMissingAdHocRequirements = diagnoseMissingAdHocProtocolRequirement(C, C.Id_remoteCall, decl);
    }
    if (checkAdHocRequirementAccessControl(decl, Proto, remoteCallDecl)) {
      anyMissingAdHocRequirements = true;
    }

    // - remoteCallVoid
    auto remoteCallVoidDecl =
        getRemoteCallOnDistributedActorSystem(decl, /*isVoidReturn=*/true);
    if (!remoteCallVoidDecl && diagnose) {
      anyMissingAdHocRequirements = diagnoseMissingAdHocProtocolRequirement(C, C.Id_remoteCallVoid, decl);
    }
    if (checkAdHocRequirementAccessControl(decl, Proto, remoteCallVoidDecl)) {
      anyMissingAdHocRequirements = true;
    }

    return anyMissingAdHocRequirements;
  }

  // ==== ----------------------------------------------------------------------
  // Check the ad-hoc requirements of 'DistributedTargetInvocationEncoder'
  if (Proto->isSpecificProtocol(KnownProtocolKind::DistributedTargetInvocationEncoder)) {
    // - recordArgument
    auto recordArgumentDecl =
        getRecordArgumentOnDistributedInvocationEncoder(decl);
    if (!recordArgumentDecl) {
      anyMissingAdHocRequirements = diagnoseMissingAdHocProtocolRequirement(C, C.Id_recordArgument, decl);
    }
    if (checkAdHocRequirementAccessControl(decl, Proto, recordArgumentDecl)) {
      anyMissingAdHocRequirements = true;
    }

    // - recordReturnType
    auto recordReturnTypeDecl =
        getRecordReturnTypeOnDistributedInvocationEncoder(decl);
    if (!recordReturnTypeDecl) {
      anyMissingAdHocRequirements = diagnoseMissingAdHocProtocolRequirement(C, C.Id_recordReturnType, decl);
    }
    if (checkAdHocRequirementAccessControl(decl, Proto, recordReturnTypeDecl)) {
      anyMissingAdHocRequirements = true;
    }

    return anyMissingAdHocRequirements;
  }

  // ==== ----------------------------------------------------------------------
  // Check the ad-hoc requirements of 'DistributedTargetInvocationDecoder'
  if (Proto->isSpecificProtocol(KnownProtocolKind::DistributedTargetInvocationDecoder)) {
    // - decodeNextArgument
    auto decodeNextArgumentDecl =
        getDecodeNextArgumentOnDistributedInvocationDecoder(decl);
    if (!decodeNextArgumentDecl) {
      anyMissingAdHocRequirements = diagnoseMissingAdHocProtocolRequirement(C, C.Id_decodeNextArgument, decl);
    }
    if (checkAdHocRequirementAccessControl(decl, Proto, decodeNextArgumentDecl)) {
      anyMissingAdHocRequirements = true;
    }

    return anyMissingAdHocRequirements;
  }

  // === -----------------------------------------------------------------------
  // Check the ad-hoc requirements of 'DistributedTargetInvocationResultHandler'
  if (Proto->isSpecificProtocol(KnownProtocolKind::DistributedTargetInvocationResultHandler)) {
    // - onReturn
    auto onReturnDecl =
        getOnReturnOnDistributedTargetInvocationResultHandler(decl);
    if (!onReturnDecl) {
      anyMissingAdHocRequirements = diagnoseMissingAdHocProtocolRequirement(C, C.Id_onReturn, decl);
    }
    if (checkAdHocRequirementAccessControl(decl, Proto, onReturnDecl)) {
      anyMissingAdHocRequirements = true;
    }

    return anyMissingAdHocRequirements;
  }

  assert(!anyMissingAdHocRequirements &&
         "Should have returned in appropriate type checking block earlier!");
  return false;
}

static bool checkDistributedTargetResultType(
    ModuleDecl *module, ValueDecl *valueDecl,
    Type serializationRequirement,
    bool diagnose) {
  auto &C = valueDecl->getASTContext();

  if (serializationRequirement && serializationRequirement->hasError()) {
    return false;
  }
  if (!serializationRequirement || serializationRequirement->hasError()) {
    return false; // error of the type would be diagnosed elsewhere
  }

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

  SmallVector<ProtocolDecl *, 4> serializationRequirements;
  // Collect extra "SerializationRequirement: SomeProtocol" requirements
  if (serializationRequirement && !serializationRequirement->hasError()) {
    auto srl = serializationRequirement->getExistentialLayout();
    llvm::copy(srl.getProtocols(),
               std::back_inserter(serializationRequirements));
  }

  auto isCodableRequirement =
      checkDistributedSerializationRequirementIsExactlyCodable(
          C, serializationRequirement);

  for (auto serializationReq: serializationRequirements) {
    auto conformance =
        module->checkConformance(resultType, serializationReq);
    if (conformance.isInvalid()) {
      if (diagnose) {
        llvm::StringRef conformanceToSuggest = isCodableRequirement ?
                                               "Codable" : // Codable is a typealias, easier to diagnose like that
                                               serializationReq->getNameStr();

        auto diag = valueDecl->diagnose(
            diag::distributed_actor_target_result_not_codable,
            resultType,
            valueDecl,
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

  // === AssociatedTypes
  // --- SerializationRequirement MUST be a protocol TODO(distributed): rdar://91663941
  // we may lift this in the future and allow classes but this requires more
  // work to enable associatedtypes to be constrained to class or protocol,
  // which then will unlock using them as generic constraints in protocols.
  Type requirementTy = getDistributedActorSystemSerializationType(nominal);

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
  if (!func->isDistributed())
    return false;

  // ==== Ensure the Distributed module is available,
  if (!swift::ensureDistributedModuleLoaded(func))
    return true;

  auto &C = func->getASTContext();
  return evaluateOrDefault(C.evaluator,
                           CheckDistributedFunctionRequest{func},
                           false); // no error if cycle
}

bool CheckDistributedFunctionRequest::evaluate(
    Evaluator &evaluator, AbstractFunctionDecl *func) const {
  if (auto *accessor = dyn_cast<AccessorDecl>(func)) {
    auto *var = cast<VarDecl>(accessor->getStorage());
    assert(var->isDistributed() && accessor->isDistributedGetter());
  } else {
    assert(func->isDistributed());
  }

  auto &C = func->getASTContext();
  auto module = func->getParentModule();

  /// If no distributed module is available, then no reason to even try checks.
  if (!C.getLoadedModule(C.Id_Distributed)) {
    func->diagnose(diag::distributed_decl_needs_explicit_distributed_import,
                   func);
    return true;
  }

  Type serializationReqType =
      getDistributedActorSerializationType(func->getDeclContext());

  for (auto param: *func->getParameters()) {
    // --- Check the parameter conforming to serialization requirements
    if (serializationReqType && !serializationReqType->hasError()) {
      // If the requirement is exactly `Codable` we diagnose it ia bit nicer.
      auto serializationRequirementIsCodable =
          checkDistributedSerializationRequirementIsExactlyCodable(
              C, serializationReqType);

      // --- Check parameters for 'SerializationRequirement' conformance
      auto paramTy = func->mapTypeIntoContext(param->getInterfaceType());

      auto srl = serializationReqType->getExistentialLayout();
      for (auto req: srl.getProtocols()) {
        if (module->checkConformance(paramTy, req).isInvalid()) {
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
    }

    // --- Check parameters for various illegal modifiers
    if (param->isInOut()) {
      param->diagnose(
          diag::distributed_actor_func_inout,
          param->getName(),
          func
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
          func);
      return true;
    }

    if (param->isVariadic()) {
      param->diagnose(
          diag::distributed_actor_func_variadic,
          param->getName(),
          func
      );
    }
  }

  // --- Result type must be either void or a serialization requirement conforming type
  if (checkDistributedTargetResultType(module, func, serializationReqType,
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
      var->diagnose(diag::distributed_property_can_only_be_computed, var);
    return true;
  }

  // distributed properties cannot have setters
  if (var->getWriteImpl() != swift::WriteImplKind::Immutable) {
    if (diagnose)
      var->diagnose(diag::distributed_property_can_only_be_computed_get_only,
                    var->getName());
    return true;
  }

  auto serializationRequirement =
      getDistributedActorSerializationType(var->getDeclContext());

  auto module = var->getModuleContext();
  if (checkDistributedTargetResultType(module, var, serializationRequirement, diagnose)) {
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
        prop->setInvalid();
      }
    }
  }
}

// ==== ------------------------------------------------------------------------

void TypeChecker::checkDistributedActor(SourceFile *SF, NominalTypeDecl *nominal) {
  if (!nominal || !nominal->isDistributedActor())
    return;

  // ==== Ensure the Distributed module is available,
  // without it there's no reason to check the decl in more detail anyway.
  if (!swift::ensureDistributedModuleLoaded(nominal))
    return;

  // ==== Constructors
  // --- Get the default initializer
  // If applicable, this will create the default 'init(transport:)' initializer
  (void)nominal->getDefaultInitializer();

  for (auto member : nominal->getMembers()) {
    // --- Ensure 'distributed func' all thunks
    if (auto *var = dyn_cast<VarDecl>(member)) {
      if (!var->isDistributed())
        continue;

      if (auto thunk = var->getDistributedThunk())
        SF->addDelayedFunction(thunk);

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
      }

      if (auto thunk = func->getDistributedThunk()) {
        SF->addDelayedFunction(thunk);
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

bool TypeChecker::checkDistributedFunc(FuncDecl *func) {
  return swift::checkDistributedFunction(func);
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
  auto decoderTy = getAssociatedTypeOfDistributedSystemOfActor(
      actor, ctx.Id_InvocationDecoder);
  return decoderTy ? decoderTy->getAnyNominal() : nullptr;
}

FuncDecl *
GetDistributedActorConcreteArgumentDecodingMethodRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *decl) const {
  auto &ctx = decl->getASTContext();

  if (auto actor = dyn_cast<ClassDecl>(decl)) {
    auto *decoder = getDistributedActorInvocationDecoder(actor);
    // If distributed actor is generic over actor system, there is not
    // going to be a concrete decoder.
    if (!decoder)
      return nullptr;

    auto decoderTy = decoder->getDeclaredInterfaceType();

    auto members =
        TypeChecker::lookupMember(actor->getDeclContext(), decoderTy,
                                  DeclNameRef(ctx.Id_decodeNextArgument));

    // typealias SerializationRequirement = any ...
    auto serializationTy = getAssociatedTypeOfDistributedSystemOfActor(
        actor, ctx.Id_SerializationRequirement);

    if (!serializationTy || !serializationTy->is<ExistentialType>())
      return nullptr;

    SmallVector<ProtocolDecl *, 4> serializationRequirements;
    {
      auto layout = serializationTy->getExistentialLayout();
      llvm::copy(layout.getProtocols(),
                 std::back_inserter(serializationRequirements));
    }

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

      auto paramTy =
          genericParamList->getParams()[0]->getDeclaredInterfaceType();

      // `decodeNextArgument` should return its generic parameter value
      if (!FD->getResultInterfaceType()->isEqual(paramTy))
        continue;

      // Let's find out how many serialization requirements does this method cover e.g. `Codable` is two requirements - `Encodable` and `Decodable`.
      auto nextArgumentSig = FD->getGenericSignature();
      bool okay =
          llvm::all_of(serializationRequirements, [&](ProtocolDecl *p) -> bool {
            return nextArgumentSig->requiresProtocol(paramTy, p);
          });

      // If the current method covers all of the serialization requirements,
      // it's a match. Note that it might also have other requirements, but
      // we let that go as long as there are no two candidates that differ
      // only in generic requirements.
      if (okay)
        candidates.push_back(FD);
    }

    // Type-checker should reject any definition of invocation decoder
    // that doesn't have a correct version of `decodeNextArgument` declared.
    assert(candidates.size() == 1);
    return candidates.front();
  }

  /// No concrete candidate found, return null and perform the call via a
  /// witness
  return nullptr;
}

llvm::ArrayRef<ValueDecl *>
GetDistributedMethodWitnessedProtocolRequirements::evaluate(
    Evaluator &evaluator,
    AbstractFunctionDecl *afd) const {
  // Only a 'distributed' decl can witness 'distributed' protocol
  assert(afd->isDistributed());
  auto &C = afd->getASTContext();

  auto result = llvm::SmallVector<ValueDecl *, 1>();
  for (auto witnessedRequirement : afd->getSatisfiedProtocolRequirements()) {
    if (witnessedRequirement->isDistributed()) {
      result.push_back(witnessedRequirement);
    }
  }

  return C.AllocateCopy(result);
}
