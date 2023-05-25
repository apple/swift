//===--- TypeCheckDeclObjC.cpp - Type Checking for ObjC Declarations ------===//
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
// This file implements semantic analysis for Objective-C-specific
// aspects of declarations.
//
//===----------------------------------------------------------------------===//
#include "TypeCheckObjC.h"
#include "TypeChecker.h"
#include "TypeCheckConcurrency.h"
#include "TypeCheckProtocol.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/StringExtras.h"

#include "clang/AST/DeclObjC.h"

using namespace swift;

#pragma mark Determine whether an entity is representable in Objective-C.

DiagnosticBehavior
swift::behaviorLimitForObjCReason(ObjCReason reason, ASTContext &ctx) {
  switch(reason) {
  case ObjCReason::ExplicitlyCDecl:
  case ObjCReason::ExplicitlyDynamic:
  case ObjCReason::ExplicitlyObjC:
  case ObjCReason::ExplicitlyObjCMembers:
  case ObjCReason::ExplicitlyIBOutlet:
  case ObjCReason::ExplicitlyIBAction:
  case ObjCReason::ExplicitlyIBSegueAction:
  case ObjCReason::ExplicitlyNSManaged:
  case ObjCReason::MemberOfObjCProtocol:
  case ObjCReason::OverridesObjC:
  case ObjCReason::WitnessToObjC:
  case ObjCReason::ImplicitlyObjC:
  case ObjCReason::MemberOfObjCExtension:
    return DiagnosticBehavior::Unspecified;

  case ObjCReason::ExplicitlyIBInspectable:
  case ObjCReason::ExplicitlyGKInspectable:
    if (!ctx.LangOpts.EnableSwift3ObjCInference)
      return DiagnosticBehavior::Unspecified;
    return DiagnosticBehavior::Ignore;

  case ObjCReason::ExplicitlyObjCByAccessNote:
    return ctx.LangOpts.getAccessNoteFailureLimit();

  case ObjCReason::MemberOfObjCSubclass:
  case ObjCReason::MemberOfObjCMembersClass:
  case ObjCReason::ElementOfObjCEnum:
  case ObjCReason::Accessor:
    return DiagnosticBehavior::Ignore;
  }
  llvm_unreachable("unhandled reason");
}

unsigned swift::getObjCDiagnosticAttrKind(ObjCReason reason) {
  switch (reason) {
  case ObjCReason::ExplicitlyCDecl:
  case ObjCReason::ExplicitlyDynamic:
  case ObjCReason::ExplicitlyObjC:
  case ObjCReason::ExplicitlyObjCMembers:
  case ObjCReason::ExplicitlyIBOutlet:
  case ObjCReason::ExplicitlyIBAction:
  case ObjCReason::ExplicitlyIBSegueAction:
  case ObjCReason::ExplicitlyNSManaged:
  case ObjCReason::MemberOfObjCProtocol:
  case ObjCReason::OverridesObjC:
  case ObjCReason::WitnessToObjC:
  case ObjCReason::ImplicitlyObjC:
  case ObjCReason::ExplicitlyIBInspectable:
  case ObjCReason::ExplicitlyGKInspectable:
  case ObjCReason::MemberOfObjCExtension:
  case ObjCReason::ExplicitlyObjCByAccessNote:
    return static_cast<unsigned>(reason);

  case ObjCReason::MemberOfObjCSubclass:
  case ObjCReason::MemberOfObjCMembersClass:
  case ObjCReason::ElementOfObjCEnum:
  case ObjCReason::Accessor:
    // Diagnostics involving these ObjCReasons should always be ignored, so we
    // deliberately return a value which is out of bounds for the `%select` in
    // `OBJC_ATTR_SELECT`.
    return ~static_cast<unsigned>(0);
  }
  llvm_unreachable("unhandled reason");
}

void ObjCReason::describe(const Decl *D) const {
  switch (kind) {
  case ObjCReason::MemberOfObjCProtocol:
    D->diagnose(diag::objc_inferring_on_objc_protocol_member);
    break;

  case ObjCReason::OverridesObjC: {
    unsigned kind = isa<VarDecl>(D) ? 0
                  : isa<SubscriptDecl>(D) ? 1
                  : isa<ConstructorDecl>(D) ? 2
                  : 3;

    auto overridden = cast<ValueDecl>(D)->getOverriddenDecl();
    overridden->diagnose(diag::objc_overriding_objc_decl,
                         kind, overridden->getName());
    break;
  }

  case ObjCReason::WitnessToObjC: {
    auto requirement = getObjCRequirement();
    requirement->diagnose(diag::objc_witness_objc_requirement,
                D->getDescriptiveKind(), requirement->getName(),
                cast<ProtocolDecl>(requirement->getDeclContext())
                  ->getName());
    break;
  }

  case ObjCReason::ExplicitlyObjCByAccessNote:
  case ObjCReason::ExplicitlyCDecl:
  case ObjCReason::ExplicitlyDynamic:
  case ObjCReason::ExplicitlyObjC:
  case ObjCReason::ExplicitlyObjCMembers:
  case ObjCReason::ExplicitlyIBOutlet:
  case ObjCReason::ExplicitlyIBAction:
  case ObjCReason::ExplicitlyIBSegueAction:
  case ObjCReason::ExplicitlyNSManaged:
  case ObjCReason::ImplicitlyObjC:
  case ObjCReason::ExplicitlyIBInspectable:
  case ObjCReason::ExplicitlyGKInspectable:
  case ObjCReason::MemberOfObjCExtension:
  case ObjCReason::MemberOfObjCMembersClass:
  case ObjCReason::MemberOfObjCSubclass:
  case ObjCReason::ElementOfObjCEnum:
  case ObjCReason::Accessor:
    // No additional note required.
    break;
  }
}

void ObjCReason::setAttrInvalid() const {
  if (requiresAttr(kind))
    getAttr()->setInvalid();
}

static void diagnoseTypeNotRepresentableInObjC(const DeclContext *DC,
                                               Type T,
                                               SourceRange TypeRange,
                                               DiagnosticBehavior behavior) {
  auto &diags = DC->getASTContext().Diags;

  // Special diagnostic for tuples.
  if (T->is<TupleType>()) {
    if (T->isVoid())
      diags.diagnose(TypeRange.Start, diag::not_objc_empty_tuple)
          .highlight(TypeRange)
          .limitBehavior(behavior);
    else
      diags.diagnose(TypeRange.Start, diag::not_objc_tuple)
          .highlight(TypeRange)
          .limitBehavior(behavior);
    return;
  }

  // Special diagnostic for classes.
  if (auto *CD = T->getClassOrBoundGenericClass()) {
    if (!CD->isObjC())
      diags.diagnose(TypeRange.Start, diag::not_objc_swift_class)
          .highlight(TypeRange)
          .limitBehavior(behavior);
    return;
  }

  // Special diagnostic for structs.
  if (auto *SD = T->getStructOrBoundGenericStruct()) {
    diags.diagnose(TypeRange.Start, diag::not_objc_swift_struct)
        .highlight(TypeRange)
        .limitBehavior(behavior);
    return;
  }

  // Special diagnostic for enums.
  if (T->is<EnumType>()) {
    diags.diagnose(TypeRange.Start, diag::not_objc_swift_enum)
        .highlight(TypeRange)
        .limitBehavior(behavior);
    return;
  }

  // Special diagnostic for protocols and protocol compositions.
  if (T->isExistentialType()) {
    if (T->isAny()) {
      // Any is not @objc.
      diags.diagnose(TypeRange.Start,
                     diag::not_objc_empty_protocol_composition)
          .limitBehavior(behavior);
      return;
    }

    auto layout = T->getExistentialLayout();

    // See if the superclass is not @objc.
    if (auto superclass = layout.explicitSuperclass) {
      if (!superclass->getClassOrBoundGenericClass()->isObjC()) {
        diags.diagnose(TypeRange.Start, diag::not_objc_class_constraint,
                       superclass)
            .limitBehavior(behavior);
        return;
      }
    }

    // Find a protocol that is not @objc.
    bool sawErrorProtocol = false;
    for (auto PD : layout.getProtocols()) {
      if (PD->isSpecificProtocol(KnownProtocolKind::Error)) {
        sawErrorProtocol = true;
        break;
      }

      if (!PD->isObjC()) {
        diags.diagnose(TypeRange.Start, diag::not_objc_protocol,
                       PD->getDeclaredInterfaceType())
            .limitBehavior(behavior);
        return;
      }
    }

    if (sawErrorProtocol) {
      diags.diagnose(TypeRange.Start,
                     diag::not_objc_error_protocol_composition)
          .limitBehavior(behavior);
      return;
    }

    return;
  }

  if (T->is<ArchetypeType>() || T->isTypeParameter()) {
    diags.diagnose(TypeRange.Start, diag::not_objc_generic_type_param)
        .highlight(TypeRange)
        .limitBehavior(behavior);
    return;
  }

  if (auto fnTy = T->getAs<FunctionType>()) {
    if (fnTy->getExtInfo().isAsync()) {
      diags.diagnose(TypeRange.Start, diag::not_objc_function_type_async)
        .highlight(TypeRange)
        .limitBehavior(behavior);
      return;
    }

    if (fnTy->getExtInfo().isThrowing()) {
      diags.diagnose(TypeRange.Start, diag::not_objc_function_type_throwing)
        .highlight(TypeRange)
        .limitBehavior(behavior);
      return;
    }

    diags.diagnose(TypeRange.Start, diag::not_objc_function_type_param)
      .highlight(TypeRange)
      .limitBehavior(behavior);
    return;
  }
}

static void diagnoseFunctionParamNotRepresentable(
    const AbstractFunctionDecl *AFD, unsigned NumParams,
    unsigned ParamIndex, const ParamDecl *P, ObjCReason Reason) {
  auto behavior = behaviorLimitForObjCReason(Reason, AFD->getASTContext());

  if (NumParams == 1) {
    softenIfAccessNote(AFD, Reason.getAttr(),
      AFD->diagnose(diag::objc_invalid_on_func_single_param_type,
                    getObjCDiagnosticAttrKind(Reason))
          .limitBehavior(behavior));
  } else {
    softenIfAccessNote(AFD, Reason.getAttr(),
      AFD->diagnose(diag::objc_invalid_on_func_param_type,
                    ParamIndex + 1, getObjCDiagnosticAttrKind(Reason))
          .limitBehavior(behavior));
  }
  SourceRange SR;

  if (P->hasAttachedPropertyWrapper()) {
    auto wrapperTy = P->getPropertyWrapperBackingPropertyType();
    SR = P->getOutermostAttachedPropertyWrapper()->getRange();
    diagnoseTypeNotRepresentableInObjC(AFD, wrapperTy, SR, behavior);
  } else {
    if (auto typeRepr = P->getTypeRepr())
      SR = typeRepr->getSourceRange();
    diagnoseTypeNotRepresentableInObjC(AFD, P->getType(), SR, behavior);
  }
  Reason.describe(AFD);
}

static bool isParamListRepresentableInObjC(const AbstractFunctionDecl *AFD,
                                           const ParameterList *PL,
                                           ObjCReason Reason) {
  // If you change this function, you must add or modify a test in PrintAsClang.
  ASTContext &ctx = AFD->getASTContext();
  auto &diags = ctx.Diags;
  auto behavior = behaviorLimitForObjCReason(Reason, ctx);
  bool IsObjC = true;
  unsigned NumParams = PL->size();
  for (unsigned ParamIndex = 0; ParamIndex != NumParams; ++ParamIndex) {
    auto param = PL->get(ParamIndex);

    // Swift Varargs are not representable in Objective-C.
    if (param->isVariadic()) {
      softenIfAccessNote(AFD, Reason.getAttr(),
        diags.diagnose(param->getStartLoc(), diag::objc_invalid_on_func_variadic,
                       getObjCDiagnosticAttrKind(Reason))
          .highlight(param->getSourceRange())
          .limitBehavior(behavior));
      Reason.describe(AFD);

      return false;
    }

    // Swift inout parameters are not representable in Objective-C.
    if (param->isInOut()) {
      softenIfAccessNote(AFD, Reason.getAttr(),
        diags.diagnose(param->getStartLoc(), diag::objc_invalid_on_func_inout,
                       getObjCDiagnosticAttrKind(Reason))
          .highlight(param->getSourceRange())
          .limitBehavior(behavior));
      Reason.describe(AFD);

      return false;
    }

    if (param->getType()->hasError())
      return false;

    if (param->hasAttachedPropertyWrapper()) {
      if (param->getPropertyWrapperBackingPropertyType()->isRepresentableIn(
          ForeignLanguage::ObjectiveC,
          const_cast<AbstractFunctionDecl *>(AFD)))
        continue;
    } else if (param->getType()->isRepresentableIn(
          ForeignLanguage::ObjectiveC,
          const_cast<AbstractFunctionDecl *>(AFD))) {
      continue;
    }

    // Permit '()' when this method overrides a method with a
    // foreign error convention that replaces NSErrorPointer with ()
    // and this is the replaced parameter.
    AbstractFunctionDecl *overridden;
    if (param->getType()->isVoid() && AFD->hasThrows() &&
        (overridden = AFD->getOverriddenDecl())) {
      auto foreignError = overridden->getForeignErrorConvention();
      if (foreignError &&
          foreignError->isErrorParameterReplacedWithVoid() &&
          foreignError->getErrorParameterIndex() == ParamIndex) {
        continue;
      }
    }

    IsObjC = false;
    diagnoseFunctionParamNotRepresentable(AFD, NumParams, ParamIndex,
                                          param, Reason);
  }
  return IsObjC;
}

/// Check whether the given declaration contains its own generic parameters,
/// and therefore is not representable in Objective-C.
static bool checkObjCWithGenericParams(const ValueDecl *VD, ObjCReason Reason) {
  auto behavior = behaviorLimitForObjCReason(Reason, VD->getASTContext());

  auto *GC = VD->getAsGenericContext();
  assert(GC);
  if (GC->getGenericParams()) {
    softenIfAccessNote(VD, Reason.getAttr(),
      VD->diagnose(diag::objc_invalid_with_generic_params,
                   VD->getDescriptiveKind(), getObjCDiagnosticAttrKind(Reason))
          .limitBehavior(behavior));
    Reason.describe(VD);

    return true;
  }

  if (GC->getTrailingWhereClause()) {
    softenIfAccessNote(VD, Reason.getAttr(),
      VD->diagnose(diag::objc_invalid_with_generic_requirements,
                   VD->getDescriptiveKind(), getObjCDiagnosticAttrKind(Reason))
          .limitBehavior(behavior));
    Reason.describe(VD);

    return true;
  }

  return false;
}

/// CF types cannot have @objc methods, because they don't have real class
/// objects.
static bool checkObjCInForeignClassContext(const ValueDecl *VD,
                                           ObjCReason Reason) {
  auto behavior = behaviorLimitForObjCReason(Reason, VD->getASTContext());

  auto type = VD->getDeclContext()->getDeclaredInterfaceType();
  if (!type)
    return false;

  auto clazz = type->getClassOrBoundGenericClass();
  if (!clazz)
    return false;

  switch (clazz->getForeignClassKind()) {
  case ClassDecl::ForeignKind::Normal:
    return false;

  case ClassDecl::ForeignKind::CFType:
    VD->diagnose(diag::objc_invalid_on_foreign_class,
                 getObjCDiagnosticAttrKind(Reason))
        .limitBehavior(behavior);
    Reason.describe(VD);
    break;

  case ClassDecl::ForeignKind::RuntimeOnly:
    VD->diagnose(diag::objc_in_objc_runtime_visible,
                 VD->getDescriptiveKind(), getObjCDiagnosticAttrKind(Reason),
                 clazz->getName())
        .limitBehavior(behavior);
    Reason.describe(VD);
    break;
  }

  return true;
}

