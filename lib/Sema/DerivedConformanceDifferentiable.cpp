//===--- DerivedConformanceDifferentiable.cpp - Derived Differentiable ----===//
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
// This file implements explicit derivation of the Differentiable protocol for
// struct and class types.
//
//===----------------------------------------------------------------------===//

#include "CodeSynthesis.h"
#include "TypeChecker.h"
#include "TypeCheckType.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "swift/AST/AutoDiff.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"
#include "DerivedConformances.h"

using namespace swift;

/// Return true if `move(by:)` can be invoked on the given `Differentiable`-
/// conforming property.
///
/// If the given property is a `var`, return true because `move(by:)` can be
/// invoked regardless.  Otherwise, return true if and only if the property's
/// type's 'Differentiable.move(by:)' witness is non-mutating.
static bool canInvokeMoveByOnProperty(
    VarDecl *vd, ProtocolConformanceRef diffableConformance) {
  assert(diffableConformance && "Property must conform to 'Differentiable'");
  // `var` always supports `move(by:)` since it is mutable.
  if (vd->getIntroducer() == VarDecl::Introducer::Var)
    return true;
  // When the property is a `let`, the only case that would be supported is when
  // it has a `move(by:)` protocol requirement witness that is non-mutating.
  auto interfaceType = vd->getInterfaceType();
  auto &C = vd->getASTContext();
  auto witness = diffableConformance.getWitnessByName(
      interfaceType, DeclName(C, C.Id_move, {C.Id_by}));
  if (!witness)
    return false;
  auto *decl = cast<FuncDecl>(witness.getDecl());
  return !decl->isMutating();
}

/// Get the stored properties of a nominal type that are relevant for
/// differentiation, except the ones tagged `@noDerivative`.
static void
getStoredPropertiesForDifferentiation(
    NominalTypeDecl *nominal, DeclContext *DC,
    SmallVectorImpl<VarDecl *> &result,
    bool includeLetPropertiesWithNonmutatingMoveBy = false) {
  auto &C = nominal->getASTContext();
  auto *diffableProto = C.getProtocol(KnownProtocolKind::Differentiable);
  for (auto *vd : nominal->getStoredProperties()) {
    // Peer through property wrappers: use original wrapped properties instead.
    if (auto *originalProperty = vd->getOriginalWrappedProperty()) {
      // Skip immutable wrapped properties. `mutating func move(by:)` cannot
      // be synthesized to update these properties.
      if (!originalProperty->isSettable(DC))
        continue;
      // Use the original wrapped property.
      vd = originalProperty;
    }
    // Skip stored properties with `@noDerivative` attribute.
    if (vd->getAttrs().hasAttribute<NoDerivativeAttr>())
      continue;
    if (vd->getInterfaceType()->hasError())
      continue;
    auto varType = DC->mapTypeIntoContext(vd->getValueInterfaceType());
    auto conformance = TypeChecker::conformsToProtocol(
        varType, diffableProto, DC->getParentModule());
    if (!conformance)
      continue;
    // Skip `let` stored properties with a mutating `move(by:)` if requested.
    // `mutating func move(by:)` cannot be synthesized to update `let`
    // properties.
    if (!includeLetPropertiesWithNonmutatingMoveBy && 
        !canInvokeMoveByOnProperty(vd, conformance))
      continue;
    result.push_back(vd);
  }
}

/// Convert the given `ValueDecl` to a `StructDecl` if it is a `StructDecl` or a
/// `TypeDecl` with an underlying struct type. Otherwise, return `nullptr`.
static StructDecl *convertToStructDecl(ValueDecl *v) {
  if (auto *structDecl = dyn_cast<StructDecl>(v))
    return structDecl;
  auto *typeDecl = dyn_cast<TypeDecl>(v);
  if (!typeDecl)
    return nullptr;
  return dyn_cast_or_null<StructDecl>(
      typeDecl->getDeclaredInterfaceType()->getAnyNominal());
}

