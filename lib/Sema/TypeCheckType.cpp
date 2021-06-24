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
#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/NameLookup.h"
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
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;

#define DEBUG_TYPE "TypeCheckType"

/// Type resolution.

TypeResolution
TypeResolution::forStructural(DeclContext *dc, TypeResolutionOptions options,
                              OpenUnboundGenericTypeFn unboundTyOpener,
                              HandlePlaceholderTypeReprFn placeholderHandler) {
  return TypeResolution(dc, TypeResolutionStage::Structural, options,
                        unboundTyOpener, placeholderHandler);
}

TypeResolution
TypeResolution::forInterface(DeclContext *dc, TypeResolutionOptions options,
                             OpenUnboundGenericTypeFn unboundTyOpener,
                             HandlePlaceholderTypeReprFn placeholderHandler) {
  TypeResolution result(dc, TypeResolutionStage::Interface, options,
                        unboundTyOpener, placeholderHandler);
  result.complete.genericSig = dc->getGenericSignatureOfContext();
  result.complete.builder = nullptr;
  return result;
}

TypeResolution
TypeResolution::forContextual(DeclContext *dc, TypeResolutionOptions options,
                              OpenUnboundGenericTypeFn unboundTyOpener,
                              HandlePlaceholderTypeReprFn placeholderHandler) {
  return forContextual(dc, dc->getGenericEnvironmentOfContext(), options,
                       unboundTyOpener, placeholderHandler);
}

TypeResolution
TypeResolution::forContextual(DeclContext *dc, GenericEnvironment *genericEnv,
                              TypeResolutionOptions options,
                              OpenUnboundGenericTypeFn unboundTyOpener,
                              HandlePlaceholderTypeReprFn placeholderHandler) {
  TypeResolution result(dc, TypeResolutionStage::Contextual, options,
                        unboundTyOpener, placeholderHandler);
  result.genericEnv = genericEnv;
  return result;
}

TypeResolution TypeResolution::withOptions(TypeResolutionOptions opts) const {
  TypeResolution result(dc, stage, opts, unboundTyOpener, placeholderHandler);
  result.genericEnv = genericEnv;
  result.complete = complete;
  return result;
}

ASTContext &TypeResolution::getASTContext() const {
  return dc->getASTContext();
}

GenericSignatureBuilder *TypeResolution::getGenericSignatureBuilder() const {
  assert(stage == TypeResolutionStage::Interface);
  if (!complete.builder) {
    auto genericSig = getGenericSignature();
    complete.builder = genericSig->getGenericSignatureBuilder();
  }

  return complete.builder;
}

GenericSignature TypeResolution::getGenericSignature() const {
  switch (stage) {
  case TypeResolutionStage::Contextual:
    return dc->getGenericSignatureOfContext();

  case TypeResolutionStage::Interface:
    if (complete.genericSig)
      return complete.genericSig;

    return dc->getGenericSignatureOfContext();

  case TypeResolutionStage::Structural:
    return GenericSignature();
  }
  llvm_unreachable("unhandled stage");
}

bool TypeResolution::usesArchetypes() const {
  switch (stage) {
  case TypeResolutionStage::Structural:
  case TypeResolutionStage::Interface:
    return false;

  case TypeResolutionStage::Contextual:
    return true;
  }
  llvm_unreachable("unhandled stage");
}

Type TypeResolution::mapTypeIntoContext(Type type) const {
  switch (stage) {
  case TypeResolutionStage::Structural:
  case TypeResolutionStage::Interface:
    return type;

  case TypeResolutionStage::Contextual:
    return GenericEnvironment::mapTypeIntoContext(genericEnv, type);
  }
  llvm_unreachable("unhandled stage");
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

Type TypeResolution::resolveDependentMemberType(
                                          Type baseTy, DeclContext *DC,
                                          SourceRange baseRange,
                                          ComponentIdentTypeRepr *ref) const {
  // FIXME(ModQual): Reject qualified names immediately; they cannot be
  // dependent member types.
  Identifier refIdentifier = ref->getNameRef().getBaseIdentifier();

  switch (stage) {
  case TypeResolutionStage::Structural:
    return DependentMemberType::get(baseTy, refIdentifier);

  case TypeResolutionStage::Contextual:
    llvm_unreachable("Dependent type after archetype substitution");

  case TypeResolutionStage::Interface:
    // Handled below.
    break;
  }

  assert(stage == TypeResolutionStage::Interface);
  if (!getGenericSignature())
    return ErrorType::get(baseTy);

  auto builder = getGenericSignatureBuilder();
  auto baseEquivClass =
    builder->resolveEquivalenceClass(
                                baseTy,
                                ArchetypeResolutionKind::CompleteWellFormed);
  if (!baseEquivClass)
    return ErrorType::get(baseTy);

  ASTContext &ctx = baseTy->getASTContext();

  // Look for a nested type with the given name.
  if (auto nestedType =
          baseEquivClass->lookupNestedType(*builder, refIdentifier)) {
    // Record the type we found.
    ref->setValue(nestedType, nullptr);
  } else {
    // Resolve the base to a potential archetype.
    // Perform typo correction.
    TypoCorrectionResults corrections(ref->getNameRef(), ref->getNameLoc());
    TypeChecker::performTypoCorrection(DC, DeclRefKind::Ordinary,
                                       MetatypeType::get(baseTy),
                                       defaultMemberLookupOptions,
                                       corrections, builder);

    // Check whether we have a single type result.
    auto singleType = cast_or_null<TypeDecl>(
      corrections.getUniqueCandidateMatching([](ValueDecl *result) {
        return isa<TypeDecl>(result);
      }));

    // If we don't have a single result, complain and fail.
    if (!singleType) {
      auto name = ref->getNameRef();
      auto nameLoc = ref->getNameLoc();
      const auto kind = describeDeclOfType(baseTy);
      ctx.Diags.diagnose(nameLoc, diag::invalid_member_type, name, kind, baseTy)
          .highlight(baseRange);
      corrections.noteAllCandidates();

      return ErrorType::get(ctx);
    }

    // We have a single type result. Suggest it.
    ctx.Diags.diagnose(ref->getNameLoc(), diag::invalid_member_type_suggest,
                       baseTy, ref->getNameRef(),
                       singleType->getBaseName())
      .fixItReplace(ref->getNameLoc().getSourceRange(),
                    singleType->getBaseName().userFacingName());

    // Correct to the single type result.
    ref->overwriteNameRef(singleType->createNameRef());
    ref->setValue(singleType, nullptr);
  }

  auto *concrete = ref->getBoundDecl();

  // If the nested type has been resolved to an associated type, use it.
  if (auto assocType = dyn_cast<AssociatedTypeDecl>(concrete)) {
    return DependentMemberType::get(baseTy, assocType);
  }

  // Otherwise, the nested type comes from a concrete type,
  // or it's a typealias declared in protocol or protocol extension.
  // Substitute the base type into it.

  // Make sure that base type didn't get replaced along the way.
  assert(baseTy->isTypeParameter());

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
    baseTy = baseEquivClass->concreteType ? baseEquivClass->concreteType
                                          : baseEquivClass->superclass;
    assert(baseTy);
  }

  return TypeChecker::substMemberTypeWithBase(DC->getParentModule(), concrete,
                                              baseTy);
}

Type TypeResolution::resolveSelfAssociatedType(Type baseTy,
                                               DeclContext *DC,
                                               Identifier name) const {
  switch (stage) {
  case TypeResolutionStage::Structural:
    return DependentMemberType::get(baseTy, name);

  case TypeResolutionStage::Contextual:
    llvm_unreachable("Dependent type after archetype substitution");

  case TypeResolutionStage::Interface:
    // Handled below.
    break;
  }

  assert(stage == TypeResolutionStage::Interface);
  auto builder = getGenericSignatureBuilder();
  auto baseEquivClass =
    builder->resolveEquivalenceClass(
                                baseTy,
                                ArchetypeResolutionKind::CompleteWellFormed);
  if (!baseEquivClass)
    return ErrorType::get(baseTy);

  // Look for a nested type with the given name.
  auto nestedType = baseEquivClass->lookupNestedType(*builder, name);
  assert(nestedType);

  // If the nested type has been resolved to an associated type, use it.
  if (auto assocType = dyn_cast<AssociatedTypeDecl>(nestedType)) {
    return DependentMemberType::get(baseTy, assocType);
  }

  if (nestedType->getDeclContext()->getSelfClassDecl()) {
    // We found a member of a class from a protocol or protocol
    // extension.
    //
    // Get the superclass of the 'Self' type parameter.
    baseTy = (baseEquivClass->concreteType
              ? baseEquivClass->concreteType
              : baseEquivClass->superclass);
    assert(baseTy);
  }

  return TypeChecker::substMemberTypeWithBase(DC->getParentModule(), nestedType,
                                              baseTy);
}

bool TypeResolution::areSameType(Type type1, Type type2) const {
  if (type1->isEqual(type2))
    return true;

  switch (stage) {
  case TypeResolutionStage::Structural:
  case TypeResolutionStage::Interface:
    // If neither type has a type parameter, we're done.
    if (!type1->hasTypeParameter() && !type2->hasTypeParameter())
      return false;

    break;

  case TypeResolutionStage::Contextual:
    // Contextual types have already been uniqued, so the isEqual() result
    // above is complete.
    return false;
  }

  // If we have a generic signature, canonicalize using it.
  if (auto genericSig = getGenericSignature()) {
    // If both are type parameters, we can use a cheaper check
    // that avoids transforming the type and computing anchors.
    if (type1->isTypeParameter() &&
        type2->isTypeParameter()) {
      return genericSig->areSameTypeParameterInContext(type1, type2);
    }
    return genericSig->getCanonicalTypeInContext(type1)
      == genericSig->getCanonicalTypeInContext(type2);
  }

  // Otherwise, perform a structural check.
  assert(stage == TypeResolutionStage::Structural);

  // FIXME: We should be performing a deeper equality check here.
  // If both refer to associated types with the same name, they'll implicitly
  // be considered equivalent.
  auto depMem1 = type1->getAs<DependentMemberType>();
  if (!depMem1) return false;

  auto depMem2 = type2->getAs<DependentMemberType>();
  if (!depMem2) return false;

  if (depMem1->getName() != depMem2->getName()) return false;

  return areSameType(depMem1->getBase(), depMem2->getBase());
}

