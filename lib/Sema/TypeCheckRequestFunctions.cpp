//===--- TypeCheckRequests.cpp - Type Checking Requests ------------------===//
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
#include "TypeChecker.h"
#include "TypeCheckType.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/Attr.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/MacroDefinition.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeLoc.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"
#include "swift/Subsystems.h"

using namespace swift;

InheritedTypeResult InheritedTypeRequest::evaluate(
    Evaluator &evaluator,
    llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl,
    unsigned index, TypeResolutionStage stage) const {
  // Figure out how to resolve types.
  DeclContext *dc;
  TypeResolverContext context;

  if (auto typeDecl = decl.dyn_cast<const TypeDecl *>()) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(typeDecl)) {
      dc = (DeclContext *)nominal;
      context = TypeResolverContext::Inherited;
    } else {
      dc = typeDecl->getDeclContext();
      if (isa<GenericTypeParamDecl>(typeDecl))
        context = TypeResolverContext::GenericParameterInherited;
      else {
        assert(isa<AssociatedTypeDecl>(typeDecl));
        context = TypeResolverContext::AssociatedTypeInherited;
      }
    }
  } else {
    dc = (DeclContext *)decl.get<const ExtensionDecl *>();
    context = TypeResolverContext::Inherited;
  }

  std::optional<TypeResolution> resolution;
  switch (stage) {
  case TypeResolutionStage::Structural:
    resolution =
        TypeResolution::forStructural(dc, context,
                                      /*unboundTyOpener*/ nullptr,
                                      /*placeholderHandler*/ nullptr,
                                      /*packElementOpener*/ nullptr);
    break;

  case TypeResolutionStage::Interface:
    resolution =
        TypeResolution::forInterface(dc, context,
                                     /*unboundTyOpener*/ nullptr,
                                     /*placeholderHandler*/ nullptr,
                                     /*packElementOpener*/ nullptr);
    break;
  }

  const InheritedEntry &inheritedEntry = InheritedTypes(decl).getEntry(index);

  Type inheritedType;
  if (auto *typeRepr = inheritedEntry.getTypeRepr()) {
    // Check for suppressed inferrable conformances.
    if (auto itr = dyn_cast<InverseTypeRepr>(typeRepr)) {
      Type inheritedTy = resolution->resolveType(itr->getConstraint());
      return InheritedTypeResult::forSuppressed(inheritedTy, itr);
    }
    inheritedType = resolution->resolveType(typeRepr);
  } else {
    auto ty = inheritedEntry.getType();
    if (inheritedEntry.isSuppressed()) {
      return InheritedTypeResult::forSuppressed(ty, nullptr);
    }
    inheritedType = ty;
  }

  return InheritedTypeResult::forInherited(
      inheritedType ? inheritedType : ErrorType::get(dc->getASTContext()));
}

Type
SuperclassTypeRequest::evaluate(Evaluator &evaluator,
                                ClassDecl *classDecl,
                                TypeResolutionStage stage) const {
  if (!classDecl->getSuperclassDecl())
    return Type();

  for (unsigned int idx : classDecl->getInherited().getIndices()) {
    auto result = evaluateOrDefault(evaluator,
                                    InheritedTypeRequest{classDecl, idx, stage},
                                    InheritedTypeResult::forDefault())
                      .getInheritedTypeOrNull(classDecl->getASTContext());
    if (!result)
      continue;

    // If we found a class, return it.
    if (result->getClassOrBoundGenericClass()) {
      return result;
    }

    // If we found an existential with a superclass bound, return it.
    if (result->isExistentialType()) {
      if (auto superclassType =
            result->getExistentialLayout().explicitSuperclass) {
        if (superclassType->getClassOrBoundGenericClass()) {
          return superclassType;
        }
      }
    }
  }

  // No superclass.
  return Type();
}

Type EnumRawTypeRequest::evaluate(Evaluator &evaluator,
                                  EnumDecl *enumDecl) const {
  for (unsigned int idx : enumDecl->getInherited().getIndices()) {
    auto inheritedType =
        evaluateOrDefault(
            evaluator,
            InheritedTypeRequest{enumDecl, idx, TypeResolutionStage::Interface},
            InheritedTypeResult::forDefault())
            .getInheritedTypeOrNull(enumDecl->getASTContext());
    if (!inheritedType) continue;

    // Skip protocol conformances.
    if (inheritedType->isConstraintType()) continue;

    // We found a raw type; return it.
    return inheritedType;
  }

  // No raw type.
  return Type();
}