/// Get the `Differentiable` protocol `TangentVector` associated type witness
/// for the given interface type and declaration context.
static Type getTangentVectorInterfaceType(Type contextualType,
                                          DeclContext *DC) {
  auto &C = DC->getASTContext();
  auto *diffableProto = C.getProtocol(KnownProtocolKind::Differentiable);
  assert(diffableProto && "`Differentiable` protocol not found");
  auto conf =
      TypeChecker::conformsToProtocol(contextualType, diffableProto,
                                      DC->getParentModule());
  assert(conf && "Contextual type must conform to `Differentiable`");
  if (!conf)
    return nullptr;
  auto tanType = conf.getTypeWitnessByName(contextualType, C.Id_TangentVector);
  return tanType->hasArchetype() ? tanType->mapTypeOutOfContext() : tanType;
}

/// Returns true iff the given nominal type declaration can derive
/// `TangentVector` as `Self` in the given conformance context.
static bool canDeriveTangentVectorAsSelf(NominalTypeDecl *nominal,
                                         DeclContext *DC) {
  // `Self` must not be a class declaration.
  if (nominal->getSelfClassDecl())
    return false;

  auto nominalTypeInContext =
      DC->mapTypeIntoContext(nominal->getDeclaredInterfaceType());
  auto &C = nominal->getASTContext();
  auto *diffableProto = C.getProtocol(KnownProtocolKind::Differentiable);
  auto *addArithProto = C.getProtocol(KnownProtocolKind::AdditiveArithmetic);
  // `Self` must conform to `AdditiveArithmetic`.
  if (!TypeChecker::conformsToProtocol(nominalTypeInContext, addArithProto,
                                       DC->getParentModule()))
    return false;
  for (auto *field : nominal->getStoredProperties()) {
    // `Self` must not have any `@noDerivative` stored properties.
    if (field->getAttrs().hasAttribute<NoDerivativeAttr>())
      return false;
    // `Self` must have all stored properties satisfy `Self == TangentVector`.
    auto fieldType = DC->mapTypeIntoContext(field->getValueInterfaceType());
    auto conf = TypeChecker::conformsToProtocol(fieldType, diffableProto,
                                                DC->getParentModule());
    if (!conf)
      return false;
    auto tangentType = conf.getTypeWitnessByName(fieldType, C.Id_TangentVector);
    if (!fieldType->isEqual(tangentType))
      return false;
  }
  return true;
}

bool DerivedConformance::canDeriveDifferentiable(NominalTypeDecl *nominal,
                                                 DeclContext *DC,
                                                 ValueDecl *requirement) {
  // Experimental differentiable programming must be enabled.
  if (auto *SF = DC->getParentSourceFile())
    if (!isDifferentiableProgrammingEnabled(*SF))
      return false;

  auto &C = nominal->getASTContext();
  // If there are any `TangentVector` type witness candidates, check whether
  // there exists only a single valid candidate.
  bool canUseTangentVectorAsSelf = canDeriveTangentVectorAsSelf(nominal, DC);
  auto isValidTangentVectorCandidate = [&](ValueDecl *v) -> bool {
    // Valid candidate must be a struct or a typealias to a struct.
    auto *structDecl = convertToStructDecl(v);
    if (!structDecl)
      return false;
    // Valid candidate must either:
    // 1. Be implicit (previously synthesized).
    if (structDecl->isImplicit())
      return true;
    // 2. Equal nominal, when the nominal can derive `TangentVector` as `Self`.
    // Nominal type must not customize `TangentVector` to anything other than
    // `Self`. Otherwise, synthesis is semantically unsupported.
    if (structDecl == nominal && canUseTangentVectorAsSelf)
      return true;
    // Otherwise, candidate is invalid.
    return false;
  };
  auto tangentDecls = nominal->lookupDirect(C.Id_TangentVector);
  // There can be at most one valid `TangentVector` type.
  if (tangentDecls.size() > 1)
    return false;
  // There cannot be any invalid `TangentVector` types.
  if (tangentDecls.size() == 1) {
    auto *tangentDecl = tangentDecls.front();
    if (!isValidTangentVectorCandidate(tangentDecl))
      return false;
  }

  // Check `TangentVector` struct derivation conditions.
  // Nominal type must be a struct or class. (No stored properties is okay.)
  if (!isa<StructDecl>(nominal) && !isa<ClassDecl>(nominal))
    return false;
  // If there are no `TangentVector` candidates, derivation is possible if all
  // differentiation stored properties conform to `Differentiable`.
  SmallVector<VarDecl *, 16> diffProperties;
  getStoredPropertiesForDifferentiation(nominal, DC, diffProperties);
  auto *diffableProto = C.getProtocol(KnownProtocolKind::Differentiable);
  return llvm::all_of(diffProperties, [&](VarDecl *v) {
    if (v->getInterfaceType()->hasError())
      return false;
    auto varType = DC->mapTypeIntoContext(v->getValueInterfaceType());
    return (bool)TypeChecker::conformsToProtocol(varType, diffableProto,
                                                 DC->getParentModule());
  });
}