/// Whether the given declaration can be exposed as Objective-C.
static bool canExposeActorIsolatedAsObjC(
    const ValueDecl *value, const ActorIsolation &isolation) {
  if (isAccessibleAcrossActors(
          const_cast<ValueDecl *>(value), isolation, value->getDeclContext()))
    return true;

  // An async function can be exposed as Objective-C.
  if (auto func = dyn_cast<AbstractFunctionDecl>( value))
    return func->hasAsync();

  return false;
}

/// Actor-isolated declarations cannot be @objc.
static bool checkObjCActorIsolation(const ValueDecl *VD, ObjCReason Reason) {
  // Check actor isolation.
  switch (auto isolation = getActorIsolation(const_cast<ValueDecl *>(VD))) {
  case ActorIsolation::ActorInstance:
    if (!canExposeActorIsolatedAsObjC(VD, isolation)) {
      // Actor-isolated functions cannot be @objc.
      VD->diagnose(diag::actor_isolated_objc, VD->getDescriptiveKind(),
                   VD->getName());
      Reason.describe(VD);
      if (auto FD = dyn_cast<FuncDecl>(VD)) {
        addAsyncNotes(const_cast<FuncDecl *>(FD));
      }

      return true;
    }

    // FIXME: Substitution map?
    diagnoseNonSendableTypesInReference(
        const_cast<ValueDecl *>(VD), VD->getDeclContext(),
        VD->getLoc(), SendableCheckReason::ObjC);
    return false;

  case ActorIsolation::GlobalActor:
  case ActorIsolation::GlobalActorUnsafe:
    // FIXME: Consider whether to limit @objc on global-actor-qualified
    // declarations. Perhaps only allow main actor, which we can reflect
    // in the generated header.
    return false;

  case ActorIsolation::Independent:
  case ActorIsolation::Unspecified:
    return false;
  }
}

static VersionRange getMinOSVersionForClassStubs(const llvm::Triple &target) {
  if (target.isMacOSX())
    return VersionRange::allGTE(llvm::VersionTuple(10, 15, 0));
  if (target.isiOS()) // also returns true on tvOS
    return VersionRange::allGTE(llvm::VersionTuple(13, 0, 0));
  if (target.isWatchOS())
    return VersionRange::allGTE(llvm::VersionTuple(6, 0, 0));
  return VersionRange::all();
}

static bool checkObjCClassStubAvailability(ASTContext &ctx, const Decl *decl) {
  auto minRange = getMinOSVersionForClassStubs(ctx.LangOpts.Target);

  auto targetRange = AvailabilityContext::forDeploymentTarget(ctx);
  if (targetRange.getOSVersion().isContainedIn(minRange))
    return true;

  auto declRange = AvailabilityInference::availableRange(decl, ctx);
  return declRange.getOSVersion().isContainedIn(minRange);
}

static const ClassDecl *getResilientAncestor(ModuleDecl *mod,
                                             const ClassDecl *classDecl) {
  auto *superclassDecl = classDecl;

  for (;;) {
    if (superclassDecl->hasResilientMetadata(mod,
                                             ResilienceExpansion::Maximal))
      return superclassDecl;

    superclassDecl = superclassDecl->getSuperclassDecl();
  }
}

/// Check whether the given declaration occurs within a constrained
/// extension, or an extension of a generic class, or an
/// extension of an Objective-C runtime visible class, and
/// therefore is not representable in Objective-C.
static bool checkObjCInExtensionContext(const ValueDecl *value,
                                        ObjCReason reason) {
  auto behavior = behaviorLimitForObjCReason(reason, value->getASTContext());
  auto DC = value->getDeclContext();

  if (auto ED = dyn_cast<ExtensionDecl>(DC)) {
    if (ED->getTrailingWhereClause()) {
      softenIfAccessNote(value, reason.getAttr(),
        value->diagnose(diag::objc_in_extension_context)
            .limitBehavior(behavior));
      reason.describe(value);
      return true;
    }

    if (auto classDecl = ED->getSelfClassDecl()) {
      auto *mod = value->getModuleContext();
      auto &ctx = mod->getASTContext();

      if (!checkObjCClassStubAvailability(ctx, value)) {
        if (classDecl->checkAncestry().contains(
              AncestryFlags::ResilientOther) ||
            classDecl->hasResilientMetadata(mod,
                                            ResilienceExpansion::Maximal)) {
          auto &target = ctx.LangOpts.Target;
          auto platform = prettyPlatformString(targetPlatform(ctx.LangOpts));
          auto range = getMinOSVersionForClassStubs(target);
          auto *ancestor = getResilientAncestor(mod, classDecl);
          softenIfAccessNote(value, reason.getAttr(),
            value->diagnose(diag::objc_in_resilient_extension,
                            value->getDescriptiveKind(),
                            ancestor->getName(),
                            platform,
                            range.getLowerEndpoint())
                .limitBehavior(behavior));
          reason.describe(value);
          return true;
        }
      }

      if (classDecl->isGenericContext()) {
        // We do allow one special case. A @_dynamicReplacement(for:) function.
        // Currently, this is only supported if the replaced decl is from a
        // module compiled with -enable-implicit-dynamic.
        if (value->getDynamicallyReplacedDecl() &&
            value->getDynamicallyReplacedDecl()
                ->getModuleContext()
                ->isImplicitDynamicEnabled())
          return false;
        if (!classDecl->isTypeErasedGenericClass()) {
          softenIfAccessNote(value, reason.getAttr(),
            value->diagnose(diag::objc_in_generic_extension,
                            classDecl->isGeneric())
                .limitBehavior(behavior));
          reason.describe(value);
          return true;
        }
      }
    }
  }

  return false;
}

/// Determines whether the given type is a valid Objective-C class type that
/// can be returned as a result of a throwing function.
static bool isValidObjectiveCErrorResultType(DeclContext *dc, Type type) {
  switch (type->getForeignRepresentableIn(ForeignLanguage::ObjectiveC, dc)
            .first) {
  case ForeignRepresentableKind::Trivial:
  case ForeignRepresentableKind::None:
    // Special case: If the type is Unmanaged<T>, then return true, because
    // Unmanaged<T> can be represented in Objective-C (if T can be).
    if (auto BGT = type->getAs<BoundGenericType>()) {
      if (BGT->isUnmanaged()) {
        return true;
      }
    }
    return false;

  case ForeignRepresentableKind::Object:
  case ForeignRepresentableKind::Bridged:
  case ForeignRepresentableKind::BridgedError:
  case ForeignRepresentableKind::StaticBridged:
    return true;
  }

  llvm_unreachable("Unhandled ForeignRepresentableKind in switch.");
}

bool swift::isRepresentableInObjC(
       const AbstractFunctionDecl *AFD,
       ObjCReason Reason,
       Optional<ForeignAsyncConvention> &asyncConvention,
       Optional<ForeignErrorConvention> &errorConvention) {
  // Clear out the async and error conventions. They will be added later if
  // needed.
  asyncConvention = None;
  errorConvention = None;

  // If you change this function, you must add or modify a test in PrintAsClang.
  ASTContext &ctx = AFD->getASTContext();
  DiagnosticStateRAII diagState(ctx.Diags);


  if (checkObjCInForeignClassContext(AFD, Reason))
    return false;
  if (checkObjCWithGenericParams(AFD, Reason))
    return false;
  if (checkObjCInExtensionContext(AFD, Reason))
    return false;
  if (checkObjCActorIsolation(AFD, Reason))
    return false;

  auto behavior = behaviorLimitForObjCReason(Reason, ctx);
  if (AFD->isOperator()) {
    AFD->diagnose((isa<ProtocolDecl>(AFD->getDeclContext())
                    ? diag::objc_operator_proto
                    : diag::objc_operator))
        .limitBehavior(behavior);
    return false;
  }

  if (auto accessor = dyn_cast<AccessorDecl>(AFD)) {
    // Accessors can only be @objc if the storage declaration is.
    // Global computed properties may however @_cdecl their accessors.
    auto storage = accessor->getStorage();
    bool storageIsObjC = storage->isObjC()
        || Reason == ObjCReason::ExplicitlyCDecl
        || Reason == ObjCReason::WitnessToObjC
        || Reason == ObjCReason::MemberOfObjCProtocol;

    switch (accessor->getAccessorKind()) {
    case AccessorKind::DidSet:
    case AccessorKind::WillSet: {
      // willSet/didSet implementations are never exposed to objc, they are
      // always directly dispatched from the synthesized setter.
      diagnoseAndRemoveAttr(accessor, Reason.getAttr(),
                            diag::objc_observing_accessor)
          .limitBehavior(behavior);
      Reason.describe(accessor);
      return false;
    }

    case AccessorKind::Get:
      if (!storageIsObjC) {
        auto error = isa<VarDecl>(storage)
                       ? diag::objc_getter_for_nonobjc_property
                       : diag::objc_getter_for_nonobjc_subscript;

        diagnoseAndRemoveAttr(accessor, Reason.getAttr(), error)
            .limitBehavior(behavior);
        Reason.describe(accessor);
        return false;
      }
      return true;

    case AccessorKind::Set:
      if (!storageIsObjC) {
        auto error = isa<VarDecl>(storage)
                       ? diag::objc_setter_for_nonobjc_property
                       : diag::objc_setter_for_nonobjc_subscript;

        diagnoseAndRemoveAttr(accessor, Reason.getAttr(), error)
            .limitBehavior(behavior);
        Reason.describe(accessor);
        return false;
      }
      return true;

    case AccessorKind::Address:
    case AccessorKind::MutableAddress:
      diagnoseAndRemoveAttr(accessor, Reason.getAttr(), diag::objc_addressor)
          .limitBehavior(behavior);
      Reason.describe(accessor);
      return false;

    case AccessorKind::Read:
    case AccessorKind::Modify:
      diagnoseAndRemoveAttr(accessor, Reason.getAttr(),
                            diag::objc_coroutine_accessor)
          .limitBehavior(behavior);
      Reason.describe(accessor);
      return false;
    }
    llvm_unreachable("bad kind");
  }

  // As a special case, an initializer with a single, named parameter of type
  // '()' is always representable in Objective-C. This allows us to cope with
  // zero-parameter methods with selectors that are longer than "init". For
  // example, this allows:
  //
  // \code
  // class Foo {
  //   @objc init(malice: ()) { } // selector is "initWithMalice"
  // }
  // \endcode
  bool isSpecialInit = false;
  if (auto init = dyn_cast<ConstructorDecl>(AFD))
    isSpecialInit = init->isObjCZeroParameterWithLongSelector();

  if (!isSpecialInit &&
      !isParamListRepresentableInObjC(AFD,
                                      AFD->getParameters(),
                                      Reason)) {
    return false;
  }

  if (auto FD = dyn_cast<FuncDecl>(AFD)) {
    Type ResultType = FD->mapTypeIntoContext(FD->getResultInterfaceType());
    if (!FD->hasAsync() &&
        !ResultType->hasError() &&
        !ResultType->isVoid() &&
        !ResultType->isUninhabited() &&
        !ResultType->isRepresentableIn(ForeignLanguage::ObjectiveC,
                                       const_cast<FuncDecl *>(FD))) {
      softenIfAccessNote(AFD, Reason.getAttr(),
       AFD->diagnose(diag::objc_invalid_on_func_result_type,
                      getObjCDiagnosticAttrKind(Reason))
            .limitBehavior(behavior));
      diagnoseTypeNotRepresentableInObjC(FD, ResultType,
                                         FD->getResultTypeSourceRange(),
                                         behavior);
      Reason.describe(FD);
      return false;
    }
  }

  if (AFD->hasAsync()) {
    // Asynchronous functions move all of the result value and thrown error
    // information into a completion handler.
    auto FD = dyn_cast<FuncDecl>(AFD);
    if (!FD) {
      softenIfAccessNote(AFD, Reason.getAttr(),
        AFD->diagnose(diag::not_objc_function_async, AFD->getDescriptiveKind())
          .highlight(AFD->getAsyncLoc())
          .limitBehavior(behavior));
      Reason.describe(AFD);

      return false;
    }

    // The completion handler transformation cannot properly represent a
    // dynamic 'Self' type, so disallow @objc for such methods.
    if (FD->hasDynamicSelfResult()) {
      AFD->diagnose(diag::async_objc_dynamic_self)
          .highlight(AFD->getAsyncLoc())
          .limitBehavior(behavior);
      Reason.describe(AFD);

      return false;
    }

    // The completion handler parameter always goes at the end.
    unsigned completionHandlerParamIndex = AFD->getParameters()->size();

    // Decompose the return type to form the parameter type of the completion
    // handler.
    SmallVector<AnyFunctionType::Param, 2> completionHandlerParams;
    auto addCompletionHandlerParam = [&](Type type) {
      // For a throwing asynchronous function, make each parameter type optional
      // if that's representable in Objective-C.
      if (AFD->hasThrows() &&
          !type->getOptionalObjectType() &&
          isValidObjectiveCErrorResultType(const_cast<FuncDecl *>(FD), type)) {
        type = OptionalType::get(type);
      }

      completionHandlerParams.push_back(AnyFunctionType::Param(type));

      // Make sure that the parameter type is representable in Objective-C.
      if (!type->isRepresentableIn(
              ForeignLanguage::ObjectiveC, const_cast<FuncDecl *>(FD))) {
        softenIfAccessNote(AFD, Reason.getAttr(),
          AFD->diagnose(diag::objc_invalid_on_func_result_type,
                        getObjCDiagnosticAttrKind(Reason))
              .limitBehavior(behavior));
        diagnoseTypeNotRepresentableInObjC(FD, type,
                                           FD->getResultTypeSourceRange(),
                                           behavior);
        Reason.describe(FD);

        return true;
      }

      return false;
    };

    // Translate the result type of the function into parameters for the
    // completion handler parameter, exploding one level of tuple if needed.
    Type resultType = FD->mapTypeIntoContext(FD->getResultInterfaceType());
    if (auto tupleType = resultType->getAs<TupleType>()) {
      for (const auto &tupleElt : tupleType->getElements()) {
        if (addCompletionHandlerParam(tupleElt.getType()))
          return false;
      }
    } else {
      if (addCompletionHandlerParam(resultType))
        return false;
    }

    // For a throwing asynchronous function, an Error? parameter is added
    // to the completion handler parameter, and will be non-nil to signal
    // a thrown error.
    Optional<unsigned> completionHandlerErrorParamIndex;
    if (FD->hasThrows()) {
      completionHandlerErrorParamIndex = completionHandlerParams.size();
      auto errorType = ctx.getErrorExistentialType();
      addCompletionHandlerParam(OptionalType::get(errorType));
    }

    Type completionHandlerType = FunctionType::get(
        completionHandlerParams, TupleType::getEmpty(ctx),
        ASTExtInfoBuilder(FunctionTypeRepresentation::Block, false).build());

    asyncConvention = ForeignAsyncConvention(
        completionHandlerType->getCanonicalType(), completionHandlerParamIndex,
        completionHandlerErrorParamIndex,
        /* no flag argument */ None, false);
  } else if (AFD->hasThrows()) {
    // Synchronous throwing functions must map to a particular error convention.
    DeclContext *dc = const_cast<AbstractFunctionDecl *>(AFD);
    SourceLoc throwsLoc;
    Type resultType;

    const ConstructorDecl *ctor = nullptr;
    if (auto func = dyn_cast<FuncDecl>(AFD)) {
      resultType = func->getResultInterfaceType();
      throwsLoc = func->getThrowsLoc();
    } else {
      ctor = cast<ConstructorDecl>(AFD);
      throwsLoc = ctor->getThrowsLoc();
    }

    ForeignErrorConvention::Kind kind;
    CanType errorResultType;
    Type optOptionalType;
    if (ctor) {
      // Initializers always use the nil result convention.
      kind = ForeignErrorConvention::NilResult;

      // Only non-failing initializers can throw.
      if (ctor->isFailable()) {
        softenIfAccessNote(AFD, Reason.getAttr(),
          AFD->diagnose(diag::objc_invalid_on_failing_init,
                        getObjCDiagnosticAttrKind(Reason))
            .highlight(throwsLoc)
            .limitBehavior(behavior));
        Reason.describe(AFD);

        return false;
      }
    } else if (resultType->isVoid()) {
      // Functions that return nothing (void) can be throwing; they indicate
      // failure with a 'false' result.
      kind = ForeignErrorConvention::ZeroResult;
      NominalTypeDecl *boolDecl = ctx.getObjCBoolDecl();
      // On Linux, we might still run @objc tests even though there's
      // no ObjectiveC Foundation, so use Swift.Bool instead of crapping
      // out.
      if (boolDecl == nullptr)
        boolDecl = ctx.getBoolDecl();

      if (boolDecl == nullptr) {
        AFD->diagnose(diag::broken_bool);
        return false;
      }

      errorResultType = boolDecl->getDeclaredInterfaceType()->getCanonicalType();
    } else if (!resultType->getOptionalObjectType() &&
               isValidObjectiveCErrorResultType(dc, resultType)) {
      // Functions that return a (non-optional) type bridged to Objective-C
      // can be throwing; they indicate failure with a nil result.
      kind = ForeignErrorConvention::NilResult;
    } else if ((optOptionalType = resultType->getOptionalObjectType()) &&
               isValidObjectiveCErrorResultType(dc, optOptionalType)) {
      // Cannot return an optional bridged type, because 'nil' is reserved
      // to indicate failure. Call this out in a separate diagnostic.
      softenIfAccessNote(AFD, Reason.getAttr(),
        AFD->diagnose(diag::objc_invalid_on_throwing_optional_result,
                      getObjCDiagnosticAttrKind(Reason),
                      resultType)
          .highlight(throwsLoc)
          .limitBehavior(behavior));
      Reason.describe(AFD);
      return false;
    } else {
      // Other result types are not permitted.
      softenIfAccessNote(AFD, Reason.getAttr(),
        AFD->diagnose(diag::objc_invalid_on_throwing_result,
                      getObjCDiagnosticAttrKind(Reason), resultType)
          .highlight(throwsLoc)
          .limitBehavior(behavior));
      Reason.describe(AFD);
      return false;
    }

    // The error type is always 'AutoreleasingUnsafeMutablePointer<NSError?>?'.
    auto nsErrorTy = ctx.getNSErrorType();
    Type errorParameterType;
    if (nsErrorTy) {
      errorParameterType = OptionalType::get(nsErrorTy);
      errorParameterType
        = BoundGenericType::get(
            ctx.getAutoreleasingUnsafeMutablePointerDecl(),
            nullptr,
            errorParameterType);
      errorParameterType = OptionalType::get(errorParameterType);
    }

    // Determine the parameter index at which the error will go.
    unsigned errorParameterIndex;
    bool foundErrorParameterIndex = false;

    // If there is an explicit @objc attribute with a name, look for
    // the "error" selector piece.
    if (auto objc = AFD->getAttrs().getAttribute<ObjCAttr>()) {
      if (auto objcName = objc->getName()) {
        auto selectorPieces = objcName->getSelectorPieces();
        for (unsigned i = selectorPieces.size(); i > 0; --i) {
          // If the selector piece is "error", this is the location of
          // the error parameter.
          auto piece = selectorPieces[i-1];
          if (piece == ctx.Id_error) {
            errorParameterIndex = i-1;
            foundErrorParameterIndex = true;
            break;
          }

          // If the first selector piece ends with "Error", it's here.
          if (i == 1 && camel_case::getLastWord(piece.str()) == "Error") {
            errorParameterIndex = i-1;
            foundErrorParameterIndex = true;
            break;
          }
        }
      }
    }

    // If the selector did not provide an index for the error, find
    // the last parameter that is not a trailing closure.
    if (!foundErrorParameterIndex) {
      auto *paramList = AFD->getParameters();
      errorParameterIndex = paramList->size();

      // Note: the errorParameterIndex is actually a SIL function
      // parameter index, which means tuples are exploded. Normally
      // tuple types cannot be bridged to Objective-C, except for
      // one special case -- a constructor with a single named parameter
      // 'foo' of tuple type becomes a zero-argument selector named
      // 'initFoo'.
      if (auto *CD = dyn_cast<ConstructorDecl>(AFD))
        if (CD->isObjCZeroParameterWithLongSelector())
          --errorParameterIndex;

      while (errorParameterIndex > 0) {
        // Skip over trailing closures.
        auto type = paramList->get(errorParameterIndex - 1)->getType();

        // It can't be a trailing closure unless it has a specific form.
        // Only consider the rvalue type.
        type = type->getRValueType();

        // Look through one level of optionality.
        if (auto objectType = type->getOptionalObjectType())
          type = objectType;

        // Is it a function type?
        if (!type->is<AnyFunctionType>()) break;
        --errorParameterIndex;
      }
    }

    // Form the error convention.
    CanType canErrorParameterType;
    if (errorParameterType)
      canErrorParameterType = errorParameterType->getCanonicalType();
    switch (kind) {
    case ForeignErrorConvention::ZeroResult:
      errorConvention = ForeignErrorConvention::getZeroResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType,
                          errorResultType);
      break;

    case ForeignErrorConvention::NonZeroResult:
      errorConvention = ForeignErrorConvention::getNonZeroResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType,
                          errorResultType);
      break;

    case ForeignErrorConvention::ZeroPreservedResult:
      errorConvention = ForeignErrorConvention::getZeroPreservedResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType);
      break;

    case ForeignErrorConvention::NilResult:
      errorConvention = ForeignErrorConvention::getNilResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType);
      break;

    case ForeignErrorConvention::NonNilError:
      errorConvention = ForeignErrorConvention::getNilResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType);
      break;
    }
  }

  return true;
}

