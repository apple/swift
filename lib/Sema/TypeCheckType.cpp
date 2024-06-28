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
#include "TypeCheckInvertible.h"

#include "swift/AST/ASTDemangler.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsParse.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PackExpansionMatcher.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SILLayout.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeLoc.h"
#include "swift/AST/TypeRepr.h"
#include "swift/AST/TypeResolutionStage.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/EnumMap.h"
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
  case TypeResolverContext::ScalarGenericArgument:
  case TypeResolverContext::VariadicGenericArgument:
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
  case TypeResolverContext::Inverted:
    break;
  }

  llvm_unreachable("Invalid type resolution context");
}

Type TypeResolution::resolveDependentMemberType(
    Type baseTy, DeclContext *DC, SourceRange baseRange,
    QualifiedIdentTypeRepr *repr) const {
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
                  repr->getNameRef(), singleType)
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
    // not the protocol 'Self' type. This is invalid and will
    // be diagnosed anyway, but we want to avoid an invariant
    // violation from trying to construct a nominal type with
    // a generic parameter as its parent type.
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

  // Protocols can't appear in constrained extensions, and their where
  // clauses pertain to the requirement signature of the protocol and not
  // its generic signature.
  if (isa<ProtocolDecl>(decl))
    return true;

  auto *dc = decl->getDeclContext();

  const auto genericSig = decl->getGenericSignature();

  if (genericSig.getPointer() ==
      dc->getSelfNominalTypeDecl()->getGenericSignature().getPointer()) {
    return true;
  }

  // Otherwise, our decl has a where clause of its own, or its inside of a
  // constrained extension.
  SourceLoc noteLoc;
  {
    const auto ext = dyn_cast<ExtensionDecl>(dc);
    if (ext && genericSig.getPointer() ==
        ext->getGenericSignature().getPointer()) {
      noteLoc = ext->getLoc();
    } else {
      noteLoc = decl->getLoc();
    }

    if (noteLoc.isInvalid())
      noteLoc = loc;
  }

  const auto subMap = parentTy->getContextSubstitutions(dc);
  const auto substitutions = [&](SubstitutableType *type) -> Type {
    auto result = QueryTypeSubstitutionMap{subMap}(type);
    if (result->hasTypeParameter()) {
      if (contextSig) {
        // Avoid building this generic environment unless we need it.
        auto *genericEnv = contextSig.getGenericEnvironment();
        return genericEnv->mapTypeIntoContext(result);
      }
    }
    return result;
  };

  const auto result = TypeChecker::checkGenericArgumentsForDiagnostics(
      module, genericSig.getRequirements(), substitutions);
  switch (result.getKind()) {
  case CheckRequirementsResult::RequirementFailure:
    if (loc.isValid()) {
      TypeChecker::diagnoseRequirementFailure(
          result.getRequirementFailureInfo(), loc, noteLoc,
          decl->getDeclaredInterfaceType(), genericSig.getGenericParams(),
          substitutions, module);
    }

    return false;
  case CheckRequirementsResult::SubstitutionFailure:
    return false;
  case CheckRequirementsResult::Success:
    return true;
  }
  llvm_unreachable("invalid requirement check type");
}

