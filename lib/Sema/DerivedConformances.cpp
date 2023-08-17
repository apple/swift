//===--- DerivedConformances.cpp - Derived conformance utilities ----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "TypeCheckConcurrency.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Types.h"
#include "swift/ClangImporter/ClangModule.h"
#include "DerivedConformances.h"

using namespace swift;

enum NonconformingMemberKind { AssociatedValue, StoredProperty };

DerivedConformance::DerivedConformance(ASTContext &ctx, Decl *conformanceDecl,
                                       NominalTypeDecl *nominal,
                                       ProtocolDecl *protocol)
    : Context(ctx), ConformanceDecl(conformanceDecl), Nominal(nominal),
      Protocol(protocol) {
  assert(getConformanceContext()->getSelfNominalTypeDecl() == nominal);
}

DeclContext *DerivedConformance::getConformanceContext() const {
  return cast<DeclContext>(ConformanceDecl);
}

ModuleDecl *DerivedConformance::getParentModule() const {
  return cast<DeclContext>(ConformanceDecl)->getParentModule();
}

void DerivedConformance::addMembersToConformanceContext(
    ArrayRef<Decl *> children) {
  auto IDC = cast<IterableDeclContext>(ConformanceDecl);
  for (auto child : children)
    IDC->addMember(child);
}

void DerivedConformance::addMemberToConformanceContext(
    Decl *member, Decl *hint) {
  auto IDC = cast<IterableDeclContext>(ConformanceDecl);
  IDC->addMember(member, hint, /*insertAtHead=*/false);
}

void DerivedConformance::addMemberToConformanceContext(
    Decl *member, bool insertAtHead) {
  auto IDC = cast<IterableDeclContext>(ConformanceDecl);
  IDC->addMember(member, /*hint=*/nullptr, insertAtHead);
}

Type DerivedConformance::getProtocolType() const {
  return Protocol->getDeclaredInterfaceType();
}

bool DerivedConformance::derivesProtocolConformance(DeclContext *DC,
                                                    NominalTypeDecl *Nominal,
                                                    ProtocolDecl *Protocol) {
  const auto derivableKind = Protocol->getKnownDerivableProtocolKind();
  if (!derivableKind)
    return false;

  // When the necessary requirements are met, the conformance to OptionSet
  // is serendipitously derived via memberwise initializer synthesis.
  if (*derivableKind == KnownDerivableProtocolKind::OptionSet) {
    return false;
  }

  if (*derivableKind == KnownDerivableProtocolKind::Hashable) {
    // We can always complete a partial Hashable implementation, and we can
    // synthesize a full Hashable implementation for structs and enums with
    // Hashable components.
    return canDeriveHashable(Nominal);
  }

  if (*derivableKind == KnownDerivableProtocolKind::Actor)
    return canDeriveActor(DC, Nominal);

  if (*derivableKind == KnownDerivableProtocolKind::Identifiable)
    return canDeriveIdentifiable(Nominal, DC);
  if (*derivableKind == KnownDerivableProtocolKind::DistributedActor)
    return canDeriveDistributedActor(Nominal, DC);
  if (*derivableKind == KnownDerivableProtocolKind::DistributedActorSystem)
    return canDeriveDistributedActorSystem(Nominal, DC);

  if (*derivableKind == KnownDerivableProtocolKind::AdditiveArithmetic)
    return canDeriveAdditiveArithmetic(Nominal, DC);

  // Eagerly return true here. Actual synthesis conditions are checked in
  // `DerivedConformance::deriveDifferentiable`: they are complicated and depend
  // on the requirement being derived.
  if (*derivableKind == KnownDerivableProtocolKind::Differentiable)
    return true;

  if (*derivableKind == KnownDerivableProtocolKind::Encodable) {
    return canDeriveEncodable(Nominal);
  }

  if (*derivableKind == KnownDerivableProtocolKind::Decodable) {
    return canDeriveDecodable(Nominal);
  }

  if (auto *enumDecl = dyn_cast<EnumDecl>(Nominal)) {
    switch (*derivableKind) {
        // The presence of a raw type is an explicit declaration that
        // the compiler should derive a RawRepresentable conformance.
      case KnownDerivableProtocolKind::RawRepresentable:
        return canDeriveRawRepresentable(DC, Nominal);

        // Enums without associated values can implicitly derive Equatable
        // conformance.
      case KnownDerivableProtocolKind::Equatable:
        return canDeriveEquatable(DC, Nominal);
      
      case KnownDerivableProtocolKind::Comparable:
        return !enumDecl->hasPotentiallyUnavailableCaseValue()
            && canDeriveComparable(DC, enumDecl); 

        // "Simple" enums without availability attributes can explicitly derive
        // a CaseIterable conformance.
        //
        // FIXME: Lift the availability restriction.
      case KnownDerivableProtocolKind::CaseIterable:
        return !enumDecl->hasPotentiallyUnavailableCaseValue()
            && enumDecl->hasOnlyCasesWithoutAssociatedValues();

        // @objc enums can explicitly derive their _BridgedNSError conformance.
      case KnownDerivableProtocolKind::BridgedNSError:
        return enumDecl->isObjC() && enumDecl->hasCases()
            && enumDecl->hasOnlyCasesWithoutAssociatedValues();

        // Enums without associated values and enums with a raw type of String
        // or Int can explicitly derive CodingKey conformance.
      case KnownDerivableProtocolKind::CodingKey: {
        Type rawType = enumDecl->getRawType();
        if (rawType) {
          return rawType->isString() || rawType->isInt();
        }

        // hasOnlyCasesWithoutAssociatedValues will return true for empty enums;
        // empty enums are allowed to conform as well.
        return enumDecl->hasOnlyCasesWithoutAssociatedValues();
      }

      default:
        return false;
    }
  } else if (isa<StructDecl>(Nominal)) {
    switch (*derivableKind) {
    case KnownDerivableProtocolKind::Equatable:
      // Structs can explicitly derive Equatable conformance.
      return canDeriveEquatable(DC, Nominal);
    default:
      return false;
    }
  }
  return false;
}