Type TypeChecker::getArraySliceType(SourceLoc loc, Type elementType) {
  ASTContext &ctx = elementType->getASTContext();
  if (!ctx.getArrayDecl()) {
    ctx.Diags.diagnose(loc, diag::sugar_type_not_found, 0);
    return ErrorType::get(ctx);
  }

  return ArraySliceType::get(elementType);
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

Type TypeResolution::resolveTypeInContext(TypeDecl *typeDecl,
                                          DeclContext *foundDC,
                                          bool isSpecialized) const {
  auto fromDC = getDeclContext();
  ASTContext &ctx = fromDC->getASTContext();

  // If we found a generic parameter, map to the archetype if there is one.
  if (auto genericParam = dyn_cast<GenericTypeParamDecl>(typeDecl)) {
    return mapTypeIntoContext(genericParam->getDeclaredInterfaceType());
  }

  if (!isSpecialized) {
    // If we are referring to a type within its own context, and we have either
    // a generic type with no generic arguments or a non-generic type, use the
    // type within the context.
    if (auto *nominalType = dyn_cast<NominalTypeDecl>(typeDecl)) {
      for (auto *parentDC = fromDC; !parentDC->isModuleScopeContext();
           parentDC = parentDC->getParent()) {
        auto *parentNominal = parentDC->getSelfNominalTypeDecl();
        if (parentNominal == nominalType)
          return mapTypeIntoContext(parentDC->getDeclaredInterfaceType());
        if (isa<ExtensionDecl>(parentDC)) {
          auto *extendedType = parentNominal;
          while (extendedType != nullptr) {
            if (extendedType == nominalType)
              return mapTypeIntoContext(
                  extendedType->getDeclaredInterfaceType());
            extendedType = extendedType->getParent()->getSelfNominalTypeDecl();
          }
        }
      }
    }

    // If we're inside an extension of a type alias, allow the type alias to be
    // referenced without generic arguments as well.
    if (auto *aliasDecl = dyn_cast<TypeAliasDecl>(typeDecl)) {
      for (auto *parentDC = fromDC; !parentDC->isModuleScopeContext();
           parentDC = parentDC->getParent()) {
        if (auto *ext = dyn_cast<ExtensionDecl>(parentDC)) {
          auto extendedType = ext->getExtendedType();
          if (auto *unboundGeneric =
                  dyn_cast<UnboundGenericType>(extendedType.getPointer())) {
            if (auto *ugAliasDecl =
                    dyn_cast<TypeAliasDecl>(unboundGeneric->getAnyGeneric())) {
              if (ugAliasDecl == aliasDecl) {
                if (getStage() == TypeResolutionStage::Structural &&
                    aliasDecl->getUnderlyingTypeRepr() != nullptr) {
                  return aliasDecl->getStructuralType();
                }
                return mapTypeIntoContext(
                    aliasDecl->getDeclaredInterfaceType());
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
                return aliasDecl->getStructuralType();
              }
              return mapTypeIntoContext(aliasDecl->getDeclaredInterfaceType());
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
        return aliasDecl->getStructuralType();
      }
      return mapTypeIntoContext(aliasDecl->getDeclaredInterfaceType());
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

    selfType = mapTypeIntoContext(foundDC->getDeclaredInterfaceType());
  } else {
    // Otherwise, we want the protocol 'Self' type for
    // substituting into alias types and associated types.
    selfType = mapTypeIntoContext(foundDC->getSelfInterfaceType());

    if (selfType->is<GenericTypeParamType>()) {
      if (typeDecl->getDeclContext()->getSelfProtocolDecl()) {
        if (isa<AssociatedTypeDecl>(typeDecl) ||
            (isa<TypeAliasDecl>(typeDecl) &&
             !cast<TypeAliasDecl>(typeDecl)->isGeneric())) {
          // FIXME: We should use this lookup method for the Interface
          // stage too, but right now that causes problems with
          // Sequence.SubSequence vs Collection.SubSequence; the former
          // is more canonical, but if we return that instead of the
          // latter, we infer the wrong associated type in some cases,
          // because we use the Sequence.SubSequence default instead of
          // the Collection.SubSequence default, even when the conforming
          // type wants to conform to Collection.
          if (getStage() == TypeResolutionStage::Structural) {
            return resolveSelfAssociatedType(selfType, foundDC,
                                             typeDecl->getName());
          } else if (auto assocType = dyn_cast<AssociatedTypeDecl>(typeDecl)) {
            typeDecl = assocType->getAssociatedTypeAnchor();
          }
        }
      }

      // FIXME: Remove this once the above FIXME is addressed.
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
      fromDC->getParentModule(), typeDecl, selfType, usesArchetypes());
}

static TypeResolutionOptions
adjustOptionsForGenericArgs(TypeResolutionOptions options) {
  options.setContext(None);
  options -= TypeResolutionFlags::SILType;

  return options;
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

/// Even if the type is not generic, it might be inside of a generic
/// context or have a free-standing 'where' clause, so we need to
/// those check requirements too.
///
/// Return true on success.
bool TypeChecker::checkContextualRequirements(GenericTypeDecl *decl,
                                              Type parentTy,
                                              SourceLoc loc,
                                              DeclContext *dc) {
  if (!parentTy || parentTy->hasUnboundGenericType() ||
      parentTy->hasTypeVariable()) {
    return true;
  }

  auto &ctx = dc->getASTContext();

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
  if (!genericSig) {
    if (loc.isValid()) {
      ctx.Diags.diagnose(loc, diag::recursive_decl_reference,
                         decl->getDescriptiveKind(), decl->getName());
      decl->diagnose(diag::kind_declared_here, DescriptiveDeclKind::Type);
    }
    return false;
  }

  const auto result =
    TypeChecker::checkGenericArguments(
        dc, loc, noteLoc,
        decl->getDeclaredInterfaceType(),
        genericSig->getGenericParams(),
        genericSig->getRequirements(),
        QueryTypeSubstitutionMap{subMap});

  switch (result) {
  case RequirementCheckResult::Failure:
  case RequirementCheckResult::SubstitutionFailure:
    return false;
  case RequirementCheckResult::Success:
    return true;
  }
  llvm_unreachable("invalid requirement check type");
}

static void diagnoseUnboundGenericType(Type ty, SourceLoc loc);

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
/// \param silParams Used to look up generic parameters in SIL mode.
/// \param comp The arguments to apply with the angle bracket range for
/// diagnostics.
///
/// \returns A BoundGenericType bound to the given arguments, or null on
/// error.
///
/// \see TypeResolution::applyUnboundGenericArguments
static Type applyGenericArguments(Type type, TypeResolution resolution,
                                  GenericParamList *silParams,
                                  ComponentIdentTypeRepr *comp) {
  const auto options = resolution.getOptions();
  auto dc = resolution.getDeclContext();
  auto loc = comp->getNameLoc().getBaseNameLoc();

  auto *generic = dyn_cast<GenericIdentTypeRepr>(comp);
  if (!generic) {
    if (auto *const unboundTy = type->getAs<UnboundGenericType>()) {
      if (!options.is(TypeResolverContext::TypeAliasDecl)) {
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

    if (TypeChecker::checkContextualRequirements(decl, parentTy, loc, dc))
      return type;

    return ErrorType::get(resolution.getASTContext());
  }

  if (type->hasError()) {
    generic->setInvalid();
    return type;
  }

  auto &ctx = dc->getASTContext();
  auto &diags = ctx.Diags;

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
    return type;
  }

  auto *unboundType = type->castTo<UnboundGenericType>();
  auto *decl = unboundType->getDecl();

  // Make sure we have the right number of generic arguments.
  // FIXME: If we have fewer arguments than we need, that might be okay, if
  // we're allowed to deduce the remaining arguments from context.
  auto genericArgs = generic->getGenericArgs();
  auto genericParams = decl->getGenericParams();
  if (genericParams->size() != genericArgs.size()) {
    if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
      diags.diagnose(loc, diag::type_parameter_count_mismatch, decl->getName(),
                     genericParams->size(), genericArgs.size(),
                     genericArgs.size() < genericParams->size())
          .highlight(generic->getAngleBrackets());
      decl->diagnose(diag::kind_declname_declared_here,
                     DescriptiveDeclKind::GenericType, decl->getName());
    }
    return ErrorType::get(ctx);
  }

  // In SIL mode, Optional<T> interprets T as a SIL type.
  if (options.contains(TypeResolutionFlags::SILType)) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
      if (nominal->isOptionalDecl()) {
        // Validate the generic argument.
        Type objectType = resolution.resolveType(genericArgs[0], silParams);
        if (objectType->hasError()) {
          return ErrorType::get(ctx);
        }

        return BoundGenericType::get(nominal, /*parent*/ Type(), objectType);
      }
    }  
  }

  // FIXME: More principled handling of circularity.
  if (!decl->getGenericSignature()) {
    diags.diagnose(loc, diag::recursive_decl_reference,
                   decl->getDescriptiveKind(), decl->getName());
    decl->diagnose(diag::kind_declared_here, DescriptiveDeclKind::Type);
    return ErrorType::get(ctx);
  }

  // Resolve the types of the generic arguments.
  auto genericResolution =
      resolution.withOptions(adjustOptionsForGenericArgs(options));

  SmallVector<Type, 2> args;
  for (auto tyR : genericArgs) {
    // Propagate failure.
    Type substTy = genericResolution.resolveType(tyR, silParams);
    if (!substTy || substTy->hasError())
      return ErrorType::get(ctx);

    args.push_back(substTy);
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

/// Apply generic arguments to the given type.
Type TypeResolution::applyUnboundGenericArguments(
    GenericTypeDecl *decl, Type parentTy, SourceLoc loc,
    ArrayRef<Type> genericArgs) const {
  assert(genericArgs.size() == decl->getGenericParams()->size() &&
         "invalid arguments, use applyGenericArguments for diagnostic emitting");

  auto genericSig = decl->getGenericSignature();
  assert(!genericSig.isNull());

  TypeSubstitutionMap subs;

  // Get the interface type for the declaration. We will be substituting
  // type parameters that appear inside this type with the provided
  // generic arguments.
  auto resultType = decl->getDeclaredInterfaceType();

  // If types involved in requirements check have either type variables
  // or unbound generics, let's skip the check here, and let the solver
  // do it when missing types are deduced.
  bool skipRequirementsCheck = false;

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

      auto parentSig = decl->getDeclContext()->getGenericSignatureOfContext();
      if (parentSig) {
        for (auto gp : parentSig->getGenericParams())
          subs[gp->getCanonicalType()->castTo<GenericTypeParamType>()] =
              genericSig->getConcreteType(gp);
      }
    } else {
      subs = parentTy->getContextSubstitutions(decl->getDeclContext());
    }

    skipRequirementsCheck |= parentTy->hasTypeVariable();
  } else if (auto genericEnv =
                 decl->getDeclContext()->getGenericEnvironmentOfContext()) {
    auto genericSig = genericEnv->getGenericSignature();
    for (auto gp : genericSig->getGenericParams()) {
      subs[gp->getCanonicalType()->castTo<GenericTypeParamType>()] =
          (usesArchetypes() ? genericEnv->mapTypeIntoContext(gp) : gp);
    }
  }

  SourceLoc noteLoc = decl->getLoc();
  if (noteLoc.isInvalid())
    noteLoc = loc;

  // Realize the types of the generic arguments and add them to the
  // substitution map.
  for (unsigned i = 0, e = genericArgs.size(); i < e; ++i) {
    auto origTy = genericSig->getInnermostGenericParams()[i];
    auto substTy = genericArgs[i];

    // Enter a substitution.
    subs[origTy->getCanonicalType()->castTo<GenericTypeParamType>()] =
      substTy;

    skipRequirementsCheck |=
        substTy->hasTypeVariable() || substTy->hasUnboundGenericType();
  }

  // Check the generic arguments against the requirements of the declaration's
  // generic signature.
  auto *module = getDeclContext()->getParentModule();

  if (!skipRequirementsCheck && getStage() > TypeResolutionStage::Structural) {
    auto result = TypeChecker::checkGenericArguments(
        getDeclContext(), loc, noteLoc,
        UnboundGenericType::get(decl, parentTy, getASTContext()),
        genericSig->getGenericParams(), genericSig->getRequirements(),
        QueryTypeSubstitutionMap{subs});

    switch (result) {
    case RequirementCheckResult::Failure:
    case RequirementCheckResult::SubstitutionFailure:
      return ErrorType::get(getASTContext());
    case RequirementCheckResult::Success:
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
                            GenericParamList *silParams,
                            ComponentIdentTypeRepr *comp) {
  // Resolve the type declaration to a specific type. How this occurs
  // depends on the current context and where the type was found.
  Type type = resolution.resolveTypeInContext(typeDecl, foundDC,
                                              isa<GenericIdentTypeRepr>(comp));

  if (type->hasError() && foundDC &&
      (isa<AssociatedTypeDecl>(typeDecl) || isa<TypeAliasDecl>(typeDecl))) {
    auto fromDC = resolution.getDeclContext();
    assert(fromDC && "No declaration context for type resolution?");
    maybeDiagnoseBadConformanceRef(fromDC, foundDC->getDeclaredInterfaceType(),
                                   comp->getNameLoc().getBaseNameLoc(),
                                   typeDecl);
  }

  return applyGenericArguments(type, resolution, silParams, comp);
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
                                ComponentIdentTypeRepr *comp,
                                NameLookupOptions lookupOptions) {
  auto dc = resolution.getDeclContext();
  ASTContext &ctx = dc->getASTContext();
  auto &diags = ctx.Diags;

  // Unqualified lookup case.
  if (parentType.isNull()) {
    if (comp->getNameRef().isSimpleName(ctx.Id_Self) &&
        !isa<GenericIdentTypeRepr>(comp)) {
      DeclContext *nominalDC = nullptr;
      NominalTypeDecl *nominal = nullptr;
      if ((nominalDC = dc->getInnermostTypeContext()) &&
          (nominal = nominalDC->getSelfNominalTypeDecl())) {
        if (isa<ClassDecl>(nominal)) {
          // Attempt to refer to 'Self' within a non-protocol nominal
          // type. Fix this by replacing 'Self' with the nominal type name.

          // Produce a Fix-It replacing 'Self' with the nominal type name.
          auto name = getDeclNameFromContext(dc, nominal);
          diags.diagnose(comp->getNameLoc(), diag::dynamic_self_invalid, name)
            .fixItReplace(comp->getNameLoc().getSourceRange(), name);

          auto type = resolution.mapTypeIntoContext(
            dc->getInnermostTypeContext()->getSelfInterfaceType());

          comp->overwriteNameRef(DeclNameRef(nominal->getName()));
          comp->setValue(nominal, nominalDC->getParent());
          return type;
        } else {
          diags.diagnose(comp->getNameLoc(), diag::cannot_find_type_in_scope,
                         comp->getNameRef());
          return ErrorType::get(ctx);
        }
      }
      // Attempt to refer to 'Self' from a free function.
      diags.diagnose(comp->getNameLoc(), diag::dynamic_self_non_method,
                     dc->getParent()->isLocalContext());

      return ErrorType::get(ctx);
    }

    // Try ignoring access control.
    NameLookupOptions relookupOptions = lookupOptions;
    relookupOptions |= NameLookupFlags::IgnoreAccessControl;
    auto inaccessibleResults =
    TypeChecker::lookupUnqualifiedType(dc, comp->getNameRef(),
                                       comp->getLoc(), relookupOptions);
    if (!inaccessibleResults.empty()) {
      // FIXME: What if the unviable candidates have different levels of access?
      auto first = cast<TypeDecl>(inaccessibleResults.front().getValueDecl());
      diags.diagnose(comp->getNameLoc(), diag::candidate_inaccessible,
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
    auto L = comp->getNameLoc();
    SourceRange R = comp->getNameLoc().getSourceRange();

    // Check if the unknown type is in the type remappings.
    auto &Remapped = ctx.RemappedTypes;
    auto TypeName = comp->getNameRef().getBaseIdentifier().str();
    auto I = Remapped.find(TypeName);
    if (I != Remapped.end()) {
      auto RemappedTy = I->second->getString();
      diags.diagnose(L, diag::cannot_find_type_in_scope_did_you_mean,
                     comp->getNameRef(), RemappedTy)
        .highlight(R)
        .fixItReplace(R, RemappedTy);

      // Replace the computed type with the suggested type.
      comp->overwriteNameRef(DeclNameRef(ctx.getIdentifier(RemappedTy)));

      // HACK: 'NSUInteger' suggests both 'UInt' and 'Int'.
      if (TypeName == ctx.getSwiftName(KnownFoundationEntity::NSUInteger)) {
        diags.diagnose(L, diag::note_remapped_type, "UInt")
          .fixItReplace(R, "UInt");
      }

      return I->second;
    }

    diags.diagnose(L, diag::cannot_find_type_in_scope, comp->getNameRef())
        .highlight(R);

    return ErrorType::get(ctx);
  }

  // Qualified lookup case.
  if (!parentType->mayHaveMembers()) {
    const auto kind = describeDeclOfType(parentType);
    diags
        .diagnose(comp->getNameLoc(), diag::invalid_member_type,
                  comp->getNameRef(), kind, parentType)
        .highlight(parentRange);
    return ErrorType::get(ctx);
  }

  // Try ignoring access control.
  NameLookupOptions relookupOptions = lookupOptions;
  relookupOptions |= NameLookupFlags::IgnoreAccessControl;
  auto inaccessibleMembers =
    TypeChecker::lookupMemberType(dc, parentType, comp->getNameRef(),
                                  relookupOptions);
  if (inaccessibleMembers) {
    // FIXME: What if the unviable candidates have different levels of access?
    const TypeDecl *first = inaccessibleMembers.front().Member;
    diags.diagnose(comp->getNameLoc(), diag::candidate_inaccessible,
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
    diags.diagnose(comp->getNameLoc(), diag::no_module_type,
                   comp->getNameRef(), moduleType->getModule()->getName());
  } else {
    LookupResult memberLookup;
    // Let's try to look any member of the parent type with the given name,
    // even if it is not a type, allowing for a more precise diagnostic.
    NLOptions memberLookupOptions = (NL_QualifiedDefault |
                                     NL_IgnoreAccessControl);
    SmallVector<ValueDecl *, 2> results;
    dc->lookupQualified(parentType, comp->getNameRef(), memberLookupOptions,
                        results);

    // Looks like this is not a member type, but simply a member of parent type.
    if (!results.empty()) {
      auto member = results[0];
      diags.diagnose(comp->getNameLoc(), diag::invalid_member_reference,
                     member->getDescriptiveKind(), member->getName(),
                     parentType)
          .highlight(parentRange);
    } else {
      const auto kind = describeDeclOfType(parentType);
      diags
          .diagnose(comp->getNameLoc(), diag::invalid_member_type,
                    comp->getNameRef(), kind, parentType)
          .highlight(parentRange);
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
                                           ComponentIdentTypeRepr *comp,
                                           DeclContext *typeDC) {
  ASTContext &ctx = resolution.getASTContext();
  auto &diags = ctx.Diags;

  auto *selfNominal = typeDC->getSelfNominalTypeDecl();
  auto declaredType = selfNominal->getDeclaredType();

  diags.diagnose(comp->getNameLoc(), diag::cannot_specialize_self);

  if (selfNominal->isGeneric() && !isa<ProtocolDecl>(selfNominal)) {
    diags.diagnose(comp->getNameLoc(), diag::specialize_explicit_type_instead,
                   declaredType)
        .fixItReplace(comp->getNameLoc().getSourceRange(),
                      declaredType.getString());
  }
}

/// Resolve the given identifier type representation as an unqualified type,
/// returning the type it references.
///
/// \returns Either the resolved type or a null type, the latter of
/// which indicates that some dependencies were unsatisfied.
static Type resolveTopLevelIdentTypeComponent(TypeResolution resolution,
                                              GenericParamList *silParams,
                                              ComponentIdentTypeRepr *comp) {
  const auto options = resolution.getOptions();
  ASTContext &ctx = resolution.getASTContext();
  auto &diags = ctx.Diags;

  // Short-circuiting.
  if (comp->isInvalid()) return ErrorType::get(ctx);

  // If the component has already been bound to a declaration, handle
  // that now.
  if (auto *typeDecl = comp->getBoundDecl()) {
    // Resolve the type declaration within this context.
    return resolveTypeDecl(typeDecl, comp->getDeclContext(), resolution,
                           silParams, comp);
  }

  // Resolve the first component, which is the only one that requires
  // unqualified name lookup.
  auto DC = resolution.getDeclContext();
  auto id = comp->getNameRef();

  // In SIL mode, we bind generic parameters here, since name lookup
  // won't find them.
  if (silParams != nullptr) {
    auto name = id.getBaseIdentifier();
    if (auto *paramDecl = silParams->lookUpGenericParam(name)) {
      comp->setValue(paramDecl, DC);

      return resolveTypeDecl(paramDecl, DC, resolution,
                             silParams, comp);
    }
  }

  NameLookupOptions lookupOptions = defaultUnqualifiedLookupOptions;
  if (options.contains(TypeResolutionFlags::AllowUsableFromInline))
    lookupOptions |= NameLookupFlags::IncludeUsableFromInline;
  auto globals = TypeChecker::lookupUnqualifiedType(DC, id, comp->getLoc(),
                                                    lookupOptions);

  // Process the names we found.
  Type current;
  TypeDecl *currentDecl = nullptr;
  DeclContext *currentDC = nullptr;
  bool isAmbiguous = false;
  for (const auto &entry : globals) {
    auto *foundDC = entry.getDeclContext();
    auto *typeDecl = cast<TypeDecl>(entry.getValueDecl());

    Type type = resolveTypeDecl(typeDecl, foundDC, resolution, silParams, comp);
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
      diags.diagnose(comp->getNameLoc(), diag::ambiguous_type_base,
                     comp->getNameRef())
        .highlight(comp->getNameLoc().getSourceRange());
      for (auto entry : globals) {
        entry.getValueDecl()->diagnose(diag::found_candidate);
      }
    }

    comp->setInvalid();
    return ErrorType::get(ctx);
  }

  // If we found a type declaration with the given name, return it now.
  if (current) {
    comp->setValue(currentDecl, currentDC);
    return current;
  }

  // 'Self' inside of a nominal type refers to that type.
  if (id.isSimpleName(ctx.Id_Self)) {
    if (auto *typeDC = DC->getInnermostTypeContext()) {
      // FIXME: The passed-in TypeRepr should get 'typechecked' as well.
      // The issue is though that ComponentIdentTypeRepr only accepts a ValueDecl
      // while the 'Self' type is more than just a reference to a TypeDecl.
      auto selfType = resolution.mapTypeIntoContext(
        typeDC->getSelfInterfaceType());

      // Check if we can reference 'Self' here, and if so, what kind of Self it is.
      auto selfTypeKind = getSelfTypeKind(DC, options);

      // We don't allow generic arguments on 'Self'.
      if (selfTypeKind != SelfTypeKind::InvalidSelf &&
          isa<GenericIdentTypeRepr>(comp)) {
        diagnoseGenericArgumentsOnSelf(resolution, comp, typeDC);
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
  return diagnoseUnknownType(resolution, nullptr, SourceRange(), comp,
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
/// \param silParams Used to look up generic parameters in SIL mode.
static Type resolveNestedIdentTypeComponent(TypeResolution resolution,
                                            GenericParamList *silParams,
                                            Type parentTy,
                                            SourceRange parentRange,
                                            ComponentIdentTypeRepr *comp) {
  const auto options = resolution.getOptions();
  auto DC = resolution.getDeclContext();
  auto &ctx = DC->getASTContext();
  auto &diags = ctx.Diags;

  auto maybeDiagnoseBadMemberType = [&](TypeDecl *member, Type memberType,
                                        AssociatedTypeDecl *inferredAssocType) {
    bool hasUnboundOpener = !!resolution.getUnboundTypeOpener();

    if (options.contains(TypeResolutionFlags::SilenceErrors)) {
      if (TypeChecker::isUnsupportedMemberTypeAccess(parentTy, member,
                                                     hasUnboundOpener)
            != TypeChecker::UnsupportedMemberTypeAccessKind::None)
        return ErrorType::get(ctx);
    }

    switch (TypeChecker::isUnsupportedMemberTypeAccess(parentTy, member,
                                                       hasUnboundOpener)) {
    case TypeChecker::UnsupportedMemberTypeAccessKind::None:
      break;

    case TypeChecker::UnsupportedMemberTypeAccessKind::NominalTypeOfUnboundGeneric:
    case TypeChecker::UnsupportedMemberTypeAccessKind::TypeAliasOfUnboundGeneric:
    case TypeChecker::UnsupportedMemberTypeAccessKind::AssociatedTypeOfUnboundGeneric:
      diagnoseUnboundGenericType(parentTy, parentRange.End);
      return ErrorType::get(ctx);

    case TypeChecker::UnsupportedMemberTypeAccessKind::TypeAliasOfExistential:
      diags.diagnose(comp->getNameLoc(), diag::typealias_outside_of_protocol,
                     comp->getNameRef());
      return ErrorType::get(ctx);

    case TypeChecker::UnsupportedMemberTypeAccessKind::AssociatedTypeOfExistential:
      diags.diagnose(comp->getNameLoc(), diag::assoc_type_outside_of_protocol,
                     comp->getNameRef());
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
      maybeDiagnoseBadConformanceRef(DC, parentTy, comp->getLoc(),
                                     inferredAssocType);
    }

    // If there are generic arguments, apply them now.
    return applyGenericArguments(memberType, resolution,
                                 silParams, comp);
  };

  // Short-circuiting.
  if (comp->isInvalid()) return ErrorType::get(ctx);

  // If the parent is a type parameter, the member is a dependent member,
  // and we skip much of the work below.
  if (parentTy->isTypeParameter()) {
    if (auto memberType = resolution.resolveDependentMemberType(
            parentTy, DC, parentRange, comp)) {
      // Hack -- if we haven't resolved this to a declaration yet, don't
      // attempt to apply generic arguments, since this will emit a
      // diagnostic, and its possible that this type will become a concrete
      // type later on.
      if (!memberType->is<DependentMemberType>() ||
          memberType->castTo<DependentMemberType>()->getAssocType()) {
        return applyGenericArguments(memberType, resolution, silParams, comp);
      }

      return memberType;
    }
  }

  // Phase 2: If a declaration has already been bound, use it.
  if (auto *typeDecl = comp->getBoundDecl()) {
    auto memberType =
      TypeChecker::substMemberTypeWithBase(DC->getParentModule(), typeDecl,
                                           parentTy);
    return maybeDiagnoseBadMemberType(typeDecl, memberType, nullptr);
  }

  // Phase 1: Find and bind the component decl.

  // Look for member types with the given name.
  NameLookupOptions lookupOptions = defaultMemberLookupOptions;
  if (options.contains(TypeResolutionFlags::AllowUsableFromInline))
    lookupOptions |= NameLookupFlags::IncludeUsableFromInline;
  LookupTypeResult memberTypes;
  if (parentTy->mayHaveMembers())
    memberTypes = TypeChecker::lookupMemberType(
        DC, parentTy, comp->getNameRef(), lookupOptions);

  // Name lookup was ambiguous. Complain.
  // FIXME: Could try to apply generic arguments first, and see whether
  // that resolves things. But do we really want that to succeed?
  if (memberTypes.size() > 1) {
    if (!options.contains(TypeResolutionFlags::SilenceErrors))
      diagnoseAmbiguousMemberType(parentTy, parentRange, comp->getNameRef(),
                                  comp->getNameLoc(), memberTypes);
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

    memberType = diagnoseUnknownType(resolution, parentTy, parentRange, comp,
                                     lookupOptions);
    member = comp->getBoundDecl();
    if (!member)
      return ErrorType::get(ctx);
  } else {
    memberType = memberTypes.back().MemberType;
    member = memberTypes.back().Member;
    inferredAssocType = memberTypes.back().InferredAssociatedType;
    comp->setValue(member, nullptr);
  }

  return maybeDiagnoseBadMemberType(member, memberType, inferredAssocType);
}

/// \param silParams Used to look up generic parameters in SIL mode.
static Type
resolveIdentTypeComponent(TypeResolution resolution,
                          GenericParamList *silParams,
                          ArrayRef<ComponentIdentTypeRepr *> components) {
  // The first component uses unqualified lookup.
  auto topLevelComp = components.front();
  auto result = resolveTopLevelIdentTypeComponent(resolution, silParams,
                                                  topLevelComp);
  if (result->hasError())
    return ErrorType::get(result->getASTContext());

  // Remaining components are resolved via iterated qualified lookups.
  SourceRange parentRange(topLevelComp->getStartLoc(),
                          topLevelComp->getEndLoc());
  for (auto nestedComp : components.drop_front()) {
    result = resolveNestedIdentTypeComponent(resolution, silParams,
                                             result, parentRange,
                                             nestedComp);
    if (result->hasError())
      return ErrorType::get(result->getASTContext());

    parentRange.End = nestedComp->getEndLoc();
  }

  // Diagnose an error if the last component's generic arguments are missing.
  auto lastComp = components.back();
  auto options = resolution.getOptions();

  if (result->is<UnboundGenericType>() &&
      !isa<GenericIdentTypeRepr>(lastComp) &&
      !resolution.getUnboundTypeOpener() &&
      !options.is(TypeResolverContext::TypeAliasDecl)) {

    if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
      diagnoseUnboundGenericType(result,
                                 lastComp->getNameLoc().getBaseNameLoc());
    }

    return ErrorType::get(result->getASTContext());
  }

  return result;
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
    // FIXME: It would be better to add a new AttributedType sugared type,
    // which would wrap the TypeAliasType or ParenType, and apply the
    // isNoEscape bit when de-sugaring.
    // <https://bugs.swift.org/browse/SR-2520>
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
    GenericParamList *genericParams;

  public:
    explicit TypeResolver(const TypeResolution &resolution,
                          GenericParamList *genericParams = nullptr)
        : resolution(resolution), genericParams(genericParams) {}

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
    SmallVector<AnyFunctionType::Param, 8> resolveASTFunctionTypeParams(
        TupleTypeRepr *inputRepr, TypeResolutionOptions options,
        bool requiresMappingOut, DifferentiabilityKind diffKind);

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
                           Optional<SILResultInfo> &errorResult);
    bool resolveSingleSILResult(TypeRepr *repr, TypeResolutionOptions options,
                                SmallVectorImpl<SILYieldInfo> &yields,
                                SmallVectorImpl<SILResultInfo> &results,
                                Optional<SILResultInfo> &errorResult);
    NeverNullType resolveIdentifierType(IdentTypeRepr *IdType,
                                        TypeResolutionOptions options);
    NeverNullType resolveSpecifierTypeRepr(SpecifierTypeRepr *repr,
                                           TypeResolutionOptions options);
    NeverNullType resolveIsolatedTypeRepr(IsolatedTypeRepr *repr,
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
    NeverNullType resolveTupleType(TupleTypeRepr *repr,
                                   TypeResolutionOptions options);
    NeverNullType resolveCompositionType(CompositionTypeRepr *repr,
                                         TypeResolutionOptions options);
    NeverNullType resolveMetatypeType(MetatypeTypeRepr *repr,
                                      TypeResolutionOptions options);
    NeverNullType resolveProtocolType(ProtocolTypeRepr *repr,
                                      TypeResolutionOptions options);
    NeverNullType resolveSILBoxType(SILBoxTypeRepr *repr,
                                    TypeResolutionOptions options);

    NeverNullType
    buildMetatypeType(MetatypeTypeRepr *repr, Type instanceType,
                      Optional<MetatypeRepresentation> storedRepr);
    NeverNullType
    buildProtocolType(ProtocolTypeRepr *repr, Type instanceType,
                      Optional<MetatypeRepresentation> storedRepr);

    NeverNullType resolveOpaqueReturnType(TypeRepr *repr, StringRef mangledName,
                                          unsigned ordinal,
                                          TypeResolutionOptions options);
  };
} // end anonymous namespace

Type TypeResolution::resolveType(TypeRepr *TyR,
                                 GenericParamList *silParams) const {
  auto &ctx = getASTContext();
  auto Ty =
      evaluateOrDefault(ctx.evaluator,
                        ResolveTypeRequest{this, TyR, silParams}, Type());
  if (!Ty)
    return ErrorType::get(ctx);
  return Ty;
}

Type ResolveTypeRequest::evaluate(Evaluator &evaluator,
                                  const TypeResolution *resolution,
                                  TypeRepr *TyR,
                                  GenericParamList *silParams) const {
  const auto options = resolution->getOptions();
  auto &ctx = resolution->getASTContext();
  auto result =
      TypeResolver(*resolution, silParams)
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

  // Diagnose an attempt to use a placeholder at the top level.
  if (isa<PlaceholderTypeRepr>(TyR) &&
      resolution->getOptions().contains(TypeResolutionFlags::Direct)) {
    if (!resolution->getOptions().contains(TypeResolutionFlags::SilenceErrors))
      ctx.Diags.diagnose(loc, diag::top_level_placeholder_type);

    TyR->setInvalid();
    return ErrorType::get(ctx);
  }

  return result;
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
  if (!isa<SpecifierTypeRepr>(repr) && !isa<TupleTypeRepr>(repr) &&
      !isa<AttributedTypeRepr>(repr) && !isa<FunctionTypeRepr>(repr) &&
      !isa<IdentTypeRepr>(repr) &&
      !isa<ImplicitlyUnwrappedOptionalTypeRepr>(repr)) {
    options.setContext(None);
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
  case TypeReprKind::InOut:
  case TypeReprKind::Shared:
  case TypeReprKind::Owned:
    return resolveSpecifierTypeRepr(cast<SpecifierTypeRepr>(repr), options);

  case TypeReprKind::Isolated:
    return resolveIsolatedTypeRepr(cast<IsolatedTypeRepr>(repr), options);

  case TypeReprKind::SimpleIdent:
  case TypeReprKind::GenericIdent:
  case TypeReprKind::CompoundIdent:
    return resolveIdentifierType(cast<IdentTypeRepr>(repr), options);

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
    return resolveSILBoxType(cast<SILBoxTypeRepr>(repr), options);

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

  case TypeReprKind::Tuple:
    return resolveTupleType(cast<TupleTypeRepr>(repr), options);

  case TypeReprKind::Composition:
    return resolveCompositionType(cast<CompositionTypeRepr>(repr), options);

  case TypeReprKind::Metatype:
    return resolveMetatypeType(cast<MetatypeTypeRepr>(repr), options);

  case TypeReprKind::Protocol:
    return resolveProtocolType(cast<ProtocolTypeRepr>(repr), options);
      
  case TypeReprKind::OpaqueReturn: {
    // Only valid as the return type of a function, which should be handled
    // during function decl type checking.
    auto opaqueRepr = cast<OpaqueReturnTypeRepr>(repr);
    if (!(options & TypeResolutionFlags::SilenceErrors)) {
      diagnose(opaqueRepr->getOpaqueLoc(),
               diag::unsupported_opaque_type);
    }
    
    // Try to resolve the constraint upper bound type as a placeholder.
    options |= TypeResolutionFlags::SilenceErrors;
    auto constraintType = resolveType(opaqueRepr->getConstraint(),
                                      options);

    return !constraintType->hasError() ? ErrorType::get(constraintType)
                                       : ErrorType::get(getASTContext());
  }

  case TypeReprKind::Placeholder: {
    auto &ctx = getASTContext();
    // Fill in the placeholder if there's an appropriate handler.
    if (const auto handlerFn = resolution.getPlaceholderHandler())
      if (const auto ty = handlerFn(ctx, cast<PlaceholderTypeRepr>(repr)))
        return ty;

    // Complain if we're allowed to and bail out with an error.
    if (!options.contains(TypeResolutionFlags::SilenceErrors))
      ctx.Diags.diagnose(repr->getLoc(),
                         diag::placeholder_type_not_allowed);

    return ErrorType::get(resolution.getASTContext());
  }

  case TypeReprKind::Fixed:
    return cast<FixedTypeRepr>(repr)->getType();
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

  // Resolve global actor.
  CustomAttr *globalActorAttr = nullptr;
  Type globalActor;
  if (isa<FunctionTypeRepr>(repr)) {
    auto foundGlobalActor = checkGlobalActorAttributes(
        repr->getLoc(), getDeclContext(),
        std::vector<CustomAttr *>(
          attrs.getCustomAttrs().begin(), attrs.getCustomAttrs().end()));
    if (foundGlobalActor) {
      globalActorAttr = foundGlobalActor->first;
      globalActor = resolveType(globalActorAttr->getTypeRepr(), options);
      if (globalActor->hasError())
        globalActor = Type();
    }
  }

  // Diagnose custom attributes that haven't been processed yet.
  for (auto customAttr : attrs.getCustomAttrs()) {
    // If this was the global actor we matched, ignore it.
    if (globalActorAttr == customAttr)
      continue;

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
        TypeRepr *base;
        if (auto metatypeRepr = dyn_cast<MetatypeTypeRepr>(repr)) {
          base = metatypeRepr->getBase();
        } else if (auto protocolRepr = dyn_cast<ProtocolTypeRepr>(repr)) {
          base = protocolRepr->getBase();
        } else {
          base = nullptr;
        }

        if (base) {
          Optional<MetatypeRepresentation> storedRepr;
          // The instance type is not a SIL type.
          auto instanceOptions = options;
          instanceOptions.setContext(None);
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
            llvm::StringSwitch<Optional<SILFunctionType::Representation>>(
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
                .Default(None);
        if (!parsedRep) {
          diagnoseInvalid(repr, attrs.getLoc(TAK_convention),
                          diag::unsupported_sil_convention,
                          attrs.getConventionName());
          rep = SILFunctionType::Representation::Thin;
        } else {
          rep = *parsedRep;
          parsedClangFunctionType = tryParseClangType(
              attrs.ConventionArguments.getValue(), shouldStoreClangType(rep));
        }

        if (rep == SILFunctionType::Representation::WitnessMethod) {
          auto protocolName =
            attrs.ConventionArguments.getValue().WitnessMethodProtocol;
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
            llvm::StringSwitch<Optional<FunctionType::Representation>>(
                attrs.getConventionName())
                .Case("swift", FunctionType::Representation::Swift)
                .Case("block", FunctionType::Representation::Block)
                .Case("thin", FunctionType::Representation::Thin)
                .Case("c", FunctionType::Representation::CFunctionPointer)
                .Default(None);
        if (!parsedRep) {
          diagnoseInvalid(repr, attrs.getLoc(TAK_convention),
                          diag::unsupported_convention,
                          attrs.getConventionName());
          rep = FunctionType::Representation::Swift;
        } else {
          rep = *parsedRep;

          parsedClangFunctionType = tryParseClangType(
              attrs.ConventionArguments.getValue(), shouldStoreClangType(rep));
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

  auto instanceOptions = options;
  instanceOptions.setContext(None);

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
    attrs.ConventionArguments = None;
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

  // In SIL, handle @opened (n), which creates an existential archetype.
  if (attrs.has(TAK_opened)) {
    if (!ty->isExistentialType()) {
      diagnoseInvalid(repr, attrs.getLoc(TAK_opened), diag::opened_non_protocol,
                      ty);
    } else {
      ty = OpenedArchetypeType::get(ty, attrs.OpenedID);
    }
    attrs.clearAttribute(TAK_opened);
  }

  // In SIL files *only*, permit @weak and @unowned to apply directly to types.
  if (attrs.hasOwnership()) {
    if (auto SF = getDeclContext()->getParentSourceFile()) {
      if (SF->Kind == SourceFileKind::SIL) {
        if (((attrs.has(TAK_sil_weak) || attrs.has(TAK_sil_unmanaged)) &&
             ty->getOptionalObjectType()) ||
            (!attrs.has(TAK_sil_weak) && ty->hasReferenceSemantics())) {
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
                                           bool requiresMappingOut,
                                           DifferentiabilityKind diffKind) {
  SmallVector<AnyFunctionType::Param, 8> elements;
  elements.reserve(inputRepr->getNumElements());

  auto elementOptions = options.withoutContext(true);
  elementOptions.setContext(TypeResolverContext::FunctionInput);
  for (unsigned i = 0, end = inputRepr->getNumElements(); i != end; ++i) {
    auto *eltTypeRepr = inputRepr->getElementType(i);

    // If the element is a variadic parameter, resolve the parameter type as if
    // it were in non-parameter position, since we want functions to be
    // @escaping in this case.
    auto thisElementOptions = elementOptions;
    bool variadic = false;
    if (inputRepr->hasEllipsis() &&
        elements.size() == inputRepr->getEllipsisIndex()) {
      thisElementOptions = elementOptions.withoutContext();
      thisElementOptions.setContext(TypeResolverContext::VariadicFunctionInput);
      variadic = true;
    }

    auto ty = resolveType(eltTypeRepr, thisElementOptions);
    if (ty->hasError()) {
      elements.emplace_back(ErrorType::get(getASTContext()));
      continue;
    }

    // Parameters of polymorphic functions speak in terms of interface types.
    if (requiresMappingOut) {
      ty = ty->mapTypeOutOfContext();
    }

    bool autoclosure = false;
    if (auto *ATR = dyn_cast<AttributedTypeRepr>(eltTypeRepr))
      autoclosure = ATR->getAttrs().has(TAK_autoclosure);

    ValueOwnership ownership = ValueOwnership::Default;

    auto *nestedRepr = eltTypeRepr;

    // Look through parens here; other than parens, specifiers
    // must appear at the top level of a parameter type.
    while (auto *tupleRepr = dyn_cast<TupleTypeRepr>(nestedRepr)) {
      if (!tupleRepr->isParenType())
        break;
      nestedRepr = tupleRepr->getElementType(0);
    }

    bool isolated = false;
    while (true) {
      if (auto *specifierRepr = dyn_cast<SpecifierTypeRepr>(nestedRepr)) {
        switch (specifierRepr->getKind()) {
        case TypeReprKind::Shared:
          ownership = ValueOwnership::Shared;
          nestedRepr = specifierRepr->getBase();
          continue;
        case TypeReprKind::InOut:
          ownership = ValueOwnership::InOut;
          nestedRepr = specifierRepr->getBase();
            continue;
        case TypeReprKind::Owned:
          ownership = ValueOwnership::Owned;
          nestedRepr = specifierRepr->getBase();
          continue;
        case TypeReprKind::Isolated:
          isolated = true;
          nestedRepr = specifierRepr->getBase();
          continue;
        default:
          break;
        }
      }
      break;
    }

    bool noDerivative = false;
    if (auto *attrTypeRepr = dyn_cast<AttributedTypeRepr>(nestedRepr)) {
      if (attrTypeRepr->getAttrs().has(TAK_noDerivative)) {
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
    }

    auto paramFlags = ParameterTypeFlags::fromParameterType(
        ty, variadic, autoclosure, /*isNonEphemeral*/ false, ownership,
        isolated, noDerivative);
    elements.emplace_back(ty, Identifier(), paramFlags,
                          inputRepr->getElementName(i));
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
  ASTBuilder builder(getASTContext());
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

  Optional<llvm::SaveAndRestore<GenericParamList *>> saveGenericParams;

  if (auto *genericParams = repr->getGenericParams())
    saveGenericParams.emplace(this->genericParams, genericParams);

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

  TypeResolutionOptions options = None;
  options |= parentOptions.withoutContext().getFlags();
  auto params = resolveASTFunctionTypeParams(
      repr->getArgsTypeRepr(), options,
      repr->getGenericEnvironment() != nullptr, diffKind);

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
  if (auto genericEnv = repr->getGenericEnvironment()) {
    outputTy = outputTy->mapTypeOutOfContext();
    return GenericFunctionType::get(genericEnv->getGenericSignature(),
                                    params, outputTy, extInfo);
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
                                              TypeResolutionOptions options) {
  // Resolve the field types.
  SmallVector<SILField, 4> fields;
  {
    // Resolve field types using the box type's generic environment, if it
    // has one. (TODO: Field types should never refer to generic parameters
    // outside the box's own environment; we should really validate that...)
    TypeResolution fieldResolution{resolution};

    auto *genericEnv = repr->getGenericEnvironment();
    auto *genericParams = repr->getGenericParams();

    if (genericParams) {
      fieldResolution = TypeResolution::forContextual(
          getDeclContext(), genericEnv, options,
          resolution.getUnboundTypeOpener(),
          resolution.getPlaceholderHandler());
    }

    TypeResolver fieldResolver{fieldResolution,
                               genericParams};
    for (auto &fieldRepr : repr->getFields()) {
      auto fieldTy = fieldResolver.resolveType(fieldRepr.getFieldType(), options);
      fields.push_back({fieldTy->getCanonicalType(), fieldRepr.isMutable()});
    }
  }

  // Substitute out parsed context types into interface types.
  CanGenericSignature genericSig;
  if (auto *genericEnv = repr->getGenericEnvironment()) {
    genericSig = genericEnv->getGenericSignature().getCanonicalSignature();

    for (auto &field : fields) {
      auto transTy = field.getLoweredType()->mapTypeOutOfContext();
      field = {transTy->getCanonicalType(), field.isMutable()};
    }
  }
  
  // Resolve the generic arguments.
  // Start by building a TypeSubstitutionMap.
  SubstitutionMap subMap;
  if (genericSig) {
    TypeSubstitutionMap genericArgMap;

    auto params = genericSig->getGenericParams();
    if (repr->getGenericArguments().size()
          != genericSig->getGenericParams().size()) {
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

  auto layout = SILLayout::get(getASTContext(), genericSig, fields);
  return SILBoxType::get(getASTContext(), layout, subMap);
}

NeverNullType TypeResolver::resolveSILFunctionType(
    FunctionTypeRepr *repr, TypeResolutionOptions options,
    SILCoroutineKind coroutineKind,
    SILFunctionType::ExtInfoBuilder extInfoBuilder, ParameterConvention callee,
    TypeRepr *witnessMethodProtocol) {
  options.setContext(None);

  bool hasError = false;

  // Resolve parameter and result types using the function's generic
  // environment.
  SmallVector<SILParameterInfo, 4> params;
  SmallVector<SILYieldInfo, 4> yields;
  SmallVector<SILResultInfo, 4> results;
  Optional<SILResultInfo> errorResult;

  // Resolve generic params in the pattern environment, if present, or
  // else the function's generic environment, if it has one.
  GenericParamList *genericParams = repr->getGenericParams();
  if (genericParams == nullptr)
    genericParams = this->genericParams;
  GenericParamList *componentGenericParams = repr->getPatternGenericParams();
  if (componentGenericParams == nullptr)
    componentGenericParams = genericParams;

  GenericEnvironment *genericEnv = repr->getGenericEnvironment();
  GenericEnvironment *componentTypeEnv =
    repr->getPatternGenericEnvironment()
      ? repr->getPatternGenericEnvironment()
      : genericEnv;

  {
    TypeResolution functionResolution{resolution};
    if (componentTypeEnv) {
      functionResolution = TypeResolution::forContextual(
          getDeclContext(), componentTypeEnv, options,
          resolution.getUnboundTypeOpener(),
          resolution.getPlaceholderHandler());
    }
    
    auto argsTuple = repr->getArgsTypeRepr();
    // SIL functions cannot be variadic.
    if (argsTuple->hasEllipsis()) {
      diagnose(argsTuple->getEllipsisLoc(), diag::sil_function_ellipsis);
    }
    // SIL functions cannot have parameter names.
    for (auto &element : argsTuple->getElements()) {
      if (element.UnderscoreLoc.isValid())
        diagnose(element.UnderscoreLoc, diag::sil_function_input_label);
    }

    TypeResolver silResolver{functionResolution, componentGenericParams};
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

  auto resolveSubstitutions = [&](GenericEnvironment *env,
                                  ArrayRef<TypeRepr*> args,
                                  TypeResolver &&parameterResolver) {
    auto sig = env->getGenericSignature().getCanonicalSignature();
    TypeSubstitutionMap subsMap;
    auto params = sig->getGenericParams();
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
    if (genericEnv) {
      auto resolveSILParameters =
          TypeResolution::forContextual(getDeclContext(), genericEnv, options,
                                        resolution.getUnboundTypeOpener(),
                                        resolution.getPlaceholderHandler());
      patternSubs = resolveSubstitutions(repr->getPatternGenericEnvironment(),
                                         repr->getPatternSubstitutions(),
                                         TypeResolver{resolveSILParameters,
                                                      genericParams});
    } else {
      patternSubs = resolveSubstitutions(repr->getPatternGenericEnvironment(),
                                         repr->getPatternSubstitutions(),
                                         TypeResolver{resolution,
                                                      genericParams});
    }
  }

  // Resolve invocation substitutions if we have them.
  SubstitutionMap invocationSubs;
  if (!repr->getInvocationSubstitutions().empty()) {
    invocationSubs = resolveSubstitutions(repr->getGenericEnvironment(),
                                          repr->getInvocationSubstitutions(),
                                          TypeResolver{resolution,
                                                       genericParams});
  }

  if (hasError) {
    return ErrorType::get(getASTContext());
  }

  CanGenericSignature genericSig =
    genericEnv ? genericEnv->getGenericSignature().getCanonicalSignature()
               : CanGenericSignature();


  // FIXME: Remap the parsed context types to interface types.
  SmallVector<SILParameterInfo, 4> interfaceParams;
  SmallVector<SILYieldInfo, 4> interfaceYields;
  SmallVector<SILResultInfo, 4> interfaceResults;
  Optional<SILResultInfo> interfaceErrorResult;
  if (componentTypeEnv) {
    for (auto &param : params) {
      auto transParamType = param.getInterfaceType()->mapTypeOutOfContext()
          ->getCanonicalType();
      interfaceParams.push_back(param.getWithInterfaceType(transParamType));
    }
    for (auto &yield : yields) {
      auto transYieldType = yield.getInterfaceType()->mapTypeOutOfContext()
          ->getCanonicalType();
      interfaceYields.push_back(yield.getWithInterfaceType(transYieldType));
    }
    for (auto &result : results) {
      auto transResultType = result.getInterfaceType()->mapTypeOutOfContext()
          ->getCanonicalType();
      interfaceResults.push_back(result.getWithInterfaceType(transResultType));
    }

    if (errorResult) {
      auto transErrorResultType = errorResult->getInterfaceType()
          ->mapTypeOutOfContext()
          ->getCanonicalType();
      interfaceErrorResult =
        errorResult->getWithInterfaceType(transErrorResultType);
    }
  } else {
    interfaceParams = params;
    interfaceYields = yields;
    interfaceResults = results;
    interfaceErrorResult = errorResult;
  }

  SubstitutionMap interfacePatternSubs = patternSubs;
  if (interfacePatternSubs && repr->getGenericEnvironment()) {
    interfacePatternSubs =
      interfacePatternSubs.mapReplacementTypesOutOfContext();
  }

  ProtocolConformanceRef witnessMethodConformance;
  if (witnessMethodProtocol) {
    auto resolved = resolveType(witnessMethodProtocol, options);
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
    auto result = results.empty() ? Optional<SILResultInfo>() : results[0];
    clangFnType = getASTContext().getCanonicalClangFunctionType(
        interfaceParams, result, representation);
    extInfoBuilder = extInfoBuilder.withClangFunctionType(clangFnType);
  }

  return SILFunctionType::get(genericSig, extInfoBuilder.build(), coroutineKind,
                              callee, interfaceParams, interfaceYields,
                              interfaceResults, interfaceErrorResult,
                              interfacePatternSubs, invocationSubs,
                              getASTContext(), witnessMethodConformance);
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
             ParameterConvention::Indirect_In_Constant);
    checkFor(TypeAttrKind::TAK_inout, ParameterConvention::Indirect_Inout);
    checkFor(TypeAttrKind::TAK_inout_aliasable,
             ParameterConvention::Indirect_InoutAliasable);
    checkFor(TypeAttrKind::TAK_owned, ParameterConvention::Direct_Owned);
    checkFor(TypeAttrKind::TAK_guaranteed,
             ParameterConvention::Direct_Guaranteed);
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

bool TypeResolver::resolveSingleSILResult(TypeRepr *repr,
                                          TypeResolutionOptions options,
                                          SmallVectorImpl<SILYieldInfo> &yields,
                              SmallVectorImpl<SILResultInfo> &ordinaryResults,
                                       Optional<SILResultInfo> &errorResult) {
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
  if (errorResult.hasValue()) {
    diagnose(repr->getStartLoc(),
             diag::sil_function_multiple_error_results);
    return true;
  }

  errorResult = resolvedResult;
  return false;
}

bool TypeResolver::resolveSILResults(TypeRepr *repr,
                                     TypeResolutionOptions options,
                                SmallVectorImpl<SILYieldInfo> &yields,
                                SmallVectorImpl<SILResultInfo> &ordinaryResults,
                                Optional<SILResultInfo> &errorResult) {
  if (auto tuple = dyn_cast<TupleTypeRepr>(repr)) {
    bool hadError = false;
    for (auto &element : tuple->getElements()) {
      if (element.UnderscoreLoc.isValid())
        diagnose(element.UnderscoreLoc, diag::sil_function_output_label);
    }
    for (auto elt : tuple->getElements()) {
      if (resolveSingleSILResult(elt.Type, options,
                                 yields, ordinaryResults, errorResult))
        hadError = true;
    }
    return hadError;
  }

  return resolveSingleSILResult(repr, options,
                                yields, ordinaryResults, errorResult);
}

NeverNullType
TypeResolver::resolveIdentifierType(IdentTypeRepr *IdType,
                                    TypeResolutionOptions options) {
  auto ComponentRange = IdType->getComponentRange();
  auto Components = llvm::makeArrayRef(ComponentRange.begin(),
                                       ComponentRange.end());
  Type result = resolveIdentTypeComponent(resolution.withOptions(options),
                                          genericParams, Components);
  if (!result || result->hasError()) {
    return ErrorType::get(getASTContext());
  }

  if (auto moduleTy = result->getAs<ModuleType>()) {
    // Allow module types only if flag is specified.
    if (options.contains(TypeResolutionFlags::AllowModule))
      return moduleTy;
    // Otherwise, emit an error.
    if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
      auto moduleName = moduleTy->getModule()->getName();
      diagnose(Components.back()->getNameLoc(),
               diag::cannot_find_type_in_scope, DeclNameRef(moduleName));
      diagnose(Components.back()->getNameLoc(),
               diag::note_module_as_type, moduleName);
    }
    Components.back()->setInvalid();
    return ErrorType::get(getASTContext());
  }

  // Hack to apply context-specific @escaping to a typealias with an underlying
  // function type.
  if (result->is<FunctionType>())
    result = applyNonEscapingIfNecessary(result, options);

  return result;
}

NeverNullType
TypeResolver::resolveSpecifierTypeRepr(SpecifierTypeRepr *repr,
                                       TypeResolutionOptions options) {
  // inout is only valid for (non-Subscript and non-EnumCaseDecl)
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
    switch (repr->getKind()) {
    case TypeReprKind::InOut:
      name = "inout";
      break;
    case TypeReprKind::Shared:
      name = "__shared";
      break;
    case TypeReprKind::Owned:
      name = "__owned";
      break;
    default:
      llvm_unreachable("unknown SpecifierTypeRepr kind");
    }
    diagnoseInvalid(repr, repr->getSpecifierLoc(), diagID, name);
    return ErrorType::get(getASTContext());
  }

  if (isa<InOutTypeRepr>(repr)
      && !isa<ImplicitlyUnwrappedOptionalTypeRepr>(repr->getBase())) {
    // Anything within an inout isn't a parameter anymore.
    options.setContext(TypeResolverContext::InoutFunctionInput);
  }

  return resolveType(repr->getBase(), options);
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

NeverNullType TypeResolver::resolveArrayType(ArrayTypeRepr *repr,
                                             TypeResolutionOptions options) {
  auto baseTy = resolveType(repr->getBase(), options.withoutContext());
  if (baseTy->hasError()) {
    return ErrorType::get(getASTContext());
  }

  auto sliceTy =
    TypeChecker::getArraySliceType(repr->getBrackets().Start, baseTy);
  if (sliceTy->hasError())
    return ErrorType::get(getASTContext());

  return sliceTy;
}

NeverNullType
TypeResolver::resolveDictionaryType(DictionaryTypeRepr *repr,
                                    TypeResolutionOptions options) {
  options = adjustOptionsForGenericArgs(options);

  auto keyTy = resolveType(repr->getKey(), options.withoutContext());
  if (keyTy->hasError()) {
    return ErrorType::get(getASTContext());
  }

  auto valueTy = resolveType(repr->getValue(), options.withoutContext());
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

  auto baseTy = resolveType(repr->getBase(), elementOptions);
  if (baseTy->hasError()) {
    return ErrorType::get(getASTContext());
  }

  auto optionalTy = TypeChecker::getOptionalType(repr->getQuestionLoc(),
                                                 baseTy);
  if (optionalTy->hasError()) {
    return ErrorType::get(getASTContext());
  }

  return optionalTy;
}

NeverNullType TypeResolver::resolveImplicitlyUnwrappedOptionalType(
    ImplicitlyUnwrappedOptionalTypeRepr *repr, TypeResolutionOptions options,
    bool isDirect) {
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
  case TypeResolverContext::VariadicFunctionInput:
  case TypeResolverContext::ForEachStmt:
  case TypeResolverContext::ExtensionBinding:
  case TypeResolverContext::ExplicitCastExpr:
  case TypeResolverContext::SubscriptDecl:
  case TypeResolverContext::EnumElementDecl:
  case TypeResolverContext::EnumPatternPayload:
  case TypeResolverContext::TypeAliasDecl:
  case TypeResolverContext::GenericTypeAliasDecl:
  case TypeResolverContext::GenericRequirement:
  case TypeResolverContext::ImmediateOptionalTypeArgument:
  case TypeResolverContext::InExpression:
  case TypeResolverContext::EditorPlaceholderExpr:
  case TypeResolverContext::AbstractFunctionDecl:
  case TypeResolverContext::ClosureExpr:
    doDiag = true;
    break;
  }

  if (doDiag && !options.contains(TypeResolutionFlags::SilenceErrors)) {
    // Prior to Swift 5, we allow 'as T!' and turn it into a disjunction.
    if (getASTContext().isSwiftVersionAtLeast(5)) {
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
    return ErrorType::get(getASTContext());
  }

  auto uncheckedOptionalTy =
      TypeChecker::getOptionalType(repr->getExclamationLoc(), baseTy);
  if (uncheckedOptionalTy->hasError()) {
    return ErrorType::get(getASTContext());
  }

  return uncheckedOptionalTy;
}

NeverNullType TypeResolver::resolveTupleType(TupleTypeRepr *repr,
                                             TypeResolutionOptions options) {
  SmallVector<TupleTypeElt, 8> elements;
  elements.reserve(repr->getNumElements());

  llvm::SmallDenseSet<Identifier> seenEltNames;
  seenEltNames.reserve(repr->getNumElements());

  auto elementOptions = options;
  if (!repr->isParenType()) {
    elementOptions = elementOptions.withoutContext(true);
  }

  // Variadic tuples are not permitted.
  bool complained = false;
  if (repr->hasEllipsis()) {
    diagnose(repr->getEllipsisLoc(), diag::tuple_ellipsis);
    repr->removeEllipsis();
    complained = true;
  }

  bool hadError = false;
  bool foundDupLabel = false;
  for (unsigned i = 0, end = repr->getNumElements(); i != end; ++i) {
    auto *tyR = repr->getElementType(i);

    auto ty = resolveType(tyR, elementOptions);
    if (ty->hasError())
      hadError = true;

    auto eltName = repr->getElementName(i);

    elements.emplace_back(ty, eltName, ParameterTypeFlags());

    if (eltName.empty())
      continue;

    if (seenEltNames.count(eltName) == 1) {
      foundDupLabel = true;
    }

    seenEltNames.insert(eltName);
  }

  if (hadError)
    return ErrorType::get(getASTContext());

  // Single-element labeled tuples are not permitted outside of declarations
  // or SIL, either.
  if (elements.size() == 1 && elements[0].hasName()
      && !(options & TypeResolutionFlags::SILType)) {
    if (!complained) {
      diagnose(repr->getElementNameLoc(0), diag::tuple_single_element)
        .fixItRemoveChars(repr->getElementNameLoc(0),
                          repr->getElementType(0)->getStartLoc());
    }

    elements[0] = TupleTypeElt(elements[0].getType());
  }

  // Tuples with duplicate element labels are not permitted
  if (foundDupLabel) {
    diagnose(repr->getLoc(), diag::tuple_duplicate_label);
  }

  return TupleType::get(elements, getASTContext());
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

  for (auto tyR : repr->getTypes()) {
    auto ty = resolveType(tyR, options.withoutContext());
    if (ty->hasError()) return ty;

    auto nominalDecl = ty->getAnyNominal();
    if (nominalDecl && isa<ClassDecl>(nominalDecl)) {
      if (checkSuperclass(tyR->getStartLoc(), ty))
        continue;

      Members.push_back(ty);
      continue;
    }

    if (ty->isExistentialType()) {
      auto layout = ty->getExistentialLayout();
      if (auto superclass = layout.explicitSuperclass)
        if (checkSuperclass(tyR->getStartLoc(), superclass))
          continue;
      if (!layout.getProtocols().empty())
        HasProtocol = true;

      Members.push_back(ty);
      continue;
    }

    diagnose(tyR->getStartLoc(),
             diag::invalid_protocol_composition_member,
             ty);
  }

  // Avoid confusing diagnostics ('MyClass' not convertible to 'MyClass',
  // etc) by collapsing a composition consisting of a single class down
  // to the class itself.
  if (SuperclassType && !HasProtocol)
    return SuperclassType;

  // In user-written types, AnyObject constraints always refer to the
  // AnyObject type in the standard library.
  return ProtocolCompositionType::get(getASTContext(), Members,
                                      /*HasExplicitAnyObject=*/false);
}

NeverNullType TypeResolver::resolveMetatypeType(MetatypeTypeRepr *repr,
                                                TypeResolutionOptions options) {
  // The instance type of a metatype is always abstract, not SIL-lowered.
  auto ty = resolveType(repr->getBase(), options.withoutContext());
  if (ty->hasError()) {
    return ErrorType::get(getASTContext());
  }

  Optional<MetatypeRepresentation> storedRepr;
  
  // In SIL mode, a metatype must have a @thin, @thick, or
  // @objc_metatype attribute, so metatypes should have been lowered
  // in resolveAttributedType.
  if (options & TypeResolutionFlags::SILType) {
    diagnose(repr->getStartLoc(), diag::sil_metatype_without_repr);
    storedRepr = MetatypeRepresentation::Thick;
  }

  return buildMetatypeType(repr, ty, storedRepr);
}

NeverNullType
TypeResolver::buildMetatypeType(MetatypeTypeRepr *repr, Type instanceType,
                                Optional<MetatypeRepresentation> storedRepr) {
  if (instanceType->isAnyExistentialType()) {
    // TODO: diagnose invalid representations?
    return ExistentialMetatypeType::get(instanceType, storedRepr);
  } else {
    return MetatypeType::get(instanceType, storedRepr);
  }
}

NeverNullType TypeResolver::resolveProtocolType(ProtocolTypeRepr *repr,
                                                TypeResolutionOptions options) {
  // The instance type of a metatype is always abstract, not SIL-lowered.
  auto ty = resolveType(repr->getBase(), options.withoutContext());
  if (ty->hasError()) {
    return ErrorType::get(getASTContext());
  }

  Optional<MetatypeRepresentation> storedRepr;
  
  // In SIL mode, a metatype must have a @thin, @thick, or
  // @objc_metatype attribute, so metatypes should have been lowered
  // in resolveAttributedType.
  if (options & TypeResolutionFlags::SILType) {
    diagnose(repr->getStartLoc(), diag::sil_metatype_without_repr);
    storedRepr = MetatypeRepresentation::Thick;
  }

  return buildProtocolType(repr, ty, storedRepr);
}

NeverNullType
TypeResolver::buildProtocolType(ProtocolTypeRepr *repr, Type instanceType,
                                Optional<MetatypeRepresentation> storedRepr) {
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
    if (aliasDecl->getGenericParams()) {
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

    subs = baseTy->getContextSubstitutionMap(module, member->getDeclContext());
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

class UnsupportedProtocolVisitor
  : public TypeReprVisitor<UnsupportedProtocolVisitor>, public ASTWalker
{
  ASTContext &Ctx;
  bool checkStatements;
  bool hitTopStmt;
    
public:
  UnsupportedProtocolVisitor(ASTContext &ctx, bool checkStatements)
    : Ctx(ctx), checkStatements(checkStatements), hitTopStmt(false) { }

  bool walkToTypeReprPre(TypeRepr *T) override {
    if (T->isInvalid())
      return false;
    if (auto compound = dyn_cast<CompoundIdentTypeRepr>(T)) {
      // Only visit the last component to check, because nested typealiases in
      // existentials are okay.
      visit(compound->getComponentRange().back());
      return false;
    }
    // Arbitrary protocol constraints are OK on opaque types.
    if (isa<OpaqueReturnTypeRepr>(T))
      return false;
    
    visit(T);
    return true;
  }

  std::pair<bool, Stmt*> walkToStmtPre(Stmt *S) override {
    if (checkStatements && !hitTopStmt) {
      hitTopStmt = true;
      return { true, S };
    }

    return { false, S };
  }

  bool walkToDeclPre(Decl *D) override {
    return !checkStatements;
  }

  void visitTypeRepr(TypeRepr *T) {
    // Do nothing for all TypeReprs except the ones listed below.
  }

  void visitIdentTypeRepr(IdentTypeRepr *T) {
    if (T->isInvalid())
      return;
    
    auto comp = T->getComponentRange().back();
    if (auto *proto = dyn_cast_or_null<ProtocolDecl>(comp->getBoundDecl())) {
      if (!proto->existentialTypeSupported()) {
        Ctx.Diags.diagnose(comp->getNameLoc(),
                           diag::unsupported_existential_type,
                           proto->getName());
        T->setInvalid();
      }
    } else if (auto *alias = dyn_cast_or_null<TypeAliasDecl>(comp->getBoundDecl())) {
      auto type = Type(alias->getDeclaredInterfaceType()->getDesugaredType());
      type.findIf([&](Type type) -> bool {
        if (T->isInvalid())
          return false;
        if (type->isExistentialType()) {
          auto layout = type->getExistentialLayout();
          for (auto *proto : layout.getProtocols()) {
            auto *protoDecl = proto->getDecl();
            if (protoDecl->existentialTypeSupported())
              continue;
            
            Ctx.Diags.diagnose(comp->getNameLoc(),
                               diag::unsupported_existential_type,
                               protoDecl->getName());
            T->setInvalid();
          }
        }
        return false;
      });
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

void TypeChecker::checkUnsupportedProtocolType(Decl *decl) {
  if (!decl || decl->isInvalid())
    return;

  auto &ctx = decl->getASTContext();
  if (auto *protocolDecl = dyn_cast<ProtocolDecl>(decl)) {
    checkUnsupportedProtocolType(ctx, protocolDecl->getTrailingWhereClause());
  } else if (auto *genericDecl = dyn_cast<GenericTypeDecl>(decl)) {
    checkUnsupportedProtocolType(ctx, genericDecl->getGenericParams());
    checkUnsupportedProtocolType(ctx, genericDecl->getTrailingWhereClause());
  } else if (auto *assocType = dyn_cast<AssociatedTypeDecl>(decl)) {
    checkUnsupportedProtocolType(ctx, assocType->getTrailingWhereClause());
  } else if (auto *extDecl = dyn_cast<ExtensionDecl>(decl)) {
    checkUnsupportedProtocolType(ctx, extDecl->getTrailingWhereClause());
  } else if (auto *subscriptDecl = dyn_cast<SubscriptDecl>(decl)) {
    checkUnsupportedProtocolType(ctx, subscriptDecl->getGenericParams());
    checkUnsupportedProtocolType(ctx, subscriptDecl->getTrailingWhereClause());
  } else if (auto *funcDecl = dyn_cast<AbstractFunctionDecl>(decl)) {
    if (!isa<AccessorDecl>(funcDecl)) {
      checkUnsupportedProtocolType(ctx, funcDecl->getGenericParams());
      checkUnsupportedProtocolType(ctx, funcDecl->getTrailingWhereClause());
    }
  }

  if (isa<TypeDecl>(decl) || isa<ExtensionDecl>(decl))
    return;

  UnsupportedProtocolVisitor visitor(ctx, /*checkStatements=*/false);
  decl->walk(visitor);
}

void TypeChecker::checkUnsupportedProtocolType(ASTContext &ctx, Stmt *stmt) {
  if (!stmt)
    return;

  UnsupportedProtocolVisitor visitor(ctx, /*checkStatements=*/true);
  stmt->walk(visitor);
}

void TypeChecker::checkUnsupportedProtocolType(
    ASTContext &ctx, TrailingWhereClause *whereClause) {
  if (whereClause == nullptr)
    return;

  UnsupportedProtocolVisitor visitor(ctx, /*checkStatements=*/false);
  visitor.visitRequirements(whereClause->getRequirements());
}

void TypeChecker::checkUnsupportedProtocolType(
    ASTContext &ctx, GenericParamList *genericParams) {
  if (genericParams  == nullptr)
    return;

  UnsupportedProtocolVisitor visitor(ctx, /*checkStatements=*/false);
  visitor.visitRequirements(genericParams->getRequirements());
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

  const auto type = TypeResolution::forContextual(dc, options, unboundTyOpener,
                                                  /*placeholderHandler*/nullptr)
                        .resolveType(attr->getTypeRepr());

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