/// Synthesize body for `move(by:)`.
static std::pair<BraceStmt *, bool>
deriveBodyDifferentiable_move(AbstractFunctionDecl *funcDecl, void *) {
  auto &C = funcDecl->getASTContext();
  auto *parentDC = funcDecl->getParent();
  auto *nominal = parentDC->getSelfNominalTypeDecl();

  // Get `Differentiable.move(by:)` protocol requirement.
  auto *diffProto = C.getProtocol(KnownProtocolKind::Differentiable);
  auto *requirement = getProtocolRequirement(diffProto, C.Id_move);

  // Get references to `self` and parameter declarations.
  auto *selfDecl = funcDecl->getImplicitSelfDecl();
  auto *selfDRE =
      new (C) DeclRefExpr(selfDecl, DeclNameLoc(), /*Implicit*/ true);
  auto *paramDecl = funcDecl->getParameters()->get(0);
  auto *paramDRE =
      new (C) DeclRefExpr(paramDecl, DeclNameLoc(), /*Implicit*/ true);

  SmallVector<VarDecl *, 8> diffProperties;
  getStoredPropertiesForDifferentiation(nominal, parentDC, diffProperties);

  // Create call expression applying a member `move(by:)` method to a
  // parameter member: `self.<member>.move(by: offset.<member>)`.
  auto createMemberMethodCallExpr = [&](VarDecl *member) -> Expr * {
    auto *module = nominal->getModuleContext();
    auto memberType =
        parentDC->mapTypeIntoContext(member->getValueInterfaceType());
    auto confRef = module->lookupConformance(memberType, diffProto);
    assert(confRef && "Member does not conform to `Differentiable`");

    // Get member type's requirement witness: `<Member>.move(by:)`.
    ValueDecl *memberWitnessDecl = requirement;
    if (confRef.isConcrete())
      if (auto *witness = confRef.getConcrete()->getWitnessDecl(requirement))
        memberWitnessDecl = witness;
    assert(memberWitnessDecl && "Member witness declaration must exist");

    // Create reference to member method: `self.<member>.move(by:)`.
    Expr *memberExpr =
        new (C) MemberRefExpr(selfDRE, SourceLoc(), member, DeclNameLoc(),
                              /*Implicit*/ true);
    auto *memberMethodExpr =
        new (C) MemberRefExpr(memberExpr, SourceLoc(), memberWitnessDecl,
                              DeclNameLoc(), /*Implicit*/ true);

    // Create reference to parameter member: `offset.<member>`.
    VarDecl *paramMember = nullptr;
    auto *paramNominal = paramDecl->getTypeInContext()->getAnyNominal();
    assert(paramNominal && "Parameter should have a nominal type");
    // Find parameter member corresponding to returned nominal member.
    for (auto *candidate : paramNominal->getStoredProperties()) {
      if (candidate->getName() == member->getName()) {
        paramMember = candidate;
        break;
      }
    }
    assert(paramMember && "Could not find corresponding parameter member");
    auto *paramMemberExpr =
        new (C) MemberRefExpr(paramDRE, SourceLoc(), paramMember, DeclNameLoc(),
                              /*Implicit*/ true);
    // Create expression: `self.<member>.move(by: offset.<member>)`.
    auto *args = ArgumentList::forImplicitSingle(C, C.Id_by, paramMemberExpr);
    return CallExpr::createImplicit(C, memberMethodExpr, args);
  };

  // Collect member `move(by:)` method call expressions.
  SmallVector<ASTNode, 2> memberMethodCallExprs;
  SmallVector<Identifier, 2> memberNames;
  for (auto *member : diffProperties) {
    memberMethodCallExprs.push_back(createMemberMethodCallExpr(member));
    memberNames.push_back(member->getName());
  }
  auto *braceStmt = BraceStmt::create(C, SourceLoc(), memberMethodCallExprs,
                                      SourceLoc(), true);
  return std::pair<BraceStmt *, bool>(braceStmt, false);
}