SmallVector<VarDecl *, 3>
DerivedConformance::storedPropertiesNotConformingToProtocol(
    DeclContext *DC, StructDecl *theStruct, ProtocolDecl *protocol) {
  auto storedProperties = theStruct->getStoredProperties();
  SmallVector<VarDecl *, 3> nonconformingProperties;
  for (auto propertyDecl : storedProperties) {
    if (!propertyDecl->isUserAccessible())
      continue;

    auto type = propertyDecl->getValueInterfaceType();
    if (!type)
      nonconformingProperties.push_back(propertyDecl);

    if (!TypeChecker::conformsToProtocol(DC->mapTypeIntoContext(type), protocol,
                                         DC->getParentModule())) {
      nonconformingProperties.push_back(propertyDecl);
    }
  }
  return nonconformingProperties;
}

void DerivedConformance::tryDiagnoseFailedDerivation(DeclContext *DC,
                                                     NominalTypeDecl *nominal,
                                                     ProtocolDecl *protocol) {
  auto knownProtocol = protocol->getKnownProtocolKind();
  if (!knownProtocol)
    return;
  
  if (*knownProtocol == KnownProtocolKind::Equatable) {
    tryDiagnoseFailedEquatableDerivation(DC, nominal);
  }

  if (*knownProtocol == KnownProtocolKind::Hashable) {
    tryDiagnoseFailedHashableDerivation(DC, nominal);
  }

  if (*knownProtocol == KnownProtocolKind::Comparable) {
    tryDiagnoseFailedComparableDerivation(DC, nominal);
  }

  if (*knownProtocol == KnownProtocolKind::DistributedActor) {
    tryDiagnoseFailedDistributedActorDerivation(DC, nominal);
  }

  if (*knownProtocol == KnownProtocolKind::DistributedActorSystem) {
    tryDiagnoseFailedDistributedActorSystemDerivation(DC, nominal);
  }
}

void DerivedConformance::diagnoseAnyNonConformingMemberTypes(
    DeclContext *DC, NominalTypeDecl *nominal, ProtocolDecl *protocol) {
  ASTContext &ctx = DC->getASTContext();

  if (auto *enumDecl = dyn_cast<EnumDecl>(nominal)) {
    auto nonconformingAssociatedTypes =
        associatedValuesNotConformingToProtocol(DC, enumDecl, protocol);
    for (auto *typeToDiagnose : nonconformingAssociatedTypes) {
      SourceLoc reprLoc;
      if (auto *repr = typeToDiagnose->getTypeRepr())
        reprLoc = repr->getStartLoc();
      ctx.Diags.diagnose(
          reprLoc, diag::missing_member_type_conformance_prevents_synthesis,
          NonconformingMemberKind::AssociatedValue,
          typeToDiagnose->getInterfaceType(),
          protocol->getDeclaredInterfaceType(),
          nominal->getDeclaredInterfaceType());
    }
  }

  if (auto *structDecl = dyn_cast<StructDecl>(nominal)) {
    auto nonconformingStoredProperties =
        storedPropertiesNotConformingToProtocol(DC, structDecl, protocol);
    for (auto *propertyToDiagnose : nonconformingStoredProperties) {
      ctx.Diags.diagnose(
          propertyToDiagnose->getLoc(),
          diag::missing_member_type_conformance_prevents_synthesis,
          NonconformingMemberKind::StoredProperty,
          propertyToDiagnose->getInterfaceType(),
          protocol->getDeclaredInterfaceType(),
          nominal->getDeclaredInterfaceType());
    }
  }
}

void DerivedConformance::diagnoseIfSynthesisUnsupportedForDecl(
    NominalTypeDecl *nominal, ProtocolDecl *protocol) {
  auto shouldDiagnose = false;

  if (protocol->isSpecificProtocol(KnownProtocolKind::Equatable) ||
      protocol->isSpecificProtocol(KnownProtocolKind::Hashable)) {
    shouldDiagnose = isa<ClassDecl>(nominal);
  }

  if (protocol->isSpecificProtocol(KnownProtocolKind::Comparable)) {
    shouldDiagnose = !isa<EnumDecl>(nominal);
  }

  if (shouldDiagnose) {
    auto &ctx = nominal->getASTContext();
    ctx.Diags.diagnose(
        nominal->getLoc(), diag::automatic_protocol_synthesis_unsupported,
        protocol->getName().str(), nominal->getDescriptiveKind());
  }
}

