//===--- TypeCheckGeneric.cpp - Generics ----------------------------------===//
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
// This file implements support for generics.
//
//===----------------------------------------------------------------------===//
#include "TypeCheckProtocol.h"
#include "TypeCheckType.h"
#include "TypeChecker.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeResolutionStage.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Defer.h"
#include "llvm/Support/ErrorHandling.h"

using namespace swift;

//
// Generic functions
//

/// Get the opaque type representing the return type of a declaration, or
/// create it if it does not yet exist.
OpaqueTypeDecl *
OpaqueResultTypeRequest::evaluate(Evaluator &evaluator,
                                  ValueDecl *originatingDecl) const {
  auto *repr = originatingDecl->getOpaqueResultTypeRepr();
  assert(repr && "Declaration does not have an opaque result type");
  auto *dc = originatingDecl->getInnermostDeclContext();
  auto &ctx = dc->getASTContext();

  // Protocol requirements can't have opaque return types.
  //
  // TODO: Maybe one day we could treat this as sugar for an associated type.
  if (isa<ProtocolDecl>(originatingDecl->getDeclContext())
      && originatingDecl->isProtocolRequirement()) {

    SourceLoc fixitLoc;
    if (auto vd = dyn_cast<VarDecl>(originatingDecl)) {
      fixitLoc = vd->getParentPatternBinding()->getStartLoc();
    } else {
      fixitLoc = originatingDecl->getStartLoc();
    }

    std::string result;
    const char *const placeholder = "<#AssocType#>";
    {
      llvm::raw_string_ostream out(result);
      out << "associatedtype " << placeholder << ": ";
      // FIXME [OPAQUE SUPPORT]: to produce the right associate type for the
      // replacement in general, we would need to recurse into the type repr and
      // replace every `OpaqueReturnType` with its 'constraint'. Things get
      // trickier when we allow named opaque return types.
      if (isa<OpaqueReturnTypeRepr>(repr)) {
        cast<OpaqueReturnTypeRepr>(repr)->getConstraint()->print(out);
      } else {
        out << "<#type#>";
      }
      out << "\n";
    }

    ctx.Diags
        .diagnose(repr->getLoc(), diag::opaque_type_in_protocol_requirement)
        .fixItInsert(fixitLoc, result)
        .fixItReplace(repr->getSourceRange(), placeholder);
    repr->setInvalid();

    return nullptr;
  }

  // Check the availability of the opaque type runtime support.
  TypeChecker::checkAvailability(
      repr->getSourceRange(),
      ctx.getOpaqueTypeAvailability(),
      diag::availability_opaque_types_only_version_newer,
      originatingDecl->getInnermostDeclContext());

  // Create a generic signature for the opaque environment. This is the outer
  // generic signature with an added generic parameters representing the opaque
  // types and their interface constraints.
  auto originatingDC = originatingDecl->getInnermostDeclContext();
  auto outerGenericSignature = originatingDC->getGenericSignatureOfContext();
  unsigned opaqueSignatureDepth =
      outerGenericSignature
          ? outerGenericSignature.getGenericParams().back()->getDepth() + 1
          : 0;

  // Determine the context of the opaque type declaration we'll be creating.
  auto parentDC = originatingDecl->getDeclContext();
  auto originatingGenericContext = originatingDecl->getAsGenericContext();
  GenericParamList *genericParams;
  GenericSignature interfaceSignature;
  CollectedOpaqueReprs opaqueReprs;
  if (auto namedOpaque = dyn_cast<NamedOpaqueReturnTypeRepr>(repr)) {
    // Produce the generic signature for the opaque type.
    genericParams = namedOpaque->getGenericParams();
    genericParams->setDepth(opaqueSignatureDepth);

    InferredGenericSignatureRequest request{
        outerGenericSignature.getPointer(),
        genericParams,
        WhereClauseOwner(),
        /*addedRequirements=*/{},
        /*inferenceSources=*/{},
        /*allowConcreteGenericParams=*/false};

    interfaceSignature = evaluateOrDefault(
        ctx.evaluator, request, GenericSignatureWithError())
          .getPointer();
    if (!interfaceSignature) {
      // Already produced an error.
      return nullptr;
    }
  } else {
    opaqueReprs = collectOpaqueTypeReprs(repr, ctx, dc);

    if (opaqueReprs.empty()) {
      return nullptr;
    }

    SmallVector<GenericTypeParamType *, 2> genericParamTypes;
    SmallVector<Requirement, 2> requirements;
    for (unsigned i = 0; i < opaqueReprs.size(); ++i) {
      auto *currentRepr = opaqueReprs[i];

      if( auto opaqueReturn = dyn_cast<OpaqueReturnTypeRepr>(currentRepr) ) {
        // Usually, we resolve the opaque constraint and bail if it isn't a class
        // or existential type (see below). However, in this case we know we will
        // fail, so we can bail early and provide a better diagnostic.
        if (auto *optionalRepr =
                dyn_cast<OptionalTypeRepr>(opaqueReturn->getConstraint())) {
          std::string buf;
          llvm::raw_string_ostream stream(buf);
          stream << "(some " << optionalRepr->getBase() << ")?";

          ctx.Diags.diagnose(currentRepr->getLoc(),
                             diag::opaque_type_invalid_constraint);
          ctx.Diags
             .diagnose(currentRepr->getLoc(), diag::opaque_of_optional_rewrite)
             .fixItReplaceChars(currentRepr->getStartLoc(),
                                currentRepr->getEndLoc(), stream.str());
          repr->setInvalid();
          return nullptr;
        }
      }

      auto *paramType = GenericTypeParamType::get(/*isParameterPack*/ false,
                                                  opaqueSignatureDepth, i, ctx);
      genericParamTypes.push_back(paramType);
    
      TypeRepr *constraint = currentRepr;
      
      if (auto opaqueReturn = dyn_cast<OpaqueReturnTypeRepr>(currentRepr)){
        constraint = opaqueReturn->getConstraint();
      }
      // Try to resolve the constraint repr in the parent decl context. It
      // should be some kind of existential type. Pass along the error type if
      // resolving the repr failed.
      auto constraintType = TypeResolution::forInterface(
                                dc, TypeResolverContext::GenericRequirement,
                                // Unbound generics and placeholders are
                                // meaningless in opaque types.
                                /*unboundTyOpener*/ nullptr,
                                /*placeholderHandler*/ nullptr,
                                /*packElementOpener*/ nullptr)
                                .resolveType(constraint);

      if (constraintType->hasError())
        return nullptr;

      RequirementKind kind;
      if (constraintType->isConstraintType())
        kind = RequirementKind::Conformance;
      else if (constraintType->getClassOrBoundGenericClass())
        kind = RequirementKind::Superclass;
      else {
        // Error out if the constraint type isn't a class or existential type.
        ctx.Diags.diagnose(currentRepr->getLoc(),
                           diag::opaque_type_invalid_constraint);
        currentRepr->setInvalid();
        return nullptr;
      }

      assert(!constraintType->hasArchetype());
      requirements.emplace_back(kind, paramType, constraintType);
    }

    interfaceSignature = buildGenericSignature(ctx, outerGenericSignature,
                                               genericParamTypes,
                                               std::move(requirements));
    genericParams = originatingGenericContext
        ? originatingGenericContext->getGenericParams()
        : nullptr;
  }

  // Create the OpaqueTypeDecl for the result type.
  auto opaqueDecl = OpaqueTypeDecl::get(
      originatingDecl, genericParams, parentDC, interfaceSignature,
      opaqueReprs);
  if (auto originatingSig = originatingDC->getGenericSignatureOfContext()) {
    opaqueDecl->setGenericSignature(originatingSig);
  } else {
    // Avoid kicking off GenericSignatureRequest for the OpaqueTypeDecl.
    opaqueDecl->setGenericSignature(GenericSignature());
  }

  // Resolving in the context of `opaqueDecl` allows type resolution to create
  // opaque archetypes where needed
  auto interfaceType =
      TypeResolution::forInterface(opaqueDecl, TypeResolverContext::None,
                                   /*unboundTyOpener*/ nullptr,
                                   /*placeholderHandler*/ nullptr,
                                   /*packElementOpener*/ nullptr)
          .resolveType(repr);

  // Opaque types cannot be used in parameter position.
  Type desugared = interfaceType->getDesugaredType();
  bool hasError = desugared.findIf([&](Type type) -> bool {
    if (auto *fnType = type->getAs<FunctionType>()) {
      for (auto param : fnType->getParams()) {
        if (!param.getPlainType()->hasOpaqueArchetype())
          continue;

        ctx.Diags.diagnose(repr->getLoc(),
                           diag::opaque_type_in_parameter,
                           false, interfaceType);
        repr->setInvalid();
        return true;
      }
    }

    return false;
  });

  if (hasError)
    return nullptr;

  auto metatype = MetatypeType::get(interfaceType);
  opaqueDecl->setInterfaceType(metatype);

  // Record the opaque return type decl in the parent source file,
  // which will be used in IRGen to emit all opaque type decls
  // in a Swift module for type reconstruction.
  if (auto *sourceFile = dc->getParentSourceFile())
    sourceFile->addOpaqueResultTypeDecl(opaqueDecl);

  return opaqueDecl;
}