bool SuppressesConformanceRequest::evaluate(Evaluator &evaluator,
                                            NominalTypeDecl *nominal,
                                            KnownProtocolKind kp) const {
  auto inheritedTypes = InheritedTypes(nominal);
  auto inheritedClause = inheritedTypes.getEntries();
  for (unsigned i = 0, n = inheritedClause.size(); i != n; ++i) {
    InheritedTypeRequest request{nominal, i, TypeResolutionStage::Interface};
    auto result = evaluateOrDefault(evaluator, request,
                                    InheritedTypeResult::forDefault());
    if (result != InheritedTypeResult::Suppressed)
      continue;
    auto pair = result.getSuppressed();
    auto ty = pair.first;
    if (!ty)
      continue;
    auto other = ty->getKnownProtocol();
    if (!other)
      continue;
    if (other == kp)
      return true;
  }
  return false;
}

CustomAttr *
AttachedResultBuilderRequest::evaluate(Evaluator &evaluator,
                                         ValueDecl *decl) const {
  ASTContext &ctx = decl->getASTContext();
  auto dc = decl->getDeclContext();
  for (auto attr : decl->getAttrs().getAttributes<CustomAttr>()) {
    auto mutableAttr = const_cast<CustomAttr *>(attr);
    // Figure out which nominal declaration this custom attribute refers to.
    auto *nominal = evaluateOrDefault(ctx.evaluator,
                                      CustomAttrNominalRequest{mutableAttr, dc},
                                      nullptr);

    if (!nominal)
      continue;

    // Return the first custom attribute that is a result builder type.
    if (nominal->getAttrs().hasAttribute<ResultBuilderAttr>())
      return mutableAttr;
  }

  return nullptr;
}

/// Attempt to infer the result builder type for a declaration.
static Type inferResultBuilderType(ValueDecl *decl)  {
  auto dc = decl->getDeclContext();
  if (!dc->isTypeContext() || isa<ProtocolDecl>(dc))
    return Type();

  auto funcDecl = dyn_cast<FuncDecl>(decl);
  if (!funcDecl || !funcDecl->hasBody() ||
      !decl->getDeclContext()->getParentSourceFile())
    return Type();

  // Check whether there are any return statements in the function's body.
  // If there are, the result builder transform will be disabled,
  // so don't infer a result builder.
  if (!TypeChecker::findReturnStatements(funcDecl).empty())
    return Type();

  // Only getters can have result builders. When we find one, look at
  // the storage declaration for the purposes of witness matching.
  auto lookupDecl = decl;
  if (auto accessor = dyn_cast<AccessorDecl>(funcDecl)) {
    if (accessor->getAccessorKind() != AccessorKind::Get)
      return Type();

    lookupDecl = accessor->getStorage();
  }

  // Find all of the potentially inferred result builder types.
  struct Match {
    enum Kind {
      Conformance,
      DynamicReplacement,
    } kind;

    union {
      struct {
        ProtocolConformance *conformance;
        ValueDecl *requirement;
      } conformanceMatch;

      ValueDecl *dynamicReplacement;
    };

    Type resultBuilderType;

    static Match forConformance(
        ProtocolConformance *conformance,
        ValueDecl *requirement,
        Type resultBuilderType) {
      Match match;
      match.kind = Conformance;
      match.conformanceMatch.conformance = conformance;
      match.conformanceMatch.requirement = requirement;
      match.resultBuilderType = resultBuilderType;
      return match;
    }

    static Match forDynamicReplacement(
        ValueDecl *dynamicReplacement, Type resultBuilderType) {
      Match match;
      match.kind = DynamicReplacement;
      match.dynamicReplacement = dynamicReplacement;
      match.resultBuilderType = resultBuilderType;
      return match;
    }

    DeclName getSourceName() const {
      switch (kind) {
      case Conformance:
        return conformanceMatch.conformance->getProtocol()->getName();

      case DynamicReplacement:
        return dynamicReplacement->getName();
      }
      llvm_unreachable("unhandled decl name kind!");
    }
  };

  // The set of matches from which we can infer result builder types.
  SmallVector<Match, 2> matches;

  // Determine all of the conformances within the same context as
  // this declaration. If this declaration is a witness to any
  // requirement within one of those protocols that has a result builder
  // attached, use that result builder type.
  auto addConformanceMatches = [&matches](ValueDecl *lookupDecl) {
    DeclContext *dc = lookupDecl->getDeclContext();
    auto idc = cast<IterableDeclContext>(dc->getAsDecl());
    auto conformances = idc->getLocalConformances(
        ConformanceLookupKind::NonStructural);

    for (auto conformance : conformances) {
      auto protocol = conformance->getProtocol();
      for (auto found : protocol->lookupDirect(lookupDecl->getName())) {
        if (!isa<ProtocolDecl>(found->getDeclContext()))
          continue;

        auto requirement = dyn_cast<ValueDecl>(found);
        if (!requirement)
          continue;

        Type resultBuilderType = requirement->getResultBuilderType();
        if (!resultBuilderType)
          continue;

        auto witness = conformance->getWitnessDecl(requirement);
        if (witness != lookupDecl)
          continue;

        // Substitute Self and associated type witnesses into the
        // result builder type. Type parameters will be mapped
        // into context when applying the result builder to the
        // function body in the constraint system.
        auto subs = SubstitutionMap::getProtocolSubstitutions(
            protocol, dc->getSelfInterfaceType(),
            ProtocolConformanceRef(conformance));
        Type subResultBuilderType = resultBuilderType.subst(subs);

        matches.push_back(
            Match::forConformance(
              conformance, requirement, subResultBuilderType));
      }
    }
  };

  addConformanceMatches(lookupDecl);

  // Look for result builder types inferred through dynamic replacements.
  if (auto replaced = lookupDecl->getDynamicallyReplacedDecl()) {
    if (auto resultBuilderType = replaced->getResultBuilderType()) {
      matches.push_back(
        Match::forDynamicReplacement(replaced, resultBuilderType));
    } else {
      addConformanceMatches(replaced);
    }
  }

  if (matches.size() == 0)
    return Type();

  // Determine whether there is more than one actual result builder type.
  Type resultBuilderType = matches[0].resultBuilderType;
  for (const auto &match : matches) {
    // If the types were the same anyway, there's nothing to do.
    Type otherResultBuilderType = match.resultBuilderType;
    if (resultBuilderType->isEqual(otherResultBuilderType))
      continue;

    // We have at least two different result builder types.
    // Diagnose the ambiguity and provide potential solutions.
    decl->diagnose(
        diag::result_builder_infer_ambig, lookupDecl->getName(),
        resultBuilderType, otherResultBuilderType);
    decl->diagnose(diag::result_builder_infer_add_return)
      .fixItInsert(funcDecl->getBodySourceRange().End, "return <#expr#>\n");
    for (const auto &match : matches) {
      decl->diagnose(
          diag::result_builder_infer_pick_specific,
          match.resultBuilderType,
          static_cast<unsigned>(match.kind),
          match.getSourceName())
        .fixItInsert(
          lookupDecl->getAttributeInsertionLoc(false),
          "@" + match.resultBuilderType.getString() + " ");
    }

    return Type();
  }

  return resultBuilderType;
}