void swift::diagnoseInvalidGenericArguments(SourceLoc loc,
                                            ValueDecl *decl,
                                            unsigned argCount,
                                            unsigned paramCount,
                                            bool hasParameterPack,
                                            SourceRange angleBrackets) {
  auto &ctx = decl->getASTContext();
  auto &diags = ctx.Diags;

  if (!hasParameterPack) {
    // For generic types without type parameter packs, we require
    // the number of declared generic parameters match the number of
    // arguments.
    if (argCount < paramCount) {
      auto diag = diags
          .diagnose(loc, diag::too_few_generic_arguments, decl->getBaseIdentifier(),
                    argCount, paramCount);
      if (angleBrackets.isValid())
        diag.highlight(angleBrackets);
    } else {
      auto diag = diags
          .diagnose(loc, diag::too_many_generic_arguments, decl->getBaseIdentifier(),
                    argCount, paramCount);
      if (angleBrackets.isValid())
        diag.highlight(angleBrackets);
    }
  } else {
    if (argCount < paramCount - 1) {
      auto diag = diags
          .diagnose(loc, diag::too_few_generic_arguments_pack, decl->getBaseIdentifier(),
                    argCount, paramCount - 1);
      if (angleBrackets.isValid())
        diag.highlight(angleBrackets);
    } else {
      auto diag = diags
          .diagnose(loc, diag::generic_argument_pack_mismatch, decl->getBaseIdentifier());
      if (angleBrackets.isValid())
        diag.highlight(angleBrackets);
    }
  }

  decl->diagnose(diag::kind_declname_declared_here,
                 DescriptiveDeclKind::GenericType, decl->getName());
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
/// \param repr The syntactic representation of \p type, with a possible
/// generic argument list to apply.
/// \returns A BoundGenericType bound to the given arguments, or null on
/// error.
///
/// \see TypeResolution::applyUnboundGenericArguments
static Type applyGenericArguments(Type type,
                                  const TypeResolution &resolution,
                                  SILTypeResolutionContext *silContext,
                                  DeclRefTypeRepr *repr) {
  auto options = resolution.getOptions();
  auto dc = resolution.getDeclContext();
  auto loc = repr->getNameLoc().getBaseNameLoc();

  if (!repr->hasGenericArgList()) {
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
    repr->setInvalid();
    return type;
  }

  auto &ctx = dc->getASTContext();
  auto &diags = ctx.Diags;

  const auto genericArgs = repr->getGenericArgs();

  // Parameterized protocol types have their own code path.
  if (auto *protoType = type->getAs<ProtocolType>()) {
    auto *protoDecl = protoType->getDecl();

    auto assocTypes = protoDecl->getPrimaryAssociatedTypes();
    if (assocTypes.empty()) {
      diags.diagnose(loc, diag::protocol_does_not_have_primary_assoc_type,
                     protoType)
           .fixItRemove(repr->getAngleBrackets());
      if (!protoDecl->isImplicit()) {
        diags.diagnose(protoDecl, diag::decl_declared_here, protoDecl);
      }
      return ErrorType::get(ctx);
    }

    // Make sure we have the right number of generic arguments.
    if (genericArgs.size() != assocTypes.size()) {
      diags.diagnose(loc,
                     diag::parameterized_protocol_type_argument_count_mismatch,
                     protoType, genericArgs.size(), assocTypes.size(),
                     (genericArgs.size() < assocTypes.size()) ? 1 : 0);

      return ErrorType::get(ctx);
    }

    // Disallow opaque types anywhere in the structure of the generic arguments
    // to a parameterized existential type.
    if (options.is(TypeResolverContext::ExistentialConstraint))
      options |= TypeResolutionFlags::DisallowOpaqueTypes;
    auto argOptions = options.withoutContext().withContext(
        TypeResolverContext::ScalarGenericArgument);
    auto genericResolution = resolution.withOptions(argOptions);

    // Resolve the generic arguments.
    SmallVector<Type, 2> argTys;
    for (auto *genericArg : genericArgs) {
      Type argTy = genericResolution.resolveType(genericArg, silContext);
      if (!argTy || argTy->hasError())
        return ErrorType::get(ctx);

      argTys.push_back(argTy);
    }

    auto parameterized =
        ParameterizedProtocolType::get(ctx, protoType, argTys);

    if (resolution.getOptions().isConstraintImplicitExistential() &&
        !ctx.LangOpts.hasFeature(Feature::ImplicitSome)) {
      diags.diagnose(loc, diag::existential_requires_any, parameterized,
                     ExistentialType::get(parameterized),
                     /*isAlias=*/isa<TypeAliasType>(type.getPointer()));
      return ErrorType::get(ctx);
    }

    return parameterized;
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
        SourceRange angles = repr->getAngleBrackets();
        diag.fixItRemoveChars(angles.Start,
                              angles.End.getAdvancedLocOrInvalid(1));
      }

      repr->setInvalid();
    }
    return ErrorType::get(ctx);
  }

  auto *unboundType = type->castTo<UnboundGenericType>();
  auto *decl = unboundType->getDecl();

  auto genericParams = decl->getGenericParams();
  auto hasParameterPack = llvm::any_of(
      *genericParams, [](auto *paramDecl) {
          return paramDecl->isParameterPack();
      });

  // If the type declares at least one parameter pack, allow pack expansions
  // anywhere in the argument list. We'll use the PackMatcher to ensure that
  // everything lines up. Otherwise, don't allow pack expansions to appear
  // at all.
  auto argOptions = options.withoutContext().withContext(
      hasParameterPack
      ? TypeResolverContext::VariadicGenericArgument
      : TypeResolverContext::ScalarGenericArgument);
  auto genericResolution = resolution.withOptions(argOptions);

  // In SIL mode, Optional<T> interprets T as a SIL type.
  if (options.contains(TypeResolutionFlags::SILType)) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
      if (nominal->isOptionalDecl()) {
        genericResolution = resolution;
      }
    }
  }

  // Resolve the types of the generic arguments.
  SmallVector<Type, 2> args;
  for (auto tyR : genericArgs) {
    // Propagate failure.
    Type substTy = genericResolution.resolveType(tyR, silContext);
    if (!substTy || substTy->hasError())
      return ErrorType::get(ctx);

    args.push_back(substTy);
  }

  // Make sure we have the right number of generic arguments.
  if (!hasParameterPack) {
    // For generic types without type parameter packs, we require
    // the number of declared generic parameters match the number of
    // arguments.
    if (genericArgs.size() != genericParams->size()) {
      if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
        diagnoseInvalidGenericArguments(
            loc, decl, genericArgs.size(), genericParams->size(),
            /*hasParameterPack=*/false, repr->getAngleBrackets());
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
    if (matcher.match() || matcher.pairs.size() != params.size()) {
      if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
        diagnoseInvalidGenericArguments(
            loc, decl, genericArgs.size(), genericParams->size(),
            /*hasParameterPack=*/true, repr->getAngleBrackets());
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

  // Construct the substituted type.
  const auto result = resolution.applyUnboundGenericArguments(
      decl, unboundType->getParent(), loc, args);

  // Migration hack.
  bool isMutablePointer;
  if (isPointerToVoid(dc->getASTContext(), result, isMutablePointer)) {
    if (isMutablePointer)
      diags.diagnose(loc, diag::use_of_void_pointer, "Mutable").
        fixItReplace(repr->getSourceRange(), "UnsafeMutableRawPointer");
    else
      diags.diagnose(loc, diag::use_of_void_pointer, "").
        fixItReplace(repr->getSourceRange(), "UnsafeRawPointer");
  }

  if (auto clangDecl = decl->getClangDecl()) {
    if (auto classTemplateDecl =
            dyn_cast<clang::ClassTemplateDecl>(clangDecl)) {
      SmallVector<Type, 2> typesOfGenericArgs;
      for (auto typeRepr : genericArgs) {
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
  if (options.contains(TypeResolutionFlags::SILType)) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
      if (nominal->isOptionalDecl()) {
        skipRequirementsCheck = true;
      }
    }
  }

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
    switch (result.getKind()) {
    case CheckRequirementsResult::RequirementFailure:
      if (loc.isValid()) {
        TypeChecker::diagnoseRequirementFailure(
            result.getRequirementFailureInfo(), loc, noteLoc,
            UnboundGenericType::get(decl, parentTy, getASTContext()),
            genericSig.getGenericParams(), substitutions, module);
      }

      LLVM_FALLTHROUGH;
    case CheckRequirementsResult::SubstitutionFailure:
      return ErrorType::get(getASTContext());
    case CheckRequirementsResult::Success:
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

  ctx.Diags.diagnose(loc, diagCode, typeDecl, parentTy);
}

/// Returns a valid type or ErrorType in case of an error.
static Type resolveTypeDecl(TypeDecl *typeDecl, DeclContext *foundDC,
                            const TypeResolution &resolution,
                            SILTypeResolutionContext *silContext,
                            UnqualifiedIdentTypeRepr *repr) {
  // Resolve the type declaration to a specific type. How this occurs
  // depends on the current context and where the type was found.
  Type type = resolution.resolveTypeInContext(typeDecl, foundDC,
                                              repr->hasGenericArgList());

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
static Type diagnoseUnknownType(const TypeResolution &resolution,
                                Type parentType,
                                SourceRange parentRange,
                                DeclRefTypeRepr *repr,
                                NameLookupOptions lookupOptions) {
  assert(parentType || isa<UnqualifiedIdentTypeRepr>(repr));

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

    if (repr->isSimpleUnqualifiedIdentifier(ctx.Id_Self)) {
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
      auto formalAccess = first->getFormalAccess();
      auto nameLoc = repr->getNameLoc();
      if (formalAccess >= AccessLevel::Public &&
          diagnoseMissingImportForMember(first, dc, nameLoc.getStartLoc()))
        return ErrorType::get(ctx);

      diags.diagnose(nameLoc, diag::candidate_inaccessible, first,
                     formalAccess);

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
    
    // type-casting operators such as 'is' and 'as'.
    if (resolution.getOptions().is(TypeResolverContext::ExplicitCastExpr)) {
      auto lookupResult = TypeChecker::lookupUnqualified(
          dc, repr->getNameRef(), repr->getLoc(), lookupOptions);
      if (!lookupResult.empty()) {
        auto first = lookupResult.front().getValueDecl();
        diags.diagnose(L, diag::cannot_find_type_in_cast_expression, first)
          .highlight(R);
        diags.diagnose(first, diag::decl_declared_here, first);
        return ErrorType::get(ctx);
      }
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
    auto formalAccess = first->getFormalAccess();
    auto nameLoc = repr->getNameLoc();
    if (formalAccess >= AccessLevel::Public &&
        diagnoseMissingImportForMember(first, dc, nameLoc.getStartLoc()))
      return ErrorType::get(ctx);

    diags.diagnose(nameLoc, diag::candidate_inaccessible, first, formalAccess);

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
                    member, parentType)
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
        ctx.Diags.diagnose(typeDecl, diag::decl_declared_here, typeDecl);
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

static void diagnoseGenericArgumentsOnSelf(const TypeResolution &resolution,
                                           UnqualifiedIdentTypeRepr *repr,
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
static Type
resolveUnqualifiedIdentTypeRepr(const TypeResolution &resolution,
                                SILTypeResolutionContext *silContext,
                                UnqualifiedIdentTypeRepr *repr) {
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
      // The issue is though that DeclRefTypeRepr only accepts a ValueDecl
      // while the 'Self' type is more than just a reference to a TypeDecl.
      auto selfType = typeDC->getSelfInterfaceType();

      // Check if we can reference 'Self' here, and if so, what kind of Self it is.
      auto selfTypeKind = getSelfTypeKind(DC, options);

      // We don't allow generic arguments on 'Self'.
      if (selfTypeKind != SelfTypeKind::InvalidSelf &&
          repr->hasGenericArgList()) {
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
static Type resolveQualifiedIdentTypeRepr(const TypeResolution &resolution,
                                          SILTypeResolutionContext *silContext,
                                          Type parentTy,
                                          QualifiedIdentTypeRepr *repr) {
  const auto options = resolution.getOptions();
  auto DC = resolution.getDeclContext();
  auto &ctx = DC->getASTContext();
  auto &diags = ctx.Diags;
  const auto parentRange = repr->getBase()->getSourceRange();
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

static bool isDefaultNoEscapeContext(TypeResolutionOptions options) {
  return options.is(TypeResolverContext::FunctionInput) &&
         !options.contains(TypeResolutionFlags::DirectEscaping) &&
         !options.hasBase(TypeResolverContext::EnumElementDecl);
}

// Hack to apply context-specific @escaping to an AST function type.
static Type applyNonEscapingIfNecessary(Type ty,
                                        TypeResolutionOptions options) {
  bool defaultNoEscape = isDefaultNoEscapeContext(options);

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
      const auto attrLoc = ATR->getAttrLoc(TypeAttrKind::Autoclosure);
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

  using ContextualTypeAttrResolver =
    llvm::function_ref<bool(TypeAttribute *attr)>;

  class TypeAttrSet;

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

    bool isSILSourceFile() const {
      auto SF = getDeclContext()->getParentSourceFile();
      return (SF && SF->Kind == SourceFileKind::SIL);
    }

    bool isInterfaceFile() const {
      auto SF = getDeclContext()->getParentSourceFile();
      return (SF && SF->Kind == SourceFileKind::Interface);
    }

    /// Short-hand to query the current stage of type resolution.
    bool inStage(TypeResolutionStage stage) const {
      return resolution.getStage() == stage;
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
    
    bool diagnoseDisallowedExistential(TypeRepr *repr);
    
    bool diagnoseInvalidPlaceHolder(OpaqueReturnTypeRepr *repr);

    Type resolveGlobalActor(SourceLoc loc, TypeResolutionOptions options,
                            CustomAttr *&attr, TypeAttrSet &attrs);

    const clang::Type *tryParseClangType(ConventionTypeAttr *conv,
                                         bool hasConventionCOrBlock);

    NeverNullType resolveAttributedTypeRepr(AttributedTypeRepr *repr,
                                            TypeResolutionOptions options);

    NeverNullType resolveAttributedType(TypeRepr *underlyingRepr,
                                        TypeResolutionOptions options,
                                        TypeAttrSet &attrs);

    NeverNullType resolveOpenedExistentialArchetype(
        TypeRepr *repr, TypeResolutionOptions options, OpenedTypeAttr *attr);

    NeverNullType resolvePackElementArchetype(
        TypeRepr *repr, TypeResolutionOptions options, PackElementTypeAttr *attr);

    NeverNullType
    resolveASTFunctionType(FunctionTypeRepr *repr,
                           TypeResolutionOptions options,
                           TypeAttrSet *attrs);
    SmallVector<AnyFunctionType::Param, 8>
    resolveASTFunctionTypeParams(TupleTypeRepr *inputRepr,
                                 TypeResolutionOptions options,
                                 DifferentiabilityKind diffKind);

    NeverNullType resolveSILFunctionType(
        FunctionTypeRepr *repr, TypeResolutionOptions options,
        TypeAttrSet *attrs);
    SILParameterInfo resolveSILParameter(TypeRepr *repr,
                                         TypeResolutionOptions options,
                                         TypeAttrSet *yieldAttrs = nullptr);
    SILYieldInfo resolveSILYield(TypeRepr *repr, TypeResolutionOptions options,
                                 TypeAttrSet &remainingAttrs);
    bool resolveSILResults(TypeRepr *repr, TypeResolutionOptions options,
                           SmallVectorImpl<SILYieldInfo> &yields,
                           SmallVectorImpl<SILResultInfo> &results,
                           std::optional<SILResultInfo> &errorResult);
    bool resolveSingleSILResult(TypeRepr *repr, TypeResolutionOptions options,
                                SmallVectorImpl<SILYieldInfo> &yields,
                                SmallVectorImpl<SILResultInfo> &results,
                                std::optional<SILResultInfo> &errorResult);
    NeverNullType resolveDeclRefTypeReprRec(DeclRefTypeRepr *repr,
                                            TypeResolutionOptions options);
    NeverNullType resolveDeclRefTypeRepr(DeclRefTypeRepr *repr,
                                         TypeResolutionOptions options);
    NeverNullType resolveOwnershipTypeRepr(OwnershipTypeRepr *repr,
                                           TypeResolutionOptions options);
    NeverNullType resolveIsolatedTypeRepr(IsolatedTypeRepr *repr,
                                          TypeResolutionOptions options);
    NeverNullType resolveTransferringTypeRepr(TransferringTypeRepr *repr,
                                              TypeResolutionOptions options);
    NeverNullType resolveSendingTypeRepr(SendingTypeRepr *repr,
                                         TypeResolutionOptions options);
    NeverNullType
    resolveCompileTimeConstTypeRepr(CompileTimeConstTypeRepr *repr,
                                    TypeResolutionOptions options);
    NeverNullType resolveLifetimeDependentReturnTypeRepr(
        LifetimeDependentReturnTypeRepr *repr, TypeResolutionOptions options);
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
                                  TypeAttrSet *attrs = nullptr);
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
    NeverNullType resolveInverseType(InverseTypeRepr *repr,
                                     TypeResolutionOptions options);
    NeverNullType resolveProtocolType(ProtocolTypeRepr *repr,
                                      TypeResolutionOptions options);
    NeverNullType resolveSILBoxType(SILBoxTypeRepr *repr,
                                    TypeResolutionOptions options,
                                    TypeAttrSet *attrs);
    NeverNullType resolveSILReferenceStorage(TypeAttribute *attr,
                                             NeverNullType ty);

    NeverNullType resolveSILMetatype(TypeRepr *repr,
                                     TypeResolutionOptions options,
                                     TypeAttribute *thicknessAttr);
    NeverNullType
    buildMetatypeType(MetatypeTypeRepr *repr, Type instanceType,
                      std::optional<MetatypeRepresentation> storedRepr);
    NeverNullType
    buildProtocolType(ProtocolTypeRepr *repr, Type instanceType,
                      std::optional<MetatypeRepresentation> storedRepr);

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

  class TypeAttrSet {
    const ASTContext &ctx;

    llvm::TinyPtrVector<CustomAttr*> customAttrs;
    EnumMap<TypeAttrKind, TypeAttribute *> typeAttrs;

    llvm::SmallBitVector claimedCustomAttrs;
    FixedBitSet<NumTypeAttrKinds> claimedTypeAttrs;

#ifndef NDEBUG
    bool diagnosedUnclaimed = false;
#endif

  public:
    TypeAttrSet(const ASTContext &ctx) : ctx(ctx) {}

    TypeAttrSet(const TypeAttrSet &) = delete;
    TypeAttrSet &operator=(const TypeAttrSet &) = delete;

    ~TypeAttrSet() {
      assert(diagnosedUnclaimed);
    }

    static TypeAttrKind getRepresentative(TypeAttrKind attrKind);

    /// Accumulate attributes from a chain of attributed type reprs,
    /// and return the first non-attribute type repr.
    TypeRepr *accumulate(AttributedTypeRepr *typeRepr);

    /// Accumulate attributes from the given array.  Duplicate attributes
    /// will be diagnosed.
    void accumulate(ArrayRef<TypeOrCustomAttr> attrs);

    /// Return all of the custom attributes.
    ArrayRef<CustomAttr*> getCustomAttrs() const {
      return customAttrs;
    }

    /// Claim a custom attribute.  It will not be diagnosed as unused.
    void claim(CustomAttr *attr) {
      auto it = std::find(customAttrs.begin(), customAttrs.end(), attr);
      assert(it != customAttrs.end() && "attribute not in set");
      claimedCustomAttrs.set(it - customAttrs.begin());
    }

    TypeAttribute *getWithoutClaiming(TypeAttrKind attrKind) {
      auto it = typeAttrs.find(attrKind);
      if (it != typeAttrs.end()) {
        return *it;
      } else {
        return nullptr;
      }
    }

    /// Claim the attribute matching the given representative kind.
    /// It will not be diagnosed as unused.
    TypeAttribute *claim(TypeAttrKind attrKind) {
      assert(getRepresentative(attrKind) == attrKind);
      auto it = typeAttrs.find(attrKind);
      if (it != typeAttrs.end()) {
        claimedTypeAttrs.insert(it - typeAttrs.begin());
        return *it;
      } else {
        return nullptr;
      }
    }

    /// Claim all attributes for which the given function returns true.
    void claimAllWhere(ContextualTypeAttrResolver resolver) {
      size_t i = 0;
      for (TypeAttribute *attr : typeAttrs) {
        if (resolver(attr))
          claimedTypeAttrs.insert(i);
        i++;
      }
    }

    /// Claim all attributes for which the given function returns true,
    /// but process them in reverse source order.
    void reversedClaimAllWhere(ContextualTypeAttrResolver resolver) {
      for (size_t i = typeAttrs.size(); i > 0; --i) {
        TypeAttribute *attr = typeAttrs.begin()[i - 1];
        if (resolver(attr))
          claimedTypeAttrs.insert(i - 1);
      }
    }

    /// Diagnose any unclaimed attributes left in the set.
    void diagnoseUnclaimed(const TypeResolution &resolution,
                           TypeResolutionOptions options,
                           NeverNullType resolvedType);

  private:
    void diagnoseConflict(TypeAttrKind representativeKind,
                          TypeAttribute *firstAttr,
                          TypeAttribute *secondAttr);

    void diagnoseUnclaimed(CustomAttr *attr,
                           const TypeResolution &resolution,
                           TypeResolutionOptions options,
                           NeverNullType resolvedType);

    void diagnoseUnclaimed(TypeAttribute *attr,
                           const TypeResolution &resolution,
                           TypeResolutionOptions options,
                           NeverNullType resolvedType);

    template<typename ...ArgTypes>
    InFlightDiagnostic diagnose(ArgTypes &&...Args) const {
      auto &diags = ctx.Diags;
      return diags.diagnose(std::forward<ArgTypes>(Args)...);
    }
  };

  template <class AttrClass>
  AttrClass *claim(TypeAttrSet &attrs) {
    auto attr = attrs.claim(AttrClass::StaticKind);
    return cast_or_null<AttrClass>(attr);
  }

  template <class AttrClass>
  AttrClass *claim(TypeAttrSet *attrs) {
    return (attrs ? claim<AttrClass>(*attrs) : nullptr);
  }

  template <class AttrClass>
  AttrClass *getWithoutClaiming(TypeAttrSet &attrs) {
    auto attr = attrs.getWithoutClaiming(AttrClass::StaticKind);
    return cast_or_null<AttrClass>(attr);
  }

  template <class AttrClass>
  AttrClass *getWithoutClaiming(TypeAttrSet *attrs) {
    return (attrs ? getWithoutClaiming<AttrClass>(*attrs) : nullptr);
  }
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


bool swift::diagnoseMissingOwnership(ParamSpecifier ownership,
                                     TypeRepr *repr, Type ty,
                                     const TypeResolution &resolution) {
  auto options = resolution.getOptions();

  assert(!ty->hasError());

  if (options.hasBase(TypeResolverContext::EnumElementDecl))
    return false; // no need for ownership in enum cases.

  // The parameter type is written with respect to the surrounding
  // generic environment.
  ty = GenericEnvironment::mapTypeIntoContext(
             resolution.getGenericSignature().getGenericEnvironment(),
             ty);

  if (ty->hasError() || !ty->isNoncopyable())
    return false; // copyable types do not need ownership

  if (ownership != ParamSpecifier::Default)
    return false; // it has ownership

  auto &diags = resolution.getASTContext().Diags;
  auto loc = repr->getLoc();
  repr->setInvalid();

  // We don't yet support any ownership specifiers for parameters of subscript
  // decls, give a tailored error message saying you simply can't use a
  // noncopyable type here.
  if (options.hasBase(TypeResolverContext::SubscriptDecl)) {
    diags.diagnose(loc, diag::noncopyable_parameter_subscript_unsupported);
  } else {
    // general error diagnostic
    diags.diagnose(loc, diag::noncopyable_parameter_requires_ownership, ty);

    diags.diagnose(loc, diag::noncopyable_parameter_ownership_suggestion,
                   "borrowing", "for an immutable reference")
        .fixItInsert(repr->getStartLoc(), "borrowing ");

    diags.diagnose(loc, diag::noncopyable_parameter_ownership_suggestion,
                   "inout", "for a mutable reference")
        .fixItInsert(repr->getStartLoc(), "inout ");

    diags.diagnose(loc, diag::noncopyable_parameter_ownership_suggestion,
                   "consuming", "to take the value from the caller")
        .fixItInsert(repr->getStartLoc(), "consuming ");
  }

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
    options.setContext(std::nullopt);
  }

  bool isDirect = false;
  if ((options & TypeResolutionFlags::Direct) && !isa<SpecifierTypeRepr>(repr)){
    isDirect = true;
    options -= TypeResolutionFlags::Direct;
  }

  switch (repr->getKind()) {
  case TypeReprKind::Error:
    cast<ErrorTypeRepr>(repr)->dischargeDiagnostic(getASTContext());
    return ErrorType::get(getASTContext());

  case TypeReprKind::Attributed:
    return resolveAttributedTypeRepr(cast<AttributedTypeRepr>(repr), options);
  case TypeReprKind::Ownership:
    return resolveOwnershipTypeRepr(cast<OwnershipTypeRepr>(repr), options);
  case TypeReprKind::Isolated:
    return resolveIsolatedTypeRepr(cast<IsolatedTypeRepr>(repr), options);
  case TypeReprKind::Transferring:
    return resolveTransferringTypeRepr(cast<TransferringTypeRepr>(repr),
                                       options);
  case TypeReprKind::Sending:
    return resolveSendingTypeRepr(cast<SendingTypeRepr>(repr), options);
  case TypeReprKind::CompileTimeConst:
      return resolveCompileTimeConstTypeRepr(cast<CompileTimeConstTypeRepr>(repr),
                                             options);
  case TypeReprKind::UnqualifiedIdent:
  case TypeReprKind::QualifiedIdent: {
      return resolveDeclRefTypeRepr(cast<DeclRefTypeRepr>(repr), options);
  }

  case TypeReprKind::Function: {
    if (!(options & TypeResolutionFlags::SILType)) {
      // Default non-escaping for closure parameters
      auto result =
          resolveASTFunctionType(cast<FunctionTypeRepr>(repr), options, nullptr);
      return result;
    }
    return resolveSILFunctionType(cast<FunctionTypeRepr>(repr), options, nullptr);
  }
  case TypeReprKind::SILBox:
    assert((options & TypeResolutionFlags::SILType) && "SILBox repr in non-SIL type context?!");
    return resolveSILBoxType(cast<SILBoxTypeRepr>(repr), options, nullptr);

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

  case TypeReprKind::Inverse:
    return resolveInverseType(cast<InverseTypeRepr>(repr), options);

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
      auto *unqualIdentRepr =
          dyn_cast<UnqualifiedIdentTypeRepr>(opaqueRepr->getBase());
      if (unqualIdentRepr && !unqualIdentRepr->hasGenericArgList()) {
        Identifier name = unqualIdentRepr->getNameRef().getBaseIdentifier();
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

  case TypeReprKind::LifetimeDependentReturn:
    return resolveLifetimeDependentReturnTypeRepr(
        cast<LifetimeDependentReturnTypeRepr>(repr), options);
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

/// In SIL, handle '@opened(UUID, constraintType) interfaceType',
/// which creates an opened archetype.
NeverNullType
TypeResolver::resolveOpenedExistentialArchetype(
    TypeRepr *repr, TypeResolutionOptions options, OpenedTypeAttr *openedAttr) {
  assert(silContext);

  options.setContext(std::nullopt);

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
  auto constraintType = resolveType(openedAttr->getConstraintType(), options);

  Type archetypeType;
  if (!constraintType->isExistentialType()) {
    diagnoseInvalid(repr, openedAttr->getAtLoc(),
                    diag::opened_bad_constraint_type,
                    constraintType);

    archetypeType = ErrorType::get(constraintType->getASTContext());
  } else if (!interfaceType->isTypeParameter()) {
    diagnoseInvalid(repr, openedAttr->getAtLoc(),
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
                                             openedAttr->getUUID());
  }

  return archetypeType;
}

/// In SIL, handle '@pack_element(UUID) interfaceType',
/// which creates an opened archetype.
NeverNullType
TypeResolver::resolvePackElementArchetype(
    TypeRepr *repr, TypeResolutionOptions options,
    PackElementTypeAttr *attr) {
  assert(silContext);

  auto dc = getDeclContext();
  auto &ctx = dc->getASTContext();

  const SILTypeResolutionContext::OpenedPackElement *entry = nullptr;
  if (const auto *openedPacksMap = silContext->OpenedPackElements) {
    auto it = openedPacksMap->find(attr->getUUID());
    if (it != openedPacksMap->end()) {
      entry = &it->second;
    }
  }
  if (!entry) {
    diagnoseInvalid(repr, attr->getAttrLoc(),
                    diag::sil_pack_element_uuid_not_found);
    return ErrorType::get(ctx);
  }

  options.setContext(std::nullopt);

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
    diagnoseInvalid(repr, attr->getAttrLoc(),
                    diag::opened_bad_interface_type,
                    interfaceType);

    return ErrorType::get(ctx);
  }

  // Map the interface type into the element context.
  auto archetypeType =
    entry->Environment->mapPackTypeIntoElementContext(interfaceType);
  if (archetypeType->hasError()) {
    diagnoseInvalid(repr, attr->getAttrLoc(),
                    diag::opened_bad_interface_type,
                    interfaceType);
  }
  return archetypeType;
}

static unsigned countIsolatedParamsUpTo(FunctionTypeRepr* fnTy,
                                        unsigned bound) {
  unsigned count = 0;
  for (auto arg : fnTy->getArgsTypeRepr()->getElements()) {
    if (isa<IsolatedTypeRepr>(arg.Type)) {
      count += 1;
      if (count >= bound)
        break;
    }
  }
  return count;
}

// We trick the general infrastructure into diagnosing conflicts between
// different attributes in the same category by using an arbitrary
// representative from the category as the key when adding the attribute
// to the TypeAttrSet.  We then just recognize this case and emit a
// different diagnostic when diagnosing the conflict.
//
// The only basic requirement for categorization this way is that we'll
// never want to allow two attributes from the same set.  So it's okay
// that we lump the parameter, result, and error convention attributes
// into a single category, because it's still invalid to apply e.g.
// indirect_in and pack_out to the same value, even though one of them
// is likely also structurally invalid on the value.  (This is useful
// for that specific case because some of the attributes are used for
// multiple roles, like `owned`.)
static constexpr TypeAttrKind TAR_SILValueConvention = TypeAttrKind::Owned;
static constexpr TypeAttrKind TAR_SILMetatype = TypeAttrKind::Thin;
static constexpr TypeAttrKind TAR_TypeTransformer = TypeAttrKind::Opened;
static constexpr TypeAttrKind TAR_SILCoroutine = TypeAttrKind::YieldOnce;
static constexpr TypeAttrKind TAR_SILCalleeConvention =
    TypeAttrKind::CalleeOwned;
static constexpr TypeAttrKind TAR_SILReferenceStorage = TypeAttrKind::SILWeak;

TypeAttrKind TypeAttrSet::getRepresentative(TypeAttrKind kind) {
  switch (kind) {
  // Most attributes are singleton for the purposes of this analysis.
  default: return kind;

  case TypeAttrKind::Autoreleased:
  case TypeAttrKind::InGuaranteed:
  case TypeAttrKind::In:
  case TypeAttrKind::InConstant:
  case TypeAttrKind::Inout:
  case TypeAttrKind::InoutAliasable:
  case TypeAttrKind::Owned:
  case TypeAttrKind::Guaranteed:
  case TypeAttrKind::PackOwned:
  case TypeAttrKind::PackGuaranteed:
  case TypeAttrKind::PackInout:
  case TypeAttrKind::Out:
  case TypeAttrKind::PackOut:
  case TypeAttrKind::UnownedInnerPointer:
  case TypeAttrKind::Error:
  case TypeAttrKind::ErrorIndirect:
  case TypeAttrKind::ErrorUnowned:
    return TAR_SILValueConvention;

  case TypeAttrKind::Thin:
  case TypeAttrKind::Thick:
  case TypeAttrKind::ObjCMetatype:
    return TAR_SILMetatype;

  case TypeAttrKind::YieldMany:
  case TypeAttrKind::YieldOnce:
    return TAR_SILCoroutine;

  case TypeAttrKind::CalleeOwned:
  case TypeAttrKind::CalleeGuaranteed:
    return TAR_SILCalleeConvention;

#define REF_STORAGE(Name, name, ...) case TypeAttrKind::SIL##Name:
#include "swift/AST/ReferenceStorage.def"
    return TAR_SILReferenceStorage;

  // These are total transforms on the type, and one of them can apply
  // at once.
  case TypeAttrKind::Opened:
  case TypeAttrKind::PackElement:
  case TypeAttrKind::OpaqueReturnTypeOf:
    return TAR_TypeTransformer;
  };
}

TypeRepr *TypeAttrSet::accumulate(AttributedTypeRepr *attrRepr) {
  while (true) {
    accumulate(attrRepr->getAttrs());
    auto underlyingRepr = attrRepr->getTypeRepr();
    attrRepr = dyn_cast<AttributedTypeRepr>(underlyingRepr);
    if (!attrRepr) return underlyingRepr;
  }
}

void TypeAttrSet::accumulate(ArrayRef<TypeOrCustomAttr> attrs) {
  for (auto attr : attrs) {
    // Just put custom attributes into a separate list.
    if (auto customAttr = attr.dyn_cast<CustomAttr*>()) {
      customAttrs.push_back(customAttr);
      continue;
    }

    auto typeAttr = attr.get<TypeAttribute*>();
    auto representativeKind = getRepresentative(typeAttr->getKind());

    // Try to insert the attribute in the set under its representative
    // kind.  If this succeeds, we don't have a conflict.
    auto insertResult = typeAttrs.insert(representativeKind, typeAttr);
    if (insertResult.second) continue;

    // Dignose the conflict.
    TypeAttribute *previousAttr = *insertResult.first;

    diagnoseConflict(representativeKind, previousAttr, typeAttr);
  }

  claimedCustomAttrs.resize(customAttrs.size());
}

void TypeAttrSet::diagnoseConflict(TypeAttrKind representativeKind,
                                   TypeAttribute *firstAttr,
                                   TypeAttribute *secondAttr) {
  secondAttr->setInvalid();

  // Special diagnostic for an exact repeat
  if (firstAttr->getKind() == secondAttr->getKind()) {
    diagnose(secondAttr->getStartLoc(), diag::duplicate_attribute,
             /*modifier*/false)
      .fixItRemove(secondAttr->getSourceRange());
    return;
  }

  // Special diagnostic for SIL metatypes
  if (representativeKind == TAR_SILMetatype) {
    diagnose(secondAttr->getStartLoc(), diag::sil_metatype_multiple_reprs);
    return;
  }

  // Generic conflict diagnostic
  diagnose(secondAttr->getStartLoc(), diag::mutually_exclusive_attrs,
           secondAttr->getAttrName(), firstAttr->getAttrName(),
           /*modifier*/false);
}

void TypeAttrSet::diagnoseUnclaimed(const TypeResolution &resolution,
                                    TypeResolutionOptions options,
                                    NeverNullType resolvedType) {
#ifndef NDEBUG
  assert(!diagnosedUnclaimed && "diagnosing unclaimed attributes twice");
  diagnosedUnclaimed = true;
#endif

  // Don't diagnose unclaimed attributes in this stage if we weren't able
  // to resolve the type.
  if (resolvedType->is<DependentMemberType>() &&
      resolution.getStage() == TypeResolutionStage::Structural) {
    return;
  }

  // Custom attributes
  for (size_t i : range(customAttrs.size())) {
    if (claimedCustomAttrs[i]) continue;

    auto customAttr = customAttrs[i];
    diagnoseUnclaimed(customAttr, resolution, options, resolvedType);
  }

  // Type attributes
  size_t i = 0;
  for (auto attr : typeAttrs) {
    if (claimedTypeAttrs.contains(i)) continue;
    i++;

    diagnoseUnclaimed(attr, resolution, options, resolvedType);
  }
}

void TypeAttrSet::diagnoseUnclaimed(CustomAttr *attr,
                                    const TypeResolution &resolution,
                                    TypeResolutionOptions options,
                                    NeverNullType resolvedType) {
  // Ignore attributes that have already been marked invalid.
  if (attr->isInvalid()) return;

  attr->setInvalid();

  // Diagnose the attribute, because we don't yet handle custom type
  // attributes.
  std::string typeName;
  if (auto typeRepr = attr->getTypeRepr()) {
    llvm::raw_string_ostream out(typeName);
    typeRepr->print(out);
  } else {
    typeName = attr->getType().getString();
  }

  diagnose(attr->getLocation(), diag::unknown_attribute, typeName);
}

static bool isSILAttribute(TypeAttrKind attrKind) {
  switch (attrKind) {
#define SIL_TYPE_ATTR(SPELLING, CLASS) case TypeAttrKind::CLASS:
#include "swift/AST/TypeAttr.def"
  case TypeAttrKind::NoEscape: // noescape is only used in SIL now
    return true;

  default:
    return false;
  }
}

static bool isFunctionAttribute(TypeAttrKind attrKind) {
  static const TypeAttrKind FunctionAttrs[] = {
      TypeAttrKind::Convention,
      TypeAttrKind::Pseudogeneric,
      TypeAttrKind::Unimplementable,
      TypeAttrKind::CalleeOwned,
      TypeAttrKind::CalleeGuaranteed,
      TypeAttrKind::NoEscape,
      TypeAttrKind::Autoclosure,
      TypeAttrKind::Differentiable,
      TypeAttrKind::Escaping,
      TypeAttrKind::Sendable,
      TypeAttrKind::YieldOnce,
      TypeAttrKind::YieldMany,
      TypeAttrKind::Async,
  };
  return llvm::any_of(FunctionAttrs, [attrKind](TypeAttrKind functionAttr) {
                        return functionAttr == attrKind;
                      });
}

void TypeAttrSet::diagnoseUnclaimed(TypeAttribute *attr,
                                    const TypeResolution &resolution,
                                    TypeResolutionOptions options,
                                    NeverNullType resolvedType) {
  if (attr->isInvalid()) return;

  attr->setInvalid();

  // Use a special diagnostic for SIL attributes.
  if (!(options & TypeResolutionFlags::SILType) &&
      isSILAttribute(attr->getKind())) {
    diagnose(attr->getStartLoc(), diag::unknown_attribute, attr->getAttrName());
    return;
  }

  // Recognize function attributes being applied to non-functions.
  if (isFunctionAttribute(attr->getKind()) &&
      !resolvedType->is<AnyFunctionType>()) {
    auto escapingAttr = dyn_cast<EscapingTypeAttr>(attr);

    // Try to recognize `@escaping` placed on optional types.
    if (escapingAttr) {
      Type optionalObjectType = resolvedType->getOptionalObjectType();
      if (optionalObjectType && optionalObjectType->is<AnyFunctionType>()) {
        diagnose(escapingAttr->getAttrLoc(),
                 diag::escaping_optional_type_argument)
          .fixItRemove(attr->getSourceRange());
        return;
      }
    }

    auto diagnostic = diagnose(attr->getStartLoc(),
                               diag::attribute_requires_function_type,
                               attr->getAttrName());
    if (isa<EscapingTypeAttr>(attr))
      diagnostic.fixItRemove(attr->getSourceRange());
    return;
  }

  ctx.Diags.diagnose(attr->getStartLoc(), diag::attribute_does_not_apply_to_type);
}

Type TypeResolver::resolveGlobalActor(SourceLoc loc, TypeResolutionOptions options,
                                      CustomAttr *&attr, TypeAttrSet &attrs) {
  auto foundGlobalActor = checkGlobalActorAttributes(
      loc, getDeclContext(), attrs.getCustomAttrs());
  if (!foundGlobalActor)
    return Type();

  attr = foundGlobalActor->first;
  attrs.claim(attr);

  Type result = resolveType(attr->getTypeRepr(), options);

  // Diagnose use of global actor attributes under the task-to-thread model.
  if (!result->hasError() && !attr->isInvalid() &&
      getASTContext().LangOpts.isConcurrencyModelTaskToThread()) {
    if (Decl *decl = getDeclContext()->getAsDecl()) {
      if (!AvailableAttr::isUnavailable(decl))
        diagnose(attr->getLocation(),
                 diag::concurrency_task_to_thread_model_global_actor_annotation,
                 attr->getTypeRepr(), "task-to-thread concurrency model");
    }
  }

  return result;
}

const clang::Type *TypeResolver::tryParseClangType(ConventionTypeAttr *conv,
                                                   bool hasConventionCOrBlock) {
  auto clangTypeString = conv->getClangType();
  auto clangTypeLoc = conv->getClangTypeLoc();
  if (!clangTypeString || clangTypeString->empty())
    return nullptr;
  if (!hasConventionCOrBlock) {
    diagnose(clangTypeLoc, diag::unexpected_ctype_for_non_c_convention,
             conv->getConventionName(), *clangTypeString);
    return nullptr;
  }

  const clang::Type *type =
      getASTContext().getClangModuleLoader()->parseClangFunctionType(
          *clangTypeString, clangTypeLoc);
  if (!type)
    diagnose(clangTypeLoc, diag::unable_to_parse_c_function_type,
             *clangTypeString);
  return type;
}

NeverNullType
TypeResolver::resolveAttributedTypeRepr(AttributedTypeRepr *attrRepr,
                                        TypeResolutionOptions options) {
  TypeAttrSet attrs(getASTContext());
  TypeRepr *underlyingRepr = attrs.accumulate(attrRepr);

  auto result = resolveAttributedType(underlyingRepr, options, attrs);

  attrs.diagnoseUnclaimed(resolution, options, result);

  return result;
}

NeverNullType
TypeResolver::resolveAttributedType(TypeRepr *repr, TypeResolutionOptions options,
                                    TypeAttrSet &attrs) {
  // Adjust the context for the @escaping attribute.  We don't claim here
  // because we want to diagnose it later if we didn't build a function type.
  if (getWithoutClaiming<EscapingTypeAttr>(attrs))
    options |= TypeResolutionFlags::DirectEscaping;

  // There are basically three kinds of type attributes:
  //
  // - Some attributes are basically totally new type structure, like
  //   `@opened`, and their presence completely changes how the underlying
  //   type is interpreted and built.  We have to check for these first.
  //
  // - Some attributes are adjustments to specific syntactic forms.
  //   Generally we just make the attributes available to the resolution
  //   function for the underlying type.
  //
  // - Some attributes apply uniformly to all types, or at least they
  //   potentially do.  Once we've built the underlying type, we need
  //   to check for them.

  // These are the total type transforms.
  Type ty;
  if (auto attr = attrs.claim(TAR_TypeTransformer)) {
    if (auto opaqueAttr = dyn_cast<OpaqueReturnTypeOfTypeAttr>(attr)) {
      ty = resolveOpaqueReturnType(repr, opaqueAttr->getMangledName(),
                                   opaqueAttr->getIndex(),
                                   options);
    } else if (auto openedAttr = dyn_cast<OpenedTypeAttr>(attr)) {
      ty = resolveOpenedExistentialArchetype(repr, options, openedAttr);
    } else {
      auto packElementAttr = cast<PackElementTypeAttr>(attr);
      ty = resolvePackElementArchetype(repr, options, packElementAttr);
    }

  // The SIL metatype attributes are basically total type transforms, too.
  // TODO: this should really be restricted to lowered types
  } else if (auto attr = isSILSourceFile()
                            ? attrs.claim(TAR_SILMetatype) : nullptr) {
    ty = resolveSILMetatype(repr, options, attr);

  // Okay, propagate attributes down to specific resolvers.

  // Functions
  } else if (auto fnRepr = dyn_cast<FunctionTypeRepr>(repr)) {
    if (options & TypeResolutionFlags::SILType)
      ty = resolveSILFunctionType(fnRepr, options, &attrs);
    else
      ty = resolveASTFunctionType(fnRepr, options, &attrs);

  // Boxes 
  } else if (auto boxRepr = dyn_cast<SILBoxTypeRepr>(repr)) {
    ty = resolveSILBoxType(boxRepr, options, &attrs);

  // Packs
  } else if (auto packRepr = dyn_cast<PackTypeRepr>(repr)) {
    ty = resolvePackType(packRepr, options, &attrs);

  // Otherwise, just resolve normally.
  } else {
    ty = resolveType(repr, options);
  }

  // TODO: It would be better to write all of these attribute checks
  // using claimAllWhere so that the work doen is proportional to the
  // number of attributes that were actually written.

  if (auto uncheckedAttr = claim<UncheckedTypeAttr>(attrs)) {
    if (ty->hasError()) return ty;

    if (!options.is(TypeResolverContext::Inherited) ||
        getDeclContext()->getSelfProtocolDecl()) {
      diagnoseInvalid(repr, uncheckedAttr->getAtLoc(),
                      diag::unchecked_not_inheritance_clause);
      ty = ErrorType::get(getASTContext());
    } else if (!ty->isConstraintType()) {
      diagnoseInvalid(repr, uncheckedAttr->getAtLoc(),
                      diag::unchecked_not_existential, ty);
      ty = ErrorType::get(getASTContext());
    }

    // Nothing to record in the type.
  }

  if (auto preconcurrencyAttr = claim<PreconcurrencyTypeAttr>(attrs)) {
    if (ty->hasError())
      return ty;

    if (!options.is(TypeResolverContext::Inherited) ||
        getDeclContext()->getSelfProtocolDecl()) {
      diagnoseInvalid(repr, preconcurrencyAttr->getAtLoc(),
                      diag::preconcurrency_not_inheritance_clause);
      ty = ErrorType::get(getASTContext());
    } else if (!ty->isConstraintType()) {
      diagnoseInvalid(repr, preconcurrencyAttr->getAtLoc(),
                      diag::preconcurrency_not_existential, ty);
      ty = ErrorType::get(getASTContext());
    }

    // Nothing to record in the type.
  }

  if (auto retroactiveAttr = claim<RetroactiveTypeAttr>(attrs)) {
    if (ty->hasError()) return ty;

    auto extension = dyn_cast_or_null<ExtensionDecl>(getDeclContext());
    bool isInInheritanceClause = options.is(TypeResolverContext::Inherited);
    if (!isInInheritanceClause || !extension) {
      diagnoseInvalid(repr, retroactiveAttr->getAtLoc(),
                      diag::retroactive_not_in_extension_inheritance_clause)
          .fixItRemove(retroactiveAttr->getSourceRange());
      ty = ErrorType::get(getASTContext());
    }
  }

  // Type aliases inside protocols are not yet resolved in the structural
  // stage of type resolution
  if (ty->is<DependentMemberType>() &&
      resolution.getStage() == TypeResolutionStage::Structural) {
    return ty;
  }

  // Consume @escaping if we did ultimately produce a function type.
  //
  // A better way to handle this might be to thread attributes to both of the
  // places that can produce function types (name resolution and
  // FunctionTypeRepr) and have them check and claim it themselves.
  // Then this "meaningless" diagnostic can just be special case
  // in diagnoseUnclaimed.
  if ((options & TypeResolutionFlags::DirectEscaping) &&
      ty->is<FunctionType>()) {
    // We might not actually have an @escaping attribute here if we saw
    // something like `@escaping (@moreAttributes FnType)`.
    if (auto escapingAttr = claim<EscapingTypeAttr>(attrs)) {
      // The attribute is meaningless except on non-variadic parameter types.
      if (!options.is(TypeResolverContext::FunctionInput) ||
          options.getBaseContext() == TypeResolverContext::EnumElementDecl) {
        auto loc = escapingAttr->getAtLoc();
        auto attrRange = escapingAttr->getSourceRange();

        // Try to find a better diagnostic based on how the type is being used
        if (options.is(TypeResolverContext::ImmediateOptionalTypeArgument)) {
          diagnoseInvalid(repr, repr->getLoc(),
                          diag::escaping_optional_type_argument)
              .fixItRemove(attrRange);
        } else if (options.is(TypeResolverContext::InoutFunctionInput)) {
          diagnoseInvalid(repr, repr->getLoc(),
                          diag::escaping_inout_parameter)
              .fixItRemove(attrRange);
        } else {
          diagnoseInvalid(repr, loc, diag::escaping_non_function_parameter)
              .fixItRemove(attrRange);
        }

        ty = ErrorType::get(getASTContext());
      }
    }
  }

  if (auto autoclosureAttr = claim<AutoclosureTypeAttr>(attrs)) {
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

    bool didDiagnose = false;
    if (options.is(TypeResolverContext::VariadicFunctionInput) &&
        !options.hasBase(TypeResolverContext::EnumElementDecl)) {
      diagnoseInvalid(repr, autoclosureAttr->getAtLoc(),
                      diag::attr_not_on_variadic_parameters, "@autoclosure");
      didDiagnose = true;
    } else if (!options.is(TypeResolverContext::FunctionInput)) {
      diagnoseInvalid(repr, autoclosureAttr->getAtLoc(),
                      diag::attr_only_on_parameters, "@autoclosure");
      didDiagnose = true;
    }

    if (didDiagnose) {
      ty = ErrorType::get(getASTContext());
    }
  }

  // @noDerivative is valid on function parameters (AST and SIL) or on
  // function results (SIL-only).  We just unconditionally claim this here,
  // which is a little unnecessary.
  if (auto noDerivativeAttr = claim<NoDerivativeTypeAttr>(attrs)) {
    bool isNoDerivativeAllowed =
        options.is(TypeResolverContext::FunctionInput) ||
        options.is(TypeResolverContext::InoutFunctionInput) ||
        (options.is(TypeResolverContext::FunctionResult) &&
          (options & TypeResolutionFlags::SILType));
    auto *SF = getDeclContext()->getParentSourceFile();
    if (SF && !isDifferentiableProgrammingEnabled(*SF)) {
      diagnose(
          noDerivativeAttr->getAtLoc(),
          diag::differentiable_programming_attr_used_without_required_module,
          TypeAttribute::getAttrName(TypeAttrKind::NoDerivative),
          getASTContext().Id_Differentiation);
    } else if (!isNoDerivativeAllowed) {
      bool isVariadicFunctionParam =
          options.is(TypeResolverContext::VariadicFunctionInput) &&
          !options.hasBase(TypeResolverContext::EnumElementDecl);
      diagnose(noDerivativeAttr->getAtLoc(),
               (isVariadicFunctionParam
                    ? diag::attr_not_on_variadic_parameters
                    : diag::attr_only_on_parameters_of_differentiable),
               "@noDerivative");
    }
  }

  if (getASTContext().LangOpts.hasFeature(Feature::LayoutPrespecialization)) {
    (void) claim<NoMetadataTypeAttr>(attrs);
    // TODO: add proper validation
  }

  // There are a bunch of attributes in SIL that are essentially new
  // type constructors.  Some of these are allowed even in AST positions;
  // other are only allowed in lowered types.
  if (isSILSourceFile()) {
    // Process the attributes in reverse source order in order to follow the
    // "recursive structure" of the type constructors.
    attrs.reversedClaimAllWhere([&](TypeAttribute *attr) {
      switch (attrs.getRepresentative(attr->getKind())) {
      case TypeAttrKind::DynamicSelf:
        ty = rebuildWithDynamicSelf(getASTContext(), ty);
        return true;

      case TAR_SILReferenceStorage:
        ty = resolveSILReferenceStorage(attr, ty);
        return true;

      case TypeAttrKind::BlockStorage:
        if (options & TypeResolutionFlags::SILType) {
          ty = SILBlockStorageType::get(ty->getCanonicalType());;
          return true;
        }
        return false;

      case TypeAttrKind::Box:
        if (options & TypeResolutionFlags::SILType) {
          ty = SILBoxType::get(ty->getCanonicalType());
          return true;
        }
        return false;

      case TypeAttrKind::MoveOnly:
        if (options & TypeResolutionFlags::SILType) {
          ty = SILMoveOnlyWrappedType::get(ty->getCanonicalType());
          return true;
        }
        return false;

      default:
        return false;
      }
    });
  }

  return ty;
}

NeverNullType
TypeResolver::resolveSILReferenceStorage(TypeAttribute *attr, NeverNullType ty) {
  if (ty->hasError()) return ty;

  auto ownership = [&] {
#define REF_STORAGE(Name, name, ...) \
    if (isa<SIL##Name##TypeAttr>(attr)) return ReferenceOwnership::Name;
#include "swift/AST/ReferenceStorage.def"
    llvm_unreachable("bad reference storage kind");
  }();

  bool isOptional = false;
  auto objType = ty;
  if (auto obj = ty->getOptionalObjectType()) {
    isOptional = true;
    objType = obj;
  }

  switch (optionalityOf(ownership)) {
  case ReferenceOwnershipOptionality::Disallowed:
    if (isOptional) {
      diagnose(attr->getStartLoc(), diag::invalid_ownership_with_optional,
               ownership);
      attr->setInvalid();
      return ty;
    }
    break;
  case ReferenceOwnershipOptionality::Allowed:
    break;
  case ReferenceOwnershipOptionality::Required:
    if (!isOptional) {
      diagnose(attr->getStartLoc(), diag::invalid_ownership_not_optional,
               ownership, OptionalType::get(ty));
      attr->setInvalid();
      return ty;

    }
    break;
  }

  if (!objType->allowsOwnership(resolution.getGenericSignature().getPointer())) {
    diagnose(attr->getStartLoc(), diag::invalid_ownership_type,
             ownership, objType);
    attr->setInvalid();
    return ty;
  }

  return ReferenceStorageType::get(ty, ownership, getASTContext());
}

NeverNullType
TypeResolver::resolveSILMetatype(TypeRepr *repr,
                                 TypeResolutionOptions options,
                                 TypeAttribute *thicknessAttr) {
  if (auto existential = dyn_cast<ExistentialTypeRepr>(repr))
    repr = existential->getConstraint();

  TypeRepr *base;
  if (auto metatypeRepr = dyn_cast<MetatypeTypeRepr>(repr)) {
    base = metatypeRepr->getBase();
  } else if (auto protocolRepr = dyn_cast<ProtocolTypeRepr>(repr)) {
    base = protocolRepr->getBase();
  } else {
    diagnose(thicknessAttr->getStartLoc(), diag::sil_metatype_not_metatype);
    return getASTContext().TheErrorType;
  }

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

  MetatypeRepresentation storedRepr = [&] {
    if (isa<ThinTypeAttr>(thicknessAttr)) {
      return MetatypeRepresentation::Thin;
    } else if (isa<ThickTypeAttr>(thicknessAttr)) {
      return MetatypeRepresentation::Thick;
    } else {
      assert(isa<ObjCMetatypeTypeAttr>(thicknessAttr));
      return MetatypeRepresentation::ObjC;
    }
  }();

  Type ty;
  if (auto metatype = dyn_cast<MetatypeTypeRepr>(repr)) {
    ty = buildMetatypeType(metatype, instanceTy, storedRepr);
  } else {
    ty = buildProtocolType(cast<ProtocolTypeRepr>(repr),
                           instanceTy, storedRepr);
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

  auto *dc = getDeclContext();

  auto elementOptions = options.withoutContext(true);
  elementOptions.setContext(TypeResolverContext::FunctionInput);
  for (unsigned i = 0, end = inputRepr->getNumElements(); i != end; ++i) {
    auto *eltTypeRepr = inputRepr->getElementType(i);

    // Look through parens here; other than parens, specifiers
    // must appear at the top level of a parameter type.
    auto *nestedRepr = eltTypeRepr->getWithoutParens();

    ParamSpecifier ownership = ParamSpecifier::Default;
    OwnershipTypeRepr *ownershipRepr = nullptr;

    bool isolated = false;
    bool compileTimeConst = false;
    bool isSending = false;
    while (true) {
      if (auto *specifierRepr = dyn_cast<SpecifierTypeRepr>(nestedRepr)) {
        switch (specifierRepr->getKind()) {
        case TypeReprKind::Ownership:
          ownershipRepr = cast<OwnershipTypeRepr>(specifierRepr);
          ownership = ownershipRepr->getSpecifier();
          nestedRepr = specifierRepr->getBase();
          continue;
        case TypeReprKind::Transferring:
          isSending = true;
          nestedRepr = specifierRepr->getBase();
          continue;
        case TypeReprKind::Sending:
          isSending = true;
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

    while (auto *ATR = dyn_cast<AttributedTypeRepr>(nestedRepr)) {
      if (ATR->has(TypeAttrKind::Autoclosure))
        autoclosure = true;

      if (ATR->has(TypeAttrKind::NoDerivative)) {
        if (diffKind == DifferentiabilityKind::NonDifferentiable &&
            isDifferentiableProgrammingEnabled(
                *dc->getParentSourceFile()))
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

    // Validate the presence of ownership for a noncopyable parameter.
    if (inStage(TypeResolutionStage::Interface)
        && !ty->hasUnboundGenericType()) {
      diagnoseMissingOwnership(ownership, eltTypeRepr, ty, resolution);

      // @_staticExclusiveOnly types cannot be passed as 'inout' in function
      // types.
      if (auto SD = ty->getStructOrBoundGenericStruct()) {
        if (SD->getAttrs().hasAttribute<StaticExclusiveOnlyAttr>() &&
            ownership == ParamSpecifier::InOut) {
          diagnose(eltTypeRepr->getLoc(),
                   diag::attr_static_exclusive_only_let_only_param,
                   ty);
        }
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

    if (isSending) {
      if (ownership == ParamSpecifier::Default) {
        ownership = ParamSpecifier::ImplicitlyCopyableConsuming;
      }
    }

    auto paramFlags = ParameterTypeFlags::fromParameterType(
        ty, variadic, autoclosure, /*isNonEphemeral*/ false, ownership,
        isolated, noDerivative, compileTimeConst, isSending);
    elements.emplace_back(ty, argumentLabel, paramFlags, parameterName);
  }

  return elements;
}

NeverNullType
TypeResolver::resolveOpaqueReturnType(TypeRepr *repr, StringRef mangledName,
                                      unsigned ordinal,
                                      TypeResolutionOptions options) {
  // The type representation should be an unqualified identifier. We don't
  // really use the identifier for anything, but we do resolve any generic
  // arguments to instantiate the possibly-generic opaque type.
  SmallVector<Type, 4> TypeArgsBuf;
  if (auto *unqualIdentRepr = dyn_cast<UnqualifiedIdentTypeRepr>(repr)) {
    for (auto argRepr : unqualIdentRepr->getGenericArgs()) {
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
    TypeAttrSet *attrs) {
  auto &ctx = getASTContext();

  auto isolatedAttr = claim<IsolatedTypeAttr>(attrs);

  AnyFunctionType::Representation representation =
    FunctionType::Representation::Swift;
  const clang::Type *parsedClangFunctionType = nullptr;
  auto conventionAttr = claim<ConventionTypeAttr>(attrs);
  if (conventionAttr) {
    auto parsedRep =
        llvm::StringSwitch<std::optional<FunctionType::Representation>>(
            conventionAttr->getConventionName())
            .Case("swift", FunctionType::Representation::Swift)
            .Case("block", FunctionType::Representation::Block)
            .Case("thin", FunctionType::Representation::Thin)
            .Case("c", FunctionType::Representation::CFunctionPointer)
            .Default(std::nullopt);
    if (!parsedRep) {
      diagnoseInvalid(repr, conventionAttr->getAtLoc(),
                      diag::unsupported_convention,
                      conventionAttr->getConventionName());
      representation = FunctionType::Representation::Swift;
    } else {
      representation = *parsedRep;

      parsedClangFunctionType = tryParseClangType(
          conventionAttr, shouldStoreClangType(representation));

      // Certain conventions are not compatible with autoclosures.
      if (getWithoutClaiming<AutoclosureTypeAttr>(attrs)) {
        if (representation == FunctionType::Representation::CFunctionPointer ||
            representation == FunctionType::Representation::Block) {
          diagnoseInvalid(repr, conventionAttr->getAtLoc(),
                          diag::invalid_autoclosure_and_convention_attributes,
                          conventionAttr->getConventionName());
          representation = FunctionType::Representation::Swift;
          parsedClangFunctionType = nullptr;
        }
      }

      // Don't allow `@isolated` to be combined with non-default
      // conventions.
      if (isolatedAttr &&
          representation != FunctionType::Representation::Swift) {
        diagnoseInvalid(repr, conventionAttr->getAtLoc(),
                        diag::invalid_isolated_and_convention_attributes,
                        conventionAttr->getConventionName());
        representation = FunctionType::Representation::Swift;
        parsedClangFunctionType = nullptr;
      }
    }
  }

  DifferentiabilityKind diffKind = DifferentiabilityKind::NonDifferentiable;
  if (auto diffAttr = claim<DifferentiableTypeAttr>(attrs)) {
    auto *SF = getDeclContext()->getParentSourceFile();
    if (SF && isDifferentiableProgrammingEnabled(*SF)) {
      diffKind = diffAttr->getDifferentiability();
    } else {
      diagnoseInvalid(
          repr, diffAttr->getAtLoc(),
          diag::differentiable_programming_attr_used_without_required_module,
          diffAttr->getAttrName(), ctx.Id_Differentiation);
    }
  }

  bool sendable = claim<SendableTypeAttr>(attrs);

  auto isolation = FunctionTypeIsolation::forNonIsolated();

  // Check that we don't have more than one isolated parameter.
  unsigned numIsolatedParams = countIsolatedParamsUpTo(repr, 2);
  if (!repr->isWarnedAbout() && numIsolatedParams > 1) {
    diagnose(repr->getLoc(), diag::isolated_parameter_duplicate_type)
        .warnUntilSwiftVersion(6);

    if (ctx.LangOpts.isSwiftVersionAtLeast(6))
      return ErrorType::get(ctx);
    else
      repr->setWarned();
  }

  if (attrs) {
    CustomAttr *globalActorAttr = nullptr;
    Type globalActor = resolveGlobalActor(repr->getLoc(), parentOptions,
                                          globalActorAttr, *attrs);
    if (globalActor && !globalActor->hasError() && !globalActorAttr->isInvalid()) {
      if (numIsolatedParams != 0) {
        diagnose(repr->getLoc(), diag::isolated_parameter_global_actor_type)
            .warnUntilSwiftVersion(6);
        globalActorAttr->setInvalid();
      } else if (isolatedAttr) {
        diagnose(repr->getLoc(), diag::isolated_attr_global_actor_type,
                 isolatedAttr->getIsolationKindName());
        globalActorAttr->setInvalid();
      } else {
        isolation = FunctionTypeIsolation::forGlobalActor(globalActor);

        // This inference is currently gated because `@Sendable` impacts mangling.
        if (ctx.LangOpts.hasFeature(Feature::GlobalActorIsolatedTypesUsability))
          sendable = true;
      }
    }

    if (isolatedAttr && !isolatedAttr->isInvalid()) {
      switch (isolatedAttr->getIsolationKind()) {
      case IsolatedTypeAttr::IsolationKind::Dynamic:
        if (representation != FunctionType::Representation::Swift) {
          assert(conventionAttr);
          diagnoseInvalid(repr, isolatedAttr->getAtLoc(),
                          diag::isolated_attr_bad_convention,
                          isolatedAttr->getIsolationKindName(),
                          conventionAttr->getConventionName());
        } else {
          isolation = FunctionTypeIsolation::forErased();
        }
        break;
      }
    }
  }

  // Diagnose a couple of things that we can parse in SIL mode but we don't
  // allow in formal types.
  if (auto patternParams = repr->getPatternGenericParams()) {
    diagnose(patternParams->getLAngleLoc(),
             diag::ast_subst_function_type);
    return ErrorType::get(ctx);
  } else if (!repr->getInvocationSubstitutions().empty()) {
    diagnose(repr->getInvocationSubstitutions()[0]->getStartLoc(),
             diag::ast_subst_function_type);
    return ErrorType::get(ctx);
  }

  std::optional<SILInnerGenericContextRAII> innerGenericContext;
  if (auto *genericParams = repr->getGenericParams()) {
    if (!silContext) {
      diagnose(genericParams->getLAngleLoc(), diag::generic_function_type)
        .highlight(genericParams->getSourceRange());
      return ErrorType::get(ctx);
    }
    innerGenericContext.emplace(silContext, genericParams);
  }

  TypeResolutionOptions options = std::nullopt;
  options |= parentOptions.withoutContext().getFlags();
  auto params =
      resolveASTFunctionTypeParams(repr->getArgsTypeRepr(), options, diffKind);

  // Use parameter isolation if we have any.  This overrides all other
  // forms (and should cause conflict diagnostics).
  if (hasIsolatedParameter(params)) {
    isolation = FunctionTypeIsolation::forParameter();

    if (isolatedAttr) {
      diagnose(repr->getLoc(), diag::isolated_parameter_isolated_attr_type,
               isolatedAttr->getIsolationKindName());
      isolatedAttr->setInvalid();
    }
  }

  auto resultOptions = options.withoutContext();
  resultOptions.setContext(TypeResolverContext::FunctionResult);
  auto outputTy = resolveType(repr->getResultTypeRepr(), resultOptions);
  if (outputTy->hasError()) {
    return ErrorType::get(ctx);
  }

  // If this is a function type without parens around the parameter list,
  // diagnose this and produce a fixit to add them.
  if (!repr->isWarnedAbout()) {
    // If someone wrote (Void) -> () in Swift 3, they probably meant
    // () -> (), but (Void) -> () is (()) -> () so emit a warning
    // asking if they meant () -> ().
    auto args = repr->getArgsTypeRepr();
    if (args->getNumElements() == 1) {
      if (args->getElementType(0)->isSimpleUnqualifiedIdentifier(ctx.Id_Void)) {
        diagnose(args->getStartLoc(), diag::paren_void_probably_void)
            .fixItReplace(args->getSourceRange(), "()");
        repr->setWarned();
      }
    }
  }

  Type thrownTy;
  if (auto thrownTypeRepr = repr->getThrownTypeRepr()) {
    ASTContext &ctx = getASTContext();
    auto thrownTypeOptions = options.withoutContext();
    thrownTy = resolveType(thrownTypeRepr, thrownTypeOptions);
    if (thrownTy->hasError()) {
      thrownTy = Type();
    } else if (!options.contains(TypeResolutionFlags::SilenceErrors) &&
               !thrownTy->hasTypeParameter() &&
               !resolution.getDeclContext()->getParentModule()->checkConformance(
                  thrownTy, ctx.getErrorDecl())) {
      diagnoseInvalid(
          thrownTypeRepr, thrownTypeRepr->getLoc(), diag::thrown_type_not_error,
          thrownTy);
    }
  }

  bool hasSendingResult =
      isa_and_nonnull<TransferringTypeRepr>(repr->getResultTypeRepr()) ||
      isa_and_nonnull<SendingTypeRepr>(repr->getResultTypeRepr());

  // TODO: maybe make this the place that claims @escaping.
  bool noescape = isDefaultNoEscapeContext(parentOptions);

  FunctionType::ExtInfoBuilder extInfoBuilder(
      FunctionTypeRepresentation::Swift, noescape, repr->isThrowing(), thrownTy,
      diffKind, /*clangFunctionType*/ nullptr, isolation,
      LifetimeDependenceInfo(), hasSendingResult);

  const clang::Type *clangFnType = parsedClangFunctionType;
  if (shouldStoreClangType(representation) && !clangFnType)
    clangFnType =
        getASTContext().getClangFunctionType(params, outputTy, representation);

  auto extInfo = extInfoBuilder.withRepresentation(representation)
                     .withSendable(sendable)
                     .withAsync(repr->isAsync())
                     .withClangFunctionType(clangFnType)
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
                                              TypeResolutionOptions options,
                                              TypeAttrSet *attrs) {
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

  bool capturesGenerics = claim<CapturesGenericsTypeAttr>(attrs);

  auto layout = SILLayout::get(getASTContext(), genericSig, fields,
                               capturesGenerics);
  return SILBoxType::get(getASTContext(), layout, subMap);
}

NeverNullType TypeResolver::resolveSILFunctionType(FunctionTypeRepr *repr,
                                                   TypeResolutionOptions options,
                                                   TypeAttrSet *attrs) {
  assert(silContext);
  options.setContext(std::nullopt);

  bool hasError = false;

  auto coroutineKind = SILCoroutineKind::None;
  if (auto coroAttr = attrs ? attrs->claim(TAR_SILCoroutine) : nullptr) {
    assert(isa<YieldOnceTypeAttr>(coroAttr) ||
           isa<YieldManyTypeAttr>(coroAttr));
    coroutineKind = (isa<YieldOnceTypeAttr>(coroAttr)
                       ? SILCoroutineKind::YieldOnce
                       : SILCoroutineKind::YieldMany);
  }

  ParameterConvention callee = ParameterConvention::Direct_Unowned;
  if (auto calleeAttr = attrs ? attrs->claim(TAR_SILCalleeConvention) : nullptr) {
    assert(isa<CalleeOwnedTypeAttr>(calleeAttr) ||
           isa<CalleeGuaranteedTypeAttr>(calleeAttr));
    callee = (isa<CalleeOwnedTypeAttr>(calleeAttr)
                ? ParameterConvention::Direct_Owned
                : ParameterConvention::Direct_Guaranteed);
  }

  SILFunctionType::Representation representation =
    SILFunctionType::Representation::Thick;
  const clang::Type *clangFnType = nullptr;
  TypeRepr *witnessMethodProtocol = nullptr;
  auto conventionAttr = claim<ConventionTypeAttr>(attrs);
  if (conventionAttr) {
    auto parsedRep =
      llvm::StringSwitch<std::optional<SILFunctionType::Representation>>(
            conventionAttr->getConventionName())
        .Case("thick", SILFunctionType::Representation::Thick)
        .Case("block", SILFunctionType::Representation::Block)
        .Case("thin", SILFunctionType::Representation::Thin)
        .Case("c", SILFunctionType::Representation::CFunctionPointer)
        .Case("method", SILFunctionType::Representation::Method)
        .Case("objc_method",
              SILFunctionType::Representation::ObjCMethod)
        .Case("witness_method",
              SILFunctionType::Representation::WitnessMethod)
        .Case("keypath_accessor_getter",
              SILFunctionType::Representation::KeyPathAccessorGetter)
        .Case("keypath_accessor_setter",
              SILFunctionType::Representation::KeyPathAccessorSetter)
        .Case("keypath_accessor_equals",
              SILFunctionType::Representation::KeyPathAccessorEquals)
        .Case("keypath_accessor_hash",
              SILFunctionType::Representation::KeyPathAccessorHash)
        .Default(std::nullopt);
    if (!parsedRep) {
      conventionAttr->setInvalid();
      diagnoseInvalid(repr, conventionAttr->getAtLoc(),
                      diag::unsupported_sil_convention,
                      conventionAttr->getConventionName());
      hasError = true;
    } else {
      representation = *parsedRep;

      clangFnType = tryParseClangType(conventionAttr,
                                      shouldStoreClangType(representation));

      if (*parsedRep == SILFunctionType::Representation::WitnessMethod) {
        auto protocolName = conventionAttr->getWitnessMethodProtocol();
        // FIXME: parse the DeclNameLoc to here
        witnessMethodProtocol = UnqualifiedIdentTypeRepr::create(
            getASTContext(), DeclNameLoc(), protocolName);
      }
    }
  }

  DifferentiabilityKind diffKind = DifferentiabilityKind::NonDifferentiable;
  if (auto diffAttr = claim<DifferentiableTypeAttr>(attrs)) {
    auto *SF = getDeclContext()->getParentSourceFile();
    if (SF && isDifferentiableProgrammingEnabled(*SF)) {
      diffKind = diffAttr->getDifferentiability();
    } else {
      diagnoseInvalid(
          repr, diffAttr->getAtLoc(),
          diag::differentiable_programming_attr_used_without_required_module,
          diffAttr->getAttrName(), getASTContext().Id_Differentiation);
      hasError = true;
    }
  }

  bool pseudogeneric = claim<PseudogenericTypeAttr>(attrs);
  bool noescape = claim<NoEscapeTypeAttr>(attrs);
  bool sendable = claim<SendableTypeAttr>(attrs);
  bool async = claim<AsyncTypeAttr>(attrs);
  bool unimplementable = claim<UnimplementableTypeAttr>(attrs);
  auto isolation = SILFunctionTypeIsolation::Unknown;

  if (auto isolatedAttr = claim<IsolatedTypeAttr>(attrs)) {
    switch (isolatedAttr->getIsolationKind()) {
    case IsolatedTypeAttr::IsolationKind::Dynamic:
      if (representation != SILFunctionType::Representation::Thick) {
        assert(conventionAttr);
        diagnoseInvalid(repr, isolatedAttr->getAtLoc(),
                        diag::isolated_attr_bad_convention,
                        isolatedAttr->getIsolationKindName(),
                        conventionAttr->getConventionName());
      } else {
        isolation = SILFunctionTypeIsolation::Erased;
      }
      break;
    }
  }

  auto extInfoBuilder = SILFunctionType::ExtInfoBuilder(
      representation, pseudogeneric, noescape, sendable, async, unimplementable,
      isolation, diffKind, clangFnType, LifetimeDependenceInfo());

  // Resolve parameter and result types using the function's generic
  // environment.
  SmallVector<SILParameterInfo, 4> params;
  SmallVector<SILYieldInfo, 4> yields;
  SmallVector<SILResultInfo, 4> results;
  std::optional<SILResultInfo> errorResult;

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

    witnessMethodConformance = getDeclContext()->getParentModule()
      ->checkConformance(selfType, protocolType->getDecl());
    assert(witnessMethodConformance &&
           "found witness_method without matching conformance");
  }

  if (shouldStoreClangType(representation) && !clangFnType) {
    assert(results.size() <= 1 && yields.size() == 0 &&
           "C functions and blocks have at most 1 result and 0 yields.");
    auto result = results.empty() ? std::optional<SILResultInfo>() : results[0];
    clangFnType = getASTContext().getCanonicalClangFunctionType(params, result,
                                                                representation);
    extInfoBuilder = extInfoBuilder.withClangFunctionType(clangFnType);
  }

  std::optional<LifetimeDependenceInfo> lifetimeDependenceInfo;
  if (auto *lifetimeDependentTypeRepr =
          dyn_cast<LifetimeDependentReturnTypeRepr>(
              repr->getResultTypeRepr())) {
    lifetimeDependenceInfo = LifetimeDependenceInfo::fromTypeRepr(
        lifetimeDependentTypeRepr, params, getDeclContext());
    if (lifetimeDependenceInfo.has_value()) {
      extInfoBuilder =
          extInfoBuilder.withLifetimeDependenceInfo(*lifetimeDependenceInfo);
    }
  }

  return SILFunctionType::get(genericSig.getCanonicalSignature(),
                              extInfoBuilder.build(), coroutineKind,
                              callee, params, yields, results, errorResult,
                              patternSubs, invocationSubs, getASTContext(),
                              witnessMethodConformance);
}

SILYieldInfo TypeResolver::resolveSILYield(TypeRepr *repr,
                                           TypeResolutionOptions options,
                                           TypeAttrSet &attrs) {
  options.setContext(TypeResolverContext::FunctionInput);
  SILParameterInfo paramInfo = resolveSILParameter(repr, options, &attrs);

  attrs.diagnoseUnclaimed(resolution, options, paramInfo.getInterfaceType());

  return SILYieldInfo(paramInfo.getInterfaceType(), paramInfo.getConvention());
}

SILParameterInfo TypeResolver::resolveSILParameter(
                                 TypeRepr *repr,
                                 TypeResolutionOptions options,
                                 TypeAttrSet *yieldAttrs) {
  assert(options.is(TypeResolverContext::FunctionInput) &&
         "Parameters should be marked as inputs");

  auto convention = DefaultParameterConvention;
  auto parameterOptions = SILParameterInfo::Options();

  std::optional<TypeAttrSet> attrsBuffer;
  TypeAttrSet *attrs = nullptr;
  if (yieldAttrs) {
    attrs = yieldAttrs;
    assert(!isa<AttributedTypeRepr>(repr));
  } else if (auto attrRepr = dyn_cast<AttributedTypeRepr>(repr)) {
    attrsBuffer.emplace(getASTContext());
    attrs = &*attrsBuffer;
    repr = attrs->accumulate(attrRepr);
  }

  Type type;
  if (attrs) {
    attrs->claimAllWhere([&](TypeAttribute *attr) {
      switch (attr->getKind()) {

#define OWNERSHIP(ATTR, KIND)                                                  \
  case TypeAttrKind::ATTR:                                                     \
    convention = ParameterConvention::KIND;                                    \
    return true;
        OWNERSHIP(InGuaranteed, Indirect_In_Guaranteed)
        OWNERSHIP(InCXX, Indirect_In_CXX)
        OWNERSHIP(In, Indirect_In)
        OWNERSHIP(InConstant, Indirect_In)
        OWNERSHIP(Inout, Indirect_Inout)
        OWNERSHIP(InoutAliasable, Indirect_InoutAliasable)
        OWNERSHIP(Owned, Direct_Owned)
        OWNERSHIP(Guaranteed, Direct_Guaranteed)
        OWNERSHIP(PackOwned, Pack_Owned)
        OWNERSHIP(PackGuaranteed, Pack_Guaranteed)
        OWNERSHIP(PackInout, Pack_Inout)
#undef OWNERSHIP

      case TypeAttrKind::NoDerivative:
        parameterOptions |= SILParameterInfo::NotDifferentiable;
        return true;

      case TypeAttrKind::SILIsolated:
        parameterOptions |= SILParameterInfo::Isolated;
        return true;

      case TypeAttrKind::SILSending:
        parameterOptions |= SILParameterInfo::Sending;
        return true;

      default:
        return false;
      }
    });

    type = resolveAttributedType(repr, options, *attrs);

    if (!yieldAttrs)
      attrs->diagnoseUnclaimed(resolution, options, type);
  } else {
    type = resolveType(repr, options);
  }

  bool hadError = false;
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
                          parameterOptions);
}

bool TypeResolver::resolveSingleSILResult(
    TypeRepr *repr, TypeResolutionOptions options,
    SmallVectorImpl<SILYieldInfo> &yields,
    SmallVectorImpl<SILResultInfo> &ordinaryResults,
    std::optional<SILResultInfo> &errorResult) {
  Type type;
  auto convention = DefaultResultConvention;
  bool isErrorResult = false;
  SILResultInfo::Options resultInfoOptions;

  options.setContext(TypeResolverContext::FunctionResult);

  // Look through LifetimeDependentReturnTypeRepr.
  // LifetimeDependentReturnTypeRepr will be processed separately when building
  // SILFunctionType.
  if (auto *lifetimeDependentTypeRepr =
          dyn_cast<LifetimeDependentReturnTypeRepr>(repr)) {
    repr = lifetimeDependentTypeRepr->getBase();
  }

  if (auto attrRepr = dyn_cast<AttributedTypeRepr>(repr)) {
    TypeAttrSet attrs(getASTContext());
    auto repr = attrs.accumulate(attrRepr);

    // Recognize @yields.
    if (claim<YieldsTypeAttr>(attrs)) {
      // The treatment from this point on is basically completely different.
      auto yield = resolveSILYield(repr, options, attrs);
      if (yield.getInterfaceType()->hasError())
        return true;

      yields.push_back(yield);
      return false;
    }

    if (auto conventionAttr = attrs.claim(TAR_SILValueConvention)) {
      switch (conventionAttr->getKind()) {
#define ERROR(ATTR, CONVENTION)                                                \
  case TypeAttrKind::ATTR:                                                     \
    isErrorResult = true;                                                      \
    convention = ResultConvention::CONVENTION;                                 \
    break;
#define NORMAL(ATTR, CONVENTION)                                               \
  case TypeAttrKind::ATTR:                                                     \
    convention = ResultConvention::CONVENTION;                                 \
    break;

        ERROR(Error, Owned)
        ERROR(ErrorIndirect, Indirect)
        ERROR(ErrorUnowned, Unowned)
        NORMAL(Out, Indirect)
        NORMAL(Owned, Owned)
        NORMAL(UnownedInnerPointer, UnownedInnerPointer)
        NORMAL(Autoreleased, Autoreleased)
        NORMAL(PackOut, Pack)
#undef NORMAL
#undef ERROR

      default:
        diagnose(conventionAttr->getStartLoc(),
                 diag::sil_function_invalid_convention,
                 /*result*/ 1);
      }
    }

    // Recognize `@noDerivative`.
    if (claim<NoDerivativeTypeAttr>(attrs)) {
      resultInfoOptions |= SILResultInfo::NotDifferentiable;
    }

    if (claim<SILSendingTypeAttr>(attrs)) {
      resultInfoOptions |= SILResultInfo::IsSending;
    }

    type = resolveAttributedType(repr, options, attrs);
    attrs.diagnoseUnclaimed(resolution, options, type);
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

  SILResultInfo resolvedResult(type->getCanonicalType(), convention,
                               resultInfoOptions);

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
    std::optional<SILResultInfo> &errorResult) {
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
TypeResolver::resolveDeclRefTypeReprRec(DeclRefTypeRepr *repr,
                                        TypeResolutionOptions options) {
  auto &ctx = getASTContext();

  Type result;

  if (auto *unqualIdentTR = dyn_cast<UnqualifiedIdentTypeRepr>(repr)) {
    // The base component uses unqualified lookup.
    result = resolveUnqualifiedIdentTypeRepr(resolution.withOptions(options),
                                             silContext, unqualIdentTR);

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
    auto *qualIdentTR = cast<QualifiedIdentTypeRepr>(repr);
    auto *baseTR = qualIdentTR->getBase();

    Type baseTy;
    if (auto *declRefBaseTR = dyn_cast<DeclRefTypeRepr>(baseTR)) {
      baseTy = resolveDeclRefTypeReprRec(declRefBaseTR, options);
    } else {
      baseTy = resolveType(baseTR, options);
    }

    if (baseTy->hasError()) {
      return ErrorType::get(ctx);
    }

    result = resolveQualifiedIdentTypeRepr(resolution.withOptions(options),
                                           silContext, baseTy, qualIdentTR);
  }

  return result->hasError() ? ErrorType::get(ctx) : result;
}

NeverNullType
TypeResolver::resolveDeclRefTypeRepr(DeclRefTypeRepr *repr,
                                     TypeResolutionOptions options) {
  Type result = resolveDeclRefTypeReprRec(repr, options);

  // Diagnose an error if generic arguments are missing.
  if (result->is<UnboundGenericType>() && !repr->hasGenericArgList() &&
      !resolution.getUnboundTypeOpener() &&
      !options.is(TypeResolverContext::TypeAliasDecl) &&
      !options.is(TypeResolverContext::ExtensionBinding)) {

    if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
      // Tailored diagnostic for custom attributes.
      if (options.is(TypeResolverContext::CustomAttr)) {
        auto &ctx = resolution.getASTContext();
        ctx.Diags.diagnose(repr->getNameLoc(), diag::unknown_attribute,
                           repr->getNameRef().getBaseIdentifier().str());

        return ErrorType::get(ctx);
      }

      diagnoseUnboundGenericType(result, repr->getNameLoc().getBaseNameLoc());
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
      diagnose(repr->getNameLoc(), diag::cannot_find_type_in_scope,
               DeclNameRef(moduleName));
      diagnose(repr->getNameLoc(), diag::note_module_as_type, moduleName);
    }
    repr->setInvalid();
    return ErrorType::get(getASTContext());
  }

    auto *dc = getDeclContext();
    auto &ctx = getASTContext();

    // Gate the 'Escapable' type behind a specific flag for now.
    //
    // NOTE: we do not return an ErrorType, though! We're just artificially
    // preventing people from referring to the type without the feature.
    if (auto proto = result->getAs<ProtocolType>()) {
      if (auto known = proto->getKnownProtocol()) {
        if (*known == KnownProtocolKind::Escapable
            && !isSILSourceFile()
            && !isInterfaceFile()
            && !ctx.LangOpts.hasFeature(Feature::NonescapableTypes)) {
          diagnoseInvalid(repr, repr->getLoc(),
                          diag::escapable_requires_feature_flag);
        }
      }
    }

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
    if (options.hasBase(TypeResolverContext::SubscriptDecl) ||
        options.hasBase(TypeResolverContext::EnumElementDecl)) {
      diagID = diag::attr_only_valid_on_func_or_init_params;
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
  case ParamSpecifier::ImplicitlyCopyableConsuming:
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

  // Keep the `type` to be returned, while we unwrap and inspect the inner
  // type for whether it can be isolated on.
  Type type = resolveType(repr->getBase(), options);
  Type unwrappedType = type;

  // Optional actor types are fine - `nil` represents `nonisolated`.
  if (auto wrappedOptionalType = unwrappedType->getOptionalObjectType()) {
    unwrappedType = wrappedOptionalType;
  }

  if (auto dynamicSelfType = dyn_cast<DynamicSelfType>(unwrappedType)) {
    unwrappedType = dynamicSelfType->getSelfType();
  }

  if (inStage(TypeResolutionStage::Interface)) {
    if (auto *env = resolution.getGenericSignature().getGenericEnvironment())
      unwrappedType = env->mapTypeIntoContext(unwrappedType);

    if (!unwrappedType->isAnyActorType() && !unwrappedType->hasError()) {
      diagnoseInvalid(
          repr, repr->getSpecifierLoc(),
          diag::isolated_parameter_not_actor, type);
      return ErrorType::get(type);
    }
  }

  return type;
}

NeverNullType
TypeResolver::resolveSendingTypeRepr(SendingTypeRepr *repr,
                                     TypeResolutionOptions options) {
  if (options.is(TypeResolverContext::TupleElement)) {
    diagnoseInvalid(repr, repr->getSpecifierLoc(),
                    diag::sending_cannot_be_applied_to_tuple_elt);
    return ErrorType::get(getASTContext());
  }

  if (!options.is(TypeResolverContext::ClosureExpr) &&
      !options.is(TypeResolverContext::FunctionResult) &&
      (!options.is(TypeResolverContext::FunctionInput) ||
       options.hasBase(TypeResolverContext::EnumElementDecl))) {
    diagnoseInvalid(repr, repr->getSpecifierLoc(),
                    diag::sending_only_on_parameters_and_results);
    return ErrorType::get(getASTContext());
  }

  // Return the type.
  return resolveType(repr->getBase(), options);
}

NeverNullType
TypeResolver::resolveTransferringTypeRepr(TransferringTypeRepr *repr,
                                          TypeResolutionOptions options) {
  if (options.is(TypeResolverContext::TupleElement)) {
    diagnoseInvalid(repr, repr->getSpecifierLoc(),
                    diag::transferring_cannot_be_applied_to_tuple_elt);
    return ErrorType::get(getASTContext());
  }

  if (!options.is(TypeResolverContext::FunctionResult) &&
      (!options.is(TypeResolverContext::FunctionInput) ||
       options.hasBase(TypeResolverContext::EnumElementDecl))) {
    diagnoseInvalid(repr, repr->getSpecifierLoc(),
                    diag::transferring_only_on_parameters_and_results);
    return ErrorType::get(getASTContext());
  }

  // Return the type.
  return resolveType(repr->getBase(), options);
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
  {
    // Check that we can validly substitute the baseTy into an array. We do not
    // actually resolve to that valid array type, as we want to return the
    // sugared Type node ArraySliceType instead!
    auto *arrayDecl = ctx.getArrayDecl();
    if (!arrayDecl) {
      ctx.Diags.diagnose(repr->getBrackets().Start,
                         diag::sugar_type_not_found, 0);
      return ErrorType::get(ctx);
    }

    Type genericArgs[1] = {baseTy};
    auto arrayTy =
        resolution.applyUnboundGenericArguments(arrayDecl,
            /*parentTy=*/nullptr,
                                                repr->getBrackets().Start,
                                                genericArgs);
    if (arrayTy->hasError()) {
      return ErrorType::get(ctx);
    }
  }

  return ArraySliceType::get(baseTy);
}

NeverNullType TypeResolver::resolveLifetimeDependentReturnTypeRepr(
    LifetimeDependentReturnTypeRepr *repr, TypeResolutionOptions options) {
  if (options.is(TypeResolverContext::TupleElement)) {
    diagnoseInvalid(repr, repr->getSpecifierLoc(),
                    diag::lifetime_dependence_cannot_be_applied_to_tuple_elt);
    return ErrorType::get(getASTContext());
  }
  if (!options.is(TypeResolverContext::FunctionResult)) {
    diagnoseInvalid(
        repr, repr->getSpecifierLoc(),
        diag::lifetime_dependence_only_on_function_method_init_result);
    return ErrorType::get(getASTContext());
  }
  return resolveType(repr->getBase(), options);
}

NeverNullType
TypeResolver::resolveDictionaryType(DictionaryTypeRepr *repr,
                                    TypeResolutionOptions options) {
  auto argOptions = options.withoutContext().withContext(
      TypeResolverContext::ScalarGenericArgument);

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

  {
    // Check that we can validly substitute the baseTy into an Optional
    Type genericArgs[1] = {baseTy};
    auto substTy =
        resolution.applyUnboundGenericArguments(ctx.getOptionalDecl(),
                                                /*parentTy=*/nullptr,
                                                repr->getQuestionLoc(),
                                                genericArgs);
    if (substTy->hasError()) {
      return ErrorType::get(ctx);
    }
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
  case TypeResolverContext::ScalarGenericArgument:
  case TypeResolverContext::VariadicGenericArgument:
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
  case TypeResolverContext::Inverted:
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

  {
    // Check that we can validly substitute the baseTy into an Optional
    Type genericArgs[1] = {baseTy};
    auto substTy =
        resolution.applyUnboundGenericArguments(ctx.getOptionalDecl(),
            /*parentTy=*/nullptr,
                                                repr->getExclamationLoc(),
                                                genericArgs);
    if (substTy->hasError()) {
      return ErrorType::get(ctx);
    }
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
  if (inStage(TypeResolutionStage::Interface)) {
    auto contextTy = GenericEnvironment::mapTypeIntoContext(
        resolution.getGenericSignature().getGenericEnvironment(), element);
    if (!contextTy->hasError() && contextTy->isNoncopyable()) {
      diagnoseInvalid(repr, repr->getLoc(), diag::noncopyable_generics_variadic,
                      element);
      return ErrorType::get(getASTContext());
    }
  }

  return element;
}

NeverNullType TypeResolver::resolvePackType(PackTypeRepr *repr,
                                            TypeResolutionOptions options,
                                            TypeAttrSet *attrs) {
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
    bool silDirect = claim<DirectTypeAttr>(attrs);

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

  if (!packReference->isRootParameterPack()) {
    auto diag =
        ctx.Diags.diagnose(repr->getLoc(), diag::each_non_pack, packReference);
    bool addEachFixitApplied = false;
    if (auto *packIdent =
            dyn_cast<UnqualifiedIdentTypeRepr>(repr->getPackType())) {
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
  std::optional<unsigned> moveOnlyElementIndex = std::nullopt;
  for (unsigned i = 0, end = repr->getNumElements(); i != end; ++i) {
    auto *tyR = repr->getElementType(i);

    auto ty = resolveType(tyR, elementOptions);
    if (ty->hasError()) {
      hadError = true;
    }
    // Tuples with move-only elements aren't yet supported.
    // Track the presence of a noncopyable field for diagnostic purposes only.
    // We don't need to re-diagnose if a tuple contains another tuple, though,
    // since we should've diagnosed the inner tuple already.
    if (!ctx.LangOpts.hasFeature(Feature::MoveOnlyTuples) &&
        !options.contains(TypeResolutionFlags::SILMode) &&
        inStage(TypeResolutionStage::Interface) &&
        !moveOnlyElementIndex.has_value() &&
        !ty->hasUnboundGenericType() &&
        !isa<TupleTypeRepr>(tyR)) {
      auto contextTy = GenericEnvironment::mapTypeIntoContext(
          resolution.getGenericSignature().getGenericEnvironment(), ty);
      if (contextTy->isNoncopyable())
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
  
  if (moveOnlyElementIndex.has_value()) {
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

  SmallVector<Type, 4> Members;

  // Note that the superclass type will appear as part of one of the
  // types in 'Members', so it's not used when constructing the
  // fully-realized type below -- but we just record it to make sure
  // there is only one superclass.
  Type SuperclassType;

  // Did we see at least one protocol or inverse?
  bool HasNonClassMember = false;

  // If true, we cannot form a composition from these members.
  bool IsInvalid = false;

  std::function<void (SourceLoc, Type)> checkMember
      = [&](SourceLoc loc, Type ty) {
    if (auto pct = ty->getAs<ProtocolCompositionType>()) {
      if (!pct->getInverses().empty())
        HasNonClassMember = true;

      for (auto member : pct->getMembers())
        checkMember(loc, member);
      return;
    }

    if (ty->is<ProtocolType>() ||
        ty->is<ParameterizedProtocolType>()) {
      HasNonClassMember = true;
      return;
    }

    assert(isa<ClassDecl>(ty->getAnyNominal()));

    if (SuperclassType && !SuperclassType->isEqual(ty)) {
      diagnose(loc, diag::protocol_composition_one_class, ty,
               SuperclassType);
      IsInvalid = true;
      return;
    }

    SuperclassType = ty;
  };

  for (auto tyR : repr->getTypes()) {
    auto ty = resolveType(tyR,
        options.withContext(TypeResolverContext::GenericRequirement));
    if (ty->hasError()) return ty;

    if (ty->is<ProtocolType>()) {
      checkMember(tyR->getStartLoc(), ty);
      Members.push_back(ty);
      continue;
    }

    // FIXME: Support compositions involving parameterized protocol types,
    // like 'any Collection<String> & Sendable', etc.
    if (ty->is<ParameterizedProtocolType>() &&
        !options.isConstraintImplicitExistential() &&
        options.getContext() != TypeResolverContext::ExistentialConstraint) {
      checkMember(tyR->getStartLoc(), ty);
      Members.push_back(ty);
      continue;
    }

    if (ty->is<ProtocolCompositionType>()) {
      checkMember(tyR->getStartLoc(), ty);
      Members.push_back(ty);
      continue;
    }

    if (isa_and_nonnull<ClassDecl>(ty->getAnyNominal())) {
      checkMember(tyR->getStartLoc(), ty);
      Members.push_back(ty);
      continue;
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
  if (SuperclassType && !HasNonClassMember)
    return SuperclassType;

  auto composition =
      ProtocolCompositionType::get(getASTContext(), Members,
                                   /*Inverses=*/{},
                                   /*HasExplicitAnyObject=*/false);

  // Flatten the composition.
  if (auto canComposition = dyn_cast<ProtocolCompositionType>(
          composition->getCanonicalType())) {
    auto inverses = canComposition->getInverses();
    auto layout = composition->getExistentialLayout();

    // Cannot provide an inverse in the same composition requiring the protocol.
    for (auto ip : inverses) {
      auto kp = getKnownProtocolKind(ip);

      if (layout.requiresClass()) {
        bool hasExplicitAnyObject = layout.hasExplicitAnyObject;
        diagnose(repr->getStartLoc(),
                 diag::inverse_with_class_constraint,
                 hasExplicitAnyObject,
                 getProtocolName(kp),
                 layout.getSuperclass());
        IsInvalid = true;
        break;
      }

      auto *proto = getASTContext().getProtocol(kp);
      for (auto *otherProto : layout.getProtocols()) {
        if (proto == otherProto ||
            otherProto->inheritsFrom(proto)) {
          diagnose(repr->getLoc(),
                   diag::inverse_conflicts_explicit_composition,
                   getProtocolName(kp));
          IsInvalid = true;
          break;
        }
      }
    }
  }

  if (IsInvalid) {
    repr->setInvalid();
    return ErrorType::get(getASTContext());
  }

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

  std::optional<MetatypeRepresentation> storedRepr;

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
    std::optional<MetatypeRepresentation> storedRepr) {
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

NeverNullType TypeResolver::resolveInverseType(InverseTypeRepr *repr,
                                               TypeResolutionOptions options) {
  auto subOptions = options.withoutContext(true)
      .withContext(TypeResolverContext::Inverted);
  auto ty = resolveType(repr->getConstraint(), subOptions);
  if (ty->hasError())
    return ErrorType::get(getASTContext());

  // If the inverted type is an existential metatype, unwrap the existential
  // metatype so we can look at the instance type. We'll re-wrap at the end.
  ExistentialMetatypeType *existentialTy =
      dyn_cast<ExistentialMetatypeType>(ty.get().getPointer());
  if (existentialTy) {
    ty = existentialTy->getInstanceType();
  }

  auto wrapInExistential = [existentialTy](Type type) -> Type {
    if (!existentialTy)
      return type;

    std::optional<MetatypeRepresentation> repr;
    if (existentialTy->hasRepresentation())
      repr = existentialTy->getRepresentation();
    return ExistentialMetatypeType::get(type, repr);
  };

  if (auto kp = ty->getKnownProtocol()) {
    if (auto kind = getInvertibleProtocolKind(*kp)) {

      // Gate the '~Escapable' type behind a specific flag for now.
      // Uses of 'Escapable' itself are already diagnosed; return ErrorType.
      if (*kind == InvertibleProtocolKind::Escapable &&
          !getASTContext().LangOpts.hasFeature(Feature::NonescapableTypes)) {
        return wrapInExistential(ErrorType::get(getASTContext()));
      }

      return wrapInExistential(
          ProtocolCompositionType::getInverseOf(getASTContext(), *kind));
    }
  }

  // Rewrap for diagnostic purposes.
  ty = wrapInExistential(ty);
  diagnoseInvalid(repr, repr->getLoc(), diag::inverse_type_not_invertible, ty);
  return ErrorType::get(getASTContext());
}

NeverNullType TypeResolver::resolveProtocolType(ProtocolTypeRepr *repr,
                                                TypeResolutionOptions options) {
  // The instance type of a metatype is always abstract, not SIL-lowered.
  auto ty = resolveType(repr->getBase(),
      options.withContext(TypeResolverContext::ProtocolMetatypeBase));
  if (ty->hasError()) {
    return ErrorType::get(getASTContext());
  }

  std::optional<MetatypeRepresentation> storedRepr;

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
    std::optional<MetatypeRepresentation> storedRepr) {
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

/// Usually, existential types, existential metatypes, and singleton
/// metatypes of existential must be written using `any` syntax. For example,
/// `any P`, `any P.Type`, and `(any P).Type` respectively. This walker
/// traverses an AST in search for occurrences of these types and checks their
/// written syntax.
class ExistentialTypeSyntaxChecker : public ASTWalker {
  ASTContext &Ctx;
  bool checkStatements;
  bool hitTopStmt;

  unsigned exprCount = 0;
  llvm::SmallVector<TypeRepr *, 4> reprStack;
    
public:
  ExistentialTypeSyntaxChecker(ASTContext &ctx, bool checkStatements)
      : Ctx(ctx), checkStatements(checkStatements), hitTopStmt(false) {}

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
    if (T->isInvalid())
      return Action::SkipNode();

    reprStack.push_back(T);

    auto *declRefTR = dyn_cast<DeclRefTypeRepr>(T);
    if (!declRefTR) {
      return Action::Continue();
    }

    // We only care about the type of an outermost member type representation.
    // For example, in `A<T>.B.C<U>`, check `C` and generic arguments `U` and
    // `T`, but not `A` or `B`.
    if (auto *parentQualIdentTR =
            dyn_cast_or_null<QualifiedIdentTypeRepr>(Parent.getAsTypeRepr())) {
      if (T == parentQualIdentTR->getBase()) {
        return Action::Continue();
      }
    }

    checkDeclRefTypeRepr(declRefTR);

    return Action::Continue();
  }

  PostWalkAction walkToTypeReprPost(TypeRepr *T) override {
    assert(reprStack.back() == T);
    reprStack.pop_back();
    return Action::Continue();
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    if (checkStatements && !hitTopStmt) {
      hitTopStmt = true;
      return Action::Continue(S);
    }

    return Action::SkipNode(S);
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    return Action::SkipNodeIf(checkStatements);
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    ++exprCount;
    return Action::Continue(E);
  }

  PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
    --exprCount;
    return Action::Continue(E);
  }

private:
  /// Returns a Boolean value indicating whether the insertion of `any` before
  /// a type representation with the given parent requires paretheses.
  static bool anySyntaxNeedsParens(TypeRepr *parent) {
    switch (parent->getKind()) {
    case TypeReprKind::Optional:
    case TypeReprKind::ImplicitlyUnwrappedOptional:
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
    case TypeReprKind::UnqualifiedIdent:
    case TypeReprKind::QualifiedIdent:
    case TypeReprKind::Dictionary:
    case TypeReprKind::Inverse:
    case TypeReprKind::Tuple:
    case TypeReprKind::Fixed:
    case TypeReprKind::Self:
    case TypeReprKind::Array:
    case TypeReprKind::SILBox:
    case TypeReprKind::Isolated:
    case TypeReprKind::Transferring:
    case TypeReprKind::Sending:
    case TypeReprKind::Placeholder:
    case TypeReprKind::CompileTimeConst:
    case TypeReprKind::Vararg:
    case TypeReprKind::Pack:
    case TypeReprKind::PackExpansion:
    case TypeReprKind::PackElement:
    case TypeReprKind::LifetimeDependentReturn:
      return false;
    }
  }

  void emitInsertAnyFixit(InFlightDiagnostic &diag, DeclRefTypeRepr *T) const {
    TypeRepr *replacementT = T;

    // Insert parens in expression context for '(any P).self'
    bool needsParens = (exprCount != 0);

    // Compute the replacement node (the node to which to apply `any`).
    if (reprStack.size() > 1) {
      auto it = reprStack.end() - 1;
      auto replacementIt = it;

      // Backtrack the stack and expand the replacement range to any parent
      // inverses, compositions or `.Type` metatypes, skipping only parentheses.
      //
      // E.g. `(X & ~P).Type` → `any (X & ~P).Type`.
      //             ↑
      //             We're here
      do {
        --it;
        if ((*it)->isParenType()) {
          continue;
        }

        if (isa<InverseTypeRepr>(*it) || isa<CompositionTypeRepr>(*it) ||
            isa<MetatypeTypeRepr>(*it)) {
          replacementIt = it;
          continue;
        }

        break;
      } while (it != reprStack.begin());

      // Whether parentheses are necessary is determined by the immediate parent
      // of the replacement node.
      if (replacementIt != reprStack.begin()) {
        needsParens = anySyntaxNeedsParens(*(replacementIt - 1));
      }

      replacementT = *replacementIt;
    }

    llvm::SmallString<64> fix;
    {
      llvm::raw_svector_ostream OS(fix);
      if (needsParens)
        OS << "(";
      ExistentialTypeRepr existential(SourceLoc(), replacementT);
      existential.print(OS);
      if (needsParens)
        OS << ")";
    }

    diag.fixItReplace(replacementT->getSourceRange(), fix);
  }

  /// Returns a Boolean value indicating whether the type representation being
  /// visited, assuming it is a constraint type demanding `any` or `some`, is
  /// missing either keyword.
  bool isAnyOrSomeMissing() const {
    assert(isa<DeclRefTypeRepr>(reprStack.back()));

    if (reprStack.size() < 2) {
      return true;
    }

    auto it = reprStack.end() - 1;
    while (true) {
      --it;
      if (it == reprStack.begin()) {
        break;
      }

      // Look through parens, inverses, metatypes, and compositions.
      if ((*it)->isParenType() || isa<InverseTypeRepr>(*it) ||
          isa<CompositionTypeRepr>(*it) || isa<MetatypeTypeRepr>(*it)) {
        continue;
      }

      // Look through '?' and '!' too; `any P?` et al. is diagnosed in the
      // type resolver.
      if (isa<OptionalTypeRepr>(*it) ||
          isa<ImplicitlyUnwrappedOptionalTypeRepr>(*it)) {
        continue;
      }

      break;
    }

    return !(isa<OpaqueReturnTypeRepr>(*it) || isa<ExistentialTypeRepr>(*it));
  }

  void checkDeclRefTypeRepr(DeclRefTypeRepr *T) const {
    assert(!T->isInvalid());

    if (Ctx.LangOpts.hasFeature(Feature::ImplicitSome)) {
      return;
    }

    // Backtrack the stack, looking just through parentheses and metatypes. If
    // we find an inverse (which always requires `any`), diagnose it specially.
    if (reprStack.size() > 1) {
      auto it = reprStack.end() - 2;
      while (it != reprStack.begin() &&
             ((*it)->isParenType() || isa<MetatypeTypeRepr>(*it))) {
        --it;
        continue;
      }

      if (auto *inverse = dyn_cast<InverseTypeRepr>(*it);
          inverse && isAnyOrSomeMissing()) {
        auto diag = Ctx.Diags.diagnose(inverse->getTildeLoc(),
                                       diag::inverse_requires_any);
        emitInsertAnyFixit(diag, T);
        return;
      }
    }

    auto *decl = T->getBoundDecl();
    if (!decl) {
      return;
    }

    if (auto *proto = dyn_cast<ProtocolDecl>(decl)) {
      if (proto->existentialRequiresAny() && isAnyOrSomeMissing()) {
        auto diag =
            Ctx.Diags.diagnose(T->getNameLoc(), diag::existential_requires_any,
                               proto->getDeclaredInterfaceType(),
                               proto->getDeclaredExistentialType(),
                               /*isAlias=*/false);
        emitInsertAnyFixit(diag, T);
      }
    } else if (auto *alias = dyn_cast<TypeAliasDecl>(decl)) {
      auto type = Type(alias->getDeclaredInterfaceType()->getDesugaredType());

      // If this is a type alias to a constraint type, the type
      // alias name must be prefixed with 'any' to be used as an
      // existential type.
      if (type->isConstraintType() && !type->isAny() && !type->isAnyObject()) {
        bool diagnose = false;

        // Look for protocol members that require 'any'.
        auto layout = type->getExistentialLayout();
        for (auto *protoDecl : layout.getProtocols()) {
          if (protoDecl->existentialRequiresAny()) {
            diagnose = true;
            break;
          }
        }

        // If inverses are present, require 'any' too.
        if (auto *PCT = type->getAs<ProtocolCompositionType>())
          diagnose |= !PCT->getInverses().empty();

        if (diagnose && isAnyOrSomeMissing()) {
          auto diag = Ctx.Diags.diagnose(
              T->getNameLoc(), diag::existential_requires_any,
              alias->getDeclaredInterfaceType(),
              ExistentialType::get(alias->getDeclaredInterfaceType()),
              /*isAlias=*/true);
          emitInsertAnyFixit(diag, T);
        }
      }
    }
  }

public:
  void checkRequirements(ArrayRef<RequirementRepr> reqts) {
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
    ExistentialTypeSyntaxChecker checker(ctx, /*checkStatements=*/false);
    macroExpansionDecl->getArgs()->walk(checker);
    for (auto *genArg : macroExpansionDecl->getGenericArgs())
      genArg->walk(checker);
  }

  if (isa<TypeDecl>(decl) || isa<ExtensionDecl>(decl) ||
      isa<MacroExpansionDecl>(decl))
    return;

  ExistentialTypeSyntaxChecker checker(ctx, /*checkStatements=*/false);
  decl->walk(checker);
}

void TypeChecker::checkExistentialTypes(ASTContext &ctx, Stmt *stmt,
                                        DeclContext *DC) {
  if (!stmt)
    return;

  // Skip diagnosing existential `any` requirements in swiftinterfaces.
  auto sourceFile = DC->getParentSourceFile();
  if (sourceFile && sourceFile->Kind == SourceFileKind::Interface)
    return;

  ExistentialTypeSyntaxChecker checker(ctx, /*checkStatements=*/true);
  stmt->walk(checker);
}

void TypeChecker::checkExistentialTypes(ASTContext &ctx,
                                        TypeAliasDecl *typeAlias) {
  if (!typeAlias || !typeAlias->getUnderlyingTypeRepr())
    return;

  // A type alias to a plain constraint type is allowed.
  if (typeAlias->getUnderlyingType()->isConstraintType())
    return;

  ExistentialTypeSyntaxChecker checker(ctx, /*checkStatements=*/true);
  typeAlias->getUnderlyingTypeRepr()->walk(checker);
}

void TypeChecker::checkExistentialTypes(
    ASTContext &ctx, TrailingWhereClause *whereClause) {
  if (whereClause == nullptr)
    return;

  ExistentialTypeSyntaxChecker checker(ctx, /*checkStatements=*/false);
  checker.checkRequirements(whereClause->getRequirements());
}

void TypeChecker::checkExistentialTypes(
    ASTContext &ctx, GenericParamList *genericParams) {
  if (genericParams  == nullptr)
    return;

  ExistentialTypeSyntaxChecker checker(ctx, /*checkStatements=*/false);
  checker.checkRequirements(genericParams->getRequirements());
}

Type CustomAttrTypeRequest::evaluate(Evaluator &eval, CustomAttr *attr,
                                     DeclContext *dc,
                                     CustomAttrTypeKind typeKind) const {
  const TypeResolutionOptions options(TypeResolverContext::PatternBindingDecl);

  OpenUnboundGenericTypeFn unboundTyOpener = nullptr;
  // Property delegates allow their type to be an unbound generic.
  if (typeKind == CustomAttrTypeKind::PropertyWrapper) {
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


Type ExplicitCaughtTypeRequest::evaluate(
    Evaluator &evaluator, ASTContext *ctxPtr, CatchNode catchNode
) const {
  ASTContext &ctx = *ctxPtr;

  // try!/try? always catch 'any Error'.
  if (catchNode.is<AnyTryExpr *>()) {
    return ctx.getErrorExistentialType();
  }

  // Functions
  if (auto func = catchNode.dyn_cast<AbstractFunctionDecl *>()) {
    TypeRepr *thrownTypeRepr = func->getThrownTypeRepr();

    // If there is no explicit thrown type, check whether it throws at all.
    if (!thrownTypeRepr) {
      // If it throws, it throws 'any Error'.
      if (func->hasThrows())
        return ctx.getErrorExistentialType();

      // Otherwise, 'Never'.
      return ctx.getNeverType();
    }

    // We have an explicit thrown error type, so resolve it.
    auto options = TypeResolutionOptions(TypeResolverContext::None);
    if (func->preconcurrency())
      options |= TypeResolutionFlags::Preconcurrency;

    return TypeResolution::forInterface(func, options,
                                        /*unboundTyOpener*/ nullptr,
                                        PlaceholderType::get,
                                        /*packElementOpener*/ nullptr)
        .resolveType(thrownTypeRepr);
  }

  // Closures
  if (auto closure = catchNode.dyn_cast<ClosureExpr *>()) {
    // Explicit thrown error type.
    if (auto thrownTypeRepr = closure->getExplicitThrownTypeRepr()) {
      return TypeResolution::resolveContextualType(
               thrownTypeRepr, closure,
               TypeResolutionOptions(TypeResolverContext::None),
               /*unboundTyOpener*/ nullptr, PlaceholderType::get,
               /*packElementOpener*/ nullptr);
    }

    // Explicit 'throws' implies that this throws 'any Error'.
    if (closure->getThrowsLoc().isValid()) {
      return ctx.getErrorExistentialType();
    }

    // Thrown error type will be inferred.
    return Type();
  }

  // do..catch statements.
  if (auto doCatch = catchNode.dyn_cast<DoCatchStmt *>()) {
    // A do..catch block with no explicit 'throws' annotation will infer
    // the thrown error type.
    if (doCatch->getThrowsLoc().isInvalid()) {
      return Type();
    }

    auto typeRepr = doCatch->getCaughtTypeRepr();

    // If there is no explicitly-specified thrown error type, it's 'any Error'.
    if (!typeRepr) {
      return ctx.getErrorExistentialType();
    }

    return TypeResolution::resolveContextualType(
        typeRepr, doCatch->getDeclContext(),
        TypeResolutionOptions(TypeResolverContext::None),
        /*unboundTyOpener*/ nullptr, PlaceholderType::get,
        /*packElementOpener*/ nullptr);
  }

  llvm_unreachable("Unhandled catch node");
}