/// Determine whether the given type is \c Self, an associated type of \c Self,
/// or a concrete type.
static bool isSelfDerivedOrConcrete(Type protoSelf, Type type) {
  // Check for a concrete type.
  if (!type->hasTypeParameter())
    return true;

  if (type->isTypeParameter() &&
      type->getRootGenericParam()->isEqual(protoSelf))
    return true;

  return false;
}

// For a generic requirement in a protocol, make sure that the requirement
// set didn't add any requirements to Self or its associated types.
void TypeChecker::checkProtocolSelfRequirements(ValueDecl *decl) {
  // For a generic requirement in a protocol, make sure that the requirement
  // set didn't add any requirements to Self or its associated types.
  if (auto *proto = dyn_cast<ProtocolDecl>(decl->getDeclContext())) {
    auto &ctx = proto->getASTContext();
    auto protoSelf = proto->getSelfInterfaceType();
    auto sig = decl->getInnermostDeclContext()->getGenericSignatureOfContext();
    for (auto req : sig.getRequirements()) {
      // If one of the types in the requirement is dependent on a non-Self
      // type parameter, this requirement is okay.
      if (!isSelfDerivedOrConcrete(protoSelf, req.getFirstType()) ||
          !isSelfDerivedOrConcrete(protoSelf, req.getSecondType()))
        continue;

      // The conformance of 'Self' to the protocol is okay.
      if (req.getKind() == RequirementKind::Conformance &&
          req.getProtocolDecl() == proto &&
          req.getFirstType()->is<GenericTypeParamType>())
        continue;

      ctx.Diags.diagnose(decl,
                         diag::requirement_restricts_self,
                         decl->getDescriptiveKind(), decl->getName(),
                         req.getFirstType().getString(),
                         static_cast<unsigned>(req.getKind()),
                         req.getSecondType().getString());
    }
  }
}