/// Synthesize function declaration for a `Differentiable` method requirement.
static ValueDecl *deriveDifferentiable_method(
    DerivedConformance &derived, Identifier methodName, Identifier argumentName,
    Identifier parameterName, Type parameterType, Type returnType,
    AbstractFunctionDecl::BodySynthesizer bodySynthesizer) {
  auto *nominal = derived.Nominal;
  auto &C = derived.Context;
  auto *parentDC = derived.getConformanceContext();

  auto *param = new (C) ParamDecl(SourceLoc(), SourceLoc(), argumentName,
                                  SourceLoc(), parameterName, parentDC);
  param->setSpecifier(ParamDecl::Specifier::Default);
  param->setInterfaceType(parameterType);
  param->setImplicit();
  ParameterList *params = ParameterList::create(C, {param});

  DeclName declName(C, methodName, params);
  auto *const funcDecl = FuncDecl::createImplicit(
      C, StaticSpellingKind::None, declName, /*NameLoc=*/SourceLoc(),
      /*Async=*/false,
      /*Throws=*/false,
      /*GenericParams=*/nullptr, params, returnType, parentDC);
  funcDecl->setSynthesized();
  if (!nominal->getSelfClassDecl())
    funcDecl->setSelfAccessKind(SelfAccessKind::Mutating);
  funcDecl->setBodySynthesizer(bodySynthesizer.Fn, bodySynthesizer.Context);

  funcDecl->setGenericSignature(parentDC->getGenericSignatureOfContext());
  funcDecl->copyFormalAccessFrom(nominal, /*sourceIsParentContext*/ true);

  derived.addMembersToConformanceContext({funcDecl});
  return funcDecl;
}

/// Synthesize the `move(by:)` function declaration.
static ValueDecl *deriveDifferentiable_move(DerivedConformance &derived) {
  auto &C = derived.Context;
  auto *parentDC = derived.getConformanceContext();
  auto tangentType =
      getTangentVectorInterfaceType(parentDC->getSelfTypeInContext(), parentDC);
  return deriveDifferentiable_method(
      derived, C.Id_move, C.Id_by, C.Id_offset, tangentType,
      C.TheEmptyTupleType, {deriveBodyDifferentiable_move, nullptr});
}

