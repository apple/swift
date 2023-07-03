//===--- TypeCheckType.cpp - Type Validation ------------------------------===//
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
// This file implements validation for Swift types, emitting semantic errors as
// appropriate and checking default initializer values.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "TypeCheckAvailability.h"
#include "TypeCheckConcurrency.h"
#include "TypeCheckProtocol.h"
#include "TypeCheckType.h"
#include "TypoCorrection.h"

#include "swift/AST/ASTDemangler.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsParse.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PackExpansionMatcher.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SILLayout.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeLoc.h"
#include "swift/AST/TypeResolutionStage.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/StringExtras.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/Parse/Lexer.h"
#include "swift/Sema/SILTypeResolutionContext.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclTemplate.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"

using namespace swift;

#define DEBUG_TYPE "TypeCheckType"

/// Type resolution.

TypeResolution
TypeResolution::forStructural(DeclContext *dc, TypeResolutionOptions options,
                              OpenUnboundGenericTypeFn unboundTyOpener,
                              HandlePlaceholderTypeReprFn placeholderHandler,
                              OpenPackElementFn packElementOpener) {
  return TypeResolution(dc, {}, TypeResolutionStage::Structural, options,
                        unboundTyOpener, placeholderHandler, packElementOpener);
}

TypeResolution
TypeResolution::forInterface(DeclContext *dc, TypeResolutionOptions options,
                             OpenUnboundGenericTypeFn unboundTyOpener,
                             HandlePlaceholderTypeReprFn placeholderHandler,
                             OpenPackElementFn packElementOpener) {
  return forInterface(dc, dc->getGenericSignatureOfContext(), options,
                      unboundTyOpener, placeholderHandler, packElementOpener);
}

TypeResolution
TypeResolution::forInterface(DeclContext *dc, GenericSignature genericSig,
                             TypeResolutionOptions options,
                             OpenUnboundGenericTypeFn unboundTyOpener,
                             HandlePlaceholderTypeReprFn placeholderHandler,
                             OpenPackElementFn packElementOpener) {
  return TypeResolution(dc, genericSig, TypeResolutionStage::Interface, options,
                        unboundTyOpener, placeholderHandler, packElementOpener);
}

TypeResolution TypeResolution::withOptions(TypeResolutionOptions opts) const {
  return TypeResolution(dc, genericSig, stage, opts, unboundTyOpener,
                        placeholderHandler, packElementOpener);
}

TypeResolution TypeResolution::withoutPackElementOpener() const {
  return TypeResolution(dc, genericSig, stage, options, unboundTyOpener,
                        placeholderHandler, {});
}

ASTContext &TypeResolution::getASTContext() const {
  return dc->getASTContext();
}

GenericSignature TypeResolution::getGenericSignature() const {
  assert(
      stage == TypeResolutionStage::Interface &&
      "Structural resolution shouldn't require generic signature computation");

  if (genericSig)
    return genericSig;

  return dc->getGenericSignatureOfContext();
}

// FIXME: It would be nice to have a 'DescriptiveTypeKind' abstraction instead.
static DescriptiveDeclKind describeDeclOfType(Type t) {
  if (!t) {
    return DescriptiveDeclKind::Type;
  }
  if (auto *NTD = t->getAnyNominal()) {
    return NTD->getDescriptiveKind();
  }
  return DescriptiveDeclKind::Type;
}

static unsigned getGenericRequirementKind(TypeResolutionOptions options) {
  switch (options.getBaseContext()) {
  case TypeResolverContext::GenericRequirement:
  case TypeResolverContext::SameTypeRequirement:
    return 0;

  case TypeResolverContext::GenericParameterInherited:
    return 1;

  case TypeResolverContext::AssociatedTypeInherited:
    return 2;

  case TypeResolverContext::None:
  case TypeResolverContext::Inherited:
  case TypeResolverContext::FunctionInput:
  case TypeResolverContext::PackElement:
  case TypeResolverContext::TupleElement:
  case TypeResolverContext::GenericArgument:
  case TypeResolverContext::ProtocolGenericArgument:
  case TypeResolverContext::ExtensionBinding:
  case TypeResolverContext::TypeAliasDecl:
  case TypeResolverContext::GenericTypeAliasDecl:
  case TypeResolverContext::ExistentialConstraint:
  case TypeResolverContext::MetatypeBase:
  case TypeResolverContext::InExpression:
  case TypeResolverContext::ExplicitCastExpr:
  case TypeResolverContext::ForEachStmt:
  case TypeResolverContext::PatternBindingDecl:
  case TypeResolverContext::EditorPlaceholderExpr:
  case TypeResolverContext::ClosureExpr:
  case TypeResolverContext::VariadicFunctionInput:
  case TypeResolverContext::InoutFunctionInput:
  case TypeResolverContext::FunctionResult:
  case TypeResolverContext::SubscriptDecl:
  case TypeResolverContext::EnumElementDecl:
  case TypeResolverContext::MacroDecl:
  case TypeResolverContext::EnumPatternPayload:
  case TypeResolverContext::ProtocolMetatypeBase:
  case TypeResolverContext::ImmediateOptionalTypeArgument:
  case TypeResolverContext::AbstractFunctionDecl:
  case TypeResolverContext::CustomAttr:
    break;
  }

  llvm_unreachable("Invalid type resolution context");
}

Type TypeResolution::resolveDependentMemberType(Type baseTy, DeclContext *DC,
                                                SourceRange baseRange,
                                                IdentTypeRepr *repr) const {
  // FIXME(ModQual): Reject qualified names immediately; they cannot be
  // dependent member types.
  Identifier refIdentifier = repr->getNameRef().getBaseIdentifier();
  ASTContext &ctx = DC->getASTContext();

  switch (stage) {
  case TypeResolutionStage::Structural:
    return DependentMemberType::get(baseTy, refIdentifier);

  case TypeResolutionStage::Interface:
    // Handled below.
    break;
  }

  assert(stage == TypeResolutionStage::Interface);
  auto genericSig = getGenericSignature();
  if (!genericSig)
    return ErrorType::get(baseTy);

  // Look for a nested type with the given name.
  if (auto nestedType = genericSig->lookupNestedType(baseTy, refIdentifier)) {
    if (options.isGenericRequirement()) {
      if (auto *protoDecl = nestedType->getDeclContext()->getExtendedProtocolDecl()) {
        if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
          unsigned kind = getGenericRequirementKind(options);
          ctx.Diags.diagnose(repr->getNameLoc(),
                             diag::protocol_extension_in_where_clause,
                             nestedType->getName(), protoDecl->getName(), kind);
          if (protoDecl->getLoc() && nestedType->getLoc()) {
            ctx.Diags.diagnose(nestedType->getLoc(),
                               diag::protocol_extension_in_where_clause_note,
                               nestedType->getName(), protoDecl->getName());
          }
        }

        return ErrorType::get(ctx);
      }
    }

    // Record the type we found.
    repr->setValue(nestedType, nullptr);
  } else {
    // Resolve the base to a potential archetype.
    // Perform typo correction.
    TypoCorrectionResults corrections(repr->getNameRef(), repr->getNameLoc());
    TypeChecker::performTypoCorrection(DC, DeclRefKind::Ordinary,
                                       MetatypeType::get(baseTy),
                                       defaultMemberLookupOptions,
                                       corrections, genericSig);

    // Check whether we have a single type result.
    auto singleType = cast_or_null<TypeDecl>(
      corrections.getUniqueCandidateMatching([](ValueDecl *result) {
        return isa<TypeDecl>(result);
      }));

    // If we don't have a single result, complain and fail.
    if (!singleType) {
      auto name = repr->getNameRef();
      auto nameLoc = repr->getNameLoc();
      const auto kind = describeDeclOfType(baseTy);
      ctx.Diags.diagnose(nameLoc, diag::invalid_member_type, name, kind, baseTy)
          .highlight(baseRange);
      corrections.noteAllCandidates();

      return ErrorType::get(ctx);
    }

    // We have a single type result. Suggest it.
    ctx.Diags
        .diagnose(repr->getNameLoc(), diag::invalid_member_type_suggest, baseTy,
                  repr->getNameRef(), singleType->getBaseName())
        .fixItReplace(repr->getNameLoc().getSourceRange(),
                      singleType->getBaseName().userFacingName());

    // Correct to the single type result.
    repr->setValue(singleType, nullptr);
  }

  auto *concrete = repr->getBoundDecl();

  if (auto concreteBase = genericSig->getConcreteType(baseTy)) {
    bool hasUnboundOpener = !!getUnboundTypeOpener();
    switch (TypeChecker::isUnsupportedMemberTypeAccess(concreteBase, concrete,
                                                       hasUnboundOpener)) {
    case TypeChecker::UnsupportedMemberTypeAccessKind::TypeAliasOfExistential:
      ctx.Diags.diagnose(repr->getNameLoc(),
                         diag::typealias_outside_of_protocol,
                         repr->getNameRef(), concreteBase);
      break;
    case TypeChecker::UnsupportedMemberTypeAccessKind::AssociatedTypeOfExistential:
      ctx.Diags.diagnose(repr->getNameLoc(),
                         diag::assoc_type_outside_of_protocol,
                         repr->getNameRef(), concreteBase);
      break;
    default:
      break;
    };
  }

  // If the nested type has been resolved to an associated type, use it.
  if (auto assocType = dyn_cast<AssociatedTypeDecl>(concrete)) {
    return DependentMemberType::get(baseTy, assocType);
  }

  // There are two situations possible here:
  //
  // 1. Member comes from the protocol, which means that it has been
  //    found through a conformance constraint placed on base e.g. `T: P`.
  //    In this case member is a `typealias` declaration located in
  //    protocol or protocol extension.
  //
  // 2. Member comes from struct/enum/class type, which means that it
  //    has been found through same-type constraint on base e.g. `T == Q`.
  //
  // If this is situation #2 we need to make sure to switch base to
  // a concrete type (according to equivalence class) otherwise we'd
  // end up using incorrect generic signature while attempting to form
  // a substituted type for the member we found.
  if (!concrete->getDeclContext()->getSelfProtocolDecl()) {
    if (auto concreteTy = genericSig->getConcreteType(baseTy))
      baseTy = concreteTy;
    else {
      baseTy = genericSig->getSuperclassBound(baseTy);
      assert(baseTy);
    }
  }

  return TypeChecker::substMemberTypeWithBase(DC->getParentModule(), concrete,
                                              baseTy);
}

bool TypeResolution::areSameType(Type type1, Type type2) const {
  if (type1->isEqual(type2))
    return true;

  // If neither type has a type parameter, we're done.
  if (!type1->hasTypeParameter() && !type2->hasTypeParameter()) {
    return false;
  }

  if (stage == TypeResolutionStage::Interface) {
    // If we have a generic signature, canonicalize using it.
    if (auto genericSig = getGenericSignature()) {
      // If both are type parameters, we can use a cheaper check
      // that avoids transforming the type and computing anchors.
      if (type1->isTypeParameter() && type2->isTypeParameter()) {
        return genericSig->areReducedTypeParametersEqual(type1, type2);
      }
      return genericSig.getReducedType(type1) ==
             genericSig.getReducedType(type2);
    }
  }

  assert(stage == TypeResolutionStage::Structural);
  return false;
}

Type TypeChecker::getOptionalType(SourceLoc loc, Type elementType) {
  ASTContext &ctx = elementType->getASTContext();
  if (!ctx.getOptionalDecl()) {
    ctx.Diags.diagnose(loc, diag::sugar_type_not_found, 1);
    return ErrorType::get(ctx);
  }

  return OptionalType::get(elementType);
}

Type
TypeChecker::getDynamicBridgedThroughObjCClass(DeclContext *dc,
                                               Type dynamicType,
                                               Type valueType) {
  // We can only bridge from class or Objective-C existential types.
  if (!dynamicType->satisfiesClassConstraint())
    return Type();

  // If the value type cannot be bridged, we're done.
  if (!valueType->isPotentiallyBridgedValueType())
    return Type();

  return dc->getASTContext().getBridgedToObjC(dc, valueType);
}

/// Retrieve the identity form of the opaque type archetype type.
static Type getOpaqueArchetypeIdentity(
    OpaqueTypeDecl *opaqueDecl, unsigned ordinal) {
  auto outerGenericSignature = opaqueDecl->getNamingDecl()
                                   ->getInnermostDeclContext()
                                   ->getGenericSignatureOfContext();

  SubstitutionMap subs;
  if (outerGenericSignature)
    subs = outerGenericSignature->getIdentitySubstitutionMap();

  Type interfaceType = opaqueDecl->getOpaqueGenericParams()[ordinal];
  return OpaqueTypeArchetypeType::get(opaqueDecl, interfaceType, subs);
}

/// Adjust the underlying type of a typealias within the given context to
/// account for @preconcurrency.
static Type adjustTypeAliasTypeInContext(
    Type type, TypeAliasDecl *aliasDecl, DeclContext *fromDC,
    TypeResolutionOptions options) {
  // If we are in a @preconcurrency declaration, don't adjust the types of
  // type aliases.
  if (options.contains(TypeResolutionFlags::Preconcurrency))
    return type;

  // If the type alias itself isn't marked with @preconcurrency, don't
  // adjust the type.
  if (!aliasDecl->preconcurrency())
    return type;

  // Only adjust the type within a strict concurrency context, so we don't
  // change the types as seen by code that hasn't adopted concurrency.
  if (contextRequiresStrictConcurrencyChecking(
          fromDC,
          [](const AbstractClosureExpr *closure) {
            return closure->getType();
          },
          [](const ClosureExpr *closure) {
            return closure->isIsolatedByPreconcurrency();
          }))
    return type;

  return type->stripConcurrency(
      /*recurse=*/true, /*dropGlobalActor=*/true);
}

Type TypeResolution::resolveTypeInContext(TypeDecl *typeDecl,
                                          DeclContext *foundDC,
                                          bool isSpecialized) const {
  auto fromDC = getDeclContext();
  ASTContext &ctx = fromDC->getASTContext();

  // If we found a generic parameter, map to the archetype if there is one.
  if (auto genericParam = dyn_cast<GenericTypeParamDecl>(typeDecl)) {
    // If this generic parameter is for an opaque type, map to the opened
    // archetype.
    if (auto opaqueDecl = dyn_cast<OpaqueTypeDecl>(getDeclContext())) {
      if (genericParam->getDepth() ==
              opaqueDecl->getOpaqueGenericParams().front()->getDepth()) {
        return getOpaqueArchetypeIdentity(opaqueDecl, genericParam->getIndex());
      }
    }

    return genericParam->getDeclaredInterfaceType();
  }

  /// Call this function before returning the underlying type of a typealias,
  /// to adjust its type for concurrency.
  auto adjustAliasType = [&](Type type) -> Type {
    return adjustTypeAliasTypeInContext(
        type, cast<TypeAliasDecl>(typeDecl), fromDC, options);
  };

  if (!isSpecialized) {
    // If we are referring to a type within its own context, and we have either
    // a generic type with no generic arguments or a non-generic type, use the
    // type within the context.
    if (auto *nominalType = dyn_cast<NominalTypeDecl>(typeDecl)) {
      for (auto *parentDC = fromDC; !parentDC->isModuleScopeContext();
           parentDC = parentDC->getParentForLookup()) {
        auto *parentNominal = parentDC->getSelfNominalTypeDecl();
        if (parentNominal == nominalType)
          return parentDC->getDeclaredInterfaceType();
        if (isa<ExtensionDecl>(parentDC)) {
          auto *extendedType = parentNominal;
          while (extendedType != nullptr) {
            if (extendedType == nominalType)
              return extendedType->getDeclaredInterfaceType();
            extendedType = extendedType->getParent()->getSelfNominalTypeDecl();
          }
        }
      }
    }

    // If we're inside an extension of a type alias, allow the type alias to be
    // referenced without generic arguments as well.
    if (auto *aliasDecl = dyn_cast<TypeAliasDecl>(typeDecl)) {
      for (auto *parentDC = fromDC; !parentDC->isModuleScopeContext();
           parentDC = parentDC->getParentForLookup()) {
        if (auto *ext = dyn_cast<ExtensionDecl>(parentDC)) {
          auto extendedType = ext->getExtendedType();
          if (auto *unboundGeneric =
                  dyn_cast<UnboundGenericType>(extendedType.getPointer())) {
            if (auto *ugAliasDecl =
                    dyn_cast<TypeAliasDecl>(unboundGeneric->getAnyGeneric())) {
              if (ugAliasDecl == aliasDecl) {
                if (getStage() == TypeResolutionStage::Structural &&
                    aliasDecl->getUnderlyingTypeRepr() != nullptr) {
                  return adjustAliasType(aliasDecl->getStructuralType());
                }
                return adjustAliasType(aliasDecl->getDeclaredInterfaceType());
              }

              extendedType = unboundGeneric->getParent();
              continue;
            }
          }
          if (auto *aliasType =
                  dyn_cast<TypeAliasType>(extendedType.getPointer())) {
            if (aliasType->getDecl() == aliasDecl) {
              if (getStage() == TypeResolutionStage::Structural &&
                  aliasDecl->getUnderlyingTypeRepr() != nullptr) {
                return adjustAliasType(aliasDecl->getStructuralType());
              }
              return adjustAliasType(aliasDecl->getDeclaredInterfaceType());
            }
            extendedType = aliasType->getParent();
            continue;
          }
        }
      }
    }
  }

  // Simple case -- the type is not nested inside of another type.
  // However, it might be nested inside another generic context, so
  // we do want to write the type in terms of interface types or
  // context archetypes, depending on the resolver given to us.
  if (!typeDecl->getDeclContext()->isTypeContext()) {
    if (auto *aliasDecl = dyn_cast<TypeAliasDecl>(typeDecl)) {
      // For a generic typealias, return the unbound generic form of the type.
      if (aliasDecl->getGenericParams())
        return aliasDecl->getUnboundGenericType();

      // Otherwise, return the appropriate type.
      if (getStage() == TypeResolutionStage::Structural &&
          aliasDecl->getUnderlyingTypeRepr() != nullptr) {
        return adjustAliasType(aliasDecl->getStructuralType());
      }
      return adjustAliasType(aliasDecl->getDeclaredInterfaceType());
    }

    // When a nominal type used outside its context, return the unbound
    // generic form of the type.
    if (auto *nominalDecl = dyn_cast<NominalTypeDecl>(typeDecl))
      return nominalDecl->getDeclaredType();

    assert(isa<ModuleDecl>(typeDecl));
    return typeDecl->getDeclaredInterfaceType();
  }

  assert(foundDC);

  // selfType is the self type of the context, unless the
  // context is a protocol type, in which case we might have
  // to use the existential type or superclass bound as a
  // parent type instead.
  Type selfType;
  if (isa<NominalTypeDecl>(typeDecl) &&
      typeDecl->getDeclContext()->getSelfProtocolDecl()) {
    // When looking up a nominal type declaration inside of a
    // protocol extension, always use the nominal type and
    // not the protocol 'Self' type.
    if (!foundDC->getDeclaredInterfaceType())
      return ErrorType::get(ctx);

    selfType = foundDC->getDeclaredInterfaceType();
  } else {
    // Otherwise, we want the protocol 'Self' type for
    // substituting into alias types and associated types.
    selfType = foundDC->getSelfInterfaceType();

    if (selfType->is<GenericTypeParamType>()) {
      if (isa<ProtocolDecl>(typeDecl->getDeclContext())) {
        if (isa<AssociatedTypeDecl>(typeDecl) ||
            (isa<TypeAliasDecl>(typeDecl) &&
             !cast<TypeAliasDecl>(typeDecl)->isGeneric() &&
             !isSpecialized)) {
          if (getStage() == TypeResolutionStage::Structural) {
            return DependentMemberType::get(selfType, typeDecl->getName());
          } else if (auto assocType = dyn_cast<AssociatedTypeDecl>(typeDecl)) {
            typeDecl = assocType->getAssociatedTypeAnchor();
          }
        }
      }

      if (typeDecl->getDeclContext()->getSelfClassDecl()) {
        // We found a member of a class from a protocol or protocol
        // extension.
        //
        // Get the superclass of the 'Self' type parameter.
        auto sig = foundDC->getGenericSignatureOfContext();
        if (!sig)
          return ErrorType::get(ctx);
        auto superclassType = sig->getSuperclassBound(selfType);
        if (!superclassType)
          return ErrorType::get(ctx);

        selfType = superclassType;
      }
    }
  }

  // Finally, substitute the base type into the member type.
  return TypeChecker::substMemberTypeWithBase(
      fromDC->getParentModule(), typeDecl, selfType, /*useArchetypes=*/false);
}

/// This function checks if a bound generic type is UnsafePointer<Void> or
/// UnsafeMutablePointer<Void>. For these two type representations, we should
/// warn users that they are deprecated and replace them with more handy
/// UnsafeRawPointer and UnsafeMutableRawPointer, respectively.
static bool isPointerToVoid(ASTContext &Ctx, Type Ty, bool &IsMutable) {
  if (Ty.isNull())
    return false;
  auto *BGT = Ty->getAs<BoundGenericType>();
  if (!BGT)
    return false;
  if (!BGT->isUnsafePointer() && !BGT->isUnsafeMutablePointer())
    return false;
  IsMutable = BGT->isUnsafeMutablePointer();
  assert(BGT->getGenericArgs().size() == 1);
  return BGT->getGenericArgs().front()->isVoid();
}

bool TypeChecker::checkContextualRequirements(GenericTypeDecl *decl,
                                              Type parentTy, SourceLoc loc,
                                              ModuleDecl *module,
                                              GenericSignature contextSig) {
  assert(parentTy && "expected a parent type");
  if (parentTy->hasUnboundGenericType() || parentTy->hasTypeVariable()) {
    return true;
  }

  SourceLoc noteLoc;
  {
    // We are interested in either a contextual where clause or
    // a constrained extension context.
    const auto ext = dyn_cast<ExtensionDecl>(decl->getDeclContext());
    if (decl->getTrailingWhereClause())
      noteLoc = decl->getLoc();
    else if (ext && ext->isConstrainedExtension())
      noteLoc = ext->getLoc();
    else
      return true;

    if (noteLoc.isInvalid())
      noteLoc = loc;
  }

  const auto subMap = parentTy->getContextSubstitutions(decl->getDeclContext());
  const auto genericSig = decl->getGenericSignature();

  const auto substitutions = [&](SubstitutableType *type) -> Type {
    auto result = QueryTypeSubstitutionMap{subMap}(type);
    if (result->hasTypeParameter()) {
      if (contextSig) {
        auto *genericEnv = contextSig.getGenericEnvironment();
        return genericEnv->mapTypeIntoContext(result);
      }
    }
    return result;
  };

  const auto result = TypeChecker::checkGenericArgumentsForDiagnostics(
      module, genericSig.getRequirements(), substitutions);
  switch (result) {
  case CheckGenericArgumentsResult::RequirementFailure:
    if (loc.isValid()) {
      TypeChecker::diagnoseRequirementFailure(
          result.getRequirementFailureInfo(), loc, noteLoc,
          decl->getDeclaredInterfaceType(), genericSig.getGenericParams(),
          substitutions, module);
    }

    return false;
  case CheckGenericArgumentsResult::SubstitutionFailure:
    return false;
  case CheckGenericArgumentsResult::Success:
    return true;
  }
  llvm_unreachable("invalid requirement check type");
}

