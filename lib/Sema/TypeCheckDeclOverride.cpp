//===--- TypeCheckOverride.cpp - Override Checking ------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements semantic analysis for declaration overrides.
//
//===----------------------------------------------------------------------===//
#include "MiscDiagnostics.h"
#include "TypeCheckAvailability.h"
#include "TypeCheckConcurrency.h"
#include "TypeCheckDecl.h"
#include "TypeCheckObjC.h"
#include "TypeChecker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Availability.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/TypeCheckRequests.h"
using namespace swift;

static void adjustFunctionTypeForOverride(Type &type) {
  // Drop 'throws'.
  // FIXME: Do we want to allow overriding a function returning a value
  // with one returning Never?
  auto fnType = type->castTo<AnyFunctionType>();
  auto extInfo = fnType->getExtInfo();
  extInfo = extInfo.withThrows(false);
  if (!fnType->getExtInfo().isEqualTo(extInfo, useClangTypes(fnType)))
    type = fnType->withExtInfo(extInfo);
}

/// Drop the optionality of the result type of the given function type.
static Type dropResultOptionality(Type type, unsigned uncurryLevel) {
  // We've hit the result type.
  if (uncurryLevel == 0) {
    if (auto objectTy = type->getOptionalObjectType())
      return objectTy;

    return type;
  }

  // Determine the input and result types of this function.
  auto fnType = type->castTo<AnyFunctionType>();
  auto parameters = fnType->getParams();
  Type resultType =
      dropResultOptionality(fnType->getResult(), uncurryLevel - 1);

  // Produce the resulting function type.
  if (auto genericFn = dyn_cast<GenericFunctionType>(fnType)) {
    return GenericFunctionType::get(genericFn->getGenericSignature(),
                                    parameters, resultType,
                                    fnType->getExtInfo());
  }

  return FunctionType::get(parameters, resultType, fnType->getExtInfo());
}

Type swift::getMemberTypeForComparison(const ValueDecl *member,
                                       const ValueDecl *derivedDecl) {
  auto *method = dyn_cast<AbstractFunctionDecl>(member);
  auto *ctor = dyn_cast_or_null<ConstructorDecl>(method);

  auto abstractStorage = dyn_cast<AbstractStorageDecl>(member);
  assert((method || abstractStorage) && "Not a method or abstractStorage?");
  auto *subscript = dyn_cast_or_null<SubscriptDecl>(abstractStorage);

  auto memberType = member->getInterfaceType();
  if (memberType->is<ErrorType>())
    return memberType;

  if (derivedDecl) {
    auto *dc = derivedDecl->getDeclContext();
    auto owningType = dc->getDeclaredInterfaceType();
    assert(owningType);

    memberType = owningType->adjustSuperclassMemberDeclType(member, derivedDecl,
                                                            memberType);
  }

  if (method) {
    // For methods, strip off the 'Self' type.
    memberType = memberType->castTo<AnyFunctionType>()->getResult();
    adjustFunctionTypeForOverride(memberType);
  } else if (subscript) {
    // For subscripts, we don't have a 'Self' type, but turn it
    // into a monomorphic function type.
    auto funcTy = memberType->castTo<AnyFunctionType>();
    // FIXME: Verify ExtInfo state is correct, not working by accident.
    FunctionType::ExtInfo info;
    memberType =
        FunctionType::get(funcTy->getParams(), funcTy->getResult(), info);
  } else {
    // For properties, strip off ownership.
    memberType = memberType->getReferenceStorageReferent();
  }

  // Ignore the optionality of initializers when comparing types;
  // we'll enforce this separately
  if (ctor) {
    memberType = dropResultOptionality(memberType, 1);
  }

  return memberType;
}

static bool
areAccessorsOverrideCompatible(const AbstractStorageDecl *storage,
                               const AbstractStorageDecl *parentStorage) {
  // It's okay for the storage to disagree about whether to use a getter or
  // a read accessor; we'll patch up any differences when setting overrides
  // for the accessors.  We don't want to diagnose anything involving
  // `@_borrowed` because it is not yet part of the language.

  // All the other checks are for non-static storage only.
  if (storage->isStatic())
    return true;

  // The storage must agree on whether reads are mutating.  For accessors,
  // this is sufficient to imply that they use the same SelfAccessKind
  // because we do not allow accessors to be consuming.
  if (storage->isGetterMutating() != parentStorage->isGetterMutating())
    return false;

  // We allow covariance about whether the storage itself is mutable, so we
  // can only check mutating-ness of setters if both have one.
  if (storage->supportsMutation() && parentStorage->supportsMutation()) {
    // The storage must agree on whether writes are mutating.
    if (storage->isSetterMutating() != parentStorage->isSetterMutating())
      return false;

    // Those together should imply that read-write accesses have the same
    // mutability.
  }

  return true;
}

bool swift::isOverrideBasedOnType(const ValueDecl *decl, Type declTy,
                                  const ValueDecl *parentDecl) {
  auto genericSig =
      decl->getInnermostDeclContext()->getGenericSignatureOfContext();

  auto canDeclTy = declTy->getReducedType(genericSig);

  auto declIUOAttr = decl->isImplicitlyUnwrappedOptional();
  auto parentDeclIUOAttr = parentDecl->isImplicitlyUnwrappedOptional();

  if (declIUOAttr != parentDeclIUOAttr)
    return false;

  // If the generic signatures don't match, then return false because we don't
  // want to complain if an overridden method matches multiple superclass
  // methods which differ in generic signature.
  //
  // We can still succeed with a subtype match later in
  // OverrideMatcher::match().
  if (auto declCtx = decl->getAsGenericContext()) {
    // The below logic now works correctly for protocol requirements which are
    // themselves generic, but that would be an ABI break, since we would now
    // drop the protocol requirements from witness tables. Simulate the old
    // behavior by not considering generic declarations in protocols as
    // overrides at all.
    if (decl->getDeclContext()->getSelfProtocolDecl() &&
        declCtx->isGeneric())
      return false;

    auto *parentCtx = parentDecl->getAsGenericContext();

    if (declCtx->isGeneric() != parentCtx->isGeneric())
      return false;

    if (declCtx->isGeneric() &&
        (declCtx->getGenericParams()->size() !=
         parentCtx->getGenericParams()->size()))
      return false;

    auto &ctx = decl->getASTContext();
    auto sig = ctx.getOverrideGenericSignature(parentDecl, decl);
    if (sig &&
        declCtx->getGenericSignature().getCanonicalSignature() !=
            sig.getCanonicalSignature()) {
      return false;
    }
  }

  auto parentDeclTy = getMemberTypeForComparison(parentDecl, decl);
  if (parentDeclTy->hasError())
    return false;

  auto canParentDeclTy = parentDeclTy->getReducedType(genericSig);

  // If this is a constructor, let's compare only parameter types.
  if (isa<ConstructorDecl>(decl)) {
    // Within a protocol context, check for a failability mismatch.
    if (isa<ProtocolDecl>(decl->getDeclContext())) {
      if (cast<ConstructorDecl>(decl)->isFailable() !=
          cast<ConstructorDecl>(parentDecl)->isFailable())
        return false;
      if (cast<ConstructorDecl>(decl)->isImplicitlyUnwrappedOptional() !=
          cast<ConstructorDecl>(parentDecl)->isImplicitlyUnwrappedOptional())
        return false;
    }

    auto fnType1 = declTy->castTo<AnyFunctionType>();
    auto fnType2 = parentDeclTy->castTo<AnyFunctionType>();
    return AnyFunctionType::equalParams(fnType1->getParams(),
                                        fnType2->getParams());

  // In a non-static protocol requirement, verify that the self access kind
  // matches.
  } else if (auto func = dyn_cast<FuncDecl>(decl)) {
    // We only compare `isMutating()` rather than `getSelfAccessKind()`
    // because we don't want to complain about `nonmutating` vs. `__consuming`
    // conflicts at this time, especially since `__consuming` is not yet
    // officially part of the language.
    if (!func->isStatic() &&
        func->isMutating() != cast<FuncDecl>(parentDecl)->isMutating())
      return false;

  // In abstract storage, verify that the accessor mutating-ness matches.
  } else if (auto storage = dyn_cast<AbstractStorageDecl>(decl)) {
    auto parentStorage = cast<AbstractStorageDecl>(parentDecl);
    if (!areAccessorsOverrideCompatible(storage, parentStorage))
      return false;
  }

  return canDeclTy == canParentDeclTy;
}

static bool isUnavailableInAllVersions(ValueDecl *decl) {
  ASTContext &ctx = decl->getASTContext();
  auto *attr = decl->getAttrs().getUnavailable(ctx);

  if (!attr)
    return false;
  if (attr->isUnconditionallyUnavailable())
    return true;

  return attr->getVersionAvailability(ctx)
             == AvailableVersionComparison::Unavailable;
}

/// Perform basic checking to determine whether a declaration can override a
/// declaration in a superclass.
static bool areOverrideCompatibleSimple(ValueDecl *decl,
                                        ValueDecl *parentDecl) {
  // If the number of argument labels does not match, these overrides cannot
  // be compatible.
  if (decl->getName().getArgumentNames().size() !=
        parentDecl->getName().getArgumentNames().size())
    return false;

  // If the parent declaration is not in a class (or extension thereof) or
  // a protocol, we cannot override it.
  if (decl->getDeclContext()->getSelfClassDecl() &&
      parentDecl->getDeclContext()->getSelfClassDecl()) {
    // Okay: class override
  } else if (isa<ProtocolDecl>(decl->getDeclContext()) &&
             isa<ProtocolDecl>(parentDecl->getDeclContext())) {
    // Okay: protocol override.
  } else {
    // Cannot be an override.
    return false;
  }

  // Ignore declarations that are defined inside constrained extensions.
  if (auto *ext = dyn_cast<ExtensionDecl>(parentDecl->getDeclContext()))
    if (ext->isConstrainedExtension())
      return false;

  // The declarations must be of the same kind.
  if (decl->getKind() != parentDecl->getKind())
    return false;

  // If the parent decl is unavailable, the subclass decl can shadow it, but it
  // can't override it. To avoid complex version logic, we don't apply this to
  // `obsoleted` members, only `unavailable` ones.
  // FIXME: Refactor to allow that when the minimum version is always satisfied.
  if (isUnavailableInAllVersions(parentDecl))
    // If the subclass decl is trying to override, we'll diagnose it later.
    if (!decl->getAttrs().hasAttribute<OverrideAttr>())
      return false;

  // Ignore invalid parent declarations.
  // FIXME: Do we really need this?
  if (parentDecl->isInvalid())
    return false;

  // If their staticness is different, they aren't compatible.
  if (decl->isStatic() != parentDecl->isStatic())
    return false;

  // If their genericity is different, they aren't compatible.
  if (auto genDecl = decl->getAsGenericContext()) {
    auto genParentDecl = parentDecl->getAsGenericContext();
    if (genDecl->isGeneric() != genParentDecl->isGeneric())
      return false;

    if (genDecl->isGeneric() &&
        (genDecl->getGenericParams()->size() !=
         genParentDecl->getGenericParams()->size()))
      return false;
  }

  // Factory initializers cannot be overridden.
  if (auto parentCtor = dyn_cast<ConstructorDecl>(parentDecl))
    if (parentCtor->isFactoryInit())
      return false;

  return true;
}