/// All generic parameters of a generic function must be referenced in the
/// declaration's type, otherwise we have no way to infer them.
void TypeChecker::checkReferencedGenericParams(GenericContext *dc) {
  // Don't do this check for accessors: they're not used directly, so we
  // never need to infer their generic arguments.  This is mostly a
  // compile-time optimization, but it also avoids problems with accessors
  // like 'read' and 'modify' that would arise due to yields not being
  // part of the formal type.
  if (isa<AccessorDecl>(dc))
    return;

  auto *genericParams = dc->getGenericParams();
  auto genericSig = dc->getGenericSignatureOfContext();
  if (!genericParams)
    return;

  auto *decl = cast<ValueDecl>(dc->getInnermostDeclarationDeclContext());

  // A helper class to collect referenced generic type parameters
  // and dependent member types.
  class ReferencedGenericTypeWalker : public TypeWalker {
    SmallPtrSet<CanType, 4> ReferencedGenericParams;

  public:
    ReferencedGenericTypeWalker() {}
    Action walkToTypePre(Type ty) override {
      // Find generic parameters or dependent member types.
      // Once such a type is found, don't recurse into its children.
      if (!ty->hasTypeParameter())
        return Action::SkipChildren;
      if (ty->isTypeParameter()) {
        ReferencedGenericParams.insert(ty->getCanonicalType());
        return Action::SkipChildren;
      }
      return Action::Continue;
    }

    SmallPtrSetImpl<CanType> &getReferencedGenericParams() {
      return ReferencedGenericParams;
    }
  };

  // Collect all generic params referenced in parameter types and
  // return type.
  ReferencedGenericTypeWalker paramsAndResultWalker;
  auto *funcTy = decl->getInterfaceType()->castTo<GenericFunctionType>();
  for (const auto &param : funcTy->getParams())
    param.getPlainType().walk(paramsAndResultWalker);
  funcTy->getResult().walk(paramsAndResultWalker);

  // Set of generic params referenced in parameter types,
  // return type or requirements.
  auto &referencedGenericParams =
      paramsAndResultWalker.getReferencedGenericParams();

  // Check if at least one of the generic params in the requirement refers
  // to an already referenced generic parameter. If this is the case,
  // then the other type is also considered as referenced, because
  // it is used to put requirements on the first type.
  auto reqTypesVisitor = [&referencedGenericParams](Requirement req) -> bool {
    Type first;
    Type second;

    switch (req.getKind()) {
    case RequirementKind::SameShape:
    case RequirementKind::Superclass:
    case RequirementKind::SameType:
      second = req.getSecondType();
      LLVM_FALLTHROUGH;

    case RequirementKind::Conformance:
    case RequirementKind::Layout:
      first = req.getFirstType();
      break;
    }

    // Collect generic parameter types referenced by types used in a requirement.
    ReferencedGenericTypeWalker walker;
    if (first && first->hasTypeParameter())
      first.walk(walker);
    if (second && second->hasTypeParameter())
      second.walk(walker);
    auto &genericParamsUsedByRequirementTypes =
        walker.getReferencedGenericParams();

    // If at least one of the collected generic types or a root generic
    // parameter of dependent member types is known to be referenced by
    // parameter types, return types or other types known to be "referenced",
    // then all the types used in the requirement are considered to be
    // referenced, because they are used to defined something that is known
    // to be referenced.
    bool foundNewReferencedGenericParam = false;
    if (std::any_of(genericParamsUsedByRequirementTypes.begin(),
                    genericParamsUsedByRequirementTypes.end(),
                    [&referencedGenericParams](CanType t) {
                      assert(t->isTypeParameter());
                      return referencedGenericParams.find(
                                 t->getRootGenericParam()
                                     ->getCanonicalType()) !=
                             referencedGenericParams.end();
                    })) {
      std::for_each(genericParamsUsedByRequirementTypes.begin(),
                    genericParamsUsedByRequirementTypes.end(),
                    [&referencedGenericParams,
                     &foundNewReferencedGenericParam](CanType t) {
                      // Add only generic type parameters, but ignore any
                      // dependent member types, because requirement
                      // on a dependent member type does not provide enough
                      // information to infer the base generic type
                      // parameter.
                      if (!t->is<GenericTypeParamType>())
                        return;
                      if (referencedGenericParams.insert(t).second)
                        foundNewReferencedGenericParam = true;
                    });
    }
    return foundNewReferencedGenericParam;
  };

  ArrayRef<Requirement> requirements;

  auto FindReferencedGenericParamsInRequirements =
    [&requirements, genericSig, &reqTypesVisitor] {
    requirements = genericSig.getRequirements();
    // Try to find new referenced generic parameter types in requirements until
    // we reach a fix point. We need to iterate until a fix point, because we
    // may have e.g. chains of same-type requirements like:
    // not-yet-referenced-T1 == not-yet-referenced-T2.DepType2,
    // not-yet-referenced-T2 == not-yet-referenced-T3.DepType3,
    // not-yet-referenced-T3 == referenced-T4.DepType4.
    // When we process the first of these requirements, we don't know yet that
    // T2
    // will be referenced, because T3 will be referenced,
    // because T3 == T4.DepType4.
    while (true) {
      bool foundNewReferencedGenericParam = false;
      for (auto req : requirements) {
        if (reqTypesVisitor(req))
          foundNewReferencedGenericParam = true;
      }
      if (!foundNewReferencedGenericParam)
        break;
    }
  };

  // Find the depth of the function's own generic parameters.
  unsigned fnGenericParamsDepth = genericParams->getParams().front()->getDepth();

  // Check that every generic parameter type from the signature is
  // among referencedGenericParams.
  for (auto *genParam : genericSig.getGenericParams()) {
    auto *paramDecl = genParam->getDecl();
    if (paramDecl->getDepth() != fnGenericParamsDepth)
      continue;
    if (!referencedGenericParams.count(genParam->getCanonicalType())) {
      // Lazily search for generic params that are indirectly used in the
      // function signature. Do it only if there is a generic parameter
      // that is not known to be referenced yet.
      if (requirements.empty()) {
        FindReferencedGenericParamsInRequirements();
        // Nothing to do if this generic parameter is considered to be
        // referenced after analyzing the requirements from the generic
        // signature.
        if (referencedGenericParams.count(genParam->getCanonicalType()))
          continue;
      }
      // Produce an error that this generic parameter cannot be bound.
      if (paramDecl->isImplicit()) {
        paramDecl->getASTContext().Diags
          .diagnose(paramDecl->getOpaqueTypeRepr()->getLoc(),
                    diag::unreferenced_generic_parameter,
                    paramDecl->getNameStr());
      } else {
        paramDecl->diagnose(diag::unreferenced_generic_parameter,
                            paramDecl->getNameStr());
      }
    }
  }
}