/// Apply generic arguments to the given type.
///
/// If the type is itself not generic, this does nothing.
///
/// This function emits diagnostics about an invalid type or the wrong number
/// of generic arguments, whereas
/// \c TypeResolution::applyUnboundGenericArguments requires this
/// to be in a correct and valid form.
///
/// \param type The generic type to which to apply arguments.
/// \param resolution The type resolution to perform.
/// \param silContext Used to look up generic parameters in SIL mode.
/// \param repr The arguments to apply with the angle bracket range for
/// diagnostics.
///
/// \returns A BoundGenericType bound to the given arguments, or null on
/// error.
///
/// \see TypeResolution::applyUnboundGenericArguments
static Type applyGenericArguments(Type type, TypeResolution resolution,
                                  SILTypeResolutionContext *silContext,
                                  IdentTypeRepr *repr) {
  auto options = resolution.getOptions();
  auto dc = resolution.getDeclContext();
  auto loc = repr->getNameLoc().getBaseNameLoc();

  auto *generic = dyn_cast<GenericIdentTypeRepr>(repr);
  if (!generic) {
    if (auto *const unboundTy = type->getAs<UnboundGenericType>()) {
      if (!options.is(TypeResolverContext::TypeAliasDecl) &&
          !options.is(TypeResolverContext::ExtensionBinding)) {
        // If the resolution object carries an opener, attempt to open
        // the unbound generic type.
        // TODO: We should be able to just open the generic arguments as N
        // different PlaceholderTypes.
        if (const auto openerFn = resolution.getUnboundTypeOpener())
          if (const auto boundTy = openerFn(unboundTy))
            return boundTy;

        return type;
      }
    }

    if (resolution.getStage() == TypeResolutionStage::Structural)
      return type;

    GenericTypeDecl *decl;
    Type parentTy;
    if (auto *aliasTy = dyn_cast<TypeAliasType>(type.getPointer())) {
      decl = aliasTy->getDecl();
      parentTy = aliasTy->getParent();
    } else if (auto *nominalTy = type->getAs<NominalType>()) {
      decl = nominalTy->getDecl();
      parentTy = nominalTy->getParent();
    } else {
      return type;
    }

    if (!parentTy) {
      return type;
    }

    if (TypeChecker::checkContextualRequirements(
            decl, parentTy, loc, dc->getParentModule(),
            resolution.getGenericSignature()))
      return type;

    return ErrorType::get(resolution.getASTContext());
  }

  if (type->hasError()) {
    generic->setInvalid();
    return type;
  }

  auto &ctx = dc->getASTContext();
  auto &diags = ctx.Diags;

  auto genericArgs = generic->getGenericArgs();

  if (auto *protoType = type->getAs<ProtocolType>()) {
    auto *protoDecl = protoType->getDecl();

    auto assocTypes = protoDecl->getPrimaryAssociatedTypes();
    if (assocTypes.empty()) {
      diags.diagnose(loc, diag::protocol_does_not_have_primary_assoc_type,
                     protoType)
           .fixItRemove(generic->getAngleBrackets());
      if (!protoDecl->isImplicit()) {
        diags.diagnose(protoDecl, diag::decl_declared_here,
                       protoDecl->getName());
      }
      return ErrorType::get(ctx);
    }

    if (genericArgs.size() != assocTypes.size()) {
      diags.diagnose(loc,
                     diag::parameterized_protocol_type_argument_count_mismatch,
                     protoType, genericArgs.size(), assocTypes.size(),
                     (genericArgs.size() < assocTypes.size()) ? 1 : 0);

      return ErrorType::get(ctx);
    }

    // Build ParameterizedProtocolType if the protocol has a primary associated
    // type and we're in a supported context.
    if (resolution.getOptions().isConstraintImplicitExistential() &&
        !ctx.LangOpts.hasFeature(Feature::ImplicitSome)) {

      if (!genericArgs.empty()) {

        SmallVector<Type, 2> argTys;
        for (auto *genericArg : genericArgs) {
          Type argTy = resolution.resolveType(genericArg);
          if (!argTy || argTy->hasError())
            return ErrorType::get(ctx);

          argTys.push_back(argTy);
        }

        auto parameterized =
            ParameterizedProtocolType::get(ctx, protoType, argTys);
        diags.diagnose(loc, diag::existential_requires_any, parameterized,
                       ExistentialType::get(parameterized),
                       /*isAlias=*/isa<TypeAliasType>(type.getPointer()));
      } else {
        diags.diagnose(loc, diag::existential_requires_any,
                       protoDecl->getDeclaredInterfaceType(),
                       protoDecl->getDeclaredExistentialType(),
                       /*isAlias=*/isa<TypeAliasType>(type.getPointer()));
      }

      return ErrorType::get(ctx);
    }

    // Disallow opaque types anywhere in the structure of the generic arguments
    // to a parameterized existential type.
    if (options.is(TypeResolverContext::ExistentialConstraint))
      options |= TypeResolutionFlags::DisallowOpaqueTypes;
    auto argOptions = options.withoutContext().withContext(
        TypeResolverContext::GenericArgument);
    auto genericResolution = resolution.withOptions(argOptions);

    SmallVector<Type, 2> argTys;
    for (auto *genericArg : genericArgs) {
      Type argTy = genericResolution.resolveType(genericArg, silContext);
      if (!argTy || argTy->hasError())
        return ErrorType::get(ctx);

      argTys.push_back(argTy);
    }

    return ParameterizedProtocolType::get(ctx, protoType, argTys);
  }

  // We must either have an unbound generic type, or a generic type alias.
  if (!type->is<UnboundGenericType>()) {
     if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
      auto diag = diags.diagnose(loc, diag::not_a_generic_type, type);

      // Don't add fixit on module type; that isn't the right type regardless
      // of whether it had generic arguments.
      if (!type->is<ModuleType>()) {
        // When turning a SourceRange into CharSourceRange the closing angle
        // brackets on nested generics are lexed as one token.
        SourceRange angles = generic->getAngleBrackets();
        diag.fixItRemoveChars(angles.Start,
                              angles.End.getAdvancedLocOrInvalid(1));
      }

      generic->setInvalid();
    }
    return ErrorType::get(ctx);
  }

  auto *unboundType = type->castTo<UnboundGenericType>();
  auto *decl = unboundType->getDecl();

  // Make sure we have the right number of generic arguments.
  auto genericParams = decl->getGenericParams();
  auto hasParameterPack = llvm::any_of(
      *genericParams, [](auto *paramDecl) {
          return paramDecl->isParameterPack();
      });

  // Resolve the types of the generic arguments.
  auto argOptions = options.withoutContext().withContext(
      TypeResolverContext::GenericArgument);
  auto genericResolution = resolution.withOptions(argOptions);

  // In SIL mode, Optional<T> interprets T as a SIL type.
  if (options.contains(TypeResolutionFlags::SILType)) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
      if (nominal->isOptionalDecl()) {
        genericResolution = resolution;
      }
    }
  }

  SmallVector<Type, 2> args;
  for (auto tyR : genericArgs) {
    // Propagate failure.
    Type substTy = genericResolution.resolveType(tyR, silContext);
    if (!substTy || substTy->hasError())
      return ErrorType::get(ctx);

    args.push_back(substTy);
  }

  if (!hasParameterPack) {
    // For generic types without type parameter packs, we require
    // the number of declared generic parameters match the number of
    // arguments.
    if (genericArgs.size() != genericParams->size()) {
      if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
        diags
            .diagnose(loc, diag::type_parameter_count_mismatch, decl->getName(),
                      genericParams->size(),
                      genericArgs.size(),
                      genericArgs.size() < genericParams->size(),
                      /*hasParameterPack=*/0)
            .highlight(generic->getAngleBrackets());
        decl->diagnose(diag::kind_declname_declared_here,
                       DescriptiveDeclKind::GenericType, decl->getName());
      }
      return ErrorType::get(ctx);
    }
  } else {
    // For generic types with type parameter packs, we only require
    // that the number of arguments is enough to saturate the number of
    // regular generic parameters. The parameter pack will absorb
    // zero or arguments.
    SmallVector<Type, 2> params;
    for (auto paramDecl : genericParams->getParams()) {
      auto paramType = paramDecl->getDeclaredInterfaceType();
      params.push_back(paramDecl->isParameterPack()
                       ? PackExpansionType::get(paramType, paramType)
                       : paramType);
    }

    PackMatcher matcher(params, args, ctx);
    if (matcher.match()) {
      if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
        diags
            .diagnose(loc, diag::type_parameter_count_mismatch, decl->getName(),
                      genericParams->size() - 1,
                      genericArgs.size(),
                      genericArgs.size() < genericParams->size(),
                      /*hasParameterPack=*/1)
            .highlight(generic->getAngleBrackets());
        decl->diagnose(diag::kind_declname_declared_here,
                       DescriptiveDeclKind::GenericType, decl->getName());
      }
      return ErrorType::get(ctx);
    }

    args.clear();
    for (unsigned i : indices(params)) {
      auto found = std::find_if(matcher.pairs.begin(),
                                matcher.pairs.end(),
                                [&](const MatchedPair &pair) -> bool {
                                  return pair.lhsIdx == i;
                                });
      assert(found != matcher.pairs.end());

      auto arg = found->rhs;

      // PackMatcher will always produce a PackExpansionType as the
      // arg for a pack parameter, if necessary by wrapping a PackType
      // in one.  (It's a weird representation.)  Look for that pattern
      // and unwrap the pack.  Otherwise, we must have matched with a
      // single component which happened to be an expansion; wrap that
      // in a PackType.  In either case, we always want arg to end up
      // a PackType.
      if (auto *expansionType = arg->getAs<PackExpansionType>()) {
        auto pattern = expansionType->getPatternType();
        if (auto pack = pattern->getAs<PackType>()) {
          arg = pack;
        } else {
          arg = PackType::get(ctx, {expansionType});
        }
      }

      args.push_back(arg);
    }
  }

  const auto result = resolution.applyUnboundGenericArguments(
      decl, unboundType->getParent(), loc, args);

  // Migration hack.
  bool isMutablePointer;
  if (isPointerToVoid(dc->getASTContext(), result, isMutablePointer)) {
    if (isMutablePointer)
      diags.diagnose(loc, diag::use_of_void_pointer, "Mutable").
        fixItReplace(generic->getSourceRange(), "UnsafeMutableRawPointer");
    else
      diags.diagnose(loc, diag::use_of_void_pointer, "").
        fixItReplace(generic->getSourceRange(), "UnsafeRawPointer");
  }

  if (auto clangDecl = decl->getClangDecl()) {
    if (auto classTemplateDecl =
            dyn_cast<clang::ClassTemplateDecl>(clangDecl)) {
      SmallVector<Type, 2> typesOfGenericArgs;
      for (auto typeRepr : generic->getGenericArgs()) {
        typesOfGenericArgs.push_back(resolution.resolveType(typeRepr));
      }

      SmallVector<clang::TemplateArgument, 2> templateArguments;
      std::unique_ptr<TemplateInstantiationError> error =
          ctx.getClangTemplateArguments(
              classTemplateDecl->getTemplateParameters(), typesOfGenericArgs,
              templateArguments);

      if (error) {
        std::string failedTypesStr;
        llvm::raw_string_ostream failedTypesStrStream(failedTypesStr);
        llvm::interleaveComma(error->failedTypes, failedTypesStrStream);
        // TODO: This error message should not reference implementation details.
        // See: https://github.com/apple/swift/pull/33053#discussion_r477003350
        ctx.Diags.diagnose(
            loc, diag::unable_to_convert_generic_swift_types.ID,
            {classTemplateDecl->getName(), StringRef(failedTypesStr)});
        return ErrorType::get(ctx);
      }

      auto *clangModuleLoader = decl->getASTContext().getClangModuleLoader();
      auto instantiatedDecl = clangModuleLoader->instantiateCXXClassTemplate(
          const_cast<clang::ClassTemplateDecl *>(classTemplateDecl),
          templateArguments);
      if (instantiatedDecl) {
        instantiatedDecl->setTemplateInstantiationType(result);
        return instantiatedDecl->getDeclaredInterfaceType();
      } else {
        diags.diagnose(loc, diag::cxx_class_instantiation_failed);
        return ErrorType::get(ctx);
      }
    }
  }
  return result;
}

/// if any of the generic args are a concrete move-only type, emit an error.
/// returns true iff an error diagnostic was emitted
static bool didDiagnoseMoveOnlyGenericArgs(ASTContext &ctx,
                                         SourceLoc loc,
                                         Type unboundTy,
                                         ArrayRef<Type> genericArgs) {
  bool didEmitDiag = false;
  for (auto t: genericArgs) {
    if (!t->isPureMoveOnly())
      continue;

    ctx.Diags.diagnose(loc, diag::noncopyable_generics_specific, t, unboundTy);
    didEmitDiag = true;
  }

  return didEmitDiag;
}

/// Apply generic arguments to the given type.
Type TypeResolution::applyUnboundGenericArguments(
    GenericTypeDecl *decl, Type parentTy, SourceLoc loc,
    ArrayRef<Type> genericArgs) const {
  assert(genericArgs.size() == decl->getGenericParams()->size() &&
         "invalid arguments, use applyGenericArguments to emit diagnostics "
         "and collect arguments to pack generic parameters");

  TypeSubstitutionMap subs;

  // Get the interface type for the declaration. We will be substituting
  // type parameters that appear inside this type with the provided
  // generic arguments.
  auto resultType = decl->getDeclaredInterfaceType();

  // If types involved in requirements check have either type variables
  // or unbound generics, let's skip the check here, and let the solver
  // do it when missing types are deduced.
  bool skipRequirementsCheck = false;

  // check for generic args that are move-only
  auto &ctx = getASTContext();
  if (didDiagnoseMoveOnlyGenericArgs(ctx, loc, resultType, genericArgs))
    return ErrorType::get(ctx);

  // Get the substitutions for outer generic parameters from the parent
  // type.
  if (parentTy) {
    if (parentTy->hasUnboundGenericType()) {
      // If we're working with a nominal type declaration, just construct
      // a bound generic type without checking the generic arguments.
      if (auto *nominalDecl = dyn_cast<NominalTypeDecl>(decl)) {
        return BoundGenericType::get(nominalDecl, parentTy, genericArgs);
      }

      if (!resultType->hasTypeParameter())
        return resultType;

      auto genericSig = decl->getGenericSignature();
      auto parentSig = decl->getDeclContext()->getGenericSignatureOfContext();
      for (auto gp : parentSig.getGenericParams())
        subs[gp->getCanonicalType()->castTo<GenericTypeParamType>()] =
            genericSig->getConcreteType(gp);
    } else {
      subs = parentTy->getContextSubstitutions(decl->getDeclContext());
    }

    skipRequirementsCheck |= parentTy->hasTypeVariable();

  // Fill in substitutions for outer generic parameters if we have a local
  // type in generic context. This isn't actually supported all the way,
  // but we have to put something here so we don't crash.
  } else if (auto parentSig =
                 decl->getDeclContext()->getGenericSignatureOfContext()) {
    for (auto gp : parentSig.getGenericParams()) {
      subs[gp->getCanonicalType()->castTo<GenericTypeParamType>()] = gp;
    }
  }

  // Realize the types of the generic arguments and add them to the
  // substitution map.
  auto innerParams = decl->getGenericParams()->getParams();
  for (unsigned i : indices(innerParams)) {
    auto origTy = innerParams[i]->getDeclaredInterfaceType();
    auto origGP = origTy->getCanonicalType()->castTo<GenericTypeParamType>();

    auto substTy = genericArgs[i];

    // Enter a substitution.
    subs[origGP] = substTy;

    skipRequirementsCheck |=
        substTy->hasTypeVariable() || substTy->hasUnboundGenericType();
  }

  // Check the generic arguments against the requirements of the declaration's
  // generic signature.
  auto *module = getDeclContext()->getParentModule();

  if (!skipRequirementsCheck && getStage() == TypeResolutionStage::Interface) {
    // Check the generic arguments against the requirements of the declaration's
    // generic signature.

    SourceLoc noteLoc = decl->getLoc();
    if (noteLoc.isInvalid())
      noteLoc = loc;

    auto genericSig = decl->getGenericSignature();
    const auto substitutions = [&](SubstitutableType *type) -> Type {
      auto result = QueryTypeSubstitutionMap{subs}(type);
      if (result->hasTypeParameter()) {
        if (const auto contextSig = getGenericSignature()) {
          auto *genericEnv = contextSig.getGenericEnvironment();
          return genericEnv->mapTypeIntoContext(result);
        }
      }
      return result;
    };

    const auto result = TypeChecker::checkGenericArgumentsForDiagnostics(
        module, genericSig.getRequirements(), substitutions);
    switch (result) {
    case CheckGenericArgumentsResult::RequirementFailure:
      if (loc.isValid()) {
        TypeChecker::diagnoseRequirementFailure(
            result.getRequirementFailureInfo(), loc, noteLoc,
            UnboundGenericType::get(decl, parentTy, getASTContext()),
            genericSig.getGenericParams(), substitutions, module);
      }

      LLVM_FALLTHROUGH;
    case CheckGenericArgumentsResult::SubstitutionFailure:
      return ErrorType::get(getASTContext());
    case CheckGenericArgumentsResult::Success:
      break;
    }
  }

  // For a typealias, use the underlying type. We'll wrap up the result
  // later.
  auto typealias = dyn_cast<TypeAliasDecl>(decl);
  if (typealias) {
    resultType = typealias->getUnderlyingType();
  }

  // Apply the substitution map to the interface type of the declaration.
  resultType = resultType.subst(QueryTypeSubstitutionMap{subs},
                                LookUpConformanceInModule(module));

  // Form a sugared typealias reference.
  if (typealias && (!parentTy || !parentTy->isAnyExistentialType())) {
    auto genericSig = typealias->getGenericSignature();
    auto subMap = SubstitutionMap::get(genericSig,
                                       QueryTypeSubstitutionMap{subs},
                                       LookUpConformanceInModule(module));
    resultType = TypeAliasType::get(typealias, parentTy, subMap, resultType);
  }

  return resultType;
}

/// Diagnose a use of an unbound generic type.
static void diagnoseUnboundGenericType(Type ty, SourceLoc loc) {
  auto &ctx = ty->getASTContext();
  if (auto unbound = ty->getAs<UnboundGenericType>()) {
    auto *decl = unbound->getDecl();
    {
      // Compute the string before creating a new diagnostic, since
      // getDefaultGenericArgumentsString() might emit its own
      // diagnostics.
      SmallString<64> genericArgsToAdd;
      bool hasGenericArgsToAdd =
          TypeChecker::getDefaultGenericArgumentsString(genericArgsToAdd,
                                                        decl);

      auto diag = ctx.Diags.diagnose(loc,
          diag::generic_type_requires_arguments, ty);
      if (hasGenericArgsToAdd)
        diag.fixItInsertAfter(loc, genericArgsToAdd);
    }

    decl->diagnose(diag::kind_declname_declared_here,
                   DescriptiveDeclKind::GenericType,
                   decl->getName());
  } else {
    ty.findIf([&](Type t) -> bool {
      if (auto unbound = t->getAs<UnboundGenericType>()) {
        ctx.Diags.diagnose(loc,
            diag::generic_type_requires_arguments, t);
        return true;
      }

      return false;
    });
  }
}

// Produce a diagnostic if the type we referenced was an
// associated type but the type itself was erroneous. We'll produce a
// diagnostic here if the diagnostic for the bad type witness would show up in
// a different context.
static void maybeDiagnoseBadConformanceRef(DeclContext *dc,
                                           Type parentTy,
                                           SourceLoc loc,
                                           TypeDecl *typeDecl) {
  auto protocol = dyn_cast<ProtocolDecl>(typeDecl->getDeclContext());

  // If we weren't given a conformance, go look it up.
  ProtocolConformance *conformance = nullptr;
  if (protocol) {
    auto conformanceRef = dc->getParentModule()->lookupConformance(
        parentTy, protocol);
    if (conformanceRef.isConcrete())
      conformance = conformanceRef.getConcrete();
  }

  // If any errors have occurred, don't bother diagnosing this cross-file
  // issue.
  ASTContext &ctx = dc->getASTContext();
  if (ctx.Diags.hadAnyError())
    return;

  auto diagCode =
    (!protocol || (conformance && !conformance->getConditionalRequirementsIfAvailable()))
          ? diag::unsupported_recursion_in_associated_type_reference
          : diag::broken_associated_type_witness;

  ctx.Diags.diagnose(loc, diagCode, isa<TypeAliasDecl>(typeDecl),
                     typeDecl->getName(), parentTy);
}

/// Returns a valid type or ErrorType in case of an error.
static Type resolveTypeDecl(TypeDecl *typeDecl, DeclContext *foundDC,
                            TypeResolution resolution,
                            SILTypeResolutionContext *silContext,
                            IdentTypeRepr *repr) {
  // Resolve the type declaration to a specific type. How this occurs
  // depends on the current context and where the type was found.
  Type type = resolution.resolveTypeInContext(typeDecl, foundDC,
                                              isa<GenericIdentTypeRepr>(repr));

  if (type->hasError() && foundDC &&
      (isa<AssociatedTypeDecl>(typeDecl) || isa<TypeAliasDecl>(typeDecl))) {
    auto fromDC = resolution.getDeclContext();
    assert(fromDC && "No declaration context for type resolution?");
    maybeDiagnoseBadConformanceRef(fromDC, foundDC->getDeclaredInterfaceType(),
                                   repr->getNameLoc().getBaseNameLoc(),
                                   typeDecl);
  }

  return applyGenericArguments(type, resolution, silContext, repr);
}

static std::string getDeclNameFromContext(DeclContext *dc,
                                          NominalTypeDecl *nominal) {
  // We don't allow an unqualified reference to a type inside an
  // extension if the type is itself nested inside another type,
  // eg:
  //
  // extension A.B { ... B ... }
  //
  // Instead, you must write 'A.B'. Calculate the right name to use
  // for fixits.
  if (!isa<ExtensionDecl>(dc)) {
    SmallVector<Identifier, 2> idents;
    auto *parentNominal = nominal;
    while (parentNominal != nullptr) {
      idents.push_back(parentNominal->getName());
      parentNominal = parentNominal->getDeclContext()->getSelfNominalTypeDecl();
    }
    std::reverse(idents.begin(), idents.end());
    std::string result;
    for (auto ident : idents) {
      if (!result.empty())
        result += ".";
      result += ident.str();
    }
    return result;
  } else {
    return std::string(nominal->getName());
  }
}

/// Diagnose a reference to an unknown type.
///
/// This routine diagnoses a reference to an unknown type, and
/// attempts to fix the reference via various means.
///
/// \returns either the corrected type, if possible, or an error type to
/// that correction failed.
static Type diagnoseUnknownType(TypeResolution resolution,
                                Type parentType,
                                SourceRange parentRange,
                                IdentTypeRepr *repr,
                                NameLookupOptions lookupOptions) {
  auto dc = resolution.getDeclContext();
  ASTContext &ctx = dc->getASTContext();
  auto &diags = ctx.Diags;

  // Unqualified lookup case.
  if (parentType.isNull()) {
    // Tailored diagnostic for custom attributes.
    if (resolution.getOptions().is(TypeResolverContext::CustomAttr)) {
      diags.diagnose(repr->getNameLoc(), diag::unknown_attribute,
                     repr->getNameRef().getBaseIdentifier().str());

      return ErrorType::get(ctx);
    }

    if (repr->getNameRef().isSimpleName(ctx.Id_Self) &&
        !isa<GenericIdentTypeRepr>(repr)) {
      DeclContext *nominalDC = nullptr;
      NominalTypeDecl *nominal = nullptr;
      if ((nominalDC = dc->getInnermostTypeContext()) &&
          (nominal = nominalDC->getSelfNominalTypeDecl())) {
        if (isa<ClassDecl>(nominal)) {
          // Attempt to refer to 'Self' within a non-protocol nominal
          // type. Fix this by replacing 'Self' with the nominal type name.

          // Produce a Fix-It replacing 'Self' with the nominal type name.
          auto name = getDeclNameFromContext(dc, nominal);
          diags.diagnose(repr->getNameLoc(), diag::dynamic_self_invalid, name)
              .fixItReplace(repr->getNameLoc().getSourceRange(), name);

          repr->setValue(nominal, nominalDC->getParent());

          return dc->getInnermostTypeContext()->getSelfInterfaceType();
        } else {
          diags.diagnose(repr->getNameLoc(), diag::cannot_find_type_in_scope,
                         repr->getNameRef());
          return ErrorType::get(ctx);
        }
      }
      // Attempt to refer to 'Self' from a free function.
      diags.diagnose(repr->getNameLoc(), diag::dynamic_self_non_method,
                     dc->getParent()->isLocalContext());

      return ErrorType::get(ctx);
    }

    // Try ignoring access control.
    NameLookupOptions relookupOptions = lookupOptions;
    relookupOptions |= NameLookupFlags::IgnoreAccessControl;
    auto inaccessibleResults = TypeChecker::lookupUnqualifiedType(
        dc, repr->getNameRef(), repr->getLoc(), relookupOptions);
    if (!inaccessibleResults.empty()) {
      // FIXME: What if the unviable candidates have different levels of access?
      auto first = cast<TypeDecl>(inaccessibleResults.front().getValueDecl());
      diags.diagnose(repr->getNameLoc(), diag::candidate_inaccessible,
                     first->getBaseName(), first->getFormalAccess());

      // FIXME: If any of the candidates (usually just one) are in the same
      // module we could offer a fix-it.
      for (auto lookupResult : inaccessibleResults)
        lookupResult.getValueDecl()->diagnose(diag::kind_declared_here,
                                              DescriptiveDeclKind::Type);

      // Don't try to recover here; we'll get more access-related diagnostics
      // downstream if we do.
      return ErrorType::get(ctx);
    }

    // Fallback.
    auto L = repr->getNameLoc();
    SourceRange R = repr->getNameLoc().getSourceRange();

    // Check if the unknown type is in the type remappings.
    auto &Remapped = ctx.RemappedTypes;
    auto TypeName = repr->getNameRef().getBaseIdentifier().str();
    auto I = Remapped.find(TypeName);
    if (I != Remapped.end()) {
      auto RemappedTy = I->second->getString();
      diags
          .diagnose(L, diag::cannot_find_type_in_scope_did_you_mean,
                    repr->getNameRef(), RemappedTy)
          .highlight(R)
          .fixItReplace(R, RemappedTy);

      // Replace the computed type with the suggested type.
      repr->overwriteNameRef(DeclNameRef(ctx.getIdentifier(RemappedTy)));

      // HACK: 'NSUInteger' suggests both 'UInt' and 'Int'.
      if (TypeName == swift::getSwiftName(KnownFoundationEntity::NSUInteger)) {
        diags.diagnose(L, diag::note_remapped_type, "UInt")
          .fixItReplace(R, "UInt");
      }

      return I->second;
    }

    diags.diagnose(L, diag::cannot_find_type_in_scope, repr->getNameRef())
        .highlight(R);
    if (!ctx.LangOpts.DisableExperimentalClangImporterDiagnostics) {
      ctx.getClangModuleLoader()->diagnoseTopLevelValue(
          repr->getNameRef().getFullName());
    }

    return ErrorType::get(ctx);
  }

  // Qualified lookup case.
  if (!parentType->mayHaveMembers()) {
    const auto kind = describeDeclOfType(parentType);
    diags
        .diagnose(repr->getNameLoc(), diag::invalid_member_type,
                  repr->getNameRef(), kind, parentType)
        .highlight(parentRange);

    if (!ctx.LangOpts.DisableExperimentalClangImporterDiagnostics) {
      ctx.getClangModuleLoader()->diagnoseMemberValue(
          repr->getNameRef().getFullName(), parentType);
    }

    return ErrorType::get(ctx);
  }

  // Try ignoring access control.
  NameLookupOptions relookupOptions = lookupOptions;
  relookupOptions |= NameLookupFlags::IgnoreAccessControl;
  auto inaccessibleMembers = TypeChecker::lookupMemberType(
      dc, parentType, repr->getNameRef(),
      repr->getLoc(), relookupOptions);
  if (inaccessibleMembers) {
    // FIXME: What if the unviable candidates have different levels of access?
    const TypeDecl *first = inaccessibleMembers.front().Member;
    diags.diagnose(repr->getNameLoc(), diag::candidate_inaccessible,
                   first->getBaseName(), first->getFormalAccess());

    // FIXME: If any of the candidates (usually just one) are in the same module
    // we could offer a fix-it.
    for (auto lookupResult : inaccessibleMembers)
      lookupResult.Member->diagnose(diag::kind_declared_here,
                                    DescriptiveDeclKind::Type);

    // Don't try to recover here; we'll get more access-related diagnostics
    // downstream if we do.
    return ErrorType::get(ctx);
  }

  // FIXME: Typo correction!

  // Lookup into a type.
  if (auto moduleType = parentType->getAs<ModuleType>()) {
    diags.diagnose(repr->getNameLoc(), diag::no_module_type, repr->getNameRef(),
                   moduleType->getModule()->getName());
  } else {
    LookupResult memberLookup;
    // Let's try to look any member of the parent type with the given name,
    // even if it is not a type, allowing for a more precise diagnostic.
    NLOptions memberLookupOptions = (NL_QualifiedDefault |
                                     NL_IgnoreAccessControl);
    SmallVector<ValueDecl *, 2> results;
    dc->lookupQualified(parentType, repr->getNameRef(), repr->getLoc(),
                        memberLookupOptions, results);

    // Looks like this is not a member type, but simply a member of parent type.
    if (!results.empty()) {
      auto member = results[0];
      diags
          .diagnose(repr->getNameLoc(), diag::invalid_member_reference,
                    member->getDescriptiveKind(), member->getName(), parentType)
          .highlight(parentRange);
    } else {
      const auto kind = describeDeclOfType(parentType);
      diags
          .diagnose(repr->getNameLoc(), diag::invalid_member_type,
                    repr->getNameRef(), kind, parentType)
          .highlight(parentRange);

      if (!ctx.LangOpts.DisableExperimentalClangImporterDiagnostics) {
        ctx.getClangModuleLoader()->diagnoseMemberValue(
            repr->getNameRef().getFullName(), parentType);
      }

      // Note where the type was defined, this can help diagnose if the user
      // expected name lookup to find a module when there's a conflicting type.
      if (auto typeDecl = parentType->getNominalOrBoundGenericNominal()) {
        ctx.Diags.diagnose(typeDecl, diag::decl_declared_here,
                           typeDecl->getName());
      }
    }
  }
  return ErrorType::get(ctx);
}