ValueDecl *DerivedConformance::getDerivableRequirement(NominalTypeDecl *nominal,
                                                       ValueDecl *requirement) {
  // Note: whenever you update this function, also update
  // TypeChecker::deriveProtocolRequirement.
  ASTContext &ctx = nominal->getASTContext();
  const auto name = requirement->getName();

  // Local function that retrieves the requirement with the same name as
  // the provided requirement, but within the given known protocol.
  auto getRequirement = [&](KnownProtocolKind kind) -> ValueDecl * {
    // Dig out the protocol.
    auto proto = ctx.getProtocol(kind);
    if (!proto) return nullptr;

    auto conformance = nominal->getParentModule()->lookupConformance(
        nominal->getDeclaredInterfaceType(), proto);
    if (conformance) {
      auto DC = conformance.getConcrete()->getDeclContext();
      // Check whether this nominal type derives conformances to the protocol.
      if (!DerivedConformance::derivesProtocolConformance(DC, nominal, proto))
        return nullptr;
    }

    // Retrieve the requirement.
    return proto->getSingleRequirement(name);
  };

  // Properties.
  if (isa<VarDecl>(requirement)) {
    // RawRepresentable.rawValue
    if (name.isSimpleName(ctx.Id_rawValue))
      return getRequirement(KnownProtocolKind::RawRepresentable);

    // Hashable.hashValue
    if (name.isSimpleName(ctx.Id_hashValue))
      return getRequirement(KnownProtocolKind::Hashable);

    // CaseIterable.allValues
    if (name.isSimpleName(ctx.Id_allCases))
      return getRequirement(KnownProtocolKind::CaseIterable);

    // _BridgedNSError._nsErrorDomain
    if (name.isSimpleName(ctx.Id_nsErrorDomain))
      return getRequirement(KnownProtocolKind::BridgedNSError);

    // CodingKey.stringValue
    if (name.isSimpleName(ctx.Id_stringValue))
      return getRequirement(KnownProtocolKind::CodingKey);

    // CodingKey.intValue
    if (name.isSimpleName(ctx.Id_intValue))
      return getRequirement(KnownProtocolKind::CodingKey);

    // AdditiveArithmetic.zero
    if (name.isSimpleName(ctx.Id_zero))
      return getRequirement(KnownProtocolKind::AdditiveArithmetic);

    // Actor.unownedExecutor
    if (name.isSimpleName(ctx.Id_unownedExecutor)) {
      if (nominal->isDistributedActor()) {
        return getRequirement(KnownProtocolKind::DistributedActor);
      } else {
        return getRequirement(KnownProtocolKind::Actor);
      }
    }

    // DistributedActor.id
    if (name.isSimpleName(ctx.Id_id))
      return getRequirement(KnownProtocolKind::DistributedActor);

    // DistributedActor.actorSystem
    if (name.isSimpleName(ctx.Id_actorSystem))
      return getRequirement(KnownProtocolKind::DistributedActor);

    return nullptr;
  }

  // Functions.
  if (auto func = dyn_cast<FuncDecl>(requirement)) {
    if (func->isOperator() && name.getBaseName() == "<")
      return getRequirement(KnownProtocolKind::Comparable);
    
    if (func->isOperator() && name.getBaseName() == "==")
      return getRequirement(KnownProtocolKind::Equatable);

    // AdditiveArithmetic.+
    // AdditiveArithmetic.-
    if (func->isOperator() && name.getArgumentNames().size() == 2 &&
        (name.getBaseName() == "+" || name.getBaseName() == "-")) {
      return getRequirement(KnownProtocolKind::AdditiveArithmetic);
    }

    // Differentiable.move(by:)
    if (name.isCompoundName() && name.getBaseName() == ctx.Id_move) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 1 && argumentNames[0] == ctx.Id_by)
        return getRequirement(KnownProtocolKind::Differentiable);
    }

    // Encodable.encode(to: Encoder)
    if (name.isCompoundName() && name.getBaseName() == ctx.Id_encode) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 1 && argumentNames[0] == ctx.Id_to)
        return getRequirement(KnownProtocolKind::Encodable);
    }

    // Hashable.hash(into: inout Hasher)
    if (name.isCompoundName() && name.getBaseName() == ctx.Id_hash) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 1 && argumentNames[0] == ctx.Id_into)
        return getRequirement(KnownProtocolKind::Hashable);
    }

    // static DistributedActor.resolve(id:using:)
    if (name.isCompoundName() && name.getBaseName() == ctx.Id_resolve &&
        func->isStatic()) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 2 &&
          argumentNames[0] == ctx.Id_id &&
          argumentNames[1] == ctx.Id_using) {
        return getRequirement(KnownProtocolKind::DistributedActor);
      }
    }

    // DistributedActor.actorSystem
    if (name.isCompoundName() &&
        name.getBaseName() == ctx.Id_invokeHandlerOnReturn)
      return getRequirement(KnownProtocolKind::DistributedActorSystem);

    return nullptr;
  }

  // Initializers.
  if (auto ctor = dyn_cast<ConstructorDecl>(requirement)) {
    auto argumentNames = name.getArgumentNames();
    if (argumentNames.size() == 1) {
      if (argumentNames[0] == ctx.Id_rawValue)
        return getRequirement(KnownProtocolKind::RawRepresentable);

      // CodingKey.init?(stringValue:), CodingKey.init?(intValue:)
      if (ctor->isFailable() &&
          !ctor->isImplicitlyUnwrappedOptional() &&
          (argumentNames[0] == ctx.Id_stringValue ||
           argumentNames[0] == ctx.Id_intValue))
        return getRequirement(KnownProtocolKind::CodingKey);

      // Decodable.init(from: Decoder)
      if (argumentNames[0] == ctx.Id_from)
        return getRequirement(KnownProtocolKind::Decodable);
    }

    return nullptr;
  }

  // Associated types.
  if (isa<AssociatedTypeDecl>(requirement)) {
    // RawRepresentable.RawValue
    if (name.isSimpleName(ctx.Id_RawValue))
      return getRequirement(KnownProtocolKind::RawRepresentable);

    // CaseIterable.AllCases
    if (name.isSimpleName(ctx.Id_AllCases))
      return getRequirement(KnownProtocolKind::CaseIterable);

    // Differentiable.TangentVector
    if (name.isSimpleName(ctx.Id_TangentVector))
      return getRequirement(KnownProtocolKind::Differentiable);

    return nullptr;
  }

  return nullptr;
}