/// Ensure we don't re-declare any generic parameters in the current scope,
/// or shadow a generic parameter from an outer scope.
void TypeChecker::checkShadowedGenericParams(GenericContext *dc) {
  // Collect all outer generic parameters for lookup.
  llvm::SmallDenseMap<Identifier, GenericTypeParamDecl *, 4> genericParamDecls;
  for (auto *parentDC = dc->getParent(); parentDC != nullptr;
       parentDC = parentDC->getParentForLookup()) {
    if (auto *extensionDecl = dyn_cast<ExtensionDecl>(parentDC)) {
      parentDC = extensionDecl->getExtendedNominal();

      // This can happen with invalid code.
      if (parentDC == nullptr)
        return;
    }
    if (auto *parentDecl = parentDC->getAsDecl()) {
      if (auto *parentGeneric = parentDecl->getAsGenericContext()) {
        if (auto *genericParamList = parentGeneric->getGenericParams()) {
          for (auto *genericParamDecl : genericParamList->getParams()) {
            if (genericParamDecl->isOpaqueType())
              continue;
            genericParamDecls[genericParamDecl->getName()] = genericParamDecl;
          }
        }
      }
    }
  }

  for (auto *genericParamDecl : dc->getGenericParams()->getParams()) {
    if (genericParamDecl->isOpaqueType() || genericParamDecl->isImplicit())
      continue;

    auto found = genericParamDecls.find(genericParamDecl->getName());
    if (found != genericParamDecls.end()) {
      auto *existingParamDecl = found->second;

      if (existingParamDecl->getDeclContext() == dc) {
        genericParamDecl->diagnose(
            diag::invalid_redecl,
            genericParamDecl->getName());
      } else {
        genericParamDecl->diagnose(
            diag::shadowed_generic_param,
            genericParamDecl->getName()).warnUntilSwiftVersion(6);
      }

      if (existingParamDecl->getLoc()) {
        existingParamDecl->diagnose(diag::invalid_redecl_prev,
                                    existingParamDecl->getName());
      }

      continue;
    }

    genericParamDecls[genericParamDecl->getName()] = genericParamDecl;
  }
}

///
/// Generic types
///

/// Collect additional requirements into \p sameTypeReqs.
static void collectAdditionalExtensionRequirements(
    Type type, SmallVectorImpl<Requirement> &sameTypeReqs) {
  if (type->is<ErrorType>())
    return;

  // Find the nominal type declaration and its parent type.
  if (type->is<ProtocolCompositionType>())
    type = type->getCanonicalType();

  // A parameterized protocol type is not a nominal. Unwrap it to get
  // the underlying nominal, and record same-type requirements for
  // the primary associated types.
  if (auto *paramProtoTy = type->getAs<ParameterizedProtocolType>()) {
    auto *protoTy = paramProtoTy->getBaseType();
    type = protoTy;

    paramProtoTy->getRequirements(
        protoTy->getDecl()->getSelfInterfaceType(),
        sameTypeReqs);
  }

  Type parentType = type->getNominalParent();
  GenericTypeDecl *genericDecl = type->getAnyGeneric();

  // Visit the parent type, if there is one.
  if (parentType) {
    collectAdditionalExtensionRequirements(parentType, sameTypeReqs);
  }

  // Find the nominal type.
  auto nominal = dyn_cast<NominalTypeDecl>(genericDecl);
  auto typealias = dyn_cast<TypeAliasDecl>(genericDecl);
  if (!nominal) {
    type = typealias->getUnderlyingType();
    nominal = type->getNominalOrBoundGenericNominal();
  }

  // If we have a bound generic type, add same-type requirements for each of
  // its generic arguments.
  if (auto currentBoundType = type->getAs<BoundGenericType>()) {
    auto *genericParams = currentBoundType->getDecl()->getGenericParams();
    for (unsigned gpIndex : indices(genericParams->getParams())) {
      auto *gp = genericParams->getParams()[gpIndex];
      auto gpType = gp->getDeclaredInterfaceType();

      sameTypeReqs.emplace_back(RequirementKind::SameType, gpType,
                                currentBoundType->getGenericArgs()[gpIndex]);
    }
  }

  // If we have a passthrough typealias, add the requirements from its
  // generic signature.
  if (typealias && TypeChecker::isPassThroughTypealias(typealias, nominal)) {
    for (auto req : typealias->getGenericSignature().getRequirements())
      sameTypeReqs.push_back(req);
  }
}