enum class SelfTypeKind {
  StaticSelf,
  DynamicSelf,
  InvalidSelf
};

static SelfTypeKind getSelfTypeKind(DeclContext *dc,
                                    TypeResolutionOptions options) {
  auto *typeDC = dc->getInnermostTypeContext();

  // For protocols, skip this code path and find the 'Self' generic parameter.
  if (typeDC->getSelfProtocolDecl())
    return SelfTypeKind::InvalidSelf;

  // In enums and structs, 'Self' is just a shorthand for the nominal type,
  // and can be used anywhere.
  if (!typeDC->getSelfClassDecl())
    return SelfTypeKind::StaticSelf;

  // In class methods, 'Self' is the DynamicSelfType and can only appear in
  // the return type.
  switch (options.getBaseContext()) {
  case TypeResolverContext::FunctionResult:
  case TypeResolverContext::PatternBindingDecl:
    return SelfTypeKind::DynamicSelf;
  case TypeResolverContext::AbstractFunctionDecl:
  case TypeResolverContext::SubscriptDecl:
  case TypeResolverContext::TypeAliasDecl:
  case TypeResolverContext::GenericTypeAliasDecl:
    // When checking a function or subscript parameter list, we have to go up
    // one level to determine if we're in a local context or not.
    if (dc->getParent()->isLocalContext())
      return SelfTypeKind::DynamicSelf;

    return SelfTypeKind::InvalidSelf;
  default:
    // In local functions inside classes, 'Self' is the DynamicSelfType and can
    // be used anywhere.
    if (dc->isLocalContext())
      return SelfTypeKind::DynamicSelf;

    return SelfTypeKind::InvalidSelf;
  }
}

static void diagnoseGenericArgumentsOnSelf(TypeResolution resolution,
                                           IdentTypeRepr *repr,
                                           DeclContext *typeDC) {
  ASTContext &ctx = resolution.getASTContext();
  auto &diags = ctx.Diags;

  auto *selfNominal = typeDC->getSelfNominalTypeDecl();
  auto declaredType = selfNominal->getDeclaredType();

  diags.diagnose(repr->getNameLoc(), diag::cannot_specialize_self);

  if (selfNominal->isGeneric() && !isa<ProtocolDecl>(selfNominal)) {
    diags
        .diagnose(repr->getNameLoc(), diag::specialize_explicit_type_instead,
                  declaredType)
        .fixItReplace(repr->getNameLoc().getSourceRange(),
                      declaredType.getString());
  }
}

/// Resolve the given identifier type representation as an unqualified type,
/// returning the type it references.
/// \param silContext Used to look up generic parameters in SIL mode.
///
/// \returns Either the resolved type or a null type, the latter of
/// which indicates that some dependencies were unsatisfied.
static Type resolveUnqualifiedIdentTypeRepr(TypeResolution resolution,
                                            SILTypeResolutionContext *silContext,
                                            IdentTypeRepr *repr) {
  const auto options = resolution.getOptions();
  ASTContext &ctx = resolution.getASTContext();
  auto &diags = ctx.Diags;

  // Short-circuiting.
  if (repr->isInvalid()) return ErrorType::get(ctx);

  // If the representation has already been bound to a declaration, handle
  // that now.
  if (auto *typeDecl = repr->getBoundDecl()) {
    // Resolve the type declaration within this context.
    return resolveTypeDecl(typeDecl, repr->getDeclContext(), resolution,
                           silContext, repr);
  }

  // Resolve the representation using unqualified name lookup.
  auto DC = resolution.getDeclContext();
  auto id = repr->getNameRef();

  // In SIL mode, we bind generic parameters here, since name lookup
  // won't find them.
  if (silContext && silContext->GenericParams) {
    auto name = id.getBaseIdentifier();
    if (auto *paramDecl = silContext->GenericParams->lookUpGenericParam(name)) {
      repr->setValue(paramDecl, DC);

      return resolveTypeDecl(paramDecl, DC, resolution, silContext, repr);
    }
  }

  NameLookupOptions lookupOptions = defaultUnqualifiedLookupOptions;
  if (options.contains(TypeResolutionFlags::AllowUsableFromInline))
    lookupOptions |= NameLookupFlags::IncludeUsableFromInline;
  auto globals =
      TypeChecker::lookupUnqualifiedType(DC, id, repr->getLoc(), lookupOptions);

  // If we're doing structural resolution and one of the results is an
  // associated type, ignore any other results found from the same
  // DeclContext; they are going to be protocol typealiases, possibly
  // from constrained extensions, and trying to compute their type in
  // resolveTypeInContext() might hit request cycles since structural
  // resolution is performed while computing the requirement signature
  // of the protocol.
  DeclContext *assocTypeDC = nullptr;
  if (resolution.getStage() == TypeResolutionStage::Structural) {
    for (const auto &entry : globals) {
      if (isa<AssociatedTypeDecl>(entry.getValueDecl())) {
        assocTypeDC = entry.getDeclContext();
        break;
      }
    }
  }

  // Process the names we found.
  Type current;
  TypeDecl *currentDecl = nullptr;
  DeclContext *currentDC = nullptr;
  bool isAmbiguous = false;
  for (const auto &entry : globals) {
    auto *foundDC = entry.getDeclContext();
    auto *typeDecl = cast<TypeDecl>(entry.getValueDecl());

    // See the comment above.
    if (assocTypeDC != nullptr &&
        foundDC == assocTypeDC && !isa<AssociatedTypeDecl>(typeDecl))
      continue;

    // Compute the type of the found declaration when referenced from this
    // location.
    Type type = resolveTypeDecl(typeDecl, foundDC, resolution, silContext, repr);
    if (type->is<ErrorType>())
      return type;

    // If this is the first result we found, record it.
    if (current.isNull()) {
      current = type;
      currentDecl = typeDecl;
      currentDC = foundDC;
      continue;
    }

    // Otherwise, check for an ambiguity.
    if (!resolution.areSameType(current, type)) {
      isAmbiguous = true;
      break;
    }

    // We have a found multiple type aliases that refer to the same thing.
    // Ignore the duplicate.
  }

  // Complain about any ambiguities we detected.
  // FIXME: We could recover by looking at later components.
  if (isAmbiguous) {
    if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
      diags
          .diagnose(repr->getNameLoc(), diag::ambiguous_type_base,
                    repr->getNameRef())
          .highlight(repr->getNameLoc().getSourceRange());
      for (auto entry : globals) {
        entry.getValueDecl()->diagnose(diag::found_candidate);
      }
    }

    repr->setInvalid();
    return ErrorType::get(ctx);
  }

  // If we found a type declaration with the given name, return it now.
  if (current) {
    repr->setValue(currentDecl, currentDC);
    return current;
  }

  // 'Self' inside of a nominal type refers to that type.
  if (id.isSimpleName(ctx.Id_Self)) {
    if (auto *typeDC = DC->getInnermostTypeContext()) {
      // FIXME: The passed-in TypeRepr should get 'typechecked' as well.
      // The issue is though that IdentTypeRepr only accepts a ValueDecl
      // while the 'Self' type is more than just a reference to a TypeDecl.
      auto selfType = typeDC->getSelfInterfaceType();

      // Check if we can reference 'Self' here, and if so, what kind of Self it is.
      auto selfTypeKind = getSelfTypeKind(DC, options);

      // We don't allow generic arguments on 'Self'.
      if (selfTypeKind != SelfTypeKind::InvalidSelf &&
          isa<GenericIdentTypeRepr>(repr)) {
        diagnoseGenericArgumentsOnSelf(resolution, repr, typeDC);
      }

      switch (selfTypeKind) {
      case SelfTypeKind::StaticSelf:
        return selfType;
      case SelfTypeKind::DynamicSelf:
        return DynamicSelfType::get(selfType, ctx);
      case SelfTypeKind::InvalidSelf:
        break;
      }
    }
  }

  // If we're not allowed to complain, bail out.
  if (options.contains(TypeResolutionFlags::SilenceErrors))
    return ErrorType::get(ctx);

  // Complain and give ourselves a chance to recover.
  return diagnoseUnknownType(resolution, nullptr, SourceRange(), repr,
                             lookupOptions);
}

static void diagnoseAmbiguousMemberType(Type baseTy, SourceRange baseRange,
                                        DeclNameRef name, DeclNameLoc nameLoc,
                                        LookupTypeResult &lookup) {
  ASTContext &ctx = baseTy->getASTContext();
  auto &diags = ctx.Diags;
  if (auto moduleTy = baseTy->getAs<ModuleType>()) {
    diags.diagnose(nameLoc, diag::ambiguous_module_type, name,
                   moduleTy->getModule()->getName())
      .highlight(baseRange);
  } else {
    diags.diagnose(nameLoc, diag::ambiguous_member_type, name, baseTy)
      .highlight(baseRange);
  }
  for (const auto &member : lookup) {
    member.Member->diagnose(diag::found_candidate_type, member.MemberType);
  }
}

/// Resolve the given identifier type representation as a qualified
/// lookup within the given parent type, returning the type it
/// references.
/// \param silContext Used to look up generic parameters in SIL mode.
static Type resolveQualifiedIdentTypeRepr(TypeResolution resolution,
                                          SILTypeResolutionContext *silContext,
                                          Type parentTy,
                                          SourceRange parentRange,
                                          IdentTypeRepr *repr) {
  const auto options = resolution.getOptions();
  auto DC = resolution.getDeclContext();
  auto &ctx = DC->getASTContext();
  auto &diags = ctx.Diags;
  auto isExtensionBinding = options.is(TypeResolverContext::ExtensionBinding);

  auto maybeDiagnoseBadMemberType = [&](TypeDecl *member, Type memberType,
                                        AssociatedTypeDecl *inferredAssocType) {
    bool hasUnboundOpener = !!resolution.getUnboundTypeOpener();

    // Type aliases might require adjustment due to @preconcurrency.
    if (auto aliasDecl = dyn_cast<TypeAliasDecl>(member)) {
      memberType = adjustTypeAliasTypeInContext(
          memberType, aliasDecl, resolution.getDeclContext(),
          resolution.getOptions());
    }

    if (options.contains(TypeResolutionFlags::SilenceErrors)) {
      if (TypeChecker::isUnsupportedMemberTypeAccess(
              parentTy, member, hasUnboundOpener, isExtensionBinding) !=
          TypeChecker::UnsupportedMemberTypeAccessKind::None)
        return ErrorType::get(ctx);
    }

    switch (TypeChecker::isUnsupportedMemberTypeAccess(
        parentTy, member, hasUnboundOpener, isExtensionBinding)) {
    case TypeChecker::UnsupportedMemberTypeAccessKind::None:
      break;

    case TypeChecker::UnsupportedMemberTypeAccessKind::NominalTypeOfUnboundGeneric:
    case TypeChecker::UnsupportedMemberTypeAccessKind::TypeAliasOfUnboundGeneric:
    case TypeChecker::UnsupportedMemberTypeAccessKind::AssociatedTypeOfUnboundGeneric:
      diagnoseUnboundGenericType(parentTy, parentRange.End);
      return ErrorType::get(ctx);

    case TypeChecker::UnsupportedMemberTypeAccessKind::TypeAliasOfExistential:
      diags.diagnose(repr->getNameLoc(), diag::typealias_outside_of_protocol,
                     repr->getNameRef(), parentTy);
      return ErrorType::get(ctx);

    case TypeChecker::UnsupportedMemberTypeAccessKind::AssociatedTypeOfExistential:
      diags.diagnose(repr->getNameLoc(), diag::assoc_type_outside_of_protocol,
                     repr->getNameRef(), parentTy);
      return ErrorType::get(ctx);
    }

    // Only the last component of the underlying type of a type alias may
    // be an unbound generic.
    if (options.is(TypeResolverContext::TypeAliasDecl)) {
      if (parentTy->is<UnboundGenericType>()) {
        if (!options.contains(TypeResolutionFlags::SilenceErrors))
          diagnoseUnboundGenericType(parentTy, parentRange.End);

        return ErrorType::get(ctx);
      }
    }

    // Diagnose a bad conformance reference if we need to.
    if (!options.contains(TypeResolutionFlags::SilenceErrors) &&
        inferredAssocType && memberType->hasError()) {
      maybeDiagnoseBadConformanceRef(DC, parentTy, repr->getLoc(),
                                     inferredAssocType);
    }

    // If there are generic arguments, apply them now.
    return applyGenericArguments(memberType, resolution, silContext, repr);
  };

  // Short-circuiting.
  if (repr->isInvalid()) return ErrorType::get(ctx);

  // If the parent is a type parameter, the member is a dependent member,
  // and we skip much of the work below.
  if (parentTy->isTypeParameter()) {
    if (auto memberType = resolution.resolveDependentMemberType(
            parentTy, DC, parentRange, repr)) {
      // Hack -- if we haven't resolved this to a declaration yet, don't
      // attempt to apply generic arguments, since this will emit a
      // diagnostic, and its possible that this type will become a concrete
      // type later on.
      if (!memberType->is<DependentMemberType>() ||
          memberType->castTo<DependentMemberType>()->getAssocType()) {
        return applyGenericArguments(memberType, resolution, silContext, repr);
      }

      return memberType;
    }
  }

  // Phase 2: If a declaration has already been bound, use it.
  if (auto *typeDecl = repr->getBoundDecl()) {
    auto memberType =
      TypeChecker::substMemberTypeWithBase(DC->getParentModule(), typeDecl,
                                           parentTy);
    return maybeDiagnoseBadMemberType(typeDecl, memberType, nullptr);
  }

  // Phase 1: Find and bind the type declaration.

  // Look for member types with the given name.
  NameLookupOptions lookupOptions = defaultMemberLookupOptions;
  if (options.contains(TypeResolutionFlags::AllowUsableFromInline))
    lookupOptions |= NameLookupFlags::IncludeUsableFromInline;
  LookupTypeResult memberTypes;
  if (parentTy->mayHaveMembers())
    memberTypes = TypeChecker::lookupMemberType(
        DC, parentTy, repr->getNameRef(), repr->getLoc(), lookupOptions);

  // Name lookup was ambiguous. Complain.
  // FIXME: Could try to apply generic arguments first, and see whether
  // that resolves things. But do we really want that to succeed?
  if (memberTypes.size() > 1) {
    if (!options.contains(TypeResolutionFlags::SilenceErrors))
      diagnoseAmbiguousMemberType(parentTy, parentRange, repr->getNameRef(),
                                  repr->getNameLoc(), memberTypes);
    return ErrorType::get(ctx);
  }

  // If we didn't find anything, complain.
  Type memberType;
  TypeDecl *member = nullptr;
  AssociatedTypeDecl *inferredAssocType = nullptr;
  if (!memberTypes) {
    // If we're not allowed to complain or we couldn't fix the
    // source, bail out.
    if (options.contains(TypeResolutionFlags::SilenceErrors))
      return ErrorType::get(ctx);

    memberType = diagnoseUnknownType(resolution, parentTy, parentRange, repr,
                                     lookupOptions);
    member = repr->getBoundDecl();
    if (!member)
      return ErrorType::get(ctx);
  } else {
    memberType = memberTypes.back().MemberType;
    member = memberTypes.back().Member;
    inferredAssocType = memberTypes.back().InferredAssociatedType;
    repr->setValue(member, nullptr);
  }

  return maybeDiagnoseBadMemberType(member, memberType, inferredAssocType);
}

// Hack to apply context-specific @escaping to an AST function type.
static Type applyNonEscapingIfNecessary(Type ty,
                                        TypeResolutionOptions options) {
  // Remember whether this is a function parameter.
  bool defaultNoEscape = options.is(TypeResolverContext::FunctionInput) &&
                         !options.hasBase(TypeResolverContext::EnumElementDecl);

  // Desugar here
  auto *funcTy = ty->castTo<FunctionType>();
  auto extInfo = funcTy->getExtInfo();
  if (defaultNoEscape && !extInfo.isNoEscape()) {
    extInfo = extInfo.withNoEscape();

    // We lost the sugar to flip the isNoEscape bit.
    //
    // FIXME(https://github.com/apple/swift/issues/45125): It would be better
    // to add a new AttributedType sugared type, which would wrap the
    // TypeAliasType and apply the isNoEscape bit when de-sugaring.
    return FunctionType::get(funcTy->getParams(), funcTy->getResult(), extInfo);
  }

  // Note: original sugared type
  return ty;
}

/// Validate whether type associated with @autoclosure attribute is correct,
/// it supposed to be a function type with no parameters.
/// \returns true if there was an error, false otherwise.
static bool validateAutoClosureAttr(DiagnosticEngine &Diags, const SourceLoc &loc,
                                    Type paramType) {
  if (auto *fnType = paramType->getAs<FunctionType>()) {
    if (fnType->getNumParams() != 0) {
      Diags.diagnose(loc, diag::autoclosure_function_input_nonunit);
      return true;
    }
    // A function type with no parameters.
    return false;
  }

  Diags.diagnose(loc, diag::autoclosure_function_type);
  return true;
}

/// Check whether the type associated with particular source location
/// has `@autoclosure` attribute, and if so, validate that such use is correct.
/// \returns true if there was an error, false otherwise.
static bool validateAutoClosureAttributeUse(DiagnosticEngine &Diags,
                                            const TypeRepr *TR,
                                            Type type,
                                            TypeResolutionOptions options) {
  if (!TR || TR->isInvalid())
    return false;

  // If is a parameter declaration marked as @autoclosure.
  if (options.is(TypeResolverContext::FunctionInput)) {
    if (auto *ATR = dyn_cast<AttributedTypeRepr>(TR)) {
      const auto attrLoc = ATR->getAttrs().getLoc(TAK_autoclosure);
      if (attrLoc.isValid())
        return validateAutoClosureAttr(Diags, attrLoc, type);
    }
  }

  // Otherwise, let's dig into the type and see if there are any
  // functions with parameters marked as @autoclosure,
  // such would be a part of expressions like:
  // `let _: (@autoclosure () -> Int) -> Void = ...`.
  bool isValid = true;
  type.visit([&](Type subType) {
    if (auto *fnType = subType->getAs<FunctionType>()) {
      isValid &= llvm::none_of(
          fnType->getParams(), [&](const FunctionType::Param &param) {
            return param.isAutoClosure() &&
                   validateAutoClosureAttr(Diags, TR->getLoc(),
                                           param.getPlainType());
          });
    }
  });

  return !isValid;
}

namespace {
  const auto DefaultParameterConvention = ParameterConvention::Direct_Unowned;
  const auto DefaultResultConvention = ResultConvention::Unowned;

  /// A wrapper that ensures that the returned type from
  /// \c TypeResolver::resolveType is never the null \c Type. It otherwise
  /// tries to behave like \c Type, so it provides the proper conversion and
  /// arrow operators.
  class NeverNullType final {
  public:
    /// Forbid default construction.
    NeverNullType() = delete;
    /// Forbid construction from \c nullptr.
    NeverNullType(std::nullptr_t) = delete;

  public:
    /// Construct a never-null Type. If \p Ty is null, a fatal error is thrown.
    NeverNullType(Type Ty) : WrappedTy(Ty) {
      if (WrappedTy.isNull()) {
        llvm::report_fatal_error("Resolved to null type!");
      }
    }

    /// Construct a never-null Type. If \p TyB is null, a fatal error is thrown.
    NeverNullType(TypeBase *TyB) : NeverNullType(Type(TyB)) {}

    operator Type() const { return WrappedTy; }
    Type get() const { return WrappedTy; }

    TypeBase *operator->() const { return WrappedTy.operator->(); }

  private:
    Type WrappedTy;
  };

  class TypeResolver {
    const TypeResolution &resolution;

    /// Used in SIL mode.
    SILTypeResolutionContext *silContext;

  public:
    explicit TypeResolver(const TypeResolution &resolution,
                          SILTypeResolutionContext *silContext = nullptr)
        : resolution(resolution), silContext(silContext) {}

    NeverNullType resolveType(TypeRepr *repr, TypeResolutionOptions options);

  private:
    ASTContext &getASTContext() const { return resolution.getASTContext(); }
    DeclContext *getDeclContext() { return resolution.getDeclContext(); }
    const DeclContext *getDeclContext() const {
      return resolution.getDeclContext();
    }

  private:
    template<typename ...ArgTypes>
    InFlightDiagnostic diagnose(ArgTypes &&...Args) const {
      auto &diags = getASTContext().Diags;
      return diags.diagnose(std::forward<ArgTypes>(Args)...);
    }

    template <typename... ArgTypes>
    InFlightDiagnostic diagnoseInvalid(TypeRepr *repr,
                                       ArgTypes &&... Args) const {
      auto &diags = getASTContext().Diags;
      repr->setInvalid();
      return diags.diagnose(std::forward<ArgTypes>(Args)...);
    }

    bool diagnoseMoveOnlyGeneric(TypeRepr *repr,
                                 Type unboundTy, Type genericArgTy);
    bool diagnoseMoveOnlyMissingOwnership(TypeRepr *repr,
                                          TypeResolutionOptions options);
    
    bool diagnoseDisallowedExistential(TypeRepr *repr);
    
    bool diagnoseInvalidPlaceHolder(OpaqueReturnTypeRepr *repr);

    NeverNullType resolveOpenedExistentialArchetype(
        TypeAttributes &attrs, TypeRepr *repr,
        TypeResolutionOptions options);

    NeverNullType resolvePackElementArchetype(
        TypeAttributes &attrs, TypeRepr *repr,
        TypeResolutionOptions options);

    NeverNullType resolveAttributedType(AttributedTypeRepr *repr,
                                        TypeResolutionOptions options);
    NeverNullType resolveAttributedType(TypeAttributes &attrs, TypeRepr *repr,
                                        TypeResolutionOptions options);
    NeverNullType
    resolveASTFunctionType(FunctionTypeRepr *repr,
                           TypeResolutionOptions options,
                           AnyFunctionType::Representation representation =
                               AnyFunctionType::Representation::Swift,
                           bool noescape = false,
                           bool concurrent = false,
                           const clang::Type *parsedClangFunctionType = nullptr,
                           DifferentiabilityKind diffKind =
                               DifferentiabilityKind::NonDifferentiable,
                           Type globalActor = Type());
    SmallVector<AnyFunctionType::Param, 8>
    resolveASTFunctionTypeParams(TupleTypeRepr *inputRepr,
                                 TypeResolutionOptions options,
                                 DifferentiabilityKind diffKind);