Type ResultBuilderTypeRequest::evaluate(Evaluator &evaluator,
                                          ValueDecl *decl) const {
  // Look for a result-builder custom attribute.
  auto attr = decl->getAttachedResultBuilder();
  if (!attr)
    return inferResultBuilderType(decl);

  // Resolve a type for the attribute.
  auto mutableAttr = const_cast<CustomAttr*>(attr);
  auto dc = decl->getDeclContext();
  auto &ctx = dc->getASTContext();
  Type type = evaluateOrDefault(
      evaluator,
      CustomAttrTypeRequest{mutableAttr, dc, CustomAttrTypeKind::NonGeneric},
      Type());
  if (!type || type->hasError()) return Type();

  auto nominal = type->getAnyNominal();
  if (!nominal) {
    assert(ctx.Diags.hadAnyError());
    return Type();
  }

  // Do some additional checking on parameters.
  if (auto param = dyn_cast<ParamDecl>(decl)) {
    // The parameter had better already have an interface type.
    Type paramType = param->getInterfaceType();
    assert(paramType);
    auto paramFnType = paramType->getAs<FunctionType>();

    // Require the parameter to be an interface type.
    if (!paramFnType) {
      ctx.Diags.diagnose(attr->getLocation(),
                         diag::result_builder_parameter_not_of_function_type,
                         nominal->getName());
      mutableAttr->setInvalid();
      return Type();
    }

    // Forbid the parameter to be an autoclosure.
    if (param->isAutoClosure()) {
      ctx.Diags.diagnose(attr->getLocation(),
                         diag::result_builder_parameter_autoclosure,
                         nominal->getName());
      mutableAttr->setInvalid();
      return Type();
    }
  }

  return type->mapTypeOutOfContext();
}

// Define request evaluation functions for each of the type checker requests.
static AbstractRequestFunction *typeCheckerRequestFunctions[] = {
#define SWIFT_REQUEST(Zone, Name, Sig, Caching, LocOptions)                    \
  reinterpret_cast<AbstractRequestFunction *>(&Name::evaluateRequest),
#include "swift/AST/TypeCheckerTypeIDZone.def"
#undef SWIFT_REQUEST
};

void swift::registerTypeCheckerRequestFunctions(Evaluator &evaluator) {
  evaluator.registerRequestFunctions(Zone::TypeChecker,
                                     typeCheckerRequestFunctions);
}