static bool
diagnoseMismatchedOptionals(const ValueDecl *member,
                            const ParameterList *params, TypeLoc resultTL,
                            const ValueDecl *parentMember,
                            const ParameterList *parentParams, Type owningTy,
                            bool treatIUOResultAsError) {
  auto &diags = member->getASTContext().Diags;

  bool emittedError = false;
  Type plainParentTy = owningTy->adjustSuperclassMemberDeclType(
      parentMember, member, parentMember->getInterfaceType());
  const auto *parentTy = plainParentTy->castTo<FunctionType>();
  if (isa<AbstractFunctionDecl>(parentMember))
    parentTy = parentTy->getResult()->castTo<FunctionType>();

  // Check the parameter types.
  auto checkParam = [&](const ParamDecl *decl, const ParamDecl *parentDecl) {
    Type paramTy = decl->getType();
    Type parentParamTy = parentDecl->getType();

    auto *repr = decl->getTypeRepr();
    if (!repr)
      return;

    bool paramIsOptional =  (bool) paramTy->getOptionalObjectType();
    bool parentIsOptional = (bool) parentParamTy->getOptionalObjectType();

    if (paramIsOptional == parentIsOptional)
      return;

    if (!paramIsOptional) {
      if (parentDecl->isImplicitlyUnwrappedOptional())
        if (!treatIUOResultAsError)
          return;

      emittedError = true;
      auto diag = diags.diagnose(decl->getStartLoc(),
                                 diag::override_optional_mismatch,
                                 member->getDescriptiveKind(),
                                 isa<SubscriptDecl>(member),
                                 parentParamTy, paramTy);
      if (repr->isSimple()) {
        diag.fixItInsertAfter(repr->getEndLoc(), "?");
      } else {
        diag.fixItInsert(repr->getStartLoc(), "(");
        diag.fixItInsertAfter(repr->getEndLoc(), ")?");
      }
      return;
    }

    if (!decl->isImplicitlyUnwrappedOptional())
      return;

    // Allow silencing this warning using parens.
    if (paramTy->hasParenSugar())
      return;

    diags
        .diagnose(decl->getStartLoc(), diag::override_unnecessary_IUO,
                  member->getDescriptiveKind(), parentParamTy, paramTy)
        .highlight(repr->getSourceRange());

    if (auto iuoRepr = dyn_cast<ImplicitlyUnwrappedOptionalTypeRepr>(repr)) {
      diags
          .diagnose(iuoRepr->getExclamationLoc(),
                    diag::override_unnecessary_IUO_remove)
          .fixItRemove(iuoRepr->getExclamationLoc());
    }

    diags.diagnose(repr->getStartLoc(), diag::override_unnecessary_IUO_silence)
        .fixItInsert(repr->getStartLoc(), "(")
        .fixItInsertAfter(repr->getEndLoc(), ")");
  };

  // FIXME: If we ever allow argument reordering, this is incorrect.
  ArrayRef<ParamDecl *> sharedParams = params->getArray();
  ArrayRef<ParamDecl *> sharedParentParams = parentParams->getArray();
  assert(sharedParams.size() == sharedParentParams.size());
  for_each(sharedParams, sharedParentParams, checkParam);

  if (!resultTL.getTypeRepr())
    return emittedError;

  auto checkResult = [&](TypeLoc resultTL, Type parentResultTy) {
    Type resultTy = resultTL.getType();
    if (!resultTy || !parentResultTy)
      return;

    if (!resultTy->getOptionalObjectType())
      return;

    TypeRepr *TR = resultTL.getTypeRepr();

    bool resultIsPlainOptional = true;
    if (member->isImplicitlyUnwrappedOptional())
      resultIsPlainOptional = false;

    if (resultIsPlainOptional || treatIUOResultAsError) {
      if (parentResultTy->getOptionalObjectType())
        return;
      emittedError = true;
      auto diag = diags.diagnose(resultTL.getSourceRange().Start,
                                 diag::override_optional_result_mismatch,
                                 member->getDescriptiveKind(),
                                 isa<SubscriptDecl>(member),
                                 parentResultTy, resultTy);
      if (auto optForm = dyn_cast<OptionalTypeRepr>(TR)) {
        diag.fixItRemove(optForm->getQuestionLoc());
      } else if (auto iuoForm =
          dyn_cast<ImplicitlyUnwrappedOptionalTypeRepr>(TR)) {
        diag.fixItRemove(iuoForm->getExclamationLoc());
      }
      return;
    }

    if (!parentResultTy->getOptionalObjectType())
      return;

    // Allow silencing this warning using parens.
    if (resultTy->hasParenSugar())
      return;

    diags.diagnose(resultTL.getSourceRange().Start,
                   diag::override_unnecessary_result_IUO,
                   member->getDescriptiveKind(), parentResultTy, resultTy)
      .highlight(resultTL.getSourceRange());

    auto sugaredForm = dyn_cast<ImplicitlyUnwrappedOptionalTypeRepr>(TR);
    if (sugaredForm) {
      diags.diagnose(sugaredForm->getExclamationLoc(),
                     diag::override_unnecessary_IUO_use_strict)
        .fixItReplace(sugaredForm->getExclamationLoc(), "?");
    }

    diags.diagnose(resultTL.getSourceRange().Start,
                   diag::override_unnecessary_IUO_silence)
      .fixItInsert(resultTL.getSourceRange().Start, "(")
      .fixItInsertAfter(resultTL.getSourceRange().End, ")");
  };

  checkResult(resultTL, parentTy->getResult());
  return emittedError;
}

/// Record that the \c overriding declarations overrides the
/// \c overridden declaration.
///
/// \returns true if an error occurred.
static bool checkSingleOverride(ValueDecl *override, ValueDecl *base);

/// If the difference between the types of \p decl and \p base is something
/// we feel confident about fixing (even partially), emit a note with fix-its
/// attached. Otherwise, no note will be emitted.
///
/// \returns true iff a diagnostic was emitted.
static bool noteFixableMismatchedTypes(ValueDecl *decl, const ValueDecl *base) {
  auto &ctx = decl->getASTContext();
  auto &diags = ctx.Diags;

  Type baseTy = base->getInterfaceType();
  if (baseTy->hasError())
    return false;

  if (auto *baseInit = dyn_cast<ConstructorDecl>(base)) {
    // Special-case initializers, whose "type" isn't useful besides the
    // input arguments.
    auto *fnType = baseTy->getAs<AnyFunctionType>();
    baseTy = fnType->getResult();
    Type argTy = FunctionType::composeTuple(
        ctx, baseTy->getAs<AnyFunctionType>()->getParams(),
        ParameterFlagHandling::IgnoreNonEmpty);
    auto diagKind = diag::override_type_mismatch_with_fixits_init;
    unsigned numArgs = baseInit->getParameters()->size();
    return computeFixitsForOverriddenDeclaration(
        decl, base, [&](bool HasNotes) -> Optional<InFlightDiagnostic> {
          if (!HasNotes)
            return None;
          return diags.diagnose(decl, diagKind,
                                /*plural*/ std::min(numArgs, 2U), argTy);
        });
  } else {
    if (isa<AbstractFunctionDecl>(base))
      baseTy = baseTy->getAs<AnyFunctionType>()->getResult();

    return computeFixitsForOverriddenDeclaration(
        decl, base, [&](bool HasNotes) -> Optional<InFlightDiagnostic> {
          if (!HasNotes)
            return None;
          return diags.diagnose(decl, diag::override_type_mismatch_with_fixits,
                                base->getDescriptiveKind(), baseTy);
        });
  }

  return false;
}

namespace {
  enum class OverrideCheckingAttempt {
    PerfectMatch,
    MismatchedSendability,
    MismatchedOptional,
    MismatchedTypes,
    BaseName,
    BaseNameWithMismatchedOptional,
    Final
  };

  OverrideCheckingAttempt &operator++(OverrideCheckingAttempt &attempt) {
    assert(attempt != OverrideCheckingAttempt::Final);
    attempt = static_cast<OverrideCheckingAttempt>(1+static_cast<int>(attempt));
    return attempt;
  }

  struct OverrideMatch {
    ValueDecl *Decl;
    bool IsExact;
  };
}

static void diagnoseGeneralOverrideFailure(ValueDecl *decl,
                                           ArrayRef<OverrideMatch> matches,
                                           OverrideCheckingAttempt attempt) {
  auto &diags = decl->getASTContext().Diags;

  switch (attempt) {
  case OverrideCheckingAttempt::PerfectMatch:
    diags.diagnose(decl, diag::override_multiple_decls_base,
                   decl->getName());
    break;
  case OverrideCheckingAttempt::MismatchedSendability: {
    SendableCheckContext fromContext(decl->getDeclContext(),
                                     SendableCheck::Explicit);
    auto baseDeclClass =
        decl->getOverriddenDecl()->getDeclContext()->getSelfClassDecl();

    diagnoseSendabilityErrorBasedOn(baseDeclClass, fromContext,
                                    [&](DiagnosticBehavior limit) {
      diags.diagnose(decl, diag::override_sendability_mismatch, decl->getName())
          .limitBehavior(limit);
      return false;
    });
    break;
  }
  case OverrideCheckingAttempt::BaseName:
    diags.diagnose(decl, diag::override_multiple_decls_arg_mismatch,
                   decl->getName());
    break;
  case OverrideCheckingAttempt::MismatchedOptional:
  case OverrideCheckingAttempt::MismatchedTypes:
  case OverrideCheckingAttempt::BaseNameWithMismatchedOptional: {
    auto isClassContext = decl->getDeclContext()->getSelfClassDecl() != nullptr;
    auto diag = diag::method_does_not_override;
    if (isa<ConstructorDecl>(decl))
      diag = diag::initializer_does_not_override;
    else if (isa<SubscriptDecl>(decl))
      diag = diag::subscript_does_not_override;
    else if (isa<VarDecl>(decl))
      diag = diag::property_does_not_override;
    diags.diagnose(decl, diag, isClassContext);
    break;
  }
  case OverrideCheckingAttempt::Final:
    llvm_unreachable("should have exited already");
  }

  for (auto match : matches) {
    auto matchDecl = match.Decl;
    if (attempt == OverrideCheckingAttempt::PerfectMatch) {
      diags.diagnose(matchDecl, diag::overridden_here);
      continue;
    }

    auto diag = diags.diagnose(matchDecl, diag::overridden_near_match_here,
                               matchDecl->getDescriptiveKind(),
                               matchDecl->getName());
    if (attempt == OverrideCheckingAttempt::BaseName) {
      fixDeclarationName(diag, decl, matchDecl->getName());
    }
  }
}

static bool parameterTypesMatch(const ValueDecl *derivedDecl,
                                const ValueDecl *baseDecl,
                                TypeMatchOptions matchMode) {
  const ParameterList *derivedParams = nullptr;
  const ParameterList *baseParams = nullptr;
  if ((isa<AbstractFunctionDecl>(derivedDecl) &&
       isa<AbstractFunctionDecl>(baseDecl)) ||
      isa<SubscriptDecl>(baseDecl)) {
    derivedParams = getParameterList(const_cast<ValueDecl *>(derivedDecl));
    baseParams = getParameterList(const_cast<ValueDecl *>(baseDecl));
  }

  if (!derivedParams && !baseParams) {
    return false;
  }

  if (baseParams->size() != derivedParams->size())
    return false;

  auto subs = SubstitutionMap::getOverrideSubstitutions(baseDecl, derivedDecl);

  for (auto i : indices(baseParams->getArray())) {
    auto *baseParam = baseParams->get(i);
    auto *derivedParam = derivedParams->get(i);

    // Make sure inout-ness and varargs match.
    if (baseParam->isInOut() != derivedParam->isInOut() ||
        baseParam->isVariadic() != derivedParam->isVariadic()) {
      return false;
    }

    auto baseParamTy = baseParam->getInterfaceType();
    baseParamTy = baseParamTy.subst(subs);
    auto derivedParamTy = derivedParam->getInterfaceType();

    if (baseParam->isInOut() || baseParam->isVariadic()) {
      // Inout and vararg parameters must match exactly.
      if (baseParamTy->isEqual(derivedParamTy))
        continue;
    } else {
      // Attempt contravariant match.
      if (baseParamTy->matchesParameter(derivedParamTy, matchMode))
        continue;

      // Try once more for a match, using the underlying type of an
      // IUO if we're allowing that.
      if (baseParam->isImplicitlyUnwrappedOptional() &&
          matchMode.contains(TypeMatchFlags::AllowNonOptionalForIUOParam)) {
        baseParamTy = baseParamTy->getOptionalObjectType();
        if (baseParamTy->matches(derivedParamTy, matchMode))
          continue;
      }
    }

    // If there is no match, then we're done.
    return false;
  }

  return true;
}