DeclRefExpr *
DerivedConformance::createSelfDeclRef(AbstractFunctionDecl *fn) {
  ASTContext &C = fn->getASTContext();

  auto selfDecl = fn->getImplicitSelfDecl();
  return new (C) DeclRefExpr(selfDecl, DeclNameLoc(), /*implicit*/true);
}

CallExpr *
DerivedConformance::createBuiltinCall(ASTContext &ctx,
                                      BuiltinValueKind builtin,
                                      ArrayRef<Type> typeArgs,
                                      ArrayRef<ProtocolConformanceRef>
                                        conformances,
                                      ArrayRef<Expr *> args) {
  auto name = ctx.getIdentifier(getBuiltinName(builtin));
  auto decl = getBuiltinValueDecl(ctx, name);
  assert(decl);

  ConcreteDeclRef declRef = decl;
  auto fnType = decl->getInterfaceType();
  if (auto genericFnType = fnType->getAs<GenericFunctionType>()) {
    auto generics = genericFnType->getGenericSignature();
    auto subs = SubstitutionMap::get(generics, typeArgs, conformances);
    declRef = ConcreteDeclRef(decl, subs);
    fnType = genericFnType->substGenericArgs(subs);
  } else {
    assert(typeArgs.empty());
  }

  auto resultType = fnType->castTo<FunctionType>()->getResult();

  Expr *ref = new (ctx) DeclRefExpr(declRef, DeclNameLoc(),
                                    /*Implicit=*/true,
                                    AccessSemantics::Ordinary, fnType);
  auto *argList = ArgumentList::forImplicitUnlabeled(ctx, args);
  auto *call = CallExpr::createImplicit(ctx, ref, argList);
  call->setType(resultType);
  call->setThrows(false);

  return call;
}

CallExpr *DerivedConformance::createDiagnoseUnavailableCodeReachedCallExpr(
    ASTContext &ctx) {
  FuncDecl *diagnoseDecl = ctx.getDiagnoseUnavailableCodeReached();
  auto diagnoseDeclRefExpr =
      new (ctx) DeclRefExpr(diagnoseDecl, DeclNameLoc(), true);
  diagnoseDeclRefExpr->setType(diagnoseDecl->getInterfaceType());
  auto argList = ArgumentList::createImplicit(ctx, {});
  auto callExpr = CallExpr::createImplicit(ctx, diagnoseDeclRefExpr, argList);
  callExpr->setType(ctx.getNeverType());
  callExpr->setThrows(false);
  return callExpr;
}

AccessorDecl *DerivedConformance::
addGetterToReadOnlyDerivedProperty(VarDecl *property,
                                   Type propertyContextType) {
  auto getter =
    declareDerivedPropertyGetter(property, propertyContextType);

  property->setImplInfo(StorageImplInfo::getImmutableComputed());
  property->setAccessors(SourceLoc(), {getter}, SourceLoc());

  return getter;
}

AccessorDecl *
DerivedConformance::declareDerivedPropertyGetter(VarDecl *property,
                                                 Type propertyContextType) {
  auto &C = property->getASTContext();
  auto parentDC = property->getDeclContext();
  ParameterList *params = ParameterList::createEmpty(C);

  auto getterDecl = AccessorDecl::create(
      C,
      /*FuncLoc=*/SourceLoc(), /*AccessorKeywordLoc=*/SourceLoc(),
      AccessorKind::Get, property,
      /*StaticLoc=*/SourceLoc(), StaticSpellingKind::None,
      /*Async=*/false, /*AsyncLoc=*/SourceLoc(),
      /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(), params,
      property->getInterfaceType(), parentDC);
  getterDecl->setImplicit();
  getterDecl->setIsTransparent(false);
  getterDecl->copyFormalAccessFrom(property);

  return getterDecl;
}