/// Return associated `TangentVector` struct for a nominal type, if it exists.
/// If not, synthesize the struct.
static StructDecl *
getOrSynthesizeTangentVectorStruct(DerivedConformance &derived, Identifier id) {
  auto *parentDC = derived.getConformanceContext();
  auto *nominal = derived.Nominal;
  auto &C = nominal->getASTContext();

  // If the associated struct already exists, return it.
  auto lookup = nominal->lookupDirect(C.Id_TangentVector);
  assert(lookup.size() < 2 &&
         "Expected at most one associated type named `TangentVector`");
  if (lookup.size() == 1) {
    auto *structDecl = convertToStructDecl(lookup.front());
    assert(structDecl && "Expected lookup result to be a struct");
    return structDecl;
  }

  // Otherwise, synthesize a new struct.

  // Compute `tvDesiredProtos`, the set of protocols that the new `TangentVector` struct must
  // inherit, by collecting all the `TangentVector` conformance requirements imposed by the
  // protocols that `derived.ConformanceDecl` inherits.
  //
  // Note that, for example, this will always find `AdditiveArithmetic` and `Differentiable` because
  // the `Differentiable` protocol itself requires that its `TangentVector` conforms to
  // `AdditiveArithmetic` and `Differentiable`.
  llvm::SmallSetVector<ProtocolDecl *, 4> tvDesiredProtos;

  auto *diffableProto = C.getProtocol(KnownProtocolKind::Differentiable);
  auto *tvAssocType = diffableProto->getAssociatedType(C.Id_TangentVector);

  auto localProtos = cast<IterableDeclContext>(derived.ConformanceDecl)
      ->getLocalProtocols();
  for (auto proto : localProtos) {
    for (auto req : proto->getRequirementSignature().getRequirements()) {
      if (req.getKind() != RequirementKind::Conformance)
        continue;
      auto *firstType = req.getFirstType()->getAs<DependentMemberType>();
      if (!firstType || firstType->getAssocType() != tvAssocType)
        continue;
      tvDesiredProtos.insert(req.getProtocolDecl());
    }
  }
  SmallVector<InheritedEntry, 4> tvDesiredProtoInherited;
  for (auto *p : tvDesiredProtos)
    tvDesiredProtoInherited.push_back(
      InheritedEntry(TypeLoc::withoutLoc(p->getDeclaredInterfaceType())));

  // Cache original members and their associated types for later use.
  SmallVector<VarDecl *, 8> diffProperties;
  getStoredPropertiesForDifferentiation(nominal, parentDC, diffProperties);

  auto synthesizedLoc = derived.ConformanceDecl->getEndLoc();
  auto *structDecl =
      new (C) StructDecl(synthesizedLoc, C.Id_TangentVector, synthesizedLoc,
                         /*Inherited*/ C.AllocateCopy(tvDesiredProtoInherited),
                         /*GenericParams*/ {}, parentDC);
  structDecl->setBraces({synthesizedLoc, synthesizedLoc});
  structDecl->setImplicit();
  structDecl->setSynthesized();
  structDecl->copyFormalAccessFrom(nominal, /*sourceIsParentContext*/ true);

  // Add stored properties to the `TangentVector` struct.
  for (auto *member : diffProperties) {
    // Add a tangent stored property to the `TangentVector` struct, with the
    // name and `TangentVector` type of the original property.
    auto *tangentProperty = new (C) VarDecl(
        member->isStatic(), member->getIntroducer(),
        /*NameLoc*/ SourceLoc(), member->getName(), structDecl);
    // Note: `tangentProperty` is not marked as implicit or synthesized here,
    // because that incorrectly affects memberwise initializer synthesis and
    // causes the type checker to not guarantee the order of these members.
    auto memberContextualType =
        parentDC->mapTypeIntoContext(member->getValueInterfaceType());
    auto memberTanType =
        getTangentVectorInterfaceType(memberContextualType, parentDC);
    tangentProperty->setInterfaceType(memberTanType);
    Pattern *memberPattern =
        NamedPattern::createImplicit(C, tangentProperty, memberTanType);
    memberPattern =
        TypedPattern::createImplicit(C, memberPattern, memberTanType);
    memberPattern->setType(memberTanType);
    auto *memberBinding = PatternBindingDecl::createImplicit(
        C, StaticSpellingKind::None, memberPattern, /*initExpr*/ nullptr,
        structDecl);
    structDecl->addMember(tangentProperty);
    structDecl->addMember(memberBinding);
    tangentProperty->copyFormalAccessFrom(member,
                                          /*sourceIsParentContext*/ true);
    tangentProperty->setSetterAccess(member->getFormalAccess());

    // Cache the tangent property.
    C.evaluator.cacheOutput(TangentStoredPropertyRequest{member, CanType()},
                            TangentPropertyInfo(tangentProperty));

    // Now that the original property has a corresponding tangent property, it
    // should be marked `@differentiable` so that the differentiation transform
    // will synthesize derivative functions for its accessors. We only add this
    // to public stored properties, because their access outside the module will
    // go through accessor declarations.
    if (member->getEffectiveAccess() > AccessLevel::Internal &&
        !member->getAttrs().hasAttribute<DifferentiableAttr>()) {
      auto *getter = member->getSynthesizedAccessor(AccessorKind::Get);
      (void)getter->getInterfaceType();
      // If member or its getter already has a `@differentiable` attribute,
      // continue.
      if (member->getAttrs().hasAttribute<DifferentiableAttr>() ||
          getter->getAttrs().hasAttribute<DifferentiableAttr>())
        continue;
      GenericSignature derivativeGenericSignature =
          getter->getGenericSignature();
      // If the parent declaration context is an extension, the nominal type may
      // conditionally conform to `Differentiable`. Use the extension generic
      // requirements in getter `@differentiable` attributes.
      if (auto *extDecl = dyn_cast<ExtensionDecl>(parentDC->getAsDecl()))
        if (auto extGenSig = extDecl->getGenericSignature())
          derivativeGenericSignature = extGenSig;
      auto *diffableAttr = DifferentiableAttr::create(
          getter, /*implicit*/ true, SourceLoc(), SourceLoc(),
          DifferentiabilityKind::Reverse,
          /*parameterIndices*/ IndexSubset::get(C, 1, {0}),
          derivativeGenericSignature);
      member->getAttrs().add(diffableAttr);
    }
  }

  // If nominal type is `@frozen`, also mark `TangentVector` struct.
  if (nominal->getAttrs().hasAttribute<FrozenAttr>())
    structDecl->getAttrs().add(new (C) FrozenAttr(/*implicit*/ true));
  
  // Add `typealias TangentVector = Self` so that the `TangentVector` itself
  // won't need its own conformance derivation.
  auto *tangentEqualsSelfAlias = new (C) TypeAliasDecl(
      SourceLoc(), SourceLoc(), C.Id_TangentVector, SourceLoc(),
      /*GenericParams*/ nullptr, structDecl);
  tangentEqualsSelfAlias->setUnderlyingType(structDecl->getDeclaredInterfaceType());
  tangentEqualsSelfAlias->copyFormalAccessFrom(structDecl,
                                               /*sourceIsParentContext*/ true);
  tangentEqualsSelfAlias->setImplicit();
  tangentEqualsSelfAlias->setSynthesized();
  structDecl->addMember(tangentEqualsSelfAlias);

  // The implicit memberwise constructor must be explicitly created so that it
  // can called in `AdditiveArithmetic` and `Differentiable` methods. Normally,
  // the memberwise constructor is synthesized during SILGen, which is too late.
  TypeChecker::addImplicitConstructors(structDecl);

  // After memberwise initializer is synthesized, mark members as implicit.
  for (auto *member : structDecl->getStoredProperties())
    member->setImplicit();

  derived.addMembersToConformanceContext({structDecl});

  TypeChecker::checkConformancesInContext(structDecl);

  return structDecl;
}