/// Returns true if `derivedDecl` has a `@differentiable` attribute that
/// overrides one from `baseDecl`.
static bool hasOverridingDifferentiableAttribute(ValueDecl *derivedDecl,
                                                 ValueDecl *baseDecl) {
  ASTContext &ctx = derivedDecl->getASTContext();
  auto &diags = ctx.Diags;

  auto *derivedAFD = dyn_cast<AbstractFunctionDecl>(derivedDecl);
  auto *baseAFD = dyn_cast<AbstractFunctionDecl>(baseDecl);

  if (!derivedAFD || !baseAFD)
    return false;

  auto derivedDAs =
      derivedAFD->getAttrs()
          .getAttributes<DifferentiableAttr, /*AllowInvalid*/ true>();
  auto baseDAs = baseAFD->getAttrs().getAttributes<DifferentiableAttr>();

  // Make sure all the `@differentiable` attributes on `baseDecl` are
  // also declared on `derivedDecl`.
  bool diagnosed = false;
  for (auto *baseDA : baseDAs) {
    auto baseParameters = baseDA->getParameterIndices();
    auto defined = false;
    for (auto derivedDA : derivedDAs) {
      auto derivedParameters = derivedDA->getParameterIndices();
      // If base and derived parameter indices are both defined, check whether
      // base parameter indices are a subset of derived parameter indices.
      if (derivedParameters && baseParameters &&
          baseParameters->isSubsetOf(derivedParameters)) {
        defined = true;
        break;
      }
      // Parameter indices may not be resolved because override matching happens
      // before attribute checking for declaration type-checking.
      // If parameter indices have not been resolved, avoid emitting diagnostic.
      // Assume that attributes are valid.
      if (!derivedParameters || !baseParameters) {
        defined = true;
        break;
      }
    }
    if (defined)
      continue;
    diagnosed = true;
    // Emit an error and fix-it showing the missing base declaration's
    // `@differentiable` attribute.
    // Omit printing `wrt:` clause if attribute's differentiability parameters
    // match inferred differentiability parameters.
    auto *inferredParameters =
        TypeChecker::inferDifferentiabilityParameters(derivedAFD, nullptr);
    bool omitWrtClause =
        !baseParameters ||
        baseParameters->getNumIndices() == inferredParameters->getNumIndices();
    // Get `@differentiable` attribute description.
    std::string baseDiffAttrString;
    llvm::raw_string_ostream os(baseDiffAttrString);
    baseDA->print(os, derivedDecl, omitWrtClause);
    os.flush();
    diags
        .diagnose(derivedDecl,
                  diag::overriding_decl_missing_differentiable_attr,
                  baseDiffAttrString)
        .fixItInsert(derivedDecl->getStartLoc(), baseDiffAttrString + ' ');
    diags.diagnose(baseDecl, diag::overridden_here);
  }
  // If a diagnostic was produced, return false.
  if (diagnosed)
    return false;

  // If there is no `@differentiable` attribute in `derivedDecl`, then
  // overriding is not allowed.
  auto *derivedDC = derivedDecl->getDeclContext();
  auto *baseDC = baseDecl->getDeclContext();
  if (derivedDC->getSelfClassDecl() && baseDC->getSelfClassDecl())
    return false;

  // Finally, go through all `@differentiable` attributes in `derivedDecl` and
  // check if they subsume any of the `@differentiable` attributes in
  // `baseDecl`.
  for (auto derivedDA : derivedDAs) {
    auto derivedParameters = derivedDA->getParameterIndices();
    auto overrides = true;
    for (auto baseDA : baseDAs) {
      auto baseParameters = baseDA->getParameterIndices();
      // If the parameter indices of `derivedDA` are a subset of those of
      // `baseDA`, then `baseDA` subsumes `derivedDA` and the function is
      // marked as overridden.
      if (derivedParameters && baseParameters &&
          derivedParameters->isSubsetOf(baseParameters)) {
        overrides = false;
        break;
      }
    }
    if (overrides)
      return true;
  }

  return false;
}

/// Returns true if the given declaration is for the `NSObject.hashValue`
/// property.
static bool isNSObjectHashValue(ValueDecl *baseDecl) {
  ASTContext &ctx = baseDecl->getASTContext();

  if (auto baseVar = dyn_cast<VarDecl>(baseDecl)) {
    if (auto classDecl = baseVar->getDeclContext()->getSelfClassDecl()) {
      return baseVar->getName() == ctx.Id_hashValue &&
             classDecl->isNSObject();
    }
  }
  return false;
}

/// Returns true if the given declaration is for the `NSObject.hash(into:)`
/// function.
static bool isNSObjectHashMethod(ValueDecl *baseDecl) {
  auto baseFunc = dyn_cast<FuncDecl>(baseDecl);
  if (!baseFunc)
    return false;

  if (auto classDecl = baseFunc->getDeclContext()->getSelfClassDecl()) {
    ASTContext &ctx = baseDecl->getASTContext();
    return baseFunc->getBaseName() == ctx.Id_hash && classDecl->isNSObject();
  }
  return false;
}

namespace {
  /// Class that handles the checking of a particular declaration against
  /// superclass entities that it could override.
  class OverrideMatcher {
    ASTContext &ctx;
    ValueDecl *decl;

    /// The set of declarations in which we'll look for overridden
    /// methods.
    SmallVector<NominalTypeDecl *, 2> superContexts;

    /// Cached member lookup results.
    SmallVector<ValueDecl *, 4> members;

    /// The lookup name used to find \c members.
    DeclName membersName;

    /// The type of the declaration, cached here once it has been computed.
    Type cachedDeclType;

  public:
    OverrideMatcher(ValueDecl *decl);

    /// Returns true when it's possible to perform any override matching.
    explicit operator bool() const {
      return !superContexts.empty();
    }

    /// Whether this is an override of a class member.
    bool isClassOverride() const {
      return decl->getDeclContext()->getSelfClassDecl() != nullptr;
    }

    /// Whether this is an override of a protocol member.
    bool isProtocolOverride() const {
      return decl->getDeclContext()->getSelfProtocolDecl() != nullptr;
    }

    /// Match this declaration against potential members in the superclass,
    /// using the heuristics appropriate for the given \c attempt.
    SmallVector<OverrideMatch, 2> match(OverrideCheckingAttempt attempt);

    /// Check each of the given matches, returning only those that
    /// succeeded.
    TinyPtrVector<ValueDecl *> checkPotentialOverrides(
                                        SmallVectorImpl<OverrideMatch> &matches,
                                        OverrideCheckingAttempt attempt);

  private:
    /// We have determined that we have an override of the given \c baseDecl.
    ///
    /// Check that the override itself is valid.
    bool checkOverride(ValueDecl *baseDecl,
                       OverrideCheckingAttempt attempt);

    /// Retrieve the type of the declaration, to be used in comparisons.
    Type getDeclComparisonType() {
      if (!cachedDeclType) {
        cachedDeclType = getMemberTypeForComparison(decl);
      }

      return cachedDeclType;
    }

    /// Adjust the interface of the given declaration, which is found in
    /// a supertype of the given type.
    Type getSuperMemberDeclType(ValueDecl *baseDecl) const {
      auto selfType = decl->getDeclContext()->getSelfInterfaceType();
      if (selfType->getClassOrBoundGenericClass()) {
        selfType = selfType->getSuperclass();
        assert(selfType && "No superclass type?");
      }

      return selfType->adjustSuperclassMemberDeclType(
               baseDecl, decl, baseDecl->getInterfaceType());
    }
  };
}

OverrideMatcher::OverrideMatcher(ValueDecl *decl)
    : ctx(decl->getASTContext()), decl(decl) {
  // The final step for this constructor is to set up the superclass type,
  // without which we will not perform an matching. Early exits therefore imply
  // that there is no way we can match this declaration.
  // FIXME: Break the cycle here.
  if (decl->hasInterfaceType() && decl->isInvalid())
    return;

  auto *dc = decl->getDeclContext();
  if (auto classDecl = dc->getSelfClassDecl()) {
    if (auto superclassDecl = classDecl->getSuperclassDecl())
      superContexts.push_back(superclassDecl);
  } else if (auto protocol = dyn_cast<ProtocolDecl>(dc)) {
    auto inheritedProtocols = protocol->getInheritedProtocols();
    superContexts.insert(superContexts.end(), inheritedProtocols.begin(),
                         inheritedProtocols.end());
  }
}

SmallVector<OverrideMatch, 2> OverrideMatcher::match(
                                             OverrideCheckingAttempt attempt) {
  // If there's no matching we can do, fail.
  if (!*this) return { };

  auto dc = decl->getDeclContext();

  // Determine what name we should look for.
  DeclName name;
  switch (attempt) {
  case OverrideCheckingAttempt::PerfectMatch:
  case OverrideCheckingAttempt::MismatchedSendability:
  case OverrideCheckingAttempt::MismatchedOptional:
  case OverrideCheckingAttempt::MismatchedTypes:
    name = decl->getName();
    break;
  case OverrideCheckingAttempt::BaseName:
  case OverrideCheckingAttempt::BaseNameWithMismatchedOptional:
    name = decl->getBaseName();
    break;
  case OverrideCheckingAttempt::Final:
    // Give up.
    return { };
  }

  // If we don't have members available yet, or we looked them up based on a
  // different name, look them up now.
  if (members.empty() || name != membersName) {
    membersName = name;
    members.clear();
    // FIXME: This suggests we need to use TypeChecker's high-level lookup
    // entrypoints.  But first we need one that supports additive qualified
    // lookup.
    for (auto *ctx : superContexts) {
      ctx->synthesizeSemanticMembersIfNeeded(membersName);
    }
    dc->lookupQualified(superContexts, DeclNameRef(membersName),
                        decl->getLoc(), NL_QualifiedDefault, members);
  }

  // Check each member we found.
  SmallVector<OverrideMatch, 2> matches;
  for (auto parentDecl : members) {
    // Check whether there are any obvious reasons why the two given
    // declarations do not have an overriding relationship.
    if (!areOverrideCompatibleSimple(decl, parentDecl))
      continue;

    // Check whether the derived declaration has a `@differentiable` attribute
    // that overrides one from the parent declaration.
    if (hasOverridingDifferentiableAttribute(decl, parentDecl))
      continue;

    auto parentMethod = dyn_cast<AbstractFunctionDecl>(parentDecl);
    auto parentStorage = dyn_cast<AbstractStorageDecl>(parentDecl);
    assert(parentMethod || parentStorage);
    (void)parentMethod;
    (void)parentStorage;

    // If the generic requirements don't match, don't try anything else below,
    // because it will compute an invalid interface type by applying malformed
    // substitutions.
    if (isClassOverride()) {
      using Direction = ASTContext::OverrideGenericSignatureReqCheck;
      if (decl->getAsGenericContext()) {
        if (!ctx.overrideGenericSignatureReqsSatisfied(
                parentDecl, decl, Direction::DerivedReqSatisfiedByBase)) {
          continue;
        }
      }
    }

    // Check whether the types are identical.
    Type declTy = getDeclComparisonType();
    if (isOverrideBasedOnType(decl, declTy, parentDecl)) {
      matches.push_back({parentDecl, true});
      continue;
    }

    // If this is a property, we accept the match and then reject it below
    // if the types don't line up, since you can't overload properties based
    // on types.
    if ((isa<VarDecl>(parentDecl) && isClassOverride()) ||
        attempt == OverrideCheckingAttempt::MismatchedTypes) {
      matches.push_back({parentDecl, false});
      continue;
    }

    // For a protocol override, we require an exact match.
    if (isProtocolOverride()) {
      continue;
    }

    // Failing that, check for subtyping.
    TypeMatchOptions matchMode = TypeMatchFlags::AllowOverride;
    if (attempt == OverrideCheckingAttempt::MismatchedOptional ||
        attempt == OverrideCheckingAttempt::BaseNameWithMismatchedOptional){
      matchMode |= TypeMatchFlags::AllowTopLevelOptionalMismatch;
    } else if (parentDecl->isObjC()) {
      matchMode |= TypeMatchFlags::AllowNonOptionalForIUOParam;
      matchMode |= TypeMatchFlags::IgnoreNonEscapingForOptionalFunctionParam;
    }
    if (attempt == OverrideCheckingAttempt::MismatchedSendability)
      matchMode |= TypeMatchFlags::IgnoreFunctionSendability;

    auto declFnTy = getDeclComparisonType()->getAs<AnyFunctionType>();
    auto parentDeclTy = getMemberTypeForComparison(parentDecl, decl);
    auto parentDeclFnTy = parentDeclTy->getAs<AnyFunctionType>();
    if (declFnTy && parentDeclFnTy) {
      auto paramsAndResultMatch = [=]() -> bool {
        return parameterTypesMatch(decl, parentDecl, matchMode) &&
               declFnTy->getResult()->matches(parentDeclFnTy->getResult(),
                                              matchMode);
      };

      if (declFnTy->matchesFunctionType(parentDeclFnTy, matchMode,
                                        paramsAndResultMatch)) {
        matches.push_back({parentDecl, false});
        continue;
      }
    } else if (getDeclComparisonType()->matches(parentDeclTy, matchMode)) {
      matches.push_back({parentDecl, false});
      continue;
    }
  }

  // If we have more than one match, and any of them was exact, remove all
  // non-exact matches.
  if (matches.size() > 1) {
    bool hadExactMatch = std::find_if(matches.begin(), matches.end(),
                                      [](const OverrideMatch &match) {
                                        return match.IsExact;
                                      }) != matches.end();
    if (hadExactMatch) {
      matches.erase(std::remove_if(matches.begin(), matches.end(),
                                   [&](const OverrideMatch &match) {
                                     return !match.IsExact;
                                   }),
                    matches.end());
    }
  }

  return matches;
}