GenericSignature
GenericSignatureRequest::evaluate(Evaluator &evaluator,
                                  GenericContext *GC) const {
  assert(!isa<OpaqueTypeDecl>(GC));

  auto &ctx = GC->getASTContext();

  // The signature of a Protocol is trivial (Self: TheProtocol) so let's compute
  // it.
  if (auto PD = dyn_cast<ProtocolDecl>(GC)) {
    auto self = PD->getSelfInterfaceType()->castTo<GenericTypeParamType>();
    auto req =
        Requirement(RequirementKind::Conformance, self,
                    PD->getDeclaredInterfaceType());
    auto sig = GenericSignature::get({self}, {req});

    // Debugging of the generic signature builder and generic signature
    // generation.
    if (ctx.TypeCheckerOpts.DebugGenericSignatures) {
      llvm::errs() << "\n";
      PD->printContext(llvm::errs());
      llvm::errs() << "Generic signature: ";
      PrintOptions Opts;
      Opts.ProtocolQualifiedDependentMemberTypes = true;
      sig->print(llvm::errs(), Opts);
      llvm::errs() << "\n";
      llvm::errs() << "Canonical generic signature: ";
      sig.getCanonicalSignature()->print(llvm::errs(), Opts);
      llvm::errs() << "\n";
    }
    return sig;
  }

  if (auto accessor = dyn_cast<AccessorDecl>(GC))
    if (auto subscript = dyn_cast<SubscriptDecl>(accessor->getStorage()))
       return subscript->getGenericSignature();

  bool allowConcreteGenericParams = false;
  auto *genericParams = GC->getGenericParams();
  const auto *where = GC->getTrailingWhereClause();

  if (!genericParams && !where) {
    // We can fast-path computing the generic signature of non-generic
    // declarations by re-using the parent context's signature.
    return GC->getParentForLookup()->getGenericSignatureOfContext();
  }

  if (genericParams) {
    // Setup the depth of the generic parameters.
    const_cast<GenericParamList *>(genericParams)
        ->setDepth(GC->getGenericContextDepth());
  }

  // ...or we may only have a contextual where clause.
  if (where) {
    // If there is no generic context for the where clause to
    // rely on, diagnose that now and bail out.
    if (!GC->isGenericContext()) {
      ctx.Diags.diagnose(where->getWhereLoc(),
                         GC->getParent()->isModuleScopeContext()
                             ? diag::where_nongeneric_toplevel
                             : diag::where_nongeneric_ctx);
      return nullptr;
    }
  }

  if (!genericParams && where)
    allowConcreteGenericParams = true;

  GenericSignature parentSig;
  SmallVector<TypeLoc, 2> inferenceSources;
  SmallVector<Requirement, 2> sameTypeReqs;
  if (auto VD = dyn_cast<ValueDecl>(GC->getAsDecl())) {
    parentSig = GC->getParentForLookup()->getGenericSignatureOfContext();

    auto func = dyn_cast<AbstractFunctionDecl>(VD);
    auto subscr = dyn_cast<SubscriptDecl>(VD);
    auto macro = dyn_cast<MacroDecl>(VD);
    assert(func || subscr || macro || isa<NominalTypeDecl>(VD) ||
           isa<TypeAliasDecl>(VD));

    // For functions and subscripts, resolve the parameter and result types and
    // note them as requirement inference sources.
    if (subscr || func || (macro && macro->parameterList)) {
      const auto baseOptions =
          TypeResolutionOptions(func ? TypeResolverContext::AbstractFunctionDecl
                                     : TypeResolverContext::SubscriptDecl);

      const auto resolution =
          TypeResolution::forStructural(GC, baseOptions,
                                        /*unboundTyOpener*/ nullptr,
                                        /*placeholderHandler*/ nullptr,
                                        /*packElementOpener*/ nullptr);
      auto params = func ? func->getParameters()
                      : subscr ? subscr->getIndices()
                      : macro->parameterList;
      for (auto param : *params) {
        auto *typeRepr = param->getTypeRepr();
        if (typeRepr == nullptr)
            continue;

        auto paramOptions = baseOptions;

        if (auto *specifier = dyn_cast<SpecifierTypeRepr>(typeRepr)) {
          if (isa<OwnershipTypeRepr>(specifier))
            paramOptions |= TypeResolutionFlags::HasOwnership;

          typeRepr = specifier->getBase();
        }

        if (auto *packExpansion = dyn_cast<VarargTypeRepr>(typeRepr)) {
          paramOptions.setContext(TypeResolverContext::VariadicFunctionInput);
        } else {
          paramOptions.setContext(TypeResolverContext::FunctionInput);
        }

        paramOptions |= TypeResolutionFlags::Direct;

        const auto type =
            resolution.withOptions(paramOptions).resolveType(typeRepr);

        inferenceSources.emplace_back(typeRepr, type);
      }

      // Gather requirements from the result type.
      auto *resultTypeRepr = [&subscr, &func, &macro]() -> TypeRepr * {
        if (subscr) {
          return subscr->getElementTypeRepr();
        } else if (macro) {
          return macro->resultType.getTypeRepr();
        } else if (auto *FD = dyn_cast<FuncDecl>(func)) {
          return FD->getResultTypeRepr();
        } else {
          return nullptr;
        }
      }();
      if (resultTypeRepr && !resultTypeRepr->hasOpaque()) {
        const auto resultType =
            resolution.withOptions(TypeResolverContext::FunctionResult)
                .resolveType(resultTypeRepr);

        inferenceSources.emplace_back(resultTypeRepr, resultType);
      }
    }
  } else if (auto *ext = dyn_cast<ExtensionDecl>(GC)) {
    parentSig = ext->getExtendedNominal()->getGenericSignatureOfContext();
    genericParams = nullptr;

    collectAdditionalExtensionRequirements(ext->getExtendedType(), sameTypeReqs);

    // Re-use the signature of the type being extended by default.
    if (sameTypeReqs.empty() && !ext->getTrailingWhereClause()) {
      return parentSig;
    }

    // Extensions allow parameters to be equated with concrete types.
    allowConcreteGenericParams = true;
  } else {
    llvm_unreachable("Unknown generic declaration kind");
  }

  auto request = InferredGenericSignatureRequest{
      parentSig.getPointer(),
      genericParams, WhereClauseOwner(GC),
      sameTypeReqs, inferenceSources,
      allowConcreteGenericParams};
  auto sig = evaluateOrDefault(ctx.evaluator, request,
                               GenericSignatureWithError()).getPointer();

  // Debugging of the generic signature builder and generic signature
  // generation.
  if (ctx.TypeCheckerOpts.DebugGenericSignatures) {
    llvm::errs() << "\n";
    if (auto *VD = dyn_cast_or_null<ValueDecl>(GC->getAsDecl())) {
      VD->dumpRef(llvm::errs());
      llvm::errs() << "\n";
    } else {
      GC->printContext(llvm::errs());
    }
    llvm::errs() << "Generic signature: ";
    PrintOptions Opts;
    Opts.ProtocolQualifiedDependentMemberTypes = true;
    sig->print(llvm::errs(), Opts);
    llvm::errs() << "\n";
    llvm::errs() << "Canonical generic signature: ";
    sig.getCanonicalSignature()->print(llvm::errs(), Opts);
    llvm::errs() << "\n";
  }

  return sig;
}