    NeverNullType resolveSILFunctionType(
        FunctionTypeRepr *repr, TypeResolutionOptions options,
        SILCoroutineKind coroutineKind = SILCoroutineKind::None,
        SILFunctionType::ExtInfoBuilder extInfoBuilder =
            SILFunctionType::ExtInfoBuilder(),
        ParameterConvention calleeConvention = DefaultParameterConvention,
        TypeRepr *witnessmethodProtocol = nullptr);
    SILParameterInfo resolveSILParameter(TypeRepr *repr,
                                         TypeResolutionOptions options);
    SILYieldInfo resolveSILYield(TypeAttributes &remainingAttrs,
                                 TypeRepr *repr, TypeResolutionOptions options);
    bool resolveSILResults(TypeRepr *repr, TypeResolutionOptions options,
                           SmallVectorImpl<SILYieldInfo> &yields,
                           SmallVectorImpl<SILResultInfo> &results,
                           llvm::Optional<SILResultInfo> &errorResult);
    bool resolveSingleSILResult(TypeRepr *repr, TypeResolutionOptions options,
                                SmallVectorImpl<SILYieldInfo> &yields,
                                SmallVectorImpl<SILResultInfo> &results,
                                llvm::Optional<SILResultInfo> &errorResult);
    NeverNullType resolveDeclRefTypeRepr(DeclRefTypeRepr *repr,
                                         TypeResolutionOptions options);
    NeverNullType resolveOwnershipTypeRepr(OwnershipTypeRepr *repr,
                                           TypeResolutionOptions options);
    NeverNullType resolveIsolatedTypeRepr(IsolatedTypeRepr *repr,
                                          TypeResolutionOptions options);
    NeverNullType resolveCompileTimeConstTypeRepr(CompileTimeConstTypeRepr *repr,
                                                  TypeResolutionOptions options);
    NeverNullType resolveArrayType(ArrayTypeRepr *repr,
                                   TypeResolutionOptions options);
    NeverNullType resolveDictionaryType(DictionaryTypeRepr *repr,
                                        TypeResolutionOptions options);
    NeverNullType resolveOptionalType(OptionalTypeRepr *repr,
                                      TypeResolutionOptions options);
    NeverNullType resolveImplicitlyUnwrappedOptionalType(
        ImplicitlyUnwrappedOptionalTypeRepr *repr,
        TypeResolutionOptions options, bool isDirect);
    NeverNullType resolveVarargType(VarargTypeRepr *repr,
                                    TypeResolutionOptions options);
    NeverNullType resolvePackType(PackTypeRepr *repr,
                                  TypeResolutionOptions options,
                                  bool direct = false);
    NeverNullType resolvePackExpansionType(PackExpansionTypeRepr *repr,
                                           TypeResolutionOptions options);
    NeverNullType resolvePackElement(PackElementTypeRepr *repr,
                                     TypeResolutionOptions options);
    NeverNullType resolveTupleType(TupleTypeRepr *repr,
                                   TypeResolutionOptions options);
    NeverNullType resolveCompositionType(CompositionTypeRepr *repr,
                                         TypeResolutionOptions options);
    NeverNullType resolveExistentialType(ExistentialTypeRepr *repr,
                                         TypeResolutionOptions options);
    NeverNullType resolveMetatypeType(MetatypeTypeRepr *repr,
                                      TypeResolutionOptions options);
    NeverNullType resolveProtocolType(ProtocolTypeRepr *repr,
                                      TypeResolutionOptions options);
    NeverNullType resolveSILBoxType(SILBoxTypeRepr *repr,
                                    bool capturesGenerics,
                                    TypeResolutionOptions options);

    NeverNullType
    buildMetatypeType(MetatypeTypeRepr *repr, Type instanceType,
                      llvm::Optional<MetatypeRepresentation> storedRepr);
    NeverNullType
    buildProtocolType(ProtocolTypeRepr *repr, Type instanceType,
                      llvm::Optional<MetatypeRepresentation> storedRepr);

    NeverNullType resolveOpaqueReturnType(TypeRepr *repr, StringRef mangledName,
                                          unsigned ordinal,
                                          TypeResolutionOptions options);
  };

  /// A helper class to change to a new generic context in a scope when
  /// parsing SIL.
  class SILInnerGenericContextRAII {
    SILTypeResolutionContext *silContext;
    GenericParamList *savedParams;

  public:
    SILInnerGenericContextRAII(SILTypeResolutionContext *silContext,
                               GenericParamList *newParams)
      : silContext(silContext),
        savedParams(silContext->GenericParams) {
      silContext->GenericParams = newParams;
    }

    SILInnerGenericContextRAII(const SILInnerGenericContextRAII &) = delete;
    SILInnerGenericContextRAII &operator=(const SILInnerGenericContextRAII &) = delete;

    ~SILInnerGenericContextRAII() {
      silContext->GenericParams = savedParams;
    }
  };
} // end anonymous namespace

Type TypeResolution::resolveContextualType(
    TypeRepr *TyR, DeclContext *dc, TypeResolutionOptions opts,
    OpenUnboundGenericTypeFn unboundTyOpener,
    HandlePlaceholderTypeReprFn placeholderHandler,
    OpenPackElementFn packElementOpener,
    SILTypeResolutionContext *silContext) {
  return resolveContextualType(TyR, dc, dc->getGenericSignatureOfContext(),
                               opts, unboundTyOpener, placeholderHandler,
                               packElementOpener, silContext);
}

Type TypeResolution::resolveContextualType(
    TypeRepr *TyR, DeclContext *dc, GenericSignature genericSig,
    TypeResolutionOptions opts, OpenUnboundGenericTypeFn unboundTyOpener,
    HandlePlaceholderTypeReprFn placeholderHandler,
    OpenPackElementFn packElementOpener,
    SILTypeResolutionContext *silContext) {
  const auto resolution = TypeResolution::forInterface(
      dc, genericSig, opts, unboundTyOpener, placeholderHandler,
      packElementOpener);
  const auto ty = resolution.resolveType(TyR, silContext);

  return GenericEnvironment::mapTypeIntoContext(
      resolution.getGenericSignature().getGenericEnvironment(), ty);
}

static Type evaluateTypeResolution(const TypeResolution *resolution,
                                   TypeRepr *TyR,
                                   SILTypeResolutionContext *silContext);

Type TypeResolution::resolveType(TypeRepr *TyR,
                                 SILTypeResolutionContext *silContext) const {
  // Bypass the request evaluator if we have a SIL context.  SIL type
  // resolution doesn't obey the functional laws of request evaluation
  // (resolution contexts can be mutable, e.g. to handle local archetypes),
  // and the SIL parser doesn't benefit from either cycle-checking
  // (since SIL functions can't declare types) or request caching (since
  // the SIL parser throws away TypeReprs as soon as it resolves them).
  //
  // This also has the mild benefit of reducing the storage overhead of
  // type resolution caching.
  if (silContext) {
    return evaluateTypeResolution(this, TyR, silContext);
  }

  auto &ctx = getASTContext();
  auto Ty =
      evaluateOrDefault(ctx.evaluator,
                        ResolveTypeRequest{this, TyR}, Type());
  if (!Ty)
    return ErrorType::get(ctx);
  return Ty;
}

Type ResolveTypeRequest::evaluate(Evaluator &evaluator,
                                  const TypeResolution *resolution,
                                  TypeRepr *TyR) const {
  return evaluateTypeResolution(resolution, TyR, /*silContext*/ nullptr);
}

static Type evaluateTypeResolution(const TypeResolution *resolution,
                                   TypeRepr *TyR,
                                   SILTypeResolutionContext *silContext) {
  const auto options = resolution->getOptions();
  auto &ctx = resolution->getASTContext();
  auto result =
      TypeResolver(*resolution, silContext)
          .resolveType(TyR, resolution->getOptions());

  // If we resolved down to an error, make sure to mark the typeRepr as invalid
  // so we don't produce a redundant diagnostic.
  if (result->hasError()) {
    TyR->setInvalid();
    return result;
  }

  auto loc = TyR->getLoc();

  if (options.contains(TypeResolutionFlags::SILType)
      && !result->isLegalSILType()) {
    ctx.Diags.diagnose(loc, diag::illegal_sil_type, result);
    return ErrorType::get(ctx);
  }

  if (validateAutoClosureAttributeUse(ctx.Diags, TyR, result, options))
    return ErrorType::get(ctx);

  return result;
}

bool TypeResolver::diagnoseDisallowedExistential(TypeRepr *repr) {
  auto options = resolution.getOptions();
  if (!(options & TypeResolutionFlags::SilenceErrors) &&
      options.contains(TypeResolutionFlags::DisallowOpaqueTypes)) {
    // We're specifically looking at an existential type `any P<some Q>`,
    // so emit a tailored diagnostic. We don't emit an ErrorType here
    // for better recovery.
    diagnose(repr->getLoc(),
             diag::unsupported_opaque_type_in_existential);
    // FIXME: We shouldn't have to invalid the type repr here, but not
    // doing so causes a double-diagnostic.
    repr->setInvalid();
    return true;
  } else {
    return false;
  }
}

bool TypeResolver::diagnoseInvalidPlaceHolder(OpaqueReturnTypeRepr *repr) {
  if (repr->getConstraint()->isInvalid()){
    if (isa<PlaceholderTypeRepr>(repr->getConstraint()))
      return true;
  }
  return false;
}

/// Checks the given type, assuming that it appears as an argument for a
/// generic parameter in the \c repr, to see if it is move-only.
///
/// Because generic type parameters currently all assume copyability of
/// the substituted type, it's an error for a move-only type to appear
/// as an argument for type parameters.
///
/// returns true if an error diagnostic was emitted
bool TypeResolver::diagnoseMoveOnlyGeneric(TypeRepr *repr,
                                           Type unboundTy,
                                           Type genericArgTy) {
  if (genericArgTy->isPureMoveOnly()) {
    if (unboundTy) {
      diagnoseInvalid(repr, repr->getLoc(), diag::noncopyable_generics_specific,
                      genericArgTy, unboundTy);
    } else {
      diagnoseInvalid(repr, repr->getLoc(), diag::noncopyable_generics,
                      genericArgTy);
    }
    return true;
  }
  return false;
}

/// Assuming this repr has resolved to a move-only / noncopyable type, checks
/// to see if that resolution happened in a context requiring an ownership
/// annotation. If it did and there was no ownership specified, emits a
/// diagnostic.
///
/// \returns true if an error diagnostic was emitted
bool TypeResolver::diagnoseMoveOnlyMissingOwnership(
                                              TypeRepr *repr,
                                              TypeResolutionOptions options) {
  // Though this is only required on function inputs... we can ignore
  // InoutFunctionInput since it's already got ownership.
  if (!options.is(TypeResolverContext::FunctionInput))
    return false;

  // Enum cases don't need to specify ownership for associated values
  if (options.hasBase(TypeResolverContext::EnumElementDecl))
    return false;

  // Otherwise, we require ownership.
  if (options.contains(TypeResolutionFlags::HasOwnership))
    return false;

  // Don't diagnose in SIL; ownership is already required there.
  if (options.contains(TypeResolutionFlags::SILType))
    return false;

  //////////////////
  // At this point, we know we have a noncopyable parameter that is missing an
  // ownership specifier, so we need to emit an error

  // We don't yet support any ownership specifiers for parameters of subscript
  // decls, give a tailored error message saying you simply can't use a
  // noncopyable type here.
  if (options.hasBase(TypeResolverContext::SubscriptDecl)) {
    diagnose(repr->getLoc(), diag::noncopyable_parameter_subscript_unsupported);
  } else {
    // general error diagnostic
    diagnose(repr->getLoc(),
             diag::noncopyable_parameter_requires_ownership);

    diagnose(repr->getLoc(), diag::noncopyable_parameter_ownership_suggestion,
             "borrowing", "for an immutable reference")
        .fixItInsert(repr->getStartLoc(), "borrowing ");

    diagnose(repr->getLoc(), diag::noncopyable_parameter_ownership_suggestion,
             "inout", "for a mutable reference")
        .fixItInsert(repr->getStartLoc(), "inout ");

    diagnose(repr->getLoc(), diag::noncopyable_parameter_ownership_suggestion,
             "consuming", "to take the value from the caller")
        .fixItInsert(repr->getStartLoc(), "consuming ");
  }

  // to avoid duplicate diagnostics
  repr->setInvalid();
  return true;
}

NeverNullType TypeResolver::resolveType(TypeRepr *repr,
                                        TypeResolutionOptions options) {
  assert(repr && "Cannot validate null TypeReprs!");

  // If we know the type representation is invalid, just return an
  // error type.
  if (repr->isInvalid())
    return ErrorType::get(getASTContext());

  // Strip the "is function input" bits unless this is a type that knows about
  // them.
  if (options.is(TypeResolverContext::FunctionInput) &&
      !isa<SpecifierTypeRepr>(repr) && !isa<TupleTypeRepr>(repr) &&
      !isa<AttributedTypeRepr>(repr) && !isa<FunctionTypeRepr>(repr) &&
      !isa<DeclRefTypeRepr>(repr) && !isa<PackExpansionTypeRepr>(repr) &&
      !isa<ImplicitlyUnwrappedOptionalTypeRepr>(repr)) {
    options.setContext(llvm::None);
  }

  bool isDirect = false;
  if ((options & TypeResolutionFlags::Direct) && !isa<SpecifierTypeRepr>(repr)){
    isDirect = true;
    options -= TypeResolutionFlags::Direct;
  }

  switch (repr->getKind()) {
  case TypeReprKind::Error:
    return ErrorType::get(getASTContext());

  case TypeReprKind::Attributed:
    return resolveAttributedType(cast<AttributedTypeRepr>(repr), options);
  case TypeReprKind::Ownership:
    return resolveOwnershipTypeRepr(cast<OwnershipTypeRepr>(repr), options);
  case TypeReprKind::Isolated:
    return resolveIsolatedTypeRepr(cast<IsolatedTypeRepr>(repr), options);
  case TypeReprKind::CompileTimeConst:
      return resolveCompileTimeConstTypeRepr(cast<CompileTimeConstTypeRepr>(repr),
                                             options);
  case TypeReprKind::SimpleIdent:
  case TypeReprKind::GenericIdent:
  case TypeReprKind::Member: {
      return resolveDeclRefTypeRepr(cast<DeclRefTypeRepr>(repr), options);
  }

  case TypeReprKind::Function: {
    if (!(options & TypeResolutionFlags::SILType)) {
      // Default non-escaping for closure parameters
      auto result =
          resolveASTFunctionType(cast<FunctionTypeRepr>(repr), options);
      if (result->is<FunctionType>())
        return applyNonEscapingIfNecessary(result, options);
      return result;
    }
    return resolveSILFunctionType(cast<FunctionTypeRepr>(repr), options);
  }
  case TypeReprKind::SILBox:
    assert((options & TypeResolutionFlags::SILType) && "SILBox repr in non-SIL type context?!");
    return resolveSILBoxType(cast<SILBoxTypeRepr>(repr),
                             /*captures generics*/ false,
                             options);

  case TypeReprKind::Array:
    return resolveArrayType(cast<ArrayTypeRepr>(repr), options);

  case TypeReprKind::Dictionary:
    return resolveDictionaryType(cast<DictionaryTypeRepr>(repr), options);

  case TypeReprKind::Optional:
    return resolveOptionalType(cast<OptionalTypeRepr>(repr), options);

  case TypeReprKind::ImplicitlyUnwrappedOptional: {
    auto iuoRepr = cast<ImplicitlyUnwrappedOptionalTypeRepr>(repr);
    return resolveImplicitlyUnwrappedOptionalType(iuoRepr, options, isDirect);
  }

  case TypeReprKind::Vararg:
    return resolveVarargType(cast<VarargTypeRepr>(repr), options);

  case TypeReprKind::Pack:
    return resolvePackType(cast<PackTypeRepr>(repr), options);

  case TypeReprKind::PackExpansion:
    return resolvePackExpansionType(cast<PackExpansionTypeRepr>(repr), options);

  case TypeReprKind::PackElement:
    return resolvePackElement(cast<PackElementTypeRepr>(repr), options);

  case TypeReprKind::Tuple:
    return resolveTupleType(cast<TupleTypeRepr>(repr), options);

    case TypeReprKind::Composition: {
      auto *DC = getDeclContext();
      if (getASTContext().LangOpts.hasFeature(Feature::ImplicitSome)) {
        if (auto opaqueDecl = dyn_cast<OpaqueTypeDecl>(DC)) {
          if (auto ordinal = opaqueDecl->getAnonymousOpaqueParamOrdinal(repr)){
            diagnoseDisallowedExistential(repr);
            return getOpaqueArchetypeIdentity(opaqueDecl, *ordinal);
          }
        }
      }

      return resolveCompositionType(cast<CompositionTypeRepr>(repr), options);
    }

  case TypeReprKind::Metatype:
    return resolveMetatypeType(cast<MetatypeTypeRepr>(repr), options);

  case TypeReprKind::Protocol:
    return resolveProtocolType(cast<ProtocolTypeRepr>(repr), options);

  case TypeReprKind::OpaqueReturn: {
    // If the opaque type is in a valid position, e.g. part of a function return
    // type, resolution should happen in the context of an `OpaqueTypeDecl`.
    // This decl is implicit in the source and is created in such contexts by
    // evaluation of an `OpaqueResultTypeRequest`.
    auto opaqueRepr = cast<OpaqueReturnTypeRepr>(repr);
    auto *DC = getDeclContext();
    
    bool isInExistential = diagnoseDisallowedExistential(opaqueRepr);
    bool hasInvalidPlaceholder = diagnoseInvalidPlaceHolder(opaqueRepr);
    
    if (auto opaqueDecl = dyn_cast<OpaqueTypeDecl>(DC)) {
      if (auto ordinal = opaqueDecl->getAnonymousOpaqueParamOrdinal(opaqueRepr)){
        return !isInExistential ? getOpaqueArchetypeIdentity(opaqueDecl, *ordinal)
                                : ErrorType::get(getASTContext());
      }
    }

    // Check whether any of the generic parameters in the context represents
    // this opaque type. If so, return that generic parameter.
    if (auto declDC = DC->getAsDecl()) {
      if (auto genericContext = declDC->getAsGenericContext()) {
        if (auto genericParams = genericContext->getGenericParams()) {
          for (auto genericParam : *genericParams) {
            if (genericParam->getOpaqueTypeRepr() == opaqueRepr)
              return genericParam->getDeclaredInterfaceType();
          }
        }
      }
    }
    if (!repr->isInvalid() && !hasInvalidPlaceholder){
      // We are not inside an `OpaqueTypeDecl`, so diagnose an error.
      if (!(options & TypeResolutionFlags::SilenceErrors)) {
        diagnose(opaqueRepr->getOpaqueLoc(),
                 diag::unsupported_opaque_type);
      }
    }
    // Try to resolve the constraint upper bound type as a placeholder.
    options |= TypeResolutionFlags::SilenceErrors;
    auto constraintType = resolveType(opaqueRepr->getConstraint(),
                                      options);

    return !constraintType->hasError() ? ErrorType::get(constraintType)
                                       : ErrorType::get(getASTContext());
  }

  case TypeReprKind::Existential: {
    auto *existential = cast<ExistentialTypeRepr>(repr);
    return resolveExistentialType(existential, options);
  }

  case TypeReprKind::NamedOpaqueReturn: {
    // If the named opaque result type is in a valid position, resolution
    // should happen in the context of an `OpaqueTypeDecl`. This decl is
    // implicit in the source and is created in such contexts by evaluation of
    // an `OpaqueResultTypeRequest`.
    auto opaqueRepr = cast<NamedOpaqueReturnTypeRepr>(repr);
    if (auto opaqueDecl = dyn_cast<OpaqueTypeDecl>(getDeclContext())) {
      // Resolve the base type within this context.

      // FIXME: Temporary hack to resolve an identifier type to one of the
      // generic parameters of the opaque return type. This should be subsumed
      // be proper name lookup, OF COURSE.
      if (auto simpleIdent = dyn_cast<SimpleIdentTypeRepr>(
              opaqueRepr->getBase())) {
        Identifier name = simpleIdent->getNameRef().getBaseIdentifier();
        if (auto gpDecl = opaqueRepr->getGenericParams()
                ->lookUpGenericParam(name)) {
          auto outerGenericSignature = opaqueDecl->getNamingDecl()
                                           ->getInnermostDeclContext()
                                           ->getGenericSignatureOfContext();

          SubstitutionMap subs;
          if (outerGenericSignature)
            subs = outerGenericSignature->getIdentitySubstitutionMap();

          return OpaqueTypeArchetypeType::get(
              opaqueDecl, gpDecl->getDeclaredInterfaceType(), subs);
        }
      }

      return resolveType(opaqueRepr->getBase(), options);
    }

    // We are not inside an `OpaqueTypeDecl`, so diagnose an error.
    if (!(options & TypeResolutionFlags::SilenceErrors)) {
      diagnose(repr->getStartLoc(), diag::unsupported_opaque_type);
    }

    return ErrorType::get(getASTContext());
  }

  case TypeReprKind::Placeholder: {
    auto &ctx = getASTContext();
    // Fill in the placeholder if there's an appropriate handler.
    if (const auto handlerFn = resolution.getPlaceholderHandler())
      if (const auto ty = handlerFn(ctx, cast<PlaceholderTypeRepr>(repr)))
        return ty;

    // Complain if we're allowed to and bail out with an error.
    if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
      ctx.Diags.diagnose(repr->getLoc(),
                         diag::placeholder_type_not_allowed);
    }

    return ErrorType::get(resolution.getASTContext());
  }

  case TypeReprKind::Fixed:
    return cast<FixedTypeRepr>(repr)->getType();

  case TypeReprKind::Self:
    return cast<SelfTypeRepr>(repr)->getType();
  }
  llvm_unreachable("all cases should be handled");
}

static Type rebuildWithDynamicSelf(ASTContext &Context, Type ty) {
  if (auto metatypeTy = ty->getAs<MetatypeType>()) {
    return MetatypeType::get(
        rebuildWithDynamicSelf(Context, metatypeTy->getInstanceType()),
        metatypeTy->getRepresentation());
  } else if (auto optionalTy = ty->getOptionalObjectType()) {
    return OptionalType::get(rebuildWithDynamicSelf(Context, optionalTy));
  } else {
    return DynamicSelfType::get(ty, Context);
  }
}

NeverNullType
TypeResolver::resolveAttributedType(AttributedTypeRepr *repr,
                                    TypeResolutionOptions options) {
  // Copy the attributes, since we're about to start hacking on them.
  TypeAttributes attrs = repr->getAttrs();
  assert(!attrs.empty());

  return resolveAttributedType(attrs, repr->getTypeRepr(), options);
}

/// In SIL, handle '@opened(UUID, constraintType) interfaceType',
/// which creates an opened archetype.
NeverNullType
TypeResolver::resolveOpenedExistentialArchetype(
    TypeAttributes &attrs, TypeRepr *repr,
    TypeResolutionOptions options) {
  assert(silContext);

  options.setContext(llvm::None);

  auto *dc = getDeclContext();
  auto &ctx = dc->getASTContext();

  // The interface type is the type wrapped by the attribute. Resolve it
  // with the fake <Self> generic parameter list uniquely stored in the
  // ASTContext, and use structural resolution to avoid querying the
  // DeclContext's generic signature, which is not the right signature
  // for this.
  auto structuralResolution = TypeResolution::forStructural(
      dc, options,
      /*unboundTyOpener*/ nullptr,
      /*placeholderHandler*/ nullptr,
      /*packElementOpener*/ nullptr);
  auto interfaceType = [&] {
    SILInnerGenericContextRAII scope(silContext,
                                     ctx.getSelfGenericParamList(dc));

    TypeResolver interfaceTypeResolver(structuralResolution, silContext);
    return interfaceTypeResolver.resolveType(repr, options);
  }();

  // The constraint type is stored inside the attribute. It is resolved
  // normally, as if it were written in the current context.
  auto constraintType = resolveType(attrs.getConstraintType(), options);

  Type archetypeType;
  if (!constraintType->isExistentialType()) {
    diagnoseInvalid(repr, attrs.getLoc(TAK_opened),
                    diag::opened_bad_constraint_type,
                    constraintType);

    archetypeType = ErrorType::get(constraintType->getASTContext());
  } else if (!interfaceType->isTypeParameter()) {
    diagnoseInvalid(repr, attrs.getLoc(TAK_opened),
                    diag::opened_bad_interface_type,
                    interfaceType);

    archetypeType = ErrorType::get(interfaceType->getASTContext());
  } {
    // The constraint type is written with respect to the surrounding
    // generic environment.
    constraintType = GenericEnvironment::mapTypeIntoContext(
               resolution.getGenericSignature().getGenericEnvironment(),
               constraintType);

    // The opened existential type is formed by mapping the interface type
    // into a new opened generic environment.
    archetypeType = OpenedArchetypeType::get(constraintType->getCanonicalType(),
                                             interfaceType,
                                             GenericSignature(),
                                             attrs.getOpenedID());
  }

  attrs.clearAttribute(TAK_opened);

  return archetypeType;
}

/// In SIL, handle '@pack_element(UUID) interfaceType',
/// which creates an opened archetype.
NeverNullType
TypeResolver::resolvePackElementArchetype(
    TypeAttributes &attrs, TypeRepr *repr,
    TypeResolutionOptions options) {
  assert(silContext);
  assert(attrs.has(TAK_pack_element));
  assert(attrs.OpenedID.has_value());

  attrs.clearAttribute(TAK_pack_element);

  auto dc = getDeclContext();
  auto &ctx = dc->getASTContext();

  const SILTypeResolutionContext::OpenedPackElement *entry = nullptr;
  if (const auto *openedPacksMap = silContext->OpenedPackElements) {
    auto it = openedPacksMap->find(*attrs.OpenedID);
    if (it != openedPacksMap->end()) {
      entry = &it->second;
    }
  }
  if (!entry) {
    diagnoseInvalid(repr, attrs.getLoc(TAK_pack_element),
                    diag::sil_pack_element_uuid_not_found);
    return ErrorType::get(ctx);
  }

  options.setContext(llvm::None);

  // The interface type is the type wrapped by the attribute. Resolve it
  // within the generic parameter list for the opened generic environment.
  // Use structural resolution to avoid querying the DeclContext's
  // generic signature, which is not the right signature for this.
  auto structuralResolution = TypeResolution::forStructural(
      dc, options,
      /*unboundTyOpener*/ nullptr,
      /*placeholderHandler*/ nullptr,
      /*packElementOpener*/ nullptr);
  auto interfaceType = [&] {
    SILInnerGenericContextRAII scope(silContext, entry->Params);

    TypeResolver interfaceTypeResolver(structuralResolution, silContext);
    return interfaceTypeResolver.resolveType(repr, options);
  }();

  if (!interfaceType->isTypeParameter()) {
    diagnoseInvalid(repr, attrs.getLoc(TAK_pack_element),
                    diag::opened_bad_interface_type,
                    interfaceType);

    return ErrorType::get(ctx);
  }

  // Map the interface type into the element context.
  auto archetypeType =
    entry->Environment->mapPackTypeIntoElementContext(interfaceType);
  if (archetypeType->hasError()) {
    diagnoseInvalid(repr, attrs.getLoc(TAK_pack_element),
                    diag::opened_bad_interface_type,
                    interfaceType);
  }
  return archetypeType;
}