static void checkOverrideAccessControl(ValueDecl *baseDecl, ValueDecl *decl,
                                       ASTContext &ctx) {
  if (ctx.isAccessControlDisabled())
    return;

  if (isa<ProtocolDecl>(decl->getDeclContext()))
    return;

  auto &diags = ctx.Diags;

  auto dc = decl->getDeclContext();
  auto classDecl = dc->getSelfClassDecl();
  assert(classDecl != nullptr && "Should have ruled out protocols above");

  bool isAccessor = isa<AccessorDecl>(decl);

  // Check that the override has the required access level.
  // Overrides have to be at least as accessible as what they
  // override, except:
  //   - they don't have to be more accessible than their class and
  //   - a final method may be public instead of open.
  // Also diagnose attempts to override a non-open method from outside its
  // defining module.  This is not required for constructors, which are
  // never really "overridden" in the intended sense here, because of
  // course derived classes will change how the class is initialized.
  bool baseHasOpenAccess = baseDecl->hasOpenAccess(dc);
  if (!isAccessor &&
      !baseHasOpenAccess &&
      baseDecl->getModuleContext() != decl->getModuleContext() &&
      !isa<ConstructorDecl>(decl)) {
    // NSObject.hashValue and NSObject.hash(into:) are not overridable;
    // one should override NSObject.hash instead.
    if (isNSObjectHashValue(baseDecl)) {
      decl->diagnose(diag::override_nsobject_hashvalue_error)
        .fixItReplace(SourceRange(decl->getNameLoc()), "hash");
    } else if (isNSObjectHashMethod(baseDecl)) {
      decl->diagnose(diag::override_nsobject_hash_error)
        .fixItReplace(cast<FuncDecl>(decl)->getFuncLoc(), getTokenText(tok::kw_var))
        .fixItReplace(cast<FuncDecl>(decl)->getParameters()->getSourceRange(), ": Int");
    } else {
      diags.diagnose(decl, diag::override_of_non_open,
                     decl->getDescriptiveKind());
    }
  } else if (baseHasOpenAccess &&
             classDecl->hasOpenAccess(dc) &&
             decl->getFormalAccess() < AccessLevel::Public &&
             !decl->isSemanticallyFinal()) {
    {
      auto diag = diags.diagnose(decl, diag::override_not_accessible,
                                 /*setter*/false,
                                 decl->getDescriptiveKind(),
                                 /*fromOverridden*/true);
      fixItAccess(diag, decl, AccessLevel::Open);
    }
    diags.diagnose(baseDecl, diag::overridden_here);

  } else if (!isa<ConstructorDecl>(decl)) {
    auto matchAccessScope =
      baseDecl->getFormalAccessScope(dc);
    auto classAccessScope =
      classDecl->getFormalAccessScope(dc);
    auto requiredAccessScope =
      matchAccessScope.intersectWith(classAccessScope);
    auto scopeDC = requiredAccessScope->getDeclContext();

    bool shouldDiagnose = !decl->isAccessibleFrom(scopeDC);

    bool shouldDiagnoseSetter = false;
    if (auto matchASD = dyn_cast<AbstractStorageDecl>(baseDecl)) {
      if (!shouldDiagnose && matchASD->isSettable(dc)){
        if (matchASD->isSetterAccessibleFrom(dc)) {
          auto matchSetterAccessScope =
            matchASD->getSetterFormalAccessScope(dc);
          auto requiredSetterAccessScope =
            matchSetterAccessScope.intersectWith(classAccessScope);
          auto setterScopeDC = requiredSetterAccessScope->getDeclContext();

          const auto *ASD = cast<AbstractStorageDecl>(decl);
          shouldDiagnoseSetter =
              ASD->isSettable(setterScopeDC) &&
              !ASD->isSetterAccessibleFrom(setterScopeDC);
        }
      }
    }

    if (shouldDiagnose || shouldDiagnoseSetter) {
      bool overriddenForcesAccess =
        (requiredAccessScope->hasEqualDeclContextWith(matchAccessScope) &&
         !baseHasOpenAccess);
      AccessLevel requiredAccess =
        requiredAccessScope->requiredAccessForDiagnostics();
      {
        auto diag = diags.diagnose(decl, diag::override_not_accessible,
                                   shouldDiagnoseSetter,
                                   decl->getDescriptiveKind(),
                                   overriddenForcesAccess);
        fixItAccess(diag, decl, requiredAccess, shouldDiagnoseSetter);
      }
      diags.diagnose(baseDecl, diag::overridden_here);
    }
  }
}

bool OverrideMatcher::checkOverride(ValueDecl *baseDecl,
                                    OverrideCheckingAttempt attempt) {
  auto &diags = ctx.Diags;
  auto baseTy = getMemberTypeForComparison(baseDecl, decl);
  bool emittedMatchError = false;

  // If the name of our match differs from the name we were looking for,
  // complain.
  if (decl->getName() != baseDecl->getName()) {
    auto diag = diags.diagnose(decl, diag::override_argument_name_mismatch,
                               isa<ConstructorDecl>(decl),
                               decl->getName(),
                               baseDecl->getName());
    fixDeclarationName(diag, decl, baseDecl->getName());
    emittedMatchError = true;
  }

  // If we have an explicit ownership modifier and our parent doesn't,
  // complain.
  auto parentAttr =
      baseDecl->getAttrs().getAttribute<ReferenceOwnershipAttr>();
  if (auto ownershipAttr =
          decl->getAttrs().getAttribute<ReferenceOwnershipAttr>()) {
    ReferenceOwnership parentOwnership;
    if (parentAttr)
      parentOwnership = parentAttr->get();
    else
      parentOwnership = ReferenceOwnership::Strong;
    if (parentOwnership != ownershipAttr->get()) {
      diags.diagnose(decl, diag::override_ownership_mismatch,
                     parentOwnership, ownershipAttr->get());
      diags.diagnose(baseDecl, diag::overridden_here);
    }
  }

  // If a super method returns Self, and the subclass overrides it to
  // instead return the subclass type, complain.
  // This case gets this far because the type matching above specifically
  // strips out dynamic self via replaceCovariantResultType(), and that
  // is helpful in several cases - just not this one.
  auto dc = decl->getDeclContext();
  auto classDecl = dc->getSelfClassDecl();
  if (decl->getASTContext().isSwiftVersionAtLeast(5) &&
      baseDecl->getInterfaceType()->hasDynamicSelfType() &&
      !decl->getInterfaceType()->hasDynamicSelfType() &&
      !classDecl->isSemanticallyFinal()) {
    diags.diagnose(decl, diag::override_dynamic_self_mismatch);
    diags.diagnose(baseDecl, diag::overridden_here);
  }

  checkOverrideAccessControl(baseDecl, decl, ctx);

  bool mayHaveMismatchedOptionals =
      (attempt == OverrideCheckingAttempt::MismatchedOptional ||
       attempt == OverrideCheckingAttempt::BaseNameWithMismatchedOptional);

  auto declIUOAttr = decl->isImplicitlyUnwrappedOptional();
  auto matchDeclIUOAttr = baseDecl->isImplicitlyUnwrappedOptional();

  // If this is an exact type match, we're successful!
  Type declTy = getDeclComparisonType();
  Type owningTy = dc->getDeclaredInterfaceType();
  auto isClassContext = classDecl != nullptr;
  if (declIUOAttr == matchDeclIUOAttr && declTy->isEqual(baseTy)) {
    // Nothing to do.

  } else if (auto method = dyn_cast<AbstractFunctionDecl>(decl)) {
    if (attempt == OverrideCheckingAttempt::MismatchedTypes) {
      auto diagKind = diag::method_does_not_override;
      if (isa<ConstructorDecl>(method))
        diagKind = diag::initializer_does_not_override;
      diags.diagnose(decl, diagKind, isClassContext);
      noteFixableMismatchedTypes(decl, baseDecl);
      diags.diagnose(baseDecl, diag::overridden_near_match_here,
                     baseDecl->getDescriptiveKind(),
                     baseDecl->getName());
      emittedMatchError = true;

    } else if (!isa<AccessorDecl>(method) &&
               (baseDecl->isObjC() || mayHaveMismatchedOptionals)) {
      // Private migration help for overrides of Objective-C methods.
      TypeLoc resultTL;
      if (auto *methodAsFunc = dyn_cast<FuncDecl>(method))
        resultTL = TypeLoc(methodAsFunc->getResultTypeRepr(),
                           methodAsFunc->getResultInterfaceType());

      emittedMatchError |= diagnoseMismatchedOptionals(
          method, method->getParameters(), resultTL, baseDecl,
          cast<AbstractFunctionDecl>(baseDecl)->getParameters(),
          owningTy, mayHaveMismatchedOptionals);
    }
  } else if (auto subscript = dyn_cast<SubscriptDecl>(decl)) {
    // Otherwise, if this is a subscript, validate that covariance is ok.
    // If the parent is non-mutable, it's okay to be covariant.
    auto parentSubscript = cast<SubscriptDecl>(baseDecl);
    if (parentSubscript->supportsMutation() &&
        attempt != OverrideCheckingAttempt::MismatchedTypes) {
      diags.diagnose(subscript, diag::override_mutable_covariant_subscript,
                     declTy, baseTy);
      diags.diagnose(baseDecl, diag::subscript_override_here);
      return true;
    }

    if (attempt == OverrideCheckingAttempt::MismatchedTypes) {
      diags.diagnose(decl, diag::subscript_does_not_override, isClassContext);
      noteFixableMismatchedTypes(decl, baseDecl);
      diags.diagnose(baseDecl, diag::overridden_near_match_here,
                     baseDecl->getDescriptiveKind(),
                     baseDecl->getName());
      emittedMatchError = true;

    } else if (mayHaveMismatchedOptionals) {
      TypeLoc elementTL(subscript->getElementTypeRepr(),
                        subscript->getElementInterfaceType());
      emittedMatchError |= diagnoseMismatchedOptionals(
          subscript, subscript->getIndices(),
          elementTL, baseDecl,
          cast<SubscriptDecl>(baseDecl)->getIndices(), owningTy,
          mayHaveMismatchedOptionals);
    }
  } else if (auto property = dyn_cast<VarDecl>(decl)) {
    auto propertyTy = property->getInterfaceType();
    auto parentPropertyTy = getSuperMemberDeclType(baseDecl);

    CanType parentPropertyCanTy =
      parentPropertyTy->getReducedType(
        decl->getInnermostDeclContext()->getGenericSignatureOfContext());
    if (!propertyTy->matches(parentPropertyCanTy,
                             TypeMatchFlags::AllowOverride)) {
      diags.diagnose(property, diag::override_property_type_mismatch,
                     property->getName(), propertyTy, parentPropertyTy);
      noteFixableMismatchedTypes(decl, baseDecl);
      diags.diagnose(baseDecl, diag::property_override_here);
      return true;
    }

    // Differing only in Optional vs. ImplicitlyUnwrappedOptional is fine.
    bool IsSilentDifference = false;
    if (auto propertyTyNoOptional = propertyTy->getOptionalObjectType())
      if (auto parentPropertyTyNoOptional =
              parentPropertyTy->getOptionalObjectType())
        if (propertyTyNoOptional->isEqual(parentPropertyTyNoOptional))
          IsSilentDifference = true;

    // The overridden property must not be mutable.
    if (cast<AbstractStorageDecl>(baseDecl)->supportsMutation() &&
        !IsSilentDifference) {
      diags.diagnose(property, diag::override_mutable_covariant_property,
                  property->getName(), parentPropertyTy, propertyTy);
      diags.diagnose(baseDecl, diag::property_override_here);
      return true;
    }
  }

  if (emittedMatchError)
    return true;

  if (attempt == OverrideCheckingAttempt::MismatchedSendability) {
    SendableCheckContext fromContext(decl->getDeclContext(),
                                     SendableCheck::Explicit);
    auto baseDeclClass = baseDecl->getDeclContext()->getSelfClassDecl();

    diagnoseSendabilityErrorBasedOn(baseDeclClass, fromContext,
                                    [&](DiagnosticBehavior limit) {
      diags.diagnose(decl, diag::override_sendability_mismatch,
                     decl->getName())
        .limitBehavior(limit);
      diags.diagnose(baseDecl, diag::overridden_here)
        .limitBehavior(limit);
      return false;
    });
  }
  // Catch-all to make sure we don't silently accept something we shouldn't.
  else if (attempt != OverrideCheckingAttempt::PerfectMatch) {
    OverrideMatch match{decl, /*isExact=*/false};
    diagnoseGeneralOverrideFailure(decl, match, attempt);
  }

  return checkSingleOverride(decl, baseDecl);
}

