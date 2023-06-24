//===--- CSDiagnostics.cpp - Constraint Diagnostics -----------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements diagnostics for the constraint system.
//
//===----------------------------------------------------------------------===//

#include "CSDiagnostics.h"
#include "MiscDiagnostics.h"
#include "TypeCheckProtocol.h"
#include "TypoCorrection.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsClangImporter.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/ClangImporter/ClangImporterRequests.h"
#include "swift/Parse/Lexer.h"
#include "swift/Sema/IDETypeChecking.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallString.h"
#include <string>

using namespace swift;
using namespace constraints;

static bool hasFixFor(const Solution &solution, ConstraintLocator *locator) {
  return llvm::any_of(solution.Fixes, [&locator](const ConstraintFix *fix) {
    return fix->getLocator() == locator;
  });
}

FailureDiagnostic::~FailureDiagnostic() {}

bool FailureDiagnostic::diagnose(bool asNote) {
  return asNote ? diagnoseAsNote() : diagnoseAsError();
}

bool FailureDiagnostic::diagnoseAsNote() {
  return false;
}

ASTNode FailureDiagnostic::getAnchor() const {
  auto *locator = getLocator();
  // Resolve the locator to a specific expression.

  auto anchor = locator->getAnchor();

  {
    SourceRange range;
    auto path = locator->getPath();

    simplifyLocator(anchor, path, range);
    if (!anchor)
      return locator->getAnchor();
  }

  return anchor;
}

Type FailureDiagnostic::getType(ASTNode node, bool wantRValue) const {
  return resolveType(getRawType(node), /*reconstituteSugar=*/false, wantRValue);
}

Type FailureDiagnostic::getRawType(ASTNode node) const {
  return S.getType(node);
}

Type FailureDiagnostic::resolveType(Type rawType, bool reconstituteSugar,
                                    bool wantRValue) const {
  rawType = rawType.transform([&](Type type) -> Type {
    if (auto *typeVar = type->getAs<TypeVariableType>()) {
      auto resolvedType = S.simplifyType(typeVar);

      if (!resolvedType->hasUnresolvedType())
        return resolvedType;

      // If type variable was simplified to an unresolved pack expansion
      // type, let's examine its original pattern type because it could
      // contain type variables replaceable with their generic parameter
      // types.
      if (auto *expansion = resolvedType->getAs<PackExpansionType>()) {
        auto *locator = typeVar->getImpl().getLocator();
        auto *openedExpansionTy =
            locator->castLastElementTo<LocatorPathElt::PackExpansionType>()
                .getOpenedType();
        auto patternType = resolveType(openedExpansionTy->getPatternType());
        return PackExpansionType::get(patternType, expansion->getCountType());
      }

      Type GP = typeVar->getImpl().getGenericParameter();
      return resolvedType->is<UnresolvedType>() && GP ? GP : resolvedType;
    }

    if (type->hasElementArchetype()) {
      auto *env = getDC()->getGenericEnvironmentOfContext();
      return env->mapElementTypeIntoPackContext(type);
    }

    return type->isPlaceholder() ? Type(type->getASTContext().TheUnresolvedType)
                                 : type;
  });

  if (reconstituteSugar)
    rawType = rawType->reconstituteSugar(/*recursive*/ true);
  return wantRValue ? rawType->getRValueType() : rawType;
}

template <typename... ArgTypes>
InFlightDiagnostic
FailureDiagnostic::emitDiagnostic(ArgTypes &&... Args) const {
  return emitDiagnosticAt(getLoc(), std::forward<ArgTypes>(Args)...);
}

template <typename... ArgTypes>
InFlightDiagnostic
FailureDiagnostic::emitDiagnosticAt(ArgTypes &&... Args) const {
  auto &DE = getASTContext().Diags;
  DiagnosticBehavior behaviorLimit;
  switch (fixBehavior) {
  case FixBehavior::Error:
  case FixBehavior::AlwaysWarning:
    behaviorLimit = DiagnosticBehavior::Unspecified;
    break;

  case FixBehavior::DowngradeToWarning:
    behaviorLimit = DiagnosticBehavior::Warning;
    break;

  case FixBehavior::Suppress:
    behaviorLimit = DiagnosticBehavior::Ignore;
    break;
  }

  return std::move(DE.diagnose(std::forward<ArgTypes>(Args)...)
                     .limitBehavior(behaviorLimit));
}

Expr *FailureDiagnostic::findParentExpr(const Expr *subExpr) const {
  auto &cs = getConstraintSystem();
  return cs.getParentExpr(const_cast<Expr *>(subExpr));
}

ArgumentList *
FailureDiagnostic::getArgumentListFor(ConstraintLocator *locator) const {
  return S.getArgumentList(locator);
}

Expr *FailureDiagnostic::getBaseExprFor(const Expr *anchor) const {
  if (!anchor)
    return nullptr;

  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(anchor))
    return UDE->getBase();
  else if (auto *SE = dyn_cast<SubscriptExpr>(anchor))
    return SE->getBase();
  else if (auto *MRE = dyn_cast<MemberRefExpr>(anchor))
    return MRE->getBase();
  else if (auto *call = dyn_cast<CallExpr>(anchor)) {
    auto fnType = getType(call->getFn());
    if (fnType->isCallAsFunctionType(getDC())) {
      return call->getFn();
    }
  }

  return nullptr;
}

Type FailureDiagnostic::restoreGenericParameters(
    Type type,
    llvm::function_ref<void(GenericTypeParamType *, Type)> substitution) {
  llvm::SmallPtrSet<GenericTypeParamType *, 4> processed;
  return type.transform([&](Type type) -> Type {
    if (auto *typeVar = type->getAs<TypeVariableType>()) {
      type = resolveType(typeVar);
      if (auto *GP = typeVar->getImpl().getGenericParameter()) {
        if (processed.insert(GP).second)
          substitution(GP, type);
        return GP;
      }
    }

    return type;
  });
}

bool FailureDiagnostic::conformsToKnownProtocol(
    Type type, KnownProtocolKind protocol) const {
  auto &cs = getConstraintSystem();
  return TypeChecker::conformsToKnownProtocol(type, protocol,
                                              cs.DC->getParentModule());
}

Type RequirementFailure::getOwnerType() const {
  auto anchor = getRawAnchor();

  // If diagnostic is anchored at assignment expression
  // it means that requirement failure happened while trying
  // to convert source to destination, which means that
  // owner type is actually not an assignment expression
  // itself but its source.
  if (auto *assignment = getAsExpr<AssignExpr>(anchor))
    anchor = assignment->getSrc();

  return getType(anchor)->getInOutObjectType()->getMetatypeInstanceType();
}

const GenericContext *RequirementFailure::getGenericContext() const {
  if (auto *genericCtx = AffectedDecl->getAsGenericContext())
    return genericCtx;

  auto parentDecl = AffectedDecl->getDeclContext()->getAsDecl();
  if (!parentDecl)
    return nullptr;

  return parentDecl->getAsGenericContext();
}

const Requirement &RequirementFailure::getRequirement() const {
  // If this is a conditional requirement failure we need to
  // fetch conformance from constraint system associated with
  // type requirement this conditional conformance belongs to.
  auto requirements = isConditional()
                          ? Conformance->getConditionalRequirements()
                          : Signature.getRequirements();
  return requirements[getRequirementIndex()];
}

ProtocolConformance *RequirementFailure::getConformanceForConditionalReq(
    ConstraintLocator *locator) {
  auto reqElt = locator->castLastElementTo<LocatorPathElt::AnyRequirement>();
  if (!reqElt.isConditionalRequirement())
    return nullptr;

  auto conformanceRef =
    locator->findLast<LocatorPathElt::ConformanceRequirement>();
  assert(conformanceRef && "Invalid locator for a conditional requirement");
  return conformanceRef->getConformance();
}

ValueDecl *RequirementFailure::getDeclRef() const {
  // Get a declaration associated with given type (if any).
  // This is used to retrieve affected declaration when
  // failure is in any way contextual, and declaration can't
  // be fetched directly from constraint system.
  auto getAffectedDeclFromType = [](Type type) -> ValueDecl * {
    assert(type);
    // If problem is related to a typealias, let's point this
    // diagnostic directly to its declaration without desugaring.
    if (auto *alias = dyn_cast<TypeAliasType>(type.getPointer()))
      return alias->getDecl();

    if (auto existential = type->getAs<ExistentialType>())
      return existential->getConstraintType()->getAnyGeneric();

    return type->getAnyGeneric();
  };

  // TODO: potentially we are tracking more information than we need to here
  // because the decl might also available via the contextual type. In the long
  // run we probably want to refactor to get rid of get/set contextual.
  if (auto opaqueLocator =
          getLocator()->findFirst<LocatorPathElt::OpenedOpaqueArchetype>()) {
    return opaqueLocator->getDecl();
  }

  // If the locator is for a result builder body result type, the requirement
  // came from the function's return type.
  if (getLocator()->isForResultBuilderBodyResult()) {
    auto *func = getAsDecl<FuncDecl>(getAnchor());
    return getAffectedDeclFromType(func->getResultInterfaceType());
  }

  if (isFromContextualType()) {
    auto anchor = getRawAnchor();
    auto contextualPurpose = getContextualTypePurpose(anchor);
    auto contextualTy = getContextualType(anchor);

    // If the issue is a mismatch between `return` statement/expression
    // and its contextual requirements, it means that affected declaration
    // is a declarer of a contextual "result" type e.g. member of a
    // type, local function etc.
    if (contextualPurpose == CTP_ReturnStmt ||
        contextualPurpose == CTP_ReturnSingleExpr) {
      return cast<ValueDecl>(getDC()->getAsDecl());
    }

    if (contextualPurpose == CTP_DefaultParameter ||
        contextualPurpose == CTP_AutoclosureDefaultParameter) {
      return cast<ValueDecl>(getDC()->getParent()->getAsDecl());
    }

    // We check for a contextual type here because we form a
    // ContextualType(CTP_Initialization) LocatorPathElt for pattern bindings
    // when there is no TypedPattern present. In such a case, there will be
    // no recorded contextual type (though the pattern may produce a type that
    // could be considered contextual).
    if (contextualTy) {
      // If the contextual type is e.g a tuple, we may not be able to resolve
      // a decl. Fall through to getting the 'owner type' in that case.
      if (auto *D = getAffectedDeclFromType(contextualTy))
        return D;
    } else {
      assert((contextualPurpose == CTP_Initialization ||
              contextualPurpose == CTP_Unused) &&
             "Should have had a contextual type");
    }
  }

  if (getLocator()->isFirstElement<LocatorPathElt::CoercionOperand>())
    return getAffectedDeclFromType(getOwnerType());

  if (auto overload = getCalleeOverloadChoiceIfAvailable(getLocator())) {
    // If there is a declaration associated with this
    // failure e.g. an overload choice of the call
    // expression, let's see whether failure is
    // associated with it directly or rather with
    // one of its parents.
    if (auto *decl = overload->choice.getDeclOrNull()) {
      // If declaration is an operator let's always use
      // it to produce `in reference to` diagnostics.
      if (decl->isOperator())
        return decl;

      auto *DC = decl->getDeclContext();

      do {
        if (auto *parent = DC->getAsDecl()) {
          if (auto *GC = parent->getAsGenericContext()) {
            // FIXME: Is this intending an exact match?
            if (GC->getGenericSignature().getPointer() != Signature.getPointer())
              continue;

            // If this is a signature if an extension
            // then it means that code has referenced
            // something incorrectly and diagnostic
            // should point to the referenced declaration.
            if (isa<ExtensionDecl>(parent))
              break;

            return cast<ValueDecl>(parent);
          }
        }
      } while ((DC = DC->getParent()));

      return decl;
    }
  }

  return getAffectedDeclFromType(getOwnerType());
}

GenericSignature RequirementFailure::getSignature(ConstraintLocator *locator) {
  if (isConditional())
    return Conformance->getGenericSignature();

  if (auto genericElt = locator->findLast<LocatorPathElt::OpenedGeneric>())
    return genericElt->getSignature();

  llvm_unreachable("Type requirement failure should always have signature");
}

bool RequirementFailure::isFromContextualType() const {
  auto path = getLocator()->getPath();
  assert(!path.empty());
  return path.front().getKind() == ConstraintLocator::ContextualType;
}

const DeclContext *RequirementFailure::getRequirementDC() const {
  // In case of conditional requirement failure, we don't
  // have to guess where the it comes from.
  if (isConditional())
    return Conformance->getDeclContext();

  const auto &req = getRequirement();
  auto *DC = AffectedDecl->getDeclContext();

  do {
    if (auto sig = DC->getGenericSignatureOfContext()) {
      if (sig->isRequirementSatisfied(req))
        return DC;
    }
  } while ((DC = DC->getParent()));

  return AffectedDecl->getAsGenericContext();
}

bool RequirementFailure::isStaticOrInstanceMember(const ValueDecl *decl) {
  if (decl->isInstanceMember())
    return true;

  if (auto *AFD = dyn_cast<AbstractFunctionDecl>(decl))
    return AFD->isStatic() && !AFD->isOperator();

  return decl->isStatic();
}

bool WrappedValueMismatch::diagnoseAsError() {
  auto *locator = getLocator();
  auto elt = locator->castLastElementTo<LocatorPathElt::WrappedValue>();

  emitDiagnostic(diag::composed_property_wrapper_mismatch, getFromType(),
                 resolveType(elt.getType())->getString(), getToType());

  return true;
}

bool RequirementFailure::diagnoseAsError() {
  const auto *reqDC = getRequirementDC();
  auto *genericCtx = getGenericContext();

  // Instead of printing archetypes rooted on an opened existential, which
  // are currently an implementation detail, have a weird textual
  // representation, and may be misleading (a root opened archetype prints like
  // an existential type), use the corresponding 'Self'-rooted interface types
  // from the requirement, which are more familiar.
  const auto lhs = [&] {
    if (getLHS()->hasOpenedExistential()) {
      return getRequirement().getFirstType();
    }

    return getLHS();
  }();
  const auto rhs = [&] {
    if (getRHS()->hasOpenedExistential()) {
      return getRequirement().getSecondType();
    }

    return getRHS();
  }();

  if (auto *OTD = dyn_cast<OpaqueTypeDecl>(AffectedDecl)) {
    auto *namingDecl = OTD->getNamingDecl();
    emitDiagnostic(diag::type_does_not_conform_in_opaque_return,
                   namingDecl->getDescriptiveKind(), namingDecl->getName(),
                   lhs, rhs, rhs->isAnyObject());

    if (auto *repr = namingDecl->getOpaqueResultTypeRepr()) {
      emitDiagnosticAt(repr->getLoc(), diag::opaque_return_type_declared_here)
          .highlight(repr->getSourceRange());
    }
    return true;
  }

  if (reqDC->isTypeContext() && genericCtx != reqDC &&
      (genericCtx->isChildContextOf(reqDC) ||
       isStaticOrInstanceMember(AffectedDecl))) {
    auto *NTD = reqDC->getSelfNominalTypeDecl();
    emitDiagnostic(
        getDiagnosticInRereference(), AffectedDecl->getDescriptiveKind(),
        AffectedDecl->getName(), NTD->getDeclaredType(), lhs, rhs);
  } else {
    emitDiagnostic(getDiagnosticOnDecl(), AffectedDecl->getDescriptiveKind(),
                   AffectedDecl->getName(), lhs, rhs);
  }

  maybeEmitRequirementNote(reqDC->getAsDecl(), lhs, rhs);

  return true;
}

bool RequirementFailure::diagnoseAsNote() {
  const auto &req = getRequirement();
  const auto *reqDC = getRequirementDC();

  // Layout requirement doesn't have a second type, let's always
  // `AnyObject`.
  auto requirementTy = req.getKind() == RequirementKind::Layout
                           ? getASTContext().getAnyObjectConstraint()
                           : req.getSecondType();

  emitDiagnosticAt(reqDC->getAsDecl(), getDiagnosticAsNote(), getLHS(),
                   getRHS(), req.getFirstType(), requirementTy);
  return true;
}

void RequirementFailure::maybeEmitRequirementNote(const Decl *anchor, Type lhs,
                                                  Type rhs) const {
  auto &req = getRequirement();

  if (req.getKind() != RequirementKind::SameType) {
    if (auto wrappedType = lhs->getOptionalObjectType()) {
      auto kind = (req.getKind() == RequirementKind::Superclass ?
                   ConstraintKind::Subtype : ConstraintKind::ConformsTo);
      if (TypeChecker::typesSatisfyConstraint(wrappedType, rhs,
                                              /*openArchetypes=*/false,
                                              kind, getDC()))
        emitDiagnostic(diag::wrapped_type_satisfies_requirement, wrappedType);
    }
  }

  if (isConditional()) {
    emitDiagnosticAt(anchor,
                     diag::requirement_implied_by_conditional_conformance,
                     resolveType(Conformance->getType()),
                     Conformance->getProtocol()->getDeclaredInterfaceType());
    return;
  }

  if (req.getKind() == RequirementKind::Layout ||
      rhs->isEqual(req.getSecondType())) {
    // If the note is tautological, bail out.
    if (lhs->isEqual(req.getFirstType())) {
      return;
    }

    emitDiagnosticAt(anchor, diag::where_requirement_failure_one_subst,
                     req.getFirstType(), lhs);
    return;
  }

  if (lhs->isEqual(req.getFirstType())) {
    emitDiagnosticAt(anchor, diag::where_requirement_failure_one_subst,
                     req.getSecondType(), rhs);
    return;
  }

  if (req.getKind() == RequirementKind::SameShape) {
    // Same-shape requirements are broken down into two ShapeOf
    // constraints against the same type variable, so a failure is not
    // necessarily ordered to match the original requriement. For now,
    // don't emit a note.
    return;
  }

  emitDiagnosticAt(anchor, diag::where_requirement_failure_both_subst,
                   req.getFirstType(), lhs, req.getSecondType(), rhs);
}

bool MissingConformanceFailure::diagnoseAsError() {
  auto anchor = getAnchor();
  auto nonConformingType = getLHS();
  auto protocolType = getRHS();

  // If this is a requirement of a pattern-matching operator,
  // let's see whether argument already has a fix associated
  // with it and if so skip conformance error, otherwise we'd
  // produce an unrelated `<type> doesn't conform to Equatable protocol`
  // diagnostic.
  if (isPatternMatchingOperator(anchor)) {
    auto *expr = castToExpr(anchor);
    if (auto *binaryOp = dyn_cast_or_null<BinaryExpr>(findParentExpr(expr))) {
      auto *caseExpr = binaryOp->getLHS();

      llvm::SmallPtrSet<Expr *, 4> anchors;
      for (const auto *fix : getSolution().Fixes) {
        if (auto anchor = fix->getAnchor()) {
          auto path = fix->getLocator()->getPath();
          SourceRange range;
          simplifyLocator(anchor, path, range);
          if (anchor && anchor.is<Expr *>())
            anchors.insert(getAsExpr(anchor));
        }
      }

      bool hasFix = false;
      auto &cs = getConstraintSystem();
      cs.forEachExpr(caseExpr, [&](Expr *expr) -> Expr * {
        hasFix |= anchors.count(expr);
        return hasFix ? nullptr : expr;
      });

      if (hasFix)
        return false;
    }
  }

  // If the problem has been (unambiguously) determined to be related
  // to one of of the standard comparison operators and argument is
  // enum with associated values, let's produce a tailored note which
  // says that conformances for enums with associated values can't be
  // synthesized.
  if (isStandardComparisonOperator(anchor)) {
    auto *expr = castToExpr(anchor);
    auto isEnumWithAssociatedValues = [](Type type) -> bool {
      if (auto *enumType = type->getAs<EnumType>())
        return !enumType->getDecl()->hasOnlyCasesWithoutAssociatedValues();
      return false;
    };

    // Limit this to `Equatable` and `Comparable` protocols for now.
    auto *protocol = getRHS()->castTo<ProtocolType>()->getDecl();
    if (isEnumWithAssociatedValues(getLHS()) &&
        (protocol->isSpecificProtocol(KnownProtocolKind::Equatable) ||
         protocol->isSpecificProtocol(KnownProtocolKind::Comparable))) {
      if (RequirementFailure::diagnoseAsError()) {
        auto opName = getOperatorName(expr);
        emitDiagnostic(diag::no_binary_op_overload_for_enum_with_payload,
                       opName->str());
        return true;
      }
    }
  }

  if (diagnoseAsAmbiguousOperatorRef())
    return true;

  if (nonConformingType->isObjCExistentialType()) {
    emitDiagnostic(diag::protocol_does_not_conform_static, nonConformingType,
                   protocolType);
    return true;
  }

  if (diagnoseTypeCannotConform(nonConformingType, protocolType))
    return true;

  // If none of the special cases could be diagnosed,
  // let's fallback to the most general diagnostic.
  return RequirementFailure::diagnoseAsError();
}

bool MissingConformanceFailure::diagnoseTypeCannotConform(
    Type nonConformingType, Type protocolType) const {
  if (getRequirement().getKind() == RequirementKind::Layout ||
      !(nonConformingType->is<AnyFunctionType>() ||
      nonConformingType->is<TupleType>() ||
      nonConformingType->isExistentialType() ||
      nonConformingType->is<AnyMetatypeType>())) {
    return false;
  }

  Type constraintType = nonConformingType;
  if (auto existential = constraintType->getAs<ExistentialType>())
    constraintType = existential->getConstraintType();

  emitDiagnostic(diag::type_cannot_conform,
                 nonConformingType, protocolType);

  bool emittedSpecializedNote = false;
  if (auto protoType = protocolType->getAs<ProtocolType>()) {
    if (protoType->getDecl()->isSpecificProtocol(KnownProtocolKind::Sendable)) {
      if (nonConformingType->is<FunctionType>()) {
        emitDiagnostic(diag::nonsendable_function_type);
        emittedSpecializedNote = true;
      } else if (nonConformingType->is<TupleType>()) {
        emitDiagnostic(diag::nonsendable_tuple_type);
        emittedSpecializedNote = true;
      }
    }
  }

  if (!emittedSpecializedNote)
    emitDiagnostic(diag::only_concrete_types_conform_to_protocols);

  if (auto *OTD = dyn_cast<OpaqueTypeDecl>(AffectedDecl)) {
    auto *namingDecl = OTD->getNamingDecl();
    if (auto *repr = namingDecl->getOpaqueResultTypeRepr()) {
      emitDiagnosticAt(repr->getLoc(), diag::required_by_opaque_return,
                       namingDecl->getDescriptiveKind(),
                       namingDecl->getName())
          .highlight(repr->getSourceRange());
    }
    return true;
  }

  auto &req = getRequirement();
  auto *reqDC = getRequirementDC();
  auto *genericCtx = getGenericContext();
  auto noteLocation = reqDC->getAsDecl()->getLoc();

  if (!noteLocation.isValid())
    noteLocation = getLoc();

  if (isConditional()) {
    emitDiagnosticAt(noteLocation,
                     diag::requirement_implied_by_conditional_conformance,
                     resolveType(Conformance->getType()),
                     Conformance->getProtocol()->getDeclaredInterfaceType());
  } else if (genericCtx != reqDC && (genericCtx->isChildContextOf(reqDC) ||
                                     isStaticOrInstanceMember(AffectedDecl))) {
    emitDiagnosticAt(noteLocation, diag::required_by_decl_ref,
                     AffectedDecl->getDescriptiveKind(),
                     AffectedDecl->getName(),
                     reqDC->getSelfNominalTypeDecl()->getDeclaredType(),
                     req.getFirstType(), nonConformingType);
  } else {
    emitDiagnosticAt(noteLocation, diag::required_by_decl,
                     AffectedDecl->getDescriptiveKind(),
                     AffectedDecl->getName(), req.getFirstType(),
                     nonConformingType);
  }

  return true;
}

bool MissingConformanceFailure::diagnoseAsAmbiguousOperatorRef() {
  auto anchor = getRawAnchor();
  auto *ODRE = getAsExpr<OverloadedDeclRefExpr>(anchor);
  if (!ODRE)
    return false;

  auto isStandardType = [](Type ty) {
    return ty->isStdlibType() || ty->is<TupleType>();
  };

  auto name = ODRE->getDecls().front()->getBaseName();
  if (!(name.isOperator() && isStandardType(getLHS()) &&
        isStandardType(getRHS()))) {
    return false;
  }
  // If this is an operator reference and both types are from stdlib,
  // let's produce a generic diagnostic about invocation and a note
  // about missing conformance just in case.
  auto operatorID = name.getIdentifier();

  auto *fnType = getType(anchor)->getAs<AnyFunctionType>();
  auto params = fnType->getParams();
  if (params.size() == 2) {
    auto lhsType = params[0].getPlainType();
    auto rhsType = params[1].getPlainType();

    if (lhsType->isEqual(rhsType)) {
      emitDiagnostic(diag::cannot_apply_binop_to_same_args, operatorID.str(),
                     lhsType);
    } else {
      emitDiagnostic(diag::cannot_apply_binop_to_args, operatorID.str(),
                     lhsType, rhsType);
    }
  } else {
    emitDiagnostic(diag::cannot_apply_unop_to_arg, operatorID.str(),
                   params[0].getPlainType());
  }

  diagnoseAsNote();
  return true;
}

bool SameShapeExpansionFailure::diagnoseAsError() {
  emitDiagnostic(diag::expansion_expr_not_same_shape, lhs, rhs);
  return true;
}

Optional<Diag<Type, Type>> GenericArgumentsMismatchFailure::getDiagnosticFor(
    ContextualTypePurpose context) {
  switch (context) {
  case CTP_Initialization:
  case CTP_AssignSource:
    return diag::cannot_convert_assign;
  case CTP_ReturnStmt:
  case CTP_ReturnSingleExpr:
    return diag::cannot_convert_to_return_type;
  case CTP_DefaultParameter:
  case CTP_AutoclosureDefaultParameter:
    return diag::cannot_convert_default_arg_value;
  case CTP_YieldByValue:
    return diag::cannot_convert_yield_value;
  case CTP_DiscardStmt:
    return diag::cannot_convert_discard_value;
  case CTP_CallArgument:
    return diag::cannot_convert_argument_value;
  case CTP_ClosureResult:
    return diag::cannot_convert_closure_result;
  case CTP_ArrayElement:
    return diag::cannot_convert_array_element;
  case CTP_DictionaryKey:
    return diag::cannot_convert_dict_key;
  case CTP_DictionaryValue:
    return diag::cannot_convert_dict_value;
  case CTP_CoerceOperand:
    return diag::cannot_convert_coerce;
  case CTP_SubscriptAssignSource:
    return diag::cannot_convert_subscript_assign;
  case CTP_Condition:
    return diag::cannot_convert_condition_value;
  case CTP_WrappedProperty:
    return diag::wrapped_value_mismatch;

  case CTP_CaseStmt:
  case CTP_ThrowStmt:
  case CTP_ForEachStmt:
  case CTP_ForEachSequence:
  case CTP_ComposedPropertyWrapper:
  case CTP_Unused:
  case CTP_CannotFail:
  case CTP_YieldByReference:
  case CTP_CalleeResult:
  case CTP_EnumCaseRawValue:
  case CTP_ExprPattern:
  case CTP_SingleValueStmtBranch:
    break;
  }
  return None;
}

void GenericArgumentsMismatchFailure::emitNoteForMismatch(int position) {
  auto *locator = getLocator();
  // Since there could be implicit conversions associated with argument
  // to parameter conversions, let's use parameter type as a source of
  // generic parameter information.
  auto paramSourceTy =
      locator->isLastElement<LocatorPathElt::ApplyArgToParam>() ? getRequired()
                                                                : getActual();

  auto genericTypeDecl = paramSourceTy->getAnyGeneric();
  auto param = genericTypeDecl->getGenericParams()->getParams()[position];

  auto lhs = getActual()->getGenericArgs()[position];
  auto rhs = getRequired()->getGenericArgs()[position];

  auto noteLocation = param->getLoc();

  if (!noteLocation.isValid())
    noteLocation = getLoc();

  emitDiagnosticAt(noteLocation, diag::generic_argument_mismatch,
                   param->getName(), lhs, rhs);
}

bool GenericArgumentsMismatchFailure::diagnoseAsError() {
  auto anchor = getAnchor();

  auto fromType = getFromType();
  auto toType = getToType();

  // This is a situation where right-hand size type is wrapped
  // into a number of optionals and argument isn't e.g.
  //
  // func test(_: UnsafePointer<Int>??) {}
  //
  // var value: Float = 0
  // test(&value)
  //
  // `value` has to get implicitly wrapped into 2 optionals
  // before pointer types could be compared.
  auto locator = getLocator();
  auto path = locator->getPath();
  unsigned toDrop = 0;
  for (const auto &elt : llvm::reverse(path)) {
    if (!elt.is<LocatorPathElt::OptionalPayload>())
      break;

    // Disregard optional payload element to look at its source.
    ++toDrop;
  }

  path = path.drop_back(toDrop);

  Optional<Diag<Type, Type>> diagnostic;
  if (path.empty()) {
    if (isExpr<AssignExpr>(anchor)) {
      diagnostic = getDiagnosticFor(CTP_AssignSource);
    } else if (locator->isForCoercion()) {
      diagnostic = getDiagnosticFor(CTP_CoerceOperand);
    } else {
      return false;
    }
  } else {
    const auto &last = path.back();
    switch (last.getKind()) {
    case ConstraintLocator::TernaryBranch:
      diagnostic = diag::ternary_expr_cases_mismatch;
      break;

    case ConstraintLocator::ContextualType: {
      auto purpose = getContextualTypePurpose();
      assert(!(purpose == CTP_Unused || purpose == CTP_CannotFail));

      // If this is call to a closure e.g. `let _: A = { B() }()`
      // let's point diagnostic to its result.
      if (auto *call = getAsExpr<CallExpr>(anchor)) {
        auto *fnExpr = call->getFn();
        if (auto *closure = dyn_cast<ClosureExpr>(fnExpr)) {
          purpose = CTP_ClosureResult;
          if (closure->hasSingleExpressionBody())
            anchor = closure->getSingleExpressionBody();
        }
      }

      if (purpose == CTP_ReturnStmt) {
        if (auto *DRE = getAsExpr<DeclRefExpr>(anchor)) {
          auto *decl = DRE->getDecl();
          if (decl && decl->hasName()) {
            auto baseName = DRE->getDecl()->getBaseIdentifier();
            if (baseName.str().startswith("$__builder")) {
              diagnostic =
                  diag::cannot_convert_result_builder_result_to_return_type;
              break;
            }
          }
        }
      }

      diagnostic = getDiagnosticFor(purpose);
      break;
    }

    case ConstraintLocator::ResultBuilderBodyResult:
      diagnostic = diag::cannot_convert_result_builder_result_to_return_type;
      break;

    case ConstraintLocator::AutoclosureResult:
    case ConstraintLocator::ApplyArgToParam:
    case ConstraintLocator::ApplyArgument: {
      diagnostic = diag::cannot_convert_argument_value;
      break;
    }

    case ConstraintLocator::ParentType: {
      diagnostic = diag::cannot_convert_parent_type;
      break;
    }

    case ConstraintLocator::ClosureBody:
    case ConstraintLocator::ClosureResult: {
      diagnostic = diag::cannot_convert_closure_result;
      break;
    }

    case ConstraintLocator::TupleElement: {
      auto rawAnchor = getRawAnchor();

      if (isExpr<ArrayExpr>(rawAnchor)) {
        diagnostic = getDiagnosticFor(CTP_ArrayElement);
      } else if (isExpr<DictionaryExpr>(rawAnchor)) {
        auto eltLoc = last.castTo<LocatorPathElt::TupleElement>();
        diagnostic = getDiagnosticFor(
            eltLoc.getIndex() == 0 ? CTP_DictionaryKey : CTP_DictionaryValue);
      }
      break;
    }

    case ConstraintLocator::UnresolvedMemberChainResult: {
      diagnostic = diag::cannot_convert_chain_result_type;
      break;
    }

    case ConstraintLocator::CoercionOperand: {
      diagnostic = getDiagnosticFor(CTP_CoerceOperand);
      break;
    }

    default:
      break;
    }
  }

  if (!diagnostic) {
    // Handle all mismatches involving an `AssignExpr`
    if (auto *assignExpr = getAsExpr<AssignExpr>(anchor)) {
      diagnostic = getDiagnosticFor(CTP_AssignSource);
      fromType = getType(assignExpr->getSrc());
      toType = getType(assignExpr->getDest());
    } else {
      // If we couldn't find a specific diagnostic let's fallback to
      // attempt to handle cases where we have an apply arg to param.
      auto applyInfo = getFunctionArgApplyInfo(getLocator());
      if (applyInfo) {
        diagnostic = diag::cannot_convert_argument_value;
        fromType = applyInfo->getArgType();
        toType = applyInfo->getParamType();
      }
    }
  }
  
  if (!diagnostic)
    return false;

  emitDiagnosticAt(::getLoc(anchor), *diagnostic, fromType, toType);
  emitNotesForMismatches();
  return true;
}

/// Determine the parameter context to use for diagnostics purposes.
static ParameterContext getParameterContextForDiag(ASTNode anchor) {
  if (isExpr<SubscriptExpr>(anchor))
    return ParameterContext::Subscript;

  if (isExpr<MacroExpansionExpr>(anchor))
    return ParameterContext::MacroExpansion;

  return ParameterContext::Call;
}

bool LabelingFailure::diagnoseAsError() {
  auto *args = getArgumentListFor(getLocator());
  if (!args)
    return false;

  auto paramContext = getParameterContextForDiag(getRawAnchor());
  return diagnoseArgumentLabelError(getASTContext(), args, CorrectLabels,
                                    paramContext);
}

bool LabelingFailure::diagnoseAsNote() {
  auto *args = getArgumentListFor(getLocator());
  if (!args)
    return false;

  SmallVector<Identifier, 4> scratch;
  auto argLabels = args->getArgumentLabels(scratch);

  auto stringifyLabels = [](ArrayRef<Identifier> labels) -> std::string {
    std::string str;
    for (auto label : labels) {
      str += label.empty() ? "_" : label.str();
      str += ':';
    }
    return "(" + str + ")";
  };

  auto selectedOverload = getCalleeOverloadChoiceIfAvailable(getLocator());
  if (!selectedOverload)
    return false;

  const auto &choice = selectedOverload->choice;
  if (auto *decl = choice.getDeclOrNull()) {
    emitDiagnosticAt(decl, diag::candidate_expected_different_labels,
                     stringifyLabels(argLabels),
                     stringifyLabels(CorrectLabels));
    return true;
  }

  return false;
}

bool ArrayLiteralToDictionaryConversionFailure::diagnoseAsError() {
  ArrayExpr *AE = getAsExpr<ArrayExpr>(getAnchor());
  assert(AE);

  if (AE->getNumElements() == 0) {
    emitDiagnostic(diag::should_use_empty_dictionary_literal)
      .fixItInsertAfter(getLoc(), ":");
    return true;
  }

  auto CTP = getConstraintSystem().getContextualTypePurpose(AE);
  emitDiagnostic(diag::should_use_dictionary_literal,
                 getToType()->lookThroughAllOptionalTypes(),
                 CTP == CTP_Initialization);

  auto diagnostic = emitDiagnostic(diag::meant_dictionary_lit);
  const auto numElements = AE->getNumElements();
  if (numElements == 1) {
    diagnostic.fixItInsertAfter(AE->getElement(0)->getEndLoc(), ": <#value#>");
  } else {
    // If there is an even number of elements in the array, let's produce
    // a fix-it which suggests to replace "," with ":" to form a dictionary
    // literal.
    if ((numElements & 1) == 0) {
      const auto commaLocs = AE->getCommaLocs();
      if (commaLocs.size() == numElements - 1) {
        for (unsigned i = 0, e = numElements / 2; i != e; ++i)
          diagnostic.fixItReplace(commaLocs[i * 2], ":");
      }
    }
  }
  return true;
}

bool AttributedFuncToTypeConversionFailure::diagnoseAsError() {
  if (diagnoseParameterUse())
    return true;

  if (auto *typeVar = getRawFromType()->getAs<TypeVariableType>()) {
    if (auto *GP = typeVar->getImpl().getGenericParameter()) {
      emitDiagnostic(diag::converting_noattrfunc_to_type, attributeKind, GP);
      return true;
    }
  }

  emitDiagnostic(
      diag::converting_noattrfunc_to_type, attributeKind, getToType());
  return true;
}

static VarDecl *getDestinationVarDecl(AssignExpr *AE,
                                      const Solution &solution) {
  ConstraintLocator *locator = nullptr;
  if (auto *URDE = dyn_cast<UnresolvedDotExpr>(AE->getDest())) {
    locator = solution.getConstraintLocator(URDE, {ConstraintLocator::Member});
  } else if (auto *declRef = dyn_cast<DeclRefExpr>(AE->getDest())) {
    locator = solution.getConstraintLocator(declRef);
  }
  if (!locator)
    return nullptr;

  auto overload = solution.getOverloadChoiceIfAvailable(locator);
  if (!overload)
    return nullptr;

  return dyn_cast_or_null<VarDecl>(overload->choice.getDecl());
}

bool AttributedFuncToTypeConversionFailure::
    diagnoseFunctionParameterEscapenessMismatch(AssignExpr *AE) const {
  auto loc = getLocator();
  if (attributeKind != Escaping)
    return false;

  if (!loc->findLast<LocatorPathElt::FunctionArgument>())
    return false;

  auto destType = getType(AE->getDest())->lookThroughAllOptionalTypes();
  auto destFnType = destType->castTo<FunctionType>();
  auto sourceType = getType(AE->getSrc())->lookThroughAllOptionalTypes();

  // The tuple locator element will give us the exact parameter mismatch
  // position.
  auto tupleElt = loc->getLastElementAs<LocatorPathElt::TupleElement>();
  auto mismatchPosition = tupleElt ? tupleElt->getIndex() : 0;
  auto param = destFnType->getParams()[mismatchPosition];

  emitDiagnostic(diag::cannot_convert_assign, sourceType, destType);
  emitDiagnosticAt(AE->getDest()->getLoc(),
                   diag::escape_expected_at_parameter_position,
                   mismatchPosition, param.getParameterType());

  auto &solution = getSolution();
  auto decl = getDestinationVarDecl(AE, solution);
  // We couldn't find a declaration to add an extra note with a fix-it but
  // the main diagnostic was already covered.
  if (!decl)
    return true;

  auto declRepr = decl->getTypeReprOrParentPatternTypeRepr();
  class TopLevelFuncReprFinder : public ASTWalker {
    PreWalkAction walkToTypeReprPre(TypeRepr *TR) override {
      FnRepr = dyn_cast<FunctionTypeRepr>(TR);
      return Action::VisitChildrenIf(FnRepr == nullptr);
    }

    /// Walk macro arguments.
    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

  public:
    FunctionTypeRepr *FnRepr;
    TopLevelFuncReprFinder() : FnRepr(nullptr) {}
  };

  // Look to find top-level function repr that maybe inside optional
  // representations.
  TopLevelFuncReprFinder fnFinder;
  declRepr->walk(fnFinder);

  auto declFnRepr = fnFinder.FnRepr;
  if (!declFnRepr)
    return true;

  auto note = emitDiagnosticAt(decl->getLoc(), diag::add_explicit_escaping,
                               mismatchPosition);
  auto argsRepr = declFnRepr->getArgsTypeRepr();
  auto argRepr = argsRepr->getElement(mismatchPosition).Type;
  if (!param.isAutoClosure()) {
    note.fixItInsert(argRepr->getStartLoc(), "@escaping ");
  } else {
    auto attrRepr = dyn_cast<AttributedTypeRepr>(argRepr);
    if (attrRepr) {
      auto autoclosureEndLoc = Lexer::getLocForEndOfToken(
          getASTContext().SourceMgr,
          attrRepr->getAttrs().getLoc(TAK_autoclosure));
      note.fixItInsertAfter(autoclosureEndLoc, " @escaping");
    }
  }
  return true;
}

bool AttributedFuncToTypeConversionFailure::diagnoseParameterUse() const {
  auto convertTo = getToType();
  // If the other side is not a function, we have common case diagnostics
  // which handle function-to-type conversion diagnostics.
  if (!convertTo->is<FunctionType>())
    return false;

  auto anchor = getAnchor();
  auto diagnostic = diag::general_noattrfunc_to_attr;

  ParamDecl *PD = nullptr;
  if (auto *DRE = getAsExpr<DeclRefExpr>(anchor)) {
    PD = dyn_cast<ParamDecl>(DRE->getDecl());

    // If anchor is not a parameter declaration there
    // is no need to dig up more information.
    if (!PD)
      return false;

    // Let's check whether this is a function parameter passed
    // as an argument to another function which accepts @escaping
    // function at that position.
    if (auto argApplyInfo = getFunctionArgApplyInfo(getLocator())) {
      auto paramInterfaceTy = argApplyInfo->getParamInterfaceType();
      if (paramInterfaceTy->isTypeParameter() &&
          attributeKind == AttributeKind::Escaping) {
        auto diagnoseGenericParamFailure = [&](GenericTypeParamDecl *decl) {
          emitDiagnostic(diag::converting_noescape_param_to_generic_type,
                         PD->getName(), paramInterfaceTy);

          auto declLoc = decl->getLoc();
          if (declLoc.isValid())
            emitDiagnosticAt(decl, diag::generic_parameters_always_escaping);
        };

        // If this is a situation when non-escaping parameter is passed
        // to the argument which represents generic parameter, there is
        // a tailored diagnostic for that.

        if (auto *DMT = paramInterfaceTy->getAs<DependentMemberType>()) {
          diagnoseGenericParamFailure(DMT->getRootGenericParam()->getDecl());
          return true;
        }

        if (auto *GP = paramInterfaceTy->getAs<GenericTypeParamType>()) {
          diagnoseGenericParamFailure(GP->getDecl());
          return true;
        }
      }

      // If there are no generic parameters involved, this could
      // only mean that parameter is expecting @escaping/@Sendable function
      // type.
      diagnostic = diag::passing_noattrfunc_to_attrfunc;
    }
  } else if (auto *AE = getAsExpr<AssignExpr>(getRawAnchor())) {
    // Attempt to diagnose escape/non-escape mismatch in function
    // parameter position.
    if (diagnoseFunctionParameterEscapenessMismatch(AE))
      return true;

    if (auto *DRE = dyn_cast<DeclRefExpr>(AE->getSrc())) {
      PD = dyn_cast<ParamDecl>(DRE->getDecl());
      diagnostic = diag::assigning_noattrfunc_to_attrfunc;
    }
  }

  if (!PD)
    return false;

  emitDiagnostic(diagnostic, attributeKind, PD->getName());

  // Give a note and fix-it
  auto note = emitDiagnosticAt(
      PD, diag::noescape_parameter, attributeKind, PD->getName());

  SourceLoc reprLoc;
  SourceLoc autoclosureEndLoc;
  if (auto *repr = PD->getTypeRepr()) {
    reprLoc = repr->getStartLoc();
    if (auto *attrRepr = dyn_cast<AttributedTypeRepr>(repr)) {
      autoclosureEndLoc = Lexer::getLocForEndOfToken(
          getASTContext().SourceMgr,
          attrRepr->getAttrs().getLoc(TAK_autoclosure));
    }
  }
  if (attributeKind == AttributeKind::Concurrent) {
    note.fixItInsert(reprLoc, "@Sendable ");
  }
  else if (!PD->isAutoClosure()) {
    note.fixItInsert(reprLoc, "@escaping ");
  } else {
    note.fixItInsertAfter(autoclosureEndLoc, " @escaping");
  }

  return true;
}

ASTNode InvalidCoercionFailure::getAnchor() const {
  auto anchor = FailureDiagnostic::getAnchor();
  if (auto *assignExpr = getAsExpr<AssignExpr>(anchor))
    return assignExpr->getSrc();
  return anchor;
}

SourceLoc InvalidCoercionFailure::getLoc() const {
  if (getLocator()->isForCoercion()) {
    auto *CE = castToExpr<CoerceExpr>(getRawAnchor());
    return CE->getAsLoc();
  }
  return FailureDiagnostic::getLoc();
}

bool InvalidCoercionFailure::diagnoseAsError() {
  auto fromType = getFromType();
  auto toType = getToType();

  emitDiagnostic(diag::cannot_coerce_to_type, fromType, toType);

  if (UseConditionalCast) {
    emitDiagnostic(diag::missing_optional_downcast)
        .highlight(getSourceRange())
        .fixItReplace(getLoc(), "as?");
  } else {
    emitDiagnostic(diag::missing_forced_downcast)
        .highlight(getSourceRange())
        .fixItReplace(getLoc(), "as!");
  }

  return true;
}

bool MissingAddressOfFailure::diagnoseAsError() {
  auto argTy = getFromType();
  auto paramTy = getToType();

  if (paramTy->getAnyPointerElementType()) {
    emitDiagnostic(diag::cannot_convert_argument_value, argTy, paramTy)
        .fixItInsert(getSourceRange().Start, "&");
  } else {
    emitDiagnostic(diag::missing_address_of, argTy)
        .fixItInsert(getSourceRange().Start, "&");
  }
  return true;
}

ASTNode MissingExplicitConversionFailure::getAnchor() const {
  auto anchor = FailureDiagnostic::getAnchor();

  if (auto *assign = getAsExpr<AssignExpr>(anchor))
    return assign->getSrc();

  if (auto *paren = getAsExpr<ParenExpr>(anchor))
    return paren->getSubExpr();

  return anchor;
}

bool MissingExplicitConversionFailure::diagnoseAsError() {
  auto *DC = getDC();
  auto *anchor = castToExpr(getAnchor());

  auto fromType = getFromType();
  auto toType = getToType();

  if (!toType->hasTypeRepr())
    return false;

  bool useAs = TypeChecker::isExplicitlyConvertibleTo(fromType, toType, DC);

  auto *expr = findParentExpr(anchor);
  if (!expr)
    expr = const_cast<Expr *>(anchor);

  // If we're performing pattern matching,
  // "as" means something completely different...
  if (auto binOpExpr = dyn_cast<BinaryExpr>(expr)) {
    auto overloadedFn = dyn_cast<OverloadedDeclRefExpr>(binOpExpr->getFn());
    if (overloadedFn && !overloadedFn->getDecls().empty()) {
      ValueDecl *decl0 = overloadedFn->getDecls()[0];
      if (decl0->getBaseName() == decl0->getASTContext().Id_MatchOperator)
        return false;
    }
  }

  bool needsParensInside = exprNeedsParensBeforeAddingAs(anchor, DC);
  bool needsParensOutside = exprNeedsParensAfterAddingAs(anchor, DC);

  llvm::SmallString<2> insertBefore;
  llvm::SmallString<32> insertAfter;
  if (needsParensOutside) {
    insertBefore += "(";
  }
  if (needsParensInside) {
    insertBefore += "(";
    insertAfter += ")";
  }
  insertAfter += useAs ? " as " : " as! ";
  insertAfter += toType->getWithoutParens()->getString();
  if (needsParensOutside)
    insertAfter += ")";

  auto diagnose = [&]() {
    if (useAs) {
      return emitDiagnostic(diag::missing_explicit_conversion, fromType,
                            toType);
    } else {
      // Emit error diagnostic.
      emitDiagnostic(diag::cannot_coerce_to_type, fromType, toType);
      // Emit and return note suggesting as! where the fix-it will be placed.
      return emitDiagnostic(diag::missing_forced_downcast);
    }
  };

  auto diag = diagnose();
  if (!insertBefore.empty()) {
    diag.fixItInsert(getSourceRange().Start, insertBefore);
  }
  diag.fixItInsertAfter(getSourceRange().End, insertAfter);
  return true;
}

ASTNode MemberReferenceFailure::getAnchor() const {
  auto anchor = FailureDiagnostic::getAnchor();
  if (auto base = getBaseExprFor(getAsExpr(anchor))) {
    return base;
  } else {
    return anchor;
  }
}

SourceRange MemberAccessOnOptionalBaseFailure::getSourceRange() const {
  if (auto componentPathElt =
          getLocator()->getLastElementAs<LocatorPathElt::KeyPathComponent>()) {
    auto anchor = getAnchor();
    auto keyPathExpr = castToExpr<KeyPathExpr>(anchor);
    if (componentPathElt->getIndex() == 0) {
      if (auto rootType = keyPathExpr->getRootType()) {
        return rootType->getSourceRange();
      } else {
        return keyPathExpr->getComponents().front().getLoc();
      }
    } else {
      auto componentIdx = componentPathElt->getIndex() - 1;
      auto component = keyPathExpr->getComponents()[componentIdx];
      return component.getSourceRange();
    }
  }
  return FailureDiagnostic::getSourceRange();
}

bool MemberAccessOnOptionalBaseFailure::diagnoseAsError() {
  auto baseType = getMemberBaseType();
  auto locator = getLocator();

  // If this is an issue with `makeIterator` having an optional
  // result, it would be diagnosed by fix on the base type.
  if (auto anchor = locator->getAnchor()) {
    if (auto *UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
      if (UDE->isImplicit()) {
        auto &solution = getSolution();
        auto *baseLoc = solution.getConstraintLocator(
            UDE->getBase(),
            LocatorPathElt::ContextualType(CTP_ForEachSequence));

        if (hasFixFor(solution, baseLoc))
          return false;
      }
    }
  }


  bool resultIsOptional = ResultTypeIsOptional;

  // If we've resolved the member overload to one that returns an optional
  // type, then the result of the expression is optional (and we want to offer
  // only a '?' fixit) even though the constraint system didn't need to add any
  // additional optionality.
  auto overload = getOverloadChoiceIfAvailable(locator);
  if (overload && overload->adjustedOpenedType->getOptionalObjectType())
    resultIsOptional = true;

  auto unwrappedBaseType = baseType->getOptionalObjectType();
  if (!unwrappedBaseType)
    return false;
  
  auto sourceRange = getSourceRange();

  auto componentPathElt =
      locator->getLastElementAs<LocatorPathElt::KeyPathComponent>();
  if (componentPathElt && componentPathElt->getIndex() == 0) {
    // For members where the base type is an optional key path root
    // let's emit a tailored note suggesting to use its unwrapped type.
    auto *keyPathExpr = castToExpr<KeyPathExpr>(getAnchor());
    if (auto rootType = keyPathExpr->getRootType()) {
      emitDiagnostic(diag::optional_base_not_unwrapped, baseType, Member,
                     unwrappedBaseType);

      emitDiagnostic(diag::optional_base_remove_optional_for_keypath_root,
                     unwrappedBaseType)
          .fixItReplace(rootType->getSourceRange(),
                        unwrappedBaseType.getString());
    } else {
      emitDiagnostic(diag::invalid_optional_inferred_keypath_root, baseType,
                     Member, unwrappedBaseType);
      emitDiagnostic(diag::optional_key_path_root_base_chain, Member)
          .fixItInsert(sourceRange.End, "?.");
      emitDiagnostic(diag::optional_key_path_root_base_unwrap, Member)
          .fixItInsert(sourceRange.End, "!.");
    }
  } else {
    // Check whether or not the base of this optional unwrap is implicit self
    // This can only happen with a [weak self] capture, and is not permitted.
    if (auto dotExpr = getAsExpr<UnresolvedDotExpr>(locator->getAnchor())) {
      if (auto baseDeclRef = dyn_cast<DeclRefExpr>(dotExpr->getBase())) {
        ASTContext &Ctx = baseDeclRef->getDecl()->getASTContext();
        if (baseDeclRef->isImplicit() &&
            baseDeclRef->getDecl()->getName().isSimpleName(Ctx.Id_self)) {
          emitDiagnostic(diag::optional_self_not_unwrapped);

          emitDiagnostic(diag::optional_self_chain)
              .fixItInsertAfter(sourceRange.End, "self?.");
          return true;
        }
      }
    }
    
    emitDiagnostic(diag::optional_base_not_unwrapped, baseType, Member,
                   unwrappedBaseType);

    // FIXME: It would be nice to immediately offer "base?.member ?? defaultValue"
    // for non-optional results where that would be appropriate. For the moment
    // always offering "?" means that if the user chooses chaining, we'll end up
    // in MissingOptionalUnwrapFailure:diagnose() to offer a default value during
    // the next compile.
    emitDiagnostic(diag::optional_base_chain, Member)
        .fixItInsertAfter(sourceRange.End, "?");

    if (!resultIsOptional) {
      emitDiagnostic(diag::unwrap_with_force_value)
          .fixItInsertAfter(sourceRange.End, "!");
    }
  }

  return true;
}

void MissingOptionalUnwrapFailure::offerDefaultValueUnwrapFixIt(
    DeclContext *DC, const Expr *expr) const {
  assert(expr);

  auto *anchor = getAsExpr(getAnchor());
  // If anchor is n explicit address-of, or expression which produces
  // an l-value (e.g. first argument of `+=` operator), let's not
  // suggest default value here because that would produce r-value type.
  if (!anchor || isa<InOutExpr>(anchor))
    return;

  if (auto argApplyInfo = getFunctionArgApplyInfo(getLocator()))
    if (argApplyInfo->getParameterFlags().isInOut())
      return;

  auto diag = emitDiagnosticAt(expr->getLoc(), diag::unwrap_with_default_value);

  // Figure out what we need to parenthesize.
  bool needsParensInside =
      exprNeedsParensBeforeAddingNilCoalescing(DC, const_cast<Expr *>(expr));
  bool needsParensOutside = exprNeedsParensAfterAddingNilCoalescing(
      DC, const_cast<Expr *>(expr), [&](auto *E) { return findParentExpr(E); });

  llvm::SmallString<2> insertBefore;
  llvm::SmallString<32> insertAfter;
  if (needsParensOutside) {
    insertBefore += "(";
  }
  if (needsParensInside) {
    insertBefore += "(";
    insertAfter += ")";
  }
  insertAfter += " ?? <" "#default value#" ">";
  if (needsParensOutside)
    insertAfter += ")";

  if (!insertBefore.empty()) {
    diag.fixItInsert(expr->getStartLoc(), insertBefore);
  }
  diag.fixItInsertAfter(expr->getEndLoc(), insertAfter);
}

// Suggest a force-unwrap.
void MissingOptionalUnwrapFailure::offerForceUnwrapFixIt(
    const Expr *expr) const {
  auto diag = emitDiagnosticAt(expr->getLoc(), diag::unwrap_with_force_value);

  // If expr is optional as the result of an optional chain and this last
  // dot isn't a member returning optional, then offer to force the last
  // link in the chain, rather than an ugly parenthesized postfix force.
  if (auto optionalChain = dyn_cast<OptionalEvaluationExpr>(expr)) {
    if (auto dotExpr =
        dyn_cast<UnresolvedDotExpr>(optionalChain->getSubExpr())) {
      auto bind = dyn_cast<BindOptionalExpr>(dotExpr->getBase());
      if (bind && !getType(dotExpr)->getOptionalObjectType()) {
        diag.fixItReplace(SourceRange(bind->getLoc()), "!");
        return;
      }
    }
  }

  if (expr->canAppendPostfixExpression(true)) {
    diag.fixItInsertAfter(expr->getEndLoc(), "!");
  } else {
    diag.fixItInsert(expr->getStartLoc(), "(")
        .fixItInsertAfter(expr->getEndLoc(), ")!");
  }
}

// FIXME: This walks a partially-type checked function body, which
// is not guaranteed to yield consistent results. We should come up
// with another way of performing this analysis, for example by moving
// it to a post-type checking pass in MiscDiagnostics.
class VarDeclMultipleReferencesChecker : public ASTWalker {
  DeclContext *DC;
  VarDecl *varDecl;
  int count;

  /// Walk everything in a macro.
  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
      if (DRE->getDecl() == varDecl)
        ++count;
    }

    // FIXME: We can see UnresolvedDeclRefExprs here because we have
    // not yet run preCheckExpression() on the entire function body
    // yet.
    //
    // We could consider pre-checking more eagerly.
    if (auto *UDRE = dyn_cast<UnresolvedDeclRefExpr>(E)) {
      auto name = UDRE->getName();
      auto loc = UDRE->getLoc();
      if (name.isSimpleName(varDecl->getName()) && loc.isValid()) {
        auto *otherDecl =
            ASTScope::lookupSingleLocalDecl(DC->getParentSourceFile(),
                                            name.getFullName(), loc);
        if (otherDecl == varDecl)
          ++count;
      }
    }

    return Action::Continue(E);
  }

public:
  VarDeclMultipleReferencesChecker(DeclContext *DC, VarDecl *varDecl)
      : DC(DC), varDecl(varDecl),count(0) {}
  int referencesCount() { return count; }
};

bool DroppedGlobalActorFunctionAttr::diagnoseAsError() {
  auto fromFnType = getFromType()->getAs<AnyFunctionType>();
  if (!fromFnType)
    return false;

  Type fromGlobalActor = fromFnType->getGlobalActor();
  if (!fromGlobalActor)
    return false;

  emitDiagnostic(
      diag::converting_func_loses_global_actor, getFromType(), getToType(),
      fromGlobalActor);
  return true;
}

bool MissingOptionalUnwrapFailure::diagnoseAsError() {
  if (!getUnwrappedType()->isBool()) {
    if (diagnoseConversionToBool())
      return true;
  }

  auto *anchor = castToExpr(getAnchor());

  if (auto assignExpr = dyn_cast<AssignExpr>(anchor))
    anchor = assignExpr->getSrc();

  auto *unwrappedExpr = anchor->getValueProvidingExpr();

  if (auto *tryExpr = dyn_cast<OptionalTryExpr>(unwrappedExpr)) {
    bool isSwift5OrGreater = getASTContext().isSwiftVersionAtLeast(5);
    auto subExprType = getType(tryExpr->getSubExpr());
    bool subExpressionIsOptional = (bool)subExprType->getOptionalObjectType();

    if (isSwift5OrGreater && subExpressionIsOptional) {
      // Using 'try!' won't change the type for a 'try?' with an optional
      // sub-expr under Swift 5+, so just report that a missing unwrap can't be
      // handled here.
      return false;
    }

    emitDiagnosticAt(tryExpr->getTryLoc(), diag::missing_unwrap_optional_try,
                     getType(anchor))
        .fixItReplace({tryExpr->getTryLoc(), tryExpr->getQuestionLoc()},
                      "try!");
    return true;
  }

  auto baseType = getBaseType();
  auto unwrappedType = getUnwrappedType();

  assert(!baseType->hasTypeVariable() &&
         "Base type must not be a type variable");
  assert(!baseType->isPlaceholder() &&
         "Base type must not be a type placeholder");
  assert(!unwrappedType->hasTypeVariable() &&
         "Unwrapped type must not be a type variable");
  assert(!unwrappedType->isPlaceholder() &&
         "Unwrapped type must not be a type placeholder");

  if (!baseType->getOptionalObjectType())
    return false;

  emitDiagnosticAt(unwrappedExpr->getLoc(), diag::optional_not_unwrapped,
                   baseType, unwrappedType);

  // If this is a function type, suggest using optional chaining to
  // call it.
  if (unwrappedType->lookThroughAllOptionalTypes()->is<FunctionType>()) {
    bool isDeclRefExpr = false;
    if (isa<DeclRefExpr>(unwrappedExpr)) {
      isDeclRefExpr = true;
    } else if (auto fve = dyn_cast<ForceValueExpr>(unwrappedExpr)) {
      isDeclRefExpr = isa<DeclRefExpr>(fve->getSubExpr());
    } else if (auto boe = dyn_cast<BindOptionalExpr>(unwrappedExpr)) {
      isDeclRefExpr = isa<DeclRefExpr>(boe->getSubExpr());
    }
    if (isDeclRefExpr) {
      auto depth = baseType->getOptionalityDepth();
      auto diag = emitDiagnosticAt(unwrappedExpr->getLoc(),
                                   diag::perform_optional_chain_on_function_type);
      auto fixItString = std::string(depth, '?');
      diag.fixItInsertAfter(unwrappedExpr->getEndLoc(), fixItString);
    }
  }

  // If the expression we're unwrapping is the only reference to a
  // local variable whose type isn't explicit in the source, then
  // offer unwrapping fixits on the initializer as well.
  if (auto declRef = dyn_cast<DeclRefExpr>(unwrappedExpr)) {
    if (auto varDecl = dyn_cast<VarDecl>(declRef->getDecl())) {
      bool singleUse = false;
      AbstractFunctionDecl *AFD = nullptr;
      if ((AFD = dyn_cast<AbstractFunctionDecl>(varDecl->getDeclContext()))) {
        auto checker = VarDeclMultipleReferencesChecker(getDC(), varDecl);
        if (auto *body = AFD->getBody())
          body->walk(checker);
        singleUse = checker.referencesCount() == 1;
      }

      PatternBindingDecl *binding = varDecl->getParentPatternBinding();
      if (singleUse && binding && binding->getNumPatternEntries() == 1 &&
          varDecl->getTypeSourceRangeForDiagnostics().isInvalid()) {

        auto *initializer = varDecl->getParentInitializer();
        if (!initializer)
          return true;

        if (auto declRefExpr = dyn_cast<DeclRefExpr>(initializer)) {
          if (declRefExpr->getDecl()->isImplicitlyUnwrappedOptional()) {
            emitDiagnosticAt(declRefExpr->getLoc(),
                             diag::unwrap_iuo_initializer, baseType);
          }
        }

        auto fnTy = AFD->getInterfaceType()->castTo<AnyFunctionType>();
        bool voidReturn =
            fnTy->getResult()->isEqual(TupleType::getEmpty(getASTContext()));

        auto diag =
            emitDiagnosticAt(varDecl->getLoc(), diag::unwrap_with_guard);
        diag.fixItInsert(binding->getStartLoc(), "guard ");
        if (voidReturn) {
          diag.fixItInsertAfter(binding->getEndLoc(), " else { return }");
        } else {
          diag.fixItInsertAfter(binding->getEndLoc(), " else { return <"
                                                      "#default value#"
                                                      "> }");
        }
        diag.flush();

        offerDefaultValueUnwrapFixIt(varDecl->getDeclContext(), initializer);
        offerForceUnwrapFixIt(initializer);
      }
    }
  }

  offerDefaultValueUnwrapFixIt(getDC(), unwrappedExpr);
  offerForceUnwrapFixIt(unwrappedExpr);
  return true;
}

bool RValueTreatedAsLValueFailure::diagnoseAsError() {
  Diag<StringRef> subElementDiagID;
  Diag<Type> rvalueDiagID = diag::assignment_lhs_not_lvalue;
  auto diagExpr = castToExpr(getRawAnchor());
  SourceLoc loc = diagExpr->getLoc();

  // Assignment is not allowed inside of a condition,
  // so let's not diagnose immutability, because
  // most likely the problem is related to use of `=` itself.
  if (getContextualTypePurpose(diagExpr) == CTP_Condition)
    return false;

  // If the failure happened at the end of an unresolved member chain, it should
  // be diagnosed instead as though it happened at the last element.
  if (auto chainExpr = dyn_cast<UnresolvedMemberChainResultExpr>(diagExpr))
      diagExpr = chainExpr->getSubExpr();

  if (auto assignExpr = dyn_cast<AssignExpr>(diagExpr)) {
    // Let's check whether this is an attempt to assign
    // variable or property to itself.
    if (TypeChecker::diagnoseSelfAssignment(assignExpr))
      return true;

    diagExpr = assignExpr->getDest();
  }

  if (auto callExpr = dyn_cast<ApplyExpr>(diagExpr)) {
    loc = callExpr->getFn()->getLoc();
    auto *locator = getLocator();

    // `argument attribute` is used for identification purposes
    // only, so it could be looked through in this situation.
    if (locator->isLastElement<LocatorPathElt::ArgumentAttribute>()) {
      auto path = locator->getPath();
      locator = getConstraintLocator(getRawAnchor(), path.drop_back());
    }

    if (auto argInfo = getFunctionArgApplyInfo(locator)) {
      if (isa<PrefixUnaryExpr>(callExpr) || isa<PostfixUnaryExpr>(callExpr)) {
        subElementDiagID = diag::cannot_apply_lvalue_unop_to_subelement;
        rvalueDiagID = diag::cannot_apply_lvalue_unop_to_rvalue;
      } else if (isa<BinaryExpr>(callExpr)) {
        subElementDiagID = diag::cannot_apply_lvalue_binop_to_subelement;
        rvalueDiagID = diag::cannot_apply_lvalue_binop_to_rvalue;
      } else {
        subElementDiagID = diag::cannot_pass_rvalue_inout_subelement;
        rvalueDiagID = diag::cannot_pass_rvalue_inout;
      }
      diagExpr = argInfo->getArgExpr();
    } else {
      subElementDiagID = diag::assignment_lhs_is_apply_expression;
    }
  } else if (auto *inoutExpr = dyn_cast<InOutExpr>(diagExpr)) {
    if (auto info = getFunctionArgApplyInfo(getLocator())) {
      auto paramType = info->getParamType();
      auto argType = getType(inoutExpr)->getWithoutSpecifierType();

      PointerTypeKind ptr;
      if (argType->isArrayType() && paramType->getAnyPointerElementType(ptr)
          && (ptr == PTK_UnsafePointer || ptr == PTK_UnsafeRawPointer)) {
        emitDiagnosticAt(inoutExpr->getLoc(),
                         diag::extra_address_of_unsafepointer, paramType)
            .highlight(inoutExpr->getSourceRange())
            .fixItRemove(inoutExpr->getStartLoc());
        return true;
      }
    }

    subElementDiagID = diag::cannot_pass_rvalue_inout_subelement;
    rvalueDiagID = diag::cannot_pass_rvalue_inout;
    diagExpr = inoutExpr->getSubExpr();
  } else if (isa<DeclRefExpr>(diagExpr)) {
    subElementDiagID = diag::assignment_lhs_is_immutable_variable;
  } else if (isa<ForceValueExpr>(diagExpr)) {
    subElementDiagID = diag::assignment_bang_has_immutable_subcomponent;
  } else if (isa<MemberRefExpr>(diagExpr)) {
    subElementDiagID = diag::assignment_lhs_is_immutable_property;
  } else if (auto member = dyn_cast<UnresolvedDotExpr>(diagExpr)) {
    subElementDiagID = diag::assignment_lhs_is_immutable_property;

    if (auto *ctor = dyn_cast<ConstructorDecl>(getDC())) {
      if (auto *baseRef = dyn_cast<DeclRefExpr>(member->getBase())) {
        if (baseRef->getDecl() == ctor->getImplicitSelfDecl() &&
            ctor->getDelegatingOrChainedInitKind().initKind ==
            BodyInitKind::Delegating) {
          emitDiagnosticAt(loc, diag::assignment_let_property_delegating_init,
                           member->getName());
          if (auto overload = getOverloadChoiceIfAvailable(
                  getConstraintLocator(member, ConstraintLocator::Member))) {
            if (auto *ref = overload->choice.getDeclOrNull())
              emitDiagnosticAt(ref, diag::decl_declared_here,
                               ref->getName());
          }
          return true;
        }
      }
    }

    if (auto resolvedOverload =
            getCalleeOverloadChoiceIfAvailable(getLocator())) {
      if (resolvedOverload->choice.getKind() ==
          OverloadChoiceKind::DynamicMemberLookup)
        subElementDiagID = diag::assignment_dynamic_property_has_immutable_base;

      if (resolvedOverload->choice.isKeyPathDynamicMemberLookup()) {
        if (!getType(member->getBase(), /*wantRValue=*/false)->hasLValueType())
          subElementDiagID =
              diag::assignment_dynamic_property_has_immutable_base;
      }
    }
  } else if (isa<SubscriptExpr>(diagExpr)) {
      subElementDiagID = diag::assignment_subscript_has_immutable_base;
  } else if (auto *UME = dyn_cast<UnresolvedMemberExpr>(diagExpr)) {
    subElementDiagID = diag::assignment_lhs_is_immutable_property;
  } else {
    subElementDiagID = diag::assignment_lhs_is_immutable_variable;
  }

  AssignmentFailure failure(diagExpr, getSolution(), loc, subElementDiagID,
                            rvalueDiagID);
  return failure.diagnose();
}

bool RValueTreatedAsLValueFailure::diagnoseAsNote() {
  auto overload = getCalleeOverloadChoiceIfAvailable(getLocator());
  if (!(overload && overload->choice.isDecl()))
    return false;

  auto *decl = overload->choice.getDecl();
  emitDiagnosticAt(decl, diag::candidate_is_not_assignable,
                   decl->getDescriptiveKind(), decl->getName());
  return true;
}

static VarDecl *findSimpleReferencedVarDecl(const Expr *E) {
  if (auto *LE = dyn_cast<LoadExpr>(E))
    E = LE->getSubExpr();

  if (auto *DRE = dyn_cast<DeclRefExpr>(E))
    return dyn_cast<VarDecl>(DRE->getDecl());

  return nullptr;
}

static std::pair<VarDecl *, VarDecl *> findReferencedVarDecl(const Expr *E) {
  E = E->getValueProvidingExpr();

  if (auto *LE = dyn_cast<LoadExpr>(E))
    return findReferencedVarDecl(LE->getSubExpr());

  if (auto *AE = dyn_cast<AssignExpr>(E))
    return findReferencedVarDecl(AE->getDest());

  if (auto *D = findSimpleReferencedVarDecl(E))
    return std::make_pair(nullptr, D);

  if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
    if (auto *BaseDecl = findSimpleReferencedVarDecl(MRE->getBase()))
      return std::make_pair(BaseDecl, cast<VarDecl>(MRE->getMember().getDecl()));
  }

  return std::make_pair(nullptr, nullptr);
}

bool TypeChecker::diagnoseSelfAssignment(const Expr *expr) {
  auto *assignExpr = dyn_cast<AssignExpr>(expr);
  if (!assignExpr)
    return false;

  auto *dstExpr = assignExpr->getDest();
  auto *srcExpr = assignExpr->getSrc();

  auto dstDecl = findReferencedVarDecl(dstExpr);
  auto srcDecl = findReferencedVarDecl(srcExpr);

  if (dstDecl.second &&
      dstDecl.second->hasStorage() &&
      dstDecl == srcDecl) {
    auto &DE = dstDecl.second->getASTContext().Diags;
    DE.diagnose(expr->getLoc(), dstDecl.first ? diag::self_assignment_prop
                                              : diag::self_assignment_var)
        .highlight(dstExpr->getSourceRange())
        .highlight(srcExpr->getSourceRange());
    return true;
  }

  return false;
}

bool TrailingClosureAmbiguityFailure::diagnoseAsNote() {
  auto *anchor = getAsExpr(getAnchor());
  // This diagnostic is used opportunistically in `diagnoseAmbiguity`,
  // which means it cannot assume that anchor is always an expression.
  if (!anchor)
    return false;

  const auto *expr = findParentExpr(anchor);
  auto *callExpr = dyn_cast_or_null<CallExpr>(expr);
  if (!callExpr)
    return false;

  // FIXME(https://github.com/apple/swift/issues/57381): We ought to handle multiple trailing closures here.
  if (callExpr->getArgs()->getNumTrailingClosures() != 1)
    return false;
  if (callExpr->getFn() != anchor)
    return false;

  llvm::SmallMapVector<Identifier, const ValueDecl *, 8> choicesByLabel;
  for (const auto &choice : Choices) {
    auto *callee = dyn_cast<AbstractFunctionDecl>(choice.getDecl());
    if (!callee)
      return false;

    const ParameterList *paramList = callee->getParameters();
    const ParamDecl *param = paramList->getArray().back();

    // Sanity-check that the trailing closure corresponds to this parameter.
    if (!param->hasInterfaceType() ||
        !param->getInterfaceType()->is<AnyFunctionType>())
      return false;

    Identifier trailingClosureLabel = param->getArgumentName();
    auto &choiceForLabel = choicesByLabel[trailingClosureLabel];

    // FIXME: Cargo-culted from diagnoseAmbiguity: apparently the same decl can
    // appear more than once?
    if (choiceForLabel == callee)
      continue;

    // If just providing the trailing closure label won't solve the ambiguity,
    // don't bother offering the fix-it.
    if (choiceForLabel != nullptr)
      return false;

    choiceForLabel = callee;
  }

  // If we got here, then all of the choices have unique labels. Offer them in
  // order.
  for (const auto &choicePair : choicesByLabel) {
    auto diag = emitDiagnosticAt(
        expr->getLoc(), diag::ambiguous_because_of_trailing_closure,
        choicePair.first.empty(), choicePair.second->getName());
    swift::fixItEncloseTrailingClosure(getASTContext(), diag, callExpr,
                                       choicePair.first);
  }

  return true;
}

AssignmentFailure::AssignmentFailure(Expr *destExpr, const Solution &solution,
                                     SourceLoc diagnosticLoc)
    : FailureDiagnostic(solution, destExpr), DestExpr(destExpr),
      Loc(diagnosticLoc),
      DeclDiagnostic(findDeclDiagnostic(getASTContext(), destExpr)),
      TypeDiagnostic(diag::assignment_lhs_not_lvalue) {}

bool AssignmentFailure::diagnoseAsError() {
  auto *DC = getDC();

  // Walk through the destination expression, resolving what the problem is.  If
  // we find a node in the lvalue path that is problematic, this returns it.
  Expr *immutableExpr;
  Optional<OverloadChoice> choice;
  std::tie(immutableExpr, choice) = resolveImmutableBase(DestExpr);

  // Attempt diagnostics based on the overload choice.
  if (choice.has_value()) {

    auto getKeyPathArgument = [](SubscriptExpr *expr) {
      auto *args = expr->getArgs();
      assert(args->isUnary());
      assert(args->getLabel(0).str() == "keyPath");
      return args->getExpr(0);
    };

    if (!choice->isDecl()) {
      if (choice->getKind() == OverloadChoiceKind::KeyPathApplication &&
          !isa<ApplyExpr>(immutableExpr)) {
        std::string message = "key path is read-only";
        if (auto *SE = dyn_cast<SubscriptExpr>(immutableExpr)) {
          if (auto *DRE = dyn_cast<DeclRefExpr>(getKeyPathArgument(SE))) {
            auto identifier = DRE->getDecl()->getBaseIdentifier();
            message =
                "'" + identifier.str().str() + "' is a read-only key path";
          }
        }
        emitDiagnosticAt(Loc, DeclDiagnostic, message)
            .highlight(immutableExpr->getSourceRange());
        return true;
      }
      return false;
    }

    // Otherwise, we cannot resolve this because the available setter candidates
    // are all mutating and the base must be mutating.  If we dug out a
    // problematic decl, we can produce a nice tailored diagnostic.
    if (auto *VD = dyn_cast<VarDecl>(choice->getDecl())) {
      std::string message = "'";
      message += VD->getName().str().str();
      message += "'";

      auto type = getType(immutableExpr);

      if (isKnownKeyPathType(type))
        message += " is read-only";
      else if (VD->isCaptureList())
        message += " is an immutable capture";
      else if (VD->isImplicit())
        message += " is immutable";
      else if (VD->isLet())
        message += " is a 'let' constant";
      else if (!VD->isSettable(DC))
        message += " is a get-only property";
      else if (!VD->isSetterAccessibleFrom(DC))
        message += " setter is inaccessible";
      else {
        message += " is immutable";
      }

      emitDiagnosticAt(Loc, DeclDiagnostic, message)
          .highlight(immutableExpr->getSourceRange());

      // If there is a masked property of the same type, emit a
      // note to fixit prepend a 'self.' or 'Type.'.
      if (auto typeContext = DC->getInnermostTypeContext()) {
        SmallVector<ValueDecl *, 2> results;
        DC->lookupQualified(typeContext->getSelfNominalTypeDecl(),
                            VD->createNameRef(), Loc,
                            NL_QualifiedDefault, results);

        auto foundProperty = llvm::find_if(results, [&](ValueDecl *decl) {
          // We're looking for a settable property that is the same type as the
          // var we found.
          auto *var = dyn_cast<VarDecl>(decl);
          if (!var || var == VD)
            return false;

          if (!var->isSettable(DC) || !var->isSetterAccessibleFrom(DC))
            return false;

          if (!var->getType()->isEqual(VD->getType()))
            return false;

          // Don't suggest a property if we're in one of its accessors.
          auto *methodDC = DC->getInnermostMethodContext();
          if (auto *AD = dyn_cast_or_null<AccessorDecl>(methodDC))
            if (AD->getStorage() == var)
              return false;

          return true;
        });

        if (foundProperty != results.end()) {
          auto startLoc = immutableExpr->getStartLoc();
          auto *property = *foundProperty;
          auto selfTy = typeContext->getSelfTypeInContext();

          // If we found an instance property, suggest inserting "self.",
          // otherwise suggest "Type." for a static property.
          std::string fixItText;
          if (property->isInstanceMember()) {
            fixItText = "self.";
          } else {
            fixItText = selfTy->getString() + ".";
          }
          emitDiagnosticAt(startLoc, diag::masked_mutable_property, fixItText,
                           property->getDescriptiveKind(), selfTy)
              .fixItInsert(startLoc, fixItText);
        }
      }

      // If this is a simple variable marked with a 'let', emit a note to fixit
      // hint it to 'var'.
      VD->emitLetToVarNoteIfSimple(DC);
      return true;
    }

    // If the underlying expression was a read-only subscript, diagnose that.
    if (auto *SD = dyn_cast_or_null<SubscriptDecl>(choice->getDecl())) {
      StringRef message;
      if (!SD->supportsMutation())
        message = "subscript is get-only";
      else if (!SD->isSetterAccessibleFrom(DC))
        message = "subscript setter is inaccessible";
      else
        message = "subscript is immutable";

      emitDiagnosticAt(Loc, DeclDiagnostic, message)
          .highlight(immutableExpr->getSourceRange());
      return true;
    }

    // If we're trying to set an unapplied method, say that.
    if (auto *VD = choice->getDecl()) {
      std::string message = "'";
      message += VD->getBaseIdentifier().str();
      message += "'";

      auto diagID = DeclDiagnostic;
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(VD)) {
        if (AFD->hasImplicitSelfDecl()) {
          message += " is a method";
          diagID = diag::assignment_lhs_is_immutable_variable;
        } else {
          message += " is a function";
        }
      } else
        message += " is not settable";

      emitDiagnosticAt(Loc, diagID, message)
          .highlight(immutableExpr->getSourceRange());
      return true;
    }
  }

  // Fall back to producing diagnostics based on the expression since we
  // couldn't determine anything from the OverloadChoice.

  // If a keypath was the problem but wasn't resolved into a vardecl
  // it is ambiguous or unable to be used for setting.
  if (auto *KPE = dyn_cast_or_null<KeyPathExpr>(immutableExpr)) {
    emitDiagnosticAt(Loc, DeclDiagnostic, "immutable key path")
        .highlight(KPE->getSourceRange());
    return true;
  }

  if (auto LE = dyn_cast<LiteralExpr>(immutableExpr)) {
    emitDiagnosticAt(Loc, DeclDiagnostic, "literals are not mutable")
        .highlight(LE->getSourceRange());
    return true;
  }

  // If the expression is the result of a call, it is an rvalue, not a mutable
  // lvalue.
  if (auto *AE = dyn_cast<ApplyExpr>(immutableExpr)) {
    std::string name = "call";
    if (isa<PrefixUnaryExpr>(AE) || isa<PostfixUnaryExpr>(AE))
      name = "unary operator";
    else if (isa<BinaryExpr>(AE))
      name = "binary operator";
    else if (isa<CallExpr>(AE))
      name = "function call";
    else if (isa<DotSyntaxCallExpr>(AE) || isa<DotSyntaxBaseIgnoredExpr>(AE))
      name = "method call";

    if (auto *DRE = dyn_cast<DeclRefExpr>(AE->getFn()->getValueProvidingExpr()))
      name = std::string("'") +
             DRE->getDecl()->getBaseIdentifier().str().str() + "'";

    emitDiagnosticAt(Loc, DeclDiagnostic, name + " returns immutable value")
        .highlight(AE->getSourceRange());
    return true;
  }

  if (auto contextualType = getContextualType(immutableExpr)) {
    Type neededType = contextualType->getInOutObjectType();
    Type actualType = getType(immutableExpr)->getInOutObjectType();
    if (!neededType->isEqual(actualType)) {
      if (DeclDiagnostic.ID != diag::cannot_pass_rvalue_inout_subelement.ID) {
        emitDiagnosticAt(Loc, DeclDiagnostic,
                         "implicit conversion from '" +
                             actualType->getString() + "' to '" +
                             neededType->getString() + "' requires a temporary")
            .highlight(immutableExpr->getSourceRange());
      }
      return true;
    }
  }

  if (auto IE = dyn_cast<TernaryExpr>(immutableExpr)) {
    emitDiagnosticAt(Loc, DeclDiagnostic,
                     "result of conditional operator '? :' is never mutable")
        .highlight(IE->getQuestionLoc())
        .highlight(IE->getColonLoc());
    return true;
  }

  emitDiagnosticAt(Loc, TypeDiagnostic, getType(DestExpr))
      .highlight(immutableExpr->getSourceRange());
  return true;
}

std::pair<Expr *, Optional<OverloadChoice>>
AssignmentFailure::resolveImmutableBase(Expr *expr) const {
  auto *DC = getDC();
  expr = expr->getValueProvidingExpr();

  auto isImmutable = [&DC](ValueDecl *decl) {
    if (auto *storage = dyn_cast<AbstractStorageDecl>(decl))
      return !storage->isSettable(nullptr) ||
             !storage->isSetterAccessibleFrom(DC);

    // If this is not something which could possibly be mutable,
    // then it's immutable.
    return true;
  };

  // Provide specific diagnostics for assignment to subscripts whose base expr
  // is known to be an rvalue.
  if (auto *SE = dyn_cast<SubscriptExpr>(expr)) {
    // If we found a decl for the subscript, check to see if it is a set-only
    // subscript decl.
    if (SE->hasDecl()) {
      const auto &declRef = SE->getDecl();
      if (auto *subscript =
              dyn_cast_or_null<SubscriptDecl>(declRef.getDecl())) {
        if (isImmutable(subscript))
          return {expr, OverloadChoice(getType(SE->getBase()), subscript,
                                       FunctionRefKind::DoubleApply)};
      }
    }

    Optional<OverloadChoice> member = getMemberRef(
        getConstraintLocator(SE, ConstraintLocator::SubscriptMember));

    // If it isn't settable, return it.
    if (member) {
      if (member->isDecl() && isImmutable(member->getDecl()))
        return {expr, member};

      // We still have a choice, the choice is not a decl
      if (!member->isDecl()) {
        // This must be a keypath application
        assert(member->getKind() == OverloadChoiceKind::KeyPathApplication);

        auto *unaryArg = SE->getArgs()->getUnaryExpr();
        assert(unaryArg);
        auto indexType = getType(unaryArg);

        // In Swift versions lower than 5, this check will fail as read only
        // key paths can masquerade as writable for compatibility reasons.
        // This is fine as in this case we just fall back on old diagnostics.
        if (indexType->isKeyPath() || indexType->isPartialKeyPath()) {
          return {expr, member};
        }
      }
    }

    // If it is settable, then the base must be the problem, recurse.
    return resolveImmutableBase(SE->getBase());
  }

  // Look through property references.
  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(expr)) {
    // If we found a decl for the UDE, check it.
    auto loc = getConstraintLocator(UDE, ConstraintLocator::Member);

    auto member = getMemberRef(loc);

    // If we can resolve a member, we can determine whether it is settable in
    // this context.
    if (member && member->isDecl() && isImmutable(member->getDecl()))
      return {expr, member};

    // If we weren't able to resolve a member or if it is mutable, then the
    // problem must be with the base, recurse.
    return resolveImmutableBase(UDE->getBase());
  }

  if (auto *MRE = dyn_cast<MemberRefExpr>(expr)) {
    // If the member isn't settable, then it is the problem: return it.
    if (auto member = dyn_cast<AbstractStorageDecl>(MRE->getMember().getDecl()))
      if (isImmutable(member))
        return {expr, OverloadChoice(getType(MRE->getBase()), member,
                                     FunctionRefKind::SingleApply)};

    // If we weren't able to resolve a member or if it is mutable, then the
    // problem must be with the base, recurse.
    return resolveImmutableBase(MRE->getBase());
  }

  if (auto *UME = dyn_cast<UnresolvedMemberExpr>(expr)) {
    auto loc = getConstraintLocator(UME, ConstraintLocator::UnresolvedMember);
    auto member = getMemberRef(loc);

    // If we can resolve a member, we can determine whether it is settable in
    // this context.
    if (member && member->isDecl() && isImmutable(member->getDecl()))
      return {expr, member};
    else
      return {expr, None};
  }

  if (auto *DRE = dyn_cast<DeclRefExpr>(expr))
    return {expr,
            OverloadChoice(Type(), DRE->getDecl(), FunctionRefKind::Unapplied)};

  // Look through x!
  if (auto *FVE = dyn_cast<ForceValueExpr>(expr))
    return resolveImmutableBase(FVE->getSubExpr());

  // Look through x?
  if (auto *BOE = dyn_cast<BindOptionalExpr>(expr))
    return resolveImmutableBase(BOE->getSubExpr());

  // Look through implicit conversions
  if (auto *ICE = dyn_cast<ImplicitConversionExpr>(expr))
    if (!isa<LoadExpr>(ICE->getSubExpr()))
      return resolveImmutableBase(ICE->getSubExpr());

  if (auto *SAE = dyn_cast<SelfApplyExpr>(expr))
    return resolveImmutableBase(SAE->getFn());

  return {expr, None};
}

Optional<OverloadChoice>
AssignmentFailure::getMemberRef(ConstraintLocator *locator) const {
  auto member = getOverloadChoiceIfAvailable(locator);
  if (!member)
    return None;

  if (!member->choice.isDecl())
    return member->choice;

  auto *decl = member->choice.getDecl();
  if (isa<SubscriptDecl>(decl) &&
      isValidDynamicMemberLookupSubscript(cast<SubscriptDecl>(decl),
                                          getParentModule())) {
    auto *subscript = cast<SubscriptDecl>(decl);
    // If this is a keypath dynamic member lookup, we have to
    // adjust the locator to find member referred by it.
    if (isValidKeyPathDynamicMemberLookup(subscript)) {
      // Type has a following format:
      // `(Self) -> (dynamicMember: {Writable}KeyPath<T, U>) -> U`
      auto *fullType = member->adjustedOpenedFullType->castTo<FunctionType>();
      auto *fnType = fullType->getResult()->castTo<FunctionType>();

      auto paramTy = fnType->getParams()[0].getPlainType();
      auto keyPath = paramTy->getAnyNominal();
      auto memberLoc = getConstraintLocator(
          locator, LocatorPathElt::KeyPathDynamicMember(keyPath));

      auto memberRef = getOverloadChoiceIfAvailable(memberLoc);
      return memberRef ? Optional<OverloadChoice>(memberRef->choice) : None;
    }

    // If this is a string based dynamic lookup, there is no member declaration.
    return None;
  }

  return member->choice;
}

Diag<StringRef> AssignmentFailure::findDeclDiagnostic(ASTContext &ctx,
                                                      const Expr *destExpr) {
  if (isa<ApplyExpr>(destExpr) || isa<SelfApplyExpr>(destExpr))
    return diag::assignment_lhs_is_apply_expression;

  if (isa<UnresolvedDotExpr>(destExpr) || isa<MemberRefExpr>(destExpr))
    return diag::assignment_lhs_is_immutable_property;

  if (auto *subscript = dyn_cast<SubscriptExpr>(destExpr)) {
    auto diagID = diag::assignment_subscript_has_immutable_base;
    // If the destination is a subscript with a 'dynamicLookup:' label and if
    // the subscript is implicit, then this was actually a @dynamicMemberLookup
    // access. Emit a more specific diagnostic.
    auto *args = subscript->getArgs();
    if (subscript->isImplicit() && args->isUnary() &&
        args->getLabel(0) == ctx.Id_dynamicMember)
      diagID = diag::assignment_dynamic_property_has_immutable_base;

    return diagID;
  }

  return diag::assignment_lhs_is_immutable_variable;
}

SourceLoc ContextualFailure::getLoc() const {
  auto *locator = getLocator();

  // `getSingleExpressionBody` can point to an implicit expression
  // without source information in cases like `{ return }`.
  if (locator->isLastElement<LocatorPathElt::ClosureBody>()) {
    auto *closure = castToExpr<ClosureExpr>(locator->getAnchor());
    if (closure->hasSingleExpressionBody()) {
      auto *body = closure->getSingleExpressionBody();
      if (auto loc = body->getLoc())
        return loc;
    }
    return closure->getLoc();
  }

  return FailureDiagnostic::getLoc();
}

bool ContextualFailure::diagnoseAsError() {
  auto anchor = getAnchor();
  auto path = getLocator()->getPath();

  if (CTP == CTP_ReturnSingleExpr || CTP == CTP_ReturnStmt) {
    // Special case the "conversion to void".
    if (getToType()->isVoid()) {
      emitDiagnostic(diag::cannot_return_value_from_void_func)
          .highlight(getSourceRange());
      return true;
    }
  }

  if (diagnoseConversionToNil())
    return true;

  if (path.empty()) {
    if (auto *KPE = getAsExpr<KeyPathExpr>(anchor)) {
      emitDiagnosticAt(KPE->getLoc(),
                       diag::expr_smart_keypath_value_covert_to_contextual_type,
                       getFromType(), getToType());
      return true;
    }

    if (diagnoseCoercionToUnrelatedType())
      return true;

    if (isExpr<OptionalTryExpr>(anchor)) {
      emitDiagnostic(diag::cannot_convert_initializer_value, getFromType(),
                     getToType());
      return true;
    }

    if (isExpr<AssignExpr>(anchor)) {
      auto diagnostic = emitDiagnostic(diag::cannot_convert_assign,
                                       getFromType(), getToType());
      tryIntegerCastFixIts(diagnostic);
      return true;
    }

    return false;
  }

  // Special case of some common conversions involving Swift.String
  // indexes, catching cases where people attempt to index them with an integer.
  if (isIntegerToStringIndexConversion()) {
    emitDiagnostic(diag::string_index_not_integer, getFromType())
        .highlight(getSourceRange());
    emitDiagnostic(diag::string_index_not_integer_note);
    return true;
  }

  auto fromType = getFromType();
  auto toType = getToType();

  Diag<Type, Type> diagnostic;

  const auto lastPathEltKind = path.back().getKind();
  switch (lastPathEltKind) {
  case ConstraintLocator::ClosureBody:
  case ConstraintLocator::ClosureResult: {
    auto *closure = castToExpr<ClosureExpr>(getRawAnchor());
    if (closure->hasExplicitResultType() &&
        closure->getExplicitResultTypeRepr()) {
      auto resultRepr = closure->getExplicitResultTypeRepr();

      if (lastPathEltKind == ConstraintLocator::ClosureBody) {
        // The conflict is between the return type and the declared result type.
        emitDiagnosticAt(resultRepr->getStartLoc(),
                         diag::incorrect_explicit_closure_result_vs_return_type,
                         toType, fromType)
            .fixItReplace(resultRepr->getSourceRange(), fromType.getString());
      } else {
        // The conflict is between the declared result type and the
        // contextual type.
        emitDiagnosticAt(
            resultRepr->getStartLoc(),
            diag::incorrect_explicit_closure_result_vs_contextual_type,
            fromType, toType)
            .fixItReplace(resultRepr->getSourceRange(), toType.getString());
      }

      return true;
    }

    diagnostic = diag::cannot_convert_closure_result;
    break;
  }

  case ConstraintLocator::Condition: {
    // Tailored diagnostics for optional or assignment use
    // in condition expression.
    if (diagnoseConversionToBool())
      return true;

    diagnostic = diag::cannot_convert_condition_value;
    break;
  }
  case ConstraintLocator::CoercionOperand:
  case ConstraintLocator::InstanceType: {
    if (diagnoseCoercionToUnrelatedType())
      return true;
    break;
  }

  case ConstraintLocator::TernaryBranch: {
    auto *ternaryExpr = castToExpr<TernaryExpr>(getRawAnchor());
    fromType = getType(ternaryExpr->getThenExpr());
    toType = getType(ternaryExpr->getElseExpr());
    diagnostic = diag::ternary_expr_cases_mismatch;
    break;
  }
  case ConstraintLocator::SingleValueStmtBranch: {
    diagnostic = diag::single_value_stmt_branches_mismatch;
    break;
  }
  case ConstraintLocator::ContextualType: {
    if (diagnoseConversionToBool())
      return true;

    if (diagnoseThrowsTypeMismatch())
      return true;

    if (diagnoseYieldByReferenceMismatch())
      return true;

    if (isExpr<OptionalTryExpr>(anchor) ||
        isExpr<OptionalEvaluationExpr>(anchor)) {
      auto objectType = fromType->getOptionalObjectType();
      // Cannot assume that `fromType` is always optional here since
      // it could assume a type from context.
      if (objectType && objectType->isEqual(toType)) {
        MissingOptionalUnwrapFailure failure(getSolution(), getType(anchor),
                                             toType,
                                             getConstraintLocator(anchor));
        if (failure.diagnoseAsError())
          return true;
      }
    }

    if (CTP == CTP_ForEachStmt || CTP == CTP_ForEachSequence) {
      if (fromType->isAnyExistentialType()) {
        Type constraintType = fromType;
        if (auto existential = constraintType->getAs<ExistentialType>())
          constraintType = existential->getConstraintType();

        emitDiagnostic(diag::type_cannot_conform, fromType, toType);
        emitDiagnostic(diag::only_concrete_types_conform_to_protocols);
        return true;
      }

      emitDiagnostic(
          diag::foreach_sequence_does_not_conform_to_expected_protocol,
          fromType, toType, bool(fromType->getOptionalObjectType()))
          .highlight(getSourceRange());
      return true;
    }

    if (auto *call = getAsExpr<CallExpr>(anchor)) {
      if (isa<ClosureExpr>(call->getFn()))
        CTP = CTP_ClosureResult;
    }

    if (auto msg = getDiagnosticFor(CTP, toType)) {
      diagnostic = *msg;
      break;
    }
    return false;
  }
  case ConstraintLocator::UnresolvedMemberChainResult: {
    auto &solution = getSolution();

    auto overload =
        getCalleeOverloadChoiceIfAvailable(getConstraintLocator(anchor));
    if (!(overload && overload->choice.isDecl()))
      return false;

    auto *choice = overload->choice.getDecl();
    auto fnType = fromType->getAs<FunctionType>();
    if (!fnType) {
      emitDiagnostic(diag::expected_result_in_contextual_member,
                     choice->getName(), fromType, toType);
      return true;
    }

    // If member type is a function and contextual type matches
    // its result type, most likely problem is related to a
    // missing call e.g.:
    //
    // struct S {
    //   static func foo() -> S {}
    // }
    //
    // let _: S = .foo

    auto params = fnType->getParams();

    ParameterListInfo info(
        params, choice,
        hasAppliedSelf(overload->choice, [&solution](Type type) {
          return solution.simplifyType(type);
        }));
    auto numMissingArgs = llvm::count_if(
        indices(params), [&info](const unsigned paramIdx) -> bool {
          return !info.hasDefaultArgument(paramIdx);
        });

    if (numMissingArgs == 0 || numMissingArgs > 1) {
      auto applyFixIt = [&](InFlightDiagnostic &diagnostic) {
        // If there are no parameters we can suggest a fix-it
        // to form an explicit call.
        if (numMissingArgs == 0)
          diagnostic.fixItInsertAfter(getSourceRange().End, "()");
      };
      if (fnType->getResult()->isEqual(toType)) {
        auto diag = emitDiagnostic(
                      diag::expected_parens_in_contextual_member_type,
                      choice->getName(), fnType->getResult());
        applyFixIt(diag);
      } else {
        auto diag = emitDiagnostic(diag::expected_parens_in_contextual_member,
                                   choice->getName());
        applyFixIt(diag);
      }
    } else {
      emitDiagnostic(diag::expected_argument_in_contextual_member,
                     choice->getName(), params.front().getPlainType());
    }

    return true;
  }

  case ConstraintLocator::ResultBuilderBodyResult: {
    diagnostic = *getDiagnosticFor(CTP_Initialization, toType);
    break;
  }

  case ConstraintLocator::OptionalPayload: {
    // If this is an attempt at a Double <-> CGFloat conversion
    // through optional chaining, let's produce a tailored diagnostic.
    if (isExpr<OptionalEvaluationExpr>(getAnchor())) {
      if ((fromType->isDouble() || fromType->isCGFloat()) &&
          (toType->isDouble() || toType->isCGFloat())) {
        fromType = OptionalType::get(fromType);
        toType = OptionalType::get(toType);
        diagnostic = diag::cannot_implicitly_convert_in_optional_context;
        break;
      }
    }

    return false;
  }

  case ConstraintLocator::EnumPatternImplicitCastMatch: {
    // In this case, the types are reversed, as we are checking whether we
    // can convert the pattern type to the context type.
    std::swap(fromType, toType);
    diagnostic = diag::cannot_match_value_with_pattern;
    break;
  }

  case ConstraintLocator::PatternMatch: {
    diagnostic = diag::cannot_match_value_with_pattern;
    break;
  }

  default:
    return false;
  }

  auto diag = emitDiagnostic(diagnostic, fromType, toType);
  diag.highlight(getSourceRange());

  (void)tryFixIts(diag);
  return true;
}

bool ContextualFailure::diagnoseAsNote() {
  auto *locator = getLocator();

  auto overload = getCalleeOverloadChoiceIfAvailable(locator);
  if (!(overload && overload->choice.isDecl()))
    return false;

  auto *decl = overload->choice.getDecl();

  if (auto *anchor = getAsExpr(getAnchor())) {
    anchor = anchor->getSemanticsProvidingExpr();

    if (isa<NilLiteralExpr>(anchor)) {
      auto argLoc =
          locator->castLastElementTo<LocatorPathElt::ApplyArgToParam>();
      emitDiagnosticAt(decl, diag::note_incompatible_argument_value_nil_at_pos,
                       getToType(), argLoc.getArgIdx() + 1);
      return true;
    }
  }

  emitDiagnosticAt(decl, diag::found_candidate_type, getFromType());
  return true;
}

static Optional<Diag<Type>>
getContextualNilDiagnostic(ContextualTypePurpose CTP) {
  switch (CTP) {
  case CTP_Unused:
  case CTP_CannotFail:
    llvm_unreachable("These contextual type purposes cannot fail with a "
                     "conversion type specified!");
  case CTP_CalleeResult:
    llvm_unreachable("CTP_CalleeResult does not actually install a "
                     "contextual type");
  case CTP_Initialization:
    return diag::cannot_convert_initializer_value_nil;

  case CTP_ReturnSingleExpr:
  case CTP_ReturnStmt:
    return diag::cannot_convert_to_return_type_nil;

  case CTP_CaseStmt:
  case CTP_ThrowStmt:
  case CTP_DiscardStmt:
  case CTP_ForEachStmt:
  case CTP_ForEachSequence:
  case CTP_YieldByReference:
  case CTP_WrappedProperty:
  case CTP_ComposedPropertyWrapper:
  case CTP_ExprPattern:
  case CTP_SingleValueStmtBranch:
    return None;

  case CTP_EnumCaseRawValue:
    return diag::cannot_convert_raw_initializer_value_nil;
  case CTP_DefaultParameter:
  case CTP_AutoclosureDefaultParameter:
    return diag::cannot_convert_default_arg_value_nil;
  case CTP_YieldByValue:
    return diag::cannot_convert_yield_value_nil;
  case CTP_CallArgument:
    return diag::cannot_convert_argument_value_nil;
  case CTP_ClosureResult:
    return diag::cannot_convert_closure_result_nil;
  case CTP_ArrayElement:
    return diag::cannot_convert_array_element_nil;
  case CTP_DictionaryKey:
    return diag::cannot_convert_dict_key_nil;
  case CTP_DictionaryValue:
    return diag::cannot_convert_dict_value_nil;
  case CTP_CoerceOperand:
    return diag::cannot_convert_coerce_nil;
  case CTP_AssignSource:
    return diag::cannot_convert_assign_nil;
  case CTP_SubscriptAssignSource:
    return diag::cannot_convert_subscript_assign_nil;
  case CTP_Condition:
    return diag::cannot_convert_condition_value_nil;
  }
  llvm_unreachable("Unhandled ContextualTypePurpose in switch");
}

bool ContextualFailure::diagnoseConversionToNil() const {
  auto anchor = getAnchor();

  if (!isExpr<NilLiteralExpr>(anchor))
    return false;

  auto *locator = getLocator();

  Optional<ContextualTypePurpose> CTP;
  // Easy case were failure has been identified as contextual already.
  if (auto contextualTy =
          locator->getLastElementAs<LocatorPathElt::ContextualType>()) {
    CTP = contextualTy->getPurpose();
  } else {
    // Here we need to figure out where `nil` is located.
    // It could be e.g. an argument to a subscript/call, assignment
    // source like `s[0] = nil` or an array element like `[nil]` or
    // `[nil: 42]` as a sub-expression to a larger one.
    auto *parentExpr = findParentExpr(getAsExpr(anchor));

    // Looks like it's something similar to `let _ = nil`.
    if (!parentExpr) {
      emitDiagnostic(diag::unresolved_nil_literal);
      return true;
    }

    // Two choices here - whether it's a regular assignment
    // e.g. `let _: S = nil` or a subscript one e.g. `s[0] = nil`.
    if (auto *AE = dyn_cast<AssignExpr>(parentExpr)) {
      CTP = isa<SubscriptExpr>(AE->getDest()) ? CTP_SubscriptAssignSource
                                              : CTP_AssignSource;
    } else if (isa<ArrayExpr>(parentExpr)) {
      CTP = CTP_ArrayElement;
    } else if (isa<ClosureExpr>(parentExpr)) {
      CTP = CTP_ClosureResult;
    } else if (isa<ParenExpr>(parentExpr) || isa<TupleExpr>(parentExpr)) {
      auto *enclosingExpr = findParentExpr(parentExpr);

      if (!enclosingExpr) {
        // If there is no enclosing expression it's something like
        // `(nil)` or `(a: nil)` which can't be inferred without a
        // contextual type.
        emitDiagnostic(diag::unresolved_nil_literal);
        return true;
      }

      if (auto *TE = dyn_cast<TupleExpr>(parentExpr)) {
        // In case of dictionary e.g. `[42: nil]` we need to figure
        // out whether nil is a "key" or a "value".
        if (isa<DictionaryExpr>(enclosingExpr)) {
          assert(TE->getNumElements() == 2);
          CTP = TE->getElement(0) == castToExpr(anchor) ? CTP_DictionaryKey
                                                        : CTP_DictionaryValue;
        } else {
          // Can't initialize one of the tuple elements with `nil`.
          CTP = CTP_Initialization;
        }
      }
    } else if (isa<KeyPathExpr>(parentExpr)) {
      // This is something like `\S.[x: nil]`.
      CTP = CTP_CallArgument;
    } else if (auto *args = parentExpr->getArgs()) {
      // Check if `nil` is passed as an argument to a parameter which doesn't
      // expect it e.g. `foo(a: nil)` or `s[x: nil]`.
      if (args->findArgumentExpr(castToExpr(anchor))) {
        CTP = CTP_CallArgument;
      } else {
        // If the 'nil' isn't an argument, it'll be in the fn e.g `nil(5)`,
        // which can't be inferred without a contextual type.
        emitDiagnostic(diag::unresolved_nil_literal);
        return true;
      }
    } else if (locator->isForCoercion()) {
      // `nil` is passed as a left-hand side of the coercion
      // operator e.g. `nil as Foo`
      CTP = CTP_CoerceOperand;
    } else {
      // Otherwise let's produce a generic `nil` conversion diagnostic.
      emitDiagnostic(diag::cannot_use_nil_with_this_type, getToType());
      return true;
    }
  }

  if (!CTP)
    return false;

  if (CTP == CTP_ThrowStmt) {
    emitDiagnostic(diag::cannot_throw_nil);
    return true;
  }

  auto diagnostic = getContextualNilDiagnostic(*CTP);
  if (!diagnostic)
    return false;

  emitDiagnostic(*diagnostic, getToType());

  if (CTP == CTP_Initialization) {
    auto *patternTR = getContextualTypeLoc(getRawAnchor()).getTypeRepr();
    if (!patternTR)
      return true;

    auto diag = emitDiagnosticAt(patternTR->getLoc(), diag::note_make_optional,
                                 OptionalType::get(getToType()));
    if (patternTR->isSimple()) {
      diag.fixItInsertAfter(patternTR->getEndLoc(), "?");
    } else {
      diag.fixItInsert(patternTR->getStartLoc(), "(");
      diag.fixItInsertAfter(patternTR->getEndLoc(), ")?");
    }
  }

  return true;
}

void ContextualFailure::tryFixIts(InFlightDiagnostic &diagnostic) const {
  auto *locator = getLocator();
  // Can't apply any of the fix-its below if this failure
  // is related to `inout` argument, or a pattern mismatch.
  if (locator->isLastElement<LocatorPathElt::LValueConversion>() ||
      locator->isForPatternMatch()) {
    return;
  }

  if (trySequenceSubsequenceFixIts(diagnostic))
    return;

  if (tryIntegerCastFixIts(diagnostic))
    return;

  if (tryProtocolConformanceFixIt(diagnostic))
    return;

  if (tryTypeCoercionFixIt(diagnostic))
    return;
}

bool ContextualFailure::diagnoseCoercionToUnrelatedType() const {
  auto anchor = getRawAnchor();
  auto *coerceExpr = getAsExpr<CoerceExpr>(anchor);
  if (!coerceExpr) {
    return false;
  }

  const auto fromType = getType(coerceExpr->getSubExpr());
  const auto toType = getType(coerceExpr->getCastTypeRepr());

  auto diagnostic = getDiagnosticFor(CTP_CoerceOperand, toType);

  auto diag = emitDiagnostic(*diagnostic, fromType, toType);
  diag.highlight(getSourceRange());

  (void)tryFixIts(diag);

  return true;
}

bool ContextualFailure::diagnoseConversionToBool() const {
  auto toType = getToType();
  if (!toType->isBool())
    return false;

  auto *anchor = castToExpr(getAnchor());
  // Check for "=" converting to Bool.  The user probably meant ==.
  if (auto *AE = dyn_cast<AssignExpr>(anchor->getValueProvidingExpr())) {
    emitDiagnosticAt(AE->getEqualLoc(), diag::use_of_equal_instead_of_equality)
        .fixItReplace(AE->getEqualLoc(), "==")
        .highlight(AE->getDest()->getLoc())
        .highlight(AE->getSrc()->getLoc());
    return true;
  }

  // Determine if the boolean negation operator was applied to the anchor. This
  // upwards traversal of the AST is somewhat fragile, but enables much better
  // diagnostics if someone attempts to use an optional or integer as a boolean
  // condition.
  SourceLoc notOperatorLoc;
  if (auto *parent = findParentExpr(anchor)) {
    if (auto *parentOpCall = dyn_cast<PrefixUnaryExpr>(parent)) {
      auto &ctx = getASTContext();
      auto opRef = dyn_cast<DeclRefExpr>(parentOpCall->getFn());
      if (opRef && opRef->getDecl()->getBaseName() == ctx.Id_NegationOperator)
        notOperatorLoc = opRef->getLoc();
    }
  }

  // If we're trying to convert something from optional type to Bool, then a
  // comparison against nil was probably expected.
  auto fromType = getFromType();
  if (fromType->getOptionalObjectType()) {
    if (auto *OE = getAsExpr<OptionalEvaluationExpr>(
            anchor->getValueProvidingExpr())) {
      auto *subExpr = OE->getSubExpr();
      while (auto *BOE = getAsExpr<BindOptionalExpr>(subExpr)) {
        subExpr = BOE->getSubExpr();
      }
      // The contextual mismatch is anchored in an optional evaluation
      // expression wrapping a literal expression e.g. `0?` suggesting to use
      // `!= nil` will not be accurate in this case, so let's fallback to
      // default mismatch diagnostic.
      if (isa<LiteralExpr>(subExpr)) {
        return false;
      }
    }

    StringRef prefix = "((";
    StringRef suffix;
    if (notOperatorLoc.isValid())
      suffix = ") == nil)";
    else
      suffix = ") != nil)";

    // Check if we need the inner parentheses.
    // Technically we only need them if there's something in 'expr' with
    // lower precedence than '!=', but the code actually comes out nicer
    // in most cases with parens on anything that is non-trivial.
    if (anchor->canAppendPostfixExpression()) {
      prefix = prefix.drop_back();
      suffix = suffix.drop_front();
    }
    // FIXME: The outer parentheses may be superfluous too.

    emitDiagnostic(diag::optional_used_as_boolean, fromType,
                   notOperatorLoc.isValid())
        .fixItInsert(getSourceRange().Start, prefix)
        .fixItInsertAfter(getSourceRange().End, suffix)
        .fixItRemove(notOperatorLoc);
    return true;
  }

  // If we're trying to convert something from optional type to an integer, then
  // a comparison against nil was probably expected.
  if (conformsToKnownProtocol(fromType, KnownProtocolKind::BinaryInteger) &&
      conformsToKnownProtocol(fromType,
                              KnownProtocolKind::ExpressibleByIntegerLiteral)) {
    if (auto *IL =
            getAsExpr<IntegerLiteralExpr>(anchor->getValueProvidingExpr())) {
      // If integer literal value is either zero or one, let's suggest replacing
      // with boolean literal `true` or `false`. Otherwise fallback to generic
      // type mismatch diagnostic.
      const auto value = IL->getRawValue();
      if (value.isOne() || value.isZero()) {
        StringRef boolLiteral = value.isZero() ? "false" : "true";
        emitDiagnostic(diag::integer_used_as_boolean_literal,
                       IL->getDigitsText(), value.isOne())
            .fixItReplace(IL->getSourceRange(), boolLiteral);
        return true;
      }
      return false;
    }

    StringRef prefix = "((";
    StringRef suffix;
    if (notOperatorLoc.isValid())
      suffix = ") == 0)";
    else
      suffix = ") != 0)";

    // Check if we need the inner parentheses.
    // Technically we only need them if there's something in 'expr' with
    // lower precedence than '!=', but the code actually comes out nicer
    // in most cases with parens on anything non-trivial.
    if (anchor->canAppendPostfixExpression()) {
      prefix = prefix.drop_back();
      suffix = suffix.drop_front();
    }
    // FIXME: The outer parentheses may be superfluous too.

    emitDiagnostic(diag::integer_used_as_boolean, fromType,
                   notOperatorLoc.isValid())
        .fixItInsert(getSourceRange().Start, prefix)
        .fixItInsertAfter(getSourceRange().End, suffix)
        .fixItRemove(notOperatorLoc);
    return true;
  }

  return false;
}

bool ContextualFailure::diagnoseThrowsTypeMismatch() const {
  // If this is conversion failure due to a return statement with an argument
  // that cannot be coerced to the result type of the function, emit a
  // specific error.
  if (CTP != CTP_ThrowStmt)
    return false;

  auto anchor = getAnchor();

  // If we tried to throw the error code of an error type, suggest object
  // construction.
  auto &Ctx = getASTContext();
  if (auto errorCodeProtocol =
          Ctx.getProtocol(KnownProtocolKind::ErrorCodeProtocol)) {
    Type errorCodeType = getFromType();
    auto conformance = TypeChecker::conformsToProtocol(
        errorCodeType, errorCodeProtocol, getParentModule());
    if (conformance) {
      Type errorType =
          conformance
              .getTypeWitnessByName(errorCodeType, getASTContext().Id_ErrorType)
              ->getCanonicalType();
      if (errorType) {
        auto diagnostic = emitDiagnostic(diag::cannot_throw_error_code,
                                         errorCodeType, errorType);
        if (auto *UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
          diagnostic.fixItInsert(UDE->getDotLoc(), "(");
          diagnostic.fixItInsertAfter(UDE->getEndLoc(), ")");
        }
        return true;
      }
    }
  }

  // The conversion destination of throw is always ErrorType (at the moment)
  // if this ever expands, this should be a specific form like () is for
  // return.
  emitDiagnostic(diag::cannot_convert_thrown_type, getFromType())
      .highlight(getSourceRange());
  return true;
}

bool ContextualFailure::diagnoseYieldByReferenceMismatch() const {
  if (CTP != CTP_YieldByReference)
    return false;

  auto anchor = getAnchor();
  auto exprType = getType(anchor, /*wantRValue=*/false);
  auto contextualType = getToType();

  if (auto exprLV = exprType->getAs<LValueType>()) {
    emitDiagnostic(diag::cannot_yield_wrong_type_by_reference,
                   exprLV->getObjectType(), contextualType);
  } else if (exprType->isEqual(contextualType)) {
    emitDiagnostic(diag::cannot_yield_rvalue_by_reference_same_type, exprType);
  } else {
    emitDiagnostic(diag::cannot_yield_rvalue_by_reference, exprType,
                   contextualType);
  }
  return true;
}

bool ContextualFailure::tryIntegerCastFixIts(
    InFlightDiagnostic &diagnostic) const {
  auto fromType = getFromType();
  auto toType = getToType();

  auto anchor = getAnchor();
  auto exprRange = getSourceRange();

  if (auto *assignment = getAsExpr<AssignExpr>(anchor)) {
    toType = toType->lookThroughAllOptionalTypes();
    anchor = assignment->getSrc();
    exprRange = assignment->getSrc()->getSourceRange();
  }

  if (!isIntegerType(fromType) || !isIntegerType(toType))
    return false;

  auto getInnerCastedExpr = [&](const Expr *expr) -> Expr * {
    if (auto *CE = dyn_cast<CoerceExpr>(expr))
      return CE->getSubExpr();

    auto *CE = dyn_cast<CallExpr>(expr);
    if (!CE)
      return nullptr;
    if (!isa<ConstructorRefCallExpr>(CE->getFn()))
      return nullptr;
    return CE->getArgs()->getUnlabeledUnaryExpr();
  };

  if (auto *expr = getAsExpr(anchor)) {
    if (Expr *innerE = getInnerCastedExpr(expr)) {
      Type innerTy = getType(innerE);
      if (TypeChecker::isConvertibleTo(innerTy, toType, getDC())) {
        // Remove the unnecessary cast.
        diagnostic.fixItRemoveChars(getLoc(), innerE->getStartLoc())
            .fixItRemove(getSourceRange().End);
        return true;
      }
    }
  }

  // bridge to prevent roundabout error message
  // See rdar://problem/82828226
  if (TypeChecker::isObjCBridgedTo(fromType, toType, getDC())) {
    auto *ac = castToExpr(getAnchor());
    bool needsParensInside = exprNeedsParensBeforeAddingAs(ac, getDC());
    bool needsParensOutside = exprNeedsParensAfterAddingAs(ac, getDC());
    llvm::SmallString<2> insertBefore;
    llvm::SmallString<32> insertAfter;
    if (needsParensOutside) {
      insertBefore += "(";
    }
    if (needsParensInside) {
      insertBefore += "(";
      insertAfter += ")";
    }
    insertAfter += " as ";
    insertAfter += toType->getWithoutParens()->getString();
    if (needsParensOutside)
      insertAfter += ")";
    diagnostic.fixItInsert(exprRange.Start, insertBefore);
    diagnostic.fixItInsertAfter(exprRange.End, insertAfter);
    return true;
  }

  // Add a wrapping integer cast.
  std::string convWrapBefore = toType.getString();
  convWrapBefore += "(";
  std::string convWrapAfter = ")";
  diagnostic.fixItInsert(exprRange.Start, convWrapBefore);
  diagnostic.fixItInsertAfter(exprRange.End, convWrapAfter);
  return true;
}

bool ContextualFailure::trySequenceSubsequenceFixIts(
    InFlightDiagnostic &diagnostic) const {
  if (!getASTContext().getStdlibModule())
    return false;

  // Substring -> String conversion
  // Wrap in String.init
  if (getFromType()->isSubstring()) {
    if (getToType()->isString()) {
      auto *anchor = castToExpr(getAnchor())->getSemanticsProvidingExpr();

      if (auto *call = dyn_cast<CallExpr>(anchor)) {
        auto *fnExpr = call->getFn();
        if (auto *closure = dyn_cast<ClosureExpr>(fnExpr)) {
          if (closure->hasSingleExpressionBody())
            anchor = closure->getSingleExpressionBody();
        }
      }

      auto range = anchor->getSourceRange();
      diagnostic.fixItInsert(range.Start, "String(");
      diagnostic.fixItInsertAfter(range.End, ")");
      return true;
    }
  }

  return false;
}

bool ContextualFailure::tryTypeCoercionFixIt(
    InFlightDiagnostic &diagnostic) const {
  auto fromType = getFromType();
  auto toType = getToType();

  // Look through optional types; casts can add them, but can't remove extra
  // ones.
  bool bothOptional =
      fromType->getOptionalObjectType() && toType->getOptionalObjectType();
  if (bothOptional)
    fromType = fromType->getOptionalObjectType();
  toType = toType->lookThroughAllOptionalTypes();

  if (!toType->hasTypeRepr())
    return false;

  // If object of the optional type is a subtype of the specified contextual
  // type, let's suggest a force unwrap "!". Otherwise fallback to potential
  // coercion or force cast.
  if (!bothOptional && fromType->getOptionalObjectType()) {
    if (TypeChecker::isSubtypeOf(fromType->lookThroughAllOptionalTypes(),
                                 toType, getDC())) {
      diagnostic.fixItInsert(
          Lexer::getLocForEndOfToken(getASTContext().SourceMgr,
                                     getSourceRange().End),
          "!");
      return true;
    }
  }

  CheckedCastKind Kind = TypeChecker::typeCheckCheckedCast(
      fromType, toType, CheckedCastContextKind::None, getDC());

  if (Kind != CheckedCastKind::Unresolved) {
    bool canUseAs = Kind == CheckedCastKind::Coercion ||
                    Kind == CheckedCastKind::BridgingCoercion;
    if (bothOptional && canUseAs)
      toType = OptionalType::get(toType);
    diagnostic.fixItInsert(Lexer::getLocForEndOfToken(getASTContext().SourceMgr,
                                                      getSourceRange().End),
                           diag::insert_type_coercion, canUseAs, toType);
    return true;
  }

  return false;
}

bool ContextualFailure::tryProtocolConformanceFixIt(
    InFlightDiagnostic &diagnostic) const {
  auto innermostTyCtx = getDC()->getInnermostTypeContext();
  if (!innermostTyCtx)
    return false;

  auto nominal = innermostTyCtx->getSelfNominalTypeDecl();
  if (!nominal)
    return false;

  auto fromType = getFromType();
  // We need to get rid of optionals and parens as it's not relevant when
  // printing the diagnostic and the fix-it.
  auto unwrappedToType =
      getToType()->lookThroughAllOptionalTypes()->getWithoutParens();

  // If the protocol requires a class & we don't have one (maybe the context
  // is a struct), then bail out instead of offering a broken fix-it later on.
  auto requiresClass = false;
  ExistentialLayout layout;
  if (unwrappedToType->isExistentialType()) {
    layout = unwrappedToType->getExistentialLayout();
    requiresClass = layout.requiresClass();
  }

  if (requiresClass && !fromType->is<ClassType>()) {
    return false;
  }

  // We can only offer a fix-it if we're assigning to a protocol type and
  // the type we're assigning is the same as the innermost type context.
  bool shouldOfferFixIt = nominal->getSelfTypeInContext()->isEqual(fromType) &&
                          unwrappedToType->isExistentialType();
  if (!shouldOfferFixIt)
    return false;

  diagnostic.flush();

  // Let's build a list of protocols that the context does not conform to.
  SmallVector<std::string, 8> missingProtoTypeStrings;
  SmallVector<ProtocolDecl *, 8> missingProtocols;
  for (auto protocol : layout.getProtocols()) {
    if (!TypeChecker::conformsToProtocol(fromType, protocol,
                                         getParentModule())) {
      auto protoTy = protocol->getDeclaredInterfaceType();
      missingProtoTypeStrings.push_back(protoTy->getString());
      missingProtocols.push_back(protocol);
    }
  }

  // If we have a protocol composition type and we don't conform to all
  // the protocols of the composition, then store the composition directly.
  // This is because we need to append 'Foo & Bar' instead of 'Foo, Bar' in
  // order to match the written type.
  auto constraint = unwrappedToType;
  if (auto existential = constraint->getAs<ExistentialType>())
    constraint = existential->getConstraintType();
  if (auto compositionTy = constraint->getAs<ProtocolCompositionType>()) {
    if (compositionTy->getMembers().size() == missingProtoTypeStrings.size()) {
      missingProtoTypeStrings = {compositionTy->getString()};
    }
  }

  assert(!missingProtoTypeStrings.empty() &&
         "type already conforms to all the protocols?");

  // Combine all protocol names together, separated by commas.
  std::string protoString = llvm::join(missingProtoTypeStrings, ", ");

  // Emit a diagnostic to inform the user that they need to conform to the
  // missing protocols.
  auto conformanceDiag =
      emitDiagnostic(diag::assign_protocol_conformance_fix_it, constraint,
                     nominal->getDescriptiveKind(), fromType);
  if (nominal->getInherited().size() > 0) {
    auto lastInherited = nominal->getInherited().back().getLoc();
    auto lastInheritedEndLoc =
        Lexer::getLocForEndOfToken(getASTContext().SourceMgr, lastInherited);
    conformanceDiag.fixItInsert(lastInheritedEndLoc, ", " + protoString);
  } else {
    auto nameEndLoc = Lexer::getLocForEndOfToken(getASTContext().SourceMgr,
                                                 nominal->getNameLoc());
    conformanceDiag.fixItInsert(nameEndLoc, ": " + protoString);
  }

  // Emit fix-its to insert requirement stubs if we're in editor mode.
  if (!getASTContext().LangOpts.DiagnosticsEditorMode) {
    return true;
  }

  {
    llvm::SmallString<128> Text;
    llvm::raw_svector_ostream SS(Text);
    llvm::SetVector<MissingWitness> missingWitnesses;
    for (auto protocol : missingProtocols) {
      auto conformance = NormalProtocolConformance(
          nominal->getDeclaredType(), protocol, SourceLoc(), nominal,
          ProtocolConformanceState::Incomplete, /*isUnchecked=*/false);
      ConformanceChecker checker(getASTContext(), &conformance,
                                 missingWitnesses);
      // Type witnesses must be resolved first.
      checker.resolveTypeWitnesses();
      checker.resolveValueWitnesses();
    }

    for (auto decl : missingWitnesses) {
      swift::printRequirementStub(decl.requirement, nominal, nominal->getDeclaredType(),
                                  nominal->getStartLoc(), SS);
    }

    if (!Text.empty()) {
      conformanceDiag.fixItInsertAfter(nominal->getBraces().Start, Text.str());
    }
  }

  return true;
}

void MissingCallFailure::tryComputedPropertyFixIts() const {
  if (!isExpr<ClosureExpr>(getAnchor()))
    return;

  // It is possible that we're looking at a stored property being
  // initialized with a closure. Something like:
  //
  // var foo: Int = { return 0 }
  //
  // Let's offer another fix-it to remove the '=' to turn the stored
  // property into a computed property. If the variable is immutable, then
  // replace the 'let' with a 'var'.

  PatternBindingDecl *PBD = nullptr;

  if (auto TLCD = dyn_cast<TopLevelCodeDecl>(getDC())) {
    if (TLCD->getBody()->isImplicit()) {
      if (auto decl = TLCD->getBody()->getFirstElement().dyn_cast<Decl *>()) {
        if (auto binding = dyn_cast<PatternBindingDecl>(decl)) {
          PBD = binding;
        }
      }
    }
  } else if (auto PBI = dyn_cast<PatternBindingInitializer>(getDC())) {
    PBD = PBI->getBinding();
  }

  if (PBD) {
    if (auto VD = PBD->getSingleVar()) {
      const auto i = PBD->getPatternEntryIndexForVarDecl(VD);
      auto *initExpr = PBD->getInit(i);
      if (!VD->isStatic() &&
          !VD->getAttrs().getAttribute<DynamicReplacementAttr>() &&
          isa_and_nonnull<ClosureExpr>(initExpr)) {
        auto diag = emitDiagnostic(diag::extension_stored_property_fixit,
                                   VD->getName());
        diag.fixItRemove(PBD->getEqualLoc(i));

        if (VD->isLet()) {
          diag.fixItReplace(PBD->getStartLoc(), getTokenText(tok::kw_var));
        }

        if (auto lazyAttr = VD->getAttrs().getAttribute<LazyAttr>()) {
          diag.fixItRemove(lazyAttr->getRange());
        }
      }
    }
  }
}

bool ContextualFailure::isIntegerToStringIndexConversion() const {
  auto kind = KnownProtocolKind::ExpressibleByIntegerLiteral;

  auto fromType = getFromType();
  auto toType = getToType()->getCanonicalType();
  return (conformsToKnownProtocol(fromType, kind) &&
          toType.getString() == "String.CharacterView.Index");
}

Optional<Diag<Type, Type>>
ContextualFailure::getDiagnosticFor(ContextualTypePurpose context,
                                    Type contextualType) {
  auto forProtocol = contextualType->isConstraintType();
  switch (context) {
  case CTP_Initialization: {
    if (contextualType->isAnyObject())
      return diag::cannot_convert_initializer_value_anyobject;

    return forProtocol ? diag::cannot_convert_initializer_value_protocol
                       : diag::cannot_convert_initializer_value;
  }
  case CTP_ReturnStmt:
  case CTP_ReturnSingleExpr: {
    if (contextualType->isAnyObject())
      return diag::cannot_convert_return_type_to_anyobject;

    return forProtocol ? diag::cannot_convert_to_return_type_protocol
                       : diag::cannot_convert_to_return_type;
  }
  case CTP_EnumCaseRawValue:
    return diag::cannot_convert_raw_initializer_value;
  case CTP_DefaultParameter:
  case CTP_AutoclosureDefaultParameter:
    return forProtocol ? diag::cannot_convert_default_arg_value_protocol
                       : diag::cannot_convert_default_arg_value;
  case CTP_YieldByValue:
    return forProtocol ? diag::cannot_convert_yield_value_protocol
                       : diag::cannot_convert_yield_value;
  case CTP_CallArgument: {
    if (contextualType->isAnyObject())
      return diag::cannot_convert_argument_value_anyobject;

    return forProtocol ? diag::cannot_convert_argument_value_protocol
                       : diag::cannot_convert_argument_value;
  }
  case CTP_ClosureResult:
    return forProtocol ? diag::cannot_convert_closure_result_protocol
                       : diag::cannot_convert_closure_result;
  case CTP_ArrayElement:
    return forProtocol ? diag::cannot_convert_array_element_protocol
                       : diag::cannot_convert_array_element;
  case CTP_DictionaryKey:
    return forProtocol ? diag::cannot_convert_dict_key_protocol
                       : diag::cannot_convert_dict_key;
  case CTP_DictionaryValue:
    return forProtocol ? diag::cannot_convert_dict_value_protocol
                       : diag::cannot_convert_dict_value;
  case CTP_CoerceOperand:
    return forProtocol ? diag::cannot_convert_coerce_protocol
                       : diag::cannot_convert_coerce;
  case CTP_AssignSource: {
    if (contextualType->isAnyObject())
      return diag::cannot_convert_assign_anyobject;

    return forProtocol ? diag::cannot_convert_assign_protocol
                       : diag::cannot_convert_assign;
  }
  case CTP_SubscriptAssignSource:
    return forProtocol ? diag::cannot_convert_subscript_assign_protocol
                       : diag::cannot_convert_subscript_assign;
  case CTP_Condition:
    return diag::cannot_convert_condition_value;

  case CTP_WrappedProperty:
    return diag::wrapped_value_mismatch;

  case CTP_SingleValueStmtBranch:
    return diag::cannot_convert_initializer_value;

  case CTP_DiscardStmt:
    return diag::cannot_convert_discard_value;

  case CTP_CaseStmt:
    return diag::cannot_match_value_with_pattern;

  case CTP_ThrowStmt:
  case CTP_ForEachStmt:
  case CTP_ForEachSequence:
  case CTP_ComposedPropertyWrapper:
  case CTP_Unused:
  case CTP_CannotFail:
  case CTP_YieldByReference:
  case CTP_CalleeResult:
  case CTP_ExprPattern:
    break;
  }
  return None;
}

bool NonClassTypeToAnyObjectConversionFailure::diagnoseAsError() {
  auto locator = getLocator();
  if (locator->isForContextualType()) {
    return ContextualFailure::diagnoseAsError();
  }

  auto fromType = getFromType();
  auto toType = getToType();
  assert(fromType);
  assert(toType);
  if (locator->isLastElement<LocatorPathElt::ApplyArgToParam>()) {
    ArgumentMismatchFailure failure(getSolution(), fromType, toType, locator);
    return failure.diagnoseAsError();
  }

  Optional<Diag<Type, Type>> diagnostic;

  bool forProtocol = toType->isConstraintType();
  auto rawAnchor = getRawAnchor();

  if (isExpr<ArrayExpr>(rawAnchor)) {
    diagnostic = forProtocol ? diag::cannot_convert_array_element_protocol
                             : diag::cannot_convert_array_element;
  } else if (isExpr<DictionaryExpr>(rawAnchor)) {
    auto lastElem = locator->getLastElementAs<LocatorPathElt::TupleElement>();
    if (lastElem && lastElem->getIndex() == 0) {
      diagnostic = forProtocol ? diag::cannot_convert_dict_key_protocol
                               : diag::cannot_convert_dict_key;
    } else {
      diagnostic = forProtocol ? diag::cannot_convert_dict_value_protocol
                               : diag::cannot_convert_dict_value;
    }
  } else if (toType->isAnyObject()) {
    diagnostic = diag::cannot_convert_initializer_value_anyobject;
  }

  if (diagnostic.hasValue()) {
    emitDiagnostic(*diagnostic, fromType, toType);
    return true;
  }

  return false;
}

bool NonClassTypeToAnyObjectConversionFailure::diagnoseAsNote() {
  auto *locator = getLocator();

  if (locator->isForContextualType()) {
    return ContextualFailure::diagnoseAsNote();
  }

  if (locator->isLastElement<LocatorPathElt::ApplyArgToParam>()) {
    ArgumentMismatchFailure failure(getSolution(), getFromType(), getToType(),
                                    getLocator());
    return failure.diagnoseAsNote();
  }

  return false;
}

bool TupleContextualFailure::diagnoseAsError() {
  Diag<Type, Type> diagnostic;
  auto purpose = getContextualTypePurpose();
  if (isNumElementsMismatch())
    diagnostic = diag::tuple_types_not_convertible_nelts;
  else if (purpose == CTP_Unused)
    diagnostic = diag::tuple_types_not_convertible;
  else if (auto diag = getDiagnosticFor(purpose, getToType()))
    diagnostic = *diag;
  else
    return false;

  emitDiagnostic(diagnostic, getFromType(), getToType());
  return true;
}

bool FunctionTypeMismatch::diagnoseAsError() {
  auto purpose = getContextualTypePurpose();
  auto diagnostic = getDiagnosticFor(purpose, getToType());
  if (!diagnostic)
    return false;

  emitDiagnostic(*diagnostic, getFromType(), getToType());
  return true;
}

bool AutoClosureForwardingFailure::diagnoseAsError() {
  auto argRange = getSourceRange();
  emitDiagnostic(diag::invalid_autoclosure_forwarding)
      .highlight(argRange)
      .fixItInsertAfter(argRange.End, "()");
  return true;
}

bool AutoClosurePointerConversionFailure::diagnoseAsError() {
  auto diagnostic = diag::invalid_autoclosure_pointer_conversion;
  emitDiagnostic(diagnostic, getFromType(), getToType())
      .highlight(getSourceRange());
  return true;
}

bool NonOptionalUnwrapFailure::diagnoseAsError() {
  auto anchor = getAnchor();

  auto diagnostic = diag::invalid_optional_chain;
  if (isExpr<ForceValueExpr>(anchor))
    diagnostic = diag::invalid_force_unwrap;

  auto range = getSourceRange();
  emitDiagnostic(diagnostic, BaseType).highlight(range).fixItRemove(range.End);
  return true;
}

ASTNode MissingCallFailure::getAnchor() const {
  auto anchor = FailureDiagnostic::getAnchor();

  if (auto *FVE = getAsExpr<ForceValueExpr>(anchor))
    return FVE->getSubExpr();

  return anchor;
}

bool MissingCallFailure::diagnoseAsError() {
  auto anchor = getAnchor();
  SourceLoc insertLoc = getSourceRange().End;

  // Calls are not yet supported by key path, but it
  // is useful to record this fix to diagnose chaining
  // where one of the key path components is a method
  // reference.
  if (isExpr<KeyPathExpr>(anchor))
    return false;

  auto *locator = getLocator();
  auto path = locator->getPath();
  if (!path.empty()) {
    const auto &last = path.back();

    switch (last.getKind()) {
    case ConstraintLocator::ContextualType:
    case ConstraintLocator::ApplyArgToParam: {
      auto type = getType(anchor)->lookThroughAllOptionalTypes();
      auto fnType = type->castTo<FunctionType>();

      if (MissingArgumentsFailure::isMisplacedMissingArgument(getSolution(), locator)) {
        ArgumentMismatchFailure failure(
            getSolution(), fnType, fnType->getResult(), locator);
        return failure.diagnoseMisplacedMissingArgument();
      }

      emitDiagnostic(diag::missing_nullary_call, fnType->getResult())
          .highlight(getSourceRange())
          .fixItInsertAfter(insertLoc, "()");
      tryComputedPropertyFixIts();
      return true;
    }

    case ConstraintLocator::FunctionResult: {
      path = path.drop_back();
      if (path.back().getKind() != ConstraintLocator::AutoclosureResult)
        break;

      LLVM_FALLTHROUGH;
    }

    case ConstraintLocator::AutoclosureResult: {
      auto loc = getConstraintLocator(getRawAnchor(), path.drop_back());
      AutoClosureForwardingFailure failure(getSolution(), loc);
      return failure.diagnoseAsError();
    }
    default:
      break;
    }
  }

  if (auto *DRE = getAsExpr<DeclRefExpr>(anchor)) {
    emitDiagnostic(diag::did_not_call_function,
                   DRE->getDecl()->getBaseIdentifier())
        .fixItInsertAfter(insertLoc, "()");
    return true;
  }

  if (auto *UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
    emitDiagnostic(diag::did_not_call_method,
                   UDE->getName().getBaseIdentifier())
        .fixItInsertAfter(insertLoc, "()");
    return true;
  }

  if (auto *DSCE = getAsExpr<DotSyntaxCallExpr>(anchor)) {
    if (auto *DRE = dyn_cast<DeclRefExpr>(DSCE->getFn())) {
      emitDiagnostic(diag::did_not_call_method,
                     DRE->getDecl()->getBaseIdentifier())
          .fixItInsertAfter(insertLoc, "()");
      return true;
    }
  }

  if (auto *AE = getAsExpr<AssignExpr>(anchor)) {
    auto *srcExpr = AE->getSrc();
    if (auto *fnType = getType(srcExpr)->getAs<FunctionType>()) {
      emitDiagnosticAt(srcExpr->getLoc(), diag::missing_nullary_call,
                       fnType->getResult())
          .highlight(srcExpr->getSourceRange())
          .fixItInsertAfter(srcExpr->getEndLoc(), "()");
      return true;
    }
  }

  emitDiagnostic(diag::did_not_call_function_value)
      .fixItInsertAfter(insertLoc, "()");
  return true;
}

ASTNode PropertyWrapperReferenceFailure::getAnchor() const {
  auto anchor = FailureDiagnostic::getAnchor();
  if (getReferencedMember()) {
    return getBaseExprFor(getAsExpr(anchor));
  } else {
    return anchor;
  }
}

bool ExtraneousPropertyWrapperUnwrapFailure::diagnoseAsError() {
  auto newPrefix = usingProjection() ? "$" : "_";

  if (auto *member = getReferencedMember()) {
    emitDiagnostic(diag::incorrect_property_wrapper_reference_member,
                   member->getDescriptiveKind(), member->getName(), false,
                   getToType())
        .fixItInsert(getLoc(), newPrefix);
    return true;
  }

  emitDiagnostic(diag::incorrect_property_wrapper_reference, getPropertyName(),
                 getFromType(), getToType(), false)
      .fixItInsert(getLoc(), newPrefix);
  return true;
}

bool MissingPropertyWrapperUnwrapFailure::diagnoseAsError() {
  auto endLoc = getLoc().getAdvancedLoc(1);

  if (auto *member = getReferencedMember()) {
    emitDiagnostic(diag::incorrect_property_wrapper_reference_member,
                   member->getDescriptiveKind(), member->getName(), true,
                   getToType())
        .fixItRemoveChars(getLoc(), endLoc);
    return true;
  }

  emitDiagnostic(diag::incorrect_property_wrapper_reference, getPropertyName(),
                 getFromType(), getToType(), true)
      .fixItRemoveChars(getLoc(), endLoc);
  return true;
}

bool InvalidPropertyWrapperType::diagnoseAsError() {
  // The property wrapper constraint is currently only used for
  // implicit property wrappers on closure parameters.
  auto *wrappedVar = getAsDecl<VarDecl>(getAnchor());
  assert(wrappedVar->hasImplicitPropertyWrapper());

  emitDiagnostic(diag::invalid_implicit_property_wrapper, wrapperType);
  return true;
}

bool InvalidProjectedValueArgument::diagnoseAsError() {
  emitDiagnostic(diag::invalid_projection_argument, param->hasImplicitPropertyWrapper());

  if (!param->hasAttachedPropertyWrapper()) {
    param->diagnose(diag::property_wrapper_param_no_wrapper, param->getName());
  } else if (!param->hasImplicitPropertyWrapper() &&
             param->getOutermostAttachedPropertyWrapper()->hasArgs()) {
    param->diagnose(diag::property_wrapper_param_attr_arg);
  } else {
    Type backingType;
    if (param->hasImplicitPropertyWrapper()) {
      backingType = getType(param->getPropertyWrapperBackingProperty());
    } else {
      backingType = param->getPropertyWrapperBackingPropertyType();
    }

    param->diagnose(diag::property_wrapper_no_init_projected_value, backingType);
  }

  return true;
}

bool SubscriptMisuseFailure::diagnoseAsError() {
  auto *locator = getLocator();
  auto &sourceMgr = getASTContext().SourceMgr;

  auto *memberExpr = castToExpr<UnresolvedDotExpr>(getRawAnchor());
  auto *base = memberExpr->getBase();

  auto diag = emitDiagnostic(diag::could_not_find_subscript_member_did_you_mean,
                             getType(base));

  diag.highlight(memberExpr->getNameLoc().getSourceRange());

  if (auto *parentExpr = dyn_cast_or_null<ApplyExpr>(findParentExpr(memberExpr))) {
    auto *args = parentExpr->getArgs();

    auto toCharSourceRange = Lexer::getCharSourceRangeFromSourceRange;
    auto lastArgSymbol = toCharSourceRange(sourceMgr, args->getEndLoc());

    diag.fixItReplace(SourceRange(args->getStartLoc()),
                      getTokenText(tok::l_square));
    diag.fixItRemove(memberExpr->getNameLoc().getSourceRange());
    diag.fixItRemove(SourceRange(memberExpr->getDotLoc()));

    if (sourceMgr.extractText(lastArgSymbol) == getTokenText(tok::r_paren))
      diag.fixItReplace(SourceRange(args->getEndLoc()),
                        getTokenText(tok::r_square));
    else
      diag.fixItInsertAfter(args->getEndLoc(), getTokenText(tok::r_square));
  } else {
    diag.fixItReplace(SourceRange(memberExpr->getDotLoc(), memberExpr->getLoc()), "[<#index#>]");
  }
  diag.flush();

  if (auto overload = getOverloadChoiceIfAvailable(locator)) {
    emitDiagnosticAt(overload->choice.getDecl(), diag::kind_declared_here,
                     DescriptiveDeclKind::Subscript);
  }

  return true;
}

bool SubscriptMisuseFailure::diagnoseAsNote() {
  if (auto overload = getOverloadChoiceIfAvailable(getLocator())) {
    emitDiagnosticAt(overload->choice.getDecl(), diag::found_candidate);
    return true;
  }
  return false;
}

void MissingMemberFailure::diagnoseUnsafeCxxMethod(SourceLoc loc,
                                                   ASTNode anchor,
                                                   Type baseType,
                                                   DeclName name) const {
  auto &ctx = baseType->getASTContext();

  if (baseType->getAnyNominal() == nullptr ||
      // Don't waist time on non-cxx-methods.
      !isa_and_nonnull<clang::CXXRecordDecl>(
          baseType->getAnyNominal()->getClangDecl()))
    return;

  auto unsafeId =
      ctx.getIdentifier("__" + name.getBaseIdentifier().str().str() + "Unsafe");
  for (auto found :
       baseType->getAnyNominal()->lookupDirect(DeclBaseName(unsafeId))) {
    auto cxxMethod =
        dyn_cast_or_null<clang::CXXMethodDecl>(found->getClangDecl());
    if (!cxxMethod)
      continue;

    auto conformsToRACollection = [&](auto baseType) {
      auto raCollectionProto =
          ctx.getProtocol(KnownProtocolKind::CxxRandomAccessCollection);
      // We didn't load the overlay.
      if (raCollectionProto == nullptr)
        return false;

      SmallVector<ProtocolConformance *, 2> scratch;
      return baseType->getAnyNominal()->lookupConformance(raCollectionProto,
                                                          scratch);
    };

    auto returnTypeStr = cast<FuncDecl>(found)
                             ->getResultInterfaceType()
                             ->getAnyNominal()
                             ->getName()
                             .str();

    auto methodClangLoc = cxxMethod->getLocation();
    auto methodSwiftLoc =
        ctx.getClangModuleLoader()->importSourceLocation(methodClangLoc);

    // Rewrite front() and back() as first and last.
    if ((name.getBaseIdentifier().is("front") ||
         name.getBaseIdentifier().is("back")) &&
        cxxMethod->getReturnType()->isReferenceType() &&
        conformsToRACollection(baseType)) {
      auto dotExpr = getAsExpr<UnresolvedDotExpr>(anchor);
      auto callExpr = getAsExpr<CallExpr>(findParentExpr(dotExpr));
      bool isFront = name.getBaseIdentifier().is("front");

      ctx.Diags.diagnose(loc, diag::projection_reference_not_imported,
                         name.getBaseIdentifier().str(), returnTypeStr);
      ctx.Diags.diagnose(loc, diag::projection_may_return_interior_ptr,
                         name.getBaseIdentifier().str());
      ctx.Diags
          .diagnose(loc,
                    isFront ? diag::get_first_element : diag::get_last_element)
          .fixItReplaceChars(
              loc, loc.getAdvancedLoc(name.getBaseIdentifier().str().size()),
              isFront ? "first" : "last")
          .fixItRemove({callExpr->getArgs()->getStartLoc(),
                        callExpr->getArgs()->getEndLoc()});
    }

    // Rewrite begin() as a call to makeIterator() and end as nil.
    if (name.getBaseIdentifier().is("begin")) {
      if (conformsToRACollection(baseType)) {
        ctx.Diags.diagnose(loc, diag::iterator_method_unavailable,
                           name.getBaseIdentifier().str());
        ctx.Diags.diagnose(loc, diag::use_collection_apis)
            .fixItReplaceChars(
                loc, loc.getAdvancedLoc(name.getBaseIdentifier().str().size()),
                "makeIterator");
      } else {
        ctx.Diags.diagnose(loc, diag::iterator_method_unavailable,
                           name.getBaseIdentifier().str());
        ctx.Diags.diagnose(loc, diag::iterator_potentially_unsafe);
      }
    } else if (name.getBaseIdentifier().is("end")) {
      if (conformsToRACollection(baseType)) {
        auto dotExpr = getAsExpr<UnresolvedDotExpr>(anchor);
        auto callExpr = getAsExpr<CallExpr>(findParentExpr(dotExpr));

        ctx.Diags.diagnose(loc, diag::iterator_method_unavailable,
                           name.getBaseIdentifier().str());
        ctx.Diags.diagnose(loc, diag::replace_with_nil)
            .fixItReplaceChars(
                getAnchor().getStartLoc(),
                callExpr->getArgs()->getEndLoc().getAdvancedLoc(1), "nil");
      } else {
        ctx.Diags.diagnose(loc, diag::iterator_method_unavailable,
                           name.getBaseIdentifier().str());
        ctx.Diags.diagnose(loc, diag::iterator_potentially_unsafe);
      }
    } else if (cxxMethod->getReturnType()->isPointerType()) {
      ctx.Diags.diagnose(loc, diag::projection_ptr_not_imported,
                         name.getBaseIdentifier().str(), returnTypeStr);
      ctx.Diags.diagnose(loc, diag::projection_may_return_interior_ptr,
                         name.getBaseIdentifier().str());
      ctx.Diags
          .diagnose(methodSwiftLoc, diag::mark_safe_to_import,
                    name.getBaseIdentifier().str())
          .fixItInsert(methodSwiftLoc, " SWIFT_RETURNS_INDEPENDENT_VALUE ");
    } else if (cxxMethod->getReturnType()->isReferenceType()) {
      // Rewrite a call to .at(42) as a subscript.
      if (name.getBaseIdentifier().is("at") &&
          cxxMethod->getReturnType()->isReferenceType() &&
          conformsToRACollection(baseType)) {
        auto dotExpr = getAsExpr<UnresolvedDotExpr>(anchor);
        auto callExpr = getAsExpr<CallExpr>(findParentExpr(dotExpr));

        ctx.Diags.diagnose(dotExpr->getDotLoc(), diag::at_to_subscript)
            .fixItRemove(
                {dotExpr->getDotLoc(), callExpr->getArgs()->getStartLoc()})
            .fixItReplaceChars(
                callExpr->getArgs()->getStartLoc(),
                callExpr->getArgs()->getStartLoc().getAdvancedLoc(1), "[")
            .fixItReplaceChars(
                callExpr->getArgs()->getEndLoc(),
                callExpr->getArgs()->getEndLoc().getAdvancedLoc(1), "]");
        ctx.Diags.diagnose(loc, diag::projection_reference_not_imported,
                           name.getBaseIdentifier().str(), returnTypeStr);
        ctx.Diags.diagnose(loc, diag::projection_may_return_interior_ptr,
                           name.getBaseIdentifier().str());
      } else {
        ctx.Diags.diagnose(loc, diag::projection_reference_not_imported,
                           name.getBaseIdentifier().str(), returnTypeStr);
        ctx.Diags.diagnose(loc, diag::projection_may_return_interior_ptr,
                           name.getBaseIdentifier().str());
        ctx.Diags
            .diagnose(methodSwiftLoc, diag::mark_safe_to_import,
                      name.getBaseIdentifier().str())
            .fixItInsert(methodSwiftLoc, " SWIFT_RETURNS_INDEPENDENT_VALUE ");
      }
    } else if (cxxMethod->getReturnType()->isRecordType()) {
      if (auto cxxRecord = dyn_cast<clang::CXXRecordDecl>(
              cxxMethod->getReturnType()->getAsRecordDecl())) {
        auto methodSemantics = evaluateOrDefault(
            ctx.evaluator, CxxRecordSemantics({cxxRecord, ctx}), {});
        if (methodSemantics == CxxRecordSemanticsKind::Iterator) {
          ctx.Diags.diagnose(loc, diag::iterator_method_unavailable,
                             name.getBaseIdentifier().str());
          ctx.Diags.diagnose(loc, diag::iterator_potentially_unsafe);
        } else {
          assert(methodSemantics ==
                 CxxRecordSemanticsKind::UnsafePointerMember);

          auto baseSwiftLoc = ctx.getClangModuleLoader()->importSourceLocation(
              cxxRecord->getLocation());

          ctx.Diags.diagnose(loc, diag::projection_value_not_imported,
                             name.getBaseIdentifier().str(), returnTypeStr);
          ctx.Diags.diagnose(loc, diag::projection_may_return_interior_ptr,
                             name.getBaseIdentifier().str());
          ctx.Diags
              .diagnose(methodSwiftLoc, diag::mark_safe_to_import,
                        name.getBaseIdentifier().str())
              .fixItInsert(methodSwiftLoc, " SWIFT_RETURNS_INDEPENDENT_VALUE ");
          ctx.Diags
              .diagnose(baseSwiftLoc, diag::mark_self_contained, returnTypeStr)
              .fixItInsert(baseSwiftLoc, "SWIFT_SELF_CONTAINED ");
        }
      }
    }
  }
}

/// When a user refers a enum case with a wrong member name, we try to find a
/// enum element whose name differs from the wrong name only in convention;
/// meaning their lower case counterparts are identical.
///   - DeclName is valid when such a correct case is found; invalid otherwise.
DeclName MissingMemberFailure::findCorrectEnumCaseName(
    Type Ty, TypoCorrectionResults &corrections, DeclNameRef memberName) {
  if (memberName.isSpecial() || !memberName.isSimpleName())
    return DeclName();
  if (!Ty->getEnumOrBoundGenericEnum())
    return DeclName();
  auto candidate =
      corrections.getUniqueCandidateMatching([&](ValueDecl *candidate) {
        return (isa<EnumElementDecl>(candidate) &&
                candidate->getBaseIdentifier().str().equals_insensitive(
                    memberName.getBaseIdentifier().str()));
      });
  return (candidate ? candidate->getName() : DeclName());
}

bool MissingMemberFailure::diagnoseAsError() {
  auto anchor = getRawAnchor();
  auto memberBase = getAnchor();

  if (diagnoseForDynamicCallable())
    return true;
  
  if (diagnoseInLiteralCollectionContext())
    return true;

  if (diagnoseForSubscriptMemberWithTupleBase())
    return true;

  auto baseType = resolveType(getBaseType())->getWithoutSpecifierType();

  DeclNameLoc nameLoc(::getLoc(anchor));
  if (auto *UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
    nameLoc = UDE->getNameLoc();
  } else if (auto *UME = getAsExpr<UnresolvedMemberExpr>(anchor)) {
    nameLoc = UME->getNameLoc();
  }

  auto emitBasicError = [&](Type baseType) {
    auto diagnostic = diag::could_not_find_value_member;

    if (auto *metatype = baseType->getAs<MetatypeType>()) {
      baseType = metatype->getInstanceType();
      diagnostic = diag::could_not_find_type_member;
    }

    if (baseType->is<TupleType>())
      diagnostic = diag::could_not_find_tuple_member;

    bool hasUnresolvedPattern = false;
    if (auto *E = getAsExpr(anchor)) {
      auto &cs = getConstraintSystem();
      cs.forEachExpr(const_cast<Expr *>(E), [&](Expr *expr) {
        hasUnresolvedPattern |= isa<UnresolvedPatternExpr>(expr);
        return hasUnresolvedPattern ? nullptr : expr;
      });
    }

    if (hasUnresolvedPattern && !baseType->getAs<EnumType>()) {
      emitDiagnostic(diag::cannot_match_unresolved_expr_pattern_with_value,
                     baseType);
      return;
    }

    emitDiagnostic(diagnostic, baseType, getName())
        .highlight(getSourceRange())
        .highlight(nameLoc.getSourceRange());
    const auto &ctx = getSolution().getDC()->getASTContext();
    if (!ctx.LangOpts.DisableExperimentalClangImporterDiagnostics) {
      ctx.getClangModuleLoader()->diagnoseMemberValue(getName().getFullName(),
                                                      baseType);
      diagnoseUnsafeCxxMethod(getLoc(), anchor, baseType,
                              getName().getFullName());
    }
  };

  TypoCorrectionResults corrections(getName(), nameLoc);
  auto tryTypoCorrection = [&] (Type type) {
    TypeChecker::performTypoCorrection(getDC(), DeclRefKind::Ordinary, type,
                                       defaultMemberLookupOptions, corrections);
  };

  if (getName().getBaseName().getKind() == DeclBaseName::Kind::Subscript) {
    if (auto *metatype = baseType->getAs<MetatypeType>()) {
      emitDiagnostic(diag::could_not_find_type_member,
                     metatype->getInstanceType(), getName())
          .highlight(getSourceRange());
    } else {
      emitDiagnostic(diag::could_not_find_value_subscript, baseType)
          .highlight(getSourceRange());
    }
  } else if (getName().getBaseName() == "deinit") {
    // Specialised diagnostic if trying to access deinitialisers
    emitDiagnostic(diag::destructor_not_accessible).highlight(getSourceRange());
  } else if (auto metatypeTy = baseType->getAs<MetatypeType>()) {
    auto instanceTy = metatypeTy->getInstanceType();
    tryTypoCorrection(baseType);

    if (DeclName rightName =
            findCorrectEnumCaseName(instanceTy, corrections, getName())) {
      emitDiagnostic(diag::could_not_find_enum_case, instanceTy, getName(),
                     rightName)
          .fixItReplace(nameLoc.getBaseNameLoc(),
                        rightName.getBaseIdentifier().str());
      return true;
    }

    if (auto correction = corrections.claimUniqueCorrection()) {
      auto diagnostic =
          emitDiagnostic(diag::could_not_find_type_member_corrected, instanceTy,
                         getName(), correction->CorrectedName);
      diagnostic.highlight(getSourceRange())
          .highlight(nameLoc.getSourceRange());
      correction->addFixits(diagnostic);
    } else if ((instanceTy->getAnyNominal() ||
                instanceTy->is<ExistentialType>()) &&
               getName().getBaseName() == DeclBaseName::createConstructor()) {
      auto &cs = getConstraintSystem();

      auto result = cs.performMemberLookup(
          ConstraintKind::ValueMember, getName().withoutArgumentLabels(),
          metatypeTy, FunctionRefKind::DoubleApply, getLocator(),
          /*includeInaccessibleMembers=*/true);

      // If there are no `init` members at all produce a tailored
      // diagnostic for that, otherwise fallback to generic
      // "no such member" one.
      if (result.ViableCandidates.empty() &&
          result.UnviableCandidates.empty()) {
        emitDiagnostic(diag::no_accessible_initializers, instanceTy)
            .highlight(getSourceRange());
      } else {
        emitBasicError(baseType);
      }
    } else {
      emitBasicError(baseType);
    }
  } else if (auto moduleTy = baseType->getAs<ModuleType>()) {
    emitDiagnosticAt(::getLoc(memberBase), diag::no_member_of_module,
                     moduleTy->getModule()->getName(), getName())
        .highlight(getSourceRange())
        .highlight(nameLoc.getSourceRange());
    return true;
  } else {
    // Check for a few common cases that can cause missing members.
    auto *ED = baseType->getEnumOrBoundGenericEnum();
    if (ED && getName().isSimpleName("rawValue")) {
      auto loc = ED->getNameLoc();
      if (loc.isValid()) {
        emitBasicError(baseType);
        emitDiagnosticAt(loc, diag::did_you_mean_raw_type);
        return true;
      }
    } else if (baseType->isAny()) {
      emitBasicError(baseType);

      auto range = getSourceRange();
      emitDiagnostic(diag::any_as_anyobject_fixit)
          .fixItInsert(range.Start, "(")
          .fixItInsertAfter(range.End, " as AnyObject)");
      return true;
    }
    
    tryTypoCorrection(baseType);
    
    // If locator points to the member found via key path dynamic member lookup,
    // we provide a custom diagnostic and emit typo corrections for the wrapper type too.
    if (getLocator()->isForKeyPathDynamicMemberLookup()) {
      auto memberBaseType = getType(memberBase)->getWithoutSpecifierType();

      tryTypoCorrection(memberBaseType);

      if (auto correction = corrections.claimUniqueCorrection()) {
        auto diagnostic = emitDiagnostic(
            diag::could_not_find_value_dynamic_member_corrected, memberBaseType,
            baseType, getName(), correction->CorrectedName);
        diagnostic.highlight(getSourceRange())
            .highlight(nameLoc.getSourceRange());
        correction->addFixits(diagnostic);
      } else {
        auto diagnostic =
            emitDiagnostic(diag::could_not_find_value_dynamic_member,
                           memberBaseType, baseType, getName());
        diagnostic.highlight(getSourceRange())
            .highlight(nameLoc.getSourceRange());
      }
    } else {
      if (auto correction = corrections.claimUniqueCorrection()) {
        auto diagnostic =
            emitDiagnostic(diag::could_not_find_value_member_corrected,
                           baseType, getName(), correction->CorrectedName);
        diagnostic.highlight(getSourceRange())
            .highlight(nameLoc.getSourceRange());

        correction->addFixits(diagnostic);
      } else {
        emitBasicError(baseType);
      }
    }
  }

  // Note all the correction candidates.
  corrections.noteAllCandidates();
  return true;
}

bool MissingMemberFailure::diagnoseForDynamicCallable() const {
  auto *locator = getLocator();
  if (!locator->isLastElement<LocatorPathElt::DynamicCallable>())
    return false;

  auto memberName = getName();
  auto arguments = memberName.getArgumentNames();
  assert(arguments.size() == 1);

  auto &ctx = getASTContext();
  if (arguments.front() == ctx.Id_withKeywordArguments) {
    emitDiagnostic(diag::missing_dynamic_callable_kwargs_method, getBaseType());
    return true;
  }

  return false;
}

bool MissingMemberFailure::diagnoseInLiteralCollectionContext() const {
  auto *expr = getAsExpr(getAnchor());
  if (!expr)
    return false;

  auto *parentExpr = findParentExpr(expr);
  auto &solution = getSolution();

  if (!(parentExpr && isa<UnresolvedMemberExpr>(expr)))
    return false;

  if (!isa<UnresolvedMemberChainResultExpr>(parentExpr))
    return false;

  parentExpr = findParentExpr(parentExpr);
  if (!parentExpr)
    return false;

  // This could happen if collection is a dictionary literal i.e.
  // ["a": .test] - the element is a tuple - ("a", .test).
  if (isExpr<TupleExpr>(parentExpr))
    parentExpr = findParentExpr(parentExpr);

  if (!isExpr<CollectionExpr>(parentExpr))
    return false;

  if (auto *defaultableVar =
          getRawType(parentExpr)->getAs<TypeVariableType>()) {
    if (solution.DefaultedConstraints.count(
            defaultableVar->getImpl().getLocator()) != 0) {
      emitDiagnostic(diag::unresolved_member_no_inference, getName());
      return true;
    }
  }
  return false;
}

bool MissingMemberFailure::diagnoseForSubscriptMemberWithTupleBase() const {
  auto locator = getLocator();
  auto baseType = resolveType(getBaseType())->getWithoutSpecifierType();

  auto *SE = getAsExpr<SubscriptExpr>(locator->getAnchor());
  if (!SE)
    return false;

  auto tupleType = baseType->getAs<TupleType>();
  // For non-tuple type or empty tuples, let's fallback to the general
  // diagnostic logic.
  if (!tupleType || tupleType->getNumElements() == 0)
    return false;

  auto *args = SE->getArgs();
  if (auto *argExpr = args->getUnaryExpr()) {
    auto *literal =
        dyn_cast<IntegerLiteralExpr>(argExpr->getSemanticsProvidingExpr());

    llvm::Regex NumericRegex("^[0-9]+$");
    // Literal expressions may have other types of representations e.g. 0x01,
    // 0b01. So let's make sure to only suggest this tailored literal fix-it for
    // number only literals.
    if (literal && NumericRegex.match(literal->getDigitsText())) {
      unsigned int literalValue = 0;
      literal->getDigitsText().getAsInteger(/*Radix=*/0, literalValue);

      // Verify if the literal value is within the bounds of tuple elements.
      if (!literal->isNegative() &&
          literalValue < tupleType->getNumElements()) {
        llvm::SmallString<4> dotAccess;
        llvm::raw_svector_ostream OS(dotAccess);
        OS << "." << literalValue;

        emitDiagnostic(
            diag::could_not_find_subscript_member_tuple_did_you_mean_use_dot,
            baseType, literal->getDigitsText())
            .fixItReplace(args->getSourceRange(), OS.str());
        return true;
      }
    }

    // For subscript access on tuple base types where the subscript index is a
    // string literal expression which value matches a tuple element label,
    // let's suggest tuple label access.
    auto stringLiteral =
        dyn_cast<StringLiteralExpr>(argExpr->getSemanticsProvidingExpr());
    if (stringLiteral && !stringLiteral->getValue().empty() &&
        llvm::any_of(tupleType->getElements(), [&](TupleTypeElt element) {
          return element.getName().is(stringLiteral->getValue());
        })) {
      llvm::SmallString<16> dotAccess;
      llvm::raw_svector_ostream OS(dotAccess);
      OS << "." << stringLiteral->getValue();

      emitDiagnostic(
          diag::could_not_find_subscript_member_tuple_did_you_mean_use_dot,
          baseType, stringLiteral->getValue())
          .fixItReplace(args->getSourceRange(), OS.str());
      return true;
    }
  }

  emitDiagnostic(diag::could_not_find_subscript_member_tuple, baseType);
  return true;
}

bool UnintendedExtraGenericParamMemberFailure::diagnoseAsError() {
  MissingMemberFailure::diagnoseAsError();

  auto baseType = resolveType(getBaseType())->getWithoutSpecifierType();
  auto archetype = baseType->getMetatypeInstanceType()->castTo<ArchetypeType>();
  auto genericTy =
      archetype->mapTypeOutOfContext()->castTo<GenericTypeParamType>();
  SourceLoc loc = genericTy->getDecl()->getSourceRange().End;
  StringRef replacement;

  if (archetype->getConformsTo().size()) {
    loc = loc.getAdvancedLoc(
        archetype->getConformsTo().back()->getName().getLength());
    replacement = " &";
  } else {
    loc = loc.getAdvancedLoc(archetype->getName().getLength());
    replacement = ":";
  }
  emitDiagnosticAt(loc, diag::did_you_mean_generic_param_as_conformance,
                   ParamName, archetype)
      .fixItReplaceChars(loc, loc.getAdvancedLoc(1), replacement);
  return true;
}

bool InvalidMemberRefOnExistential::diagnoseAsError() {
  const auto Anchor = getRawAnchor();

  DeclNameLoc NameLoc;
  ParamDecl *PD = nullptr;
  if (auto *UDE = getAsExpr<UnresolvedDotExpr>(Anchor)) {
    NameLoc = UDE->getNameLoc();
    if (auto *DRE = dyn_cast<DeclRefExpr>(UDE->getBase())) {
      PD = dyn_cast<ParamDecl>(DRE->getDecl());
    }
  } else if (auto *UME = getAsExpr<UnresolvedMemberExpr>(Anchor)) {
    NameLoc = UME->getNameLoc();
  } else if (auto *SE = getAsExpr<SubscriptExpr>(Anchor)) {
    if (auto *DRE = dyn_cast<DeclRefExpr>(SE->getBase())) {
      PD = dyn_cast<ParamDecl>(DRE->getDecl());
    }
  }

  auto Diag = emitDiagnostic(diag::could_not_use_member_on_existential,
                             getBaseType(), getName());
  Diag.highlight(NameLoc.getSourceRange());
  Diag.highlight(getSourceRange());

  // If the base expression is a reference to a function or subscript
  // parameter, offer a fixit that replaces the existential parameter type with
  // its generic equivalent, e.g. func foo(p: any P) → func foo(p: some P).
  // Replacing 'any' with 'some' allows the code to compile without further
  // changes, such as naming an explicit type parameter, and is future-proofed
  // for same-type requirements on primary associated types instead of needing
  // a where clause.

  if (!PD || !PD->getDeclContext()->getAsDecl())
    return true;

  // Code inside a subscript is bound against a duplicate set of implicit
  // accessor parameters, which don't have a TypeRepr; dig out the corresponding
  // explicit subscript parameter.
  if (auto *const AD =
          dyn_cast<AccessorDecl>(PD->getDeclContext()->getAsDecl())) {
    auto *const SD = dyn_cast<SubscriptDecl>(AD->getStorage());
    if (!SD)
      return true;

    const auto AccessorParams = AD->getParameters()->getArray();
    const unsigned idx =
        llvm::find(AccessorParams, PD) - AccessorParams.begin();

    switch (AD->getAccessorKind()) {
    case AccessorKind::Set:
    case AccessorKind::WillSet:
    case AccessorKind::DidSet:
    case AccessorKind::Init:
      // Ignore references to the 'newValue' or 'oldValue' parameters.
      if (AccessorParams.front() == PD) {
        return true;
      }

      PD = SD->getIndices()->get(idx - 1);
      break;

    case AccessorKind::Get:
    case AccessorKind::Read:
    case AccessorKind::Modify:
    case AccessorKind::Address:
    case AccessorKind::MutableAddress:
      PD = SD->getIndices()->get(idx);
      break;
    }
  }

  // Bail out in the absence of a TypeRepr.
  if (!PD->getTypeRepr())
    return true;

  // Give up on 'inout' parameters. The intent is far more vague in this case,
  // and applying the fix-it would invalidate mutations.
  if (PD->isInOut())
    return true;

  auto *typeRepr = PD->getTypeRepr()->getWithoutParens();
  if (auto *STR = dyn_cast<SpecifierTypeRepr>(typeRepr)) {
    typeRepr = STR->getBase()->getWithoutParens();
  }

  SourceRange anyRange;
  TypeRepr *constraintRepr = typeRepr;
  if (auto *existentialRepr = dyn_cast<ExistentialTypeRepr>(typeRepr)) {
    constraintRepr = existentialRepr->getConstraint()->getWithoutParens();
    auto anyStart = existentialRepr->getAnyLoc();
    auto anyEnd = existentialRepr->getConstraint()->getStartLoc();
    anyRange = SourceRange(anyStart, anyEnd);
  }

  bool needsParens = false;
  while (auto *metatype = dyn_cast<MetatypeTypeRepr>(constraintRepr)) {
    // The generic equivalent of 'any P.Type' is '(some P).Type'
    constraintRepr = metatype->getBase()->getWithoutParens();
    if (isa<SimpleIdentTypeRepr>(constraintRepr))
      needsParens = !isa<TupleTypeRepr>(metatype->getBase());
  }

  std::string fix;
  llvm::raw_string_ostream OS(fix);
  if (needsParens)
    OS << "(";
  OS << "some ";
  constraintRepr->print(OS);
  if (needsParens)
    OS << ")";

  // When removing 'any', use a character-based removal to pick up
  // whitespaces between it and its constraint repr.
  Diag
    .fixItReplace(constraintRepr->getSourceRange(), fix)
    .fixItRemoveChars(anyRange.Start, anyRange.End);

  return true;
}

bool AllowTypeOrInstanceMemberFailure::diagnoseAsError() {
  auto loc = getLoc();
  auto *DC = getDC();
  auto locator = getLocator();

  if (loc.isInvalid()) {
    return true;
  }

  auto getRootExpr = [this](const Expr *childExpr) {
    auto *currExpr = const_cast<Expr *>(childExpr);
    while (auto parent = findParentExpr(currExpr))
      currExpr = parent;
    return currExpr;
  };

  auto anchor = getAnchor();

  if (!anchor.is<Expr *>())
    return false;

  Expr *expr = findParentExpr(castToExpr(anchor));
  SourceRange baseRange = expr ? expr->getSourceRange() : SourceRange();

  // If the base is an implicit self type reference, and we're in a
  // an initializer, then the user wrote something like:
  //
  //   class Foo { let x = 1, y = x }
  //
  // which runs in type context, not instance context, or
  //
  //   class Bar {
  //     let otherwise = 1              // instance member
  //     var x: Int
  //     func init(x: Int =otherwise) { // default parameter
  //       self.x = x
  //     }
  //   }
  //
  // in which an instance member is used as a default value for a
  // parameter.
  //
  // Produce a tailored diagnostic for these cases since this
  // comes up and is otherwise non-obvious what is going on.

  if (Name.isSimpleName(DeclBaseName::createConstructor()) &&
      !BaseType->is<AnyMetatypeType>()) {
    if (auto *ctorRef = getAsExpr<UnresolvedDotExpr>(getRawAnchor())) {
      if (isa<SuperRefExpr>(ctorRef->getBase())) {
        emitDiagnostic(diag::super_initializer_not_in_initializer);
        return true;
      }

      auto isCallArgument = [this](Expr *expr) {
        auto possibleApplyExpr = findParentExpr(expr);
        if (!possibleApplyExpr)
          return false;
        auto *args = possibleApplyExpr->getArgs();
        return args && args->findArgumentExpr(expr);
      };

      auto isMutable = [&DC](ValueDecl *decl) {
        if (auto *storage = dyn_cast<AbstractStorageDecl>(decl))
          return storage->isSettable(DC) && storage->isSetterAccessibleFrom(DC);

        return true;
      };

      auto *initCall = findParentExpr(ctorRef);
      auto *baseLoc = getConstraintLocator(ctorRef->getBase());
      if (auto selection = getCalleeOverloadChoiceIfAvailable(baseLoc)) {
        OverloadChoice choice = selection->choice;
        if (choice.isDecl() && isMutable(choice.getDecl()) &&
            !isCallArgument(initCall) &&
            getContextualTypePurpose(getRootExpr(ctorRef)) == CTP_Unused) {
          auto fixItLoc = ctorRef->getBase()->getSourceRange().End;
          emitDiagnostic(diag::init_not_instance_member_use_assignment)
              .fixItInsertAfter(fixItLoc, " = ");
          return true;
        }

        SourceRange fixItRng = ctorRef->getBase()->getSourceRange();
        emitDiagnostic(diag::init_not_instance_member)
            .fixItInsert(fixItRng.Start, "type(of: ")
            .fixItInsertAfter(fixItRng.End, ")");
        return true;
      }
    }
  }

  bool isStaticOrTypeMember = Member->isStatic() || isa<TypeDecl>(Member);
  if (BaseType->is<AnyMetatypeType>() && !isStaticOrTypeMember) {
    auto instanceTy = BaseType;

    if (auto *AMT = instanceTy->getAs<AnyMetatypeType>()) {
      instanceTy = AMT->getInstanceType();
    }

    auto *DC = getDC();
    if (DC->getContextKind() == DeclContextKind::Initializer) {
      auto *TypeDC = DC->getParent();
      bool propertyInitializer = true;
      // If the parent context is not a type context, we expect it
      // to be a defaulted parameter in a function declaration.
      if (!TypeDC->isTypeContext()) {
        assert(TypeDC->getContextKind() ==
               DeclContextKind::AbstractFunctionDecl &&
               "Expected function decl context for initializer!");
        TypeDC = TypeDC->getParent();
        propertyInitializer = false;
      }
      
      assert(TypeDC->isTypeContext() && "Expected type decl context!");
      
      if (TypeDC->getSelfNominalTypeDecl() == instanceTy->getAnyNominal()) {
        if (propertyInitializer) {
          emitDiagnostic(diag::instance_member_in_initializer, Name);
          return true;
        } else {
          emitDiagnostic(diag::instance_member_in_default_parameter, Name);
          return true;
        }
      }
    }

    if (auto *maybeCallExpr = getAsExpr(getRawAnchor())) {
      if (auto *UDE = dyn_cast<UnresolvedDotExpr>(maybeCallExpr)) {
        maybeCallExpr = UDE->getBase();
      }

      if (auto callExpr = dyn_cast<ApplyExpr>(maybeCallExpr)) {
        auto fnExpr = callExpr->getFn();
        auto fnType = getType(fnExpr)->getRValueType();
        auto *args = callExpr->getArgs();

        if (fnType->is<ExistentialMetatypeType>()) {
          emitDiagnosticAt(args->getStartLoc(),
                           diag::missing_init_on_metatype_initialization)
              .highlight(fnExpr->getSourceRange());
          return true;
        }
      }
    }

    // Check whether the instance member is declared on parent context and if so
    // provide more specialized message.
    auto memberTypeContext =
        Member->getDeclContext()->getInnermostTypeContext();
    auto currentTypeContext = getDC()->getInnermostTypeContext();

    if (memberTypeContext && currentTypeContext &&
        memberTypeContext->getSemanticDepth() <
        currentTypeContext->getSemanticDepth()) {
      emitDiagnostic(diag::could_not_use_instance_member_on_type,
                     currentTypeContext->getDeclaredInterfaceType(), Name,
                     memberTypeContext->getDeclaredInterfaceType(), true)
          .highlight(baseRange)
          .highlight(Member->getSourceRange());
      return true;
    }

    if (auto *UDE = getAsExpr<UnresolvedDotExpr>(getRawAnchor())) {
      auto *baseExpr = UDE->getBase();
      if (isa<TypeExpr>(baseExpr)) {
        emitDiagnostic(diag::instance_member_use_on_type, instanceTy, Name)
            .highlight(getSourceRange());
        return true;
      }
    }

    // Just emit a generic "instance member cannot be used" error
    emitDiagnostic(diag::could_not_use_instance_member_on_type, instanceTy,
                   Name, instanceTy, false)
        .highlight(getSourceRange());
    return true;
  } else {
    // If the base of the lookup is a protocol metatype, suggest
    // to replace the metatype with 'Self'
    // error saying the lookup cannot be on a protocol metatype
    Optional<InFlightDiagnostic> Diag;
    auto baseTy = BaseType;

    if (auto metatypeTy = baseTy->getAs<AnyMetatypeType>()) {
      auto instanceTy = metatypeTy->getInstanceType();

      // This will only happen if we have an unresolved dot expression
      // (.foo) where foo is a protocol member and the contextual type is
      // an optional protocol metatype.
      if (auto objectTy = instanceTy->getOptionalObjectType()) {
        instanceTy = objectTy;
        baseTy = MetatypeType::get(objectTy);
      }

      if (instanceTy->isExistentialType()) {
        // Give a customized message if we're accessing a member type
        // of a protocol -- otherwise a diagnostic talking about
        // static members doesn't make a whole lot of sense
        if (isa<TypeAliasDecl>(Member)) {
          Diag.emplace(
              emitDiagnostic(diag::typealias_outside_of_protocol, Name, instanceTy));
        } else if (isa<AssociatedTypeDecl>(Member)) {
          Diag.emplace(
              emitDiagnostic(diag::assoc_type_outside_of_protocol, Name, instanceTy));
        } else if (isa<ConstructorDecl>(Member)) {
          Diag.emplace(
              emitDiagnostic(diag::construct_protocol_by_name, instanceTy));
        } else {
          Diag.emplace(emitDiagnostic(
              diag::could_not_use_type_member_on_protocol_metatype, baseTy,
              Name));
        }

        Diag->highlight(baseRange).highlight(getSourceRange());

        // See through function decl context
        if (auto parent = getDC()->getInnermostTypeContext()) {
          // If we are in a protocol extension of 'Proto' and we see
          // 'Proto.static', suggest 'Self.static'
          if (auto extensionContext = parent->getExtendedProtocolDecl()) {
            auto constraint = instanceTy;
            if (auto existential = constraint->getAs<ExistentialType>())
              constraint = existential->getConstraintType();

            if (extensionContext->getDeclaredType()->isEqual(constraint)) {
              Diag->fixItReplace(getSourceRange(), "Self");
            }
          }
        }

        return true;
      }
    }

    // If this is a reference to a static member by one of the key path
    // components, let's provide a tailored diagnostic and return because
    // that is unsupported so there is no fix-it.
    if (locator->isInKeyPathComponent()) {
      InvalidStaticMemberRefInKeyPath failure(getSolution(), Member, locator);
      return failure.diagnoseAsError();
    }

    if (isa<EnumElementDecl>(Member)) {
      Diag.emplace(
          emitDiagnostic(diag::could_not_use_enum_element_on_instance, Name));
    } else {
      Diag.emplace(emitDiagnostic(diag::could_not_use_type_member_on_instance,
                                  baseTy, Name));
    }

    Diag->highlight(getSourceRange());

    if (Name.isSimpleName(DeclBaseName::createConstructor()) &&
        !baseTy->is<AnyMetatypeType>()) {
      if (auto ctorRef = getAsExpr<UnresolvedDotExpr>(getRawAnchor())) {
        SourceRange fixItRng = ctorRef->getNameLoc().getSourceRange();
        Diag->fixItInsert(fixItRng.Start, "type(of: ");
        Diag->fixItInsertAfter(fixItRng.End, ")");
        return true;
      }
    }

    // Determine the contextual type of the expression
    Type contextualType = getContextualType(getRawAnchor());
    // Try to provide a fix-it that only contains a '.'
    if (contextualType && baseTy->isEqual(contextualType)) {
      Diag->fixItInsert(loc, ".");
      return true;
    }

    // Check if the expression is the matching operator ~=, most often used in
    // case statements. If so, try to provide a single dot fix-it
    const Expr *contextualTypeNode = getRootExpr(getAsExpr(getAnchor()));

    // The '~=' operator is an overloaded decl ref inside a binaryExpr
    if (auto binaryExpr = dyn_cast<BinaryExpr>(contextualTypeNode)) {
      if (auto overloadedFn
          = dyn_cast<OverloadedDeclRefExpr>(binaryExpr->getFn())) {
        if (!overloadedFn->getDecls().empty()) {
          // Fetch any declaration to check if the name is '~='
          ValueDecl *decl0 = overloadedFn->getDecls()[0];
          
          if (decl0->getBaseName() == decl0->getASTContext().Id_MatchOperator) {
            // If the rhs of '~=' is the enum type, a single dot suffixes
            // since the type can be inferred
            Type secondArgType = getType(binaryExpr->getRHS());
            if (secondArgType->isEqual(baseTy)) {
              Diag->fixItInsert(loc, ".");
              return true;
            }
          }
        }
      }
    }

    // Fall back to a fix-it with a full type qualifier
    Expr *baseExpr = nullptr;
    if (const auto *SE = getAsExpr<SubscriptExpr>(getRawAnchor()))
      baseExpr = SE->getBase();
    else if (const auto UDE = getAsExpr<UnresolvedDotExpr>(getRawAnchor()))
      baseExpr = UDE->getBase();

    // An implicit 'self' reference base expression means we should
    // prepend with qualification.
    if (baseExpr && !baseExpr->isImplicit()) {
      Diag->fixItReplace(baseExpr->getSourceRange(),
                         diag::replace_with_type, baseTy);
    } else {
      Diag->fixItInsert(loc, diag::insert_type_qualification, baseTy);
    }

    return true;
  }

  return false;
}

bool PartialApplicationFailure::diagnoseAsError() {
  auto *anchor = castToExpr<UnresolvedDotExpr>(getRawAnchor());

  RefKind kind = RefKind::MutatingMethod;

  // If this is initializer delegation chain, we have a tailored message.
  if (getOverloadChoiceIfAvailable(
          getConstraintLocator(anchor, ConstraintLocator::ConstructorMember))) {
    kind = anchor->getBase()->isSuperExpr() ? RefKind::SuperInit
                                            : RefKind::SelfInit;
  } else if (anchor->getBase()->isSuperExpr()) {
    kind = RefKind::SuperMethod;
  }

  // TODO(https://github.com/apple/swift/issues/57572, diagnosticsQoI): Add a "did you mean to call it?" note with a fix-it for inserting '()' if function type has no params or all have a default value.
  auto diagnostic = CompatibilityWarning
                        ? diag::partial_application_of_function_invalid_swift4
                        : diag::partial_application_of_function_invalid;

  emitDiagnosticAt(anchor->getNameLoc(), diagnostic, kind);
  return true;
}

bool InvalidDynamicInitOnMetatypeFailure::diagnoseAsError() {
  emitDiagnostic(diag::dynamic_construct_class,
                 BaseType->getMetatypeInstanceType())
      .highlight(BaseRange);
  emitDiagnosticAt(Init, diag::note_nonrequired_initializer, Init->isImplicit(),
                   Init->getName());
  return true;
}

bool InitOnProtocolMetatypeFailure::diagnoseAsError() {
  if (IsStaticallyDerived) {
    emitDiagnostic(diag::construct_protocol_by_name,
                   BaseType->getMetatypeInstanceType())
        .highlight(BaseRange);
  } else {
    emitDiagnostic(diag::construct_protocol_value, BaseType)
        .highlight(BaseRange);
  }

  return true;
}

SourceLoc ImplicitInitOnNonConstMetatypeFailure::getLoc() const {
  if (auto *apply = getAsExpr<ApplyExpr>(getRawAnchor()))
    return apply->getArgs()->getStartLoc();

  return FailureDiagnostic::getLoc();
}

bool ImplicitInitOnNonConstMetatypeFailure::diagnoseAsError() {
  emitDiagnostic(diag::missing_init_on_metatype_initialization)
      .fixItInsert(getLoc(), ".init");
  return true;
}

ASTNode MissingArgumentsFailure::getAnchor() const {
  auto anchor = FailureDiagnostic::getAnchor();

  if (auto *captureList = getAsExpr<CaptureListExpr>(anchor))
    return captureList->getClosureBody();

  return anchor;
}

SourceLoc MissingArgumentsFailure::getLoc() const {
  auto *argList = getArgumentListFor(getLocator());
  if (argList && !argList->isImplicit())
    return argList->getLoc();
  return FailureDiagnostic::getLoc();
}

bool MissingArgumentsFailure::diagnoseAsError() {
  auto *locator = getLocator();

  if (!(locator->isLastElement<LocatorPathElt::ApplyArgToParam>() ||
        locator->isLastElement<LocatorPathElt::ContextualType>() ||
        locator->isLastElement<LocatorPathElt::ApplyArgument>() ||
        locator->isLastElement<LocatorPathElt::ClosureResult>() ||
        locator->isLastElement<LocatorPathElt::ClosureBody>()))
    return false;

  // If this is a misplaced `missing argument` situation, it would be
  // diagnosed by invalid conversion fix.
  if (isMisplacedMissingArgument(getSolution(), locator))
    return false;

  auto anchor = getAnchor();

  if (auto *closure = getAsExpr<ClosureExpr>(anchor))
    return diagnoseClosure(closure);

  // This is a situation where function type is passed as an argument
  // to a function type parameter and their argument arity is different.
  //
  // ```
  // func foo(_: (Int) -> Void) {}
  // func bar() {}
  //
  // foo(bar) // `() -> Void` vs. `(Int) -> Void`
  // ```
  if (locator->isLastElement<LocatorPathElt::ApplyArgToParam>()) {
    auto info = *(getFunctionArgApplyInfo(locator));

    auto *argExpr = info.getArgExpr();
    emitDiagnosticAt(argExpr->getLoc(), diag::cannot_convert_argument_value,
                     info.getArgType(), info.getParamType());
    // TODO: It would be great so somehow point out which arguments are missing.
    return true;
  }

  // Function type has fewer arguments than expected by context:
  //
  // ```
  // func foo() {}
  // let _: (Int) -> Void = foo
  // ```
  if (locator->isLastElement<LocatorPathElt::ContextualType>()) {
    emitDiagnostic(diag::cannot_convert_initializer_value, getType(anchor),
                   resolveType(getContextualType(getAnchor())));
    // TODO: It would be great so somehow point out which arguments are missing.
    return true;
  }

  if (diagnoseInvalidTupleDestructuring())
    return true;

  if (SynthesizedArgs.size() == 1)
    return diagnoseSingleMissingArgument();

  // At this point we know that this is a situation when
  // there are multiple arguments missing, so let's produce
  // a diagnostic which lists all of them and a fix-it
  // to add arguments at appropriate positions.

  SmallString<32> diagnostic;
  llvm::raw_svector_ostream arguments(diagnostic);

  interleave(
      SynthesizedArgs,
      [&](const SynthesizedArg &e) {
        const auto paramIdx = e.paramIdx;
        const auto &arg = e.param;

        if (arg.hasLabel()) {
          arguments << "'" << arg.getLabel().str() << "'";
        } else {
          arguments << "#" << (paramIdx + 1);
        }
      },
      [&] { arguments << ", "; });

  auto paramContext = getParameterContextForDiag(getRawAnchor());
  auto diag = emitDiagnostic(
      diag::missing_arguments_in_call, arguments.str(),
      static_cast<unsigned>(paramContext));

  auto callInfo = getCallInfo(anchor);
  auto *args = callInfo ? callInfo->second : nullptr;

  // TODO(diagnostics): We should be able to suggest this fix-it
  // unconditionally.
  SmallString<32> scratch;
  llvm::raw_svector_ostream fixIt(scratch);
  auto appendMissingArgsToFix = [&]() {
    interleave(
        SynthesizedArgs,
        [&](const SynthesizedArg &arg) {
          forFixIt(fixIt, arg.param);
        },
        [&] { fixIt << ", "; });
  };

  if (args && args->empty() && !args->isImplicit()) {
    appendMissingArgsToFix();
    diag.fixItInsertAfter(args->getLParenLoc(), fixIt.str());
  } else if (isExpr<MacroExpansionExpr>(getRawAnchor())) {
    fixIt << "(";
    appendMissingArgsToFix();
    fixIt << ")";
    diag.fixItInsertAfter(getRawAnchor().getEndLoc(), fixIt.str());
  }

  diag.flush();

  if (auto selectedOverload = getCalleeOverloadChoiceIfAvailable(locator)) {
    if (auto *decl = selectedOverload->choice.getDeclOrNull()) {
      emitDiagnosticAt(decl, diag::decl_declared_here, decl->getName());
    }
  }

  return true;
}

bool MissingArgumentsFailure::diagnoseAsNote() {
  auto *locator = getLocator();
  if (auto overload = getCalleeOverloadChoiceIfAvailable(locator)) {
    auto *fn = resolveType(overload->adjustedOpenedType)->getAs<AnyFunctionType>();
    auto loc = overload->choice.getDecl()->getLoc();

    if (loc.isInvalid())
      loc = getLoc();

    emitDiagnosticAt(loc, diag::candidate_partial_match,
                     fn->getParamListAsString(fn->getParams()));
    return true;
  }

  return false;
}

bool MissingArgumentsFailure::diagnoseSingleMissingArgument() const {
  auto &ctx = getASTContext();

  auto anchor = getRawAnchor();
  if (!(isExpr<CallExpr>(anchor) || isExpr<SubscriptExpr>(anchor) ||
        isExpr<UnresolvedMemberExpr>(anchor) ||
        isExpr<ObjectLiteralExpr>(anchor) ||
        isExpr<MacroExpansionExpr>(anchor)))
    return false;

  if (SynthesizedArgs.size() != 1)
    return false;

  auto paramContext = getParameterContextForDiag(anchor);

  const auto &argument = SynthesizedArgs.front();
  auto position = argument.paramIdx;
  auto label = argument.param.getLabel();

  auto callInfo = getCallInfo(anchor);
  if (!callInfo)
    return false;

  auto *fnExpr = callInfo->first;
  auto *args = callInfo->second;

  // Will the parameter accept a trailing closure?
  Type paramType = resolveType(argument.param.getPlainType());
  bool paramAcceptsTrailingClosure = paramType
      ->lookThroughAllOptionalTypes()->is<AnyFunctionType>();

  // Determine whether we're inserting as a trailing closure.
  auto firstTrailingClosureIdx = args->getFirstTrailingClosureIndex();
  auto insertingTrailingClosure =
      firstTrailingClosureIdx && position > *firstTrailingClosureIdx;

  SmallString<32> insertBuf;
  llvm::raw_svector_ostream insertText(insertBuf);

  if (insertingTrailingClosure)
    insertText << " ";
  else if (position != 0)
    insertText << ", ";

  forFixIt(insertText, argument.param);

  if (position == 0 && !args->empty() &&
      !args->isTrailingClosureIndex(position)) {
    insertText << ", ";
  }

  SourceLoc insertLoc;

  if (position >= args->size() && insertingTrailingClosure) {
    // Add a trailing closure to the end.

    // fn { closure }:
    //   fn {closure} label: [argMissing]
    // fn() { closure }:
    //   fn() {closure} label: [argMissing]
    // fn(argX) { closure }:
    //   fn(argX) { closure } label: [argMissing]
    insertLoc = Lexer::getLocForEndOfToken(ctx.SourceMgr, args->getEndLoc());
  } else if (!args->isUnlabeledUnary()) {
    // fn(argX, argY):
    //   fn([argMissing, ]argX, argY)
    //   fn(argX[, argMissing], argY)
    // fn(argX) { closure }:
    //   fn([argMissing, ]argX) { closure }
    //   fn(argX[, argMissing]) { closure }
    // fn(argX, argY):
    //   fn(argX, argY[, argMissing])
    if (args->empty()) {
      insertLoc = args->getRParenLoc();
    } else if (position != 0) {
      auto argPos = std::min(args->size(), position) - 1;
      insertLoc = Lexer::getLocForEndOfToken(
          ctx.SourceMgr, args->getExpr(argPos)->getEndLoc());
    } else {
      insertLoc = args->getLabelLoc(0);
      if (insertLoc.isInvalid())
        insertLoc = args->getExpr(0)->getStartLoc();
    }
  } else {
    if (args->getRParenLoc().isValid()) {
      // fn():
      //   fn([argMissing])
      // fn(argX):
      //   fn(argX[, argMissing])
      //   fn([argMissing, ]argX)
      // fn() { closure }:
      //   fn([argMissing]) {closure}
      if (position == 0) {
        insertLoc = Lexer::getLocForEndOfToken(ctx.SourceMgr,
                                               args->getLParenLoc());
      } else {
        insertLoc = Lexer::getLocForEndOfToken(
            ctx.SourceMgr, args->getExpr(0)->getEndLoc());
      }
    } else {
      // fn { closure }:
      //   fn[(argMissing)] { closure }
      assert(!isExpr<SubscriptExpr>(anchor) && "bracket less subscript");
      assert(args->hasAnyTrailingClosures() &&
             "paren less ParenExpr without trailing closure");
      insertBuf.insert(insertBuf.begin(), '(');
      insertBuf.insert(insertBuf.end(), ')');
      insertLoc =
          Lexer::getLocForEndOfToken(ctx.SourceMgr, fnExpr->getEndLoc());
    }
  }

  if (insertLoc.isInvalid())
    return false;

  // If we are trying to insert a trailing closure but the parameter
  // corresponding to the missing argument doesn't support a trailing closure,
  // don't provide a Fix-It.
  // FIXME: It's possible to parenthesize and relabel the argument list to
  // accommodate this, but it's tricky.
  bool shouldEmitFixIt =
    !(insertingTrailingClosure && !paramAcceptsTrailingClosure);

  if (label.empty()) {
    auto diag = emitDiagnosticAt(
        insertLoc, diag::missing_argument_positional, position + 1,
        static_cast<unsigned>(paramContext));
    if (shouldEmitFixIt)
      diag.fixItInsert(insertLoc, insertText.str());
  } else if (isPropertyWrapperInitialization()) {
    auto *TE = cast<TypeExpr>(fnExpr);
    emitDiagnosticAt(TE->getLoc(), diag::property_wrapper_missing_arg_init,
                     label, resolveType(TE->getInstanceType())->getString());
  } else {
    auto diag = emitDiagnosticAt(
        insertLoc, diag::missing_argument_named, label,
        static_cast<unsigned>(paramContext));
    if (shouldEmitFixIt)
      diag.fixItInsert(insertLoc, insertText.str());
  }

  if (auto selectedOverload =
          getCalleeOverloadChoiceIfAvailable(getLocator())) {
    if (auto *decl = selectedOverload->choice.getDeclOrNull()) {
      emitDiagnosticAt(decl, diag::decl_declared_here, decl->getName());
    }
  }

  return true;
}

bool MissingArgumentsFailure::diagnoseClosure(const ClosureExpr *closure) {
  FunctionType *funcType = nullptr;

  auto *locator = getLocator();
  if (locator->isForContextualType()) {
    if (auto contextualType = getContextualType(locator->getAnchor())) {
      funcType = contextualType->getAs<FunctionType>();
    }
  } else if (auto info = getFunctionArgApplyInfo(locator)) {
    auto paramType = info->getParamType();
    // Drop a single layer of optionality because argument could get injected
    // into optional and that doesn't contribute to the problem.
    if (auto objectType = paramType->getOptionalObjectType())
      paramType = objectType;
    funcType = paramType->getAs<FunctionType>();
  } else if (locator->isLastElement<LocatorPathElt::ClosureResult>() ||
             locator->isLastElement<LocatorPathElt::ClosureBody>()) {
    // Based on the locator we know this is something like this:
    // `let _: () -> ((Int) -> Void) = { return {} }`.
    funcType = getType(getRawAnchor())
                   ->castTo<FunctionType>()
                   ->getResult()
                   ->castTo<FunctionType>();
  }

  if (!funcType)
    return false;

  unsigned numSynthesized = SynthesizedArgs.size();
  auto diff = funcType->getNumParams() - numSynthesized;

  // If the closure didn't specify any arguments and it is in a context that
  // needs some, produce a fixit to turn "{...}" into "{ _,_ in ...}".
  if (diff == 0) {
    auto diag =
        emitDiagnosticAt(closure->getStartLoc(),
                         diag::closure_argument_list_missing, numSynthesized);

    std::string fixText; // Let's provide fixits for up to 10 args.
    if (funcType->getNumParams() <= 10) {
      fixText += " ";
      interleave(
          funcType->getParams(),
          [&fixText](const AnyFunctionType::Param &param) {
            if (param.hasLabel()) {
              fixText += param.getLabel().str();
            } else if (param.hasInternalLabel()) {
              fixText += param.getInternalLabel().str();
            } else {
              fixText += '_';
            }
          },
          [&fixText] { fixText += ','; });
      fixText += " in ";
    }

    if (!fixText.empty()) {
      // Determine if there is already a space after the { in the closure to
      // make sure we introduce the right whitespace.
      auto afterBrace = closure->getStartLoc().getAdvancedLoc(1);
      auto text = getASTContext().SourceMgr.extractText({afterBrace, 1});
      if (text.size() == 1 && text == " ")
        fixText = fixText.erase(fixText.size() - 1);
      else
        fixText = fixText.erase(0, 1);
      diag.fixItInsertAfter(closure->getStartLoc(), fixText);
    }

    return true;
  }

  auto params = closure->getParameters();
  bool onlyAnonymousParams =
      std::all_of(params->begin(), params->end(),
                  [](ParamDecl *param) { return !param->hasName(); });

  auto diag = emitDiagnosticAt(
      params->getStartLoc(), diag::closure_argument_list_tuple,
      resolveType(funcType), funcType->getNumParams(), diff, diff == 1);

  // If the number of parameters is less than number of inferred
  // let's try to suggest a fix-it with the rest of the missing parameters.
  if (!closure->hasExplicitResultType() &&
      closure->getInLoc().isValid()) {
    SmallString<32> fixIt;
    llvm::raw_svector_ostream OS(fixIt);

    OS << ",";
    for (unsigned i = 0; i != numSynthesized; ++i) {
      OS << ((onlyAnonymousParams) ? "_" : "<#arg#>");
      OS << ((i == numSynthesized - 1) ? " " : ",");
    }

    diag.fixItInsertAfter(params->getEndLoc(), OS.str());
  }

  return true;
}

bool MissingArgumentsFailure::diagnoseInvalidTupleDestructuring() const {
  auto *locator = getLocator();
  if (!locator->isLastElement<LocatorPathElt::ApplyArgument>())
    return false;

  auto *args = getArgumentListFor(locator);
  if (!args)
    return false;

  auto *argExpr = args->getUnaryExpr();
  if (!(argExpr && getType(argExpr)->getRValueType()->is<TupleType>()))
    return false;

  auto selectedOverload = getCalleeOverloadChoiceIfAvailable(locator);
  if (!selectedOverload)
    return false;

  auto *decl = selectedOverload->choice.getDeclOrNull();
  if (!decl)
    return false;

  auto *funcType =
      resolveType(selectedOverload->openedType)->getAs<FunctionType>();
  if (!funcType)
    return false;

  auto name = decl->getBaseName();
  auto diagnostic =
      emitDiagnostic(diag::cannot_convert_single_tuple_into_multiple_arguments,
                     decl->getDescriptiveKind(), name, name.isSpecial(),
                     funcType->getNumParams(), isa<TupleExpr>(argExpr));

  // If argument is a literal tuple, let's suggest removal of parentheses.
  if (auto *TE = dyn_cast<TupleExpr>(argExpr)) {
    diagnostic.fixItRemove(TE->getLParenLoc()).fixItRemove(TE->getRParenLoc());
  }

  diagnostic.flush();

  // Add a note which points to the overload choice location.
  emitDiagnosticAt(decl, diag::decl_declared_here, decl->getName());
  return true;
}

bool MissingArgumentsFailure::isPropertyWrapperInitialization() const {
  auto *call = getAsExpr<CallExpr>(getRawAnchor());
  if (!(call && call->isImplicit()))
    return false;

  auto TE = dyn_cast<TypeExpr>(call->getFn());
  if (!TE)
    return false;

  auto instanceTy = TE->getInstanceType();
  if (!instanceTy)
    return false;

  auto *NTD = resolveType(instanceTy)->getAnyNominal();
  return NTD && NTD->getAttrs().hasAttribute<PropertyWrapperAttr>();
}

bool MissingArgumentsFailure::isMisplacedMissingArgument(
    const Solution &solution, ConstraintLocator *locator) {
  auto *calleeLocator = solution.getCalleeLocator(locator);
  auto overloadChoice = solution.getOverloadChoiceIfAvailable(calleeLocator);
  if (!overloadChoice)
    return false;

  auto *fnType =
      solution.simplifyType(overloadChoice->adjustedOpenedType)->getAs<FunctionType>();
  if (!(fnType && fnType->getNumParams() == 2))
    return false;

  auto anchor = locator->getAnchor();

  auto hasFixFor = [&](FixKind kind, ConstraintLocator *locator) -> bool {
    auto fix = llvm::find_if(solution.Fixes, [&](const ConstraintFix *fix) {
      return fix->getLocator() == locator;
    });

    if (fix == solution.Fixes.end())
      return false;

    return (*fix)->getKind() == kind;
  };

  auto *callLocator =
      solution.getConstraintLocator(anchor, {ConstraintLocator::ApplyArgument});

  auto argFlags = fnType->getParams()[0].getParameterFlags();
  auto *argLoc = solution.getConstraintLocator(
      callLocator, LocatorPathElt::ApplyArgToParam(0, 0, argFlags));

  bool hasArgumentMismatch = hasFixFor(FixKind::AllowArgumentTypeMismatch, argLoc) ||
                             hasFixFor(FixKind::InsertCall, argLoc);
  if (!(hasArgumentMismatch && hasFixFor(FixKind::AddMissingArguments, callLocator)))
    return false;

  auto *anchorExpr = getAsExpr(anchor);
  if (!anchorExpr)
    return false;

  auto *argList = anchorExpr->getArgs();
  if (!argList)
    return false;

  auto *unaryArg = argList->getUnaryExpr();
  if (!unaryArg)
    return false;

  auto argType = solution.simplifyType(solution.getType(unaryArg));
  auto paramType = fnType->getParams()[1].getPlainType();

  return TypeChecker::isConvertibleTo(argType, paramType, solution.getDC());
}

Optional<std::pair<Expr *, ArgumentList *>>
MissingArgumentsFailure::getCallInfo(ASTNode anchor) const {
  if (auto *call = getAsExpr<CallExpr>(anchor)) {
    return std::make_pair(call->getFn(), call->getArgs());
  } else if (auto *SE = getAsExpr<SubscriptExpr>(anchor)) {
    return std::make_pair((Expr *)SE, SE->getArgs());
  } else if (auto *OLE = getAsExpr<ObjectLiteralExpr>(anchor)) {
    return std::make_pair((Expr *)OLE, OLE->getArgs());
  } else if (auto *ME = getAsExpr<MacroExpansionExpr>(anchor)) {
    return std::make_pair((Expr *)ME, ME->getArgs());
  }
  return None;
}

void MissingArgumentsFailure::forFixIt(
    llvm::raw_svector_ostream &out,
    const AnyFunctionType::Param &argument) const {
  if (argument.hasLabel())
    out << argument.getLabel().str() << ": ";

  // Explode inout type.
  if (argument.isInOut())
    out << "&";

  auto resolvedType = resolveType(argument.getPlainType());
  // @autoclosure; the type should be the result type.
  if (argument.isAutoClosure())
    resolvedType = resolvedType->castTo<FunctionType>()->getResult();

  out << "<#" << resolvedType << "#>";
}

SourceLoc ClosureParamDestructuringFailure::getLoc() const {
  auto *closure = castToExpr<ClosureExpr>(getAnchor());
  auto paramList = closure->getParameters();
  return paramList->getStartLoc();
}

SourceRange ClosureParamDestructuringFailure::getSourceRange() const {
  auto *closure = castToExpr<ClosureExpr>(getAnchor());
  auto paramList = closure->getParameters();
  return paramList->getSourceRange();
}

bool ClosureParamDestructuringFailure::diagnoseAsError() {
  auto *closure = castToExpr<ClosureExpr>(getAnchor());
  auto params = closure->getParameters();

  // In case of implicit parameters e.g. $0, $1 we
  // can't really provide good fix-it because
  // structure of parameter type itself is unclear.
  for (auto *param : params->getArray()) {
    if (param->isImplicit()) {
      emitDiagnostic(diag::closure_tuple_parameter_destructuring_implicit,
                     getParameterType());
      return true;
    }
  }

  auto diag = emitDiagnostic(diag::closure_tuple_parameter_destructuring,
                             getParameterType());

  auto *closureBody = closure->getBody();
  if (!closureBody)
    return true;

  auto &sourceMgr = getASTContext().SourceMgr;
  auto bodyStmts = closureBody->getElements();

  SourceLoc bodyLoc;
  SourceLoc inLoc = closure->getInLoc();
  // If location for `in` is unknown we can't proceed
  // since we'll not be able to figure out source line
  // to place the fix-it on.
  if (inLoc.isInvalid())
    return true;

  // If the body is empty let's put the cursor
  // right after "in", otherwise make it start
  // location of the first statement in the body.
  if (bodyStmts.empty())
    bodyLoc = Lexer::getLocForEndOfToken(sourceMgr, inLoc);
  else
    bodyLoc = bodyStmts.front().getStartLoc();

  if (bodyLoc.isInvalid())
    return true;

  SmallString<64> fixIt;
  llvm::raw_svector_ostream OS(fixIt);

  // If this is multi-line closure we'd have to insert new lines
  // in the suggested 'let' to keep the structure of the code intact,
  // otherwise just use ';' to keep everything on the same line.
  auto inLine = sourceMgr.getLineAndColumnInBuffer(inLoc).first;
  auto bodyLine = sourceMgr.getLineAndColumnInBuffer(bodyLoc).first;
  auto isMultiLineClosure = bodyLine > inLine;
  auto indent =
      bodyStmts.empty() ? "" : Lexer::getIndentationForLine(sourceMgr, bodyLoc);

  SmallString<16> parameter;
  llvm::raw_svector_ostream parameterOS(parameter);

  parameterOS << "(";
  interleave(
      params->getArray(),
      [&](const ParamDecl *param) { parameterOS << param->getNameStr(); },
      [&] { parameterOS << ", "; });
  parameterOS << ")";

  // Check if there are any explicit types associated
  // with parameters, if there are, we'll have to add
  // type information to the replacement argument.
  bool explicitTypes =
      llvm::any_of(params->getArray(),
                   [](const ParamDecl *param) { return param->getTypeRepr(); });

  if (isMultiLineClosure)
    OS << '\n' << indent;
  else if (closure->getBody()->empty())
    OS << ' ';

  // Let's form 'let <name> : [<type>]? = arg' expression.
  OS << "let " << parameterOS.str() << " = arg"
     << (isMultiLineClosure ? "\n" + indent : "; ");

  SmallString<64> argName;
  llvm::raw_svector_ostream nameOS(argName);
  if (explicitTypes) {
    nameOS << "(arg: " << getParameterType()->getString() << ")";
  } else {
    nameOS << "(arg)";
  }

  if (closure->hasSingleExpressionBody()) {
    // Let's see if we need to add result type to the argument/fix-it:
    //  - if the there is a result type associated with the closure;
    //  - and it's not a void type;
    //  - and it hasn't been explicitly written.
    auto resultType = resolveType(ContextualType->getResult());
    auto hasResult = [](Type resultType) -> bool {
      return resultType && !resultType->isVoid();
    };

    auto isValidType = [](Type resultType) -> bool {
      return resultType && !resultType->hasUnresolvedType() &&
             !resultType->hasTypeVariable();
    };

    // If there an expected result type but it hasn't been explicitly
    // provided, let's add it to the argument.
    if (hasResult(resultType) && !closure->hasExplicitResultType()) {
      nameOS << " -> ";
      if (isValidType(resultType))
        nameOS << resultType->getString();
      else
        nameOS << "<#Result#>";
    }

    if (auto stmt = bodyStmts.front().get<Stmt *>()) {
      // If the body is a single expression with implicit return.
      if (isa<ReturnStmt>(stmt) && stmt->isImplicit()) {
        // And there is non-void expected result type,
        // because we add 'let' expression to the body
        // we need to make such 'return' explicit.
        if (hasResult(resultType))
          OS << "return ";
      }
    }
  }

  diag.fixItReplace(getSourceRange(), nameOS.str())
      .fixItInsert(bodyLoc, OS.str());
  return true;
}

bool OutOfOrderArgumentFailure::diagnoseAsError() {
  auto anchor = getRawAnchor();
  auto *args = getArgumentListFor(getLocator());
  if (!args)
    return false;

  Identifier first = args->getLabel(ArgIdx);
  Identifier second = args->getLabel(PrevArgIdx);

  // Build a mapping from arguments to parameters.
  SmallVector<unsigned, 4> argBindings(args->size());
  for (unsigned paramIdx = 0; paramIdx != Bindings.size(); ++paramIdx) {
    for (auto argIdx : Bindings[paramIdx])
      argBindings[argIdx] = paramIdx;
  }

  auto argRange = [&](unsigned argIdx, Identifier label) -> SourceRange {
    auto range = args->getExpr(argIdx)->getSourceRange();
    if (!label.empty())
      range.Start = args->getLabelLoc(argIdx);

    unsigned paramIdx = argBindings[argIdx];
    if (Bindings[paramIdx].size() > 1)
      range.End = args->getExpr(Bindings[paramIdx].back())->getEndLoc();

    return range;
  };

  auto firstRange = argRange(ArgIdx, first);
  auto secondRange = argRange(PrevArgIdx, second);

  SourceLoc diagLoc = firstRange.Start;

  auto addFixIts = [&](InFlightDiagnostic diag) {
    // Don't add Fix-Its if one of the ranges is outside of the argument
    // list, which can happen when we're splicing together an argument list
    // from multiple sources.
    auto &SM = getASTContext().SourceMgr;
    auto argsRange = args->getSourceRange();
    if (!SM.rangeContains(argsRange, firstRange) ||
        !SM.rangeContains(argsRange, secondRange))
      return;

    diag.highlight(firstRange).highlight(secondRange);

    // Move the misplaced argument by removing it from one location and
    // inserting it in another location. To maintain argument comma
    // separation, since the argument is always moving to an earlier index
    // the preceding comma and whitespace is removed and a new trailing
    // comma and space is inserted with the moved argument.
    auto text = SM.extractText(
        Lexer::getCharSourceRangeFromSourceRange(SM, firstRange));

    SourceLoc removalStartLoc;
    // For the first argument, start is always next token after `(`.
    if (ArgIdx == 0) {
      removalStartLoc = args->getLParenLoc();
    } else {
      // For all other arguments, start is the next token past
      // the previous argument.
      removalStartLoc = args->getExpr(ArgIdx - 1)->getEndLoc();
    }

    SourceRange removalRange{Lexer::getLocForEndOfToken(SM, removalStartLoc),
                             firstRange.End};

    // Move requires postfix comma only if argument is moved in-between
    // other arguments.
    bool requiresComma =
        !isExpr<BinaryExpr>(anchor) && PrevArgIdx != args->size() - 1;

    diag.fixItRemove(removalRange);
    diag.fixItInsert(secondRange.Start,
                     text.str() + (requiresComma ? ", " : ""));
  };

  // There are 4 diagnostic messages variations depending on
  // labeled/unlabeled arguments.
  if (first.empty() && second.empty()) {
    addFixIts(
        emitDiagnosticAt(diagLoc,
                         isExpr<BinaryExpr>(anchor)
                             ? diag::argument_out_of_order_binary_op
                             : diag::argument_out_of_order_unnamed_unnamed,
                         ArgIdx + 1, PrevArgIdx + 1));
  } else if (first.empty() && !second.empty()) {
    addFixIts(emitDiagnosticAt(diagLoc,
                               diag::argument_out_of_order_unnamed_named,
                               ArgIdx + 1, second));
  } else if (!first.empty() && second.empty()) {
    addFixIts(emitDiagnosticAt(diagLoc,
                               diag::argument_out_of_order_named_unnamed, first,
                               PrevArgIdx + 1));
  } else {
    addFixIts(emitDiagnosticAt(diagLoc, diag::argument_out_of_order_named_named,
                               first, second));
  }
  return true;
}

SourceLoc ExtraneousArgumentsFailure::getLoc() const {
  if (auto *argList = getArgumentListFor(getLocator()))
    return argList->getLoc();
  return FailureDiagnostic::getLoc();
}

bool ExtraneousArgumentsFailure::diagnoseAsError() {
  // Simplified anchor would point directly to the
  // argument in case of contextual mismatch.
  auto anchor = getAnchor();

  if (auto *closure = getAsExpr<ClosureExpr>(anchor)) {
    auto fnType = ContextualType;
    auto params = closure->getParameters();

    auto diag = emitDiagnosticAt(
        params->getStartLoc(), diag::closure_argument_list_tuple, fnType,
        fnType->getNumParams(), params->size(), (params->size() == 1));

    // Unsed parameter is represented by `_` before `in`.
    bool onlyUnusedParams =
        std::all_of(params->begin(), params->end(),
                    [](ParamDecl *param) { return !param->hasName(); });

    // If closure expects no parameters but N was given,
    // and all of them are unused, let's suggest removing them.
    if (fnType->getNumParams() == 0 && onlyUnusedParams) {
      auto inLoc = closure->getInLoc();
      auto &sourceMgr = getASTContext().SourceMgr;

      if (inLoc.isValid()) {
        diag.fixItRemoveChars(params->getStartLoc(),
                              Lexer::getLocForEndOfToken(sourceMgr, inLoc));
        return true;
      }
    }

    diag.flush();

    // If all of the parameters are anonymous, let's point out references
    // to make it explicit where parameters are used in complex closure body,
    // which helps in situations where braces are missing for potential inner
    // closures e.g.
    //
    // func a(_: () -> Void) {}
    // func b(_: (Int) -> Void) {}
    //
    // a {
    //   ...
    //   b($0.member)
    // }
    //
    // Here `$0` is associated with `a` since braces around `member` reference
    // are missing.
    if (!closure->hasSingleExpressionBody() &&
        llvm::all_of(params->getArray(),
                     [](ParamDecl *P) { return P->isAnonClosureParam(); })) {
      if (auto *body = closure->getBody()) {
        struct ParamRefFinder : public ASTWalker {
          DiagnosticEngine &D;
          ParameterList *Params;

          /// Walk everything in a macro.
          MacroWalking getMacroWalkingBehavior() const override {
            return MacroWalking::ArgumentsAndExpansion;
          }

          ParamRefFinder(DiagnosticEngine &diags, ParameterList *params)
              : D(diags), Params(params) {}

          PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
            if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
              if (llvm::is_contained(Params->getArray(), DRE->getDecl())) {
                auto *P = cast<ParamDecl>(DRE->getDecl());
                D.diagnose(DRE->getLoc(), diag::use_of_anon_closure_param,
                           P->getName());
              }
            }
            return Action::Continue(E);
          }
        };

        ParamRefFinder finder(getASTContext().Diags, params);
        body->walk(finder);
      }
    }

    return true;
  }

  if (isContextualMismatch()) {
    auto *locator = getLocator();
    emitDiagnostic(locator->isLastElement<LocatorPathElt::ContextualType>()
                       ? diag::cannot_convert_initializer_value
                       : diag::cannot_convert_argument_value,
                   getType(anchor), ContextualType);
    return true;
  }

  if (ExtraArgs.size() == 1) {
    return diagnoseSingleExtraArgument();
  }

  if (ContextualType->getNumParams() == 0) {
    if (auto *args = getArgumentListFor(getLocator())) {
      auto paramContext = getParameterContextForDiag(getRawAnchor());
      emitDiagnostic(diag::extra_argument_to_nullary_call,
                     static_cast<unsigned>(paramContext))
          .highlight(args->getSourceRange())
          .fixItRemove(args->getSourceRange());
      return true;
    }
  }

  if (ExtraArgs.size() < 2)
    return false;

  llvm::SmallString<64> positions;
  llvm::raw_svector_ostream OS(positions);

  interleave(
      ExtraArgs,
      [&](const std::pair<unsigned, AnyFunctionType::Param> &arg) {
        OS << "#" << (arg.first + 1);
      },
      [&] { OS << ", "; });

  bool areTrailingClosures = false;
  if (auto *argList = getArgumentListFor(getLocator())) {
    areTrailingClosures = llvm::all_of(ExtraArgs, [&](auto &pair) {
      return argList->isTrailingClosureIndex(pair.first);
    });
  }

  emitDiagnostic(diag::extra_arguments_in_call, areTrailingClosures, OS.str());

  if (auto overload = getCalleeOverloadChoiceIfAvailable(getLocator())) {
    if (auto *decl = overload->choice.getDeclOrNull()) {
      emitDiagnosticAt(decl, diag::decl_declared_here, decl->getName());
    }
  }

  return true;
}

bool ExtraneousArgumentsFailure::diagnoseAsNote() {
  auto overload = getCalleeOverloadChoiceIfAvailable(getLocator());
  if (!(overload && overload->choice.isDecl()))
    return false;

  auto *decl = overload->choice.getDecl();
  auto numArgs = getTotalNumArguments();
  if (isExpr<ClosureExpr>(getAnchor())) {
    emitDiagnosticAt(decl, diag::candidate_with_extraneous_args_closure,
                     ContextualType, ContextualType->getNumParams(), numArgs,
                     (numArgs == 1));
  } else {
    emitDiagnosticAt(decl, diag::candidate_with_extraneous_args,
                     overload->adjustedOpenedType, numArgs, ContextualType,
                     ContextualType->getNumParams());
  }
  return true;
}

bool ExtraneousArgumentsFailure::diagnoseSingleExtraArgument() const {
  auto *locator = getLocator();
  auto paramContext = getParameterContextForDiag(getRawAnchor());

  // This specifically handles a case of `Void(...)` which generates
  // constraints differently from other constructor invocations and
  // wouldn't have `ApplyArgument` as a last element in the locator.
  if (auto *call = getAsExpr<CallExpr>(getRawAnchor())) {
    auto *TE = dyn_cast<TypeExpr>(call->getFn());
    if (TE && getType(TE)->getMetatypeInstanceType()->isVoid()) {
      emitDiagnosticAt(call->getLoc(), diag::extra_argument_to_nullary_call,
                       static_cast<unsigned>(paramContext))
          .highlight(call->getArgs()->getSourceRange());
      return true;
    }
  }

  auto *arguments = getArgumentListFor(locator);
  if (!arguments)
    return false;

  const auto &e = ExtraArgs.front();
  auto index = e.first;
  auto argument = e.second;
  auto *argExpr = arguments->getExpr(index);
  auto loc = argExpr->getLoc();
  if (arguments->isTrailingClosureIndex(index)) {
    emitDiagnosticAt(
        loc, diag::extra_trailing_closure_in_call,
        static_cast<unsigned>(paramContext)
    ).highlight(argExpr->getSourceRange());
  } else if (ContextualType->getNumParams() == 0) {
    auto *subExpr = arguments->getUnlabeledUnaryExpr();
    if (subExpr && argument.getPlainType()->isVoid()) {
      emitDiagnosticAt(loc, diag::extra_argument_to_nullary_call,
                       static_cast<unsigned>(paramContext))
          .fixItRemove(subExpr->getSourceRange());
    } else {
      emitDiagnosticAt(loc, diag::extra_argument_to_nullary_call,
                       static_cast<unsigned>(paramContext))
          .highlight(arguments->getSourceRange());
    }
  } else if (argument.hasLabel()) {
    emitDiagnosticAt(loc, diag::extra_argument_named, argument.getLabel(),
                     static_cast<unsigned>(paramContext))
        .highlight(arguments->getSourceRange());
  } else {
    emitDiagnosticAt(loc, diag::extra_argument_positional,
                     static_cast<unsigned>(paramContext))
        .highlight(arguments->getSourceRange());
  }
  return true;
}

bool InaccessibleMemberFailure::diagnoseAsError() {
  auto anchor = getRawAnchor();
  // Let's try to avoid over-diagnosing chains of inaccessible
  // members e.g.:
  //
  // struct A {
  //   struct B {
  //     struct C {}
  //   }
  // }
  //
  // _ = A.B.C()
  //
  // We'll have a fix for each `B', `C` and `C.init` but it makes
  // sense to diagnose only `B` and consider the rest hidden.
  Expr *baseExpr = nullptr;
  DeclNameLoc nameLoc;
  if (auto *UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
    baseExpr = UDE->getBase();
    nameLoc = UDE->getNameLoc();
  } else if (auto *UME = getAsExpr<UnresolvedMemberExpr>(anchor)) {
    nameLoc = UME->getNameLoc();
  } else if (auto *SE = getAsExpr<SubscriptExpr>(anchor)) {
    baseExpr = SE->getBase();
  } else if (auto *call = getAsExpr<CallExpr>(anchor)) {
    baseExpr = call->getFn();
  }

  if (baseExpr) {
    auto *locator = getConstraintLocator(baseExpr, ConstraintLocator::Member);
    const auto &solution = getSolution();
    if (llvm::any_of(solution.Fixes, [&locator](const ConstraintFix *fix) {
          return fix->getLocator() == locator;
        }))
      return false;
  }

  auto loc = nameLoc.isValid() ? nameLoc.getStartLoc() : ::getLoc(anchor);
  auto accessLevel = Member->getFormalAccessScope().accessLevelForDiagnostics();
  if (auto *CD = dyn_cast<ConstructorDecl>(Member)) {
    emitDiagnosticAt(loc, diag::init_candidate_inaccessible,
                     CD->getResultInterfaceType(), accessLevel)
        .highlight(nameLoc.getSourceRange());
  } else {
    emitDiagnosticAt(loc, diag::candidate_inaccessible, Member->getBaseName(),
                     accessLevel)
        .highlight(nameLoc.getSourceRange());
  }

  emitDiagnosticAt(Member, diag::decl_declared_here, Member->getName());
  return true;
}

SourceLoc AnyObjectKeyPathRootFailure::getLoc() const {
  auto anchor = getAnchor();

  if (auto *KPE = getAsExpr<KeyPathExpr>(anchor)) {
    if (auto rootTyRepr = KPE->getRootType())
      return rootTyRepr->getLoc();
  }

  return ::getLoc(anchor);
}

SourceRange AnyObjectKeyPathRootFailure::getSourceRange() const {
  auto anchor = getAnchor();

  if (auto *KPE = getAsExpr<KeyPathExpr>(anchor)) {
    if (auto rootTyRepr = KPE->getRootType())
      return rootTyRepr->getSourceRange();
  }

  return ::getSourceRange(anchor);
}

bool AnyObjectKeyPathRootFailure::diagnoseAsError() {
  // Diagnose use of AnyObject as root for a keypath
  emitDiagnostic(diag::expr_swift_keypath_anyobject_root)
      .highlight(getSourceRange());
  return true;
}

SourceLoc KeyPathSubscriptIndexHashableFailure::getLoc() const {
  auto *locator = getLocator();

  if (locator->isKeyPathSubscriptComponent()) {
    auto *KPE = castToExpr<KeyPathExpr>(getAnchor());
    if (auto kpElt = locator->findFirst<LocatorPathElt::KeyPathComponent>())
      return KPE->getComponents()[kpElt->getIndex()].getLoc();
  }

  return FailureDiagnostic::getLoc();
}

bool KeyPathSubscriptIndexHashableFailure::diagnoseAsError() {
  emitDiagnostic(diag::expr_keypath_subscript_index_not_hashable,
                 resolveType(NonConformingType));
  return true;
}

SourceLoc InvalidMemberRefInKeyPath::getLoc() const {
  auto anchor = getRawAnchor();

  if (auto *KPE = getAsExpr<KeyPathExpr>(anchor)) {
    auto *locator = getLocator();
    auto component = locator->findFirst<LocatorPathElt::KeyPathComponent>();
    assert(component);
    return KPE->getComponents()[component->getIndex()].getLoc();
  }

  return ::getLoc(anchor);
}

bool InvalidStaticMemberRefInKeyPath::diagnoseAsError() {
  emitDiagnostic(diag::expr_keypath_static_member, getName(),
                 isForKeyPathDynamicMemberLookup());
  return true;
}

bool InvalidEnumCaseRefInKeyPath::diagnoseAsError() {
  emitDiagnostic(diag::expr_keypath_enum_case, getName(),
                 isForKeyPathDynamicMemberLookup());
  return true;
}

bool InvalidMemberWithMutatingGetterInKeyPath::diagnoseAsError() {
  emitDiagnostic(diag::expr_keypath_mutating_getter, getName(),
                 isForKeyPathDynamicMemberLookup());
  return true;
}

bool InvalidMethodRefInKeyPath::diagnoseAsError() {
  emitDiagnostic(diag::expr_keypath_not_property, getKind(), getName(),
                 isForKeyPathDynamicMemberLookup());
  return true;
}

SourceLoc InvalidUseOfAddressOf::getLoc() const {
  auto anchor = getAnchor();

  if (auto *assign = getAsExpr<AssignExpr>(anchor))
    return assign->getSrc()->getLoc();

  return ::getLoc(anchor);
}

bool InvalidUseOfAddressOf::diagnoseAsError() {
  if (auto argApplyInfo = getFunctionArgApplyInfo(getLocator())) {
    if (!argApplyInfo->getParameterFlags().isInOut()) {
      emitDiagnostic(diag::extra_address_of, getToType())
          .highlight(getSourceRange())
          .fixItRemove(getSourceRange().Start);
      return true;
    }
  }

  emitDiagnostic(diag::extraneous_address_of);
  return true;
}

bool ExtraneousReturnFailure::diagnoseAsError() {
  emitDiagnostic(diag::cannot_return_value_from_void_func);
  if (auto FD = dyn_cast<FuncDecl>(getDC())) {
    // We only want to emit the note + fix-it if the function does not
    // have an explicit return type. The reason we also need to check
    // whether the parameter list has a valid loc is to guard against
    // cases like 'var foo: () { return 1 }' as here that loc will
    // be invalid. We also need to check that the name is not empty,
    // because certain decls will have empty name (like setters).
    if (FD->getResultTypeRepr() == nullptr &&
        FD->getParameters()->getStartLoc().isValid() &&
        !FD->getBaseIdentifier().empty()) {
      // Insert the fix-it after the parameter list, and after any
      // effects specifiers.
      SourceLoc loc = FD->getParameters()->getEndLoc();
      if (auto asyncLoc = FD->getAsyncLoc())
        loc = asyncLoc;

      if (auto throwsLoc = FD->getThrowsLoc())
        if (throwsLoc.getOpaquePointerValue() > loc.getOpaquePointerValue())
          loc = throwsLoc;

      auto fixItLoc = Lexer::getLocForEndOfToken(getASTContext().SourceMgr, loc);
      emitDiagnostic(diag::add_return_type_note)
          .fixItInsert(fixItLoc, " -> <#Return Type#>");
    }
  }

  return true;
}

bool NotCompileTimeConstFailure::diagnoseAsError() {
  emitDiagnostic(diag::expect_compile_time_const);
  return true;
}

bool NotCopyableFailure::diagnoseAsError() {
  emitDiagnostic(diag::noncopyable_generics, noncopyableTy);
  return true;
}

bool InvalidPackElement::diagnoseAsError() {
  emitDiagnostic(diag::each_non_pack, packElementType);
  return true;
}

bool InvalidPackReference::diagnoseAsError() {
  emitDiagnostic(diag::pack_reference_outside_expansion,
                 packType);
  return true;
}

bool InvalidPackExpansion::diagnoseAsError() {
  auto *locator = getLocator();
  if (locator->isLastElement<LocatorPathElt::ApplyArgToParam>()) {
    if (auto argInfo = getFunctionArgApplyInfo(locator)) {
      emitDiagnostic(diag::invalid_expansion_argument,
                     argInfo->getParamInterfaceType());
      return true;
    }
  }

  emitDiagnostic(diag::expansion_expr_not_allowed);
  return true;
}

bool CollectionElementContextualFailure::diagnoseAsError() {
  auto anchor = getRawAnchor();
  auto *locator = getLocator();

  auto eltType = getFromType();
  auto contextualType = getToType();

  auto diagnoseSingleElement = [&](Diag<Type, Type> msg, Type eltType,
                                   Type contextualType) {
    auto diagnostic = emitDiagnostic(msg, eltType, contextualType);
    (void)trySequenceSubsequenceFixIts(diagnostic);
  };

  auto diagnoseAllOccurrences = [&](Diag<Type, Type> diagnostic) {
    assert(AffectedElements.size() > 1);
    for (auto *element : AffectedElements) {
      emitDiagnosticAt(element->getLoc(), diagnostic, eltType, contextualType);
    }
  };

  if (locator->isForSequenceElementType()) {
    auto purpose = FailureDiagnostic::getContextualTypePurpose(getAnchor());
    // If this is a conversion failure related to binding of `for-each`
    // statement it has to be diagnosed as pattern match if there are
    // holes present in the contextual type.
    if ((purpose == ContextualTypePurpose::CTP_ForEachStmt ||
         purpose == ContextualTypePurpose::CTP_ForEachSequence) &&
        contextualType->hasUnresolvedType()) {
      auto diagnostic = emitDiagnostic(
          (contextualType->is<TupleType>() && !eltType->is<TupleType>())
              ? diag::cannot_match_expr_tuple_pattern_with_nontuple_value
              : diag::cannot_match_unresolved_expr_pattern_with_value,
          eltType);
      (void)trySequenceSubsequenceFixIts(diagnostic);
    } else {
      diagnoseSingleElement(contextualType->isExistentialType()
                                ? diag::cannot_convert_sequence_element_protocol
                                : diag::cannot_convert_sequence_element_value,
                            eltType, contextualType);
    }
    return true;
  }

  auto isFixedToDictionary = [&](ArrayExpr *anchor) {
    return llvm::any_of(getSolution().Fixes, [&](ConstraintFix *fix) {
      auto *fixAnchor = getAsExpr<ArrayExpr>(fix->getAnchor());
      return fixAnchor && fixAnchor == anchor &&
        fix->getKind() == FixKind::TreatArrayLiteralAsDictionary;
    });
  };

  bool treatAsDictionary = false;
  if (auto *AE = getAsExpr<ArrayExpr>(anchor)) {
    if (!(treatAsDictionary = isFixedToDictionary(AE))) {
      if (AffectedElements.size() > 1) {
        diagnoseAllOccurrences(diag::cannot_convert_array_element);
        return true;
      }

      diagnoseSingleElement(diag::cannot_convert_array_element, eltType,
                            contextualType);
      return true;
    }
  }

  if (treatAsDictionary || isExpr<DictionaryExpr>(anchor)) {
    auto eltLoc = locator->castLastElementTo<LocatorPathElt::TupleElement>();
    switch (eltLoc.getIndex()) {
    case 0: { // key
      if (AffectedElements.size() > 1) {
        diagnoseAllOccurrences(diag::cannot_convert_dict_key);
        return true;
      }

      diagnoseSingleElement(diag::cannot_convert_dict_key, eltType,
                            contextualType);
      return true;
    }

    case 1: { // value
      if (AffectedElements.size() > 1) {
        diagnoseAllOccurrences(diag::cannot_convert_dict_value);
        return true;
      }

      diagnoseSingleElement(diag::cannot_convert_dict_value, eltType,
                            contextualType);
      return true;
    }

    default:
      break;
    }
  }

  return false;
}

bool MissingContextualConformanceFailure::diagnoseAsError() {
  auto anchor = getAnchor();
  auto path = getLocator()->getPath();

  Optional<Diag<Type, Type>> diagnostic;
  if (path.empty()) {
    assert(isExpr<AssignExpr>(anchor));
    if (isa<SubscriptExpr>(castToExpr<AssignExpr>(anchor)->getDest())) {
      diagnostic = getDiagnosticFor(CTP_SubscriptAssignSource, getToType());
    } else {
      diagnostic = getDiagnosticFor(CTP_AssignSource, getToType());
    }
  } else {
    const auto &last = path.back();
    switch (last.getKind()) {
    case ConstraintLocator::ContextualType:
      assert(Context != CTP_Unused);
      diagnostic = getDiagnosticFor(Context, getToType());
      break;

    case ConstraintLocator::SequenceElementType: {
      diagnostic = diag::cannot_convert_sequence_element_protocol;
      break;
    }

    default:
      break;
    }
  }

  if (!diagnostic)
    return false;

  auto srcType = getFromType();
  auto dstType = getToType();

  emitDiagnostic(*diagnostic, srcType, dstType);

  if (isExpr<InOutExpr>(anchor))
    return true;

  if (srcType->isAny() && dstType->isAnyObject()) {
    emitDiagnostic(diag::any_as_anyobject_fixit)
        .fixItInsertAfter(getSourceRange().End, " as AnyObject");
  }

  return true;
}

bool MissingGenericArgumentsFailure::hasLoc(GenericTypeParamType *GP) const {
  return GP->getDecl()->getStartLoc().isValid();
}

bool MissingGenericArgumentsFailure::diagnoseAsError() {
  auto locator = getLocator();
  // Opaque result types that could not be inferred from return expressions.
  if (auto opaqueElt =
          locator->findLast<LocatorPathElt::OpenedOpaqueArchetype>()) {
    auto *opaqueDecl = opaqueElt->getDecl();
    emitDiagnostic(diag::cannot_infer_underlying_for_opaque_result,
                   opaqueDecl->getDeclaredInterfaceType());
    return true;
  }

  llvm::SmallDenseMap<TypeRepr *, SmallVector<GenericTypeParamType *, 4>>
      scopedParameters;

  auto isScoped =
      findArgumentLocations([&](TypeRepr *base, GenericTypeParamType *GP) {
        scopedParameters[base].push_back(GP);
      });

  if (!isScoped) {
    auto anchor = getAnchor();
    return diagnoseForAnchor(anchor, Parameters);
  }

  bool diagnosed = false;
  for (const auto &scope : scopedParameters)
    diagnosed |= diagnoseForAnchor(scope.first, scope.second);
  return diagnosed;
}

bool MissingGenericArgumentsFailure::diagnoseForAnchor(
    ASTNode anchor, ArrayRef<GenericTypeParamType *> params) const {
  bool diagnosed = false;
  for (auto *GP : params)
    diagnosed |= diagnoseParameter(anchor, GP);

  if (!diagnosed)
    return false;

  auto *DC = getDeclContext();
  if (!DC)
    return true;

  if (auto *SD = dyn_cast<SubscriptDecl>(DC)) {
    emitDiagnosticAt(SD, diag::note_call_to_subscript, SD->getName());
    return true;
  }

  if (auto *AFD = dyn_cast<AbstractFunctionDecl>(DC)) {
    if (isa<ConstructorDecl>(AFD)) {
      emitDiagnosticAt(AFD, diag::note_call_to_initializer);
    } else {
      emitDiagnosticAt(AFD,
                       AFD->isOperator() ? diag::note_call_to_operator
                                         : diag::note_call_to_func,
                       AFD->getName());
    }
    return true;
  }

  emitGenericSignatureNote(anchor);
  return true;
}

bool MissingGenericArgumentsFailure::diagnoseParameter(
    ASTNode anchor, GenericTypeParamType *GP) const {
  auto &solution = getSolution();
  auto loc = ::getLoc(anchor);

  auto *locator = getLocator();
  // Type variables associated with missing generic parameters are
  // going to be completely cut off from the rest of constraint system,
  // that's why we'd get two fixes in this case which is not ideal.
  if (locator->isForContextualType() &&
      llvm::count_if(solution.DefaultedConstraints,
                     [&GP](const ConstraintLocator *locator) {
                       return locator->getGenericParameter() == GP;
                     }) > 1) {
    return false;
  }

  if (auto *CE = getAsExpr<ExplicitCastExpr>(getRawAnchor())) {
    const auto castTo = getType(CE->getCastTypeRepr());
    auto *NTD = castTo->getAnyNominal();
    emitDiagnosticAt(loc, diag::unbound_generic_parameter_cast, GP,
                     NTD ? NTD->getDeclaredType() : castTo);
  } else {
    emitDiagnosticAt(loc, diag::unbound_generic_parameter, GP);
  }

  Type baseTyForNote;
  auto *DC = getDeclContext();
  if (!DC)
    return true;

  if (!hasLoc(GP))
    return true;

  if (auto *NTD =
          dyn_cast_or_null<NominalTypeDecl>(DC->getSelfNominalTypeDecl())) {
    baseTyForNote = NTD->getDeclaredType();
  } else if (auto *TAD = dyn_cast<TypeAliasDecl>(DC)) {
    baseTyForNote = TAD->getUnboundGenericType();
  } else {
    return true;
  }

  emitDiagnosticAt(GP->getDecl(), diag::archetype_declared_in_type, GP,
                   baseTyForNote);
  return true;
}

void MissingGenericArgumentsFailure::emitGenericSignatureNote(
    ASTNode anchor) const {
  auto &solution = getSolution();
  auto *paramDC = getDeclContext();

  if (!paramDC)
    return;

  auto *GTD = dyn_cast<GenericTypeDecl>(paramDC);
  if (!GTD || anchor.is<Expr *>())
    return;

  auto getParamDecl =
      [](const ConstraintLocator *locator) -> GenericTypeParamDecl * {
    return locator->isForGenericParameter()
               ? locator->getGenericParameter()->getDecl()
               : nullptr;
  };

  llvm::SmallDenseMap<GenericTypeParamDecl *, Type> params;
  for (auto &entry : solution.typeBindings) {
    auto *typeVar = entry.first;

    auto *GP = typeVar->getImpl().getGenericParameter();
    if (!GP)
      continue;

    auto type = resolveType(typeVar);
    assert(!type->is<TypeVariableType>());

    // If this is one of the defaulted parameter types, attempt
    // to emit placeholder for it instead of `Any`.
    if (llvm::any_of(solution.DefaultedConstraints,
                     [&](const ConstraintLocator *locator) {
                       return GP->getDecl() == getParamDecl(locator);
                     }))
      continue;

    params[GP->getDecl()] = type;
  }

  auto getPreferredType = [&](const GenericTypeParamDecl *GP) -> Type {
    auto type = params.find(GP);
    return (type == params.end()) ? Type() : type->second;
  };

  auto baseType = anchor.dyn_cast<TypeRepr *>();
  if (!baseType)
    return;

  SmallString<64> paramsAsString;
  if (TypeChecker::getDefaultGenericArgumentsString(paramsAsString, GTD,
                                                    getPreferredType)) {
    auto diagnostic = emitDiagnosticAt(
        baseType->getLoc(), diag::unbound_generic_parameter_explicit_fix);

    if (auto *genericTy = dyn_cast<GenericIdentTypeRepr>(baseType)) {
      // If some of the eneric arguments have been specified, we need to
      // replace existing signature with a new one.
      diagnostic.fixItReplace(genericTy->getAngleBrackets(), paramsAsString);
    } else {
      // Otherwise we can simply insert new generic signature.
      diagnostic.fixItInsertAfter(baseType->getEndLoc(), paramsAsString);
    }
  }
}

bool MissingGenericArgumentsFailure::findArgumentLocations(
    llvm::function_ref<void(TypeRepr *, GenericTypeParamType *)> callback) {
  using Callback = llvm::function_ref<void(TypeRepr *, GenericTypeParamType *)>;

  auto *const typeRepr = [this]() -> TypeRepr * {
    const auto anchor = getRawAnchor();
    if (const auto *TE = getAsExpr<TypeExpr>(anchor))
      return TE->getTypeRepr();
    else if (const auto *ECE = getAsExpr<ExplicitCastExpr>(anchor))
      return ECE->getCastTypeRepr();
    else
      return nullptr;
  }();

  if (!typeRepr)
    return false;

  struct AssociateMissingParams : public ASTWalker {
    llvm::SmallVector<GenericTypeParamType *, 4> Params;
    Callback Fn;

    AssociateMissingParams(ArrayRef<GenericTypeParamType *> params,
                           Callback callback)
        : Params(params.begin(), params.end()), Fn(callback) {}

    /// Walk everything in a macro.
    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::ArgumentsAndExpansion;
    }

    PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
      if (Params.empty())
        return Action::SkipChildren();

      auto *ident = dyn_cast<IdentTypeRepr>(T);
      if (!ident)
        return Action::Continue();

      auto *decl = dyn_cast_or_null<GenericTypeDecl>(ident->getBoundDecl());
      if (!decl)
        return Action::Continue();

      auto *paramList = decl->getGenericParams();
      if (!paramList)
        return Action::Continue();

      // There could a situation like `S<S>()`, so we need to be
      // careful not to point at first `S` because it has all of
      // its generic parameters specified.
      if (auto *generic = dyn_cast<GenericIdentTypeRepr>(ident)) {
        if (paramList->size() == generic->getNumGenericArgs())
          return Action::Continue();
      }

      for (auto *candidate : paramList->getParams()) {
        auto result =
            llvm::find_if(Params, [&](const GenericTypeParamType *param) {
              return candidate == param->getDecl();
            });

        if (result != Params.end()) {
          Fn(ident, *result);
          Params.erase(result);
        }
      }

      // Keep walking.
      return Action::Continue();
    }

    bool allParamsAssigned() const { return Params.empty(); }

  } associator(Parameters, callback);

  typeRepr->walk(associator);
  return associator.allParamsAssigned();
}

SourceLoc SkipUnhandledConstructInResultBuilderFailure::getLoc() const {
  if (auto stmt = unhandled.dyn_cast<Stmt *>())
    return stmt->getStartLoc();

  return unhandled.get<Decl *>()->getLoc();
}

/// Determine whether the given "if" chain has a missing "else".
static bool hasMissingElseInChain(IfStmt *ifStmt) {
  if (!ifStmt->getElseStmt())
    return true;

  if (auto ifElse = dyn_cast<IfStmt>(ifStmt->getElseStmt()))
    return hasMissingElseInChain(ifElse);

  return false;
}

bool SkipUnhandledConstructInResultBuilderFailure::diagnosePatternBinding(
    PatternBindingDecl *PB) const {
  bool diagnosed = false;

  for (unsigned i : range(PB->getNumPatternEntries())) {
    auto *pattern = PB->getPattern(i);

    // Each variable bound by the pattern must be stored and cannot have
    // observers.
    {
      SmallVector<VarDecl *, 8> variables;
      pattern->collectVariables(variables);

      bool diagnosedStorage = false;
      for (auto *var : variables)
        diagnosedStorage |= diagnoseStorage(var);

      // if storage has been diagnosed, let's move to the next entry.
      if (diagnosedStorage) {
        diagnosed = true;
        continue;
      }
    }

    // Diagnose all of the patterns without explicit initializers.
    if (PB->isExplicitlyInitialized(i))
      continue;

    StringRef name;

    if (auto *TP = dyn_cast<TypedPattern>(pattern)) {
      if (auto *NP = dyn_cast<NamedPattern>(TP->getSubPattern()))
        name = NP->getNameStr();
    }

    emitDiagnosticAt(pattern->getLoc(),
                     diag::result_builder_requires_explicit_var_initialization,
                     !name.empty(), name, builder->getName())
        .fixItInsertAfter(pattern->getEndLoc(), " = <#value#>");

    diagnosed = true;
  }

  return diagnosed;
}

bool SkipUnhandledConstructInResultBuilderFailure::diagnoseStorage(
    VarDecl *var) const {
  enum class PropertyKind : unsigned { lazy, wrapped, computed, observed };

  if (var->getImplInfo().isSimpleStored())
    return false;

  PropertyKind kind;
  if (var->getAttrs().hasAttribute<LazyAttr>()) {
    kind = PropertyKind::lazy;
  } else if (var->hasAttachedPropertyWrapper()) {
    kind = PropertyKind::wrapped;
  } else if (var->hasObservers()) {
    kind = PropertyKind::observed;
  } else {
    kind = PropertyKind::computed;
  }

  emitDiagnosticAt(var, diag::cannot_declare_computed_var_in_result_builder,
                   static_cast<unsigned>(kind));
  return true;
}

void SkipUnhandledConstructInResultBuilderFailure::diagnosePrimary(
    bool asNote) {

  if (auto *decl = unhandled.dyn_cast<Decl *>()) {
    auto *PB = dyn_cast<PatternBindingDecl>(decl);
    if (PB && diagnosePatternBinding(PB))
      return;
  }

  if (auto stmt = unhandled.dyn_cast<Stmt *>()) {
    emitDiagnostic(asNote ? diag::note_result_builder_control_flow
                          : diag::result_builder_control_flow,
                   builder->getName());

    // Emit custom notes to help the user introduce the appropriate 'build'
    // functions.
    SourceLoc buildInsertionLoc;
    std::string stubIndent;
    Type componentType;
    std::tie(buildInsertionLoc, stubIndent, componentType) =
        determineResultBuilderBuildFixItInfo(builder);

    if (buildInsertionLoc.isInvalid()) {
      // Do nothing.
    } else if (isa<IfStmt>(stmt) && hasMissingElseInChain(cast<IfStmt>(stmt))) {
      auto diag = emitDiagnosticAt(
          builder->getLoc(), diag::result_builder_missing_build_optional,
          builder->getDeclaredInterfaceType());

      std::string fixItString;
      {
        llvm::raw_string_ostream out(fixItString);
        printResultBuilderBuildFunction(
            builder, componentType, ResultBuilderBuildFunction::BuildOptional,
            stubIndent, out);
      }

      diag.fixItInsert(buildInsertionLoc, fixItString);
    } else if (isa<SwitchStmt>(stmt) || isa<IfStmt>(stmt)) {
      auto diag = emitDiagnosticAt(
          builder->getLoc(), diag::result_builder_missing_build_either,
          builder->getDeclaredInterfaceType());

      std::string fixItString;
      {
        llvm::raw_string_ostream out(fixItString);
        printResultBuilderBuildFunction(
            builder, componentType,
            ResultBuilderBuildFunction::BuildEitherFirst,
            stubIndent, out);
        out << '\n';
        printResultBuilderBuildFunction(
            builder, componentType,
            ResultBuilderBuildFunction::BuildEitherSecond,
            stubIndent, out);
      }

      diag.fixItInsert(buildInsertionLoc, fixItString);
    } else if (isa<ForEachStmt>(stmt)) {
      auto diag = emitDiagnosticAt(
          builder->getLoc(), diag::result_builder_missing_build_array,
          builder->getDeclaredInterfaceType());

      std::string fixItString;
      {
        llvm::raw_string_ostream out(fixItString);
        printResultBuilderBuildFunction(
            builder, componentType, ResultBuilderBuildFunction::BuildArray,
            stubIndent, out);
      }

      diag.fixItInsert(buildInsertionLoc, fixItString);
    }
  } else {
    emitDiagnostic(asNote ? diag::note_result_builder_decl
                          : diag::result_builder_decl,
                   builder->getName());
  }
}

bool SkipUnhandledConstructInResultBuilderFailure::diagnoseAsError() {
  // Following errors are already diagnosed:
  //  - brace statement - error related to absence of appropriate buildBlock
  //  - switch/case statements - empty body
  if (auto *stmt = unhandled.dyn_cast<Stmt *>()) {
    if (isa<BraceStmt>(stmt))
      return true;

    if (auto *switchStmt = getAsStmt<SwitchStmt>(stmt)) {
      auto caseStmts = switchStmt->getCases();
      if (caseStmts.empty())
        return true;
    }

    // Empty case statements are diagnosed by parser.
    if (auto *caseStmt = getAsStmt<CaseStmt>(stmt)) {
      auto *body = caseStmt->getBody();
      if (body->getNumElements() == 0)
        return true;
    }
  }

  diagnosePrimary(/*asNote=*/false);
  emitDiagnosticAt(builder, diag::kind_declname_declared_here,
                   builder->getDescriptiveKind(), builder->getName());
  return true;
}

bool SkipUnhandledConstructInResultBuilderFailure::diagnoseAsNote() {
  diagnosePrimary(/*asNote=*/true);
  return true;
}

bool MutatingMemberRefOnImmutableBase::diagnoseAsError() {
  auto *anchor = castToExpr(getRawAnchor());
  auto baseExpr = getBaseExprFor(anchor);
  if (!baseExpr)
    return false;

  auto diagIDsubelt = diag::cannot_pass_rvalue_mutating_subelement;
  auto diagIDmember = diag::cannot_pass_rvalue_mutating;

  if (auto *storage = dyn_cast<AbstractStorageDecl>(Member)) {
    if (storage->isGetterMutating()) {
      diagIDsubelt = diag::cannot_pass_rvalue_mutating_getter_subelement;
      diagIDmember = diag::cannot_pass_rvalue_mutating_getter;
    }
  }

  const auto &solution = getSolution();
  AssignmentFailure failure(baseExpr, solution, anchor->getLoc(), diagIDsubelt,
                            diagIDmember);
  return failure.diagnoseAsError();
}

bool InvalidTupleSplatWithSingleParameterFailure::diagnoseAsError() {
  auto selectedOverload = getCalleeOverloadChoiceIfAvailable(getLocator());
  if (!selectedOverload || !selectedOverload->choice.isDecl())
    return false;

  auto *choice = selectedOverload->choice.getDecl();

  auto *args = getArgumentListFor(getLocator());
  if (!args)
    return false;

  using Substitution = std::pair<GenericTypeParamType *, Type>;
  llvm::SmallVector<Substitution, 8> substitutions;

  auto paramTy = restoreGenericParameters(
      ParamType, [&](GenericTypeParamType *GP, Type resolvedType) {
        substitutions.push_back(std::make_pair(GP, resolvedType));
      });

  DeclBaseName name = choice->getBaseName();

  std::string subsStr;
  if (!substitutions.empty()) {
    llvm::array_pod_sort(
        substitutions.begin(), substitutions.end(),
        [](const std::pair<GenericTypeParamType *, Type> *lhs,
           const std::pair<GenericTypeParamType *, Type> *rhs) -> int {
          GenericParamKey key1(lhs->first);
          GenericParamKey key2(rhs->first);
          return key1 < key2 ? -1 : (key1 == key2) ? 0 : 1;
        });

    subsStr += " [with ";
    interleave(
        substitutions,
        [&subsStr](const Substitution &substitution) {
          subsStr += substitution.first->getString();
          subsStr += " = ";
          subsStr += substitution.second->getString();
        },
        [&subsStr] { subsStr += ", "; });
    subsStr += ']';
  }

  auto diagnostic =
      name.isSpecial()
          ? emitDiagnosticAt(args->getLoc(),
                             diag::single_tuple_parameter_mismatch_special,
                             choice->getDescriptiveKind(), paramTy, subsStr)
          : emitDiagnosticAt(
                args->getLoc(), diag::single_tuple_parameter_mismatch_normal,
                choice->getDescriptiveKind(), name, paramTy, subsStr);

  auto newLeftParenLoc = args->getStartLoc();
  auto firstArgLabel = args->getLabel(0);

  // Cover situations like:
  //
  // func foo(x: (Int, Int)) {}
  // foo(x: 0, 1)
  //
  // Where left paren should be suggested after the label,
  // since the label belongs to the parameter itself.
  if (!firstArgLabel.empty()) {
    auto paramTuple = resolveType(ParamType)->castTo<TupleType>();
    // If the label of the first argument matches the one required
    // by the parameter it would be omitted from the fixed parameter type.
    if (!paramTuple->getElement(0).hasName())
      newLeftParenLoc = Lexer::getLocForEndOfToken(getASTContext().SourceMgr,
                                                   args->getLabelLoc(0));
  }

  diagnostic.highlight(args->getSourceRange())
      .fixItInsertAfter(newLeftParenLoc, "(")
      .fixItInsert(args->getEndLoc(), ")");

  return true;
}

bool ThrowingFunctionConversionFailure::diagnoseAsError() {
  emitDiagnostic(diag::throws_functiontype_mismatch, getFromType(),
                 getToType());
  return true;
}

bool AsyncFunctionConversionFailure::diagnoseAsError() {
  auto *locator = getLocator();

  if (locator->isLastElement<LocatorPathElt::ApplyArgToParam>()) {
    emitDiagnostic(diag::cannot_pass_async_func_to_sync_parameter,
                   getFromType());

    if (auto *closure = getAsExpr<ClosureExpr>(getAnchor())) {
      auto asyncLoc = closure->getAsyncLoc();

      // 'async' effect is inferred from the body of the closure.
      if (asyncLoc.isInvalid()) {
        if (auto asyncNode = findAsyncNode(closure)) {
          emitDiagnosticAt(::getLoc(asyncNode),
                           diag::async_inferred_from_operation);
        }
      }
    }

    return true;
  }

  emitDiagnostic(diag::async_functiontype_mismatch, getFromType(),
                 getToType());
  return true;
}

bool InOutConversionFailure::diagnoseAsError() {
  auto *locator = getLocator();
  auto path = locator->getPath();

  if (!path.empty() &&
      path.back().getKind() == ConstraintLocator::FunctionArgument) {
    if (auto argApplyInfo = getFunctionArgApplyInfo(locator)) {
      emitDiagnostic(diag::cannot_convert_argument_value,
                     argApplyInfo->getArgType(), argApplyInfo->getParamType());
    } else {
      assert(locator->findLast<LocatorPathElt::ContextualType>());
      auto anchor = getAnchor();
      auto contextualType = getContextualType(anchor);
      auto purpose = getContextualTypePurpose();
      auto diagnostic = getDiagnosticFor(purpose, contextualType);

      if (!diagnostic)
        return false;

      emitDiagnostic(*diagnostic, getType(anchor), contextualType);
    }

    return true;
  }

  emitDiagnostic(diag::cannot_pass_rvalue_inout_converted, getFromType(),
                 getToType());
  fixItChangeArgumentType();
  return true;
}

void InOutConversionFailure::fixItChangeArgumentType() const {
  auto *argExpr = castToExpr(getAnchor());
  auto *DC = getDC();

  if (auto *IOE = dyn_cast<InOutExpr>(argExpr))
    argExpr = IOE->getSubExpr();

  auto *DRE = dyn_cast<DeclRefExpr>(argExpr);
  if (!DRE)
    return;

  auto *VD = dyn_cast_or_null<VarDecl>(DRE->getDecl());
  if (!VD)
    return;

  // Don't emit for non-local variables.
  // (But in script-mode files, we consider module-scoped
  // variables in the same file to be local variables.)
  auto VDC = VD->getDeclContext();
  bool isLocalVar = VDC->isLocalContext();
  if (!isLocalVar && VDC->isModuleScopeContext()) {
    auto argFile = DC->getParentSourceFile();
    auto varFile = VDC->getParentSourceFile();
    isLocalVar = (argFile == varFile && argFile->isScriptMode());
  }
  if (!isLocalVar)
    return;

  auto actualType = getFromType();
  auto neededType = getToType();

  SmallString<32> scratch;
  SourceLoc endLoc;   // Filled in if we decide to diagnose this
  SourceLoc startLoc; // Left invalid if we're inserting

  auto isSimpleTypelessPattern = [](Pattern *P) -> bool {
    if (auto VP = dyn_cast_or_null<BindingPattern>(P))
      P = VP->getSubPattern();
    return P && isa<NamedPattern>(P);
  };

  auto typeRange = VD->getTypeSourceRangeForDiagnostics();
  if (typeRange.isValid()) {
    startLoc = typeRange.Start;
    endLoc = typeRange.End;
  } else if (isSimpleTypelessPattern(VD->getParentPattern())) {
    endLoc = VD->getNameLoc();
    scratch += ": ";
  }

  if (endLoc.isInvalid())
    return;

  scratch += neededType.getString();

  // Adjust into the location where we actually want to insert
  endLoc = Lexer::getLocForEndOfToken(getASTContext().SourceMgr, endLoc);

  // Since we already adjusted endLoc, this will turn an insertion
  // into a zero-character replacement.
  if (!startLoc.isValid())
    startLoc = endLoc;

  emitDiagnosticAt(VD, diag::inout_change_var_type_if_possible, actualType,
                   neededType)
      .fixItReplaceChars(startLoc, endLoc, scratch);
}

bool ArgumentMismatchFailure::diagnoseAsError() {
  const auto paramType = getToType();

  // If the parameter type contains an opened archetype, it's an unsupported
  // existential member access; refrain from exposing type system implementation
  // details in diagnostics and complaining about a parameter the user cannot
  // fulfill, and let the member access failure prevail.
  if (paramType->hasOpenedExistential()) {
    return false;
  }

  if (diagnoseMisplacedMissingArgument())
    return true;

  if (diagnoseConversionToBool())
    return true;

  if (diagnoseArchetypeMismatch())
    return true;

  if (diagnosePatternMatchingMismatch())
    return true;

  if (diagnoseUseOfReferenceEqualityOperator())
    return true;

  if (diagnosePropertyWrapperMismatch())
    return true;

  if (diagnoseAttemptedRegexBuilder())
    return true;

  if (diagnoseTrailingClosureMismatch())
    return true;

  if (diagnoseKeyPathAsFunctionResultMismatch())
    return true;

  auto argType = getFromType();

  if (paramType->isAnyObject()) {
    emitDiagnostic(diag::cannot_convert_argument_value_anyobject, argType,
                   paramType);
    return true;
  }

  Diag<Type, Type> diagnostic = diag::cannot_convert_argument_value;

  // If parameter type is a protocol value, let's says that
  // argument doesn't conform to a give protocol.
  if (paramType->isExistentialType())
    diagnostic = diag::cannot_convert_argument_value_protocol;

  auto diag = emitDiagnostic(diagnostic, argType, paramType);

  // If argument is an l-value type and parameter is a pointer type,
  // let's match up its element type to the argument to see whether
  // it would be appropriate to suggest adding `&`.
  auto argument = getAnchor();
  if (getType(argument, /*wantRValue=*/false)->is<LValueType>()) {
    auto elementTy = paramType->getAnyPointerElementType();
    if (elementTy && argType->isEqual(elementTy)) {
      diag.fixItInsert(::getSourceRange(argument).Start, "&");
      return true;
    }
  }

  tryFixIts(diag);
  return true;
}

bool ArgumentMismatchFailure::diagnoseAsNote() {
  auto *locator = getLocator();
  if (auto *callee = getCallee()) {
    emitDiagnosticAt(callee, diag::candidate_has_invalid_argument_at_position,
                     getToType(), getParamPosition(),
                     locator->isLastElement<LocatorPathElt::LValueConversion>(),
                     getFromType());
    return true;
  }

  return false;
}

bool ArgumentMismatchFailure::diagnoseUseOfReferenceEqualityOperator() const {
  auto *locator = getLocator();

  if (!isArgumentOfReferenceEqualityOperator(locator))
    return false;

  auto *binaryOp = castToExpr<BinaryExpr>(getRawAnchor());
  auto *lhs = binaryOp->getLHS();
  auto *rhs = binaryOp->getRHS();

  auto name = *getOperatorName(binaryOp->getFn());

  auto lhsType = getType(lhs);
  auto rhsType = getType(rhs);

  // If both arguments where incorrect e.g. both are function types,
  // let's avoid producing a diagnostic second time, because first
  // one would cover both arguments.
  if (getAsExpr(getAnchor()) == rhs && rhsType->is<FunctionType>()) {
    auto *argLoc = getConstraintLocator(
        binaryOp,
        {ConstraintLocator::ApplyArgument,
         LocatorPathElt::ApplyArgToParam(0, 0, getParameterFlagsAtIndex(0))});

    if (llvm::any_of(getSolution().Fixes, [&argLoc](const ConstraintFix *fix) {
          return fix->getLocator() == argLoc;
        }))
      return true;
  }

  // Regardless of whether the type has reference or value semantics,
  // comparison with nil is illegal, albeit for different reasons spelled
  // out by the diagnosis.
  if (isa<NilLiteralExpr>(lhs) || isa<NilLiteralExpr>(rhs)) {
    std::string revisedName = std::string(name);
    revisedName.pop_back();

    auto loc = binaryOp->getLoc();
    auto nonNilType = isa<NilLiteralExpr>(lhs) ? rhsType : lhsType;
    auto nonNilExpr = isa<NilLiteralExpr>(lhs) ? rhs : lhs;

    // If we made it here, then we're trying to perform a comparison with
    // reference semantics rather than value semantics. The fixit will
    // lop off the extra '=' in the operator.
    if (nonNilType->getOptionalObjectType()) {
      emitDiagnosticAt(
          loc, diag::value_type_comparison_with_nil_illegal_did_you_mean,
          nonNilType)
          .fixItReplace(loc, revisedName);
    } else {
      emitDiagnosticAt(loc, diag::value_type_comparison_with_nil_illegal,
                       nonNilType)
          .highlight(nonNilExpr->getSourceRange());
    }

    return true;
  }

  if (lhsType->is<FunctionType>() || rhsType->is<FunctionType>()) {
    emitDiagnosticAt(binaryOp->getLoc(), diag::cannot_reference_compare_types,
                     name.str(), lhsType, rhsType)
        .highlight(lhs->getSourceRange())
        .highlight(rhs->getSourceRange());
    return true;
  }

  return false;
}

bool ArgumentMismatchFailure::diagnosePatternMatchingMismatch() const {
  if (!isArgumentOfPatternMatchingOperator(getLocator()))
    return false;

  auto *op = castToExpr<BinaryExpr>(getRawAnchor());
  auto *lhsExpr = op->getLHS();
  auto *rhsExpr = op->getRHS();

  auto lhsType = getType(lhsExpr);
  auto rhsType = getType(rhsExpr);

  auto diagnostic =
      lhsType->is<UnresolvedType>()
          ? emitDiagnostic(
                diag::cannot_match_unresolved_expr_pattern_with_value, rhsType)
          : emitDiagnostic(diag::cannot_match_expr_pattern_with_value, lhsType,
                           rhsType);

  diagnostic.highlight(lhsExpr->getSourceRange());
  diagnostic.highlight(rhsExpr->getSourceRange());

  if (auto optUnwrappedType = rhsType->getOptionalObjectType()) {
    if (lhsType->isEqual(optUnwrappedType)) {
      diagnostic.fixItInsertAfter(lhsExpr->getEndLoc(), "?");
    }
  }

  return true;
}

bool ArgumentMismatchFailure::diagnoseArchetypeMismatch() const {
  auto *argTy = getFromType()->getAs<ArchetypeType>();
  auto *paramTy = getToType()->getAs<ArchetypeType>();

  if (!(argTy && paramTy))
    return false;

  // Produce this diagnostic only if the names
  // of the generic parameters are the same.
  if (argTy->getName() != paramTy->getName())
    return false;

  auto getGenericTypeDecl = [&](ArchetypeType *archetype) -> ValueDecl * {
    auto paramType = archetype->getInterfaceType();

    if (auto *GTPT = paramType->getAs<GenericTypeParamType>())
      return GTPT->getDecl();

    if (auto *DMT = paramType->getAs<DependentMemberType>())
      return DMT->getAssocType();

    return nullptr;
  };

  auto *argDecl = getGenericTypeDecl(argTy);
  auto *paramDecl = getGenericTypeDecl(paramTy);

  if (!(paramDecl && argDecl))
    return false;

  emitDiagnostic(diag::cannot_convert_argument_value_generic, argTy,
                 describeGenericType(argDecl), paramTy,
                 describeGenericType(paramDecl));

  emitDiagnosticAt(argDecl, diag::descriptive_generic_type_declared_here,
                   describeGenericType(argDecl, true));

  emitDiagnosticAt(paramDecl, diag::descriptive_generic_type_declared_here,
                   describeGenericType(paramDecl, true));

  return true;
}

bool ArgumentMismatchFailure::diagnoseMisplacedMissingArgument() const {
  const auto &solution = getSolution();
  auto *locator = getLocator();

  if (!MissingArgumentsFailure::isMisplacedMissingArgument(solution, locator))
    return false;

  // Assign new type variable to a type of a parameter.
  auto *fnType = getFnType();
  const auto &param = fnType->getParams()[0];

  auto anchor = getRawAnchor();

  MissingArgumentsFailure failure(
      solution, {SynthesizedArg{0, param}},
      getConstraintLocator(anchor, ConstraintLocator::ApplyArgument));

  return failure.diagnoseSingleMissingArgument();
}

bool ArgumentMismatchFailure::diagnosePropertyWrapperMismatch() const {
  auto argType = getFromType();
  auto paramType = getToType();

  // Verify that this is an implicit call to a property wrapper initializer
  // in a form of `init(wrappedValue:)` or deprecated `init(initialValue:)`.
  auto *call = getAsExpr<CallExpr>(getRawAnchor());
  if (!(call && call->isImplicit() && isa<TypeExpr>(call->getFn())))
    return false;

  auto *args = call->getArgs();
  auto *argExpr = args->getUnaryExpr();
  if (!argExpr)
    return false;

  if (args->getLabel(0) != getASTContext().Id_wrappedValue &&
      args->getLabel(0) != getASTContext().Id_initialValue)
    return false;

  // If this is an attempt to initialize property wrapper with opaque value
  // of error type, let's just ignore that problem since original mismatch
  // has been diagnosed already.
  if (argExpr->isImplicit() && isa<OpaqueValueExpr>(argExpr) &&
      argType->is<ErrorType>())
    return true;

  emitDiagnostic(diag::cannot_convert_initializer_value, argType, paramType);
  return true;
}

/// Add a fix-it to insert an import of a module.
static void fixItImport(InFlightDiagnostic &diag, Identifier moduleName,
                        DeclContext *dc) {
  auto *SF = dc->getParentSourceFile();
  if (!SF)
    return;

  SourceLoc insertLoc;
  bool isTrailing = true;

  // Check if we can insert as the last import statement.
  auto decls = SF->getTopLevelDecls();
  for (auto *decl : decls) {
    auto *importDecl = dyn_cast<ImportDecl>(decl);
    if (!importDecl) {
      if (insertLoc.isValid())
        break;
      continue;
    }
    insertLoc = importDecl->getEndLoc();
  }

  // If not, insert it as the first decl with a valid source location.
  if (insertLoc.isInvalid()) {
    for (auto *decl : decls) {
      if (auto loc = decl->getStartLoc()) {
        insertLoc = loc;
        isTrailing = false;
        break;
      }
    }
  }

  // If we didn't resolve to a valid location, give up.
  if (insertLoc.isInvalid())
    return;

  SmallString<32> insertText;
  if (isTrailing) {
    insertText.append("\n");
  }
  insertText.append("import ");
  insertText.append(moduleName.str());
  if (isTrailing) {
    diag.fixItInsertAfter(insertLoc, insertText);
  } else {
    insertText.append("\n\n");
    diag.fixItInsert(insertLoc, insertText);
  }
}

bool ArgumentMismatchFailure::diagnoseAttemptedRegexBuilder() const {
  auto &ctx = getASTContext();

  // Should be a lone trailing closure argument.
  if (!Info.isTrailingClosure() || !Info.getArgList()->isUnary())
    return false;

  // Check if this an application of a Regex initializer, and the user has not
  // imported RegexBuilder.
  auto *ctor = dyn_cast_or_null<ConstructorDecl>(getCallee());
  if (!ctor)
    return false;

  auto *ctorDC = ctor->getInnermostTypeContext();
  if (!ctorDC || ctorDC->getSelfNominalTypeDecl() != ctx.getRegexDecl())
    return false;

  // If the RegexBuilder module is loaded, make sure it hasn't been imported.
  // Note this will cause us to diagnose even if another SourceFile has
  // imported RegexBuilder, and its extensions have leaked into this file. This
  // is a longstanding lookup bug, and it's probably a good idea to suggest
  // explicitly importing RegexBuilder regardless in that case.
  if (auto *regexBuilderModule = ctx.getLoadedModule(ctx.Id_RegexBuilder)) {
    auto &importCache = getASTContext().getImportCache();
    if (importCache.isImportedBy(regexBuilderModule, getDC()))
      return false;
  }

  // Suggest importing RegexBuilder.
  auto diag = emitDiagnostic(diag::must_import_regex_builder_module);
  fixItImport(diag, ctx.Id_RegexBuilder, getDC());
  return true;
}

bool ArgumentMismatchFailure::diagnoseTrailingClosureMismatch() const {
  if (!Info.isTrailingClosure())
    return false;

  auto paramType = getToType();
  if (paramType->lookThroughAllOptionalTypes()->is<AnyFunctionType>())
    return false;

  emitDiagnostic(diag::trailing_closure_bad_param, paramType)
      .highlight(getSourceRange());

  if (auto overload = getCalleeOverloadChoiceIfAvailable(getLocator())) {
    if (auto *decl = overload->choice.getDeclOrNull()) {
      emitDiagnosticAt(decl, diag::decl_declared_here, decl->getName());
    }
  }

  return true;
}

bool ArgumentMismatchFailure::diagnoseKeyPathAsFunctionResultMismatch() const {
  auto argExpr = getArgExpr();
  if (!isExpr<KeyPathExpr>(argExpr))
    return false;

  auto argType = getFromType();
  auto paramType = getToType();

  if (!isKnownKeyPathType(argType))
    return false;

  auto kpType = argType->castTo<BoundGenericType>();
  auto kpRootType = kpType->getGenericArgs()[0];
  auto kpValueType = kpType->getGenericArgs()[1];

  auto paramFnType = paramType->getAs<FunctionType>();
  if (!(paramFnType && paramFnType->getNumParams() == 1 &&
        paramFnType->getParams().front().getPlainType()->isEqual(kpRootType)))
    return false;

  emitDiagnostic(diag::expr_smart_keypath_value_covert_to_contextual_type,
                 kpValueType, paramFnType->getResult());
  return true;
}

void ExpandArrayIntoVarargsFailure::tryDropArrayBracketsFixIt(
    const Expr *anchor) const {
  // If this is an array literal, offer to remove the brackets and pass the
  // elements directly as variadic arguments.
  if (auto *arrayExpr = dyn_cast<ArrayExpr>(anchor)) {
    auto diag = emitDiagnosticAt(arrayExpr->getLoc(),
                                 diag::suggest_pass_elements_directly);
    diag.fixItRemove(arrayExpr->getLBracketLoc())
        .fixItRemove(arrayExpr->getRBracketLoc());
    // Handle the case where the array literal has a trailing comma.
    if (arrayExpr->getNumCommas() == arrayExpr->getNumElements())
      diag.fixItRemove(arrayExpr->getCommaLocs().back());
  }
}

bool ExpandArrayIntoVarargsFailure::diagnoseAsError() {
  if (auto *anchor = getAsExpr(getAnchor())) {
    emitDiagnostic(diag::cannot_convert_array_to_variadic, getFromType(),
                   getToType());
    tryDropArrayBracketsFixIt(anchor);
    // TODO: Array splat fix-it once that's supported.
    return true;
  }
  return false;
}

bool ExpandArrayIntoVarargsFailure::diagnoseAsNote() {
  auto overload = getCalleeOverloadChoiceIfAvailable(getLocator());
  auto *anchor = getAsExpr(getAnchor());
  if (!overload || !anchor)
    return false;

  if (auto chosenDecl = overload->choice.getDeclOrNull()) {
    emitDiagnosticAt(chosenDecl, diag::candidate_would_match_array_to_variadic,
                     getToType());
    tryDropArrayBracketsFixIt(anchor);
    return true;
  }
  return false;
}

bool ExtraneousCallFailure::diagnoseAsError() {
  auto anchor = getAnchor();
  auto *locator = getLocator();

  // If this is something like `foo()` where `foo` is a variable
  // or a property, let's suggest dropping `()`.
  auto removeParensFixIt = [&](InFlightDiagnostic &diagnostic) {
    auto *argLoc =
        getConstraintLocator(getRawAnchor(), ConstraintLocator::ApplyArgument);
    auto *argList = getArgumentListFor(argLoc);
    if (argList && argList->empty())
      diagnostic.fixItRemove(argList->getSourceRange());
  };

  if (auto overload = getCalleeOverloadChoiceIfAvailable(locator)) {
    if (auto *decl = overload->choice.getDeclOrNull()) {
      if (auto *enumCase = dyn_cast<EnumElementDecl>(decl)) {
        auto diagnostic =
            emitDiagnostic(diag::unexpected_arguments_in_enum_case,
                           enumCase->getBaseIdentifier());
        removeParensFixIt(diagnostic);
        return true;
      }
    }
  }

  auto diagnostic =
      emitDiagnostic(diag::cannot_call_non_function_value, getType(anchor));
  removeParensFixIt(diagnostic);
  return true;
}

void NonEphemeralConversionFailure::emitSuggestionNotes() const {
  auto getPointerKind = [](Type ty) -> PointerTypeKind {
    PointerTypeKind pointerKind;
    auto pointeeType = ty->lookThroughSingleOptionalType()
                         ->getAnyPointerElementType(pointerKind);
    assert(pointeeType && "Expected a pointer!");
    (void)pointeeType;

    return pointerKind;
  };

  // This must stay in sync with diag::ephemeral_use_array_with_unsafe_buffer
  // and diag::ephemeral_use_with_unsafe_pointer.
  enum AlternativeKind {
    AK_Raw = 0,
    AK_MutableRaw,
    AK_Typed,
    AK_MutableTyped,
  };

  auto getAlternativeKind = [&]() -> Optional<AlternativeKind> {
    switch (getPointerKind(getParamType())) {
    case PTK_UnsafeRawPointer:
      return AK_Raw;
    case PTK_UnsafeMutableRawPointer:
      return AK_MutableRaw;
    case PTK_UnsafePointer:
      return AK_Typed;
    case PTK_UnsafeMutablePointer:
      return AK_MutableTyped;
    case PTK_AutoreleasingUnsafeMutablePointer:
      return None;
    }
    llvm_unreachable("invalid pointer kind");
  };

  // First emit a note about the implicit conversion only lasting for the
  // duration of the call.
  auto *argExpr = getArgExpr();
  emitDiagnosticAt(
      argExpr->getLoc(), diag::ephemeral_pointer_argument_conversion_note,
      getArgType(), getParamType(), getCallee(), getCalleeFullName())
      .highlight(argExpr->getSourceRange());

  // Then try to find a suitable alternative.
  switch (ConversionKind) {
  case ConversionRestrictionKind::ArrayToPointer:
  case ConversionRestrictionKind::ArrayToCPointer: {
    // Don't suggest anything for optional arrays, as there's currently no
    // direct alternative.
    if (getArgType()->getOptionalObjectType())
      break;

    // We can suggest using withUnsafe[Mutable][Bytes/BufferPointer].
    if (auto alternative = getAlternativeKind())
      emitDiagnosticAt(argExpr->getLoc(),
                       diag::ephemeral_use_array_with_unsafe_buffer,
                       *alternative);
    break;
  }
  case ConversionRestrictionKind::StringToPointer: {
    // Don't suggest anything for optional strings, as there's currently no
    // direct alternative.
    if (getArgType()->getOptionalObjectType())
      break;

    // We can suggest withCString as long as the resulting pointer is
    // immutable.
    switch (getPointerKind(getParamType())) {
    case PTK_UnsafePointer:
    case PTK_UnsafeRawPointer:
      emitDiagnosticAt(argExpr->getLoc(),
                       diag::ephemeral_use_string_with_c_string);
      break;
    case PTK_UnsafeMutableRawPointer:
    case PTK_UnsafeMutablePointer:
    case PTK_AutoreleasingUnsafeMutablePointer:
      // There's nothing really sensible we can suggest for a mutable pointer.
      break;
    }
    break;
  }
  case ConversionRestrictionKind::InoutToPointer:
  case ConversionRestrictionKind::InoutToCPointer:
    // For an arbitrary inout-to-pointer, we can suggest
    // withUnsafe[Mutable][Bytes/Pointer].
    if (auto alternative = getAlternativeKind())
      emitDiagnosticAt(argExpr->getLoc(),
                       diag::ephemeral_use_with_unsafe_pointer, *alternative);
    break;
  case ConversionRestrictionKind::DeepEquality:
  case ConversionRestrictionKind::Superclass:
  case ConversionRestrictionKind::Existential:
  case ConversionRestrictionKind::MetatypeToExistentialMetatype:
  case ConversionRestrictionKind::ExistentialMetatypeToMetatype:
  case ConversionRestrictionKind::ValueToOptional:
  case ConversionRestrictionKind::OptionalToOptional:
  case ConversionRestrictionKind::ClassMetatypeToAnyObject:
  case ConversionRestrictionKind::ExistentialMetatypeToAnyObject:
  case ConversionRestrictionKind::ProtocolMetatypeToProtocolClass:
  case ConversionRestrictionKind::PointerToPointer:
  case ConversionRestrictionKind::PointerToCPointer:
  case ConversionRestrictionKind::ArrayUpcast:
  case ConversionRestrictionKind::DictionaryUpcast:
  case ConversionRestrictionKind::SetUpcast:
  case ConversionRestrictionKind::HashableToAnyHashable:
  case ConversionRestrictionKind::CFTollFreeBridgeToObjC:
  case ConversionRestrictionKind::ObjCTollFreeBridgeToCF:
  case ConversionRestrictionKind::CGFloatToDouble:
  case ConversionRestrictionKind::DoubleToCGFloat:
    llvm_unreachable("Expected an ephemeral conversion!");
  }
}

bool NonEphemeralConversionFailure::diagnosePointerInit() const {
  auto *constructor = dyn_cast_or_null<ConstructorDecl>(getCallee());
  if (!constructor)
    return false;

  auto constructedTy = getFnType()->getResult();

  // Strip off a level of optionality if we have a failable initializer.
  if (constructor->isFailable())
    constructedTy = constructedTy->getOptionalObjectType();

  // This must stay in sync with diag::cannot_construct_dangling_pointer.
  enum ConstructorKind {
    CK_Pointer = 0,
    CK_BufferPointer,
  };

  // Consider OpaquePointer as well as the other kinds of pointers.
  auto isConstructingPointer =
      constructedTy->getAnyPointerElementType() ||
      constructedTy->getAnyNominal() == getASTContext().getOpaquePointerDecl();

  ConstructorKind constructorKind;
  auto parameterCount = constructor->getParameters()->size();
  if (isConstructingPointer && parameterCount == 1) {
    constructorKind = CK_Pointer;
  } else if (constructedTy->getAnyBufferPointerElementType() &&
             parameterCount == 2) {
    constructorKind = CK_BufferPointer;
  } else {
    return false;
  }

  auto anchor = getRawAnchor();
  emitDiagnosticAt(::getLoc(anchor), diag::cannot_construct_dangling_pointer,
                   constructedTy, constructorKind)
      .highlight(::getSourceRange(anchor));

  emitSuggestionNotes();
  return true;
}

bool NonEphemeralConversionFailure::diagnoseAsNote() {
  // We can only emit a useful note if we have a callee.
  if (auto *callee = getCallee()) {
    emitDiagnosticAt(callee, diag::candidate_performs_illegal_ephemeral_conv,
                     getParamPosition());
    return true;
  }
  return false;
}

bool NonEphemeralConversionFailure::diagnoseAsError() {
  // Emit a specialized diagnostic for
  // Unsafe[Mutable][Raw]Pointer.init([mutating]:) &
  // Unsafe[Mutable][Raw]BufferPointer.init(start:count:).
  if (diagnosePointerInit())
    return true;

  // Otherwise, emit a more general diagnostic.
  SmallString<8> scratch;
  auto argDesc = getArgDescription(scratch);

  auto *argExpr = getArgExpr();
  if (isa<InOutExpr>(argExpr)) {
    emitDiagnosticAt(argExpr->getLoc(), diag::cannot_use_inout_non_ephemeral,
                     argDesc, getCallee(), getCalleeFullName())
        .highlight(argExpr->getSourceRange());
  } else {
    emitDiagnosticAt(argExpr->getLoc(), diag::cannot_pass_type_to_non_ephemeral,
                     getArgType(), argDesc, getCallee(), getCalleeFullName())
        .highlight(argExpr->getSourceRange());
  }
  emitSuggestionNotes();
  return true;
}

bool AssignmentTypeMismatchFailure::diagnoseMissingConformance() const {
  auto srcType = getFromType();
  auto dstType = getToType()->lookThroughAllOptionalTypes();

  llvm::SmallPtrSet<ProtocolDecl *, 4> srcMembers;
  llvm::SmallPtrSet<ProtocolDecl *, 4> dstMembers;

  auto retrieveProtocols = [](Type type,
                              llvm::SmallPtrSetImpl<ProtocolDecl *> &members) {
    auto constraint = type;
    if (auto existential = constraint->getAs<ExistentialType>())
      constraint = existential->getConstraintType();

    if (auto *protocol = constraint->getAs<ProtocolType>())
      members.insert(protocol->getDecl());

    if (auto *composition = constraint->getAs<ProtocolCompositionType>()) {
      for (auto member : composition->getMembers()) {
        if (auto *protocol = member->getAs<ProtocolType>())
          members.insert(protocol->getDecl());
      }
    }
  };

  retrieveProtocols(srcType, srcMembers);
  retrieveProtocols(dstType, dstMembers);

  if (srcMembers.empty() || dstMembers.empty())
    return false;

  // Let's check whether there is an overlap between source and destination.
  for (auto *member : srcMembers)
    dstMembers.erase(member);

  if (dstMembers.size() == 1)
    dstType = (*dstMembers.begin())->getDeclaredType();

  emitDiagnostic(diag::cannot_convert_assign_protocol, srcType, dstType);
  return true;
}

bool AssignmentTypeMismatchFailure::diagnoseAsError() {
  if (diagnoseMissingConformance())
    return true;

  return ContextualFailure::diagnoseAsError();
}

bool AssignmentTypeMismatchFailure::diagnoseAsNote() {
  auto anchor = getAnchor();

  if (auto overload =
          getCalleeOverloadChoiceIfAvailable(getConstraintLocator(anchor))) {
    if (auto *decl = overload->choice.getDeclOrNull()) {
      emitDiagnosticAt(decl,
                       diag::cannot_convert_candidate_result_to_contextual_type,
                       decl->getName(), getFromType(), getToType());
      return true;
    }
  }

  return false;
}

bool MissingContextualBaseInMemberRefFailure::diagnoseAsError() {
  auto *anchor = castToExpr(getAnchor());
  // Member reference could be wrapped into a number of parens
  // e.g. `((.foo))`.
  auto *parentExpr = findParentExpr(anchor);

  // Look through immediate call of unresolved member (e.g., `.foo(0)`).
  if (isa_and_nonnull<CallExpr>(parentExpr))
    parentExpr = findParentExpr(parentExpr);

  // FIXME: We should probably look through the entire member chain so that
  // something like `let _ = .foo().bar` gets the "no contextual type" error
  // rather than the "Cannot infer contextual base" error.
  UnresolvedMemberChainResultExpr *resultExpr = nullptr;
  if (isa_and_nonnull<UnresolvedMemberChainResultExpr>(parentExpr)) {
    resultExpr = cast<UnresolvedMemberChainResultExpr>(parentExpr);
    parentExpr = findParentExpr(parentExpr);
  }

  do {
    // If we have found something which isn't a paren let's stop,
    // otherwise let's keep unwrapping until there are either no
    // more parens or no more parents...
    if (!parentExpr || !isa<ParenExpr>(parentExpr))
      break;
  } while ((parentExpr = findParentExpr(parentExpr)));

  auto diagnostic = parentExpr || (resultExpr && getContextualType(resultExpr))
                        ? diag::cannot_infer_base_of_unresolved_member
                        : diag::unresolved_member_no_inference;

  emitDiagnostic(diagnostic, MemberName).highlight(getSourceRange());
  return true;
}

bool UnableToInferClosureParameterType::diagnoseAsError() {
  auto *closure = castToExpr<ClosureExpr>(getRawAnchor());

  // Let's check whether this closure is an argument to
  // a call which couldn't be properly resolved e.g.
  // missing  member or invalid contextual reference and
  // if so let's not diagnose this problem because main
  // issue here is inability to establish context for
  // closure inference.
  //
  // TODO(diagnostics): Once we gain an ability to determine
  // originating source of type holes this check could be
  // significantly simplified.
  {
    auto &solution = getSolution();

    // If there is a contextual mismatch associated with this
    // closure, let's not diagnose any parameter type issues.
    if (hasFixFor(solution,
                  getConstraintLocator(closure, LocatorPathElt::ContextualType(
                                                    CTP_Initialization))))
      return false;

    if (auto *parentExpr = findParentExpr(closure)) {
      while (parentExpr &&
             (isa<TupleExpr>(parentExpr) || isa<ParenExpr>(parentExpr))) {
        parentExpr = findParentExpr(parentExpr);
      }

      if (parentExpr) {
        // Missing or invalid member reference in call.
        if (auto *AE = dyn_cast<ApplyExpr>(parentExpr)) {
          if (getType(AE->getFn())->is<UnresolvedType>())
            return false;
        }

        // Any fix anchored on parent expression makes it unnecessary
        // to diagnose unability to infer parameter type because it's
        // an indication that proper context couldn't be established to
        // resolve the closure.
        ASTNode parentNode(parentExpr);
        if (llvm::any_of(solution.Fixes,
                         [&parentNode](const ConstraintFix *fix) -> bool {
                           return fix->getAnchor() == parentNode;
                         }))
          return false;
      }
    }
  }

  auto paramIdx = getLocator()
                      ->castLastElementTo<LocatorPathElt::TupleElement>()
                      .getIndex();

  auto *PD = closure->getParameters()->get(paramIdx);

  llvm::SmallString<16> id;
  llvm::raw_svector_ostream OS(id);

  OS << "'" << PD->getParameterName() << "'";

  auto loc = PD->isImplicit() ? getLoc() : PD->getLoc();
  emitDiagnosticAt(loc, diag::cannot_infer_closure_parameter_type, OS.str());
  return true;
}

bool UnableToInferClosureReturnType::diagnoseAsError() {
  auto *closure = castToExpr<ClosureExpr>(getRawAnchor());

  auto *body = closure->getBody();
  // For empty closures, let's produce a tailored message and suggest
  // adding an expression to the body.
  if (body->empty()) {
    auto diagnostic =
        emitDiagnostic(diag::cannot_infer_empty_closure_result_type);

    diagnostic.fixItInsertAfter(closure->getInLoc().isValid()
                                    ? closure->getInLoc()
                                    : body->getLBraceLoc(),
                                "<#result#>");
    return true;
  }

  auto diagnostic = emitDiagnostic(diag::cannot_infer_closure_result_type);

  // If there is a location for an 'in' token, then the argument list was
  // specified somehow but no return type was.  Insert a "-> ReturnType "
  // before the in token.
  if (closure->getInLoc().isValid()) {
    diagnostic.fixItInsert(closure->getInLoc(),
                           diag::insert_closure_return_type_placeholder,
                           /*argListSpecified=*/false);
  } else if (closure->getParameters()->size() == 0) {
    // Otherwise, the closure must take zero arguments.
    //
    // As such, we insert " () -> ReturnType in " right after the '{' that
    // starts the closure body.
    diagnostic.fixItInsertAfter(body->getLBraceLoc(),
                                diag::insert_closure_return_type_placeholder,
                                /*argListSpecified=*/true);
  }

  return true;
}

static std::pair<StringRef, StringRef>
getImportModuleAndDefaultType(const ASTContext &ctx,
                              const ObjectLiteralExpr *expr) {
  const auto &target = ctx.LangOpts.Target;

  switch (expr->getLiteralKind()) {
    case ObjectLiteralExpr::colorLiteral: {
      if (target.isMacOSX()) {
        return std::make_pair("AppKit", "NSColor");
      } else if (target.isiOS() || target.isTvOS()) {
        return std::make_pair("UIKit", "UIColor");
      }
      break;
    }

    case ObjectLiteralExpr::imageLiteral: {
      if (target.isMacOSX()) {
        return std::make_pair("AppKit", "NSImage");
      } else if (target.isiOS() || target.isTvOS()) {
        return std::make_pair("UIKit", "UIImage");
      }
      break;
    }

    case ObjectLiteralExpr::fileLiteral: {
      return std::make_pair("Foundation", "URL");
    }
  }

  return std::make_pair("", "");
}

SourceLoc UnableToInferProtocolLiteralType::getLoc() const {
  return ::getLoc(getRawAnchor());
}

bool UnableToInferProtocolLiteralType::diagnoseAsError() {
  auto &ctx = getASTContext();
  auto *expr = castToExpr<ObjectLiteralExpr>(getRawAnchor());

  StringRef importModule;
  StringRef importDefaultTypeName;
  std::tie(importModule, importDefaultTypeName) =
      getImportModuleAndDefaultType(ctx, expr);

  auto plainName = expr->getLiteralKindPlainName();
  emitDiagnostic(diag::object_literal_default_type_missing, plainName);
  if (!importModule.empty()) {
    emitDiagnostic(diag::object_literal_resolve_import, importModule,
                   importDefaultTypeName, plainName);
  }

  return true;
}

bool MissingQualifierInMemberRefFailure::diagnoseAsError() {
  auto selectedOverload = getOverloadChoiceIfAvailable(getLocator());
  if (!selectedOverload)
    return false;

  auto *UDE = castToExpr<UnresolvedDotExpr>(getRawAnchor());

  auto baseType = getType(UDE->getBase());

  auto methodKind = baseType->isAnyExistentialType()
                        ? DescriptiveDeclKind::StaticMethod
                        : DescriptiveDeclKind::Method;

  auto choice = selectedOverload->choice.getDeclOrNull();
  if (!choice)
    return false;

  auto *DC = choice->getDeclContext();
  if (!(DC->isModuleContext() || DC->isModuleScopeContext())) {
    emitDiagnostic(diag::member_shadows_function, UDE->getName(), methodKind,
                   choice->getDescriptiveKind(), choice->getName());
    return true;
  }

  auto qualifier = DC->getParentModule()->getName();

  emitDiagnostic(diag::member_shadows_global_function, UDE->getName(),
                 methodKind, choice->getDescriptiveKind(),
                 choice->getName(), qualifier);

  SmallString<32> namePlusDot = qualifier.str();
  namePlusDot.push_back('.');

  emitDiagnostic(diag::fix_unqualified_access_top_level_multi, namePlusDot,
                 choice->getDescriptiveKind(), qualifier)
      .fixItInsert(UDE->getStartLoc(), namePlusDot);

  emitDiagnosticAt(choice, diag::decl_declared_here, choice->getName());
  return true;
}

bool CoercionAsForceCastFailure::diagnoseAsError() {
  emitDiagnostic(diag::coercion_may_fail_warning, getFromType(), getToType())
      .highlight(getSourceRange());
  return true;
}

bool KeyPathRootTypeMismatchFailure::diagnoseAsError() {
  auto locator = getLocator();
  assert(locator->isKeyPathRoot() && "Expected a key path root");
  
  auto baseType = getFromType();
  auto rootType = getToType();

  emitDiagnostic(diag::expr_keypath_root_type_mismatch,
                 rootType, baseType);
  return true;
}

bool MultiArgFuncKeyPathFailure::diagnoseAsError() {
  // Diagnose use a keypath where a function with multiple arguments is expected
  emitDiagnostic(diag::expr_keypath_multiparam_func_conversion,
                 resolveType(functionType));
  return true;
}

bool UnableToInferKeyPathRootFailure::diagnoseAsError() {
  assert(isExpr<KeyPathExpr>(getAnchor()) && "Expected key path expression");
  auto contextualType = getContextualType(getAnchor());
  auto *keyPathExpr = castToExpr<KeyPathExpr>(getAnchor());

  auto emitKeyPathDiagnostic = [&]() {
    if (contextualType && contextualType->isAnyKeyPath()) {
      return emitDiagnostic(
          diag::cannot_infer_keypath_root_anykeypath_context);
    }
    return emitDiagnostic(
        diag::cannot_infer_contextual_keypath_type_specify_root);
  };

  emitKeyPathDiagnostic()
      .highlight(keyPathExpr->getLoc())
      .fixItInsertAfter(keyPathExpr->getStartLoc(), "<#Root#>");
  return true;
}

Optional<Diag<Type, Type>>
AbstractRawRepresentableFailure::getDiagnostic() const {
  auto *locator = getLocator();

  if (locator->isForContextualType()) {
    return diag::cannot_convert_initializer_value;
  } else if (locator->isForAssignment()) {
    return diag::cannot_convert_assign;
  } else if (locator->isLastElement<LocatorPathElt::ApplyArgToParam>()) {
    return diag::cannot_convert_argument_value;
  }

  return None;
}

bool AbstractRawRepresentableFailure::diagnoseAsError() {
  auto message = getDiagnostic();
  if (!message)
    return false;

  auto diagnostic = emitDiagnostic(*message, getFromType(), getToType());
  fixIt(diagnostic);
  return true;
}

bool AbstractRawRepresentableFailure::diagnoseAsNote() {
  auto *locator = getLocator();

  Optional<InFlightDiagnostic> diagnostic;
  if (locator->isForContextualType()) {
    auto overload = getCalleeOverloadChoiceIfAvailable(locator);
    if (!overload)
      return false;

    if (auto *decl = overload->choice.getDeclOrNull()) {
      diagnostic.emplace(emitDiagnosticAt(
          decl, diag::cannot_convert_candidate_result_to_contextual_type,
          decl->getName(), ExpectedType, RawReprType));
    }
  } else if (auto argConv =
                 locator->getLastElementAs<LocatorPathElt::ApplyArgToParam>()) {
    diagnostic.emplace(emitDiagnostic(
        diag::candidate_has_invalid_argument_at_position, RawReprType,
        argConv->getParamIdx(), /*inOut=*/false, ExpectedType));
  }

  if (diagnostic) {
    fixIt(*diagnostic);
    return true;
  }

  return false;
}

void MissingRawRepresentableInitFailure::fixIt(
    InFlightDiagnostic &diagnostic) const {
  if (auto *E = getAsExpr(getAnchor())) {
    auto range = E->getSourceRange();
    auto rawReprObjType = RawReprType->getOptionalObjectType();
    auto valueObjType = ExpectedType->getOptionalObjectType();

    if (rawReprObjType && valueObjType) {
      std::string mapCodeFix;

      // Check whether expression has been be wrapped in parens first.
      if (!E->canAppendPostfixExpression()) {
        diagnostic.fixItInsert(range.Start, "(");
        mapCodeFix += ")";
      }

      mapCodeFix += ".map { ";
      mapCodeFix += rawReprObjType->getString();
      mapCodeFix += "(rawValue: $0) }";

      diagnostic.fixItInsertAfter(range.End, mapCodeFix);
    } else if (rawReprObjType) {
      diagnostic
          .fixItInsert(range.Start, rawReprObjType->getString() + "(rawValue: ")
          .fixItInsertAfter(range.End, ")");
    } else if (valueObjType) {
      diagnostic.flush();

      std::string fixItBefore = RawReprType->getString() + "(rawValue: ";
      std::string fixItAfter;

      if (!E->canAppendPostfixExpression(true)) {
        fixItBefore += "(";
        fixItAfter += ")";
      }

      fixItAfter += "!) ?? <#default value#>";

      emitDiagnostic(diag::construct_raw_representable_from_unwrapped_value,
                     RawReprType, valueObjType)
          .highlight(range)
          .fixItInsert(range.Start, fixItBefore)
          .fixItInsertAfter(range.End, fixItAfter);
    } else {
      diagnostic
          .fixItInsert(range.Start, RawReprType->getString() + "(rawValue: ")
          .fixItInsertAfter(range.End, ") ?? <#default value#>");
    }
  }
}

bool MissingRawValueFailure::diagnoseAsError() {
  auto *locator = getLocator();

  if (locator->isLastElement<LocatorPathElt::AnyRequirement>()) {
    MissingConformanceFailure failure(getSolution(), locator,
                                      {RawReprType, ExpectedType});

    auto diagnosed = failure.diagnoseAsError();
    if (!diagnosed)
      return false;

    auto note = emitDiagnostic(diag::note_remapped_type, ".rawValue");
    fixIt(note);

    return true;
  }

  return AbstractRawRepresentableFailure::diagnoseAsError();
}

void MissingRawValueFailure::fixIt(InFlightDiagnostic &diagnostic) const {
  auto *E = getAsExpr(getAnchor());
  if (!E)
    return;

  std::string fix;

  auto range = E->getSourceRange();
  if (!E->canAppendPostfixExpression()) {
    diagnostic.fixItInsert(range.Start, "(");
    fix += ")";
  }

  // If raw representable is an optional we need to map its raw value out
  // out first and then, if destination is not optional, allow to specify
  // default value.
  if (RawReprType->getOptionalObjectType()) {
    fix += "?.rawValue";

    if (!ExpectedType->getOptionalObjectType())
      fix += " ?? <#default value#>";
  } else {
    fix += ".rawValue";
  }

  diagnostic.fixItInsertAfter(range.End, fix);
}

bool MissingOptionalUnwrapKeyPathFailure::diagnoseAsError() {
  emitDiagnostic(diag::optional_not_unwrapped, getFromType(),
                 getFromType()->lookThroughSingleOptionalType());
  
  emitDiagnostic(diag::optional_keypath_application_base)
      .fixItInsertAfter(getLoc(), "?");
  emitDiagnostic(diag::unwrap_with_force_value)
      .fixItInsertAfter(getLoc(), "!");
  return true;
}

SourceLoc MissingOptionalUnwrapKeyPathFailure::getLoc() const {
  auto *SE = castToExpr<SubscriptExpr>(getAnchor());
  return SE->getBase()->getEndLoc();
}

bool TrailingClosureRequiresExplicitLabel::diagnoseAsError() {
  auto argInfo = *getFunctionArgApplyInfo(getLocator());

  {
    auto diagnostic = emitDiagnostic(
        diag::unlabeled_trailing_closure_deprecated, argInfo.getParamLabel());
    fixIt(diagnostic, argInfo);
  }

  if (auto *callee = argInfo.getCallee()) {
    emitDiagnosticAt(callee, diag::decl_declared_here, callee->getName());
  }

  return true;
}

void TrailingClosureRequiresExplicitLabel::fixIt(
    InFlightDiagnostic &diagnostic, const FunctionArgApplyInfo &info) const {
  auto &ctx = getASTContext();

  auto anchor = getRawAnchor();
  Expr *fn = nullptr;

  if (auto *applyExpr = getAsExpr<ApplyExpr>(anchor)) {
    fn = applyExpr->getFn();
  } else {
    // Covers subscripts, unresolved members etc.
    fn = getAsExpr(anchor);
  }

  if (!fn)
    return;

  auto *trailingClosure = info.getArgExpr();

  auto *argList = info.getArgList();
  auto existingRParenLoc = argList->getRParenLoc();

  SourceLoc leadingCommaLoc;
  if (argList->size() >= 2) {
    leadingCommaLoc = Lexer::getLocForEndOfToken(
        ctx.SourceMgr, argList->getExpr(argList->size() - 2)->getEndLoc());
  }

  // Figure out the text to be inserted before the trailing closure.
  SmallString<16> insertionText;
  SourceLoc insertionLoc;
  if (leadingCommaLoc.isValid()) {
    insertionText += ", ";
    assert(existingRParenLoc.isValid());
    insertionLoc = leadingCommaLoc;
  } else if (existingRParenLoc.isInvalid()) {
    insertionText += "(";
    insertionLoc = Lexer::getLocForEndOfToken(ctx.SourceMgr, fn->getEndLoc());
  } else {
    insertionLoc = existingRParenLoc;
  }

  // Add the label, if there is one.
  auto paramName = info.getParamLabel();
  if (!paramName.empty()) {
    insertionText += paramName.str();
    insertionText += ": ";
  }

  // If there is an existing right parentheses/brace, remove it while we
  // insert the new text.
  if (existingRParenLoc.isValid()) {
    SourceLoc afterExistingRParenLoc =
        Lexer::getLocForEndOfToken(ctx.SourceMgr, existingRParenLoc);
    diagnostic.fixItReplaceChars(insertionLoc, afterExistingRParenLoc,
                                 insertionText);
  } else {
    // Insert the appropriate prefix.
    diagnostic.fixItInsert(insertionLoc, insertionText);
  }

  // Insert a right parenthesis/brace after the closing '}' of the trailing
  // closure;
  SourceLoc newRParenLoc =
      Lexer::getLocForEndOfToken(ctx.SourceMgr, trailingClosure->getEndLoc());
  diagnostic.fixItInsert(newRParenLoc,
                         isExpr<SubscriptExpr>(anchor) ? "]" : ")");
}

bool InvalidEmptyKeyPathFailure::diagnoseAsError() {
  auto *KPE = getAsExpr<KeyPathExpr>(getAnchor());
  assert(KPE && KPE->hasSingleInvalidComponent() &&
         "Expected a malformed key path expression");

  // If we have a string interpolation represented as key path expressions
  // e.g. \(x), \(x, a: 1). Let's skip it because this would be already
  // diagnosed and it is not the case for an extra empty key path diagnostic.
  auto *root = KPE->getParsedRoot();
  if (root && (isa<ParenExpr>(root) || isa<TupleExpr>(root)))
    return true;

  emitDiagnostic(diag::expr_swift_keypath_empty);
  return true;
}

bool InvalidPatternInExprFailure::diagnoseAsError() {
  // Check to see if we have something like 'case <fn>(let foo)', where <fn>
  // has a fix associated with it. In such a case, it's more likely than not
  // that the user is trying to write an EnumElementPattern, but has made some
  // kind of mistake in the function expr that causes it to be treated as an
  // ExprPattern. Emitting an additional error for the out of place 'let foo' is
  // just noise in that case, so let's avoid diagnosing.
  llvm::SmallPtrSet<Expr *, 4> fixAnchors;
  for (auto *fix : getSolution().Fixes) {
    if (auto *anchor = getAsExpr(fix->getAnchor()))
      fixAnchors.insert(anchor);
  }
  {
    auto *E = castToExpr(getLocator()->getAnchor());
    while (auto *parent = findParentExpr(E)) {
      if (auto *CE = dyn_cast<CallExpr>(parent)) {
        if (fixAnchors.contains(CE->getFn()))
          return false;
      }
      E = parent;
    }
  }
  emitDiagnostic(diag::pattern_in_expr, P->getDescriptiveKind());
  return true;
}

bool MissingContextualTypeForNil::diagnoseAsError() {
  auto *expr = castToExpr<NilLiteralExpr>(getAnchor());

  // If this is a standalone `nil` literal expression e.g.
  // `_ = nil`, let's diagnose it here because solver can't
  // attempt any types for it.
  auto *parentExpr = findParentExpr(expr);

  while (isa_and_nonnull<IdentityExpr>(parentExpr))
    parentExpr = findParentExpr(parentExpr);

  // In cases like `_ = nil?` AST would have `nil`
  // wrapped in `BindOptionalExpr`.
  if (isa_and_nonnull<BindOptionalExpr>(parentExpr))
    parentExpr = findParentExpr(parentExpr);

  if (parentExpr) {
    // `_ = nil as? ...`
    if (isa<ConditionalCheckedCastExpr>(parentExpr)) {
      emitDiagnostic(diag::conditional_cast_from_nil);
      return true;
    }

    // `_ = nil!`
    if (isa<ForceValueExpr>(parentExpr)) {
      emitDiagnostic(diag::cannot_force_unwrap_nil_literal);
      return true;
    }

    // `_ = nil?`
    if (isa<OptionalEvaluationExpr>(parentExpr)) {
      emitDiagnostic(diag::unresolved_nil_literal);
      return true;
    }
  }

  emitDiagnostic(diag::unresolved_nil_literal);
  return true;
}

bool CouldNotInferPlaceholderType::diagnoseAsError() {
  // If this placeholder was explicitly written out by the user, they can maybe
  // fix things by specifying an actual type.
  if (auto *typeExpr = getAsExpr<TypeExpr>(getAnchor())) {
    if (typeExpr->getLoc().isValid()) {
      emitDiagnostic(diag::could_not_infer_placeholder);
      return true;
    }
  }

  // When placeholder type appears in an editor placeholder i.e.
  // `<#T##() -> _#>` we rely on the parser to produce a diagnostic
  // about editor placeholder and glance over all placeholder type
  // inference issues.
  if (isExpr<EditorPlaceholderExpr>(getAnchor()))
    return true;

  return false;
}

bool ReferenceToInvalidDeclaration::diagnoseAsError() {
  auto &DE = getASTContext().Diags;

  // `resolveType` caches results, so there is no way
  // to suppress and then re-request the diagnostic
  // via calling `resolveType` on the same `TypeRepr`.
  if (getAsDecl<ParamDecl>(getAnchor()))
    return DE.hadAnyError();

  auto *decl = castToExpr<DeclRefExpr>(getAnchor())->getDecl();
  assert(decl);

  // This problem should have been already diagnosed during
  // validation of the declaration.
  if (DE.hadAnyError())
    return true;

  // If no errors have been emitted yet, let's emit one
  // about reference to an invalid declaration.

  emitDiagnostic(diag::reference_to_invalid_decl, decl->getName());
  emitDiagnosticAt(decl, diag::decl_declared_here, decl->getName());
  return true;
}

bool InvalidReturnInResultBuilderBody::diagnoseAsError() {
  auto *closure = castToExpr<ClosureExpr>(getAnchor());

  auto returnStmts = TypeChecker::findReturnStatements(closure);
  assert(!returnStmts.empty());

  auto loc = returnStmts.front()->getReturnLoc();
  emitDiagnosticAt(loc, diag::result_builder_disabled_by_return, BuilderType);

  // Note that one can remove all of the return statements.
  {
    auto diag = emitDiagnosticAt(loc, diag::result_builder_remove_returns);
    for (auto returnStmt : returnStmts)
      diag.fixItRemove(returnStmt->getReturnLoc());
  }

  return true;
}

bool MemberMissingExplicitBaseTypeFailure::diagnoseAsError() {
  auto UME = castToExpr<UnresolvedMemberExpr>(getAnchor());
  auto memberName = UME->getName().getBaseIdentifier().str();
  auto &DE = getASTContext().Diags;
  auto &solution = getSolution();

  auto selected = solution.getOverloadChoice(getLocator());
  auto baseType =
      resolveType(selected.choice.getBaseType()->getMetatypeInstanceType());

  SmallVector<Type, 4> optionals;
  auto baseTyUnwrapped = baseType->lookThroughAllOptionalTypes(optionals);

  if (!optionals.empty()) {
    auto baseTyName = baseType->getCanonicalType().getString();
    auto baseTyUnwrappedName = baseTyUnwrapped->getString();
    auto loc = UME->getLoc();
    auto startLoc = UME->getStartLoc();

    DE.diagnoseWithNotes(
        DE.diagnose(loc, diag::optional_ambiguous_case_ref, baseTyName,
                    baseTyUnwrappedName, memberName),
        [&]() {
          DE.diagnose(UME->getDotLoc(), diag::optional_fixit_ambiguous_case_ref)
              .fixItInsert(startLoc, "Optional");
          DE.diagnose(UME->getDotLoc(),
                      diag::type_fixit_optional_ambiguous_case_ref,
                      baseTyUnwrappedName, memberName)
              .fixItInsert(startLoc, baseTyUnwrappedName);
        });
  } else {
    auto baseTypeName = baseType->getCanonicalType().getString();
    auto baseOptionalTypeName =
        OptionalType::get(baseType)->getCanonicalType().getString();

    DE.diagnoseWithNotes(
        DE.diagnose(UME->getLoc(), diag::optional_ambiguous_case_ref,
                    baseTypeName, baseOptionalTypeName, memberName),
        [&]() {
          DE.diagnose(UME->getDotLoc(),
                      diag::type_fixit_optional_ambiguous_case_ref,
                      baseOptionalTypeName, memberName)
              .fixItInsert(UME->getDotLoc(), baseOptionalTypeName);
          DE.diagnose(UME->getDotLoc(),
                      diag::type_fixit_optional_ambiguous_case_ref,
                      baseTypeName, memberName)
              .fixItInsert(UME->getDotLoc(), baseTypeName);
        });
  }

  return true;
}

bool InvalidMemberRefOnProtocolMetatype::diagnoseAsError() {
  auto *locator = getLocator();
  auto overload = getOverloadChoiceIfAvailable(locator);
  if (!overload)
    return false;

  auto *member = overload->choice.getDeclOrNull();
  assert(member);

  emitDiagnostic(
      diag::contextual_member_ref_on_protocol_requires_self_requirement,
      member->getDescriptiveKind(), member->getName());

  auto *extension = dyn_cast<ExtensionDecl>(member->getDeclContext());

  // If this was a protocol requirement we can't suggest a fix-it.
  if (!extension)
    return true;

  auto note =
      emitDiagnosticAt(extension, diag::missing_sametype_requirement_on_self);

  if (auto *whereClause = extension->getTrailingWhereClause()) {
    auto sourceRange = whereClause->getSourceRange();
    note.fixItInsertAfter(sourceRange.End, ", Self == <#Type#> ");
  } else if (auto nameRepr = extension->getExtendedTypeRepr()) {
    // Type repr is not always available so we need to be defensive
    // about its presence and validity.
    if (nameRepr->isInvalid())
      return true;

    if (auto noteLoc = nameRepr->getEndLoc()) {
      note.fixItInsertAfter(noteLoc, " where Self == <#Type#>");
    }
  }

  return true;
}

bool CheckedCastBaseFailure::isCastTypeIUO() const {
  auto *expr = castToExpr<CheckedCastExpr>(getAnchor());
  const auto *const TR = expr->getCastTypeRepr();
  return TR && TR->getKind() == TypeReprKind::ImplicitlyUnwrappedOptional;
}

SourceRange CheckedCastBaseFailure::getCastRange() const {
  auto anchor = getAnchor();
  if (auto *forcedCastExpr = getAsExpr<ForcedCheckedCastExpr>(anchor)) {
    return {forcedCastExpr->getLoc(), forcedCastExpr->getExclaimLoc()};
  } else if (auto *conditionalCast =
                 getAsExpr<ConditionalCheckedCastExpr>(anchor)) {
    return {conditionalCast->getLoc(), conditionalCast->getQuestionLoc()};
  } else if (auto expr = getAsExpr<IsExpr>(anchor)) {
    return expr->getLoc();
  }
  llvm_unreachable("There is no other kind of checked cast!");
}

std::tuple<Type, Type, int>
CoercibleOptionalCheckedCastFailure::unwrappedTypes() const {
  SmallVector<Type, 4> fromOptionals;
  SmallVector<Type, 4> toOptionals;
  Type unwrappedFromType =
      getFromType()->lookThroughAllOptionalTypes(fromOptionals);
  Type unwrappedToType = getToType()->lookThroughAllOptionalTypes(toOptionals);
  return std::make_tuple(unwrappedFromType, unwrappedToType,
                         fromOptionals.size() - toOptionals.size());
}

bool CoercibleOptionalCheckedCastFailure::diagnoseTernaryExpr() const {
  auto *expr = getAsExpr<IsExpr>(CastExpr);
  if (!expr)
    return false;

  Type unwrappedFrom, unwrappedTo;
  std::tie(unwrappedFrom, unwrappedTo, std::ignore) = unwrappedTypes();

  SourceRange diagFromRange = getFromRange();
  SourceRange diagToRange = getToRange();
  SourceLoc asLoc = expr->getAsLoc();

  // If we're only unwrapping a single optional, we could have just
  // checked for 'nil'.
  auto diag =
      emitDiagnostic(diag::is_expr_same_type, getFromType(), getToType());
  diag.highlight(diagFromRange);
  diag.highlight(diagToRange);
  diag.fixItReplace(SourceRange(asLoc, diagToRange.End), "!= nil");

  // Add parentheses if needed.
  if (!expr->getSubExpr()->canAppendPostfixExpression()) {
    diag.fixItInsert(expr->getSubExpr()->getStartLoc(), "(");
    diag.fixItInsertAfter(expr->getSubExpr()->getEndLoc(), ")");
  }

  return true;
}

bool CoercibleOptionalCheckedCastFailure::diagnoseForcedCastExpr() const {
  auto *expr = getAsExpr<ForcedCheckedCastExpr>(CastExpr);
  if (!expr)
    return false;

  auto fromType = getFromType();
  auto toType = getToType();
  Type unwrappedFrom, unwrappedTo;
  int extraFromOptionals;
  std::tie(unwrappedFrom, unwrappedTo, extraFromOptionals) = unwrappedTypes();

  SourceRange diagFromRange = getFromRange();
  SourceRange diagToRange = getToRange();

  bool isBridged = CastKind == CheckedCastKind::BridgingCoercion;
  if (isCastTypeIUO()) {
    // IUO type could either be optional or unwrapped.
    if (auto objType = toType->getOptionalObjectType()) {
      extraFromOptionals++;
      toType = objType;
    }
  }

  std::string extraFromOptionalsStr(extraFromOptionals, '!');
  auto diag = emitDiagnostic(diag::downcast_same_type, fromType, toType,
                             extraFromOptionalsStr, isBridged);
  diag.highlight(diagFromRange);
  diag.highlight(diagToRange);

  /// Add the '!''s needed to adjust the type.
  diag.fixItInsertAfter(diagFromRange.End,
                        std::string(extraFromOptionals, '!'));
  if (isBridged) {
    // If it's bridged, we still need the 'as' to perform the bridging.
    diag.fixItReplace(getCastRange(), "as");
  } else {
    auto &ctx = getASTContext();
    // Otherwise, implicit conversions will handle it in most cases.
    SourceLoc afterExprLoc =
        Lexer::getLocForEndOfToken(ctx.SourceMgr, diagFromRange.End);

    diag.fixItRemove(SourceRange(afterExprLoc, diagToRange.End));
  }
  return true;
}

bool CoercibleOptionalCheckedCastFailure::diagnoseConditionalCastExpr() const {
  auto *expr = getAsExpr<ConditionalCheckedCastExpr>(CastExpr);
  if (!expr)
    return false;

  auto fromType = getFromType();
  auto toType = getToType();
  Type unwrappedFrom, unwrappedTo;
  std::tie(unwrappedFrom, unwrappedTo, std::ignore) = unwrappedTypes();

  SourceRange diagFromRange = getFromRange();
  SourceRange diagToRange = getToRange();

  bool isBridged = CastKind == CheckedCastKind::BridgingCoercion;

  // A single optional is carried through. It's better to use 'as' to
  // the appropriate optional type.
  auto diag =
      emitDiagnostic(diag::conditional_downcast_same_type, fromType, toType,
                     unwrappedFrom->isEqual(toType) ? 0 : isBridged ? 2 : 1);
  diag.highlight(diagFromRange);
  diag.highlight(diagToRange);

  if (isBridged) {
    // For a bridged cast, replace the 'as?' with 'as'.
    diag.fixItReplace(getCastRange(), "as");

    // Make sure we'll cast to the appropriately-optional type by adding
    // the '?'.
    // FIXME: Parenthesize!
    diag.fixItInsertAfter(diagToRange.End, "?");
  } else {
    auto &ctx = getASTContext();
    // Just remove the cast; implicit conversions will handle it.
    SourceLoc afterExprLoc =
        Lexer::getLocForEndOfToken(ctx.SourceMgr, diagFromRange.End);

    if (afterExprLoc.isValid() && diagToRange.isValid())
      diag.fixItRemove(SourceRange(afterExprLoc, diagToRange.End));
  }
  return true;
}

bool NoopCheckedCast::diagnoseIsExpr() const {
  auto *expr = getAsExpr<IsExpr>(CastExpr);
  if (!expr)
    return false;

  emitDiagnostic(diag::isa_is_always_true, "is");
  return true;
}

bool NoopCheckedCast::diagnoseConditionalCastExpr() const {
  auto *expr = getAsExpr<ConditionalCheckedCastExpr>(CastExpr);
  if (!expr)
    return false;

  emitDiagnostic(diag::conditional_downcast_coercion, getFromType(),
                 getToType());
  return true;
}

bool NoopCheckedCast::diagnoseForcedCastExpr() const {
  auto *expr = getAsExpr<ForcedCheckedCastExpr>(CastExpr);
  if (!expr)
    return false;

  auto fromType = getFromType();
  auto toType = getToType();
  auto diagLoc = expr->getLoc();

  if (isCastTypeIUO()) {
    if (auto objType = toType->getOptionalObjectType())
      toType = objType;
  }

  if (fromType->isEqual(toType)) {
    auto castTypeRepr = expr->getCastTypeRepr();
    emitDiagnostic(diag::forced_downcast_noop, toType)
        .fixItRemove(SourceRange(diagLoc, castTypeRepr->getSourceRange().End));

  } else {
    emitDiagnostic(diag::forced_downcast_coercion, fromType, toType)
        .fixItReplace(getCastRange(), "as");
  }
  return true;
}

bool NoopCheckedCast::diagnoseAsError() {
  if (diagnoseIsExpr())
    return true;

  if (diagnoseForcedCastExpr())
    return true;

  if (diagnoseConditionalCastExpr())
    return true;

  llvm_unreachable("Shouldn't reach here");
}

bool NoopExistentialToCFTypeCheckedCast::diagnoseAsError() {
  emitDiagnostic(diag::isa_is_foreign_check, getToType());
  return true;
}

bool CoercibleOptionalCheckedCastFailure::diagnoseAsError() {
  if (diagnoseTernaryExpr())
    return true;

  if (diagnoseForcedCastExpr())
    return true;

  if (diagnoseConditionalCastExpr())
    return true;

  llvm_unreachable("Shouldn't reach here");
}

bool UnsupportedRuntimeCheckedCastFailure::diagnoseAsError() {
  auto anchor = getAnchor();
  emitDiagnostic(diag::checked_cast_not_supported, getFromType(), getToType(),
                 isExpr<IsExpr>(anchor) ? 0 : 1);
  emitDiagnostic(diag::checked_cast_not_supported_coerce_instead)
      .fixItReplace(getCastRange(), "as");
  return true;
}

bool CheckedCastToUnrelatedFailure::diagnoseAsError() {
  const auto fromType = getFromType();
  const auto toType = getToType();
  auto *sub = CastExpr->getSubExpr()->getSemanticsProvidingExpr();
  // FIXME(https://github.com/apple/swift/issues/54529): This literal diagnostics needs to be revisited by a proposal to unify casting semantics for literals.
  auto &ctx = getASTContext();
  auto *dc = getDC();
  if (isa<LiteralExpr>(sub)) {
    auto *protocol = TypeChecker::getLiteralProtocol(ctx, sub);
    // Special handle for literals conditional checked cast when they can
    // be statically coerced to the cast type.
    if (protocol && TypeChecker::conformsToProtocol(toType, protocol,
                                                    dc->getParentModule())) {
      emitDiagnostic(diag::literal_conditional_downcast_to_coercion, fromType,
                     toType);
      return true;
    }
  }

  emitDiagnostic(diag::downcast_to_unrelated, getFromType(), toType)
      .highlight(getFromRange())
      .highlight(getToRange());
  // If we're referring to a function with a return value (not Void) then
  // emit a fix-it suggesting to add `()` to call the function
  if (auto DRE = dyn_cast<DeclRefExpr>(sub)) {
    if (auto FD = dyn_cast<FuncDecl>(DRE->getDecl())) {
      if (!FD->getResultInterfaceType()->isVoid()) {
        emitDiagnostic(diag::downcast_to_unrelated_fixit,
                       FD->getBaseIdentifier())
            .fixItInsertAfter(sub->getEndLoc(), "()");
      }
    }
  }
  return true;
}

bool InvalidWeakAttributeUse::diagnoseAsError() {
  auto *pattern =
      dyn_cast_or_null<NamedPattern>(getAnchor().dyn_cast<Pattern *>());
  if (!pattern)
    return false;

  auto *var = pattern->getDecl();
  auto varType = getType(var);

  auto diagnostic =
      emitDiagnosticAt(var, diag::invalid_ownership_not_optional,
                       ReferenceOwnership::Weak, varType);

  auto typeRange = var->getTypeSourceRangeForDiagnostics();
  if (varType->hasSimpleTypeRepr()) {
    diagnostic.fixItInsertAfter(typeRange.End, "?");
  } else {
    diagnostic.fixItInsert(typeRange.Start, "(")
        .fixItInsertAfter(typeRange.End, ")?");
  }

  return true;
}

bool TupleLabelMismatchWarning::diagnoseAsError() {
  emitDiagnostic(diag::tuple_label_mismatch_warning, getFromType(), getToType())
      .highlight(getSourceRange());
  return true;
}

bool AssociatedValueMismatchFailure::diagnoseAsError() {
  auto match = getLocator()->castLastElementTo<LocatorPathElt::PatternMatch>();
  auto *enumElementPattern = dyn_cast<EnumElementPattern>(match.getPattern());

  emitDiagnosticAt(enumElementPattern->getNameLoc(),
                   diag::enum_element_pattern_assoc_values_mismatch,
                   enumElementPattern->getName());
  emitDiagnosticAt(enumElementPattern->getNameLoc(),
                   diag::enum_element_pattern_assoc_values_remove)
      .fixItRemove(enumElementPattern->getSubPattern()->getSourceRange());
  return true;
}

bool SwiftToCPointerConversionInInvalidContext::diagnoseAsError() {
  auto argInfo = getFunctionArgApplyInfo(getLocator());
  if (!argInfo)
    return false;

  auto *callee = argInfo->getCallee();
  auto argType = resolveType(argInfo->getArgType());
  auto paramType = resolveType(argInfo->getParamType());

  emitDiagnostic(diag::cannot_convert_argument_value_for_swift_func, argType,
                 paramType, callee->getDescriptiveKind(), callee->getName());
  return true;
}

bool DefaultExprTypeMismatch::diagnoseAsError() {
  auto *locator = getLocator();

  unsigned paramIdx =
      locator->castLastElementTo<LocatorPathElt::ApplyArgToParam>()
          .getParamIdx();

  emitDiagnostic(diag::cannot_convert_default_value_type_to_argument_type,
                 getFromType(), getToType(), paramIdx);

  auto overload = getCalleeOverloadChoiceIfAvailable(locator);
  assert(overload);

  auto *PD = getParameterList(overload->choice.getDecl())->get(paramIdx);

  auto note = emitDiagnosticAt(PD->getLoc(), diag::default_value_declared_here);

  if (auto *defaultExpr = PD->getTypeCheckedDefaultExpr()) {
    note.highlight(defaultExpr->getSourceRange());
  }

  return true;
}

bool MissingExplicitExistentialCoercion::diagnoseAsError() {
  auto diagnostic = emitDiagnostic(diag::result_requires_explicit_coercion,
                                   ErasedResultType);
  fixIt(diagnostic);
  return true;
}

bool MissingExplicitExistentialCoercion::diagnoseAsNote() {
  auto diagnostic = emitDiagnostic(
      diag::candidate_result_requires_explicit_coercion, ErasedResultType);
  fixIt(diagnostic);
  return true;
}

bool MissingExplicitExistentialCoercion::fixItRequiresParens() const {
  auto anchor = getAsExpr(getRawAnchor());

  // If it's a member reference an an existential metatype, let's
  // use the parent "call" expression.
  if (auto *UDE = dyn_cast_or_null<UnresolvedDotExpr>(anchor))
    anchor = findParentExpr(UDE);

  if (!anchor)
    return false;

  const auto &solution = getSolution();
  return llvm::any_of(
      solution.OpenedExistentialTypes,
      [&anchor](const auto &openedExistential) {
        if (auto openedLoc = simplifyLocatorToAnchor(openedExistential.first)) {
          return anchor == getAsExpr(openedLoc);
        }
        return false;
      });
}

void MissingExplicitExistentialCoercion::fixIt(
    InFlightDiagnostic &diagnostic) const {

  if (ErasedResultType->hasTypeParameter())
    return;

  bool requiresParens = fixItRequiresParens();

  auto callRange = getSourceRange();

  if (requiresParens)
    diagnostic.fixItInsert(callRange.Start, "(");

  auto printOpts = PrintOptions::forDiagnosticArguments();
  diagnostic.fixItInsertAfter(callRange.End,
                              "as " + ErasedResultType->getString(printOpts) +
                                  (requiresParens ? ")" : ""));
}

bool ConflictingPatternVariables::diagnoseAsError() {
  for (auto *var : Vars) {
    emitDiagnosticAt(var->getStartLoc(),
                     diag::type_mismatch_multiple_pattern_list, getType(var),
                     ExpectedType);
  }
  return true;
}

bool AddMissingMacroPound::diagnoseAsError() {
  emitDiagnostic(diag::macro_expansion_missing_pound, macro->getName())
    .fixItInsert(getLoc(), "#");
  return true;
}

bool GlobalActorFunctionMismatchFailure::diagnoseTupleElement() {
  auto *locator = getLocator();
  auto path = locator->getPath();

  if (path.empty())
    return false;

  if (path.back().getKind() == ConstraintLocator::TupleElement) {
    auto anchor = getRawAnchor();
    if (isExpr<ArrayExpr>(anchor)) {
      emitDiagnostic(diag::cannot_convert_global_actor_mismatch_element,
                     getFromType(), getToType());
      return true;
    }

    auto eltLoc = locator->castLastElementTo<LocatorPathElt::TupleElement>();
    // Only handle values because functions cannot be hashable, is not
    // possible for them to be keys.
    if (isExpr<DictionaryExpr>(anchor) && eltLoc.getIndex() == 1) {
      emitDiagnostic(diag::cannot_convert_global_actor_mismatch_dict_value,
                     getFromType(), getToType());
    } else {
      emitDiagnostic(diag::cannot_convert_global_actor_mismatch_tuple_element,
                     getFromType(), getToType(), eltLoc.getIndex());
    }
    return true;
  }
  return false;
}

Diag<Type, Type>
GlobalActorFunctionMismatchFailure::getDiagnosticMessage() const {
  auto *locator = getLocator();
  auto path = locator->getPath();

  if (path.empty()) {
    return diag::cannot_convert_global_actor;
  }

  auto last = path.back();
  switch (last.getKind()) {
  case ConstraintLocator::ApplyArgToParam: {
    return diag::cannot_convert_argument_value_global_actor;
  }
  case ConstraintLocator::ContextualType: {
    return diag::cannot_convert_global_actor_contextual;
  }
  case ConstraintLocator::ClosureBody:
  case ConstraintLocator::ClosureResult: {
    return diag::cannot_convert_closure_result_global_actor;
  }
  case ConstraintLocator::TernaryBranch: {
    return diag::ternary_expr_cases_global_actor_mismatch;
  }
  case ConstraintLocator::CoercionOperand: {
    return diag::cannot_convert_global_actor_coercion;
  }
  default:
    break;
  }
  return diag::cannot_convert_global_actor;
}

bool GlobalActorFunctionMismatchFailure::diagnoseAsError() {
  if (diagnoseTupleElement())
    return true;

  const auto message = getDiagnosticMessage();
  emitDiagnostic(message, getFromType(), getToType());
  return true;
}

bool DestructureTupleToUseWithPackExpansionParameter::diagnoseAsError() {
  auto *locator = getLocator();
  auto argLoc = locator->castLastElementTo<LocatorPathElt::ApplyArgToParam>();

  {
    auto diagnostic =
        emitDiagnostic(diag::cannot_convert_tuple_into_pack_expansion_parameter,
                       argLoc.getParamIdx(), ParamShape->getNumElements(),
                       isExpr<TupleExpr>(getAnchor()));

    if (auto *tupleExpr = getAsExpr<TupleExpr>(getAnchor())) {
      diagnostic.fixItRemove(tupleExpr->getLParenLoc());
      diagnostic.fixItRemove(tupleExpr->getRParenLoc());
    }
  }

  auto selectedOverload = getCalleeOverloadChoiceIfAvailable(getLocator());
  if (!selectedOverload)
    return true;

  if (auto *decl = selectedOverload->choice.getDeclOrNull()) {
    emitDiagnosticAt(decl, diag::decl_declared_here, decl->getName());
  }

  return true;
}

bool DestructureTupleToUseWithPackExpansionParameter::diagnoseAsNote() {
  auto selectedOverload = getCalleeOverloadChoiceIfAvailable(getLocator());
  if (!selectedOverload || !selectedOverload->choice.isDecl())
    return false;

  auto *choice = selectedOverload->choice.getDecl();
  auto argLoc =
      getLocator()->castLastElementTo<LocatorPathElt::ApplyArgToParam>();

  emitDiagnosticAt(
      choice, diag::cannot_convert_tuple_into_pack_expansion_parameter_note,
      argLoc.getParamIdx(), ParamShape->getNumElements());
  return true;
}

bool ValuePackExpansionWithoutPackReferences::diagnoseAsError() {
  emitDiagnostic(diag::value_expansion_not_variadic);
  return true;
}

bool MissingEachForValuePackReference::diagnoseAsError() {
  bool fixItNeedsParens = false;
  // If 'each' is missing form a base of a member reference
  // it has to be wrapped in parens.
  if (auto anchor = getAsExpr(getAnchor())) {
    fixItNeedsParens = isExpr<UnresolvedDotExpr>(findParentExpr(anchor));
  }

  {
    auto diagnostic = emitDiagnostic(diag::value_pack_requires_keyword_each, ValuePackType);
    if (fixItNeedsParens) {
      auto range = getSourceRange();
      diagnostic.fixItInsert(range.Start, "(each ")
          .fixItInsertAfter(range.End, ")");
    } else {
      diagnostic.fixItInsert(getLoc(), "each ");
    }
  }

  return true;
}

bool InvalidMemberReferenceWithinInitAccessor::diagnoseAsError() {
  emitDiagnostic(diag::init_accessor_invalid_member_ref, MemberName);
  return true;
}