static VarDecl::Introducer
mapIntroducer(DerivedConformance::SynthesizedIntroducer intro) {
  switch (intro) {
  case DerivedConformance::SynthesizedIntroducer::Let:
    return VarDecl::Introducer::Let;
  case DerivedConformance::SynthesizedIntroducer::Var:
    return VarDecl::Introducer::Var;
  }
  llvm_unreachable("Invalid synthesized introducer!");
}

std::pair<VarDecl *, PatternBindingDecl *>
DerivedConformance::declareDerivedProperty(SynthesizedIntroducer intro,
                                           Identifier name,
                                           Type propertyInterfaceType,
                                           Type propertyContextType,
                                           bool isStatic, bool isFinal) {
  auto parentDC = getConformanceContext();

  VarDecl *propDecl = new (Context) VarDecl(
      /*IsStatic*/ isStatic, mapIntroducer(intro), SourceLoc(), name, parentDC);
  propDecl->setImplicit();
  propDecl->setSynthesized();
  propDecl->copyFormalAccessFrom(Nominal, /*sourceIsParentContext*/ true);
  propDecl->setInterfaceType(propertyInterfaceType);

  Pattern *propPat =
      NamedPattern::createImplicit(Context, propDecl, propertyContextType);

  propPat = TypedPattern::createImplicit(Context, propPat, propertyContextType);
  propPat->setType(propertyContextType);

  auto *pbDecl = PatternBindingDecl::createImplicit(
      Context, StaticSpellingKind::None, propPat, /*InitExpr*/ nullptr,
      parentDC);
  return {propDecl, pbDecl};
}

bool DerivedConformance::checkAndDiagnoseDisallowedContext(
    ValueDecl *synthesizing) const {
  // In general, conformances can't be synthesized in extensions across files;
  // but we have to allow it as a special case for Equatable and Hashable on
  // enums with no associated values to preserve source compatibility.
  bool allowCrossfileExtensions = false;
  if (Protocol->isSpecificProtocol(KnownProtocolKind::Equatable) ||
      Protocol->isSpecificProtocol(KnownProtocolKind::Hashable)) {
    auto ED = dyn_cast<EnumDecl>(Nominal);
    allowCrossfileExtensions = ED && ED->hasOnlyCasesWithoutAssociatedValues();
  }

  if (!allowCrossfileExtensions &&
      Nominal->getModuleScopeContext() !=
          getConformanceContext()->getModuleScopeContext()) {
    ConformanceDecl->diagnose(diag::cannot_synthesize_in_crossfile_extension,
                              Nominal, synthesizing->getName(),
                              getProtocolType());
    Nominal->diagnose(diag::kind_declared_here, DescriptiveDeclKind::Type);

    // In editor mode, try to insert a stub.
    if (Context.LangOpts.DiagnosticsEditorMode) {
      auto Extension = cast<ExtensionDecl>(getConformanceContext());
      auto FixitLocation = Extension->getBraces().Start;
      llvm::SmallString<128> Text;
      {
        llvm::raw_svector_ostream SS(Text);
        swift::printRequirementStub(synthesizing, Nominal,
                                    Nominal->getDeclaredType(),
                                    Extension->getStartLoc(), SS);
        if (!Text.empty()) {
          ConformanceDecl->diagnose(diag::missing_witnesses_general)
            .fixItInsertAfter(FixitLocation, Text.str());
        }
      }
    }
    return true;
  }

  // A non-final class can't have a protocol-witnesses initializer in an
  // extension.
  if (auto CD = dyn_cast<ClassDecl>(Nominal)) {
    if (!CD->isSemanticallyFinal() && isa<ConstructorDecl>(synthesizing) &&
        isa<ExtensionDecl>(ConformanceDecl)) {
      ConformanceDecl->diagnose(
          diag::cannot_synthesize_init_in_extension_of_nonfinal,
          getProtocolType(), synthesizing->getName());
      return true;
    }
  }

  if (auto ED = dyn_cast<EnumDecl>(Nominal)) {
    if (ED->getAllCases().empty() &&
        (Protocol->isSpecificProtocol(KnownProtocolKind::Encodable) ||
         Protocol->isSpecificProtocol(KnownProtocolKind::Decodable))) {
      ED->diagnose(diag::codable_synthesis_empty_enum_not_supported,
                   getProtocolType(), Nominal->getBaseIdentifier());
      return false;
    }
  }

  return false;
}