// Invalidate an existing "override" attribute or add a new invalid "override"
// attribute, which will suppress additional checking.
static void invalidateOverrideAttribute(ValueDecl *decl) {
  auto overrideAttr = decl->getAttrs().getAttribute<OverrideAttr>(true);
  if (!overrideAttr) {
    overrideAttr = new (decl->getASTContext()) OverrideAttr(true);
    decl->getAttrs().add(overrideAttr);
  }

  overrideAttr->setInvalid();
}

TinyPtrVector<ValueDecl *> OverrideMatcher::checkPotentialOverrides(
                                    SmallVectorImpl<OverrideMatch> &matches,
                                    OverrideCheckingAttempt attempt) {
  // If we override more than one declaration from a class, complain.
  if (matches.size() > 1 && decl->getDeclContext()->getSelfClassDecl()) {
    diagnoseGeneralOverrideFailure(decl, matches, attempt);
    return { };
  }

  // Check the matches. If any are ill-formed, drop them.
  TinyPtrVector<ValueDecl *> overridden;
  for (const auto &match : matches) {
    if (checkOverride(match.Decl, attempt))
      continue;

    overridden.push_back(match.Decl);
  }

  // If there were no overrides, invalidate the "override" attribute.
  if (overridden.empty())
    invalidateOverrideAttribute(decl);

  return overridden;
}

/// Determine which method or subscript this method or subscript overrides
/// (if any).
///
/// \returns true if an error occurred.
bool swift::checkOverrides(ValueDecl *decl) {
  // If there is a @_nonoverride attribute, this does not override anything.
  if (decl->getAttrs().hasAttribute<NonOverrideAttr>())
    return false;

  // If we already computed overridden declarations and either succeeded
  // or invalidated the attribute, there's nothing more to do.
  if (decl->overriddenDeclsComputed()) {
    // If we computed an overridden declaration successfully, we're done.
    if (decl->getOverriddenDecl())
      return false;

    // If we set the override attribute to "invalid", we already diagnosed
    // something here.
    if (decl->getAttrs().hasAttribute<OverrideAttr>(/*AllowInvalid=*/true) &&
        !decl->getAttrs().hasAttribute<OverrideAttr>())
      return true;

    // Otherwise, we have more checking to do.
  }

  // Members of constrained extensions are not considered to be overrides.
  if (auto *ext = dyn_cast<ExtensionDecl>(decl->getDeclContext()))
    if (ext->isConstrainedExtension())
      return false;

  // Accessor methods get overrides through their storage declaration, and
  // all checking can be performed via that mechanism.
  if (isa<AccessorDecl>(decl)) {
    (void)decl->getOverriddenDecls();
    return false;
  }

  // Set up matching, but bail out if there's nothing to match.
  OverrideMatcher matcher(decl);
  if (!matcher) return false;

  // Look for members with the same name and matching types as this
  // one.
  SmallVector<OverrideMatch, 2> matches;
  auto attempt = OverrideCheckingAttempt::PerfectMatch;
  do {
    // Determine whether we should attempt to perform matching now, or exit
    // early with a failure.
    switch (attempt) {
    case OverrideCheckingAttempt::PerfectMatch:
      break;
    case OverrideCheckingAttempt::MismatchedSendability:
      // Don't keep looking if the user didn't indicate it's an override.
      if (!decl->getAttrs().hasAttribute<OverrideAttr>())
        return false;
      break;
    case OverrideCheckingAttempt::MismatchedOptional:
    case OverrideCheckingAttempt::MismatchedTypes:
      break;
    case OverrideCheckingAttempt::BaseName:
      // Don't keep looking if this is already a simple name, or if there
      // are no arguments.
      if (decl->getName() == decl->getBaseName() ||
          decl->getName().getArgumentNames().empty())
        return false;
      break;
    case OverrideCheckingAttempt::BaseNameWithMismatchedOptional:
      break;
    case OverrideCheckingAttempt::Final:
      // Give up.
      return false;
    }

    // Try to match with this attempt kind.
    matches = matcher.match(attempt);
    if (!matches.empty())
      break;

    // If we're computing implicit overrides for a protocol member, don't
    // look any further unless there's an `override` attribute.
    if (matcher.isProtocolOverride() &&
        !decl->getAttrs().hasAttribute<OverrideAttr>())
      return false;

    // Try the next version.
    ++attempt;
  } while (true);

  assert(!matches.empty());

  // FIXME: Check for missing 'override' keyword here?

  // We performed override checking, so record the overrides.
  // FIXME: It's weird to be pushing state here, but how do we say that
  // this check subsumes the normal 'override' check?
  auto overridden = matcher.checkPotentialOverrides(matches, attempt);
  if (overridden.empty())
    invalidateOverrideAttribute(decl);
  decl->setOverriddenDecls(overridden);
  return false;
}