///
/// Checking bound generic type arguments
///

/// Create a text string that describes the bindings of generic parameters
/// that are relevant to the given set of types, e.g.,
/// "[with T = Bar, U = Wibble]".
///
/// \param types The types that will be scanned for generic type parameters,
/// which will be used in the resulting type.
///
/// \param genericParams The generic parameters to use to resugar any
/// generic parameters that occur within the types.
///
/// \param substitutions The generic parameter -> generic argument
/// substitutions that will have been applied to these types.
/// These are used to produce the "parameter = argument" bindings in the test.
static std::string gatherGenericParamBindingsText(
    ArrayRef<Type> types, ArrayRef<GenericTypeParamType *> genericParams,
    TypeSubstitutionFn substitutions) {
  llvm::SmallPtrSet<GenericTypeParamType *, 2> knownGenericParams;
  for (auto type : types) {
    if (type.isNull()) continue;

    type.visit([&](Type type) {
      if (auto gp = type->getAs<GenericTypeParamType>()) {
        knownGenericParams.insert(
            gp->getCanonicalType()->castTo<GenericTypeParamType>());
      }
    });
  }

  if (knownGenericParams.empty())
    return "";

  SmallString<128> result;
  llvm::raw_svector_ostream OS(result);
  auto options = PrintOptions::forDiagnosticArguments();

  for (auto gp : genericParams) {
    auto canonGP = gp->getCanonicalType()->castTo<GenericTypeParamType>();
    if (!knownGenericParams.count(canonGP))
      continue;

    if (result.empty())
      OS << " [with ";
    else
      OS << "; ";

    if (gp->isParameterPack())
      OS << "each ";

    OS << gp->getName().str();
    OS << " = ";

    auto type = substitutions(canonGP);
    if (!type)
      return "";

    type->print(OS, options);
  }

  OS << "]";
  return std::string(result.str());
}