/// \returns true iff the # of isolated params is > \c lowerBound
static bool hasMoreIsolatedParamsThan(FunctionTypeRepr* fnTy, unsigned lowerBound) {
  unsigned count = 0;
  for (auto arg : fnTy->getArgsTypeRepr()->getElements()) {
    if (isa<IsolatedTypeRepr>(arg.Type))
      count += 1;

    if (count > lowerBound)
      break;
  }

  return count > lowerBound;
}

NeverNullType
TypeResolver::resolveAttributedType(TypeAttributes &attrs, TypeRepr *repr,
                                    TypeResolutionOptions options) {
  // Convenience to grab the source range of a type attribute.
  auto getTypeAttrRangeWithAt = [](ASTContext &ctx, SourceLoc attrLoc) {
    return SourceRange(attrLoc, attrLoc.getAdvancedLoc(1));

  };

  // Remember whether this is a function parameter.
  bool isParam = options.is(TypeResolverContext::FunctionInput);

  // Remember whether this is a function result.
  bool isResult = options.is(TypeResolverContext::FunctionResult);

  // Remember whether this is a variadic function parameter.
  bool isVariadicFunctionParam =
      options.is(TypeResolverContext::VariadicFunctionInput) &&
      !options.hasBase(TypeResolverContext::EnumElementDecl);

  // SIL box types have an attribute to indicate when the box contains
  // the captured generic environment.
  if (auto box = dyn_cast<SILBoxTypeRepr>(repr)) {
    return resolveSILBoxType(box, attrs.has(TAK_captures_generics), options);
  }
  
  // Resolve global actor.
  CustomAttr *globalActorAttr = nullptr;
  Type globalActor;
  if (auto fnTy = dyn_cast<FunctionTypeRepr>(repr)) {
    auto foundGlobalActor = checkGlobalActorAttributes(
        repr->getLoc(), getDeclContext(),
        std::vector<CustomAttr *>(
          attrs.getCustomAttrs().begin(), attrs.getCustomAttrs().end()));
    if (foundGlobalActor) {
      globalActorAttr = foundGlobalActor->first;
      globalActor = resolveType(globalActorAttr->getTypeRepr(), options);
      if (globalActor->hasError())
        globalActor = Type();

      // make sure there is no `isolated` parameter in the type
      if (globalActorAttr->isValid()) {
        if (globalActor && hasMoreIsolatedParamsThan(fnTy, 0)) {
          diagnose(repr->getLoc(), diag::isolated_parameter_global_actor_type)
              .warnUntilSwiftVersion(6);
          globalActorAttr->setInvalid();
        }
      }
    }
  }

  // Diagnose custom attributes that haven't been processed yet.
  for (auto customAttr : attrs.getCustomAttrs()) {
    // If this was the global actor we matched, ignore it.
    if (globalActorAttr == customAttr) {
      Decl *decl = nullptr;
      if (getASTContext().LangOpts.isConcurrencyModelTaskToThread() &&
          (decl = getDeclContext()->getAsDecl()) &&
          !AvailableAttr::isUnavailable(decl))
        diagnose(customAttr->getLocation(),
                 diag::concurrency_task_to_thread_model_global_actor_annotation,
                 customAttr->getTypeRepr(), "task-to-thread concurrency model");
      continue;
    }

    // If this attribute was marked invalid, ignore it.
    if (customAttr->isInvalid())
      continue;

    // Diagnose the attribute, because we don't yet handle custom type
    // attributes.
    std::string typeName;
    if (auto typeRepr = customAttr->getTypeRepr()) {
      llvm::raw_string_ostream out(typeName);
      typeRepr->print(out);
    } else {
      typeName = customAttr->getType().getString();
    }

    diagnose(customAttr->getLocation(), diag::unknown_attribute, typeName);
    customAttr->setInvalid();
  }

  // The type we're working with, in case we want to build it differently
  // based on the attributes we see.
  Type ty;
  
  // If this is a reference to an opaque return type, resolve it.
  if (auto &opaque = attrs.OpaqueReturnTypeOf) {
    return resolveOpaqueReturnType(repr, opaque->mangledName, opaque->index,
                                   options);
  }
  
  // In SIL *only*, allow @thin, @thick, or @objc_metatype to apply to
  // a metatype.
  if (attrs.has(TAK_thin) || attrs.has(TAK_thick) || 
      attrs.has(TAK_objc_metatype)) {
    if (auto SF = getDeclContext()->getParentSourceFile()) {
      if (SF->Kind == SourceFileKind::SIL) {
        if (auto existential = dyn_cast<ExistentialTypeRepr>(repr))
          repr = existential->getConstraint();

        TypeRepr *base;
        if (auto metatypeRepr = dyn_cast<MetatypeTypeRepr>(repr)) {
          base = metatypeRepr->getBase();
        } else if (auto protocolRepr = dyn_cast<ProtocolTypeRepr>(repr)) {
          base = protocolRepr->getBase();
        } else {
          base = nullptr;
        }

        if (base) {
          llvm::Optional<MetatypeRepresentation> storedRepr;
          // The instance type is not a SIL type.
          auto instanceOptions = options;
          TypeResolverContext context = TypeResolverContext::None;
          if (isa<MetatypeTypeRepr>(repr)) {
            context = TypeResolverContext::MetatypeBase;
          } else if (isa<ProtocolTypeRepr>(repr)) {
            context = TypeResolverContext::ProtocolMetatypeBase;
          }
          instanceOptions.setContext(context);
          instanceOptions -= TypeResolutionFlags::SILType;

          auto instanceTy = resolveType(base, instanceOptions);
          if (instanceTy->hasError())
            return instanceTy;

          // Check for @thin.
          if (attrs.has(TAK_thin)) {
            storedRepr = MetatypeRepresentation::Thin;
            attrs.clearAttribute(TAK_thin);
          }

          // Check for @thick.
          if (attrs.has(TAK_thick)) {
            if (storedRepr) {
              diagnoseInvalid(repr, repr->getStartLoc(),
                              diag::sil_metatype_multiple_reprs);
            }

            storedRepr = MetatypeRepresentation::Thick;
            attrs.clearAttribute(TAK_thick);
          }

          // Check for @objc_metatype.
          if (attrs.has(TAK_objc_metatype)) {
            if (storedRepr) {
              diagnoseInvalid(repr, repr->getStartLoc(),
                              diag::sil_metatype_multiple_reprs);
            }
            storedRepr = MetatypeRepresentation::ObjC;
            attrs.clearAttribute(TAK_objc_metatype);
          }

          if (instanceTy->hasError()) {
            ty = instanceTy;
          } else if (auto metatype = dyn_cast<MetatypeTypeRepr>(repr)) {
            ty = buildMetatypeType(metatype, instanceTy, storedRepr);
          } else {
            ty = buildProtocolType(cast<ProtocolTypeRepr>(repr),
                                   instanceTy, storedRepr);
          }
        }
      }
    }
  }

  // Pass down the variable function type attributes to the
  // function-type creator.
  static const TypeAttrKind FunctionAttrs[] = {
    TAK_convention, TAK_pseudogeneric,
    TAK_callee_owned, TAK_callee_guaranteed, TAK_noescape, TAK_autoclosure,
    TAK_differentiable, TAK_escaping, TAK_Sendable,
    TAK_yield_once, TAK_yield_many, TAK_async
  };

  auto checkUnsupportedAttr = [&](TypeAttrKind attr) {
    if (attrs.has(attr)) {
      diagnoseInvalid(repr, attrs.getLoc(attr), diag::unknown_attribute,
                      TypeAttributes::getAttrName(attr));
      attrs.clearAttribute(attr);
    }
  };
  
  // Some function representation attributes are not supported at source level;
  // only SIL knows how to handle them.  Reject them unless this is a SIL input.
  if (!(options & TypeResolutionFlags::SILType)) {
    for (auto silOnlyAttr : {TAK_pseudogeneric,
                             TAK_callee_owned,
                             TAK_callee_guaranteed,
                             TAK_noescape,
                             TAK_yield_once,
                             TAK_yield_many}) {
      checkUnsupportedAttr(silOnlyAttr);
    }
  }  

  // Other function representation attributes are not normally supported at
  // source level, but we want to support them there in SIL files.
  auto SF = getDeclContext()->getParentSourceFile();
  if (!SF || SF->Kind != SourceFileKind::SIL) {
    for (auto silOnlyAttr : {TAK_thin, TAK_thick}) {
      checkUnsupportedAttr(silOnlyAttr);
    }
  }

  // In SIL mode, allow certain attributes to apply to packs.
  if (options & TypeResolutionFlags::SILType) {
    if (auto packRepr = dyn_cast<PackTypeRepr>(repr)) {
      bool direct = attrs.has(TAK_direct);
      if (direct) attrs.clearAttribute(TAK_direct);

      ty = resolvePackType(packRepr, options, direct);
    }
  }

  bool hasFunctionAttr = globalActor ||
      llvm::any_of(FunctionAttrs, [&attrs](const TypeAttrKind &attr) {
        return attrs.has(attr);
      });

  // Function attributes require a syntactic function type.
  auto *fnRepr = dyn_cast<FunctionTypeRepr>(repr);

  auto tryParseClangType = [this](TypeAttributes::Convention &conv,
                                  bool hasConventionCOrBlock)
                           -> const clang::Type * {
    if (conv.ClangType.Item.empty())
      return nullptr;
    if (!hasConventionCOrBlock) {
      diagnose(conv.ClangType.Loc,
               diag::unexpected_ctype_for_non_c_convention,
               conv.Name, conv.ClangType.Item);
      return nullptr;
    }

    const clang::Type *type =
        getASTContext().getClangModuleLoader()->parseClangFunctionType(
            conv.ClangType.Item, conv.ClangType.Loc);
    if (!type)
      diagnose(conv.ClangType.Loc, diag::unable_to_parse_c_function_type,
               conv.ClangType.Item);
    return type;
  };

  if (fnRepr && hasFunctionAttr) {
    const clang::Type *parsedClangFunctionType = nullptr;
    if (options & TypeResolutionFlags::SILType) {
      SILFunctionType::Representation rep;
      TypeRepr *witnessMethodProtocol = nullptr;

      auto coroutineKind = SILCoroutineKind::None;
      if (attrs.has(TAK_yield_once)) {
        coroutineKind = SILCoroutineKind::YieldOnce;
      } else if (attrs.has(TAK_yield_many)) {
        coroutineKind = SILCoroutineKind::YieldMany;
      }

      auto calleeConvention = ParameterConvention::Direct_Unowned;
      if (attrs.has(TAK_callee_owned)) {
        if (attrs.has(TAK_callee_guaranteed)) {
          diagnoseInvalid(repr, attrs.getLoc(TAK_callee_owned),
                          diag::sil_function_repeat_convention, /*callee*/ 2);
        }
        calleeConvention = ParameterConvention::Direct_Owned;
      } else if (attrs.has(TAK_callee_guaranteed)) {
        calleeConvention = ParameterConvention::Direct_Guaranteed;
      }

      if (!attrs.hasConvention()) {
        rep = SILFunctionType::Representation::Thick;
      } else {
        auto convention = attrs.getConventionName();
        // SIL exposes a greater number of conventions than Swift source.
        auto parsedRep =
            llvm::StringSwitch<llvm::Optional<SILFunctionType::Representation>>(
                convention)
                .Case("thick", SILFunctionType::Representation::Thick)
                .Case("block", SILFunctionType::Representation::Block)
                .Case("thin", SILFunctionType::Representation::Thin)
                .Case("c", SILFunctionType::Representation::CFunctionPointer)
                .Case("method", SILFunctionType::Representation::Method)
                .Case("objc_method",
                      SILFunctionType::Representation::ObjCMethod)
                .Case("witness_method",
                      SILFunctionType::Representation::WitnessMethod)
                .Default(llvm::None);
        if (!parsedRep) {
          diagnoseInvalid(repr, attrs.getLoc(TAK_convention),
                          diag::unsupported_sil_convention,
                          attrs.getConventionName());
          rep = SILFunctionType::Representation::Thin;
        } else {
          rep = *parsedRep;
          parsedClangFunctionType = tryParseClangType(
              attrs.ConventionArguments.value(), shouldStoreClangType(rep));
        }

        if (rep == SILFunctionType::Representation::WitnessMethod) {
          auto protocolName =
            attrs.ConventionArguments.value().WitnessMethodProtocol;
          witnessMethodProtocol = new (getASTContext())
              SimpleIdentTypeRepr(DeclNameLoc(), protocolName);
        }
      }

      DifferentiabilityKind diffKind = DifferentiabilityKind::NonDifferentiable;
      if (attrs.has(TAK_differentiable)) {
        auto *SF = getDeclContext()->getParentSourceFile();
        if (SF && isDifferentiableProgrammingEnabled(*SF)) {
          diffKind = attrs.differentiabilityKind;
        } else {
          diagnoseInvalid(
              repr, attrs.getLoc(TAK_differentiable),
              diag::
                  differentiable_programming_attr_used_without_required_module,
              TypeAttributes::getAttrName(TAK_differentiable),
              getASTContext().Id_Differentiation);
        }
      }

      auto extInfoBuilder = SILFunctionType::ExtInfoBuilder(
          rep, attrs.has(TAK_pseudogeneric), attrs.has(TAK_noescape),
          attrs.has(TAK_Sendable), attrs.has(TAK_async), diffKind,
          parsedClangFunctionType);

      ty =
          resolveSILFunctionType(fnRepr, options, coroutineKind, extInfoBuilder,
                                 calleeConvention, witnessMethodProtocol);
      if (!ty || ty->hasError())
        return ty;
    } else {
      FunctionType::Representation rep = FunctionType::Representation::Swift;
      if (attrs.hasConvention()) {
        auto parsedRep =
            llvm::StringSwitch<llvm::Optional<FunctionType::Representation>>(
                attrs.getConventionName())
                .Case("swift", FunctionType::Representation::Swift)
                .Case("block", FunctionType::Representation::Block)
                .Case("thin", FunctionType::Representation::Thin)
                .Case("c", FunctionType::Representation::CFunctionPointer)
                .Default(llvm::None);
        if (!parsedRep) {
          diagnoseInvalid(repr, attrs.getLoc(TAK_convention),
                          diag::unsupported_convention,
                          attrs.getConventionName());
          rep = FunctionType::Representation::Swift;
        } else {
          rep = *parsedRep;

          parsedClangFunctionType = tryParseClangType(
              attrs.ConventionArguments.value(), shouldStoreClangType(rep));
        }
      }

      DifferentiabilityKind diffKind = DifferentiabilityKind::NonDifferentiable;
      if (attrs.has(TAK_differentiable)) {
        auto *SF = getDeclContext()->getParentSourceFile();
        if (SF && isDifferentiableProgrammingEnabled(*SF)) {
          diffKind = attrs.differentiabilityKind;
        } else {
          diagnoseInvalid(
              repr, attrs.getLoc(TAK_differentiable),
              diag::
                  differentiable_programming_attr_used_without_required_module,
              TypeAttributes::getAttrName(TAK_differentiable),
              getASTContext().Id_Differentiation);
        }
      }

      bool concurrent = attrs.has(TAK_Sendable);

      ty = resolveASTFunctionType(fnRepr, options, rep, /*noescape=*/false,
                                  concurrent, parsedClangFunctionType,
                                  diffKind, globalActor);
      if (!ty || ty->hasError())
        return ty;
    }
  }

  // Validate use of @autoclosure
  if (attrs.has(TAK_autoclosure)) {
    bool didDiagnose = false;
    if (attrs.hasConvention()) {
      if (attrs.getConventionName() == "c" ||
          attrs.getConventionName() == "block") {
        diagnoseInvalid(repr, attrs.getLoc(TAK_convention),
                        diag::invalid_autoclosure_and_convention_attributes,
                        attrs.getConventionName());
        attrs.clearAttribute(TAK_convention);
        didDiagnose = true;
      }
    } else if (options.is(TypeResolverContext::VariadicFunctionInput) &&
               !options.hasBase(TypeResolverContext::EnumElementDecl)) {
      diagnoseInvalid(repr, attrs.getLoc(TAK_autoclosure),
                      diag::attr_not_on_variadic_parameters, "@autoclosure");
      attrs.clearAttribute(TAK_autoclosure);
      didDiagnose = true;
    } else if (!options.is(TypeResolverContext::FunctionInput)) {
      diagnoseInvalid(repr, attrs.getLoc(TAK_autoclosure),
                      diag::attr_only_on_parameters, "@autoclosure");
      attrs.clearAttribute(TAK_autoclosure);
      didDiagnose = true;
    }

    if (didDiagnose) {
      ty = ErrorType::get(getASTContext());
    }
  }

  if (attrs.has(TAK_unchecked)) {
    ty = resolveType(repr, options);
    if (!ty || ty->hasError()) return ty;

    if (!options.is(TypeResolverContext::Inherited) ||
        getDeclContext()->getSelfProtocolDecl()) {
      diagnoseInvalid(repr, attrs.getLoc(TAK_unchecked),
                      diag::unchecked_not_inheritance_clause);
      ty = ErrorType::get(getASTContext());
    } else if (!ty->isConstraintType()) {
      diagnoseInvalid(repr, attrs.getLoc(TAK_unchecked),
                      diag::unchecked_not_existential, ty);
      ty = ErrorType::get(getASTContext());
    }

    // Nothing to record in the type. Just clear the attribute.
    attrs.clearAttribute(TAK_unchecked);
  }

  if (attrs.has(TAK_opened)) {
    ty = resolveOpenedExistentialArchetype(attrs, repr, options);
  } else if (attrs.has(TAK_pack_element)) {
    ty = resolvePackElementArchetype(attrs, repr, options);
  }

  auto instanceOptions = options;
  instanceOptions.setContext(llvm::None);

  // If we didn't build the type differently above, we might have
  // a typealias pointing at a function type with the @escaping
  // attribute. Resolve the type as if it were in non-parameter
  // context, and then set isNoEscape if @escaping is not present.
  if (!ty) ty = resolveType(repr, instanceOptions);
  if (!ty || ty->hasError()) return ty;

  // Type aliases inside protocols are not yet resolved in the structural
  // stage of type resolution
  if (ty->is<DependentMemberType>() &&
      resolution.getStage() == TypeResolutionStage::Structural) {
    return ty;
  }

  // Handle @escaping
  if (ty->is<FunctionType>()) {
    if (attrs.has(TAK_escaping)) {
      // The attribute is meaningless except on non-variadic parameter types.
      if (!isParam || options.getBaseContext() == TypeResolverContext::EnumElementDecl) {
        auto loc = attrs.getLoc(TAK_escaping);
        auto attrRange = getTypeAttrRangeWithAt(getASTContext(), loc);

        // Try to find a better diagnostic based on how the type is being used
        if (options.is(TypeResolverContext::ImmediateOptionalTypeArgument)) {
          diagnoseInvalid(repr, repr->getLoc(),
                          diag::escaping_optional_type_argument)
              .fixItRemove(attrRange);
        } else {
          diagnoseInvalid(repr, loc, diag::escaping_non_function_parameter)
              .fixItRemove(attrRange);
        }

        ty = ErrorType::get(getASTContext());
      }

      attrs.clearAttribute(TAK_escaping);
    } else {
      // No attribute; set the isNoEscape bit if we're in parameter context.
      ty = applyNonEscapingIfNecessary(ty, options);
    }
  }

  if (attrs.has(TAK_autoclosure)) {
    // If this is a situation where function type is wrapped
    // into a number of parens, let's try to look through them,
    // because parens are insignificant here e.g.:
    //
    // let _: (@autoclosure (() -> Void)) -> Void = { _ in }
    if (!ty->is<FunctionType>()) {
      // @autoclosure is going to be diagnosed when type of
      // the parameter is validated, because that attribute
      // applies to the declaration now.
      repr->setInvalid();
    }

    attrs.clearAttribute(TAK_autoclosure);
  }

  if (hasFunctionAttr && !fnRepr) {
    const auto diagnoseInvalidAttr = [&](TypeAttrKind kind) {
      if (kind == TAK_escaping) {
        Type optionalObjectType = ty->getOptionalObjectType();
        if (optionalObjectType && optionalObjectType->is<AnyFunctionType>()) {
          return diagnoseInvalid(repr, attrs.getLoc(kind),
                                 diag::escaping_optional_type_argument);
        }
      }
      return diagnoseInvalid(repr, attrs.getLoc(kind),
                             diag::attribute_requires_function_type,
                             TypeAttributes::getAttrName(kind));
    };

    for (auto i : FunctionAttrs) {
      if (!attrs.has(i))
        continue;

      auto diag = diagnoseInvalidAttr(i);
      // If we see @escaping among the attributes on this type, because it
      // isn't a function type, we'll remove it.
      if (i == TAK_escaping) {
        diag.fixItRemove(getTypeAttrRangeWithAt(getASTContext(),
                                                attrs.getLoc(TAK_escaping)));
      }
      attrs.clearAttribute(i);
    }
  } else if (hasFunctionAttr && fnRepr) {
    // Remove the function attributes from the set so that we don't diagnose.
    for (auto i : FunctionAttrs)
      attrs.clearAttribute(i);
    attrs.ConventionArguments = llvm::None;
  }

  if (attrs.has(TAK_noDerivative)) {
    // @noDerivative is valid on function parameters (AST and SIL) or on
    // function results (SIL-only).
    bool isNoDerivativeAllowed =
        isParam ||
        options.is(TypeResolverContext::InoutFunctionInput) ||
        (isResult && (options & TypeResolutionFlags::SILType));
    auto *SF = getDeclContext()->getParentSourceFile();
    if (SF && !isDifferentiableProgrammingEnabled(*SF)) {
      diagnose(
          attrs.getLoc(TAK_noDerivative),
          diag::differentiable_programming_attr_used_without_required_module,
          TypeAttributes::getAttrName(TAK_noDerivative),
          getASTContext().Id_Differentiation);
    } else if (!isNoDerivativeAllowed) {
      diagnose(attrs.getLoc(TAK_noDerivative),
               (isVariadicFunctionParam
                    ? diag::attr_not_on_variadic_parameters
                    : diag::attr_only_on_parameters_of_differentiable),
               "@noDerivative");
    }
    attrs.clearAttribute(TAK_noDerivative);
  }

  if (getASTContext().LangOpts.hasFeature(Feature::LayoutPrespecialization)) {
    if (attrs.has(TAK__noMetadata)) {
      // TODO: add proper validation
      attrs.clearAttribute(TAK__noMetadata);
    }
  }

  // In SIL files *only*, permit @weak and @unowned to apply directly to types.
  if (attrs.hasOwnership()) {
    if (auto SF = getDeclContext()->getParentSourceFile()) {
      if (SF->Kind == SourceFileKind::SIL) {
        if (((attrs.has(TAK_sil_weak) || attrs.has(TAK_sil_unmanaged)) &&
             ty->getOptionalObjectType()) ||
            (!attrs.has(TAK_sil_weak) &&
             GenericEnvironment::mapTypeIntoContext(
                 resolution.getGenericSignature().getGenericEnvironment(), ty)
                 ->hasReferenceSemantics())) {
          ty = ReferenceStorageType::get(ty, attrs.getOwnership(),
                                         getASTContext());
          attrs.clearOwnership();
        }
      }
    }
  }
  
  // In SIL *only*, allow @block_storage to specify a block storage type.
  if ((options & TypeResolutionFlags::SILType) && attrs.has(TAK_block_storage)) {
    ty = SILBlockStorageType::get(ty->getCanonicalType());
    attrs.clearAttribute(TAK_block_storage);
  }
  
  // In SIL *only*, allow @box to specify a box type.
  if ((options & TypeResolutionFlags::SILType) && attrs.has(TAK_box)) {
    ty = SILBoxType::get(ty->getCanonicalType());
    attrs.clearAttribute(TAK_box);
  }

  // In SIL *only*, allow @moveOnly to specify a moveOnly type.
  if ((options & TypeResolutionFlags::SILType) && attrs.has(TAK_moveOnly)) {
    ty = SILMoveOnlyWrappedType::get(ty->getCanonicalType());
    attrs.clearAttribute(TAK_moveOnly);
  }

  // In SIL *only*, allow @dynamic_self to specify a dynamic Self type.
  if ((options & TypeResolutionFlags::SILMode) && attrs.has(TAK_dynamic_self)) {
    ty = rebuildWithDynamicSelf(getASTContext(), ty);
    attrs.clearAttribute(TAK_dynamic_self);
  }

  // In SIL *only*, allow @async to specify an async function
  if ((options & TypeResolutionFlags::SILMode) && attrs.has(TAK_async)) {
    if (fnRepr != nullptr) {
      attrs.clearAttribute(TAK_async);
    }
  }

  for (unsigned i = 0; i != TypeAttrKind::TAK_Count; ++i)
    if (attrs.has((TypeAttrKind)i)) {
      diagnoseInvalid(repr, attrs.getLoc((TypeAttrKind)i),
                      diag::attribute_does_not_apply_to_type);
    }

  return ty;
}