namespace  {
  /// Attribute visitor that checks how the given attribute should be
  /// considered when overriding a declaration.
  ///
  /// Note that the attributes visited are those of the base
  /// declaration, so if you need to check that the overriding
  /// declaration doesn't have an attribute if the base doesn't have
  /// it, this isn't sufficient.
  class AttributeOverrideChecker
          : public AttributeVisitor<AttributeOverrideChecker> {
    ValueDecl *Base;
    ValueDecl *Override;
    DiagnosticEngine &Diags;

  public:
    AttributeOverrideChecker(ValueDecl *base, ValueDecl *override)
      : Base(base), Override(override), Diags(base->getASTContext().Diags) { }

    /// Deleting this ensures that all attributes are covered by the visitor
    /// below.
    void visitDeclAttribute(DeclAttribute *A) = delete;

#define UNINTERESTING_ATTR(CLASS)                                              \
    void visit##CLASS##Attr(CLASS##Attr *) {}

    // Please keep these alphabetical.
    UNINTERESTING_ATTR(AccessControl)
    UNINTERESTING_ATTR(Alignment)
    UNINTERESTING_ATTR(AlwaysEmitIntoClient)
    UNINTERESTING_ATTR(Borrowed)
    UNINTERESTING_ATTR(Borrowing)
    UNINTERESTING_ATTR(CDecl)
    UNINTERESTING_ATTR(Consuming)
    UNINTERESTING_ATTR(Documentation)
    UNINTERESTING_ATTR(Dynamic)
    UNINTERESTING_ATTR(DynamicCallable)
    UNINTERESTING_ATTR(DynamicMemberLookup)
    UNINTERESTING_ATTR(SILGenName)
    UNINTERESTING_ATTR(Exported)
    UNINTERESTING_ATTR(ForbidSerializingReference)
    UNINTERESTING_ATTR(GKInspectable)
    UNINTERESTING_ATTR(HasMissingDesignatedInitializers)
    UNINTERESTING_ATTR(IBAction)
    UNINTERESTING_ATTR(IBDesignable)
    UNINTERESTING_ATTR(IBInspectable)
    UNINTERESTING_ATTR(IBOutlet)
    UNINTERESTING_ATTR(IBSegueAction)
    UNINTERESTING_ATTR(Indirect)
    UNINTERESTING_ATTR(InheritsConvenienceInitializers)
    UNINTERESTING_ATTR(Inline)
    UNINTERESTING_ATTR(Optimize)
    UNINTERESTING_ATTR(Exclusivity)
    UNINTERESTING_ATTR(NoLocks)
    UNINTERESTING_ATTR(NoAllocation)
    UNINTERESTING_ATTR(Inlinable)
    UNINTERESTING_ATTR(Effects)
    UNINTERESTING_ATTR(Expose)
    UNINTERESTING_ATTR(Final)
    UNINTERESTING_ATTR(MoveOnly)
    UNINTERESTING_ATTR(FixedLayout)
    UNINTERESTING_ATTR(Lazy)
    UNINTERESTING_ATTR(LegacyConsuming)
    UNINTERESTING_ATTR(LLDBDebuggerFunction)
    UNINTERESTING_ATTR(Mutating)
    UNINTERESTING_ATTR(NonMutating)
    UNINTERESTING_ATTR(NonEphemeral)
    UNINTERESTING_ATTR(NonObjC)
    UNINTERESTING_ATTR(NonOverride)
    UNINTERESTING_ATTR(NSApplicationMain)
    UNINTERESTING_ATTR(NSCopying)
    UNINTERESTING_ATTR(NSManaged)
    UNINTERESTING_ATTR(ObjCBridged)
    UNINTERESTING_ATTR(Optional)
    UNINTERESTING_ATTR(Override)
    UNINTERESTING_ATTR(RawDocComment)
    UNINTERESTING_ATTR(Required)
    UNINTERESTING_ATTR(Convenience)
    UNINTERESTING_ATTR(Semantics)
    UNINTERESTING_ATTR(EmitAssemblyVisionRemarks)
    UNINTERESTING_ATTR(SetterAccess)
    UNINTERESTING_ATTR(TypeEraser)
    UNINTERESTING_ATTR(SPIAccessControl)
    UNINTERESTING_ATTR(HasStorage)
    UNINTERESTING_ATTR(UIApplicationMain)
    UNINTERESTING_ATTR(UsableFromInline)
    UNINTERESTING_ATTR(ObjCNonLazyRealization)
    UNINTERESTING_ATTR(UnsafeNoObjCTaggedPointer)
    UNINTERESTING_ATTR(Used)
    UNINTERESTING_ATTR(Section)
    UNINTERESTING_ATTR(SwiftNativeObjCRuntimeBase)
    UNINTERESTING_ATTR(ShowInInterface)
    UNINTERESTING_ATTR(Specialize)
    UNINTERESTING_ATTR(SpecializeExtension)
    UNINTERESTING_ATTR(DynamicReplacement)
    UNINTERESTING_ATTR(PrivateImport)
    UNINTERESTING_ATTR(MainType)
    UNINTERESTING_ATTR(Preconcurrency)

    // Differentiation-related attributes.
    UNINTERESTING_ATTR(Differentiable)
    UNINTERESTING_ATTR(Derivative)
    UNINTERESTING_ATTR(Transpose)
    UNINTERESTING_ATTR(NoDerivative)

    // These can't appear on overridable declarations.
    UNINTERESTING_ATTR(Prefix)
    UNINTERESTING_ATTR(Postfix)
    UNINTERESTING_ATTR(Infix)
    UNINTERESTING_ATTR(ReferenceOwnership)

    UNINTERESTING_ATTR(SynthesizedProtocol)
    UNINTERESTING_ATTR(RequiresStoredPropertyInits)
    UNINTERESTING_ATTR(Transparent)
    UNINTERESTING_ATTR(Testable)

    UNINTERESTING_ATTR(WarnUnqualifiedAccess)
    UNINTERESTING_ATTR(DiscardableResult)

    UNINTERESTING_ATTR(ObjCImplementation)
    UNINTERESTING_ATTR(ObjCMembers)
    UNINTERESTING_ATTR(ObjCRuntimeName)
    UNINTERESTING_ATTR(RestatedObjCConformance)
    UNINTERESTING_ATTR(Initializes)
    UNINTERESTING_ATTR(Accesses)
    UNINTERESTING_ATTR(Implements)
    UNINTERESTING_ATTR(StaticInitializeObjCMetadata)
    UNINTERESTING_ATTR(ClangImporterSynthesizedType)
    UNINTERESTING_ATTR(WeakLinked)
    UNINTERESTING_ATTR(Frozen)
    UNINTERESTING_ATTR(HasInitialValue)
    UNINTERESTING_ATTR(ImplementationOnly)
    UNINTERESTING_ATTR(SPIOnly)
    UNINTERESTING_ATTR(Custom)
    UNINTERESTING_ATTR(PropertyWrapper)
    UNINTERESTING_ATTR(DisfavoredOverload)
    UNINTERESTING_ATTR(ResultBuilder)
    UNINTERESTING_ATTR(ProjectedValueProperty)
    UNINTERESTING_ATTR(OriginallyDefinedIn)
    UNINTERESTING_ATTR(Actor)
    UNINTERESTING_ATTR(DistributedActor)
    UNINTERESTING_ATTR(GlobalActor)
    UNINTERESTING_ATTR(Async)
    UNINTERESTING_ATTR(Sendable)
    UNINTERESTING_ATTR(NonSendable)

    UNINTERESTING_ATTR(AtRethrows)
    UNINTERESTING_ATTR(Marker)

    UNINTERESTING_ATTR(AtReasync)
    UNINTERESTING_ATTR(Nonisolated)
    UNINTERESTING_ATTR(ImplicitSelfCapture)
    UNINTERESTING_ATTR(InheritActorContext)
    UNINTERESTING_ATTR(Isolated)
    UNINTERESTING_ATTR(NoImplicitCopy)
    UNINTERESTING_ATTR(UnavailableFromAsync)

    UNINTERESTING_ATTR(NoMetadata)
    UNINTERESTING_ATTR(CompileTimeConst)

    UNINTERESTING_ATTR(BackDeployed)
    UNINTERESTING_ATTR(KnownToBeLocal)

    UNINTERESTING_ATTR(UnsafeInheritExecutor)
    UNINTERESTING_ATTR(CompilerInitialized)
    UNINTERESTING_ATTR(AlwaysEmitConformanceMetadata)

    UNINTERESTING_ATTR(EagerMove)
    UNINTERESTING_ATTR(NoEagerMove)

    UNINTERESTING_ATTR(RuntimeMetadata)

    UNINTERESTING_ATTR(MacroRole)
    UNINTERESTING_ATTR(LexicalLifetimes)
#undef UNINTERESTING_ATTR

    void visitAvailableAttr(AvailableAttr *attr) {
      // FIXME: Check that this declaration is at least as available as the
      // one it overrides.
    }

    void visitRethrowsAttr(RethrowsAttr *attr) {
      // 'rethrows' functions are a subtype of ordinary 'throws' functions.
      // Require 'rethrows' on the override if it was there on the base,
      // unless the override is completely non-throwing.
      if (!Override->getAttrs().hasAttribute<RethrowsAttr>() &&
          cast<AbstractFunctionDecl>(Override)->hasThrows()) {
        Diags.diagnose(Override, diag::override_rethrows_with_non_rethrows,
                       isa<ConstructorDecl>(Override));
        Diags.diagnose(Base, diag::overridden_here);
      }
    }

    void visitReasyncAttr(ReasyncAttr *attr) {
      // 'reasync' functions are a subtype of ordinary 'async' functions.
      // Require 'reasync' on the override if it was there on the base.
      if (!Override->getAttrs().hasAttribute<ReasyncAttr>()) {
        Diags.diagnose(Override, diag::override_reasync_with_non_reasync,
                       isa<ConstructorDecl>(Override));
        Diags.diagnose(Base, diag::overridden_here);
      }
    }

    void visitObjCAttr(ObjCAttr *attr) {
      // Checking for overrides of declarations that are implicitly @objc
      // and occur in class extensions, because overriding will no longer be
      // possible under the Swift 4 rules.

      // We only care about the storage declaration.
      if (isa<AccessorDecl>(Override)) return;

      // If @objc was explicit or handled elsewhere, nothing to do.
      if (!attr->isSwift3Inferred()) return;

      // If we aren't warning about Swift 3 @objc inference, we're done.
      if (Override->getASTContext().LangOpts.WarnSwift3ObjCInference ==
            Swift3ObjCInferenceWarnings::None)
        return;

      // If 'dynamic' was implicit, we'll already have warned about this.
      if (auto dynamicAttr = Base->getAttrs().getAttribute<DynamicAttr>()) {
        if (!dynamicAttr->isImplicit()) return;
      }

      // The overridden declaration needs to be in an extension.
      if (!isa<ExtensionDecl>(Base->getDeclContext())) return;

      // Complain.
      Diags.diagnose(Override, diag::override_swift3_objc_inference,
                     Override->getDescriptiveKind(),
                     Override->getName(),
                     Base->getDeclContext()
                       ->getSelfNominalTypeDecl()
                       ->getName());
      Diags.diagnose(Base, diag::make_decl_objc, Base->getDescriptiveKind())
        .fixItInsert(Base->getAttributeInsertionLoc(false),
                     "@objc ");
    }
  };
} // end anonymous namespace

/// Determine whether overriding the given declaration requires a keyword.
OverrideRequiresKeyword swift::overrideRequiresKeyword(ValueDecl *overridden) {
  if (isa<AccessorDecl>(overridden))
    return OverrideRequiresKeyword::Never;

  if (isa<ProtocolDecl>(overridden->getDeclContext())) {
    if (overridden->getASTContext().LangOpts.WarnImplicitOverrides)
      return OverrideRequiresKeyword::Implicit;

    return OverrideRequiresKeyword::Never;
  }

  if (auto ctor = dyn_cast<ConstructorDecl>(overridden)) {
    return !ctor->isDesignatedInit() || ctor->isRequired()
      ? OverrideRequiresKeyword::Never
      : OverrideRequiresKeyword::Always;
  }

  return OverrideRequiresKeyword::Always;
}

/// Returns true if the availability of the overriding declaration
/// makes it a safe override, given the availability of the base declaration.
static bool isAvailabilitySafeForOverride(ValueDecl *override,
                                          ValueDecl *base) {
  ASTContext &ctx = override->getASTContext();

  // API availability ranges are contravariant: make sure the version range
  // of an overridden declaration is fully contained in the range of the
  // overriding declaration.
  AvailabilityContext overrideInfo =
    AvailabilityInference::availableRange(override, ctx);
  AvailabilityContext baseInfo =
    AvailabilityInference::availableRange(base, ctx);

  if (baseInfo.isContainedIn(overrideInfo))
    return true;

  // Allow overrides that are not as available as the base decl as long as the
  // override is as available as its context.
  auto overrideTypeAvailability = AvailabilityInference::inferForType(
      override->getDeclContext()->getSelfTypeInContext());
  
  return overrideTypeAvailability.isContainedIn(overrideInfo);
}

/// Returns true if a diagnostic about an accessor being less available
/// than the accessor it overrides would be redundant because we will
/// already emit another diagnostic.
static bool
isRedundantAccessorOverrideAvailabilityDiagnostic(ValueDecl *override,
                                                  ValueDecl *base) {

  auto *overrideFn = dyn_cast<AccessorDecl>(override);
  auto *baseFn = dyn_cast<AccessorDecl>(base);
  if (!overrideFn || !baseFn)
    return false;

  AbstractStorageDecl *overrideASD = overrideFn->getStorage();
  AbstractStorageDecl *baseASD = baseFn->getStorage();
  if (overrideASD->getOverriddenDecl() != baseASD)
    return false;

  // If we have already emitted a diagnostic about an unsafe override
  // for the property, don't complain about the accessor.
  if (!isAvailabilitySafeForOverride(overrideASD, baseASD)) {
    return true;
  }

  // Returns true if we will already diagnose a bad override
  // on the property's accessor of the given kind.
  auto accessorOverrideAlreadyDiagnosed = [&](AccessorKind kind) {
    FuncDecl *overrideAccessor = overrideASD->getOpaqueAccessor(kind);
    FuncDecl *baseAccessor = baseASD->getOpaqueAccessor(kind);
    if (overrideAccessor && baseAccessor &&
        !isAvailabilitySafeForOverride(overrideAccessor, baseAccessor)) {
      return true;
    }
    return false;
  };

  // If we have already emitted a diagnostic about an unsafe override
  // for a getter or a setter, no need to complain about the read or
  // modify coroutines, which are synthesized to be as available as either
  // the getter and the setter.
  switch (overrideFn->getAccessorKind()) {
  case AccessorKind::Get:
  case AccessorKind::Set:
    break;

  case AccessorKind::Read:
    if (accessorOverrideAlreadyDiagnosed(AccessorKind::Get))
      return true;
    break;

  case AccessorKind::Modify:
    if (accessorOverrideAlreadyDiagnosed(AccessorKind::Get) ||
        accessorOverrideAlreadyDiagnosed(AccessorKind::Set)) {
      return true;
    }
    break;

#define OPAQUE_ACCESSOR(ID, KEYWORD)
#define ACCESSOR(ID) \
  case AccessorKind::ID:
#include "swift/AST/AccessorKinds.def"
    llvm_unreachable("checking override for non-opaque accessor");
  }

  return false;
}