/// Returns a generated guard statement that checks whether the given lhs and
/// rhs expressions are equal. If not equal, the else block for the guard
/// returns `guardReturnValue`.
/// \p C The AST context.
/// \p lhsExpr The first expression to compare for equality.
/// \p rhsExpr The second expression to compare for equality.
/// \p guardReturnValue The expression to return if the two sides are not equal 
GuardStmt *DerivedConformance::returnIfNotEqualGuard(ASTContext &C,
                                        Expr *lhsExpr,
                                        Expr *rhsExpr, 
                                        Expr *guardReturnValue) {
  SmallVector<StmtConditionElement, 1> conditions;
  SmallVector<ASTNode, 1> statements;
  
  auto returnStmt = new (C) ReturnStmt(SourceLoc(), guardReturnValue);
  statements.push_back(returnStmt);

  // Next, generate the condition being checked.
  // lhs == rhs
  auto cmpFuncExpr = new (C) UnresolvedDeclRefExpr(
    DeclNameRef(C.Id_EqualsOperator), DeclRefKind::BinaryOperator,
    DeclNameLoc());
  auto *cmpExpr = BinaryExpr::create(C, lhsExpr, cmpFuncExpr, rhsExpr,
                                     /*implicit*/ true);
  conditions.emplace_back(cmpExpr);

  // Build and return the complete guard statement.
  // guard lhs == rhs else { return lhs < rhs }
  auto body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc());
  return new (C) GuardStmt(SourceLoc(), C.AllocateCopy(conditions), body);
}
/// Returns a generated guard statement that checks whether the given lhs and
/// rhs expressions are equal. If not equal, the else block for the guard
/// returns `false`.
/// \p C The AST context.
/// \p lhsExpr The first expression to compare for equality.
/// \p rhsExpr The second expression to compare for equality. 
GuardStmt *DerivedConformance::returnFalseIfNotEqualGuard(ASTContext &C,
                                        Expr *lhsExpr,
                                        Expr *rhsExpr) {
  // return false
  auto falseExpr = new (C) BooleanLiteralExpr(false, SourceLoc(), true);
  return returnIfNotEqualGuard(C, lhsExpr, rhsExpr, falseExpr);
}
/// Returns a generated guard statement that checks whether the given expr is true.
/// If it is false, the else block for the guard returns `nil`.
/// \p C The AST context.
/// \p testExpr The expression that should be tested.
/// \p baseType The wrapped type of the to-be-returned Optional<Wrapped>.
GuardStmt *DerivedConformance::returnNilIfFalseGuardTypeChecked(ASTContext &C,
                                        Expr *testExpr,
                                        Type optionalWrappedType) {
  auto nilExpr = new (C) NilLiteralExpr(SourceLoc(), /*implicit=*/true);
  nilExpr->setType(optionalWrappedType->wrapInOptionalType());

  SmallVector<StmtConditionElement, 1> conditions;
  SmallVector<ASTNode, 1> statements;

  auto returnStmt = new (C) ReturnStmt(SourceLoc(), nilExpr);
  statements.push_back(returnStmt);

  // Next, generate the condition being checked.
  conditions.emplace_back(testExpr);

  // Build and return the complete guard statement.
  // guard lhs == rhs else { return lhs < rhs }
  auto body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc());
  return new (C) GuardStmt(SourceLoc(), C.AllocateCopy(conditions), body);
}
/// Returns a generated guard statement that checks whether the given lhs and
/// rhs expressions are equal. If not equal, the else block for the guard
/// returns lhs < rhs.
/// \p C The AST context.
/// \p lhsExpr The first expression to compare for equality.
/// \p rhsExpr The second expression to compare for equality. 
GuardStmt *DerivedConformance::returnComparisonIfNotEqualGuard(ASTContext &C,
                                        Expr *lhsExpr,
                                        Expr *rhsExpr) {
  // return lhs < rhs
  auto ltFuncExpr = new (C) UnresolvedDeclRefExpr(
    DeclNameRef(C.Id_LessThanOperator), DeclRefKind::BinaryOperator,
    DeclNameLoc());
  auto *ltExpr = BinaryExpr::create(C, lhsExpr, ltFuncExpr, rhsExpr,
                                    /*implicit*/ true);
  return returnIfNotEqualGuard(C, lhsExpr, rhsExpr, ltExpr);
}

/// Build a type-checked integer literal.
static IntegerLiteralExpr *buildIntegerLiteral(ASTContext &C, unsigned index) {
  Type intType = C.getIntType();

  auto literal = IntegerLiteralExpr::createFromUnsigned(C, index, SourceLoc());
  literal->setType(intType);
  literal->setBuiltinInitializer(C.getIntBuiltinInitDecl(C.getIntDecl()));

  return literal;
}