void TypeChecker::diagnoseRequirementFailure(
    const CheckGenericArgumentsResult::RequirementFailureInfo &reqFailureInfo,
    SourceLoc errorLoc, SourceLoc noteLoc, Type targetTy,
    ArrayRef<GenericTypeParamType *> genericParams,
    TypeSubstitutionFn substitutions, ModuleDecl *module) {
  assert(errorLoc.isValid() && noteLoc.isValid());

  const auto &req = reqFailureInfo.Req;
  const auto &substReq = reqFailureInfo.SubstReq;

  Diag<Type, Type, Type> diagnostic;
  Diag<Type, Type, StringRef> diagnosticNote;

  const auto reqKind = req.getKind();
  switch (reqKind) {
  case RequirementKind::SameShape:
    diagnostic = diag::types_not_same_shape;
    diagnosticNote = diag::same_shape_requirement;
    break;

  case RequirementKind::Conformance: {
    diagnoseConformanceFailure(substReq.getFirstType(),
                               substReq.getProtocolDecl(), module, errorLoc);

    if (reqFailureInfo.ReqPath.empty())
      return;

    diagnostic = diag::type_does_not_conform_owner;
    diagnosticNote = diag::type_does_not_inherit_or_conform_requirement;
    break;
  }

  case RequirementKind::Layout:
    diagnostic = diag::type_is_not_a_class;
    diagnosticNote = diag::anyobject_requirement;
    break;

  case RequirementKind::Superclass:
    diagnostic = diag::type_does_not_inherit;
    diagnosticNote = diag::type_does_not_inherit_or_conform_requirement;
    break;

  case RequirementKind::SameType:
    diagnostic = diag::types_not_equal;
    diagnosticNote = diag::types_not_equal_requirement;
    break;
  }

  Type secondTy, substSecondTy;
  if (req.getKind() != RequirementKind::Layout) {
    secondTy = req.getSecondType();
    substSecondTy = substReq.getSecondType();
  }

  ASTContext &ctx = module->getASTContext();
  // FIXME: Poor source-location information.
  ctx.Diags.diagnose(errorLoc, diagnostic, targetTy, substReq.getFirstType(),
                     substSecondTy);

  const auto genericParamBindingsText = gatherGenericParamBindingsText(
      {req.getFirstType(), secondTy}, genericParams, substitutions);

  ctx.Diags.diagnose(noteLoc, diagnosticNote, req.getFirstType(), secondTy,
                     genericParamBindingsText);

  ParentConditionalConformance::diagnoseConformanceStack(
      ctx.Diags, noteLoc, reqFailureInfo.ReqPath);
}

CheckGenericArgumentsResult TypeChecker::checkGenericArgumentsForDiagnostics(
    ModuleDecl *module, ArrayRef<Requirement> requirements,
    TypeSubstitutionFn substitutions) {
  using ParentConditionalConformances =
      SmallVector<ParentConditionalConformance, 2>;

  struct WorklistItem {
    /// The requirement to check. This is either a top-level requirement or
    /// a conditional requirements of the last conformancein \c ReqsPath
    /// (if any).
    Requirement Req;

    /// The substituted requirement.
    Requirement SubstReq;

    /// The chain of conditional conformances that leads to the above
    /// requirement set.
    ParentConditionalConformances Path;

    WorklistItem(Requirement Req, Requirement SubstReq,
                 ParentConditionalConformances Path)
        : Req(Req), SubstReq(SubstReq), Path(Path) {}
  };

  bool hadSubstFailure = false;
  SmallVector<WorklistItem, 4> worklist;

  for (auto req : llvm::reverse(requirements)) {
    auto substReq = req.subst(substitutions, LookUpConformanceInModule(module));
    worklist.emplace_back(req, substReq, ParentConditionalConformances{});
  }

  while (!worklist.empty()) {
    const auto item = worklist.pop_back_val();

    auto req = item.Req;
    auto substReq = item.SubstReq;

    SmallVector<Requirement, 2> subReqs;
    switch (substReq.checkRequirement(subReqs, /*allowMissing=*/true)) {
    case CheckRequirementResult::Success:
      break;

    case CheckRequirementResult::ConditionalConformance: {
      assert(substReq.getKind() == RequirementKind::Conformance);

      auto reqsPath = item.Path;
      reqsPath.push_back({substReq.getFirstType(), substReq.getProtocolDecl()});

      for (auto subReq : llvm::reverse(subReqs))
        worklist.emplace_back(subReq, subReq, reqsPath);
      break;
    }

    case CheckRequirementResult::PackRequirement: {
      for (auto subReq : llvm::reverse(subReqs)) {
        // Note: we keep the original unsubstituted pack requirement here for
        // the diagnostic
        worklist.emplace_back(req, subReq, item.Path);
      }
      break;
    }

    case CheckRequirementResult::RequirementFailure:
      return CheckGenericArgumentsResult::createRequirementFailure(
          req, substReq, item.Path);

    case CheckRequirementResult::SubstitutionFailure:
      hadSubstFailure = true;
      break;
    }
  }

  if (hadSubstFailure)
    return CheckGenericArgumentsResult::createSubstitutionFailure();

  return CheckGenericArgumentsResult::createSuccess();
}