SmallVector<AnyFunctionType::Param, 8>
TypeResolver::resolveASTFunctionTypeParams(TupleTypeRepr *inputRepr,
                                           TypeResolutionOptions options,
                                           DifferentiabilityKind diffKind) {
  SourceLoc ellipsisLoc;

  SmallVector<AnyFunctionType::Param, 8> elements;
  elements.reserve(inputRepr->getNumElements());

  auto elementOptions = options.withoutContext(true);
  elementOptions.setContext(TypeResolverContext::FunctionInput);
  for (unsigned i = 0, end = inputRepr->getNumElements(); i != end; ++i) {
    auto *eltTypeRepr = inputRepr->getElementType(i);

    // Look through parens here; other than parens, specifiers
    // must appear at the top level of a parameter type.
    auto *nestedRepr = eltTypeRepr->getWithoutParens();

    ParamSpecifier ownership = ParamSpecifier::Default;

    bool isolated = false;
    bool compileTimeConst = false;
    while (true) {
      if (auto *specifierRepr = dyn_cast<SpecifierTypeRepr>(nestedRepr)) {
        switch (specifierRepr->getKind()) {
        case TypeReprKind::Ownership:
          ownership = cast<OwnershipTypeRepr>(specifierRepr)->getSpecifier();
          nestedRepr = specifierRepr->getBase();
          continue;
        case TypeReprKind::Isolated:
          isolated = true;
          nestedRepr = specifierRepr->getBase();
          continue;
        case TypeReprKind::CompileTimeConst:
          compileTimeConst = true;
          nestedRepr = specifierRepr->getBase();
          continue;
        default:
          break;
        }
      }
      break;
    }

    bool autoclosure = false;
    bool noDerivative = false;

    if (auto *ATR = dyn_cast<AttributedTypeRepr>(nestedRepr)) {
      autoclosure = ATR->getAttrs().has(TAK_autoclosure);

      if (ATR->getAttrs().has(TAK_noDerivative)) {
        if (diffKind == DifferentiabilityKind::NonDifferentiable &&
            isDifferentiableProgrammingEnabled(
                *getDeclContext()->getParentSourceFile()))
          diagnose(nestedRepr->getLoc(),
                   diag::attr_only_on_parameters_of_differentiable,
                   "@noDerivative")
              .highlight(nestedRepr->getSourceRange());
        else
          noDerivative = true;
      }

      nestedRepr = ATR->getTypeRepr();
    }

    Type ty;

    // Do we have an old-style variadic parameter?
    bool variadic = false;

    if (auto *varargTypeRepr = dyn_cast<VarargTypeRepr>(nestedRepr)) {
      if (ellipsisLoc) {
        diagnose(varargTypeRepr->getLoc(),
                 diag::multiple_ellipsis_in_tuple)
          .highlight(ellipsisLoc)
          .fixItRemove(varargTypeRepr->getEllipsisLoc());
      }

      ellipsisLoc = varargTypeRepr->getEllipsisLoc();

      // If the element is a variadic parameter, resolve the parameter type as if
      // it were in non-parameter position, since we want functions to be
      // @escaping in this case.
      auto thisElementOptions = elementOptions.withoutContext();
      thisElementOptions.setContext(TypeResolverContext::VariadicFunctionInput);

      // We have an old-style variadic parameter.
      variadic = true;
      ty = resolveType(varargTypeRepr, thisElementOptions);
      if (ty->hasError()) {
        elements.emplace_back(ErrorType::get(getASTContext()));
        continue;
      }
    } else {
      ty = resolveType(eltTypeRepr, elementOptions);
      if (ty->hasError()) {
        elements.emplace_back(ErrorType::get(getASTContext()));
        continue;
      }
    }

    Identifier argumentLabel;
    Identifier parameterName;
    if (inputRepr->getElement(i).NameLoc.isValid() &&
        inputRepr->getElement(i).SecondNameLoc.isValid() &&
        inputRepr->getElement(i).NameLoc !=
            inputRepr->getElement(i).SecondNameLoc) {
      argumentLabel = inputRepr->getElement(i).Name;
      parameterName = inputRepr->getElement(i).SecondName;
    } else {
      parameterName = inputRepr->getElementName(i);
    }

    auto paramFlags = ParameterTypeFlags::fromParameterType(
        ty, variadic, autoclosure, /*isNonEphemeral*/ false, ownership,
        isolated, noDerivative, compileTimeConst);
    elements.emplace_back(ty, argumentLabel, paramFlags, parameterName);
  }

  return elements;
}

NeverNullType
TypeResolver::resolveOpaqueReturnType(TypeRepr *repr, StringRef mangledName,
                                      unsigned ordinal,
                                      TypeResolutionOptions options) {
  // The type repr should be a generic identifier type. We don't really use
  // the identifier for anything, but we do resolve the generic arguments
  // to instantiate the possibly-generic opaque type.
  SmallVector<Type, 4> TypeArgsBuf;
  if (auto generic = dyn_cast<GenericIdentTypeRepr>(repr)) {
    for (auto argRepr : generic->getGenericArgs()) {
      auto argTy = resolveType(argRepr, options);
      // If we cannot resolve the generic parameter, propagate the error out.
      if (argTy->hasError()) {
        return ErrorType::get(getASTContext());
      }
      TypeArgsBuf.push_back(argTy);
    }
  }
  
  // Use type reconstruction to summon the opaque type decl.
  Demangler demangle;
  auto definingDeclNode = demangle.demangleSymbol(mangledName);
  if (!definingDeclNode) {
    diagnose(repr->getLoc(), diag::no_opaque_return_type_of);
    return ErrorType::get(getASTContext());
  }
  if (definingDeclNode->getKind() == Node::Kind::Global)
    definingDeclNode = definingDeclNode->getChild(0);
  ASTBuilder builder(getASTContext(), GenericSignature());
  auto opaqueNode =
    builder.getNodeFactory().createNode(Node::Kind::OpaqueReturnTypeOf);
  opaqueNode->addChild(definingDeclNode, builder.getNodeFactory());
  
  auto TypeArgs = ArrayRef<Type>(TypeArgsBuf);
  auto ty = builder.resolveOpaqueType(opaqueNode, TypeArgs, ordinal);
  if (!ty || ty->hasError()) {
    diagnose(repr->getLoc(), diag::no_opaque_return_type_of);
    return ErrorType::get(getASTContext());
  }
  return ty;
}

NeverNullType TypeResolver::resolveASTFunctionType(
    FunctionTypeRepr *repr, TypeResolutionOptions parentOptions,
    AnyFunctionType::Representation representation, bool noescape,
    bool concurrent, const clang::Type *parsedClangFunctionType,
    DifferentiabilityKind diffKind, Type globalActor) {

  // can't have more than 1 isolated parameter.
  if (!repr->isWarnedAbout() && hasMoreIsolatedParamsThan(repr, 1)) {
    diagnose(repr->getLoc(), diag::isolated_parameter_duplicate_type)
        .warnUntilSwiftVersion(6);

    if (getASTContext().LangOpts.isSwiftVersionAtLeast(6))
      return ErrorType::get(getASTContext());
    else
      repr->setWarned();
  }

  // Diagnose a couple of things that we can parse in SIL mode but we don't
  // allow in formal types.
  if (auto patternParams = repr->getPatternGenericParams()) {
    diagnose(patternParams->getLAngleLoc(),
             diag::ast_subst_function_type);
    return ErrorType::get(getASTContext());
  } else if (!repr->getInvocationSubstitutions().empty()) {
    diagnose(repr->getInvocationSubstitutions()[0]->getStartLoc(),
             diag::ast_subst_function_type);
    return ErrorType::get(getASTContext());
  }

  llvm::Optional<SILInnerGenericContextRAII> innerGenericContext;
  if (auto *genericParams = repr->getGenericParams()) {
    if (!silContext) {
      diagnose(genericParams->getLAngleLoc(), diag::generic_function_type)
        .highlight(genericParams->getSourceRange());
      return ErrorType::get(getASTContext());
    }
    innerGenericContext.emplace(silContext, genericParams);
  }

  TypeResolutionOptions options = llvm::None;
  options |= parentOptions.withoutContext().getFlags();
  auto params =
      resolveASTFunctionTypeParams(repr->getArgsTypeRepr(), options, diffKind);

  auto resultOptions = options.withoutContext();
  resultOptions.setContext(TypeResolverContext::FunctionResult);
  auto outputTy = resolveType(repr->getResultTypeRepr(), resultOptions);
  if (outputTy->hasError()) {
    return ErrorType::get(getASTContext());
  }

  // If this is a function type without parens around the parameter list,
  // diagnose this and produce a fixit to add them.
  if (!repr->isWarnedAbout()) {
    // If someone wrote (Void) -> () in Swift 3, they probably meant
    // () -> (), but (Void) -> () is (()) -> () so emit a warning
    // asking if they meant () -> ().
    auto args = repr->getArgsTypeRepr();
    if (args->getNumElements() == 1) {
      if (const auto Void =
          dyn_cast<SimpleIdentTypeRepr>(args->getElementType(0))) {
        if (Void->getNameRef().isSimpleName(getASTContext().Id_Void)) {
          diagnose(args->getStartLoc(), diag::paren_void_probably_void)
            .fixItReplace(args->getSourceRange(), "()");
          repr->setWarned();
        }
      }
    }
  }

  FunctionType::ExtInfoBuilder extInfoBuilder(
      FunctionTypeRepresentation::Swift, noescape, repr->isThrowing(), diffKind,
      /*clangFunctionType*/ nullptr, Type());

  const clang::Type *clangFnType = parsedClangFunctionType;
  if (shouldStoreClangType(representation) && !clangFnType)
    clangFnType =
        getASTContext().getClangFunctionType(params, outputTy, representation);

  auto extInfo = extInfoBuilder.withRepresentation(representation)
                     .withConcurrent(concurrent)
                     .withAsync(repr->isAsync())
                     .withClangFunctionType(clangFnType)
                     .withGlobalActor(globalActor)
                     .build();

  // SIL uses polymorphic function types to resolve overloaded member functions.
  if (auto genericSig = repr->getGenericSignature()) {
    return GenericFunctionType::get(genericSig, params, outputTy, extInfo);
  }

  auto fnTy = FunctionType::get(params, outputTy, extInfo);
  
  if (fnTy->hasError())
    return fnTy;

  if (TypeChecker::diagnoseInvalidFunctionType(fnTy, repr->getLoc(), repr,
                                               getDeclContext(),
                                               resolution.getStage()))
    return ErrorType::get(fnTy);

  return fnTy;
}

NeverNullType TypeResolver::resolveSILBoxType(SILBoxTypeRepr *repr,
                                              bool capturesGenerics,
                                              TypeResolutionOptions options) {
  assert(silContext && "resolving SIL box type outside of SIL");

  // Resolve the field types.
  SmallVector<SILField, 4> fields;
  {
    // Resolve field types using the box type's generic environment, if it
    // has one. (TODO: Field types should never refer to generic parameters
    // outside the box's own environment; we should really validate that...)
    TypeResolution fieldResolution{resolution};

    auto genericSig = repr->getGenericSignature();
    auto *genericParams = repr->getGenericParams();

    if (genericParams) {
      fieldResolution =
          TypeResolution::forInterface(getDeclContext(), genericSig, options,
                                       resolution.getUnboundTypeOpener(),
                                       resolution.getPlaceholderHandler(),
                                       resolution.getPackElementOpener());
    }

    SILInnerGenericContextRAII scope(silContext, genericParams);
    TypeResolver fieldResolver{fieldResolution, silContext};
    for (auto &fieldRepr : repr->getFields()) {
      auto fieldTy = fieldResolver.resolveType(fieldRepr.getFieldType(), options);
      fields.push_back({fieldTy->getCanonicalType(), fieldRepr.isMutable()});
    }
  }

  // Substitute out parsed context types into interface types.
  auto genericSig = repr->getGenericSignature().getCanonicalSignature();
  
  // Resolve the generic arguments.
  // Start by building a TypeSubstitutionMap.
  SubstitutionMap subMap;
  if (genericSig) {
    TypeSubstitutionMap genericArgMap;

    auto params = genericSig.getGenericParams();
    if (repr->getGenericArguments().size()
          != genericSig.getGenericParams().size()) {
      diagnose(repr->getLoc(), diag::sil_box_arg_mismatch);
      return ErrorType::get(getASTContext());
    }
  
    for (unsigned i : indices(params)) {
      auto argTy = resolveType(repr->getGenericArguments()[i], options);
      genericArgMap.insert({params[i], argTy->getCanonicalType()});
    }

    subMap = SubstitutionMap::get(
        genericSig, QueryTypeSubstitutionMap{genericArgMap},
        LookUpConformanceInModule(getDeclContext()->getParentModule()));
  }

  auto layout = SILLayout::get(getASTContext(), genericSig, fields,
                               capturesGenerics);
  return SILBoxType::get(getASTContext(), layout, subMap);
}

NeverNullType TypeResolver::resolveSILFunctionType(
    FunctionTypeRepr *repr, TypeResolutionOptions options,
    SILCoroutineKind coroutineKind,
    SILFunctionType::ExtInfoBuilder extInfoBuilder, ParameterConvention callee,
    TypeRepr *witnessMethodProtocol) {
  assert(silContext);

  options.setContext(llvm::None);

  bool hasError = false;

  // Resolve parameter and result types using the function's generic
  // environment.
  SmallVector<SILParameterInfo, 4> params;
  SmallVector<SILYieldInfo, 4> yields;
  SmallVector<SILResultInfo, 4> results;
  llvm::Optional<SILResultInfo> errorResult;

  // Resolve generic params in the pattern environment, if present, or
  // else the function's generic environment, if it has one.
  GenericParamList *genericParams = repr->getGenericParams();
  if (genericParams == nullptr)
    genericParams = silContext->GenericParams;
  GenericParamList *componentGenericParams = repr->getPatternGenericParams();
  if (componentGenericParams == nullptr)
    componentGenericParams = genericParams;

  auto genericSig = repr->getGenericSignature();
  auto componentTypeSig = repr->getPatternGenericSignature();
  if (!componentTypeSig)
    componentTypeSig = genericSig;

  {
    auto argsTuple = repr->getArgsTypeRepr();

    // SIL functions cannot have parameter names.
    for (auto &element : argsTuple->getElements()) {
      if (element.UnderscoreLoc.isValid())
        diagnose(element.UnderscoreLoc, diag::sil_function_input_label);
    }

    TypeResolution functionResolution{resolution};
    if (componentTypeSig) {
      functionResolution = TypeResolution::forInterface(
          getDeclContext(), componentTypeSig, options,
          resolution.getUnboundTypeOpener(),
          resolution.getPlaceholderHandler(),
          resolution.getPackElementOpener());
    }

    SILInnerGenericContextRAII innerGenericContext(silContext,
                                                   componentGenericParams);

    TypeResolver silResolver{functionResolution, silContext};
    for (auto elt : argsTuple->getElements()) {
      auto elementOptions = options;
      elementOptions.setContext(TypeResolverContext::FunctionInput);
      auto param = silResolver.resolveSILParameter(elt.Type, elementOptions);
      params.push_back(param);

      if (!param.getInterfaceType() || param.getInterfaceType()->hasError())
        hasError = true;
    }

    {
      if (silResolver.resolveSILResults(repr->getResultTypeRepr(),
                                        options, yields,
                                        results, errorResult)) {
        hasError = true;
      }

      // Diagnose non-coroutines that declare yields.
      if (coroutineKind == SILCoroutineKind::None && !yields.empty()) {
        diagnose(repr->getResultTypeRepr()->getLoc(),
                 diag::sil_non_coro_yields);
        hasError = true;
      }
    }
  }

  SILInnerGenericContextRAII innerGenericContext(silContext, genericParams);

  auto resolveSubstitutions = [&](GenericSignature sig,
                                  ArrayRef<TypeRepr*> args,
                                  TypeResolver &&parameterResolver) {
    sig = sig.getCanonicalSignature();
    TypeSubstitutionMap subsMap;
    auto params = sig.getGenericParams();
    for (unsigned i : indices(args)) {
      auto resolved = parameterResolver.resolveType(args[i], options);
      subsMap.insert({params[i], resolved->getCanonicalType()});
    }
    return SubstitutionMap::get(
               sig, QueryTypeSubstitutionMap{subsMap},
               LookUpConformanceInModule(getDeclContext()->getParentModule()))
        .getCanonical();
  };

  // Resolve pattern substitutions in the invocation environment, if
  // applicable.
  SubstitutionMap patternSubs;
  if (!repr->getPatternSubstitutions().empty()) {
    if (genericSig) {
      auto resolveSILParameters =
          TypeResolution::forInterface(getDeclContext(), genericSig, options,
                                       resolution.getUnboundTypeOpener(),
                                       resolution.getPlaceholderHandler(),
                                       resolution.getPackElementOpener());
      patternSubs = resolveSubstitutions(repr->getPatternGenericSignature(),
                                         repr->getPatternSubstitutions(),
                                         TypeResolver{resolveSILParameters,
                                                      silContext});
    } else {
      patternSubs = resolveSubstitutions(repr->getPatternGenericSignature(),
                                         repr->getPatternSubstitutions(),
                                         TypeResolver{resolution,
                                                      silContext});
    }
  }

  // Resolve invocation substitutions if we have them.
  SubstitutionMap invocationSubs;
  if (!repr->getInvocationSubstitutions().empty()) {
    invocationSubs = resolveSubstitutions(repr->getGenericSignature(),
                                          repr->getInvocationSubstitutions(),
                                          TypeResolver{resolution,
                                                       silContext});
  }

  if (hasError) {
    return ErrorType::get(getASTContext());
  }

  ProtocolConformanceRef witnessMethodConformance;
  if (witnessMethodProtocol) {
    auto resolved = resolveType(witnessMethodProtocol,
        options.withContext(TypeResolverContext::GenericRequirement));
    if (resolved->hasError())
      return resolved;

    auto protocolType = resolved->getAs<ProtocolType>();
    if (!protocolType)
      return ErrorType::get(getASTContext());

    Type selfType = params.back().getInterfaceType();
    if (patternSubs)
      selfType = selfType.subst(patternSubs);
    if (invocationSubs) {
      selfType = selfType.subst(invocationSubs);
    }

    // Only once we've done all the necessary substitutions, map the type
    // into the function's environment.
    selfType = GenericEnvironment::mapTypeIntoContext(
        genericSig.getGenericEnvironment(), selfType);

    // The Self type can be nested in a few layers of metatypes (etc.).
    while (auto metatypeType = selfType->getAs<MetatypeType>()) {
      auto next = metatypeType->getInstanceType();
      if (next->isEqual(selfType))
        break;
      selfType = next;
    }

    witnessMethodConformance = TypeChecker::conformsToProtocol(
        selfType, protocolType->getDecl(),
        getDeclContext()->getParentModule());
    assert(witnessMethodConformance &&
           "found witness_method without matching conformance");
  }

  auto representation = extInfoBuilder.getRepresentation();
  const clang::Type *clangFnType = extInfoBuilder.getClangTypeInfo().getType();
  if (shouldStoreClangType(representation) && !clangFnType) {
    assert(results.size() <= 1 && yields.size() == 0 &&
           "C functions and blocks have at most 1 result and 0 yields.");
    auto result =
        results.empty() ? llvm::Optional<SILResultInfo>() : results[0];
    clangFnType = getASTContext().getCanonicalClangFunctionType(params, result,
                                                                representation);
    extInfoBuilder = extInfoBuilder.withClangFunctionType(clangFnType);
  }

  return SILFunctionType::get(genericSig.getCanonicalSignature(),
                              extInfoBuilder.build(), coroutineKind,
                              callee, params, yields, results, errorResult,
                              patternSubs, invocationSubs, getASTContext(),
                              witnessMethodConformance);
}

SILYieldInfo TypeResolver::resolveSILYield(TypeAttributes &attrs,
                                           TypeRepr *repr,
                                           TypeResolutionOptions options) {
  AttributedTypeRepr attrRepr(attrs, repr);
  options.setContext(TypeResolverContext::FunctionInput);
  SILParameterInfo paramInfo = resolveSILParameter(&attrRepr, options);
  return SILYieldInfo(paramInfo.getInterfaceType(), paramInfo.getConvention());
}

SILParameterInfo TypeResolver::resolveSILParameter(
                                 TypeRepr *repr,
                                 TypeResolutionOptions options) {
  assert(options.is(TypeResolverContext::FunctionInput) &&
         "Parameters should be marked as inputs");
  auto convention = DefaultParameterConvention;
  Type type;
  bool hadError = false;
  auto differentiability =
      SILParameterDifferentiability::DifferentiableOrNotApplicable;

  if (auto attrRepr = dyn_cast<AttributedTypeRepr>(repr)) {
    auto attrs = attrRepr->getAttrs();

    auto checkFor = [&](TypeAttrKind tak, ParameterConvention attrConv) {
      if (!attrs.has(tak)) return;
      if (convention != DefaultParameterConvention) {
        diagnose(attrs.getLoc(tak), diag::sil_function_repeat_convention,
                 /*input*/ 0);
        hadError = true;
      }
      attrs.clearAttribute(tak);
      convention = attrConv;
    };
    checkFor(TypeAttrKind::TAK_in_guaranteed,
             ParameterConvention::Indirect_In_Guaranteed);
    checkFor(TypeAttrKind::TAK_in, ParameterConvention::Indirect_In);
    checkFor(TypeAttrKind::TAK_in_constant,
             ParameterConvention::Indirect_In);
    checkFor(TypeAttrKind::TAK_inout, ParameterConvention::Indirect_Inout);
    checkFor(TypeAttrKind::TAK_inout_aliasable,
             ParameterConvention::Indirect_InoutAliasable);
    checkFor(TypeAttrKind::TAK_owned, ParameterConvention::Direct_Owned);
    checkFor(TypeAttrKind::TAK_guaranteed,
             ParameterConvention::Direct_Guaranteed);
    checkFor(TypeAttrKind::TAK_pack_owned,
             ParameterConvention::Pack_Owned);
    checkFor(TypeAttrKind::TAK_pack_guaranteed,
             ParameterConvention::Pack_Guaranteed);
    checkFor(TypeAttrKind::TAK_pack_inout,
             ParameterConvention::Pack_Inout);
    if (attrs.has(TAK_noDerivative)) {
      attrs.clearAttribute(TAK_noDerivative);
      differentiability = SILParameterDifferentiability::NotDifferentiable;
    }

    type = resolveAttributedType(attrs, attrRepr->getTypeRepr(), options);
  } else {
    type = resolveType(repr, options);
  }

  if (!type || type->hasError()) {
    hadError = true;

  // Diagnose types that are illegal in SIL.
  } else if (!type->isLegalSILType()) {
    diagnose(repr->getLoc(), diag::illegal_sil_type, type);
    hadError = true;
  }

  if (hadError)
    type = ErrorType::get(getASTContext());
  return SILParameterInfo(type->getCanonicalType(), convention,
                          differentiability);
}

bool TypeResolver::resolveSingleSILResult(
    TypeRepr *repr, TypeResolutionOptions options,
    SmallVectorImpl<SILYieldInfo> &yields,
    SmallVectorImpl<SILResultInfo> &ordinaryResults,
    llvm::Optional<SILResultInfo> &errorResult) {
  Type type;
  auto convention = DefaultResultConvention;
  bool isErrorResult = false;
  auto differentiability =
      SILResultDifferentiability::DifferentiableOrNotApplicable;
  options.setContext(TypeResolverContext::FunctionResult);

  if (auto attrRepr = dyn_cast<AttributedTypeRepr>(repr)) {
    // Copy the attributes out; we're going to destructively modify them.
    auto attrs = attrRepr->getAttrs();

    // Recognize @yields.
    if (attrs.has(TypeAttrKind::TAK_yields)) {
      attrs.clearAttribute(TypeAttrKind::TAK_yields);

      // The treatment from this point on is basically completely different.
      auto yield = resolveSILYield(attrs, attrRepr->getTypeRepr(), options);
      if (yield.getInterfaceType()->hasError())
        return true;

      yields.push_back(yield);
      return false;
    }

    // Recognize @error.
    if (attrs.has(TypeAttrKind::TAK_error)) {
      attrs.clearAttribute(TypeAttrKind::TAK_error);
      isErrorResult = true;

      // Error results are always implicitly @owned.
      convention = ResultConvention::Owned;
    }

    // Recognize `@noDerivative`.
    if (attrs.has(TAK_noDerivative)) {
      attrs.clearAttribute(TAK_noDerivative);
      differentiability = SILResultDifferentiability::NotDifferentiable;
    }

    // Recognize result conventions.
    bool hadError = false;
    auto checkFor = [&](TypeAttrKind tak, ResultConvention attrConv) {
      if (!attrs.has(tak)) return;
      if (convention != DefaultResultConvention) {
        diagnose(attrs.getLoc(tak), diag::sil_function_repeat_convention,
                 /*result*/ 1);
        hadError = true;
      }
      attrs.clearAttribute(tak);
      convention = attrConv;
    };
    checkFor(TypeAttrKind::TAK_out, ResultConvention::Indirect);
    checkFor(TypeAttrKind::TAK_owned, ResultConvention::Owned);
    checkFor(TypeAttrKind::TAK_unowned_inner_pointer,
             ResultConvention::UnownedInnerPointer);
    checkFor(TypeAttrKind::TAK_autoreleased, ResultConvention::Autoreleased);
    checkFor(TypeAttrKind::TAK_pack_out, ResultConvention::Pack);
    if (hadError) return true;

    type = resolveAttributedType(attrs, attrRepr->getTypeRepr(), options);
  } else {
    type = resolveType(repr, options);
  }

  // Propagate type-resolution errors out.
  if (!type || type->hasError()) return true;

  // Diagnose types that are illegal in SIL.
  if (!type->isLegalSILType()) {
    diagnose(repr->getStartLoc(), diag::illegal_sil_type, type);
    return false;
  }

  assert(!isErrorResult || convention == ResultConvention::Owned);
  SILResultInfo resolvedResult(type->getCanonicalType(), convention,
                               differentiability);

  if (!isErrorResult) {
    ordinaryResults.push_back(resolvedResult);
    return false;
  }

  // Error result types must have pointer-like representation.
  // FIXME: check that here?

  // We don't expect to have a reason to support multiple independent
  // error results.  (Would this be disjunctive or conjunctive?)
  if (errorResult.has_value()) {
    diagnose(repr->getStartLoc(),
             diag::sil_function_multiple_error_results);
    return true;
  }

  errorResult = resolvedResult;
  return false;
}