/// Create AST statements which convert from an enum to an Int with a switch.
/// \p stmts The generated statements are appended to this vector.
/// \p parentDC Either an extension or the enum itself.
/// \p enumDecl The enum declaration.
/// \p enumVarDecl The enum input variable.
/// \p funcDecl The parent function.
/// \p indexName The name of the output variable.
/// \return A DeclRefExpr of the output variable (of type Int).
DeclRefExpr *DerivedConformance::convertEnumToIndex(SmallVectorImpl<ASTNode> &stmts,
                                       DeclContext *parentDC,
                                       EnumDecl *enumDecl,
                                       VarDecl *enumVarDecl,
                                       AbstractFunctionDecl *funcDecl,
                                       const char *indexName) {
  ASTContext &C = enumDecl->getASTContext();
  Type enumType = enumVarDecl->getTypeInContext();
  Type intType = C.getIntType();

  auto indexVar = new (C) VarDecl(/*IsStatic*/false, VarDecl::Introducer::Var,
                                  SourceLoc(), C.getIdentifier(indexName),
                                  funcDecl);
  indexVar->setInterfaceType(intType);
  indexVar->setImplicit();

  // generate: var indexVar
  Pattern *indexPat = NamedPattern::createImplicit(C, indexVar, intType);
  indexPat = TypedPattern::createImplicit(C, indexPat, intType);
  indexPat->setType(intType);
  auto *indexBind = PatternBindingDecl::createImplicit(
      C, StaticSpellingKind::None, indexPat, /*InitExpr*/ nullptr, funcDecl);

  unsigned index = 0;
  SmallVector<ASTNode, 4> cases;
  for (auto elt : enumDecl->getAllElements()) {
    if (auto *unavailableElementCase =
            DerivedConformance::unavailableEnumElementCaseStmt(enumType, elt,
                                                               funcDecl)) {
      cases.push_back(unavailableElementCase);
      continue;
    }

    // generate: case .<Case>:
    auto pat = new (C)
        EnumElementPattern(TypeExpr::createImplicit(enumType, C), SourceLoc(),
                           DeclNameLoc(), DeclNameRef(), elt, nullptr,
                           /*DC*/ funcDecl);
    pat->setImplicit();
    pat->setType(enumType);

    auto labelItem = CaseLabelItem(pat);

    // generate: indexVar = <index>
    auto indexExpr = buildIntegerLiteral(C, index++);

    auto indexRef = new (C) DeclRefExpr(indexVar, DeclNameLoc(),
                                        /*implicit*/true,
                                        AccessSemantics::Ordinary,
                                        LValueType::get(intType));
    auto assignExpr = new (C) AssignExpr(indexRef, SourceLoc(),
                                         indexExpr, /*implicit*/ true);
    assignExpr->setType(TupleType::getEmpty(C));
    auto body = BraceStmt::create(C, SourceLoc(), ASTNode(assignExpr),
                                  SourceLoc());
    cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                     labelItem, SourceLoc(), SourceLoc(), body,
                                     /*case body vardecls*/ llvm::None));
  }

  // generate: switch enumVar { }
  auto enumRef = new (C) DeclRefExpr(enumVarDecl, DeclNameLoc(),
                                     /*implicit*/true,
                                     AccessSemantics::Ordinary,
                                     enumVarDecl->getTypeInContext());
  auto switchStmt =
      SwitchStmt::createImplicit(LabeledStmtInfo(), enumRef, cases, C);

  stmts.push_back(indexBind);
  stmts.push_back(switchStmt);

  return new (C) DeclRefExpr(indexVar, DeclNameLoc(), /*implicit*/ true,
                             AccessSemantics::Ordinary, intType);
}

/// Returns the ParamDecl for each associated value of the given enum whose type
/// does not conform to a protocol
/// \p theEnum The enum whose elements and associated values should be checked.
/// \p protocol The protocol being requested.
/// \return The ParamDecl of each associated value whose type does not conform.
SmallVector<ParamDecl *, 4>
DerivedConformance::associatedValuesNotConformingToProtocol(
    DeclContext *DC, EnumDecl *theEnum, ProtocolDecl *protocol) {
  SmallVector<ParamDecl *, 4> nonconformingAssociatedValues;
  for (auto elt : theEnum->getAllElements()) {
    auto PL = elt->getParameterList();
    if (!PL)
      continue;

    for (auto param : *PL) {
      auto type = param->getInterfaceType();
      if (TypeChecker::conformsToProtocol(DC->mapTypeIntoContext(type),
                                          protocol, DC->getParentModule())
              .isInvalid()) {
        nonconformingAssociatedValues.push_back(param);
      }
    }
  }
  return nonconformingAssociatedValues;
}

/// Returns true if, for every element of the given enum, it either has no
/// associated values or all of them conform to a protocol.
/// \p theEnum The enum whose elements and associated values should be checked.
/// \p protocol The protocol being requested.
/// \return True if all associated values of all elements of the enum conform.
bool DerivedConformance::allAssociatedValuesConformToProtocol(DeclContext *DC,
                                                 EnumDecl *theEnum,
                                                 ProtocolDecl *protocol) {
  return associatedValuesNotConformingToProtocol(DC, theEnum, protocol).empty();
}