/// Diagnose stored properties in the nominal that do not have an explicit
/// `@noDerivative` attribute, but either:
/// - Do not conform to `Differentiable`.
/// - Are a `let` stored property.
/// Emit a warning and a fixit so that users will make the attribute explicit.
static void checkAndDiagnoseImplicitNoDerivative(ASTContext &Context,
                                                 NominalTypeDecl *nominal,
                                                 DeclContext *DC) {
  // If nominal type can conform to `AdditiveArithmetic`, suggest adding a
  // conformance to `AdditiveArithmetic` in fix-its.
  // `Differentiable` protocol requirements all have default implementations
  // when `Self` conforms to `AdditiveArithmetic`, so `Differentiable`
  // derived conformances will no longer be necessary.
  bool nominalCanDeriveAdditiveArithmetic =
      DerivedConformance::canDeriveAdditiveArithmetic(nominal, DC);
  auto *diffableProto = Context.getProtocol(KnownProtocolKind::Differentiable);
  // Check all stored properties.
  for (auto *vd : nominal->getStoredProperties()) {
    // Peer through property wrappers: use original wrapped properties.
    if (auto *originalProperty = vd->getOriginalWrappedProperty()) {
      // Skip wrapped properties with `@noDerivative` attribute.
      if (originalProperty->getAttrs().hasAttribute<NoDerivativeAttr>())
        continue;
      // Diagnose wrapped properties whose property wrappers do not define
      // `wrappedValue.set`. `mutating func move(by:)` cannot be synthesized
      // to update these properties.
      if (!originalProperty->isSettable(DC)) {
        auto *wrapperDecl =
            vd->getInterfaceType()->getNominalOrBoundGenericNominal();
        auto loc =
            originalProperty->getAttributeInsertionLoc(/*forModifier*/ false);
        Context.Diags
            .diagnose(
                loc,
                diag::
                    differentiable_immutable_wrapper_implicit_noderivative_fixit,
                wrapperDecl->getName(), nominal->getName(),
                nominalCanDeriveAdditiveArithmetic)
            .fixItInsert(loc, "@noDerivative ");
        // Add an implicit `@noDerivative` attribute.
        originalProperty->getAttrs().add(
            new (Context) NoDerivativeAttr(/*Implicit*/ true));
        continue;
      }
      // Use the original wrapped property.
      vd = originalProperty;
    }
    if (vd->getInterfaceType()->hasError())
      continue;
    // Skip stored properties with `@noDerivative` attribute.
    if (vd->getAttrs().hasAttribute<NoDerivativeAttr>())
      continue;
    // Check whether to diagnose stored property.
    auto varType = DC->mapTypeIntoContext(vd->getValueInterfaceType());
    auto diffableConformance =
        TypeChecker::conformsToProtocol(varType, diffableProto,
                                        DC->getParentModule());
    // If stored property should not be diagnosed, continue.
    if (diffableConformance && 
        canInvokeMoveByOnProperty(vd, diffableConformance))
      continue;
    // Otherwise, add an implicit `@noDerivative` attribute.
    vd->getAttrs().add(new (Context) NoDerivativeAttr(/*Implicit*/ true));
    auto loc = vd->getAttributeInsertionLoc(/*forModifier*/ false);
    assert(loc.isValid() && "Expected valid source location");
    // Diagnose properties that do not conform to `Differentiable`.
    if (!diffableConformance) {
      Context.Diags
          .diagnose(
              loc,
              diag::differentiable_nondiff_type_implicit_noderivative_fixit,
              vd->getName(), vd->getTypeInContext(), nominal->getName(),
              nominalCanDeriveAdditiveArithmetic)
          .fixItInsert(loc, "@noDerivative ");
      continue;
    }
    // Otherwise, diagnose `let` property.
    Context.Diags
        .diagnose(loc,
                  diag::differentiable_let_property_implicit_noderivative_fixit,
                  nominal->getName(), nominalCanDeriveAdditiveArithmetic)
        .fixItInsert(loc, "@noDerivative ");
  }
}