CheckGenericArgumentsResult::Kind TypeChecker::checkGenericArguments(
    ModuleDecl *module, ArrayRef<Requirement> requirements,
    TypeSubstitutionFn substitutions, SubstOptions options) {
  SmallVector<Requirement, 4> worklist;

  bool hadSubstFailure = false;

  for (auto req : requirements) {
    worklist.push_back(req.subst(substitutions,
                              LookUpConformanceInModule(module), options));
  }

  while (!worklist.empty()) {
    auto req = worklist.pop_back_val();
    switch (req.checkRequirement(worklist, /*allowMissing=*/true)) {
    case CheckRequirementResult::Success:
    case CheckRequirementResult::ConditionalConformance:
    case CheckRequirementResult::PackRequirement:
      break;

    case CheckRequirementResult::RequirementFailure:
      return CheckGenericArgumentsResult::RequirementFailure;

    case CheckRequirementResult::SubstitutionFailure:
      hadSubstFailure = true;
      break;
    }
  }

  if (hadSubstFailure)
    return CheckGenericArgumentsResult::SubstitutionFailure;

  return CheckGenericArgumentsResult::Success;
}

CheckGenericArgumentsResult::Kind TypeChecker::checkGenericArguments(
    ArrayRef<Requirement> requirements) {
  SmallVector<Requirement, 4> worklist(requirements.begin(), requirements.end());

  bool hadSubstFailure = false;

  while (!worklist.empty()) {
    auto req = worklist.pop_back_val();
    switch (req.checkRequirement(worklist, /*allowMissing=*/true)) {
    case CheckRequirementResult::Success:
    case CheckRequirementResult::ConditionalConformance:
    case CheckRequirementResult::PackRequirement:
      break;

    case CheckRequirementResult::RequirementFailure:
      return CheckGenericArgumentsResult::RequirementFailure;

    case CheckRequirementResult::SubstitutionFailure:
      hadSubstFailure = true;
      break;
    }
  }

  if (hadSubstFailure)
    return CheckGenericArgumentsResult::SubstitutionFailure;

  return CheckGenericArgumentsResult::Success;
}

Requirement
RequirementRequest::evaluate(Evaluator &evaluator,
                             WhereClauseOwner owner,
                             unsigned index,
                             TypeResolutionStage stage) const {
  auto &reqRepr = getRequirement();

  // Figure out the type resolution.
  TypeResolverContext context;
  if (reqRepr.getKind() == RequirementReprKind::SameType) {
    context = TypeResolverContext::SameTypeRequirement;
  } else {
    context = TypeResolverContext::GenericRequirement;
  }
  auto options = TypeResolutionOptions(context);
  if (reqRepr.isExpansionPattern())
    options |= TypeResolutionFlags::AllowPackReferences;
  if (owner.dc->isInSpecializeExtensionContext())
    options |= TypeResolutionFlags::AllowUsableFromInline;
  llvm::Optional<TypeResolution> resolution;
  switch (stage) {
  case TypeResolutionStage::Structural:
    resolution = TypeResolution::forStructural(owner.dc, options,
                                               /*unboundTyOpener*/ nullptr,
                                               /*placeholderHandler*/ nullptr,
                                               /*packElementOpener*/ nullptr);
    break;

  case TypeResolutionStage::Interface:
    resolution = TypeResolution::forInterface(owner.dc, options,
                                              /*unboundTyOpener*/ nullptr,
                                              /*placeholderHandler*/ nullptr,
                                              /*packElementOpener*/ nullptr);
    break;
  }

  switch (reqRepr.getKind()) {
  case RequirementReprKind::TypeConstraint: {
    Type subject = resolution->resolveType(reqRepr.getSubjectRepr());
    Type constraint = resolution->resolveType(reqRepr.getConstraintRepr());
    return Requirement(constraint->getClassOrBoundGenericClass()
                         ? RequirementKind::Superclass
                         : RequirementKind::Conformance,
                       subject, constraint);
  }

  case RequirementReprKind::SameType:
    return Requirement(RequirementKind::SameType,
                       resolution->resolveType(reqRepr.getFirstTypeRepr()),
                       resolution->resolveType(reqRepr.getSecondTypeRepr()));

  case RequirementReprKind::LayoutConstraint:
    return Requirement(RequirementKind::Layout,
                       resolution->resolveType(reqRepr.getSubjectRepr()),
                       reqRepr.getLayoutConstraint());
  }
  llvm_unreachable("unhandled kind");
}

Type StructuralTypeRequest::evaluate(Evaluator &evaluator,
                                     TypeAliasDecl *typeAlias) const {  
  TypeResolutionOptions options((typeAlias->getGenericParams()
                                     ? TypeResolverContext::GenericTypeAliasDecl
                                     : TypeResolverContext::TypeAliasDecl));

  // This can happen when code completion is attempted inside
  // of typealias underlying type e.g. `typealias F = () -> Int#^TOK^#`
  auto &ctx = typeAlias->getASTContext();
  auto underlyingTypeRepr = typeAlias->getUnderlyingTypeRepr();
  if (!underlyingTypeRepr) {
    typeAlias->setInvalid();
    return ErrorType::get(ctx);
  }

  const auto type =
      TypeResolution::forStructural(typeAlias, options,
                                    /*unboundTyOpener*/ nullptr,
                                    /*placeholderHandler*/ nullptr,
                                    /*packElementOpener*/ nullptr)
          .resolveType(underlyingTypeRepr);

  auto genericSig = typeAlias->getGenericSignature();
  SubstitutionMap subs;
  if (genericSig)
    subs = genericSig->getIdentitySubstitutionMap();

  Type parent;
  auto parentDC = typeAlias->getDeclContext();
  if (parentDC->isTypeContext())
    parent = parentDC->getSelfInterfaceType();
  return TypeAliasType::get(typeAlias, parent, subs, type);
}