bool TypeResolver::resolveSILResults(
    TypeRepr *repr, TypeResolutionOptions options,
    SmallVectorImpl<SILYieldInfo> &yields,
    SmallVectorImpl<SILResultInfo> &ordinaryResults,
    llvm::Optional<SILResultInfo> &errorResult) {
  if (auto tuple = dyn_cast<TupleTypeRepr>(repr)) {
    // If any of the elements have a label, or an explicit missing label (_:),
    // resolve the entire result type as a single tuple type.
    for (auto &element : tuple->getElements()) {
      if (element.NameLoc.isValid()) {
        return resolveSingleSILResult(repr, options,
                                      yields, ordinaryResults, errorResult);
      }
    }

    // Otherwise, resolve each tuple element into its own result type.
    bool hadError = false;

    for (auto elt : tuple->getElements()) {
      if (resolveSingleSILResult(elt.Type, options,
                                 yields, ordinaryResults, errorResult))
        hadError = true;
    }

    return hadError;
  }

  // Not a tuple type.
  return resolveSingleSILResult(repr, options,
                                yields, ordinaryResults, errorResult);
}

NeverNullType
TypeResolver::resolveDeclRefTypeRepr(DeclRefTypeRepr *repr,
                                     TypeResolutionOptions options) {
  Type result;

  auto *baseComp = repr->getBaseComponent();
  if (auto *identBase = dyn_cast<IdentTypeRepr>(baseComp)) {
    // The base component uses unqualified lookup.
    result = resolveUnqualifiedIdentTypeRepr(resolution.withOptions(options),
                                             silContext, identBase);

    if (result && result->isParameterPack() &&
        // Workaround to allow 'shape' type checking of SIL.
        // TODO: Explicitly pass along whether in a 'shape' context.
        !options.contains(TypeResolutionFlags::SILMode)) {
      bool invalid = false;
      if (!options.contains(TypeResolutionFlags::AllowPackReferences)) {
        diagnose(repr->getLoc(), diag::pack_reference_must_be_in_expansion,
                 repr);
        invalid = true;
      }
      if (!options.contains(TypeResolutionFlags::FromPackReference)) {
        diagnose(repr->getLoc(), diag::pack_type_requires_keyword_each,
                 repr)
            .fixItInsert(repr->getLoc(), "each ");
        invalid = true;
      }
      if (invalid) {
        return ErrorType::get(result);
      }
    }
  } else {
    result = resolveType(baseComp, options);
  }

  if (result->hasError())
    return ErrorType::get(result->getASTContext());

  // Remaining components are resolved via iterated qualified lookups.
  if (auto *memberTR = dyn_cast<MemberTypeRepr>(repr)) {
    SourceRange parentRange = baseComp->getSourceRange();
    for (auto *nestedComp : memberTR->getMemberComponents()) {
      result = resolveQualifiedIdentTypeRepr(resolution.withOptions(options),
                                             silContext, result, parentRange,
                                             nestedComp);
      if (result->hasError())
        return ErrorType::get(result->getASTContext());

      parentRange.End = nestedComp->getEndLoc();
    }
  }

  auto lastComp = repr->getLastComponent();

  // Diagnose an error if the last component's generic arguments are missing.
  if (result->is<UnboundGenericType>() &&
      !isa<GenericIdentTypeRepr>(lastComp) &&
      !resolution.getUnboundTypeOpener() &&
      !options.is(TypeResolverContext::TypeAliasDecl) &&
      !options.is(TypeResolverContext::ExtensionBinding)) {

    if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
      // Tailored diagnostic for custom attributes.
      if (options.is(TypeResolverContext::CustomAttr)) {
        auto &ctx = resolution.getASTContext();
        ctx.Diags.diagnose(lastComp->getNameLoc(), diag::unknown_attribute,
                           lastComp->getNameRef().getBaseIdentifier().str());

        return ErrorType::get(ctx);
      }

      diagnoseUnboundGenericType(result,
                                 lastComp->getNameLoc().getBaseNameLoc());
    }

    return ErrorType::get(result->getASTContext());
  }

  if (auto moduleTy = result->getAs<ModuleType>()) {
    // Allow module types only if flag is specified.
    if (options.contains(TypeResolutionFlags::AllowModule))
      return moduleTy;
    // Otherwise, emit an error.
    if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
      auto moduleName = moduleTy->getModule()->getName();
      diagnose(lastComp->getNameLoc(), diag::cannot_find_type_in_scope,
               DeclNameRef(moduleName));
      diagnose(lastComp->getNameLoc(), diag::note_module_as_type, moduleName);
    }
    lastComp->setInvalid();
    return ErrorType::get(getASTContext());
  }

    auto *dc = getDeclContext();
    auto &ctx = getASTContext();

    if (ctx.LangOpts.hasFeature(Feature::ImplicitSome) &&
        options.isConstraintImplicitExistential()) {
      // Check whether this type is an implicit opaque result type.
      if (auto *opaqueDecl = dyn_cast<OpaqueTypeDecl>(getDeclContext())) {
        if (auto ordinal = opaqueDecl->getAnonymousOpaqueParamOrdinal(repr)) {
          diagnoseDisallowedExistential(repr);
          return getOpaqueArchetypeIdentity(opaqueDecl, *ordinal);
        }
      }

      // Check whether any of the generic parameters in the context represents
      // this opaque type. If so, return that generic parameter.
      if (auto declDC = dc->getAsDecl()) {
        if (auto genericContext = declDC->getAsGenericContext()) {
          if (auto genericParams = genericContext->getGenericParams()) {
            for (auto genericParam : *genericParams) {
              if (genericParam->getOpaqueTypeRepr() == repr)
                return genericParam->getDeclaredInterfaceType();
            }
          }
        }
      }
    }

  if (result->isConstraintType() &&
      options.isConstraintImplicitExistential()) {
    return ExistentialType::get(result);
  }

  if (!options.isConstraintImplicitExistential()) {
    // Imported existential typealiases, e.g. id<P>, can be
    // used as constraints by extracting the underlying protocol
    // types.
    auto *typeAlias = dyn_cast<TypeAliasType>(result.getPointer());
    if (typeAlias && typeAlias->is<ExistentialType>() &&
        typeAlias->getDecl()->hasClangNode()) {
      auto constraint = typeAlias->getAs<ExistentialType>()->getConstraintType();

      // FIXME: Hack! CFTypeRef, which is a typealias to AnyObject,
      // has special handling when printing as ObjC, so the typealias
      // sugar needs to be preserved. See `isCFTypeRef` in PrintAsClang.cpp.
      //
      // We can eliminate this hack by fixing the issues with PR #41147,
      // which added logic to the ClangImporter for when to import as an
      // existential versus a constraint, but it was reverted in #41207
      // because it was missing some cases of wrapping in ExistentialType.
      auto module = typeAlias->getDecl()->getDeclContext()->getParentModule();
      if ((module->getName().is("Foundation") ||
          module->getName().is("CoreFoundation")) &&
          typeAlias->getDecl()->getName() == getASTContext().getIdentifier("CFTypeRef")) {
        return TypeAliasType::get(typeAlias->getDecl(),
                                  typeAlias->getParent(),
                                  typeAlias->getSubstitutionMap(),
                                  constraint);
      }

      return constraint;
    }
  }

  // move-only types must have an ownership specifier when used as a parameter of a function.
  if (result->isPureMoveOnly())
    diagnoseMoveOnlyMissingOwnership(repr, options);

  // Hack to apply context-specific @escaping to a typealias with an underlying
  // function type.
  if (result->is<FunctionType>())
    result = applyNonEscapingIfNecessary(result, options);

  return result;
}

NeverNullType
TypeResolver::resolveOwnershipTypeRepr(OwnershipTypeRepr *repr,
                                       TypeResolutionOptions options) {
  auto ownershipRepr = dyn_cast<OwnershipTypeRepr>(repr);
  // ownership is only valid for (non-Subscript and non-EnumCaseDecl)
  // function parameters.
  if (!options.is(TypeResolverContext::FunctionInput) ||
      options.hasBase(TypeResolverContext::SubscriptDecl) ||
      options.hasBase(TypeResolverContext::EnumElementDecl)) {

    decltype(diag::attr_only_on_parameters) diagID;
    if (options.getBaseContext() == TypeResolverContext::SubscriptDecl) {
      diagID = diag::attr_not_on_subscript_parameters;
    } else if (options.is(TypeResolverContext::VariadicFunctionInput)) {
      diagID = diag::attr_not_on_variadic_parameters;
    } else {
      diagID = diag::attr_only_on_parameters;
    }
    StringRef name;
    if (ownershipRepr) {
      name = ownershipRepr->getSpecifierSpelling();
    }
    diagnoseInvalid(repr, repr->getSpecifierLoc(), diagID, name);
    return ErrorType::get(getASTContext());
  }

  if (ownershipRepr && ownershipRepr->getSpecifier() == ParamSpecifier::InOut
      && !isa<ImplicitlyUnwrappedOptionalTypeRepr>(repr->getBase())) {
    // Anything within an inout isn't a parameter anymore.
    options.setContext(TypeResolverContext::InoutFunctionInput);
  }

  // Remember that we've seen an ownership specifier for this base type.
  options |= TypeResolutionFlags::HasOwnership;

  auto result = resolveType(repr->getBase(), options);
  if (result->hasError())
    return result;

  // Check for illegal combinations of ownership specifiers and types.
  switch (ownershipRepr->getSpecifier()) {
  case ParamSpecifier::Default:
  case ParamSpecifier::InOut:
  case ParamSpecifier::LegacyShared:
  case ParamSpecifier::LegacyOwned:
  case ParamSpecifier::Borrowing:
    break;
  case ParamSpecifier::Consuming:
    if (auto *fnTy = result->getAs<FunctionType>()) {
      if (fnTy->isNoEscape()) {
        diagnoseInvalid(ownershipRepr, ownershipRepr->getLoc(),
                        diag::ownership_specifier_nonescaping_closure,
                        ownershipRepr->getSpecifierSpelling());
        return ErrorType::get(getASTContext());
      }
    }
    break;
  }

  return result;
}

NeverNullType
TypeResolver::resolveIsolatedTypeRepr(IsolatedTypeRepr *repr,
                                      TypeResolutionOptions options) {
  // isolated is only value for non-EnumCaseDecl parameters.
  if (!options.is(TypeResolverContext::FunctionInput) ||
      options.hasBase(TypeResolverContext::EnumElementDecl)) {
    diagnoseInvalid(
        repr, repr->getSpecifierLoc(), diag::attr_only_on_parameters,
        "isolated");
    return ErrorType::get(getASTContext());
  }

  Type type = resolveType(repr->getBase(), options);

  // isolated parameters must be of actor type
  if (!type->hasTypeParameter() && !type->isActorType() && !type->hasError()) {
    diagnoseInvalid(
        repr, repr->getSpecifierLoc(), diag::isolated_parameter_not_actor, type);
    return ErrorType::get(type);
  }

  return type;
}

NeverNullType
TypeResolver::resolveCompileTimeConstTypeRepr(CompileTimeConstTypeRepr *repr,
                                              TypeResolutionOptions options) {
  // TODO: more diagnostics
  return resolveType(repr->getBase(), options);
}

NeverNullType TypeResolver::resolveArrayType(ArrayTypeRepr *repr,
                                             TypeResolutionOptions options) {
  auto baseTy = resolveType(repr->getBase(), options.withoutContext());
  if (baseTy->hasError()) {
    return ErrorType::get(getASTContext());
  }

  ASTContext &ctx = getASTContext();
  // If the standard library isn't loaded, we ought to let the user know
  // something has gone terribly wrong, since the rest of the compiler is going
  // to assume it can canonicalize [T] to Array<T>.
  if (!ctx.getArrayDecl()) {
    ctx.Diags.diagnose(repr->getBrackets().Start,
                       diag::sugar_type_not_found, 0);
    return ErrorType::get(ctx);
  }

  // do not allow move-only types in an array
  if (diagnoseMoveOnlyGeneric(repr,
                              ctx.getArrayDecl()->getDeclaredInterfaceType(),
                              baseTy)) {
    return ErrorType::get(ctx);
  }

  return ArraySliceType::get(baseTy);
}

NeverNullType
TypeResolver::resolveDictionaryType(DictionaryTypeRepr *repr,
                                    TypeResolutionOptions options) {
  auto argOptions = options.withoutContext().withContext(
      TypeResolverContext::GenericArgument);

  auto keyTy = resolveType(repr->getKey(), argOptions);
  if (keyTy->hasError()) {
    return ErrorType::get(getASTContext());
  }

  auto valueTy = resolveType(repr->getValue(), argOptions);
  if (valueTy->hasError()) {
    return ErrorType::get(getASTContext());
  }

  auto *const dictDecl = getASTContext().getDictionaryDecl();
  if (!dictDecl) {
    getASTContext().Diags.diagnose(repr->getBrackets().Start,
                                   diag::sugar_type_not_found, 3);
    return ErrorType::get(getASTContext());
  }

  if (!resolution.applyUnboundGenericArguments(
          dictDecl, nullptr, repr->getStartLoc(), {keyTy, valueTy})) {
    assert(getASTContext().Diags.hadAnyError());
    return ErrorType::get(getASTContext());
  }
  return DictionaryType::get(keyTy, valueTy);
}

NeverNullType TypeResolver::resolveOptionalType(OptionalTypeRepr *repr,
                                                TypeResolutionOptions options) {
  TypeResolutionOptions elementOptions = options.withoutContext(true);
  elementOptions.setContext(TypeResolverContext::ImmediateOptionalTypeArgument);
  ASTContext &ctx = getASTContext();

  auto baseTy = resolveType(repr->getBase(), elementOptions);
  if (baseTy->hasError()) {
    return ErrorType::get(ctx);
  }

  auto optionalTy = TypeChecker::getOptionalType(repr->getQuestionLoc(),
                                                 baseTy);
  if (optionalTy->hasError()) {
    return ErrorType::get(ctx);
  }

  // do not allow move-only types in an optional
  if (diagnoseMoveOnlyGeneric(repr,
                              ctx.getOptionalDecl()->getDeclaredInterfaceType(),
                              baseTy)) {
    return ErrorType::get(ctx);
  }

  return optionalTy;
}

NeverNullType TypeResolver::resolveImplicitlyUnwrappedOptionalType(
    ImplicitlyUnwrappedOptionalTypeRepr *repr, TypeResolutionOptions options,
    bool isDirect) {
  ASTContext &ctx = getASTContext();
  TypeResolutionFlags allowIUO = TypeResolutionFlags::SILType;

  bool doDiag = false;
  switch (options.getContext()) {
  case TypeResolverContext::None:
  case TypeResolverContext::InoutFunctionInput:
    if (!isDirect || !(options & allowIUO))
      doDiag = true;
    break;
  case TypeResolverContext::FunctionInput:
  case TypeResolverContext::FunctionResult:
  case TypeResolverContext::PatternBindingDecl:
    doDiag = !isDirect;
    break;
  case TypeResolverContext::PackElement:
  case TypeResolverContext::TupleElement:
  case TypeResolverContext::GenericArgument:
  case TypeResolverContext::ProtocolGenericArgument:
  case TypeResolverContext::VariadicFunctionInput:
  case TypeResolverContext::ForEachStmt:
  case TypeResolverContext::ExtensionBinding:
  case TypeResolverContext::ExplicitCastExpr:
  case TypeResolverContext::SubscriptDecl:
  case TypeResolverContext::EnumElementDecl:
  case TypeResolverContext::MacroDecl:
  case TypeResolverContext::EnumPatternPayload:
  case TypeResolverContext::TypeAliasDecl:
  case TypeResolverContext::GenericTypeAliasDecl:
  case TypeResolverContext::GenericRequirement:
  case TypeResolverContext::ExistentialConstraint:
  case TypeResolverContext::SameTypeRequirement:
  case TypeResolverContext::ProtocolMetatypeBase:
  case TypeResolverContext::MetatypeBase:
  case TypeResolverContext::ImmediateOptionalTypeArgument:
  case TypeResolverContext::InExpression:
  case TypeResolverContext::EditorPlaceholderExpr:
  case TypeResolverContext::AbstractFunctionDecl:
  case TypeResolverContext::ClosureExpr:
  case TypeResolverContext::Inherited:
  case TypeResolverContext::GenericParameterInherited:
  case TypeResolverContext::AssociatedTypeInherited:
  case TypeResolverContext::CustomAttr:
    doDiag = true;
    break;
  }

  if (doDiag && !options.contains(TypeResolutionFlags::SilenceErrors)) {
    // Prior to Swift 5, we allow 'as T!' and turn it into a disjunction.
    if (ctx.isSwiftVersionAtLeast(5)) {
      // Mark this repr as invalid. This is the only way to indicate that
      // something went wrong without supressing checking other reprs in
      // the same type. For example:
      //
      // struct S<T, U> { ... }
      //
      // _ = S<Int!, String!>(...)
      //
      // Compiler should diagnose both `Int!` and `String!` as invalid,
      // but returning `ErrorType` from here would stop type resolution
      // after `Int!`.
      repr->setInvalid();

      diagnose(repr->getStartLoc(),
               diag::implicitly_unwrapped_optional_in_illegal_position)
          .fixItReplace(repr->getExclamationLoc(), "?");
    } else if (options.is(TypeResolverContext::ExplicitCastExpr)) {
      diagnose(
          repr->getStartLoc(),
          diag::implicitly_unwrapped_optional_deprecated_in_this_position);
    } else {
      diagnose(
          repr->getStartLoc(),
          diag::implicitly_unwrapped_optional_in_illegal_position_interpreted_as_optional)
          .fixItReplace(repr->getExclamationLoc(), "?");
    }
  }

  TypeResolutionOptions elementOptions = options.withoutContext(true);
  elementOptions.setContext(TypeResolverContext::ImmediateOptionalTypeArgument);

  auto baseTy = resolveType(repr->getBase(), elementOptions);
  if (baseTy->hasError()) {
    return ErrorType::get(ctx);
  }

  auto uncheckedOptionalTy =
      TypeChecker::getOptionalType(repr->getExclamationLoc(), baseTy);
  if (uncheckedOptionalTy->hasError()) {
    return ErrorType::get(ctx);
  }

  // do not allow move-only types in an implicitly-unwrapped optional
  if (diagnoseMoveOnlyGeneric(repr,
                              ctx.getOptionalDecl()->getDeclaredInterfaceType(),
                              baseTy)) {
    return ErrorType::get(ctx);
  }

  return uncheckedOptionalTy;
}

NeverNullType TypeResolver::resolveVarargType(VarargTypeRepr *repr,
                                              TypeResolutionOptions options) {
  auto element = resolveType(repr->getElementType(), options);

  // Non-pack variadic parameters can only appear in variadic function
  // parameter types.
  if (options.getContext() != TypeResolverContext::VariadicFunctionInput) {
    diagnose(repr->getLoc(), diag::vararg_not_allowed)
      .highlight(repr->getSourceRange());
  }

  // do not allow move-only types as the element of a vararg
  if (element->isPureMoveOnly()) {
    diagnoseInvalid(repr, repr->getLoc(), diag::noncopyable_generics_variadic,
                    element);
    return ErrorType::get(getASTContext());
  }

  return element;
}

NeverNullType TypeResolver::resolvePackType(PackTypeRepr *repr,
                                            TypeResolutionOptions options,
                                            bool silDirect) {
  // This form is currently only allowed in SIL, so we're lax about
  // where we allow this.  If this is ever made a proper language feature,
  // it should only be allowed in contexts where an expansion would be
  // allowed and where structure could be ambiguous.  That really just
  // means generic argument lists.
  //
  // Its presence should also affect how arguments are implicitly grouped
  // into packs, of course.

  auto elementReprs = repr->getElements();
  SmallVector<Type, 8> elementTypes;
  elementTypes.reserve(elementReprs.size());

  auto elementOptions = options;
  elementOptions.setContext(TypeResolverContext::PackElement);

  for (auto elementRepr : elementReprs) {
    auto elementType = resolveType(elementRepr, elementOptions);
    elementTypes.push_back(elementType);
  }

  if (options & TypeResolutionFlags::SILType) {
    SmallVector<CanType, 8> canElementTypes;
    canElementTypes.reserve(elementTypes.size());
    for (auto elementType : elementTypes)
      canElementTypes.push_back(elementType->getCanonicalType());

    SILPackType::ExtInfo extInfo(/*indirect*/ !silDirect);
    return SILPackType::get(getASTContext(), extInfo, canElementTypes);
  } else {
    return PackType::get(getASTContext(), elementTypes);
  }
}

NeverNullType TypeResolver::resolvePackExpansionType(PackExpansionTypeRepr *repr,
                                                     TypeResolutionOptions options) {
  auto &ctx = getASTContext();

  auto elementOptions = options;
  elementOptions |= TypeResolutionFlags::AllowPackReferences;

  auto elementResolution =
      resolution.withoutPackElementOpener().withOptions(elementOptions);
  auto patternType =
      elementResolution.resolveType(repr->getPatternType(), silContext);
  if (patternType->hasError())
    return ErrorType::get(ctx);

  // Find the first type parameter pack and use that as the count type.
  SmallVector<Type, 2> rootParameterPacks;
  patternType->getTypeParameterPacks(rootParameterPacks);

  if (rootParameterPacks.empty()) {
    // The pattern type must contain at least one pack reference.
    diagnose(repr->getLoc(), diag::expansion_not_variadic, patternType)
      .highlight(repr->getSourceRange());
    return ErrorType::get(ctx);
  }

  PackExpansionType *result{};
  GenericSignature genericSig;
  Type shapeType;
  if (resolution.getStage() == TypeResolutionStage::Interface) {
    genericSig = resolution.getGenericSignature();
    shapeType = genericSig->getReducedShape(rootParameterPacks[0]);
    result = PackExpansionType::get(patternType, shapeType);
  } else {
    result = PackExpansionType::get(patternType, rootParameterPacks[0]);
  }

  // We might not allow variadic expansions here at all.
  if (!options.isPackExpansionSupported()) {
    diagnose(repr->getLoc(), diag::expansion_not_allowed, result);
    return ErrorType::get(ctx);
  }

  if (resolution.getStage() == TypeResolutionStage::Interface) {
    for (auto type : rootParameterPacks) {
      if (!genericSig->haveSameShape(type, shapeType)) {
        ctx.Diags.diagnose(repr->getLoc(), diag::expansion_not_same_shape,
                           result, shapeType, type);
      }
    }
  }

  return result;
}

NeverNullType TypeResolver::resolvePackElement(PackElementTypeRepr *repr,
                                               TypeResolutionOptions options) {
  auto &ctx = getASTContext();
  options |= TypeResolutionFlags::FromPackReference;

  auto packReference = resolveType(repr->getPackType(), options);

  // If we already failed, don't diagnose again.
  if (packReference->hasError())
    return ErrorType::get(ctx);

  if (!packReference->isParameterPack()) {
    auto diag =
        ctx.Diags.diagnose(repr->getLoc(), diag::each_non_pack, packReference);
    bool addEachFixitApplied = false;
    if (auto *packIdent = dyn_cast<IdentTypeRepr>(repr->getPackType())) {
      if (auto *packIdentBinding = packIdent->getBoundDecl()) {
        if (packIdentBinding->getLoc().isValid()) {
          diag.fixItInsert(packIdentBinding->getLoc(), "each ");
          addEachFixitApplied = true;
        }
      }
    }
    if (const auto eachLoc = repr->getEachLoc();
        !addEachFixitApplied && eachLoc.isValid()) {
      const auto eachLocEnd =
          Lexer::getLocForEndOfToken(ctx.SourceMgr, eachLoc);
      diag.fixItRemoveChars(eachLoc, eachLocEnd);
    }
    return packReference;
  }

  if (!options.contains(TypeResolutionFlags::AllowPackReferences) &&
      !options.contains(TypeResolutionFlags::SILMode)) {
    ctx.Diags.diagnose(repr->getLoc(),
                       diag::pack_reference_outside_expansion,
                       packReference);
    return ErrorType::get(ctx);
  }

  // Open the pack reference to an element archetype if requested.
  if (auto openPackElement = resolution.getPackElementOpener()) {
    auto *env = resolution.getGenericSignature().getGenericEnvironment();
    return openPackElement(env->mapTypeIntoContext(packReference), repr);
  }

  return packReference;
}