/// Get or synthesize `TangentVector` struct type.
static std::pair<Type, TypeDecl *>
getOrSynthesizeTangentVectorStructType(DerivedConformance &derived) {
  auto *parentDC = derived.getConformanceContext();
  auto *nominal = derived.Nominal;
  auto &C = nominal->getASTContext();

  // Get or synthesize `TangentVector` struct.
  auto *tangentStruct =
      getOrSynthesizeTangentVectorStruct(derived, C.Id_TangentVector);
  if (!tangentStruct)
    return std::make_pair(nullptr, nullptr);

  // Check and emit warnings for implicit `@noDerivative` members.
  checkAndDiagnoseImplicitNoDerivative(C, nominal, parentDC);

  // Return the `TangentVector` struct type.
  return std::make_pair(
    parentDC->mapTypeIntoContext(
      tangentStruct->getDeclaredInterfaceType()),
    tangentStruct);
}

/// Synthesize the `TangentVector` struct type.
static std::pair<Type, TypeDecl *>
deriveDifferentiable_TangentVectorStruct(DerivedConformance &derived) {
  auto *parentDC = derived.getConformanceContext();
  auto *nominal = derived.Nominal;

  // If nominal type can derive `TangentVector` as the contextual `Self` type,
  // return it.
  if (canDeriveTangentVectorAsSelf(nominal, parentDC))
    return std::make_pair(parentDC->getSelfTypeInContext(), nullptr);

  // Otherwise, get or synthesize `TangentVector` struct type.
  return getOrSynthesizeTangentVectorStructType(derived);
}