bool swift::isRepresentableInObjC(const VarDecl *VD, ObjCReason Reason) {
  // If you change this function, you must add or modify a test in PrintAsClang.
  
  if (VD->isInvalid())
    return false;

  Type T = VD->getDeclContext()->mapTypeIntoContext(VD->getInterfaceType());
  if (auto *RST = T->getAs<ReferenceStorageType>()) {
    // In-memory layout of @weak and @unowned does not correspond to anything
    // in Objective-C, but this does not really matter here, since Objective-C
    // uses getters and setters to operate on the property.
    // Because of this, look through @weak and @unowned.
    T = RST->getReferentType();
  }
  ASTContext &ctx = VD->getASTContext();
  DiagnosticStateRAII diagState(ctx.Diags);

  bool Result = T->isRepresentableIn(ForeignLanguage::ObjectiveC,
                                     VD->getDeclContext());

  if (Result && checkObjCInExtensionContext(VD, Reason))
    return false;

  if (checkObjCInForeignClassContext(VD, Reason))
    return false;
  if (checkObjCActorIsolation(VD, Reason))
    return false;

  auto behavior = behaviorLimitForObjCReason(Reason, ctx);

  // effectful computed properties cannot be represented, and its an error
  // to mark them as @objc
  if (VD->getEffectfulGetAccessor()) {
    softenIfAccessNote(VD, Reason.getAttr(),
      VD->diagnose(diag::effectful_not_representable_objc,
                   VD->getDescriptiveKind())
          .limitBehavior(behavior));
    Reason.describe(VD);
    return false;
  }

  if (!Result) {
    SourceRange TypeRange = VD->getTypeSourceRangeForDiagnostics();
    // TypeRange can be invalid; e.g. '@objc let foo = SwiftType()'
    if (TypeRange.isInvalid())
      TypeRange = VD->getNameLoc();

    softenIfAccessNote(VD, Reason.getAttr(),
      VD->diagnose(diag::objc_invalid_on_var, getObjCDiagnosticAttrKind(Reason))
          .highlight(TypeRange)
          .limitBehavior(behavior));
    diagnoseTypeNotRepresentableInObjC(VD->getDeclContext(),
                                       VD->getInterfaceType(),
                                       TypeRange, behavior);
    Reason.describe(VD);
  }

  return Result;
}

bool swift::isRepresentableInObjC(const SubscriptDecl *SD, ObjCReason Reason) {
  // If you change this function, you must add or modify a test in PrintAsClang.
  ASTContext &ctx = SD->getASTContext();
  DiagnosticStateRAII diagState(ctx.Diags);
  auto behavior = behaviorLimitForObjCReason(Reason, ctx);

  if (checkObjCInForeignClassContext(SD, Reason))
    return false;
  if (checkObjCWithGenericParams(SD, Reason))
    return false;
  if (checkObjCActorIsolation(SD, Reason))
    return false;

  // effectful subscripts cannot be represented, and its an error
  // to mark them as @objc
  if (SD->getEffectfulGetAccessor()) {
    softenIfAccessNote(SD, Reason.getAttr(),
      SD->diagnose(diag::effectful_not_representable_objc,
                   SD->getDescriptiveKind())
          .limitBehavior(behavior));
    Reason.describe(SD);
    return false;
  }

  // ObjC doesn't support class subscripts.
  if (!SD->isInstanceMember()) {
    softenIfAccessNote(SD, Reason.getAttr(),
      SD->diagnose(diag::objc_invalid_on_static_subscript,
                   SD->getDescriptiveKind(), Reason)
          .limitBehavior(behavior));
    Reason.describe(SD);
    Reason.setAttrInvalid();
    return true;
  }

  // Figure out the type of the indices.
  auto SubscriptType = SD->getInterfaceType()->getAs<AnyFunctionType>();
  if (!SubscriptType)
    return false;

  if (SubscriptType->getParams().size() != 1)
    return false;

  auto IndexParam = SubscriptType->getParams()[0];
  if (IndexParam.isInOut())
    return false;

  Type IndexType = SubscriptType->getParams()[0].getParameterType();
  if (IndexType->hasError())
    return false;

  bool IndexResult =
    IndexType->isRepresentableIn(ForeignLanguage::ObjectiveC,
                                 SD->getDeclContext());

  Type ElementType = SD->getElementInterfaceType();
  bool ElementResult = ElementType->isRepresentableIn(
        ForeignLanguage::ObjectiveC, SD->getDeclContext());
  bool Result = IndexResult && ElementResult;

  if (Result && checkObjCInExtensionContext(SD, Reason))
    return false;

  if (!Result) {
    SourceRange TypeRange;
    if (!IndexResult)
      TypeRange = SD->getIndices()->getSourceRange();
    else
      TypeRange = SD->getElementTypeSourceRange();
    softenIfAccessNote(SD, Reason.getAttr(),
      SD->diagnose(diag::objc_invalid_on_subscript,
                   getObjCDiagnosticAttrKind(Reason))
        .highlight(TypeRange)
        .limitBehavior(behavior));

    diagnoseTypeNotRepresentableInObjC(SD->getDeclContext(),
                                       !IndexResult ? IndexType
                                                    : ElementType,
                                       TypeRange, behavior);
    Reason.describe(SD);
  }

  return Result;
}

bool swift::canBeRepresentedInObjC(const ValueDecl *decl) {
  ASTContext &ctx = decl->getASTContext();
  if (!ctx.LangOpts.EnableObjCInterop)
    return false;

  if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
    Optional<ForeignAsyncConvention> asyncConvention;
    Optional<ForeignErrorConvention> errorConvention;
    return isRepresentableInObjC(func, ObjCReason::MemberOfObjCMembersClass,
                                 asyncConvention, errorConvention);
  }

  if (auto var = dyn_cast<VarDecl>(decl))
    return isRepresentableInObjC(var, ObjCReason::MemberOfObjCMembersClass);

  if (auto subscript = dyn_cast<SubscriptDecl>(decl))
    return isRepresentableInObjC(subscript,
                                 ObjCReason::MemberOfObjCMembersClass);

  return false;
}

#pragma mark "@objc declaration handling"

/// Whether this declaration is a member of a class extension marked @objc.
static bool isMemberOfObjCClassExtension(const ValueDecl *VD) {
  auto ext = dyn_cast<ExtensionDecl>(VD->getDeclContext());
  if (!ext) return false;

  return ext->getSelfClassDecl() && ext->getAttrs().hasAttribute<ObjCAttr>();
}

/// Whether this declaration is a member of a class with the `@objcMembers`
/// attribute.
static bool isMemberOfObjCMembersClass(const ValueDecl *VD) {
  auto classDecl = VD->getDeclContext()->getSelfClassDecl();
  if (!classDecl) return false;

  return classDecl->checkAncestry(AncestryFlags::ObjCMembers);
}

ObjCReason swift::objCReasonForObjCAttr(const ObjCAttr *attr) {
  if (attr->getAddedByAccessNote())
    return ObjCReason(ObjCReason::ExplicitlyObjCByAccessNote, attr);

  return ObjCReason(ObjCReason::ExplicitlyObjC, attr);
}

// A class is @objc if it does not have generic ancestry, and it either has
// an explicit @objc attribute, or its superclass is @objc.
static Optional<ObjCReason> shouldMarkClassAsObjC(const ClassDecl *CD) {
  ASTContext &ctx = CD->getASTContext();
  auto ancestry = CD->checkAncestry();

  if (auto attr = CD->getAttrs().getAttribute<ObjCAttr>()) {
    auto reason = objCReasonForObjCAttr(attr);
    auto behavior = behaviorLimitForObjCReason(reason, ctx);

    if (ancestry.contains(AncestryFlags::Generic)) {
      if (attr->hasName() && !CD->isGenericContext()) {
        // @objc with a name on a non-generic subclass of a generic class is
        // just controlling the runtime name. Don't diagnose this case.
        const_cast<ClassDecl *>(CD)->getAttrs().add(
          new (ctx) ObjCRuntimeNameAttr(*attr));
        return None;
      }

      swift::diagnoseAndRemoveAttr(CD, attr, diag::objc_for_generic_class)
        .limitBehavior(behavior);
      reason.describe(CD);
    }

    // If the class has resilient ancestry, @objc just controls the runtime
    // name unless all targets where the class is available support
    // class stubs.
    if (ancestry.contains(AncestryFlags::ResilientOther) &&
        !checkObjCClassStubAvailability(ctx, CD)) {
      if (attr->hasName()) {
        const_cast<ClassDecl *>(CD)->getAttrs().add(
          new (ctx) ObjCRuntimeNameAttr(*attr));
        return None;
      }


      auto &target = ctx.LangOpts.Target;
      auto platform = prettyPlatformString(targetPlatform(ctx.LangOpts));
      auto range = getMinOSVersionForClassStubs(target);
      auto *ancestor = getResilientAncestor(CD->getParentModule(), CD);
      swift::diagnoseAndRemoveAttr(CD, attr,
                                   diag::objc_for_resilient_class,
                                   ancestor->getName(),
                                   platform,
                                   range.getLowerEndpoint())
        .limitBehavior(behavior);
      reason.describe(CD);
    }

    // Only allow actors and ObjC-rooted classes to be @objc.
    // (Leave a hole for test cases.)
    if (ancestry.contains(AncestryFlags::ObjC) &&
        !ancestry.contains(AncestryFlags::ClangImported) &&
        !CD->isActor()) {
      if (ctx.LangOpts.EnableObjCAttrRequiresFoundation) {
        swift::diagnoseAndRemoveAttr(CD, attr,
                                     diag::invalid_objc_swift_rooted_class)
          .limitBehavior(behavior);
        reason.describe(CD);
        // If the user has not spelled out a superclass, offer to insert
        // 'NSObject'. We could also offer to replace the existing superclass,
        // but that's a touch aggressive.
         if (CD->getInherited().empty()) {
           auto nameEndLoc = Lexer::getLocForEndOfToken(ctx.SourceMgr,
                                                        CD->getNameLoc());
           CD->diagnose(diag::invalid_objc_swift_root_class_insert_nsobject)
             .fixItInsert(nameEndLoc, ": NSObject")
             .limitBehavior(behavior);
         } else if (CD->getSuperclass().isNull()) {
           CD->diagnose(diag::invalid_objc_swift_root_class_insert_nsobject)
             .fixItInsert(CD->getInherited().front().getLoc(), "NSObject, ")
             .limitBehavior(behavior);
         }
      }

      if (!ctx.LangOpts.EnableObjCInterop) {
        swift::diagnoseAndRemoveAttr(CD, attr, diag::objc_interop_disabled)
          .limitBehavior(behavior);
        reason.describe(CD);
      }
    }

    return reason;
  }

  if (ancestry.contains(AncestryFlags::ObjC)) {
    if (ancestry.contains(AncestryFlags::Generic)) {
      return None;
    }

    if (ancestry.contains(AncestryFlags::ResilientOther) &&
        !checkObjCClassStubAvailability(ctx, CD)) {
      return None;
    }

    return ObjCReason(ObjCReason::ImplicitlyObjC);
  }

  return None;
}