NeverNullType TypeResolver::resolveTupleType(TupleTypeRepr *repr,
                                             TypeResolutionOptions options) {
  auto &ctx = getASTContext();

  SmallVector<TupleTypeElt, 8> elements;
  elements.reserve(repr->getNumElements());

  llvm::SmallDenseSet<Identifier> seenEltNames;
  seenEltNames.reserve(repr->getNumElements());

  auto elementOptions = options;
  if (!repr->isParenType()) {
    elementOptions = elementOptions.withoutContext(true);
    elementOptions = elementOptions.withContext(TypeResolverContext::TupleElement);
  }

  bool hadError = false;
  bool foundDupLabel = false;
  llvm::Optional<unsigned> moveOnlyElementIndex = llvm::None;
  for (unsigned i = 0, end = repr->getNumElements(); i != end; ++i) {
    auto *tyR = repr->getElementType(i);

    auto ty = resolveType(tyR, elementOptions);
    if (ty->hasError()) {
      hadError = true;
    }
    // Tuples with move-only elements aren't yet supported.
    // Track the presence of a noncopyable field for diagnostic purposes.
    // We don't need to re-diagnose if a tuple contains another tuple, though,
    // since we should've diagnosed the inner tuple already.
    if (ty->isPureMoveOnly() && !moveOnlyElementIndex.has_value()
        && !isa<TupleTypeRepr>(tyR)) {
      moveOnlyElementIndex = i;
    }

    auto eltName = repr->getElementName(i);

    elements.emplace_back(ty, eltName);

    if (eltName.empty())
      continue;

    if (ty->is<PackExpansionType>()) {
      diagnose(repr->getElementNameLoc(i), diag::tuple_pack_element_label);
      hadError = true;
    }

    if (seenEltNames.count(eltName) == 1) {
      foundDupLabel = true;
    }

    seenEltNames.insert(eltName);
  }

  if (hadError)
    return ErrorType::get(ctx);

  // Tuples with duplicate element labels are not permitted
  if (foundDupLabel) {
    diagnose(repr->getLoc(), diag::tuple_duplicate_label);
  }

  if (options.contains(TypeResolutionFlags::SILType)) {
    if (repr->isParenType())
      return ParenType::get(ctx, elements[0].getType());
  } else {
    // Single-element labeled tuples are not permitted outside of declarations
    // or SIL, either.
    if (elements.size() == 1 && elements[0].hasName() &&
        !elements[0].getType()->is<PackExpansionType>() &&
        !(options & TypeResolutionFlags::SILType)) {
      diagnose(repr->getElementNameLoc(0), diag::tuple_single_element)
        .fixItRemoveChars(repr->getElementNameLoc(0),
                          repr->getElementType(0)->getStartLoc());

      elements[0] = TupleTypeElt(elements[0].getType());
    }

    if (elements.size() == 1 && !elements[0].hasName() &&
        !elements[0].getType()->is<PackExpansionType>())
      return ParenType::get(ctx, elements[0].getType());
  }
  
  if (moveOnlyElementIndex.has_value()
      && !options.contains(TypeResolutionFlags::SILType)
      && !ctx.LangOpts.hasFeature(Feature::MoveOnlyTuples)) {
    auto noncopyableTy = elements[*moveOnlyElementIndex].getType();
    auto loc = repr->getElementType(*moveOnlyElementIndex)->getLoc();
    assert(!noncopyableTy->is<TupleType>() && "will use poor wording");
    diagnose(loc, diag::tuple_move_only_not_supported, noncopyableTy);
  }

  return TupleType::get(elements, ctx);
}

NeverNullType
TypeResolver::resolveCompositionType(CompositionTypeRepr *repr,
                                     TypeResolutionOptions options) {

  // Note that the superclass type will appear as part of one of the
  // types in 'Members', so it's not used when constructing the
  // fully-realized type below -- but we just record it to make sure
  // there is only one superclass.
  Type SuperclassType;
  SmallVector<Type, 4> Members;

  // Whether we saw at least one protocol. A protocol composition
  // must either be empty (in which case it is Any or AnyObject),
  // or if it has a superclass constraint, have at least one protocol.
  bool HasProtocol = false;

  auto checkSuperclass = [&](SourceLoc loc, Type t) -> bool {
    if (SuperclassType && !SuperclassType->isEqual(t)) {
      diagnose(loc, diag::protocol_composition_one_class, t,
               SuperclassType);
      return true;
    }

    SuperclassType = t;
    return false;
  };

  bool IsInvalid = false;

  for (auto tyR : repr->getTypes()) {
    auto ty = resolveType(tyR,
        options.withContext(TypeResolverContext::GenericRequirement));
    if (ty->hasError()) return ty;

    auto nominalDecl = ty->getAnyNominal();
    if (isa_and_nonnull<ClassDecl>(nominalDecl)) {
      if (checkSuperclass(tyR->getStartLoc(), ty))
        continue;

      Members.push_back(ty);
      continue;
    }

    // FIXME: Support compositions involving parameterized protocol types,
    // like 'any Collection<String> & Sendable', etc.
    if (ty->isConstraintType()) {
      if (ty->is<ProtocolType>()) {
        HasProtocol = true;
        Members.push_back(ty);
        continue;
      }

      if (ty->is<ParameterizedProtocolType>() &&
          !options.isConstraintImplicitExistential() &&
          options.getContext() != TypeResolverContext::ExistentialConstraint) {
        HasProtocol = true;
        Members.push_back(ty);
        continue;
      }

      if (ty->is<ProtocolCompositionType>()) {
        auto layout = ty->getExistentialLayout();
        if (auto superclass = layout.explicitSuperclass)
          if (checkSuperclass(tyR->getStartLoc(), superclass))
            continue;
        if (!layout.getProtocols().empty())
          HasProtocol = true;

        Members.push_back(ty);
        continue;
      }
    }

    diagnose(tyR->getStartLoc(),
             diag::invalid_protocol_composition_member,
             ty);

    IsInvalid = true;
  }

  if (IsInvalid) {
    repr->setInvalid();
    return ErrorType::get(getASTContext());
  }

  // Avoid confusing diagnostics ('MyClass' not convertible to 'MyClass',
  // etc) by collapsing a composition consisting of a single class down
  // to the class itself.
  if (SuperclassType && !HasProtocol)
    return SuperclassType;

  // In user-written types, AnyObject constraints always refer to the
  // AnyObject type in the standard library.
  auto composition =
      ProtocolCompositionType::get(getASTContext(), Members,
                                   /*HasExplicitAnyObject=*/false);
  if (options.isConstraintImplicitExistential()) {
    return ExistentialType::get(composition);
  }
  return composition;
}

NeverNullType
TypeResolver::resolveExistentialType(ExistentialTypeRepr *repr,
                                     TypeResolutionOptions options) {
  auto constraintType = resolveType(repr->getConstraint(),
      options.withContext(TypeResolverContext::ExistentialConstraint));
  if (constraintType->is<ExistentialMetatypeType>())
    return constraintType;

  // If we already failed, don't diagnose again.
  if (constraintType->hasError())
    return ErrorType::get(getASTContext());

  //TO-DO: generalize this and emit the same erorr for some P?
  if (!constraintType->isConstraintType()) {
    // Emit a tailored diagnostic for the incorrect optional
    // syntax 'any P?' with a fix-it to add parenthesis.
    auto wrapped = constraintType->getOptionalObjectType();
    if (wrapped && (wrapped->is<ExistentialType>() ||
                    wrapped->is<ExistentialMetatypeType>())) {
      std::string fix;
      llvm::raw_string_ostream OS(fix);
      constraintType->print(OS, PrintOptions::forDiagnosticArguments());
      diagnose(repr->getLoc(), diag::incorrect_optional_any,
               constraintType)
        .fixItReplace(repr->getSourceRange(), fix);
      return constraintType;
    }
    
    // Diagnose redundant `any` on an already existential type e.g. any (any P)
    // with a fix-it to remove first any.
    if (constraintType->is<ExistentialType>()) {
      diagnose(repr->getLoc(), diag::redundant_any_in_existential,
               ExistentialType::get(constraintType))
          .fixItRemove(repr->getAnyLoc());
      return constraintType;
    }

    diagnose(repr->getLoc(), diag::any_not_existential,
             constraintType->isTypeParameter(), constraintType)
        .fixItRemove(repr->getAnyLoc());
    return constraintType;
  }

  return ExistentialType::get(constraintType);
}

NeverNullType TypeResolver::resolveMetatypeType(MetatypeTypeRepr *repr,
                                                TypeResolutionOptions options) {
  // The instance type of a metatype is always abstract, not SIL-lowered.
  if (options.is(TypeResolverContext::ExistentialConstraint))
    options |= TypeResolutionFlags::DisallowOpaqueTypes;
  options = options.withContext(TypeResolverContext::MetatypeBase);
  auto ty = resolveType(repr->getBase(), options);
  if (ty->hasError()) {
    return ErrorType::get(getASTContext());
  }

  llvm::Optional<MetatypeRepresentation> storedRepr;

  // In SIL mode, a metatype must have a @thin, @thick, or
  // @objc_metatype attribute, so metatypes should have been lowered
  // in resolveAttributedType.
  if (options & TypeResolutionFlags::SILType) {
    diagnose(repr->getStartLoc(), diag::sil_metatype_without_repr);
    storedRepr = MetatypeRepresentation::Thick;
  }

  return buildMetatypeType(repr, ty, storedRepr);
}

NeverNullType TypeResolver::buildMetatypeType(
    MetatypeTypeRepr *repr, Type instanceType,
    llvm::Optional<MetatypeRepresentation> storedRepr) {
  // If the instance type is an existential metatype, figure out if
  // the syntax is of the form '(any <protocol metatype>).Type'. In
  // this case, type resolution should produce the static metatype
  // of that existential metatype, versus another existential metatype
  // via the old '<protocol metatype>.Type' syntax.
  if (instanceType->is<ExistentialMetatypeType>()) {
    // First, look for the paren type.
    auto *tuple = dyn_cast<TupleTypeRepr>(repr->getBase());
    if (tuple && tuple->isParenType()) {
      // Then, look through parens for the 'any' keyword.
      auto *element = tuple->getWithoutParens();
      if (auto *existential = dyn_cast<ExistentialTypeRepr>(element)) {
        // Finally, look for a constraint ending with '.Type'. Assume the
        // base is a protocol, otherwise resolveExistentialType would
        // have emitted an error message and returned the concrete type
        // instead of an existential metatype.
        auto *constraint = existential->getConstraint()->getWithoutParens();
        if (isa<MetatypeTypeRepr>(constraint)) {
          return MetatypeType::get(instanceType, storedRepr);
        }
      }
    }
  }

  if (instanceType->isAnyExistentialType() &&
      !instanceType->is<ExistentialType>()) {
    // TODO: diagnose invalid representations?
    return ExistentialMetatypeType::get(instanceType, storedRepr);
  } else {
    return MetatypeType::get(instanceType, storedRepr);
  }
}

NeverNullType TypeResolver::resolveProtocolType(ProtocolTypeRepr *repr,
                                                TypeResolutionOptions options) {
  // The instance type of a metatype is always abstract, not SIL-lowered.
  auto ty = resolveType(repr->getBase(),
      options.withContext(TypeResolverContext::ProtocolMetatypeBase));
  if (ty->hasError()) {
    return ErrorType::get(getASTContext());
  }

  llvm::Optional<MetatypeRepresentation> storedRepr;

  // In SIL mode, a metatype must have a @thin, @thick, or
  // @objc_metatype attribute, so metatypes should have been lowered
  // in resolveAttributedType.
  if (options & TypeResolutionFlags::SILType) {
    diagnose(repr->getStartLoc(), diag::sil_metatype_without_repr);
    storedRepr = MetatypeRepresentation::Thick;
  }

  return buildProtocolType(repr, ty, storedRepr);
}

NeverNullType TypeResolver::buildProtocolType(
    ProtocolTypeRepr *repr, Type instanceType,
    llvm::Optional<MetatypeRepresentation> storedRepr) {
  if (!instanceType->isAnyExistentialType()) {
    diagnose(repr->getProtocolLoc(), diag::dot_protocol_on_non_existential,
             instanceType);
    return ErrorType::get(getASTContext());
  }

  return MetatypeType::get(instanceType, storedRepr);
}

Type TypeChecker::substMemberTypeWithBase(ModuleDecl *module,
                                          TypeDecl *member,
                                          Type baseTy,
                                          bool useArchetypes) {
  Type sugaredBaseTy = baseTy;

  // For type members of a base class, make sure we use the right
  // derived class as the parent type. If the base type is an error
  // type, we have an invalid extension, so do nothing.
  if (!baseTy->is<ErrorType>()) {
    if (auto *ownerClass = member->getDeclContext()->getSelfClassDecl()) {
      baseTy = baseTy->getSuperclassForDecl(ownerClass, useArchetypes);
    }
  }

  if (baseTy->is<ModuleType>()) {
    baseTy = Type();
    sugaredBaseTy = Type();
  }

  // The declared interface type for a generic type will have the type
  // arguments; strip them off.
  if (auto *nominalDecl = dyn_cast<NominalTypeDecl>(member)) {
    // If the base type is not a nominal type, we might be looking up a
    // nominal member of a generic parameter. This is not supported right
    // now, but at least don't crash.
    if (member->getDeclContext()->getSelfProtocolDecl())
      return nominalDecl->getDeclaredType();

    if (!isa<ProtocolDecl>(nominalDecl) &&
        nominalDecl->getGenericParams()) {
      return UnboundGenericType::get(
          nominalDecl, baseTy,
          nominalDecl->getASTContext());
    }

    if (baseTy && baseTy->is<ErrorType>())
      return baseTy;

    return NominalType::get(
        nominalDecl, baseTy,
        nominalDecl->getASTContext());
  }

  auto *aliasDecl = dyn_cast<TypeAliasDecl>(member);
  if (aliasDecl) {
    if (aliasDecl->isGeneric()) {
      return UnboundGenericType::get(
          aliasDecl, baseTy,
          aliasDecl->getASTContext());
    }
  }

  Type resultType;
  auto memberType = aliasDecl ? aliasDecl->getUnderlyingType()
                              : member->getDeclaredInterfaceType();
  SubstitutionMap subs;
  if (baseTy) {
    // Cope with the presence of unbound generic types, which are ill-formed
    // at this point but break the invariants of getContextSubstitutionMap().
    if (baseTy->hasUnboundGenericType()) {
      if (memberType->hasTypeParameter())
        return ErrorType::get(memberType);

      return memberType;
    }

    if (baseTy->is<ErrorType>())
      return ErrorType::get(memberType);

    subs = baseTy->getMemberSubstitutionMap(module, member);
    resultType = memberType.subst(subs);
  } else {
    resultType = memberType;
  }

  // If we're referring to a typealias within a generic context, build
  // a sugared alias type.
  if (aliasDecl && (!sugaredBaseTy || !sugaredBaseTy->isAnyExistentialType())) {
    resultType = TypeAliasType::get(aliasDecl, sugaredBaseTy, subs, resultType);
  }

  return resultType;
}

namespace {

class ExistentialTypeVisitor
  : public TypeReprVisitor<ExistentialTypeVisitor>, public ASTWalker
{
  ASTContext &Ctx;
  bool checkStatements;
  bool hitTopStmt;

  unsigned exprCount = 0;
  llvm::SmallVector<TypeRepr *, 4> reprStack;
    
public:
  ExistentialTypeVisitor(ASTContext &ctx, bool checkStatements)
    : Ctx(ctx), checkStatements(checkStatements), hitTopStmt(false) { }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
    reprStack.push_back(T);

    if (T->isInvalid())
      return Action::SkipChildren();
    if (auto memberTR = dyn_cast<MemberTypeRepr>(T)) {
      // Only visit the last component to check, because nested typealiases in
      // existentials are okay.
      visit(memberTR->getLastComponent());
      return Action::SkipChildren();
    }
    // Arbitrary protocol constraints are OK on opaque types.
    if (isa<OpaqueReturnTypeRepr>(T))
      return Action::SkipChildren();

    // Arbitrary protocol constraints are okay for 'any' types.
    if (isa<ExistentialTypeRepr>(T))
      return Action::SkipChildren();

    visit(T);
    return Action::Continue();
  }

  PostWalkAction walkToTypeReprPost(TypeRepr *T) override {
    reprStack.pop_back();
    return Action::Continue();
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    if (checkStatements && !hitTopStmt) {
      hitTopStmt = true;
      return Action::Continue(S);
    }

    return Action::SkipChildren(S);
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    return Action::SkipChildrenIf(checkStatements);
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    ++exprCount;
    return Action::Continue(E);
  }

  PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
    --exprCount;
    return Action::Continue(E);
  }

  void visitTypeRepr(TypeRepr *T) {
    // Do nothing for all TypeReprs except the ones listed below.
  }

  bool existentialNeedsParens(TypeRepr *parent) {
    switch (parent->getKind()) {
    case TypeReprKind::Optional:
    case TypeReprKind::Protocol:
      return true;
    case TypeReprKind::Metatype:
    case TypeReprKind::Attributed:
    case TypeReprKind::Error:
    case TypeReprKind::Function:
    case TypeReprKind::Ownership:
    case TypeReprKind::Composition:
    case TypeReprKind::OpaqueReturn:
    case TypeReprKind::NamedOpaqueReturn:
    case TypeReprKind::Existential:
    case TypeReprKind::SimpleIdent:
    case TypeReprKind::GenericIdent:
    case TypeReprKind::Member:
    case TypeReprKind::Dictionary:
    case TypeReprKind::ImplicitlyUnwrappedOptional:
    case TypeReprKind::Tuple:
    case TypeReprKind::Fixed:
    case TypeReprKind::Self:
    case TypeReprKind::Array:
    case TypeReprKind::SILBox:
    case TypeReprKind::Isolated:
    case TypeReprKind::Placeholder:
    case TypeReprKind::CompileTimeConst:
    case TypeReprKind::Vararg:
    case TypeReprKind::Pack:
    case TypeReprKind::PackExpansion:
    case TypeReprKind::PackElement:
      return false;
    }
  }

  void visitIdentTypeRepr(IdentTypeRepr *T) {
    if (T->isInvalid())
      return;

    if (Ctx.LangOpts.hasFeature(Feature::ImplicitSome)) {
      return;
    }

    // Compute the type repr to attach 'any' to.
    TypeRepr *replaceRepr = T;
    // Insert parens in expression context for '(any P).self'
    bool needsParens = (exprCount != 0);
    if (reprStack.size() > 1) {
      auto parentIt = reprStack.end() - 2;
      needsParens = existentialNeedsParens(*parentIt);

      // Expand to include parenthesis before checking if the parent needs
      // to be replaced.
      while (parentIt != reprStack.begin() &&
             (*parentIt)->getWithoutParens() != *parentIt)
        parentIt -= 1;

      // For existential metatypes, 'any' is applied to the metatype.
      if ((*parentIt)->getKind() == TypeReprKind::Metatype) {
        replaceRepr = *parentIt;
        if (parentIt != reprStack.begin())
          needsParens = existentialNeedsParens(*(parentIt - 1));
      }
    }

    std::string fix;
    llvm::raw_string_ostream OS(fix);
    if (needsParens)
      OS << "(";
    ExistentialTypeRepr existential(SourceLoc(), replaceRepr);
    existential.print(OS);
    if (needsParens)
      OS << ")";

    if (auto *proto = dyn_cast_or_null<ProtocolDecl>(T->getBoundDecl())) {
      if (proto->existentialRequiresAny()) {
        Ctx.Diags.diagnose(T->getNameLoc(),
                           diag::existential_requires_any,
                           proto->getDeclaredInterfaceType(),
                           proto->getDeclaredExistentialType(),
                           /*isAlias=*/false)
            .fixItReplace(replaceRepr->getSourceRange(), fix);
      }
    } else if (auto *alias =
                   dyn_cast_or_null<TypeAliasDecl>(T->getBoundDecl())) {
      auto type = Type(alias->getDeclaredInterfaceType()->getDesugaredType());

      // If this is a type alias to a constraint type, the type
      // alias name must be prefixed with 'any' to be used as an
      // existential type.
      if (type->isConstraintType()) {
        auto layout = type->getExistentialLayout();
        for (auto *protoDecl : layout.getProtocols()) {
          if (!protoDecl->existentialRequiresAny())
            continue;

          Ctx.Diags.diagnose(T->getNameLoc(),
                             diag::existential_requires_any,
                             alias->getDeclaredInterfaceType(),
                             ExistentialType::get(alias->getDeclaredInterfaceType()),
                             /*isAlias=*/true)
              .fixItReplace(replaceRepr->getSourceRange(), fix);
        }
      }
    }
  }

  void visitRequirements(ArrayRef<RequirementRepr> reqts) {
    for (auto reqt : reqts) {
      if (reqt.getKind() == RequirementReprKind::SameType) {
        if (auto *repr = reqt.getFirstTypeRepr())
          repr->walk(*this);
        if (auto *repr = reqt.getSecondTypeRepr())
          repr->walk(*this);
      }
    }
  }
};

} // end anonymous namespace

void TypeChecker::checkExistentialTypes(Decl *decl) {
  if (!decl || decl->isInvalid())
    return;

  // Skip diagnosing existential `any` requirements in swiftinterfaces.
  auto sourceFile = decl->getDeclContext()->getParentSourceFile();
  if (sourceFile && sourceFile->Kind == SourceFileKind::Interface)
    return;

  auto &ctx = decl->getASTContext();
  if (auto *protocolDecl = dyn_cast<ProtocolDecl>(decl)) {
    checkExistentialTypes(ctx, protocolDecl->getTrailingWhereClause());
  } else if (auto *genericDecl = dyn_cast<GenericTypeDecl>(decl)) {
    checkExistentialTypes(ctx, genericDecl->getGenericParams());
    checkExistentialTypes(ctx, genericDecl->getTrailingWhereClause());
    if (auto *typeAlias = dyn_cast<TypeAliasDecl>(decl)) {
      checkExistentialTypes(ctx, typeAlias);
    }
  } else if (auto *assocType = dyn_cast<AssociatedTypeDecl>(decl)) {
    checkExistentialTypes(ctx, assocType->getTrailingWhereClause());
  } else if (auto *extDecl = dyn_cast<ExtensionDecl>(decl)) {
    checkExistentialTypes(ctx, extDecl->getTrailingWhereClause());
  } else if (auto *subscriptDecl = dyn_cast<SubscriptDecl>(decl)) {
    checkExistentialTypes(ctx, subscriptDecl->getGenericParams());
    checkExistentialTypes(ctx, subscriptDecl->getTrailingWhereClause());
  } else if (auto *funcDecl = dyn_cast<AbstractFunctionDecl>(decl)) {
    if (!isa<AccessorDecl>(funcDecl)) {
      checkExistentialTypes(ctx, funcDecl->getGenericParams());
      checkExistentialTypes(ctx, funcDecl->getTrailingWhereClause());
    }
  } else if (auto *macroDecl = dyn_cast<MacroDecl>(decl)) {
    checkExistentialTypes(ctx, macroDecl->getGenericParams());
    checkExistentialTypes(ctx, macroDecl->getTrailingWhereClause());
  } else if (auto *macroExpansionDecl = dyn_cast<MacroExpansionDecl>(decl)) {
    ExistentialTypeVisitor visitor(ctx, /*checkStatements=*/false);
    macroExpansionDecl->getArgs()->walk(visitor);
    for (auto *genArg : macroExpansionDecl->getGenericArgs())
      genArg->walk(visitor);
  }

  if (isa<TypeDecl>(decl) || isa<ExtensionDecl>(decl) ||
      isa<MacroExpansionDecl>(decl))
    return;

  ExistentialTypeVisitor visitor(ctx, /*checkStatements=*/false);
  decl->walk(visitor);
}

void TypeChecker::checkExistentialTypes(ASTContext &ctx, Stmt *stmt,
                                        DeclContext *DC) {
  if (!stmt)
    return;

  // Skip diagnosing existential `any` requirements in swiftinterfaces.
  auto sourceFile = DC->getParentSourceFile();
  if (sourceFile && sourceFile->Kind == SourceFileKind::Interface)
    return;

  ExistentialTypeVisitor visitor(ctx, /*checkStatements=*/true);
  stmt->walk(visitor);
}

void TypeChecker::checkExistentialTypes(ASTContext &ctx,
                                        TypeAliasDecl *typeAlias) {
  if (!typeAlias || !typeAlias->getUnderlyingTypeRepr())
    return;

  // A type alias to a plain constraint type is allowed.
  if (typeAlias->getUnderlyingType()->isConstraintType())
    return;

  ExistentialTypeVisitor visitor(ctx, /*checkStatements=*/true);
  typeAlias->getUnderlyingTypeRepr()->walk(visitor);
}

void TypeChecker::checkExistentialTypes(
    ASTContext &ctx, TrailingWhereClause *whereClause) {
  if (whereClause == nullptr)
    return;

  ExistentialTypeVisitor visitor(ctx, /*checkStatements=*/false);
  visitor.visitRequirements(whereClause->getRequirements());
}

void TypeChecker::checkExistentialTypes(
    ASTContext &ctx, GenericParamList *genericParams) {
  if (genericParams  == nullptr)
    return;

  ExistentialTypeVisitor visitor(ctx, /*checkStatements=*/false);
  visitor.visitRequirements(genericParams->getRequirements());
}

Type CustomAttrTypeRequest::evaluate(Evaluator &eval, CustomAttr *attr,
                                     DeclContext *dc,
                                     CustomAttrTypeKind typeKind) const {
  const TypeResolutionOptions options(TypeResolverContext::PatternBindingDecl);

  OpenUnboundGenericTypeFn unboundTyOpener = nullptr;
  // Property delegates allow their type to be an unbound generic.
  if (typeKind == CustomAttrTypeKind::PropertyWrapper ||
      typeKind == CustomAttrTypeKind::RuntimeMetadata) {
    unboundTyOpener = [](auto unboundTy) {
      // FIXME: Don't let unbound generic types
      // escape type resolution. For now, just
      // return the unbound generic type.
      return unboundTy;
    };
  }

  const auto type = TypeResolution::resolveContextualType(
      attr->getTypeRepr(), dc, options, unboundTyOpener,
      /*placeholderHandler*/ nullptr, /*packElementOpener*/ nullptr);

  // We always require the type to resolve to a nominal type. If the type was
  // not a nominal type, we should have already diagnosed an error via
  // CustomAttrNominalRequest.
  auto checkType = [](Type type) -> bool {
    while (auto *genericDecl = type->getAnyGeneric()) {
      if (isa<NominalTypeDecl>(genericDecl))
        return true;

      auto *aliasDecl = cast<TypeAliasDecl>(genericDecl);
      type = aliasDecl->getUnderlyingType();
    }

    return false;
  };

  if (!checkType(type)) {
    ASTContext &ctx = dc->getASTContext();
    assert(ctx.Diags.hadAnyError());
    return ErrorType::get(ctx);
  }

  return type;
}