ValueDecl *DerivedConformance::deriveDifferentiable(ValueDecl *requirement) {
  // Diagnose unknown requirements.
  if (requirement->getBaseName() != Context.Id_move) {
    Context.Diags.diagnose(requirement->getLoc(),
                           diag::broken_differentiable_requirement);
    return nullptr;
  }
  // Diagnose conformances in disallowed contexts.
  if (checkAndDiagnoseDisallowedContext(requirement))
    return nullptr;

  // Start an error diagnostic before attempting derivation.
  // If derivation succeeds, cancel the diagnostic.
  DiagnosticTransaction diagnosticTransaction(Context.Diags);
  ConformanceDecl->diagnose(diag::type_does_not_conform,
                            Nominal->getDeclaredType(), getProtocolType());
  requirement->diagnose(diag::no_witnesses,
                        getProtocolRequirementKind(requirement),
                        requirement, getProtocolType(), /*AddFixIt=*/false);

  // If derivation is possible, cancel the diagnostic and perform derivation.
  if (canDeriveDifferentiable(Nominal, getConformanceContext(), requirement)) {
    diagnosticTransaction.abort();
    if (requirement->getBaseName() == Context.Id_move)
      return deriveDifferentiable_move(*this);
  }

  // Otherwise, return nullptr.
  return nullptr;
}

std::pair<Type, TypeDecl *>
DerivedConformance::deriveDifferentiable(AssociatedTypeDecl *requirement) {
  // Diagnose unknown requirements.
  if (requirement->getBaseName() != Context.Id_TangentVector) {
    Context.Diags.diagnose(requirement->getLoc(),
                           diag::broken_differentiable_requirement);
    return std::make_pair(nullptr, nullptr);
  }

  // Start an error diagnostic before attempting derivation.
  // If derivation succeeds, cancel the diagnostic.
  DiagnosticTransaction diagnosticTransaction(Context.Diags);
  ConformanceDecl->diagnose(diag::type_does_not_conform,
                            Nominal->getDeclaredType(), getProtocolType());
  requirement->diagnose(diag::no_witnesses_type, requirement);

  // If derivation is possible, cancel the diagnostic and perform derivation.
  if (canDeriveDifferentiable(Nominal, getConformanceContext(), requirement)) {
    diagnosticTransaction.abort();
    return deriveDifferentiable_TangentVectorStruct(*this);
  }

  // Otherwise, return nullptr.
  return std::make_pair(nullptr, nullptr);
}