/// Figure out if a declaration should be exported to Objective-C.
static
Optional<ObjCReason> shouldMarkAsObjC(const ValueDecl *VD, bool allowImplicit) {
  // If Objective-C interoperability is disabled, nothing gets marked as @objc.
  if (!VD->getASTContext().LangOpts.EnableObjCInterop)
    return None;

  if (auto classDecl = dyn_cast<ClassDecl>(VD)) {
    return shouldMarkClassAsObjC(classDecl);
  }

  // Infer @objc for @_dynamicReplacement(for:) when replaced decl is @objc.
  if (isa<AbstractFunctionDecl>(VD) || isa<AbstractStorageDecl>(VD))
    if (VD->getAttrs().hasAttribute<DynamicReplacementAttr>()) {
      if (auto *replaced = VD->getDynamicallyReplacedDecl()) {
        if (replaced->isObjC())
          return ObjCReason(ObjCReason::ImplicitlyObjC);
      }
    }

  // Destructors are always @objc, with -dealloc as their entry point.
  if (isa<DestructorDecl>(VD))
    return ObjCReason(ObjCReason::ImplicitlyObjC);

  ProtocolDecl *protocolContext =
      dyn_cast<ProtocolDecl>(VD->getDeclContext());
  bool isMemberOfObjCProtocol =
      protocolContext && protocolContext->isObjC();

  // Local function to determine whether we can implicitly infer @objc.
  auto canInferImplicitObjC = [&](bool allowAnyAccess) {
    if (VD->isInvalid())
      return false;
    if (VD->isOperator())
      return false;

    // Implicitly generated declarations are not @objc, except for constructors.
    if (!allowImplicit && VD->isImplicit())
      return false;

    if (!allowAnyAccess && VD->getFormalAccess() <= AccessLevel::FilePrivate)
      return false;

    if (auto accessor = dyn_cast<AccessorDecl>(VD)) {
      switch (accessor->getAccessorKind()) {
      case AccessorKind::DidSet:
      case AccessorKind::Modify:
      case AccessorKind::Read:
      case AccessorKind::WillSet:
        return false;

      case AccessorKind::MutableAddress:
      case AccessorKind::Address:
      case AccessorKind::Get:
      case AccessorKind::Set:
        break;
      }
    }
    return true;
  };

  // explicitly declared @objc.
  if (auto attr = VD->getAttrs().getAttribute<ObjCAttr>())
    return objCReasonForObjCAttr(attr);
  // Getter or setter for an @objc property or subscript.
  if (auto accessor = dyn_cast<AccessorDecl>(VD)) {
    if (accessor->getAccessorKind() == AccessorKind::Get ||
        accessor->getAccessorKind() == AccessorKind::Set) {
      if (accessor->getStorage()->isObjC())
        return ObjCReason(ObjCReason::Accessor);

      return None;
    }
  }
  // @IBOutlet, @IBAction, @IBSegueAction, @NSManaged, and @GKInspectable imply
  // @objc.
  //
  // @IBInspectable and @GKInspectable imply @objc quietly in Swift 3
  // (where they warn on failure) and loudly in Swift 4 (error on failure).
  if (auto attr = VD->getAttrs().getAttribute<IBOutletAttr>())
    return ObjCReason(ObjCReason::ExplicitlyIBOutlet, attr);
  if (auto attr = VD->getAttrs().getAttribute<IBActionAttr>())
    return ObjCReason(ObjCReason::ExplicitlyIBAction, attr);
  if (auto attr = VD->getAttrs().getAttribute<IBSegueActionAttr>())
    return ObjCReason(ObjCReason::ExplicitlyIBSegueAction, attr);
  if (auto attr = VD->getAttrs().getAttribute<IBInspectableAttr>())
    return ObjCReason(ObjCReason::ExplicitlyIBInspectable, attr);
  if (auto attr = VD->getAttrs().getAttribute<GKInspectableAttr>())
    return ObjCReason(ObjCReason::ExplicitlyGKInspectable, attr);
  if (auto attr = VD->getAttrs().getAttribute<NSManagedAttr>()) {
    // Make sure the implicit DynamicAttr gets added before we potentially
    // disable this attribute.
    (void) VD->isDynamic();
    return ObjCReason(ObjCReason::ExplicitlyNSManaged, attr);
  }
  // A member of an @objc protocol is implicitly @objc.
  if (isMemberOfObjCProtocol) {
    if (!VD->isProtocolRequirement())
      return None;
    return ObjCReason(ObjCReason::MemberOfObjCProtocol);
  }

  // A member implementation of an @objcImplementation extension is @objc.
  if (VD->isObjCMemberImplementation())
    // FIXME: New ObjCReason::Kind?
    return ObjCReason(ObjCReason::ImplicitlyObjC);

  // A @nonobjc is not @objc, even if it is an override of an @objc, so check
  // for @nonobjc first.
  if (VD->getAttrs().hasAttribute<NonObjCAttr>() ||
      (isa<ExtensionDecl>(VD->getDeclContext()) &&
       cast<ExtensionDecl>(VD->getDeclContext())->getAttrs()
        .hasAttribute<NonObjCAttr>()))
    return None;

  if (isMemberOfObjCClassExtension(VD) && 
      canInferImplicitObjC(/*allowAnyAccess*/true))
    return ObjCReason(ObjCReason::MemberOfObjCExtension);
  if (isMemberOfObjCMembersClass(VD) && 
      canInferImplicitObjC(/*allowAnyAccess*/false))
    return ObjCReason(ObjCReason::MemberOfObjCMembersClass);

  // An override of an @objc declaration is implicitly @objc.
  if (VD->getOverriddenDecl() && VD->getOverriddenDecl()->isObjC())
    return ObjCReason(ObjCReason::OverridesObjC);
  // A witness to an @objc protocol requirement is implicitly @objc.
  if (VD->getDeclContext()->getSelfClassDecl()) {
    auto requirements =
      findWitnessedObjCRequirements(VD, /*anySingleRequirement=*/true);
    if (!requirements.empty())
      return ObjCReason::witnessToObjC(requirements.front());
  }

  ASTContext &ctx = VD->getASTContext();

  // Under Swift 3's @objc inference rules, 'dynamic' infers '@objc'.
  if (auto attr = VD->getAttrs().getAttribute<DynamicAttr>()) {
    bool isGetterOrSetter =
      isa<AccessorDecl>(VD) && cast<AccessorDecl>(VD)->isGetterOrSetter();

    if (ctx.LangOpts.EnableSwift3ObjCInference) {
      // If we've been asked to warn about deprecated @objc inference, do so
      // now.
      if (ctx.LangOpts.WarnSwift3ObjCInference !=
            Swift3ObjCInferenceWarnings::None &&
          !isGetterOrSetter) {
        VD->diagnose(diag::objc_inference_swift3_dynamic)
          .highlight(attr->getLocation())
          .fixItInsert(VD->getAttributeInsertionLoc(/*forModifier=*/false),
                      "@objc ");
      }

      return ObjCReason(ObjCReason::ExplicitlyDynamic, attr);
    }
  }

  // If we aren't provided Swift 3's @objc inference rules, we're done.
  if (!ctx.LangOpts.EnableSwift3ObjCInference)
    return None;

  // Infer '@objc' for valid, non-implicit, non-operator, members of classes
  // (and extensions thereof) whose class hierarchies originate in Objective-C,
  // e.g., which derive from NSObject, so long as the members have internal
  // access or greater.
  if (!canInferImplicitObjC(/*allowAnyAccess*/false))
    return None;

  // If this declaration is part of a class with implicitly @objc members,
  // make it implicitly @objc. However, if the declaration cannot be represented
  // as @objc, don't diagnose.
  if (auto classDecl = VD->getDeclContext()->getSelfClassDecl()) {
    // One cannot define @objc members of any foreign classes.
    if (classDecl->isForeign())
      return None;

    if (classDecl->checkAncestry(AncestryFlags::ObjC))
      return ObjCReason(ObjCReason::MemberOfObjCSubclass);
  }

  return None;
}