/// Diagnose an override for potential availability. Returns true if
/// a diagnostic was emitted and false otherwise.
static bool diagnoseOverrideForAvailability(ValueDecl *override,
                                            ValueDecl *base) {
  if (isAvailabilitySafeForOverride(override, base))
    return false;

  // Suppress diagnostics about availability overrides for accessors
  // if they would be redundant with other diagnostics.
  if (isRedundantAccessorOverrideAvailabilityDiagnostic(override, base))
    return false;

  auto &diags = override->getASTContext().Diags;
  if (auto *accessor = dyn_cast<AccessorDecl>(override)) {
    diags.diagnose(override, diag::override_accessor_less_available,
                   accessor->getDescriptiveKind(),
                   accessor->getStorage()->getBaseName());
    diags.diagnose(base, diag::overridden_here);
    return true;
  }

  diags.diagnose(override, diag::override_less_available,
                 override->getBaseName());
  diags.diagnose(base, diag::overridden_here);

  return true;
}

enum class OverrideUnavailabilityStatus {
  /// The unavailability of the base decl and override decl are compatible.
  Compatible,
  /// The base decl is unavailable but the override decl is not.
  BaseUnavailable,
  /// Do not diagnose the unavailability of these decls.
  Ignored,
};

static std::pair<OverrideUnavailabilityStatus, const AvailableAttr *>
checkOverrideUnavailability(ValueDecl *override, ValueDecl *base) {
  if (auto *overrideParent = override->getDeclContext()->getAsDecl()) {
    // If the parent of the override is unavailable, then the unavailability of
    // the override decl is irrelevant.
    if (overrideParent->getSemanticUnavailableAttr())
      return {OverrideUnavailabilityStatus::Ignored, nullptr};
  }

  if (auto *baseAccessor = dyn_cast<AccessorDecl>(base)) {
    // Ignore implicit accessors since the diagnostics are likely to duplicate
    // the diagnostics for the explicit accessors that availability was inferred
    // from.
    if (baseAccessor->isImplicit())
      return {OverrideUnavailabilityStatus::Ignored, nullptr};

    if (auto *overrideAccessor = dyn_cast<AccessorDecl>(override)) {
      // If base and override are accessors, check whether the unavailability of
      // their storage matches. Diagnosing accessors with invalid storage
      // produces redundant diagnostics.
      if (checkOverrideUnavailability(overrideAccessor->getStorage(),
                                      baseAccessor->getStorage())
              .first != OverrideUnavailabilityStatus::Compatible)
        return {OverrideUnavailabilityStatus::Ignored, nullptr};
    }
  }

  auto &ctx = override->getASTContext();
  auto *baseUnavailableAttr = base->getAttrs().getUnavailable(ctx);
  auto *overrideUnavailableAttr = override->getAttrs().getUnavailable(ctx);

  if (baseUnavailableAttr && !overrideUnavailableAttr)
    return {OverrideUnavailabilityStatus::BaseUnavailable, baseUnavailableAttr};

  return {OverrideUnavailabilityStatus::Compatible, nullptr};
}

static bool checkSingleOverride(ValueDecl *override, ValueDecl *base) {
  // This can happen with circular inheritance.
  // FIXME: This shouldn't be possible once name lookup goes through the
  // request-evaluator.
  if (override == base)
    return true;

  ASTContext &ctx = override->getASTContext();
  auto &diags = ctx.Diags;

  // Check property and subscript overriding.
  if (auto *baseASD = dyn_cast<AbstractStorageDecl>(base)) {
    auto *overrideASD = cast<AbstractStorageDecl>(override);

    // Make sure that the overriding property doesn't have storage.
    if ((overrideASD->hasStorage() ||
         overrideASD->getAttrs().hasAttribute<LazyAttr>()) &&
        !overrideASD->hasObservers()) {
      bool downgradeToWarning = false;
      if (!ctx.isSwiftVersionAtLeast(5) &&
          overrideASD->getAttrs().hasAttribute<LazyAttr>()) {
        // Swift 4.0 had a bug where lazy properties were considered
        // computed by the time of this check. Downgrade this diagnostic to
        // a warning.
        downgradeToWarning = true;
      }
      auto diagID = downgradeToWarning ?
          diag::override_with_stored_property_warn :
          diag::override_with_stored_property;
      diags.diagnose(overrideASD, diagID,
                     overrideASD->getBaseIdentifier());
      diags.diagnose(baseASD, diag::property_override_here);
      if (!downgradeToWarning)
        return true;
    }

    // Make sure that an observing property isn't observing something
    // read-only.  Observing properties look at change, read-only properties
    // have nothing to observe!
    bool baseIsSettable = baseASD->isSettable(baseASD->getDeclContext());
    if (baseIsSettable) {
      baseIsSettable =
         baseASD->isSetterAccessibleFrom(overrideASD->getDeclContext());
    }
    if (overrideASD->getWriteImpl() == WriteImplKind::InheritedWithObservers
        && !baseIsSettable) {
      diags.diagnose(overrideASD, diag::observing_readonly_property,
                     overrideASD->getBaseIdentifier());
      diags.diagnose(baseASD, diag::property_override_here);
      return true;
    }

    // Make sure we're not overriding a settable property with a non-settable
    // one.  The only reasonable semantics for this would be to inherit the
    // setter but override the getter, and that would be surprising at best.
    if (baseIsSettable && !overrideASD->isSettable(override->getDeclContext())) {
      diags.diagnose(overrideASD, diag::override_mutable_with_readonly_property,
                     overrideASD->getBaseIdentifier());
      diags.diagnose(baseASD, diag::property_override_here);
      return true;
    }


    // Make sure a 'let' property is only overridden by 'let' properties.  A
    // let property provides more guarantees than the getter of a 'var'
    // property.
    if (auto VD = dyn_cast<VarDecl>(baseASD)) {
      if (VD->isLet()) {
        diags.diagnose(overrideASD, diag::override_let_property,
                    VD->getName());
        diags.diagnose(baseASD, diag::property_override_here);
        return true;
      }
    }

    // Make sure an effectful storage decl is only overridden by a storage
    // decl with the same or fewer effect kinds.
    if (!overrideASD->isLessEffectfulThan(baseASD, EffectKind::Async)) {
      diags.diagnose(overrideASD, diag::override_with_more_effects,
                     overrideASD->getDescriptiveKind(), "async");
      return true;
    } else if (!overrideASD->isLessEffectfulThan(baseASD, EffectKind::Throws)) {
      diags.diagnose(overrideASD, diag::override_with_more_effects,
                     overrideASD->getDescriptiveKind(), "throwing");
      return true;
    }
  }

  // Various properties are only checked for the storage declarations
  // and not for the individual accessors. Otherwise, we end up with
  // duplicated diagnostics.
  bool isAccessor = isa<AccessorDecl>(override);

  // Non-Objective-C declarations in extensions cannot override or
  // be overridden.
  if (!isAccessor &&
      (isa<ExtensionDecl>(base->getDeclContext()) ||
       isa<ExtensionDecl>(override->getDeclContext())) &&
      !base->isObjC()) {
    // Suppress this diagnostic for overrides of non-open NSObject.Hashable
    // interfaces; these are diagnosed elsewhere. An error message complaining
    // about extensions would be misleading in this case; the correct fix is to
    // override NSObject.hash instead.
    if ((isNSObjectHashValue(base) || isNSObjectHashMethod(base)) &&
        !base->hasOpenAccess(override->getDeclContext()))
      return true;

    bool baseCanBeObjC = canBeRepresentedInObjC(base);
    auto nominal = base->getDeclContext()->getSelfNominalTypeDecl();
    diags.diagnose(override, diag::override_decl_extension, baseCanBeObjC,
                   !isa<ExtensionDecl>(base->getDeclContext()),
                   override->getDescriptiveKind(), override->getName(),
                   nominal->getName());
    // If the base and the override come from the same module, try to fix
    // the base declaration. Otherwise we can wind up diagnosing into e.g. the
    // SDK overlay modules.
    if (baseCanBeObjC &&
        base->getModuleContext() == override->getModuleContext()) {
      SourceLoc insertionLoc =
        override->getAttributeInsertionLoc(/*forModifier=*/false);
      diags.diagnose(base, diag::overridden_here_can_be_objc)
        .fixItInsert(insertionLoc, "@objc ");
    } else {
      diags.diagnose(base, diag::overridden_here);
    }

    return true;
  }

  // If the overriding declaration does not have the 'override' modifier on
  // it, complain.
  if (!override->getAttrs().hasAttribute<OverrideAttr>() &&
      overrideRequiresKeyword(base) != OverrideRequiresKeyword::Never &&
      !override->isImplicit() &&
      override->getDeclContext()->getParentSourceFile()) {
    auto theDiag =
      overrideRequiresKeyword(base) == OverrideRequiresKeyword::Always
        ? diag::missing_override
        : diag::missing_override_warn;

    auto diagLoc = override->getStartLoc();
    // If dynamic cast to VarDecl succeeds, use the location of its parent
    // pattern binding which will return the VarLoc.
    if (auto VD = dyn_cast<VarDecl>(override)) {
      diagLoc = VD->getParentPatternBinding()->getLoc();
    }

    diags.diagnose(override, theDiag).fixItInsert(diagLoc, "override ");
    diags.diagnose(base, diag::overridden_here);
  }

  // If the overridden method is declared in a Swift Class Declaration,
  // dispatch will use table dispatch. If the override is in an extension
  // warn, since it is not added to the class vtable.
  //
  // FIXME: Only warn if the extension is in another module, and if
  // it is in the same module, update the vtable.
  if (auto *baseDecl = dyn_cast<ClassDecl>(base->getDeclContext())) {
    if (!isAccessor &&
        baseDecl->hasKnownSwiftImplementation() &&
        !base->shouldUseObjCDispatch() &&
        isa<ExtensionDecl>(override->getDeclContext())) {
      diags.diagnose(override, diag::override_class_declaration_in_extension);
      diags.diagnose(base, diag::overridden_here);
    }
  }
  // If the overriding declaration is 'throws' but the base is not,
  // complain. Do the same for 'async'
  if (auto overrideFn = dyn_cast<AbstractFunctionDecl>(override)) {
    if (overrideFn->hasThrows() &&
        !cast<AbstractFunctionDecl>(base)->hasThrows()) {
      diags.diagnose(override, diag::override_with_more_effects,
                     override->getDescriptiveKind(), "throwing");
      diags.diagnose(base, diag::overridden_here);
    }

    if (overrideFn->hasAsync() &&
        !cast<AbstractFunctionDecl>(base)->hasAsync()) {
      diags.diagnose(override, diag::override_with_more_effects,
                     override->getDescriptiveKind(), "async");
      diags.diagnose(base, diag::overridden_here);
    }

    if (!overrideFn->hasThrows() && base->isObjC() &&
        cast<AbstractFunctionDecl>(base)->hasThrows()) {
      diags.diagnose(override, diag::override_throws_objc,
                     isa<ConstructorDecl>(override));
      diags.diagnose(base, diag::overridden_here);
    }
  }

  // The overridden declaration cannot be 'final'.
  if (base->isSemanticallyFinal() && !isAccessor) {
    // Use a special diagnostic for overriding an actor's unownedExecutor
    // method.  TODO: only if it's implicit?  But then we need to
    // propagate implicitness in module interfaces.
    auto isActorUnownedExecutor = [&] {
      auto prop = dyn_cast<VarDecl>(base);
      return (prop &&
              prop->isFinal() &&
              isa<ClassDecl>(prop->getDeclContext()) &&
              cast<ClassDecl>(prop->getDeclContext())->isActor() &&
              !prop->isStatic() &&
              prop->getName() == ctx.Id_unownedExecutor &&
              prop->getInterfaceType()->getAnyNominal() == ctx.getUnownedSerialExecutorDecl());
    };

    if (isActorUnownedExecutor()) {
      override->diagnose(diag::override_implicit_unowned_executor);
    } else {
      // FIXME: Customize message to the kind of thing.
      auto baseKind = base->getDescriptiveKind();
      switch (baseKind) {
      case DescriptiveDeclKind::StaticProperty:
      case DescriptiveDeclKind::StaticMethod:
      case DescriptiveDeclKind::StaticSubscript:
        override->diagnose(diag::override_static, baseKind);
        break;
      default:
        override->diagnose(diag::override_final,
                           override->getDescriptiveKind(), baseKind);
        break;
      }
    }

    base->diagnose(diag::overridden_here);

    return true;
  }

  // FIXME: Possibly should extend to more availability checking.
  auto unavailabilityStatusAndAttr =
      checkOverrideUnavailability(override, base);
  auto *unavailableAttr = unavailabilityStatusAndAttr.second;

  switch (unavailabilityStatusAndAttr.first) {
  case OverrideUnavailabilityStatus::BaseUnavailable: {
    diagnoseOverrideOfUnavailableDecl(override, base, unavailableAttr);

    if (isUnavailableInAllVersions(base)) {
      auto modifier = override->getAttrs().getAttribute<OverrideAttr>();
      if (modifier && modifier->isValid()) {
        diags
            .diagnose(override, diag::suggest_removing_override,
                      override->getBaseName())
            .fixItRemove(modifier->getRange());
      }
    }
    break;
  }
  case OverrideUnavailabilityStatus::Compatible:
  case OverrideUnavailabilityStatus::Ignored:
    break;
  }

  if (!ctx.LangOpts.DisableAvailabilityChecking) {
    diagnoseOverrideForAvailability(override, base);
  }

  /// Check attributes associated with the base; some may need to merged with
  /// or checked against attributes in the overriding declaration.
  AttributeOverrideChecker attrChecker(base, override);
  for (auto attr : base->getAttrs()) {
    attrChecker.visit(attr);
  }

  return false;
}