/// Returns the pattern used to match and bind the associated values (if any) of
/// an enum case.
/// \p enumElementDecl The enum element to match.
/// \p varPrefix The prefix character for variable names (e.g., a0, a1, ...).
/// \p varContext The context into which payload variables should be declared.
/// \p boundVars The array to which the pattern's variables will be appended.
/// \p useLabels If the argument has a label, use it instead of the generated
/// name.
Pattern *DerivedConformance::enumElementPayloadSubpattern(
    EnumElementDecl *enumElementDecl, char varPrefix, DeclContext *varContext,
    SmallVectorImpl<VarDecl *> &boundVars, bool useLabels) {
  auto parentDC = enumElementDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  // No arguments, so no subpattern to match.
  if (!enumElementDecl->hasAssociatedValues())
    return nullptr;

  auto argumentType = enumElementDecl->getArgumentInterfaceType();
  if (auto tupleType = argumentType->getAs<TupleType>()) {
    // Either multiple (labeled or unlabeled) arguments, or one labeled
    // argument. Return a tuple pattern that matches the enum element in arity,
    // types, and labels. For example:
    // case a(x: Int) => (x: let a0)
    // case b(Int, String) => (let a0, let a1)
    SmallVector<TuplePatternElt, 4> elementPatterns;
    int index = 0;
    for (auto tupleElement : tupleType->getElements()) {
      VarDecl *payloadVar;
      if (useLabels && tupleElement.hasName()) {
        payloadVar =
            new (C) VarDecl(/*IsStatic*/ false, VarDecl::Introducer::Let,
                            SourceLoc(), tupleElement.getName(), varContext);
        payloadVar->setInterfaceType(tupleElement.getType());
      } else {
        payloadVar = indexedVarDecl(varPrefix, index++, tupleElement.getType(),
                                    varContext);
      }
      boundVars.push_back(payloadVar);

      auto namedPattern = new (C) NamedPattern(payloadVar);
      namedPattern->setImplicit();
      auto letPattern = BindingPattern::createImplicit(
          C, VarDecl::Introducer::Let, namedPattern);
      elementPatterns.push_back(TuplePatternElt(tupleElement.getName(),
                                                SourceLoc(), letPattern));
    }

    auto pat = TuplePattern::createImplicit(C, elementPatterns);
    pat->setImplicit();
    return pat;
  }

  // Otherwise, a one-argument unlabeled payload. Return a paren pattern whose
  // underlying type is the same as the payload. For example:
  // case a(Int) => (let a0)
  auto underlyingType = argumentType->getWithoutParens();
  auto payloadVar = indexedVarDecl(varPrefix, 0, underlyingType, varContext);
  boundVars.push_back(payloadVar);

  auto namedPattern = new (C) NamedPattern(payloadVar);
  namedPattern->setImplicit();
  auto letPattern = new (C)
      BindingPattern(SourceLoc(), VarDecl::Introducer::Let, namedPattern);
  return ParenPattern::createImplicit(C, letPattern);
}

CaseStmt *DerivedConformance::unavailableEnumElementCaseStmt(
    Type enumType, EnumElementDecl *elt, DeclContext *parentDC,
    unsigned subPatternCount) {
  assert(subPatternCount > 0);

  ASTContext &C = parentDC->getASTContext();
  auto availableAttr = elt->getAttrs().getUnavailable(C);
  if (!availableAttr)
    return nullptr;

  if (!availableAttr->isUnconditionallyUnavailable())
    return nullptr;

  auto createElementPattern = [&]() -> EnumElementPattern * {
    // .<elt>
    EnumElementPattern *eltPattern = new (C) EnumElementPattern(
        TypeExpr::createImplicit(enumType, C), SourceLoc(), DeclNameLoc(),
        DeclNameRef(elt->getBaseIdentifier()), elt, nullptr, /*DC*/ parentDC);
    eltPattern->setImplicit();
    return eltPattern;
  };

  Pattern *labelItemPattern;
  if (subPatternCount > 1) {
    SmallVector<TuplePatternElt, 2> tuplePatternElts;
    for (unsigned i = 0; i < subPatternCount; i++) {
      tuplePatternElts.push_back(TuplePatternElt(createElementPattern()));
    }

    // (.<elt>, ..., .<elt>)
    auto caseTuplePattern = TuplePattern::createImplicit(C, tuplePatternElts);
    caseTuplePattern->setImplicit();
    labelItemPattern = caseTuplePattern;
  } else {
    labelItemPattern = createElementPattern();
  }

  auto labelItem = CaseLabelItem(labelItemPattern);
  auto *callExpr =
      DerivedConformance::createDiagnoseUnavailableCodeReachedCallExpr(C);
  auto body = BraceStmt::create(C, SourceLoc(), {callExpr}, SourceLoc());
  return CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(), labelItem,
                          SourceLoc(), SourceLoc(), body, {},
                          /*implicit*/ true);
}

/// Creates a named variable based on a prefix character and a numeric index.
/// \p prefixChar The prefix character for the variable's name.
/// \p index The numeric index to append to the variable's name.
/// \p type The type of the variable.
/// \p varContext The context of the variable.
/// \return A VarDecl named with the prefix and number.
VarDecl *DerivedConformance::indexedVarDecl(char prefixChar, int index, Type type,
                               DeclContext *varContext) {
  ASTContext &C = varContext->getASTContext();

  llvm::SmallString<8> indexVal;
  indexVal.append(1, prefixChar);
  APInt(32, index).toString(indexVal, 10, /*signed*/ false);
  auto indexStr = C.AllocateCopy(indexVal);
  auto indexStrRef = StringRef(indexStr.data(), indexStr.size());

  auto varDecl = new (C) VarDecl(/*IsStatic*/false, VarDecl::Introducer::Let,
                                 SourceLoc(), C.getIdentifier(indexStrRef),
                                 varContext);
  varDecl->setInterfaceType(type);
  return varDecl;
}

bool swift::memberwiseAccessorsRequireActorIsolation(NominalTypeDecl *nominal) {
  if (!getActorIsolation(nominal).isActorIsolated())
    return false;

  for (auto property : nominal->getStoredProperties()) {
    if (!property->isUserAccessible())
      continue;

    if (!property->isLet())
      return true;
  }

  return false;
}