/// Determine whether the given type is a C integer type.
static bool isCIntegerType(Type type) {
  auto nominal = type->getAnyNominal();
  if (!nominal) return false;

  ASTContext &ctx = nominal->getASTContext();
  auto stdlibModule = ctx.getStdlibModule();
  if (nominal->getParentModule() != stdlibModule)
    return false;

  // Check for each of the C integer type equivalents in the standard library.
  auto matchesStdlibTypeNamed = [&](StringRef name) {
    auto identifier = ctx.getIdentifier(name);
    SmallVector<ValueDecl *, 2> foundDecls;
    stdlibModule->lookupValue(identifier, NLKind::UnqualifiedLookup,
                              foundDecls);
    for (auto found : foundDecls) {
      auto foundType = dyn_cast<TypeDecl>(found);
      if (!foundType)
        continue;

      if (foundType->getDeclaredInterfaceType()->isEqual(type))
        return true;
    }

    return false;
  };

#define MAP_BUILTIN_TYPE(_, __)
#define MAP_BUILTIN_INTEGER_TYPE(CLANG_BUILTIN_KIND, SWIFT_TYPE_NAME) \
  if (matchesStdlibTypeNamed(#SWIFT_TYPE_NAME))                       \
    return true;
#include "swift/ClangImporter/BuiltinMappedTypes.def"

  return false;
}

/// Determine whether the given enum should be @objc.
static bool isEnumObjC(EnumDecl *enumDecl) {
  // FIXME: Use shouldMarkAsObjC once it loses it's TypeChecker argument.

  // If there is no @objc attribute, it's not @objc.
  if (!enumDecl->getAttrs().hasAttribute<ObjCAttr>())
    return false;

  Type rawType = enumDecl->getRawType();

  // @objc enums must have a raw type.
  if (!rawType) {
    enumDecl->diagnose(diag::objc_enum_no_raw_type);
    return false;
  }

  // If the raw type contains an error, we've already diagnosed it.
  if (rawType->hasError())
    return false;

  // The raw type must be one of the C integer types.
  if (!isCIntegerType(rawType)) {
    SourceRange errorRange;
    if (!enumDecl->getInherited().empty())
      errorRange = enumDecl->getInherited().front().getSourceRange();
    enumDecl->diagnose(diag::objc_enum_raw_type_not_integer, rawType)
      .highlight(errorRange);
    return false;
  }

  // We need at least one case to have a raw value.
  if (enumDecl->getAllElements().empty()) {
    enumDecl->diagnose(diag::empty_enum_raw_type);
  }
  
  return true;
}

/// Record that a declaration is @objc.
static void markAsObjC(ValueDecl *D, ObjCReason reason,
                       Optional<ForeignAsyncConvention> asyncConvention,
                       Optional<ForeignErrorConvention> errorConvention);


bool IsObjCRequest::evaluate(Evaluator &evaluator, ValueDecl *VD) const {
  DiagnosticStateRAII diagState(VD->getASTContext().Diags);

  // Access notes may add attributes that affect this calculus.
  TypeChecker::applyAccessNote(VD);

  auto dc = VD->getDeclContext();
  Optional<ObjCReason> isObjC;
  if (dc->getSelfClassDecl() && !isa<TypeDecl>(VD)) {
    // Members of classes can be @objc.
    isObjC = shouldMarkAsObjC(VD, isa<ConstructorDecl>(VD));
  }
  else if (auto classDecl = dyn_cast<ClassDecl>(VD)) {
    // Classes can be @objc.



    // Protocols and enums can also be @objc, but this is covered by the
    // isObjC() check at the beginning.
    isObjC = shouldMarkAsObjC(VD, /*allowImplicit=*/false);
  } else if (auto enumDecl = dyn_cast<EnumDecl>(VD)) {
    // Enums can be @objc so long as they have a raw type that is representable
    // as an arithmetic type in C.
    if (isEnumObjC(enumDecl))
      isObjC = objCReasonForObjCAttr(
                                 enumDecl->getAttrs().getAttribute<ObjCAttr>());
  } else if (auto enumElement = dyn_cast<EnumElementDecl>(VD)) {
    // Enum elements can be @objc so long as the containing enum is @objc.
    if (enumElement->getParentEnum()->isObjC()) {
      if (auto attr = enumElement->getAttrs().getAttribute<ObjCAttr>())
        isObjC = objCReasonForObjCAttr(attr);
      else
        isObjC = ObjCReason::ElementOfObjCEnum;
    }
  } else if (auto proto = dyn_cast<ProtocolDecl>(VD)) {
    if (auto attr = proto->getAttrs().getAttribute<ObjCAttr>()) {
      isObjC = objCReasonForObjCAttr(attr);

      // If the protocol is @objc, it may only refine other @objc protocols and
      // marker protocols.
      // FIXME: Revisit this restriction.
      for (auto inherited : proto->getInheritedProtocols()) {
        if (!inherited->isObjC() && !inherited->isMarkerProtocol()) {
          proto->diagnose(diag::objc_protocol_inherits_non_objc_protocol,
                          proto->getDeclaredInterfaceType(),
                          inherited->getDeclaredInterfaceType());
          inherited->diagnose(diag::kind_declname_declared_here,
                              DescriptiveDeclKind::Protocol,
                              inherited->getName());
          isObjC = None;
        }
      }
    }
  } else if (isa<ProtocolDecl>(dc) && cast<ProtocolDecl>(dc)->isObjC()) {
    // Members of @objc protocols are @objc.
    isObjC = shouldMarkAsObjC(VD, isa<ConstructorDecl>(VD));
  } else {
    // Cannot be @objc.
  }

  // If this declaration should not be exposed to Objective-C, we're done.
  if (!isObjC)
    return false;

  if (auto accessor = dyn_cast<AccessorDecl>(VD)) {
    auto storage = accessor->getStorage();
    if (auto storageObjCAttr = storage->getAttrs().getAttribute<ObjCAttr>()) {
      // If @objc on the storage declaration was inferred using a
      // deprecated rule, but this accessor is @objc in its own right,
      // complain.
      ASTContext &ctx = dc->getASTContext();
      auto behavior = behaviorLimitForObjCReason(*isObjC, ctx);
      if (storageObjCAttr && storageObjCAttr->isSwift3Inferred()) {
        storage->diagnose(diag::accessor_swift3_objc_inference,
                 storage->getDescriptiveKind(), storage->getName(),
                 isa<SubscriptDecl>(storage), accessor->isSetter())
          .fixItInsert(storage->getAttributeInsertionLoc(/*forModifier=*/false),
                       "@objc ")
          .limitBehavior(behavior);
      }
    }
  }

  // If needed, check whether this declaration is representable in Objective-C.
  Optional<ForeignAsyncConvention> asyncConvention;
  Optional<ForeignErrorConvention> errorConvention;
  if (auto var = dyn_cast<VarDecl>(VD)) {
    if (!isRepresentableInObjC(var, *isObjC)) {
      isObjC->setAttrInvalid();
      return false;
    }
  } else if (auto subscript = dyn_cast<SubscriptDecl>(VD)) {
    if (!isRepresentableInObjC(subscript, *isObjC)) {
      isObjC->setAttrInvalid();
      return false;
    }
  } else if (isa<DestructorDecl>(VD)) {
    // Destructors need no additional checking.
  } else if (auto func = dyn_cast<AbstractFunctionDecl>(VD)) {
    if (!isRepresentableInObjC(
            func, *isObjC, asyncConvention, errorConvention)) {
      isObjC->setAttrInvalid();
      return false;
    }
  }

  // Note that this declaration is exposed to Objective-C.
  markAsObjC(VD, *isObjC, asyncConvention, errorConvention);

  return true;
}

/// Infer the Objective-C name for a given declaration.
static ObjCSelector inferObjCName(ValueDecl *decl) {
  if (auto destructor = dyn_cast<DestructorDecl>(decl))
    return destructor->getObjCSelector();

  auto attr = decl->getAttrs().getAttribute<ObjCAttr>();

  ASTContext &ctx = decl->getASTContext();

  /// Set the @objc name.
  auto setObjCName = [&](ObjCSelector selector) {
    // If there already is an @objc attribute, invalidate and remove it. (This
    // helps with access notes.)
    if (attr) {
      attr->setInvalid();
      decl->getAttrs().removeAttribute(attr);
    }

    // Create an @objc attribute with the implicit name.
    attr = ObjCAttr::create(ctx, selector, /*implicitName=*/true);
    decl->getAttrs().add(attr);
  };

  // If this declaration overrides an @objc declaration, use its name.
  if (auto overridden = decl->getOverriddenDecl()) {
    if (overridden->isObjC()) {
      // Handle methods first.
      if (auto overriddenFunc = dyn_cast<AbstractFunctionDecl>(overridden)) {
        // Determine the selector of the overridden method.
        ObjCSelector overriddenSelector = overriddenFunc->getObjCSelector();

        // Determine whether there is a name conflict.
        bool shouldFixName = !attr || !attr->hasName();
        if (attr && attr->hasName() && *attr->getName() != overriddenSelector) {
          auto reason = objCReasonForObjCAttr(attr);
          auto behavior = behaviorLimitForObjCReason(reason, ctx);

          // If the user explicitly wrote the incorrect name, complain.
          if (!attr->isNameImplicit()) {
            {
              auto diag = std::move(
                              ctx.Diags.diagnose(
                                attr->AtLoc,
                                diag::objc_override_method_selector_mismatch,
                                *attr->getName(), overriddenSelector)
                      .limitBehavior(behavior));
              fixDeclarationObjCName(diag, decl, attr->getName(),
                                     overriddenSelector);
            }

            overriddenFunc->diagnose(diag::overridden_here);
          }

          shouldFixName = true;
        }

        // If we have to set the name, do so.
        if (shouldFixName) {
          // Override the name on the attribute.
          setObjCName(overriddenSelector);
        }
        return overriddenSelector;
      }

      // Handle properties.
      if (auto overriddenProp = dyn_cast<VarDecl>(overridden)) {
        Identifier overriddenName = overriddenProp->getObjCPropertyName();
        ObjCSelector overriddenNameAsSel(ctx, 0, overriddenName);

        // Determine whether there is a name conflict.
        bool shouldFixName = !attr || !attr->hasName();
        if (attr && attr->hasName() &&
            *attr->getName() != overriddenNameAsSel) {
          auto reason = objCReasonForObjCAttr(attr);
          auto behavior = behaviorLimitForObjCReason(reason, ctx);

          // If the user explicitly wrote the wrong name, complain.
          if (!attr->isNameImplicit()) {
            SourceLoc diagLoc = attr->AtLoc, firstNameLoc;
            if (diagLoc.isInvalid())
              diagLoc = decl->getLoc();
            if (!attr->getNameLocs().empty())
              firstNameLoc = attr->getNameLocs().front();
            softenIfAccessNote(decl, attr,
              ctx.Diags.diagnose(diagLoc,
                          diag::objc_override_property_name_mismatch,
                          attr->getName()->getSelectorPieces()[0],
                          overriddenName)
                .fixItReplaceChars(firstNameLoc,
                                   attr->getRParenLoc(),
                                   overriddenName.str())
                .limitBehavior(behavior));
            overridden->diagnose(diag::overridden_here);
          }

          shouldFixName = true;
        }

        // Fix the name, if needed.
        if (shouldFixName) {
          setObjCName(overriddenNameAsSel);
        }
        return overriddenNameAsSel;
      }
    }
  }

  // If the decl already has a name, do nothing; the protocol conformance
  // checker will handle any mismatches.
  if (attr && attr->hasName())
    return *attr->getName();

  // When no override determined the Objective-C name, look for
  // requirements for which this declaration is a witness.
  Optional<ObjCSelector> requirementObjCName;
  ValueDecl *firstReq = nullptr;
  for (auto req : findWitnessedObjCRequirements(decl)) {
    // If this is the first requirement, take its name.
    if (!requirementObjCName) {
      requirementObjCName = req->getObjCRuntimeName();
      firstReq = req;
      continue;
    }

    // If this requirement has a different name from one we've seen,
    // note the ambiguity.
    if (*requirementObjCName != *req->getObjCRuntimeName()) {
      decl->diagnose(diag::objc_ambiguous_inference,
                     decl->getDescriptiveKind(), decl->getName(),
                     *requirementObjCName, *req->getObjCRuntimeName());

      // Note the candidates and what Objective-C names they provide.
      auto diagnoseCandidate = [&](ValueDecl *req) {
        auto proto = cast<ProtocolDecl>(req->getDeclContext());
        auto diag = decl->diagnose(diag::objc_ambiguous_inference_candidate,
                                   req->getName(),
                                   proto->getName(),
                                   *req->getObjCRuntimeName());
        fixDeclarationObjCName(diag, decl,
                               decl->getObjCRuntimeName(/*skipIsObjC=*/true),
                               req->getObjCRuntimeName());
      };
      diagnoseCandidate(firstReq);
      diagnoseCandidate(req);

      // Suggest '@nonobjc' to suppress this error, and not try to
      // infer @objc for anything.
      decl->diagnose(diag::req_near_match_nonobjc, true)
        .fixItInsert(decl->getAttributeInsertionLoc(false), "@nonobjc ");
      break;
    }
  }

  // If we have a name, install it via an @objc attribute.
  if (requirementObjCName) {
    setObjCName(*requirementObjCName);
    return *requirementObjCName;
  }

  return *decl->getObjCRuntimeName(true);
}

/// Mark the given declaration as being Objective-C compatible (or
/// not) as appropriate.
///
/// If the declaration has a @nonobjc attribute, diagnose an error
/// using the given Reason, if present.
void markAsObjC(ValueDecl *D, ObjCReason reason,
                Optional<ForeignAsyncConvention> asyncConvention,
                Optional<ForeignErrorConvention> errorConvention) {
  ASTContext &ctx = D->getASTContext();

  // By now, the caller will have handled the case where an implicit @objc
  // could be overridden by @nonobjc. If we see a @nonobjc and we are trying
  // to add an @objc for whatever reason, diagnose an error.
  if (auto *attr = D->getAttrs().getAttribute<NonObjCAttr>()) {
    if (behaviorLimitForObjCReason(reason, ctx) == DiagnosticBehavior::Ignore)
      reason = ObjCReason::ImplicitlyObjC;

    D->diagnose(diag::nonobjc_not_allowed,
                getObjCDiagnosticAttrKind(reason));

    attr->setInvalid();
  }

  if (auto method = dyn_cast<AbstractFunctionDecl>(D)) {
    // Determine the foreign async and error conventions.
    Optional<ForeignAsyncConvention> inheritedAsyncConvention;
    AbstractFunctionDecl *declProvidingInheritedAsyncConvention = nullptr;
    Optional<ForeignErrorConvention> inheritedErrorConvention;
    AbstractFunctionDecl *declProvidingInheritedErrorConvention = nullptr;
    if (auto baseMethod = method->getOverriddenDecl()) {
      // If the overridden method has a foreign async or error convention,
      // adopt it. Note that the foreign async or error convention affects the
      // selector, so we perform this before inferring a selector.
      if (method->hasAsync()) {
        if (auto baseAsyncConvention
              = baseMethod->getForeignAsyncConvention()) {
          inheritedAsyncConvention = baseAsyncConvention;
          declProvidingInheritedAsyncConvention = baseMethod;
        }
      }

      if (method->hasThrows()) {
        if (auto baseErrorConvention
              = baseMethod->getForeignErrorConvention()) {
          inheritedErrorConvention = baseErrorConvention;
          declProvidingInheritedErrorConvention = baseMethod;
        }
      }
    }
    
    for (auto req : findWitnessedObjCRequirements(method)) {
      auto reqMethod = dyn_cast<AbstractFunctionDecl>(req);
      if (!reqMethod) continue;

      // If the method witnesses an ObjC requirement that is async, adopt its
      // async convention.
      if (reqMethod->hasAsync()) {
        if (auto reqAsyncConvention = reqMethod->getForeignAsyncConvention()) {
          // Check for a conflict among protocol conformances or inherited
          // methods.
          if (declProvidingInheritedAsyncConvention
              && inheritedAsyncConvention != reqAsyncConvention) {
            method->diagnose(diag::objc_ambiguous_async_convention,
                             method->getName());
            declProvidingInheritedAsyncConvention->diagnose(
                diag::objc_ambiguous_async_convention_candidate,
                declProvidingInheritedAsyncConvention->getName());
            reqMethod->diagnose(diag::objc_ambiguous_async_convention_candidate,
                                reqMethod->getName());
            break;
          }

          inheritedAsyncConvention = reqAsyncConvention;
          declProvidingInheritedAsyncConvention = reqMethod;
        }
      }

      // If the method witnesses an ObjC requirement that throws, adopt its
      // error convention.
      if (reqMethod->hasThrows()) {
        if (auto reqErrorConvention = reqMethod->getForeignErrorConvention()) {
          // Check for a conflict among protocol conformances or inherited
          // methods.
          if (declProvidingInheritedErrorConvention
              && inheritedErrorConvention != reqErrorConvention) {
            method->diagnose(diag::objc_ambiguous_error_convention,
                             method->getName());
            declProvidingInheritedErrorConvention->diagnose(
                diag::objc_ambiguous_error_convention_candidate,
                declProvidingInheritedErrorConvention->getName());
            reqMethod->diagnose(diag::objc_ambiguous_error_convention_candidate,
                                reqMethod->getName());
            break;
          }

          inheritedErrorConvention = reqErrorConvention;
          declProvidingInheritedErrorConvention = reqMethod;
        }
      }
    }

    // Attach the foreign async convention.
    if (inheritedAsyncConvention) {
      assert(method->hasAsync() && "async objc req offered for sync witness?");
      method->setForeignAsyncConvention(*inheritedAsyncConvention);

    } else if (method->hasAsync()) {
      assert(asyncConvention && "Missing async convention");
      method->setForeignAsyncConvention(*asyncConvention);
    }

    // Attach the foreign error convention.
    if (inheritedErrorConvention) {
      // Diagnose if this is a method that does not throw
      // but inherits an ObjC error convention.
      if (!method->hasThrows())
        method->diagnose(diag::satisfy_throws_objc,
                         isa<ConstructorDecl>(method));
      else
        method->setForeignErrorConvention(*inheritedErrorConvention);

    } else if (method->hasThrows() && !method->hasAsync()) {
      assert(errorConvention && "Missing error convention");
      method->setForeignErrorConvention(*errorConvention);
    }

    // Infer the Objective-C name for this method.
    auto selector = inferObjCName(method);

    // Swift does not permit class methods with Objective-C selectors 'load',
    // 'alloc', or 'allocWithZone:'. Check for these cases.
    if (!method->isInstanceMember()) {
      auto isForbiddenSelector = [&](ObjCSelector sel)
      -> Optional<Diag<unsigned, DeclName, ObjCSelector>> {
        switch (sel.getNumArgs()) {
        case 0:
          if (sel.getSelectorPieces().front() == ctx.Id_load ||
              sel.getSelectorPieces().front() == ctx.Id_alloc)
            return diag::objc_class_method_not_permitted;
          // Swift 3 and earlier allowed you to override `initialize`, but
          // Swift's semantics do not guarantee that it will be called at
          // the point you expect. It is disallowed in Swift 4 and later.
          if (sel.getSelectorPieces().front() == ctx.Id_initialize)
            return diag::objc_class_method_not_permitted;
          return None;
        case 1:
          if (sel.getSelectorPieces().front() == ctx.Id_allocWithZone)
            return diag::objc_class_method_not_permitted;
          return None;
        default:
          return None;
        }
      };
      if (auto diagID = isForbiddenSelector(selector)) {
        auto diagInfo = getObjCMethodDiagInfo(method);
        method->diagnose(*diagID, diagInfo.first, diagInfo.second, selector);
      }
    }

    // Record the method in the type, if it's a member of one.
    if (auto tyDecl = D->getDeclContext()->getSelfNominalTypeDecl()) {
      tyDecl->recordObjCMethod(method, selector);
    }

    // Record the method in the source file.
    if (auto sourceFile = method->getParentSourceFile()) {
      sourceFile->ObjCMethods[selector].push_back(method);
    }
  } else if (isa<VarDecl>(D)) {
    // Infer the Objective-C name for this property.
    (void)inferObjCName(D);

    // FIXME: We should have a class-based table to check for conflicts.
  }

  // Special handling for Swift 3 @objc inference rules that are no longer
  // present in later versions of Swift.
  if (reason == ObjCReason::MemberOfObjCSubclass) {
    // If we've been asked to unconditionally warn about these deprecated
    // @objc inference rules, do so now. However, we don't warn about
    // accessors---just the main storage declarations.
    if (ctx.LangOpts.WarnSwift3ObjCInference ==
          Swift3ObjCInferenceWarnings::Complete &&
        !(isa<AccessorDecl>(D) && cast<AccessorDecl>(D)->isGetterOrSetter())) {
      D->diagnose(diag::objc_inference_swift3_objc_derived);
      D->diagnose(diag::objc_inference_swift3_addobjc)
        .fixItInsert(D->getAttributeInsertionLoc(/*forModifier=*/false),
                     "@objc ");
      D->diagnose(diag::objc_inference_swift3_addnonobjc)
        .fixItInsert(D->getAttributeInsertionLoc(/*forModifier=*/false),
                     "@nonobjc ");
    }

    // Mark the attribute as having used Swift 3 inference, or create an
    // implicit @objc for that purpose.
    auto attr = D->getAttrs().getAttribute<ObjCAttr>();
    if (!attr) {
      attr = ObjCAttr::createUnnamedImplicit(ctx);
      D->getAttrs().add(attr);
    }
    attr->setSwift3Inferred();
  }
}

/// Compute the information used to describe an Objective-C redeclaration.
std::pair<unsigned, DeclName> swift::getObjCMethodDiagInfo(
                                AbstractFunctionDecl *member) {
  if (isa<ConstructorDecl>(member))
    return { 0 + member->isImplicit(), member->getName() };

  if (isa<DestructorDecl>(member))
    return { 2 + member->isImplicit(), member->getName() };

  if (auto accessor = dyn_cast<AccessorDecl>(member)) {
    switch (accessor->getAccessorKind()) {
#define OBJC_ACCESSOR(ID, KEYWORD)
#define ACCESSOR(ID) \
    case AccessorKind::ID:
#include "swift/AST/AccessorKinds.def"
      llvm_unreachable("Not an Objective-C entry point");

    case AccessorKind::Get:
      if (auto var = dyn_cast<VarDecl>(accessor->getStorage()))
        return { 5, var->getName() };

      return { 6, Identifier() };

    case AccessorKind::Set:
      if (auto var = dyn_cast<VarDecl>(accessor->getStorage()))
        return { 7, var->getName() };
      return { 8, Identifier() };
    }

    llvm_unreachable("Unhandled AccessorKind in switch.");
  }

  // Normal method.
  auto func = cast<FuncDecl>(member);
  return { 4, func->getName() };
}

bool swift::fixDeclarationName(InFlightDiagnostic &diag, const ValueDecl *decl,
                               DeclName targetName) {
  if (decl->isImplicit()) return false;
  if (decl->getName() == targetName) return false;

  // Handle properties directly.
  if (auto var = dyn_cast<VarDecl>(decl)) {
    // Replace the name.
    SmallString<64> scratch;
    diag.fixItReplace(var->getNameLoc(), targetName.getString(scratch));
    return false;
  }

  // We only handle functions from here on.
  auto func = dyn_cast<AbstractFunctionDecl>(decl);
  if (!func) return true;

  const auto name = func->getName();

  // Fix the name of the function itself.
  if (name.getBaseName() != targetName.getBaseName()) {
    diag.fixItReplace(func->getLoc(), targetName.getBaseName().userFacingName());
  }

  // Fix the argument names that need fixing.
  if (name.getArgumentNames().size()
          != targetName.getArgumentNames().size())
    return false;
  
  auto params = func->getParameters();
  for (unsigned i = 0, n = name.getArgumentNames().size(); i != n; ++i) {
    auto origArg = name.getArgumentNames()[i];
    auto targetArg = targetName.getArgumentNames()[i];

    if (origArg == targetArg)
      continue;

    auto *param = params->get(i);

    // The parameter has an explicitly-specified API name, and it's wrong.
    if (param->getArgumentNameLoc() != param->getLoc() &&
        param->getArgumentNameLoc().isValid()) {
      // ... but the internal parameter name was right. Just zap the
      // incorrect explicit specialization.
      if (param->getName() == targetArg) {
        diag.fixItRemoveChars(param->getArgumentNameLoc(),
                              param->getLoc());
        continue;
      }

      // Fix the API name.
      StringRef targetArgStr = targetArg.empty()? "_" : targetArg.str();
      diag.fixItReplace(param->getArgumentNameLoc(), targetArgStr);
      continue;
    }

    // The parameter did not specify a separate API name. Insert one.
    if (targetArg.empty())
      diag.fixItInsert(param->getLoc(), "_ ");
    else {
      llvm::SmallString<8> targetArgStr;
      targetArgStr += targetArg.str();
      targetArgStr += ' ';
      diag.fixItInsert(param->getLoc(), targetArgStr);
    }
  }

  return false;
}

bool swift::fixDeclarationObjCName(InFlightDiagnostic &diag, const ValueDecl *decl,
                                   Optional<ObjCSelector> nameOpt,
                                   Optional<ObjCSelector> targetNameOpt,
                                   bool ignoreImpliedName) {
  if (decl->isImplicit())
    return false;

  // Subscripts cannot be renamed, so handle them directly.
  if (isa<SubscriptDecl>(decl)) {
    diag.fixItInsert(decl->getAttributeInsertionLoc(/*forModifier=*/false),
                     "@objc ");
    return false;
  }

  auto name = *nameOpt;
  auto targetName = *targetNameOpt;

  // Dig out the existing '@objc' attribute on the witness. We don't care
  // about implicit ones because they don't have useful source location
  // information.
  auto attr = decl->getAttrs().getAttribute<ObjCAttr>();
  if (attr && (attr->isImplicit() || attr->getLocation().isInvalid()))
    attr = nullptr;

  // If there is an @objc attribute with an explicit, incorrect witness
  // name, go fix the witness name.
  if (attr && name != targetName &&
      attr->hasName() && !attr->isNameImplicit()) {
    // Find the source range covering the full name.
    SourceLoc startLoc;
    if (attr->getNameLocs().empty())
      startLoc = attr->getRParenLoc();
    else
      startLoc = attr->getNameLocs().front();

    // Replace the name with the name of the requirement.
    SmallString<64> scratch;
    diag.fixItReplaceChars(startLoc, attr->getRParenLoc(),
                           targetName.getString(scratch));
    return false;
  }

  // We need to create or amend an @objc attribute with the appropriate name.

  // Form the Fix-It text.
  SourceLoc startLoc;
  SmallString<64> fixItText;
  {
    assert((!attr || !attr->hasName() || attr->isNameImplicit() ||
            name == targetName) && "Nothing to diagnose!");
    llvm::raw_svector_ostream out(fixItText);

    // If there is no @objc attribute, we need to add our own '@objc'.
    if (!attr) {
      startLoc = decl->getAttributeInsertionLoc(/*forModifier=*/false);
      out << "@objc";
    } else {
      startLoc = Lexer::getLocForEndOfToken(decl->getASTContext().SourceMgr,
                                            attr->getRange().End);
    }

    // If the names of the witness and requirement differ, we need to
    // specify the name.
    if (name != targetName || ignoreImpliedName) {
      out << "(";
      out << targetName;
      out << ")";
    }

    if (!attr)
      out << " ";
  }

  diag.fixItInsert(startLoc, fixItText);
  return false;
}

namespace {
  /// Produce a deterministic ordering of the given declarations.
  struct OrderDeclarations {
    bool operator()(ValueDecl *lhs, ValueDecl *rhs) const {
      // If the declarations come from different modules, order based on the
      // module.
      ModuleDecl *lhsModule = lhs->getDeclContext()->getParentModule();
      ModuleDecl *rhsModule = rhs->getDeclContext()->getParentModule();
      if (lhsModule != rhsModule) {
        return lhsModule->getName().str() < rhsModule->getName().str();
      }

      // If the two declarations are in the same source file, order based on
      // location within that source file.
      SourceFile *lhsSF = lhs->getDeclContext()->getParentSourceFile();
      SourceFile *rhsSF = rhs->getDeclContext()->getParentSourceFile();
      if (lhsSF && lhsSF == rhsSF) {
        // If only one location is valid, the valid location comes first.
        if (lhs->getLoc().isValid() != rhs->getLoc().isValid()) {
          return lhs->getLoc().isValid();
        }

        // Prefer the declaration that comes first in the source file.
        const auto &SrcMgr = lhsSF->getASTContext().SourceMgr;
        return SrcMgr.isBeforeInBuffer(lhs->getLoc(), rhs->getLoc());
      }

      // The declarations are in different source files (or unknown source
      // files) of the same module. Order based on name.
      // FIXME: This isn't a total ordering.
      return lhs->getName() < rhs->getName();
    }
  };
} // end anonymous namespace

/// Lookup for an Objective-C method with the given selector in the
/// given class or any of its superclasses. We intentionally don't respect
/// access control, since everything is visible to the Objective-C runtime.
static AbstractFunctionDecl *
lookupOverriddenObjCMethod(ClassDecl *classDecl, AbstractFunctionDecl *method,
                          bool inheritingInits = true) {
  assert(classDecl);

  // Look for an Objective-C method in this class.
  auto methods = classDecl->lookupDirect(method->getObjCSelector(),
                                         method->isObjCInstanceMethod());
  if (!methods.empty()) {
    // If we aren't inheriting initializers, remove any initializers from the
    // list.
    if (!inheritingInits) {
      llvm::erase_if(methods, [](AbstractFunctionDecl *afd) {
        return isa<ConstructorDecl>(afd);
      });
      if (methods.empty())
        return nullptr;
    }
    return *std::min_element(methods.begin(), methods.end(),
                             OrderDeclarations());
  }

  // If we've reached the bottom of the inheritance hierarchy, we're done.
  if (!classDecl->hasSuperclass())
    return nullptr;

  // Determine whether we are (still) inheriting initializers.
  if (!classDecl->inheritsSuperclassInitializers())
    inheritingInits = false;

  if (isa<ConstructorDecl>(method) && !inheritingInits)
    return nullptr;

  return lookupOverriddenObjCMethod(classDecl->getSuperclassDecl(), method,
                                   inheritingInits);
}

bool swift::diagnoseUnintendedObjCMethodOverrides(SourceFile &sf) {
  auto &Ctx = sf.getASTContext();
  DiagnosticStateRAII diagState(Ctx.Diags);
  auto &methods = sf.ObjCMethodList;

  // If no Objective-C methods were defined in this file, we're done.
  if (methods.empty())
    return false;

  // Sort the methods by declaration order.
  std::sort(methods.begin(), methods.end(), OrderDeclarations());

  // For each Objective-C method declared in this file, check whether
  // it overrides something in one of its superclasses. We
  // intentionally don't respect access control here, since everything
  // is visible to the Objective-C runtime.
  bool diagnosedAny = false;
  for (auto method : methods) {
    // If the method has an @objc override, we don't need to do any
    // more checking.
    if (auto overridden = method->getOverriddenDecl()) {
      if (overridden->isObjC())
        continue;
    }

    // Skip deinitializers.
    if (isa<DestructorDecl>(method))
      continue;

    // Skip invalid declarations.
    if (method->isInvalid())
      continue;

    // Skip declarations with an invalid 'override' attribute on them.
    if (auto attr = method->getAttrs().getAttribute<OverrideAttr>(true)) {
      if (attr->isInvalid())
        continue;
    }

    auto classDecl = method->getDeclContext()->getSelfClassDecl();
    if (!classDecl)
      continue; // error-recovery path, only

    if (!classDecl->hasSuperclass())
      continue;

    // Look for a method that we have overridden in one of our superclasses by
    // virtue of having the same selector.
    // Note: This should be treated as a lookup for intra-module dependency
    // purposes, but a subclass already depends on its superclasses and any
    // extensions for many other reasons.
    auto *overriddenMethod =
        lookupOverriddenObjCMethod(classDecl->getSuperclassDecl(), method);
    if (!overriddenMethod)
      continue;

    // Ignore stub implementations.
    if (auto overriddenCtor = dyn_cast<ConstructorDecl>(overriddenMethod)) {
      if (overriddenCtor->hasStubImplementation())
        continue;
    }

    // Require the declaration to actually come from a class. Selectors that
    // come from protocol requirements are not actually overrides.
    if (!overriddenMethod->getDeclContext()->getSelfClassDecl())
      continue;

    // Diagnose the override.
    auto methodDiagInfo = getObjCMethodDiagInfo(method);
    auto overriddenDiagInfo = getObjCMethodDiagInfo(overriddenMethod);

    Ctx.Diags.diagnose(
        method, diag::objc_override_other, methodDiagInfo.first,
        methodDiagInfo.second, overriddenDiagInfo.first,
        overriddenDiagInfo.second, method->getObjCSelector(),
        overriddenMethod->getDeclContext()->getDeclaredInterfaceType());

    const ValueDecl *overriddenDecl = overriddenMethod;
    if (overriddenMethod->isImplicit())
      if (auto accessor = dyn_cast<AccessorDecl>(overriddenMethod))
        overriddenDecl = accessor->getStorage();

    Ctx.Diags.diagnose(overriddenDecl, diag::objc_declared_here,
                       overriddenDiagInfo.first, overriddenDiagInfo.second);

    diagnosedAny = true;
  }

  return diagnosedAny;
}

static ObjCAttr *getObjCAttrIfFromAccessNote(ValueDecl *VD) {
  if (auto objc = VD->getAttrs().getAttribute<ObjCAttr>())
    if (objc->getAddedByAccessNote())
      return objc;

  if (auto accessor = dyn_cast<AccessorDecl>(VD))
    return getObjCAttrIfFromAccessNote(accessor->getStorage());

  return nullptr;
}

static bool hasCustomObjCName(AbstractFunctionDecl *afd) {
  if (auto objc = afd->getAttrs().getAttribute<ObjCAttr>())
    return objc->hasName();
  return false;
}

/// Retrieve the methods involved in a specific Objective-C selector
/// conflict. The list will be sorted so that the first method is the "best" one
/// and the others can be diagnosed as conflicts with that one.
static TinyPtrVector<AbstractFunctionDecl *>
getObjCMethodConflictDecls(const SourceFile::ObjCMethodConflict &conflict) {
  // Look up all methods involved in the conflict.
  auto methods = conflict.typeDecl->lookupDirect(conflict.selector,
                                                 conflict.isInstanceMethod);

  // Find async alternatives for each.
  llvm::SmallDenseMap<AbstractFunctionDecl *, AbstractFunctionDecl *>
    asyncAlternatives;
  for (auto method : methods) {
    if (isa<ProtocolDecl>(method->getDeclContext())) {
      if (auto alt = method->getAsyncAlternative())
        asyncAlternatives[method] = alt;
    }
  }

  // Erase any invalid or stub declarations. We don't want to complain about
  // them, because we might already have complained about redeclarations
  // based on Swift matching.
  llvm::erase_if(methods,
                 [&asyncAlternatives](AbstractFunctionDecl *afd) -> bool {
    if (afd->isInvalid())
      return true;

    // If there is an async alternative, remove this entry.
    if (asyncAlternatives.count(afd))
      return true;

    if (auto ad = dyn_cast<AccessorDecl>(afd))
      return ad->getStorage()->isInvalid();

    if (auto *ctor = dyn_cast<ConstructorDecl>(afd)) {
      if (ctor->hasStubImplementation())
        return true;
    }

    return false;
  });

  // Sort the conflicting methods from the "strongest" claim to the "weakest".
  // This puts the "best" method at methods.front() so that others will be
  // diagnosed as conflicts with that one, and it helps ensure that individual
  // methods in a conflict set are diagnosed in a deterministic order.
  llvm::stable_sort(methods,
                    [](AbstractFunctionDecl *a, AbstractFunctionDecl *b) {
    #define RULE(aCriterion, bCriterion) do { \
      bool _aCriterion = (aCriterion), _bCriterion = (bCriterion); \
      if (!_aCriterion && _bCriterion) \
        return false; \
      if (_aCriterion && !_bCriterion) \
        return true; \
    } while (0)

    // Is one of these from Objective-C and the other from Swift?
    // NOTE: Inserting another rule above this will break the hasClangNode()
    // filtering below.
    RULE(a->hasClangNode(),
         b->hasClangNode());

    // Is one of these async and the other not?
    RULE(a->hasAsync(),
         b->hasAsync());

    // Is one of these explicit and the other from an access note?
    RULE(!getObjCAttrIfFromAccessNote(a),
         !getObjCAttrIfFromAccessNote(b));

    // Is one of these from the main decl and the other an extension?
    RULE(!isa<ExtensionDecl>(a->getDeclContext()),
         !isa<ExtensionDecl>(b->getDeclContext()));

    // Does one of these use plain @objc and the other @objc(selector)?
    RULE(!hasCustomObjCName(a),
         !hasCustomObjCName(b));

    // Neither has a "stronger" claim, so just try to put them in some sort of
    // consistent order.
    OrderDeclarations ordering;
    return ordering(a, b);

    #undef RULE
  });

  // If the best method is imported from ObjC, eliminate any other imported ObjC
  // methods. Selector conflicts between imported ObjC methods are spurious;
  // they're just the same ObjC method being imported under different names with
  // different ImportNameVersions.
  if (!methods.empty() && methods.front()->hasClangNode())
    llvm::erase_if(methods, [&](AbstractFunctionDecl *afd) {
      return afd != methods.front() && afd->hasClangNode();
    });

  return methods;
}

bool swift::diagnoseObjCMethodConflicts(SourceFile &sf) {
  // If there were no conflicts, we're done.
  if (sf.ObjCMethodConflicts.empty())
    return false;

  auto &Ctx = sf.getASTContext();
  DiagnosticStateRAII diagState(Ctx.Diags);

  // Build a list of all the conflicts and the methods involved in them.
  using ConflictSet = std::pair<SourceFile::ObjCMethodConflict,
                                TinyPtrVector<AbstractFunctionDecl *>>;
  llvm::SmallVector<ConflictSet, 4> conflictSets;
  for (auto conflict : sf.ObjCMethodConflicts) {
    auto methods = getObjCMethodConflictDecls(conflict);
    if (methods.size() < 2)
      continue;
    conflictSets.emplace_back(conflict, methods);
  }

  // Sort the set of conflicts so the different conflict sets are diagnosed in
  // the same order. We use the first conflicting declaration in each set to
  // perform the sort.
  llvm::stable_sort(conflictSets,
                    [](const ConflictSet &lhs, const ConflictSet &rhs) {
                      OrderDeclarations ordering;
                      return ordering(lhs.second[1], rhs.second[1]);
                    });

  // Diagnose each conflict.
  bool anyConflicts = false;
  for (const auto &conflictSet : conflictSets) {
    // Diagnose the conflict.
    anyConflicts = true;
    
    const auto &conflict = conflictSet.first;
    const auto &methods = conflictSet.second;

    ArrayRef<AbstractFunctionDecl *> methodsRef(methods);
    auto originalMethod = methods.front();
    auto origDiagInfo = getObjCMethodDiagInfo(originalMethod);
    bool originalIsImportedAsync = originalMethod->hasClangNode() &&
                                     originalMethod->hasAsync();

    auto conflictingMethods = methodsRef.slice(1);
    for (auto conflictingDecl : conflictingMethods) {
      auto diagInfo = getObjCMethodDiagInfo(conflictingDecl);

      const ValueDecl *originalDecl = originalMethod;
      if (originalMethod->isImplicit())
        if (auto accessor = dyn_cast<AccessorDecl>(originalMethod))
          originalDecl = accessor->getStorage();

      // In Swift 5.7, we discovered cases which inadvertently bypassed selector
      // conflict checking and have to be diagnosed as warnings in Swift 5:

      // * Selectors for imported methods with async variants.
      bool breakingInSwift5 = originalIsImportedAsync;
      
      // * Protocol requirements
      if (!isa<ClassDecl>(conflict.typeDecl))
        breakingInSwift5 = true;

      bool redeclSame = (diagInfo == origDiagInfo);
      auto diag = Ctx.Diags.diagnose(conflictingDecl,
                                     redeclSame ? diag::objc_redecl_same
                                                : diag::objc_redecl,
                                     diagInfo.first, diagInfo.second,
                                     origDiagInfo.first, origDiagInfo.second,
                                     conflict.selector);
      diag.warnUntilSwiftVersionIf(breakingInSwift5, 6);

      auto objcAttr = getObjCAttrIfFromAccessNote(conflictingDecl);
      swift::softenIfAccessNote(conflictingDecl, objcAttr, diag);
      if (objcAttr)
        objcAttr->setInvalid();

      if (redeclSame)
        Ctx.Diags.diagnose(originalDecl, diag::invalid_redecl_prev,
                           originalDecl->getBaseName());
      else
        Ctx.Diags.diagnose(originalDecl, diag::objc_declared_here,
                           origDiagInfo.first, origDiagInfo.second);
    }
  }

  return anyConflicts;
}

/// Retrieve the source location associated with this declaration
/// context.
static SourceLoc getDeclContextLoc(DeclContext *dc) {
  if (auto ext = dyn_cast<ExtensionDecl>(dc))
    return ext->getLoc();

  return cast<NominalTypeDecl>(dc)->getLoc();
}

bool swift::diagnoseObjCUnsatisfiedOptReqConflicts(SourceFile &sf) {
  // If there are no unsatisfied, optional @objc requirements, we're done.
  if (sf.ObjCUnsatisfiedOptReqs.empty())
    return false;

  auto &Ctx = sf.getASTContext();
  DiagnosticStateRAII diagState(Ctx.Diags);

  // Sort the set of local unsatisfied requirements, so we get a
  // deterministic order for diagnostics.
  auto &localReqs = sf.ObjCUnsatisfiedOptReqs;
  std::sort(localReqs.begin(), localReqs.end(),
            [&](const SourceFile::ObjCUnsatisfiedOptReq &lhs,
                const SourceFile::ObjCUnsatisfiedOptReq &rhs) -> bool {
              return Ctx.SourceMgr.isBeforeInBuffer(getDeclContextLoc(lhs.first),
                                                    getDeclContextLoc(rhs.first));
            });

  // Check each of the unsatisfied optional requirements.
  bool anyDiagnosed = false;
  for (const auto &unsatisfied : localReqs) {
    // Check whether there is a conflict here.
    ClassDecl *classDecl = unsatisfied.first->getSelfClassDecl();
    auto req = unsatisfied.second;
    auto selector = req->getObjCSelector();
    bool isInstanceMethod = req->isInstanceMember();
    // FIXME: Also look in superclasses?
    auto conflicts = classDecl->lookupDirect(selector, isInstanceMethod);
    if (conflicts.empty())
      continue;

    auto bestConflict = conflicts[0];
    for (auto conflict : conflicts) {
      if (conflict->getName().isCompoundName() &&
          conflict->getName().getArgumentNames().size() ==
            req->getName().getArgumentNames().size()) {
        bestConflict = conflict;
        break;
      }
    }
    assert(req != bestConflict && "requirement conflicts with itself?");

    // Diagnose the conflict.
    auto reqDiagInfo = getObjCMethodDiagInfo(unsatisfied.second);
    auto conflictDiagInfo = getObjCMethodDiagInfo(bestConflict);
    auto protocolName
      = cast<ProtocolDecl>(req->getDeclContext())->getName();
    Ctx.Diags.diagnose(bestConflict,
                       diag::objc_optional_requirement_conflict,
                       conflictDiagInfo.first,
                       conflictDiagInfo.second,
                       reqDiagInfo.first,
                       reqDiagInfo.second,
                       selector,
                       protocolName);

    // Fix the name of the witness, if we can.
    if (req->getName() != bestConflict->getName() &&
        req->getKind() == bestConflict->getKind() &&
        isa<AccessorDecl>(req) == isa<AccessorDecl>(bestConflict)) {
      // They're of the same kind: fix the name.
      unsigned kind;
      if (isa<ConstructorDecl>(req))
        kind = 1;
      else if (auto accessor = dyn_cast<AccessorDecl>(req))
        kind = isa<SubscriptDecl>(accessor->getStorage()) ? 3 : 2;
      else if (isa<FuncDecl>(req))
        kind = 0;
      else {
        llvm_unreachable("unhandled @objc declaration kind");
      }

      auto diag = Ctx.Diags.diagnose(bestConflict,
                                     diag::objc_optional_requirement_swift_rename,
                                     kind, req->getName());

      // Fix the Swift name.
      fixDeclarationName(diag, bestConflict, req->getName());

      // Fix the '@objc' attribute, if needed.
      if (!bestConflict->canInferObjCFromRequirement(req))
        fixDeclarationObjCName(diag, bestConflict,
                               bestConflict->getObjCRuntimeName(),
                               req->getObjCRuntimeName(),
                               /*ignoreImpliedName=*/true);
    }

    // @nonobjc will silence this warning.
    bool hasExplicitObjCAttribute = false;
    if (auto objcAttr = bestConflict->getAttrs().getAttribute<ObjCAttr>())
      hasExplicitObjCAttribute = !objcAttr->isImplicit();
    if (!hasExplicitObjCAttribute)
      Ctx.Diags.diagnose(bestConflict, diag::req_near_match_nonobjc, true)
        .fixItInsert(
          bestConflict->getAttributeInsertionLoc(/*forModifier=*/false),
          "@nonobjc ");

    Ctx.Diags.diagnose(getDeclContextLoc(unsatisfied.first),
                       diag::protocol_conformance_here,
                       true,
                       classDecl->getName(),
                      protocolName);
    Ctx.Diags.diagnose(req, diag::kind_declname_declared_here,
                       DescriptiveDeclKind::Requirement, reqDiagInfo.second);

    anyDiagnosed = true;
  }

  return anyDiagnosed;
}

void TypeChecker::checkObjCImplementation(ExtensionDecl *ED) {
  if (!ED->getImplementedObjCDecl())
    return;

  evaluateOrDefault(ED->getASTContext().evaluator,
                    TypeCheckObjCImplementationRequest{ED},
                    evaluator::SideEffect());
}

static Optional<Located<StaticSpellingKind>>
getLocatedStaticSpelling(ValueDecl *VD) {
  using Ret = Optional<Located<StaticSpellingKind>>;

  if (auto FD = dyn_cast<FuncDecl>(VD)) {
    return Ret({ FD->getCorrectStaticSpelling(), FD->getStaticLoc() });
  }
  else if (auto ASD = dyn_cast<AbstractStorageDecl>(VD)) {
    if (auto SD = dyn_cast<SubscriptDecl>(ASD)) {
      return Ret({ SD->getCorrectStaticSpelling(), SD->getStaticLoc() });
    }
    else if (auto VD = dyn_cast<VarDecl>(ASD)) {
      if (auto PBD = VD->getParentPatternBinding())
        return Ret({ PBD->getCorrectStaticSpelling(), PBD->getStaticLoc() });
    }
    else {
      llvm_unreachable("unknown AbstractStorageDecl");
    }
  }
  return None;
}

static Optional<StaticSpellingKind> getStaticSpelling(ValueDecl *VD) {
  if (auto locSpelling = getLocatedStaticSpelling(VD))
    return locSpelling->Item;
  return None;
}

static void
fixDeclarationStaticSpelling(InFlightDiagnostic &diag, ValueDecl *VD,
                             Optional<StaticSpellingKind> newSpelling) {
  auto spelling = getLocatedStaticSpelling(VD);
  if (!newSpelling || !spelling)
    return;

  // If we're changing to `static`, remove explicit `final` keyword.
  if (newSpelling == StaticSpellingKind::KeywordStatic)
    if (auto finalAttr = VD->getAttrs().getAttribute<FinalAttr>())
      diag.fixItRemove(finalAttr->getRange());

  auto spellingLoc = spelling->Loc;

  switch (spelling->Item) {
  case StaticSpellingKind::None:
    switch (*newSpelling) {
    case StaticSpellingKind::None:
      // Do nothing
      return;

    case StaticSpellingKind::KeywordClass:
      diag.fixItInsert(VD->getAttributeInsertionLoc(/*forModifier=*/true),
                       "class ");
      return;

    case StaticSpellingKind::KeywordStatic:
      diag.fixItInsert(VD->getAttributeInsertionLoc(/*forModifier=*/true),
                       "static ");
      return;
    }

  case StaticSpellingKind::KeywordClass:
    switch (*newSpelling) {
    case StaticSpellingKind::None:
      diag.fixItRemove(spellingLoc);
      return;

    case StaticSpellingKind::KeywordClass:
      // Do nothing
      return;

    case StaticSpellingKind::KeywordStatic:
      diag.fixItReplace(spellingLoc, "static");
      return;
    }

  case StaticSpellingKind::KeywordStatic:
    switch (*newSpelling) {
    case StaticSpellingKind::None:
      diag.fixItReplace(spellingLoc, "final");
      return;

    case StaticSpellingKind::KeywordClass:
      diag.fixItReplace(spellingLoc, "class");
      return;

    case StaticSpellingKind::KeywordStatic:
      // Do nothing
      return;
    }
  }

  llvm_unreachable("unknown StaticSpellingKind");
}

namespace {
class ObjCImplementationChecker {
  DiagnosticEngine &diags;

  template<typename ...ArgTypes>
  InFlightDiagnostic diagnose(ArgTypes &&...Args) {
    auto diag = diags.diagnose(std::forward<ArgTypes>(Args)...);
    return diag;
  }

  SmallSetVector<ValueDecl *, 16> unmatchedRequirements;

  /// Candidates with their explicit ObjC names, if any.
  llvm::SmallDenseMap<ValueDecl *, ObjCSelector, 16> unmatchedCandidates;

public:
  ObjCImplementationChecker(ExtensionDecl *ext)
      : diags(ext->getASTContext().Diags)
  {
    assert(!ext->hasClangNode() && "passed interface, not impl, to checker");

    // Did we actually match this extension to an interface? (In invalid code,
    // we might not have.)
    auto interfaceDecl = ext->getImplementedObjCDecl();
    if (!interfaceDecl)
      return;

    // Add the @_objcImplementation extension's members as candidates.
    addCandidates(ext);

    // Add its interface's members as requirements.
    auto interface = cast<IterableDeclContext>(interfaceDecl);
    addRequirements(interface);
  }

private:
  static bool hasAsync(ValueDecl *member) {
    if (!member)
      return false;

    if (auto func = dyn_cast<AbstractFunctionDecl>(member))
      return func->hasAsync();

    if (auto storage = dyn_cast<AbstractStorageDecl>(member))
      return hasAsync(storage->getEffectfulGetAccessor());

    return false;
  }

  static ValueDecl *getAsyncAlternative(ValueDecl *req) {
    if (auto func = dyn_cast<AbstractFunctionDecl>(req)) {
      auto asyncFunc = func->getAsyncAlternative();

      if (auto asyncAccessor = dyn_cast<AccessorDecl>(asyncFunc))
        return asyncAccessor->getStorage();

      return asyncFunc;
    }

    return nullptr;
  }

  void addRequirements(IterableDeclContext *idc) {
    assert(idc->getDecl()->hasClangNode());
    for (Decl *_member : idc->getMembers()) {
      // Skip accessors; we'll match their storage instead. Also skip overrides;
      // the override checker handles those.
      auto member = dyn_cast<ValueDecl>(_member);
      if (!member || isa<AccessorDecl>(member) || member->getOverriddenDecl())
        continue;

      // Skip alternate Swift names for other language modes.
      if (member->getAttrs().isUnavailable(member->getASTContext()))
        continue;

      // Skip async versions of members. We'll match against the completion
      // handler versions, hopping over to `getAsyncAlternative()` if needed.
      if (hasAsync(member))
        continue;

      auto inserted = unmatchedRequirements.insert(member);
      assert(inserted && "objc interface member added twice?");
    }
  }

  void addCandidates(ExtensionDecl *ext) {
    assert(ext->isObjCImplementation());
    for (Decl *_member : ext->getMembers()) {
      // Skip accessors; we'll match their storage instead.
      auto member = dyn_cast<ValueDecl>(_member);
      if (!member || isa<AccessorDecl>(member) || isa<DestructorDecl>(member))
        continue;

      // Skip non-member implementations.
      // FIXME: Should we consider them if they were only rejected for privacy?
      if (!member->isObjCMemberImplementation())
        continue;

      // `getExplicitObjCName()` is O(N) and would otherwise be used repeatedly
      // in `matchRequirementsAtThreshold()`, so just precompute it.
      auto inserted =
          unmatchedCandidates.insert({ member, getExplicitObjCName(member) });
      assert(inserted.second && "member implementation added twice?");

    }
  }

  static ObjCSelector getExplicitObjCName(ValueDecl *VD) {
    if (auto attr = VD->getAttrs().getAttribute<ObjCAttr>())
      return attr->getName().getValueOr(ObjCSelector());
    return ObjCSelector();
  }

public:
  void matchRequirements() {
    // Try matching requirements with decreasing stringency.
    // By working in several rounds like this, we ensure that all requirements
    // matched by an explicit @objc(<selector>) are removed from consideration
    // before we try Swift name matches, and likewise for Swift name matches and
    // "wrong" matches.
    matchRequirementsAtThreshold(MatchOutcome::MatchWithExplicitObjCName);
    matchRequirementsAtThreshold(MatchOutcome::Match);
    matchRequirementsAtThreshold(MatchOutcome::AnyRelationship);
  }

private:
  /// Describes how closely a requirement and candidate match each other.
  ///
  /// The cases in this enum are arranged from worst match to best match, so
  /// the most severe mismatches are given lower values. This order should be
  /// the same as the order in which the problems are detected by
  /// \c matches() .
  enum class MatchOutcome : uint8_t {
    /// There is absolutely no reason to think these two members are connected
    /// to one another.
    NoRelationship,

    WrongExplicitObjCName,
    WrongSwiftName,
    WrongImplicitObjCName,
    WrongStaticness,
    WrongCategory,
    WrongDeclKind,
    WrongType,
    WrongWritability,

    Match,
    MatchWithExplicitObjCName,

    AnyRelationship = WrongExplicitObjCName,
  };

  /// A list of matches which all have the same outcome. The list only includes
  /// matches with the highest-ranked outcome ever inserted into it.
  struct BestMatchList {
    MatchOutcome currentOutcome;
    TinyPtrVector<ValueDecl *> matches;

    BestMatchList(MatchOutcome minimumOutcome = MatchOutcome::AnyRelationship)
        : currentOutcome(minimumOutcome), matches()
    {}

    void insert(ValueDecl *newMatch, MatchOutcome newOutcome) {
      // Does this match meet the bar to be included?
      if (currentOutcome > newOutcome)
        return;

      // Will this match raise the bar? If so, clear the old matches.
      if (currentOutcome < newOutcome) {
        currentOutcome = newOutcome;
        matches.clear();
      }

      if (!llvm::is_contained(matches, newMatch))
        matches.push_back(newMatch);
    }
  };

  void matchRequirementsAtThreshold(MatchOutcome threshold) {
    SmallString<32> scratch;

    // We want to diagnose both (a) many-requirements-for-one-candidate and
    // (b) many-candidates-for-one-requirement situations. We also want to
    // remove matched candidates and requirements from being considered in
    // future calls to this function, but we don't want them to change during
    // this call. So we perform three passes:
    //
    // 1. Loop through `this->candidates`. Find matching requirements from
    //    `this->requirements` and either add them to `matchesByRequirement`
    //    (a data structure where each requirement is paired with a list of
    //    candidates it matched), or diagnose (a).
    //
    // 2. Loop through `matchesByRequirement`, looking at the same declarations
    //    but with each requirement paired with a complete list of candidates
    //    it matched. Either diagnose (b), or diagnose any issues with the
    //    single match we found.
    //
    // 3. Remove all requirements and candidates matched by 1 and 2 (including
    //    ambiguous ones), so they are no longer matched when we try again at a
    //    lower threshold or diagnosed as unmatched.
    //
    // (We evaluate by candidate first and by requirement second because the
    // diagnostics for ambiguous candidates are better than the ones for
    // ambiguous requirements.)

    // Maps candidates to the list of requirements they matched, retaining only
    // the best matches.
    llvm::DenseMap<ValueDecl *, BestMatchList> matchesByRequirement;

    // Requirements and candidates that have been matched (even ambiguously) and
    // should be removed from our unmatched lists.
    SmallSetVector<ValueDecl *, 16> requirementsToRemove;
    SmallSetVector<ValueDecl *, 16> candidatesToRemove;

    // First, loop through unsatisfied candidates and try the requirements.
    for (const auto &pair : unmatchedCandidates) {
      auto &cand = pair.first;
      auto &candExplicitObjCName = pair.second;

      PrettyStackTraceDecl t1(
            "checking @objcImplementation matches to candidate", cand);
      BestMatchList matchedRequirements{threshold};

      for (ValueDecl *req : unmatchedRequirements) {
        PrettyStackTraceDecl t2("trying to match header requirement", req);

        auto outcome = matches(req, cand, candExplicitObjCName);
        matchedRequirements.insert(req, outcome);
      }

      if (matchedRequirements.matches.empty())
        continue;

      // We matched these requirements at least once, so we're definitely
      // removing them.
      requirementsToRemove.set_union(matchedRequirements.matches);

      if (matchedRequirements.matches.size() == 1) {
        // Note that this is BestMatchList::insert(), so it'll only keep the
        // matches with the best outcomes.
        matchesByRequirement[matchedRequirements.matches.front()]
          .insert(cand, matchedRequirements.currentOutcome);
        continue;
      }

      // Ambiguous match (many requirements match one candidate)
      cand->diagnose(diag::objc_implementation_multiple_matching_requirements,
                     cand->getDescriptiveKind(), cand);

      bool shouldOfferFix = !candExplicitObjCName;
      for (auto req : matchedRequirements.matches) {
        auto diag =
            cand->diagnose(diag::objc_implementation_one_matched_requirement,
                           req->getDescriptiveKind(), req,
                           *req->getObjCRuntimeName(), shouldOfferFix,
                           req->getObjCRuntimeName()->getString(scratch));
        if (shouldOfferFix) {
          fixDeclarationObjCName(diag, cand, cand->getObjCRuntimeName(),
                                 req->getObjCRuntimeName(),
                                 /*ignoreImpliedName=*/true);
        }
      }

      // Schedule removal of this candidate now (since it won't be seen in the
      // next loop).
      candidatesToRemove.insert(cand);
    }

    // Now loop through requirements looking at the best candidate matches for
    // them.
    for (auto &pair : matchesByRequirement) {
      auto req = pair.first;

      // `cands` is a BestMatchList, so this is only the candidates with the
      // best outcome. If several candidates matched the requirement but some
      // had better outcomes than others, this will have already dropped the
      // worse-outcome ones.
      auto &cands = pair.second;

      // Schedule the removal of these candidates.
      candidatesToRemove.set_union(cands.matches);

      assert(!cands.matches.empty());
      if (cands.matches.size() == 1) {
        auto cand = cands.matches.front();
        diagnoseOutcome(cands.currentOutcome, req, cand,
                        unmatchedCandidates[cand]);
        continue;
      }

      // Ambiguous match (one requirement matches many candidates)
      auto reqIDC =
          cast<IterableDeclContext>(req->getDeclContext()->getAsDecl());
      auto ext =
          cast<ExtensionDecl>(reqIDC->getImplementationContext());
      ext->diagnose(diag::objc_implementation_multiple_matching_candidates,
                    req->getDescriptiveKind(), req,
                    *req->getObjCRuntimeName());

      for (auto cand : cands.matches) {
        bool shouldOfferFix = !unmatchedCandidates[cand];
        auto diag =
            cand->diagnose(diag::objc_implementation_candidate_impl_here,
                           cand->getDescriptiveKind(), cand, shouldOfferFix,
                           req->getObjCRuntimeName()->getString(scratch));

        if (shouldOfferFix) {
          fixDeclarationObjCName(diag, cand, cand->getObjCRuntimeName(),
                                 req->getObjCRuntimeName(),
                                 /*ignoreImpliedName=*/true);
        }
      }

      req->diagnose(diag::objc_implementation_requirement_here,
                    req->getDescriptiveKind(), req);
    }

    // Remove matched candidates and requirements from the unmatched lists.
    unmatchedRequirements.set_subtract(requirementsToRemove);
    for (auto cand : candidatesToRemove)
      unmatchedCandidates.erase(cand);
  }

  static bool areSwiftNamesEqual(DeclName lhs, DeclName rhs) {
    // Conflate `foo()` and `foo`. This allows us to diagnose
    // method-vs.-property mistakes more nicely.

    if (lhs.isCompoundName() && lhs.getArgumentNames().empty())
      lhs = lhs.getBaseName();

    if (rhs.isCompoundName() && rhs.getArgumentNames().empty())
      rhs = rhs.getBaseName();

    return lhs == rhs;
  }

  static bool matchParamTypes(Type reqTy, Type implTy, ValueDecl *implDecl) {
    TypeMatchOptions matchOpts = {};

    // Try a plain type match.
    if (implTy->matchesParameter(reqTy, matchOpts))
      return true;

    // If the implementation type is IUO, try unwrapping it.
    if (auto unwrappedImplTy = implTy->getOptionalObjectType())
      return implDecl->isImplicitlyUnwrappedOptional()
                && unwrappedImplTy->matchesParameter(reqTy, matchOpts);

    return false;
  }

  static bool matchTypes(Type reqTy, Type implTy, ValueDecl *implDecl) {
    TypeMatchOptions matchOpts = {};

    // Try a plain type match.
    if (reqTy->matches(implTy, matchOpts))
      return true;

    // If the implementation type is optional, try unwrapping it.
    if (auto unwrappedImplTy = implTy->getOptionalObjectType())
      return implDecl->isImplicitlyUnwrappedOptional()
                  && reqTy->matches(unwrappedImplTy, matchOpts);

    // Apply these rules to the result type and parameters if it's a function
    // type.
    if (auto funcReqTy = reqTy->getAs<AnyFunctionType>())
      if (auto funcImplTy = implTy->getAs<AnyFunctionType>())
        return funcReqTy->matchesFunctionType(funcImplTy, matchOpts,
                                              [=]() -> bool {
          auto reqParams = funcReqTy->getParams();
          auto implParams = funcImplTy->getParams();
          if (reqParams.size() != implParams.size())
            return false;

          auto implParamList =
              cast<AbstractFunctionDecl>(implDecl)->getParameters();

          for (auto i : indices(reqParams)) {
            const auto &reqParam = reqParams[i];
            const auto &implParam = implParams[i];
            ParamDecl *implParamDecl = implParamList->get(i);

            if (!matchParamTypes(reqParam.getOldType(), implParam.getOldType(),
                                 implParamDecl))
              return false;
          }

          return matchTypes(funcReqTy->getResult(), funcImplTy->getResult(),
                            implDecl);
        });

    return false;
  }

  static Type getMemberType(ValueDecl *decl) {
    if (isa<AbstractFunctionDecl>(decl))
      // Strip off the uncurried `self` parameter.
      return decl->getInterfaceType()->getAs<AnyFunctionType>()->getResult();
    return decl->getInterfaceType();
  }

  MatchOutcome matchesImpl(ValueDecl *req, ValueDecl *cand,
                           ObjCSelector explicitObjCName) const {
    bool hasObjCNameMatch =
        req->getObjCRuntimeName() == cand->getObjCRuntimeName();
    bool hasSwiftNameMatch = areSwiftNamesEqual(req->getName(), cand->getName());

    // If neither the ObjC nor Swift names match, there's absolutely no reason
    // to think these two methods are related.
    if (!hasObjCNameMatch && !hasSwiftNameMatch)
      return MatchOutcome::NoRelationship;

    // There's at least some reason to treat these as matches.

    if (explicitObjCName
          && req->getObjCRuntimeName() != explicitObjCName)
      return MatchOutcome::WrongExplicitObjCName;

    if (!hasSwiftNameMatch)
      return MatchOutcome::WrongSwiftName;

    if (!hasObjCNameMatch)
      return MatchOutcome::WrongImplicitObjCName;

    if (req->isInstanceMember() != cand->isInstanceMember())
      return MatchOutcome::WrongStaticness;

    if (cand->getDeclContext()->getImplementedObjCContext()
          != req->getDeclContext())
      return MatchOutcome::WrongCategory;

    if (cand->getKind() != req->getKind())
      return MatchOutcome::WrongDeclKind;

    if (!matchTypes(getMemberType(req), getMemberType(cand), cand))
      return MatchOutcome::WrongType;

    if (auto reqVar = dyn_cast<AbstractStorageDecl>(req))
      if (reqVar->isSettable(nullptr) &&
            !cast<AbstractStorageDecl>(cand)->isSettable(nullptr))
        return MatchOutcome::WrongWritability;

    // If we got here, everything matched. But at what quality?
    if (explicitObjCName)
      return MatchOutcome::MatchWithExplicitObjCName;

    return MatchOutcome::Match;
  }

  MatchOutcome matches(ValueDecl *req, ValueDecl *cand,
                       ObjCSelector explicitObjCName) const {
    // If the candidate we're considering is async, see if the requirement has
    // an async alternate and try to match against that instead.
    if (hasAsync(cand))
      if (auto asyncAltReq = getAsyncAlternative(req))
        return matchesImpl(asyncAltReq, cand, explicitObjCName);

    return matchesImpl(req, cand, explicitObjCName);
  }

  void diagnoseOutcome(MatchOutcome outcome, ValueDecl *req, ValueDecl *cand,
                       ObjCSelector explicitObjCName) {
    auto reqObjCName = *req->getObjCRuntimeName();

    switch (outcome) {
    case MatchOutcome::NoRelationship:
      llvm::report_fatal_error("trying to diagnoseOutcome a NoRelationship");
      return;

    case MatchOutcome::Match:
    case MatchOutcome::MatchWithExplicitObjCName:
      // Successful outcomes!
      return;

    case MatchOutcome::WrongImplicitObjCName:
    case MatchOutcome::WrongExplicitObjCName: {
      auto diag = diagnose(cand, diag::objc_implementation_wrong_objc_name,
                           *cand->getObjCRuntimeName(),
                           cand->getDescriptiveKind(), cand, reqObjCName);
      fixDeclarationObjCName(diag, cand, explicitObjCName, reqObjCName);
      return;
    }

    case MatchOutcome::WrongSwiftName: {
      auto diag = diagnose(cand, diag::objc_implementation_wrong_swift_name,
                           reqObjCName, req->getDescriptiveKind(),
                           req->getName());
      fixDeclarationName(diag, cand, req->getName());
      if (!explicitObjCName) {
        // Changing the Swift name will probably change the implicitly-computed
        // ObjC name, so let's make that explicit.
        fixDeclarationObjCName(diag, cand, cand->getObjCRuntimeName(),
                               reqObjCName, /*ignoreImpliedName=*/true);
      }
      return;
    }

    case MatchOutcome::WrongStaticness: {
      auto diag = diagnose(cand,
                           diag::objc_implementation_class_or_instance_mismatch,
                           cand->getDescriptiveKind(), cand,
                           req->getDescriptiveKind());
      fixDeclarationStaticSpelling(diag, cand, getStaticSpelling(req));
      return;
    }

    case MatchOutcome::WrongCategory:
      diagnose(cand, diag::objc_implementation_wrong_category,
               cand->getDescriptiveKind(), cand,
               getCategoryName(req->getDeclContext()),
               getCategoryName(cand->getDeclContext()->
                                 getImplementedObjCContext()));
      return;

    case MatchOutcome::WrongDeclKind:
      diagnose(cand, diag::objc_implementation_wrong_decl_kind,
               cand->getDescriptiveKind(), cand, req->getDescriptiveKind());
      return;

    case MatchOutcome::WrongType:
      diagnose(cand, diag::objc_implementation_type_mismatch,
               cand->getDescriptiveKind(), cand,
               getMemberType(cand), getMemberType(req));
      return;

    case MatchOutcome::WrongWritability:
      diagnose(cand, diag::objc_implementation_must_be_settable,
               cand->getDescriptiveKind(), cand, req->getDescriptiveKind());
      return;
    }

    llvm_unreachable("Unknown MatchOutcome");
  }

  static Identifier getCategoryName(DeclContext *dc) {
    if (auto ED = dyn_cast<ExtensionDecl>(dc))
      return ED->getObjCCategoryName();
    return Identifier();
  }

  /// Is this member an `@optional` ObjC protocol requirement?
  static bool isOptionalObjCProtocolRequirement(ValueDecl *vd) {
    if (auto clangDecl = vd->getClangDecl()) {
      if (auto method = dyn_cast<clang::ObjCMethodDecl>(clangDecl))
        return method->isOptional();
      if (auto property = dyn_cast<clang::ObjCPropertyDecl>(clangDecl))
        return property->isOptional();
    }

    return false;
  }

public:
  void diagnoseUnmatchedRequirements() {
    for (auto req : unmatchedRequirements) {
      // Ignore `@optional` protocol requirements.
      if (isOptionalObjCProtocolRequirement(req))
        continue;

      auto ext = cast<IterableDeclContext>(req->getDeclContext()->getAsDecl())
                        ->getImplementationContext();

      diagnose(ext->getDecl(), diag::objc_implementation_missing_impl,
               getCategoryName(req->getDeclContext()),
               req->getDescriptiveKind(), req);

      // FIXME: Should give fix-it to add stub implementation
    }
  }

  void diagnoseUnmatchedCandidates() {
    for (auto &pair : unmatchedCandidates) {
      auto cand = pair.first;

      diagnose(cand, diag::member_of_objc_implementation_not_objc_or_final,
               cand->getDescriptiveKind(), cand,
               cand->getDeclContext()->getSelfClassDecl());

      if (canBeRepresentedInObjC(cand))
        diagnose(cand, diag::fixit_add_private_for_objc_implementation,
                 cand->getDescriptiveKind())
            .fixItInsert(cand->getAttributeInsertionLoc(true), "private ");

      diagnose(cand, diag::fixit_add_final_for_objc_implementation,
               cand->getDescriptiveKind())
          .fixItInsert(cand->getAttributeInsertionLoc(true), "final ");
    }
  }
};
}

evaluator::SideEffect TypeCheckObjCImplementationRequest::
evaluate(Evaluator &evaluator, ExtensionDecl *ED) const {
  PrettyStackTraceDecl trace("checking member implementations of", ED);

  // FIXME: Because we check extension-by-extension, candidates and requirements
  // from different extensions are never compared, so we never get an
  // opportunity to emit `diag::objc_implementation_wrong_category`. We probably
  // need some kind of whole-module step where we compare all of the unmatched
  // candidates we considered to all unmatched requirements in the module, and
  // vice versa. The tricky bit is making sure we only diagnose for candidates
  // and requirements in our primary files!
  ObjCImplementationChecker checker(ED);

  checker.matchRequirements();
  checker.diagnoseUnmatchedCandidates();
  checker.diagnoseUnmatchedRequirements();

  return evaluator::SideEffect();
}