/// Minimize the set of overridden associated types, eliminating any
/// associated types that are overridden by other associated types.
static void minimizeOverriddenAssociatedTypes(
                           llvm::TinyPtrVector<ValueDecl *> &assocTypes) {
  // Mark associated types that are "worse" than some other associated type,
  // because they come from an inherited protocol.
  bool anyWorse = false;
  std::vector<bool> worseThanAny(assocTypes.size(), false);
  for (unsigned i : indices(assocTypes)) {
    auto assoc1 = cast<AssociatedTypeDecl>(assocTypes[i]);
    auto proto1 = assoc1->getProtocol();
    for (unsigned j : range(i + 1, assocTypes.size())) {
      auto assoc2 = cast<AssociatedTypeDecl>(assocTypes[j]);
      auto proto2 = assoc2->getProtocol();
      if (proto1->inheritsFrom(proto2)) {
        anyWorse = true;
        worseThanAny[j] = true;
      } else if (proto2->inheritsFrom(proto1)) {
        anyWorse = true;
        worseThanAny[i] = true;
        break;
      }
    }
  }

  // If we didn't find any associated types that were "worse", we're done.
  if (!anyWorse) return;

  // Copy in the associated types that aren't worse than any other associated
  // type.
  unsigned nextIndex = 0;
  MutableArrayRef<ValueDecl *> buffer = assocTypes;
  for (unsigned i : indices(buffer)) {
    if (worseThanAny[i]) continue;
    buffer[nextIndex++] = buffer[i];
  }

  assocTypes.erase(assocTypes.begin() + nextIndex, assocTypes.end());
}

/// Sort associated types just based on the protocol.
static int compareSimilarAssociatedTypes(ValueDecl *const *lhs,
                                         ValueDecl *const *rhs) {
  auto lhsProto = cast<AssociatedTypeDecl>(*lhs)->getProtocol();
  auto rhsProto = cast<AssociatedTypeDecl>(*rhs)->getProtocol();
  return TypeDecl::compare(lhsProto, rhsProto);
}

/// Compute the set of associated types that are overridden by the given
/// associated type.
static llvm::TinyPtrVector<ValueDecl *>
computeOverriddenAssociatedTypes(AssociatedTypeDecl *assocType) {
  // Find associated types with the given name in all of the inherited
  // protocols.
  llvm::TinyPtrVector<ValueDecl *> overriddenAssocTypes;
  auto proto = assocType->getProtocol();
  proto->walkInheritedProtocols([&](ProtocolDecl *inheritedProto) {
    if (proto == inheritedProto) return TypeWalker::Action::Continue;

    // Objective-C protocols
    if (inheritedProto->isObjC()) return TypeWalker::Action::Continue;

    // Look for associated types with the same name.
    bool foundAny = false;
    if (auto found = inheritedProto->getAssociatedType(assocType->getName())) {
      overriddenAssocTypes.push_back(found);
      foundAny = true;
    }

    return foundAny ? TypeWalker::Action::SkipChildren
                    : TypeWalker::Action::Continue;
  });

  // Minimize the set of inherited associated types, eliminating any that
  // themselves are overridden.
  minimizeOverriddenAssociatedTypes(overriddenAssocTypes);

  // Sort the set of inherited associated types.
  llvm::array_pod_sort(overriddenAssocTypes.begin(),
                       overriddenAssocTypes.end(),
                       compareSimilarAssociatedTypes);

  return overriddenAssocTypes;
}

llvm::TinyPtrVector<ValueDecl *>
OverriddenDeclsRequest::evaluate(Evaluator &evaluator, ValueDecl *decl) const {
  // Value to return in error cases
  auto noResults = llvm::TinyPtrVector<ValueDecl *>();

  // If there is a @_nonoverride attribute, this does not override anything.
  if (decl->getAttrs().hasAttribute<NonOverrideAttr>())
    return noResults;

  // For an associated type, compute the (minimized) set of overridden
  // declarations.
  if (auto assocType = dyn_cast<AssociatedTypeDecl>(decl)) {
    return computeOverriddenAssociatedTypes(assocType);
  }

  // Only members of classes or protocols can override other declarations.
  if (!decl->getDeclContext()->getSelfClassDecl() &&
      !isa<ProtocolDecl>(decl->getDeclContext()))
    return noResults;

  // Types that aren't associated types cannot be overridden.
  if (isa<TypeDecl>(decl))
    return noResults;

  // Accessors determine their overrides based on their abstract storage
  // declarations.
  if (auto accessor = dyn_cast<AccessorDecl>(decl)) {
    auto kind = accessor->getAccessorKind();

    switch (kind) {
    case AccessorKind::Get:
    case AccessorKind::Set:
    case AccessorKind::Read:
    case AccessorKind::Modify:
      break;

    case AccessorKind::WillSet:
    case AccessorKind::DidSet:
    case AccessorKind::Address:
    case AccessorKind::MutableAddress:
    case AccessorKind::Init:
      // These accessors are never part of the opaque set. Bail out early
      // to avoid computing the overridden declarations of the storage.
      return noResults;
    }

    auto overridingASD = accessor->getStorage();

    // Check the various overridden storage declarations.
    SmallVector<OverrideMatch, 2> matches;
    for (auto overridden : overridingASD->getOverriddenDecls()) {
      auto baseASD = cast<AbstractStorageDecl>(overridden);

      // If the base doesn't consider this an opaque accessor,
      // this isn't really an override.
      if (!baseASD->requiresOpaqueAccessor(kind))
        continue;

      // Find the base accessor; if there isn't one, we're done.
      auto baseAccessor = baseASD->getOpaqueAccessor(kind);
      if (!baseAccessor)
        continue;

      assert(!baseAccessor->hasForcedStaticDispatch() &&
             "opaque accessor with forced static dispatch?");

      switch (kind) {
      case AccessorKind::Get:
      case AccessorKind::Read:
        break;

      case AccessorKind::Modify:
      case AccessorKind::Set:
        // For setter accessors, we need the base's setter to be
        // accessible from the overriding context, or it's not an override.
        if (!baseASD->isSetterAccessibleFrom(overridingASD->getDeclContext()))
          continue;
        break;

#define OPAQUE_ACCESSOR(ID, KEYWORD)
#define ACCESSOR(ID) \
      case AccessorKind::ID:
#include "swift/AST/AccessorKinds.def"
        llvm_unreachable("non-opaque accessor was required as opaque by base");
      }

      // We are overriding the base accessor.
      matches.push_back({baseAccessor, /*IsExact=*/true});
    }

    if (matches.empty())
      return noResults;

    // Check the correctness of the overrides.
    OverrideMatcher matcher(accessor);
    return matcher.checkPotentialOverrides(
                                       matches,
                                       OverrideCheckingAttempt::PerfectMatch);
  }

  // Only initializers, declarations marked with the 'override' declaration
  // modifier, and members of protocols can override declarations.
  if (!isa<ConstructorDecl>(decl) &&
      !isa<ProtocolDecl>(decl->getDeclContext()) &&
      !decl->getAttrs().hasAttribute<OverrideAttr>())
    return noResults;

  // Try to match potential overridden declarations.
  OverrideMatcher matcher(decl);
  if (!matcher) {
    return noResults;
  }

  auto matches = matcher.match(OverrideCheckingAttempt::PerfectMatch);
  if (matches.empty()) {
    return noResults;
  }

  // If we have more than one potential match from a class, diagnose the
  // ambiguity and fail.
  if (matches.size() > 1 && decl->getDeclContext()->getSelfClassDecl()) {
    diagnoseGeneralOverrideFailure(decl, matches,
                                   OverrideCheckingAttempt::PerfectMatch);
    invalidateOverrideAttribute(decl);
    return noResults;
  }

  // Check the matches. If any are ill-formed, invalidate the override attribute
  // so we don't try again.
  return matcher.checkPotentialOverrides(matches,
                                         OverrideCheckingAttempt::PerfectMatch);
}

bool IsABICompatibleOverrideRequest::evaluate(Evaluator &evaluator,
                                              ValueDecl *decl) const {
  auto base = decl->getOverriddenDecl();
  if (!base)
    return false;

  auto baseInterfaceTy = base->getInterfaceType();
  auto derivedInterfaceTy = decl->getInterfaceType();

  auto selfInterfaceTy = decl->getDeclContext()->getDeclaredInterfaceType();

  auto overrideInterfaceTy = selfInterfaceTy->adjustSuperclassMemberDeclType(
      base, decl, baseInterfaceTy);

  return derivedInterfaceTy->matches(overrideInterfaceTy,
                                     TypeMatchFlags::AllowABICompatible);
}

void swift::checkImplementationOnlyOverride(const ValueDecl *VD) {
  if (VD->isImplicit())
    return;

  if (VD->getAttrs().hasAttribute<ImplementationOnlyAttr>())
    return;

  if (isa<AccessorDecl>(VD))
    return;

  // Is this part of the module's API or ABI?
  AccessScope accessScope =
      VD->getFormalAccessScope(nullptr,
                               /*treatUsableFromInlineAsPublic*/true);
  if (!accessScope.isPublic())
    return;

  const ValueDecl *overridden = VD->getOverriddenDecl();
  if (!overridden)
    return;

  auto *SF = VD->getDeclContext()->getParentSourceFile();
  assert(SF && "checking a non-source declaration?");

  ModuleDecl *M = overridden->getModuleContext();
  if (SF->getRestrictedImportKind(M) == RestrictedImportKind::ImplementationOnly) {
    VD->diagnose(diag::implementation_only_override_import_without_attr,
                 overridden->getDescriptiveKind())
        .fixItInsert(VD->getAttributeInsertionLoc(false),
                     "@_implementationOnly ");
    overridden->diagnose(diag::overridden_here);
    return;
  }

  if (overridden->getAttrs().hasAttribute<ImplementationOnlyAttr>()) {
    VD->diagnose(diag::implementation_only_override_without_attr,
                 overridden->getDescriptiveKind())
        .fixItInsert(VD->getAttributeInsertionLoc(false),
                     "@_implementationOnly ");
    overridden->diagnose(diag::overridden_here);
    return;
  }

  // FIXME: Check storage decls where the setter is in a separate module from
  // the getter, which is a thing Objective-C can do. The ClangImporter
  // doesn't make this easy, though, because it just gives the setter the same
  // DeclContext as the property or subscript, which means we've lost the
  // information about whether its module was implementation-only imported.
}
