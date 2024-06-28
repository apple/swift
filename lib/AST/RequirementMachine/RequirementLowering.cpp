//===--- RequirementLowering.cpp - Requirement inference and desugaring ---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// The process of constructing a requirement machine from some input requirements
// can be summarized by the following diagram.
//
//     ------------------
//    / RequirementRepr / <-------- Generic parameter lists, 'where' clauses,
//    ------------------            and protocol definitions written in source
//             |                    start here:
//             |                    - InferredGenericSignatureRequest
//             |                    - RequirementSignatureRequest
//             v                    
// +-------------------------+            -------------------
// | Requirement realization | --------> / Sema diagnostics /
// +-------------------------+           -------------------
//             |       
//             |       -------------------------------------
//             |      / Function parameter/result TypeRepr /
//             |      -------------------------------------
//             |                       |
//             |                       v
//             |            +-----------------------+
//             |            | Requirement inference |
//             |            +-----------------------+
//             |                       |
//             |       +---------------+
//             v       v
//   ------------------------
//  / StructuralRequirement / <---- Minimization of a set of abstract
//  ------------------------        requirements internally by the compiler
//             |                    starts here:
//             |                    - AbstractGenericSignatureRequest
//             v
// +------------------------+           -------------------
// | Requirement desugaring | -------> / RequirementError /
// +------------------------+          -------------------
//             |
//             v
//       --------------
//      / Requirement /
//      --------------
//             |
//             v
//  +----------------------+
//  | Concrete contraction |
//  +----------------------+
//             |                       -------------------------
//             v                      / Existing RewriteSystem /
//       --------------               -------------------------
//      / Requirement /                           |
//      --------------                            v
//             |            +--------------------------------------------+
//             |            | Importing rules from protocol dependencies |
//             |            +--------------------------------------------+
//             |                                  |
//             |   +------------------------------+
//             |   |
//             v   v
//      +-------------+
//      | RuleBuilder | <--- Construction of a rewrite system to answer
//      +-------------+      queries about an already-minimized generic
//             |                   signature or connected component of protocol
//             v                   requirement signatures starts here:
//          -------                - RewriteContext::getRequirementMachine()
//         / Rule /
//         -------
//
// This file implements the "requirement realization", "requirement inference"
// and "requirement desugaring" steps above. Concrete contraction is implemented
// in ConcreteContraction.cpp. Building rewrite rules from desugared requirements
// is implemented in RuleBuilder.cpp.
//
// # Requirement realization and inference
//
// Requirement realization takes parsed representations of generic requirements,
// and converts them to StructuralRequirements:
//
// - RequirementReprs in 'where' clauses
// - TypeReprs in generic parameter and associated type inheritance clauses
// - TypeReprs of function parameters and results, for requirement inference
//
// Requirement inference is the language feature where requirements on type
// parameters are inferred from bound generic type applications. For example,
// in the following, 'T : Hashable' is not explicitly stated:
//
//    func foo<T>(_: Set<T>) {}
//
// The application of the bound generic type "Set<T>" requires that
// 'T : Hashable', from the generic signature of the declaration of 'Set'.
// Requirement inference, when performed, will introduce this requirement.
//
// Requirement realization calls into Sema' resolveType() and similar operations
// and emits diagnostics that way.
//
// # Requirement desugaring
//
// Requirements in 'where' clauses allow for some unneeded generality that we
// eliminate early. For example:
//
// - The right hand side of a protocol conformance requirement might be a
//   protocol composition.
//
// - Same-type requirements involving concrete types can take various forms:
//   a) Between a type parameter and a concrete type, eg. 'T == Int'.
//   b) Between a concrete type and a type parameter, eg. 'Int == T'.
//   c) Between two concrete types, eg 'Array<T> == Array<Int>'.
//
// 'Desugared requirements' take the following special form:
//
// - The subject type of a requirement is always a type parameter.
//
// - The right hand side of a conformance requirement is always a single
//   protocol.
//
// - A concrete same-type requirement is always between a type parameter and
//   a concrete type.
//
// The desugaring process eliminates requirements where both sides are
// concrete by evaluating them immediately, reporting an error if the
// requirement does not hold, or silently discarding it otherwise.
//
// Conformance requirements with protocol compositions on the right hand side
// are broken down into multiple conformance requirements.
//
// Same-type requirements where both sides are concrete are decomposed by
// walking the two concrete types in parallel. If there is a mismatch in the
// concrete structure, an error is recorded. If a mismatch involves a concrete
// type and a type parameter, a new same-type requirement is recorded.
//
// For example, in the above, 'Array<T> == Array<Int>' is desugared into the
// single requirement 'T == Int'.
//
// Finally, same-type requirements between a type parameter and concrete type
// are oriented so that the type parameter always appears on the left hand side.
//
// Requirement desugaring diagnoses errors by building a list of
// RequirementError values.
//
//===----------------------------------------------------------------------===//

#include "RequirementLowering.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Requirement.h"
#include "swift/AST/RequirementSignature.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeMatcher.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/Assertions.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SetVector.h"
#include "Diagnostics.h"
#include "RewriteContext.h"
#include "NameLookup.h"

using namespace swift;
using namespace rewriting;

//
// Requirement desugaring
//

/// Desugar a same-type requirement that possibly has concrete types on either
/// side into a series of same-type and concrete-type requirements where the
/// left hand side is always a type parameter.
static void desugarSameTypeRequirement(
                                  Requirement req,
                                  SourceLoc loc,
                                  SmallVectorImpl<Requirement> &result,
                                  SmallVectorImpl<InverseRequirement> &inverses,
                                  SmallVectorImpl<RequirementError> &errors) {
  class Matcher : public TypeMatcher<Matcher> {
    SourceLoc loc;
    SmallVectorImpl<Requirement> &result;
    SmallVectorImpl<RequirementError> &errors;
    SmallVector<Position, 2> stack;

  public:
    explicit Matcher(SourceLoc loc,
                     SmallVectorImpl<Requirement> &result,
                     SmallVectorImpl<RequirementError> &errors)
      : loc(loc), result(result), errors(errors) {}

    bool alwaysMismatchTypeParameters() const { return true; }

    void pushPosition(Position pos) {
      stack.push_back(pos);
    }

    void popPosition(Position pos) {
      ASSERT(stack.back() == pos);
      stack.pop_back();
    }

    Position getPosition() const {
      if (stack.empty()) return Position::Type;
      return stack.back();
    }

    bool mismatch(TypeBase *firstType, TypeBase *secondType,
                  Type sugaredFirstType) {
      RequirementKind kind;
      switch (getPosition()) {
      case Position::Type:
        kind = RequirementKind::SameType;
        break;
      case Position::Shape:
        kind = RequirementKind::SameShape;
        break;
      }

      // If one side is a parameter pack, this is a same-element requirement, which
      // is not yet supported.
      if (firstType->isParameterPack() != secondType->isParameterPack()) {
        errors.push_back(RequirementError::forSameElement(
            {kind, sugaredFirstType, secondType}, loc));
        return true;
      }

      if (firstType->isTypeParameter() && secondType->isTypeParameter()) {
        result.emplace_back(kind, sugaredFirstType, secondType);
        return true;
      }

      if (firstType->isTypeParameter()) {
        result.emplace_back(kind, sugaredFirstType, secondType);
        return true;
      }

      if (secondType->isTypeParameter()) {
        result.emplace_back(kind, secondType, sugaredFirstType);
        return true;
      }

      errors.push_back(RequirementError::forConflictingRequirement(
          {RequirementKind::SameType, sugaredFirstType, secondType}, loc));
      return true;
    }
  } matcher(loc, result, errors);

  (void) matcher.match(req.getFirstType(), req.getSecondType());
}

static void desugarSuperclassRequirement(
                                  Requirement req,
                                  SourceLoc loc,
                                  SmallVectorImpl<Requirement> &result,
                                  SmallVectorImpl<InverseRequirement> &inverses,
                                  SmallVectorImpl<RequirementError> &errors) {
  if (req.getFirstType()->isTypeParameter()) {
    result.push_back(req);
    return;
  }

  SmallVector<Requirement, 2> subReqs;

  switch (req.checkRequirement(subReqs)) {
  case CheckRequirementResult::Success:
  case CheckRequirementResult::PackRequirement:
    break;

  case CheckRequirementResult::RequirementFailure:
    errors.push_back(RequirementError::forInvalidRequirementSubject(req, loc));
    break;

  case CheckRequirementResult::SubstitutionFailure:
    break;

  case CheckRequirementResult::ConditionalConformance:
    llvm_unreachable("Unexpected CheckRequirementResult");
  }

  for (auto subReq : subReqs)
    desugarRequirement(subReq, loc, result, inverses, errors);
}

static void desugarLayoutRequirement(
                                  Requirement req,
                                  SourceLoc loc,
                                  SmallVectorImpl<Requirement> &result,
                                  SmallVectorImpl<InverseRequirement> &inverses,
                                  SmallVectorImpl<RequirementError> &errors) {
  if (req.getFirstType()->isTypeParameter()) {
    result.push_back(req);
    return;
  }

  SmallVector<Requirement, 2> subReqs;

  switch (req.checkRequirement(subReqs)) {
  case CheckRequirementResult::Success:
  case CheckRequirementResult::PackRequirement:
    break;

  case CheckRequirementResult::RequirementFailure:
    errors.push_back(RequirementError::forInvalidRequirementSubject(req, loc));
    break;

  case CheckRequirementResult::SubstitutionFailure:
    break;

  case CheckRequirementResult::ConditionalConformance:
    llvm_unreachable("Unexpected CheckRequirementResult");
  }

  for (auto subReq : subReqs)
    desugarRequirement(subReq, loc, result, inverses, errors);
}

/// Desugar a protocol conformance requirement by splitting up protocol
/// compositions on the right hand side into conformance and superclass
/// requirements.
static void desugarConformanceRequirement(
                                  Requirement req,
                                  SourceLoc loc,
                                  SmallVectorImpl<Requirement> &result,
                                  SmallVectorImpl<InverseRequirement> &inverses,
                                  SmallVectorImpl<RequirementError> &errors) {
  SmallVector<Requirement, 2> subReqs;

  auto constraintType = req.getSecondType();

  // Fast path.
  if (constraintType->is<ProtocolType>()) {
    if (req.getFirstType()->isTypeParameter()) {
      result.push_back(req);
      return;
    }

    // Check if the subject type actually conforms.
    switch (req.checkRequirement(subReqs, /*allowMissing=*/true)) {
    case CheckRequirementResult::Success:
    case CheckRequirementResult::PackRequirement:
    case CheckRequirementResult::ConditionalConformance:
      break;

    case CheckRequirementResult::RequirementFailure:
      errors.push_back(RequirementError::forInvalidRequirementSubject(req, loc));
      break;

    case CheckRequirementResult::SubstitutionFailure:
      break;
    }
  } else if (auto *paramType = constraintType->getAs<ParameterizedProtocolType>()) {
    subReqs.emplace_back(RequirementKind::Conformance, req.getFirstType(),
                         paramType->getBaseType());
    paramType->getRequirements(req.getFirstType(), subReqs);
  } else if (auto *compositionType = constraintType->getAs<ProtocolCompositionType>()) {
    if (compositionType->hasExplicitAnyObject()) {
      subReqs.emplace_back(RequirementKind::Layout, req.getFirstType(),
                           LayoutConstraint::getLayoutConstraint(
                             LayoutConstraintKind::Class));
    }

    for (auto memberType : compositionType->getMembers()) {
      subReqs.emplace_back(
          memberType->isConstraintType()
            ? RequirementKind::Conformance
            : RequirementKind::Superclass,
          req.getFirstType(), memberType);
    }

    // Check if the composition has any inverses.
    if (!compositionType->getInverses().empty()) {
      auto subject = req.getFirstType();

      if (!subject->isTypeParameter()) {
        // Only permit type-parameter subjects.
        errors.push_back(
            RequirementError::forInvalidRequirementSubject(req, loc));
      } else {
        // Record and desugar inverses.
        auto &ctx = req.getFirstType()->getASTContext();
        for (auto ip : compositionType->getInverses())
          inverses.push_back({req.getFirstType(),
                              ctx.getProtocol(getKnownProtocolKind(ip)),
                              loc});
      }
    }
  }

  for (auto subReq : subReqs)
    desugarRequirement(subReq, loc, result, inverses, errors);
}

/// Diagnose shape requirements on non-pack types.
static void desugarSameShapeRequirement(
                                  Requirement req,
                                  SourceLoc loc,
                                  SmallVectorImpl<Requirement> &result,
                                  SmallVectorImpl<InverseRequirement> &inverses,
                                  SmallVectorImpl<RequirementError> &errors) {
  // For now, only allow shape requirements directly between pack types.
  if (!req.getFirstType()->isParameterPack() ||
      !req.getSecondType()->isParameterPack()) {
    errors.push_back(RequirementError::forInvalidShapeRequirement(
        req, loc));
  }

  result.emplace_back(RequirementKind::SameShape,
                      req.getFirstType(), req.getSecondType());
}

/// Convert a requirement where the subject type might not be a type parameter,
/// or the constraint type in the conformance requirement might be a protocol
/// composition, into zero or more "proper" requirements which can then be
/// converted into rewrite rules by the RuleBuilder.
void
swift::rewriting::desugarRequirement(
                                  Requirement req,
                                  SourceLoc loc,
                                  SmallVectorImpl<Requirement> &result,
                                  SmallVectorImpl<InverseRequirement> &inverses,
                                  SmallVectorImpl<RequirementError> &errors) {
  switch (req.getKind()) {
  case RequirementKind::SameShape:
    desugarSameShapeRequirement(req, loc, result, inverses, errors);
    break;

  case RequirementKind::Conformance:
    desugarConformanceRequirement(req, loc, result, inverses, errors);
    break;

  case RequirementKind::Superclass:
    desugarSuperclassRequirement(req, loc, result, inverses, errors);
    break;

  case RequirementKind::Layout:
    desugarLayoutRequirement(req, loc, result, inverses, errors);
    break;

  case RequirementKind::SameType:
    desugarSameTypeRequirement(req, loc, result, inverses, errors);
    break;
  }
}

void swift::rewriting::desugarRequirements(
                                  SmallVector<StructuralRequirement, 2> &reqs,
                                  SmallVectorImpl<InverseRequirement> &inverses,
                                  SmallVectorImpl<RequirementError> &errors) {
  SmallVector<StructuralRequirement, 2> result;
  for (auto req : reqs) {
    SmallVector<Requirement, 2> desugaredReqs;
    desugarRequirement(req.req, req.loc, desugaredReqs,
                       inverses, errors);

    for (auto desugaredReq : desugaredReqs)
      result.push_back({desugaredReq, req.loc});
  }

  std::swap(reqs, result);
}

//
// Requirement realization and inference.
//

static void realizeTypeRequirement(DeclContext *dc,
                                   Type subjectType, Type constraintType,
                                   SourceLoc loc,
                                   SmallVectorImpl<StructuralRequirement> &result,
                                   SmallVectorImpl<RequirementError> &errors) {
  // The GenericSignatureBuilder allowed the right hand side of a
  // conformance or superclass requirement to reference a protocol
  // typealias whose underlying type was a protocol or class.
  //
  // Since protocol typealiases resolve to DependentMemberTypes in
  // ::Structural mode, this relied on the GSB's "delayed requirements"
  // mechanism.
  //
  // The RequirementMachine does not have an equivalent, and cannot really
  // support that because we need to collect the protocols mentioned on
  // the right hand sides of conformance requirements ahead of time.
  //
  // However, we can support it in simple cases where the typealias is
  // defined in the protocol itself and is accessed as a member of 'Self'.
  if (auto *proto = dc->getSelfProtocolDecl()) {
    if (auto memberType = constraintType->getAs<DependentMemberType>()) {
      if (memberType->getBase()->isEqual(proto->getSelfInterfaceType())) {
        SmallVector<TypeDecl *, 1> result;
        lookupConcreteNestedType(proto, memberType->getName(), result);
        auto *typeDecl = findBestConcreteNestedType(result);
        if (auto *aliasDecl = dyn_cast_or_null<TypeAliasDecl>(typeDecl)) {
          constraintType = aliasDecl->getUnderlyingType();
        }
      }
    }
  }

  if (constraintType->isConstraintType()) {
    result.push_back({Requirement(RequirementKind::Conformance,
                                  subjectType, constraintType),
                      loc});
  } else if (constraintType->getClassOrBoundGenericClass()) {
    result.push_back({Requirement(RequirementKind::Superclass,
                                  subjectType, constraintType),
                      loc});
  } else {
    errors.push_back(
        RequirementError::forInvalidTypeRequirement(subjectType,
                                                    constraintType,
                                                    loc));
  }
}

namespace {

/// AST walker that infers requirements from type representations.
struct InferRequirementsWalker : public TypeWalker {
  ModuleDecl *module;
  DeclContext *dc;
  SmallVectorImpl<StructuralRequirement> &reqs;

  explicit InferRequirementsWalker(ModuleDecl *module, DeclContext *dc,
                                   SmallVectorImpl<StructuralRequirement> &reqs)
      : module(module), dc(dc), reqs(reqs) {}

  Action walkToTypePre(Type ty) override {
    // Unbound generic types are the result of recovered-but-invalid code, and
    // don't have enough info to do any useful substitutions.
    if (ty->is<UnboundGenericType>())
      return Action::Stop;

    if (!ty->hasTypeParameter())
      return Action::SkipNode;

    return Action::Continue;
  }

  Action walkToTypePost(Type ty) override {
    // Skip `Sendable` conformance requirements that are inferred from
    // `@preconcurrency` declarations.
    auto skipRequirement = [&](Requirement req, Decl *fromDecl) {
      if (!fromDecl->preconcurrency())
        return false;

      // If this decl is `@preconcurrency`, include concurrency
      // requirements. The explicit annotation directly on the decl
      // will still exclude `Sendable` requirements from ABI.
      auto *decl = dc->getAsDecl();
      if (!decl || decl->preconcurrency())
        return false;

      return (req.getKind() == RequirementKind::Conformance &&
          req.getProtocolDecl()->isSpecificProtocol(KnownProtocolKind::Sendable));
    };

    // Infer from generic typealiases.
    if (auto typeAlias = dyn_cast<TypeAliasType>(ty.getPointer())) {
      auto decl = typeAlias->getDecl();
      auto subMap = typeAlias->getSubstitutionMap();
      for (const auto &rawReq : decl->getGenericSignature().getRequirements()) {
        if (skipRequirement(rawReq, decl))
          continue;

        reqs.push_back({rawReq.subst(subMap), SourceLoc()});
      }

      return Action::Continue;
    }

    // Infer same-length requirements between pack references that
    // are expanded in parallel.
    if (auto packExpansion = ty->getAs<PackExpansionType>()) {
      // Get all pack parameters referenced from the pattern.
      SmallVector<Type, 2> packReferences;
      packExpansion->getPatternType()->getTypeParameterPacks(packReferences);

      auto countType = packExpansion->getCountType();
      for (auto pack : packReferences)
        reqs.push_back({Requirement(RequirementKind::SameShape, countType, pack),
                        SourceLoc()});
    }

    // Infer requirements from `@differentiable` function types.
    // For all non-`@noDerivative` parameter and result types:
    // - `@differentiable`, `@differentiable(_forward)`, or
    //   `@differentiable(reverse)`: add `T: Differentiable` requirement.
    // - `@differentiable(_linear)`: add
    //   `T: Differentiable`, `T == T.TangentVector` requirements.
    if (auto *fnTy = ty->getAs<AnyFunctionType>()) {
      // Add a new conformance constraint for a fixed protocol.
      auto addConformanceConstraint = [&](Type type, ProtocolDecl *protocol) {
        reqs.push_back({Requirement(RequirementKind::Conformance, type,
                                    protocol->getDeclaredInterfaceType()),
                        SourceLoc()});
      };

      auto &ctx = module->getASTContext();
      auto *differentiableProtocol =
          ctx.getProtocol(KnownProtocolKind::Differentiable);
      if (differentiableProtocol && fnTy->isDifferentiable()) {
        auto addSameTypeConstraint = [&](Type firstType,
                                         AssociatedTypeDecl *assocType) {
          auto secondType = assocType->getDeclaredInterfaceType()
              ->castTo<DependentMemberType>()
              ->substBaseType(module, firstType);
          reqs.push_back({Requirement(RequirementKind::SameType,
                                      firstType, secondType),
                          SourceLoc()});
        };
        auto *tangentVectorAssocType =
            differentiableProtocol->getAssociatedType(ctx.Id_TangentVector);
        auto addRequirements = [&](Type type, bool isLinear) {
          addConformanceConstraint(type, differentiableProtocol);
          if (isLinear)
            addSameTypeConstraint(type, tangentVectorAssocType);
        };
        auto constrainParametersAndResult = [&](bool isLinear) {
          for (auto &param : fnTy->getParams())
            if (!param.isNoDerivative())
              addRequirements(param.getPlainType(), isLinear);
          addRequirements(fnTy->getResult(), isLinear);
        };
        // Add requirements.
        constrainParametersAndResult(fnTy->getDifferentiabilityKind() ==
                                     DifferentiabilityKind::Linear);
      }

      // Infer that the thrown error type of a function type conforms to Error.
      if (auto thrownError = fnTy->getThrownError()) {
        if (auto errorProtocol = ctx.getErrorDecl()) {
          addConformanceConstraint(thrownError, errorProtocol);
        }
      }
    }

    if (!ty->isSpecialized())
      return Action::Continue;

    // Infer from generic nominal types.
    auto decl = ty->getAnyNominal();
    if (!decl) return Action::Continue;

    auto genericSig = decl->getGenericSignature();
    if (!genericSig)
      return Action::Continue;

    /// Retrieve the substitution.
    auto subMap = ty->getContextSubstitutionMap(module, decl);

    // Handle the requirements.
    // FIXME: Inaccurate TypeReprs.
    for (const auto &rawReq : genericSig.getRequirements()) {
      if (skipRequirement(rawReq, decl))
        continue;

      reqs.push_back({rawReq.subst(subMap), SourceLoc()});
    }

    return Action::Continue;
  }
};

}

/// Infer requirements from applications of BoundGenericTypes to type
/// parameters. For example, given a function declaration
///
///     func union<T>(_ x: Set<T>, _ y: Set<T>)
///
/// We automatically infer 'T : Hashable' from the fact that 'struct Set'
/// declares a Hashable requirement on its generic parameter.
void swift::rewriting::inferRequirements(
    Type type, ModuleDecl *module, DeclContext *dc,
    SmallVectorImpl<StructuralRequirement> &result) {
  if (!type)
    return;

  InferRequirementsWalker walker(module, dc, result);
  type.walk(walker);
}

/// Perform requirement inference from the type representations in the
/// requirement itself (eg, `T == Set<U>` infers `U: Hashable`).
void swift::rewriting::realizeRequirement(
    DeclContext *dc,
    Requirement req, RequirementRepr *reqRepr,
    bool shouldInferRequirements,
    SmallVectorImpl<StructuralRequirement> &result,
    SmallVectorImpl<RequirementError> &errors) {
  auto loc = (reqRepr ? reqRepr->getSeparatorLoc() : SourceLoc());
  auto *moduleForInference = dc->getParentModule();

  switch (req.getKind()) {
  case RequirementKind::SameShape:
    llvm_unreachable("Same-shape requirement not supported here");

  case RequirementKind::Superclass:
  case RequirementKind::Conformance: {
    auto firstType = req.getFirstType();
    auto secondType = req.getSecondType();

    if (shouldInferRequirements) {
      inferRequirements(firstType, moduleForInference, dc, result);
      inferRequirements(secondType, moduleForInference, dc, result);
    }

    realizeTypeRequirement(dc, firstType, secondType, loc, result, errors);
    break;
  }

  case RequirementKind::Layout: {
    if (shouldInferRequirements) {
      auto firstType = req.getFirstType();
      inferRequirements(firstType, moduleForInference, dc, result);
    }

    result.push_back({req, loc});
    break;
  }

  case RequirementKind::SameType: {
    if (shouldInferRequirements) {
      auto firstType = req.getFirstType();
      inferRequirements(firstType, moduleForInference, dc, result);

      auto secondType = req.getSecondType();
      inferRequirements(secondType, moduleForInference, dc, result);
    }

    result.push_back({req, loc});
    break;
  }
  }
}

void swift::rewriting::applyInverses(
    ASTContext &ctx,
    ArrayRef<Type> gps,
    ArrayRef<InverseRequirement> inverseList,
    SmallVectorImpl<StructuralRequirement> &result,
    SmallVectorImpl<RequirementError> &errors) {

  // No inverses to even validate.
  if (inverseList.empty())
    return;

  const bool allowInverseOnAssocType =
      ctx.LangOpts.hasFeature(Feature::SuppressedAssociatedTypes);

  // Summarize the inverses and diagnose ones that are incorrect.
  llvm::DenseMap<CanType, InvertibleProtocolSet> inverses;
  for (auto inverse : inverseList) {
    auto canSubject = inverse.subject->getCanonicalType();

    // Inverses on associated types are experimental.
    if (!allowInverseOnAssocType && canSubject->is<DependentMemberType>()) {
      // Special exception: allow if we're building the stdlib.
      if (!ctx.MainModule->isStdlibModule()) {
        errors.push_back(RequirementError::forInvalidInverseSubject(inverse));
        continue;
      }
    }

    // Noncopyable checking support for parameter packs is not implemented yet.
    if (canSubject->isParameterPack()) {
      errors.push_back(RequirementError::forInvalidInverseSubject(inverse));
      continue;
    }

    // WARNING: possible quadratic behavior, but should be OK in practice.
    auto notInScope = llvm::none_of(gps, [=](Type t) {
      return t->getCanonicalType() == canSubject;
    });

    // If the inverse is on a subject that wasn't permitted by our caller, then
    // remove and diagnose as an error. This can happen when an inner context
    // has a constraint on some outer generic parameter, e.g.,
    //
    //     protocol P {
    //       func f() where Self: ~Copyable
    //     }
    //
    if (notInScope) {
      errors.push_back(
          RequirementError::forInvalidInverseOuterSubject(inverse));
      continue;
    }

    auto state = inverses.getOrInsertDefault(canSubject);

    // Check if this inverse has already been seen.
    auto inverseKind = inverse.getKind();
    if (state.contains(inverseKind))
      continue;

    state.insert(inverseKind);
    inverses[canSubject] = state;
  }

  // Fast-path: if there are no valid inverses, then there are no requirements
  // to be removed.
  if (inverses.empty())
    return;

  // Scan the structural requirements and cancel out any inferred requirements
  // based on the inverses we saw.
  result.erase(llvm::remove_if(result, [&](StructuralRequirement structReq) {
    auto req = structReq.req;

    if (req.getKind() != RequirementKind::Conformance)
      return false;

    // Only consider requirements involving an invertible protocol.
    auto proto = req.getProtocolDecl()->getInvertibleProtocolKind();
    if (!proto)
      return false;

    // See if this subject is in-scope.
    auto subject = req.getFirstType()->getCanonicalType();
    auto result = inverses.find(subject);
    if (result == inverses.end())
      return false;

    // We now have found the inferred constraint 'Subject : Proto'.
    // So, remove it if we have recorded a 'Subject : ~Proto'.
    auto recordedInverses = result->getSecond();
    return recordedInverses.contains(*proto);
  }), result.end());
}

/// Collect structural requirements written in the inheritance clause of an
/// AssociatedTypeDecl, GenericTypeParamDecl, or ProtocolDecl.
void swift::rewriting::realizeInheritedRequirements(
    TypeDecl *decl, Type type, bool shouldInferRequirements,
    SmallVectorImpl<StructuralRequirement> &result,
    SmallVectorImpl<RequirementError> &errors) {
  auto inheritedTypes = decl->getInherited();
  auto *dc = decl->getInnermostDeclContext();
  auto *moduleForInference = dc->getParentModule();

  for (auto index : inheritedTypes.getIndices()) {
    Type inheritedType =
        inheritedTypes.getResolvedType(index, TypeResolutionStage::Structural);

    if (!inheritedType) continue;

    if (shouldInferRequirements) {
      inferRequirements(inheritedType, moduleForInference,
                        decl->getInnermostDeclContext(), result);
    }

    auto *typeRepr = inheritedTypes.getTypeRepr(index);
    SourceLoc loc = (typeRepr ? typeRepr->getStartLoc() : SourceLoc());

    realizeTypeRequirement(dc, type, inheritedType, loc, result, errors);
  }

  // Also check for `SynthesizedProtocolAttr`s with additional constraints added
  // by ClangImporter. This is how imported protocols are marked `Sendable`
  // without changing their inheritance lists.
  auto attrs = decl->getAttrs().getAttributes<SynthesizedProtocolAttr>();
  for (auto attr : attrs) {
    auto inheritedType = attr->getProtocol()->getDeclaredType();
    auto loc = attr->getLocation();

    realizeTypeRequirement(dc, type, inheritedType, loc, result, errors);
  }
}

/// StructuralRequirementsRequest realizes all the user-written requirements
/// on the associated type declarations inside of a protocol.
///
/// This request is invoked by RequirementSignatureRequest for each protocol
/// in the connected component.
ArrayRef<StructuralRequirement>
StructuralRequirementsRequest::evaluate(Evaluator &evaluator,
                                        ProtocolDecl *proto) const {
  ASSERT(!proto->hasLazyRequirementSignature());

  SmallVector<StructuralRequirement, 2> result;
  SmallVector<RequirementError, 2> errors;
  SmallVector<InverseRequirement> inverses;

  SmallVector<Type, 4> needsDefaultRequirements;
  needsDefaultRequirements.push_back(proto->getSelfInterfaceType());
  for (auto *assocTypeDecl : proto->getAssociatedTypeMembers())
    needsDefaultRequirements.push_back(assocTypeDecl->getDeclaredInterfaceType());

  auto &ctx = proto->getASTContext();
  auto selfTy = proto->getSelfInterfaceType();

  unsigned errorCount = errors.size();
  realizeInheritedRequirements(proto, selfTy,
                               /*inferRequirements=*/false,
                               result, errors);

  if (errors.size() > errorCount) {
    // Add requirements from inherited protocols, which are obtained via
    // getDirectlyInheritedNominalTypeDecls(). Normally this duplicates
    // the information found in the resolved types from the inheritance
    // clause, except when type resolution fails and returns an ErrorType.
    //
    // For example, in 'protocol P: Q & Blah', where 'Blah' does not exist,
    // the type 'Q & Blah' resolves to an ErrorType, while the simpler
    // mechanism in getDirectlyInheritedNominalTypeDecls() still finds 'Q'.
    for (auto *inheritedProto : proto->getInheritedProtocols()) {
      result.push_back({
          Requirement(RequirementKind::Conformance,
                      selfTy, inheritedProto->getDeclaredInterfaceType()),
          SourceLoc()});
    }
  }

  // Add requirements from the protocol's own 'where' clause.
  WhereClauseOwner(proto).visitRequirements(TypeResolutionStage::Structural,
      [&](const Requirement &req, RequirementRepr *reqRepr) {
        realizeRequirement(proto, req, reqRepr,
                           /*inferRequirements=*/false,
                           result, errors);
        return false;
      });

  // Remaining logic is not relevant to @objc protocols.
  if (proto->isObjC()) {
    // @objc protocols have an implicit AnyObject requirement on Self.
    auto layout = LayoutConstraint::getLayoutConstraint(
        LayoutConstraintKind::Class, ctx);
    result.push_back({Requirement(RequirementKind::Layout, selfTy, layout),
                      SourceLoc()});

    desugarRequirements(result, inverses, errors);

    SmallVector<StructuralRequirement, 2> defaults;
    InverseRequirement::expandDefaults(ctx, needsDefaultRequirements, defaults);
    applyInverses(ctx, needsDefaultRequirements, inverses, defaults, errors);
    result.append(defaults);

    diagnoseRequirementErrors(ctx, errors,
                              AllowConcreteTypePolicy::NestedAssocTypes);

    return ctx.AllocateCopy(result);
  }

  // Add requirements for each associated type.
  llvm::SmallDenseSet<Identifier, 2> assocTypes;

  for (auto *assocTypeDecl : proto->getAssociatedTypeMembers()) {
    assocTypes.insert(assocTypeDecl->getName());

    // Add requirements placed directly on this associated type.
    auto assocType = assocTypeDecl->getDeclaredInterfaceType();
    realizeInheritedRequirements(assocTypeDecl, assocType,
                                 /*inferRequirements=*/false,
                                 result, errors);

    // Add requirements from this associated type's where clause.
    WhereClauseOwner(assocTypeDecl).visitRequirements(
        TypeResolutionStage::Structural,
        [&](const Requirement &req, RequirementRepr *reqRepr) {
          realizeRequirement(proto, req, reqRepr,
                             /*inferRequirements=*/false,
                             result, errors);
          return false;
        });
  }

  // Add requirements for each typealias.
  for (auto *decl : proto->getMembers()) {
    // Protocol typealiases are modeled as same-type requirements
    // where the left hand side is 'Self.X' for some unresolved
    // DependentMemberType X, and the right hand side is the
    // underlying type of the typealias.
    if (auto *typeAliasDecl = dyn_cast<TypeAliasDecl>(decl)) {
      if (typeAliasDecl->isGeneric())
        continue;

      // Ignore the typealias if we have an associated type with the same name
      // in the same protocol. This is invalid anyway, but it's just here to
      // ensure that we produce the same requirement signature on some tests
      // with -requirement-machine-protocol-signatures=verify.
      if (assocTypes.contains(typeAliasDecl->getName()))
        continue;

      // The structural type of a typealias will always be a TypeAliasType,
      // so unwrap it to avoid a requirement that prints as 'Self.T == Self.T'
      // in diagnostics.
      auto underlyingType = typeAliasDecl->getStructuralType();
      if (underlyingType->is<UnboundGenericType>())
        continue;

      auto subjectType = DependentMemberType::get(
          selfTy, typeAliasDecl->getName());
      Requirement req(RequirementKind::SameType, subjectType,
                      underlyingType);
      result.push_back({req, typeAliasDecl->getLoc()});
    }
  }

  desugarRequirements(result, inverses, errors);

  SmallVector<StructuralRequirement, 2> defaults;
  // We do not expand defaults for invertible protocols themselves.
  // HACK: We don't expand for Sendable either. This shouldn't be needed after
  // Swift 6.0
  if (!proto->getInvertibleProtocolKind()
      && !proto->isSpecificProtocol(KnownProtocolKind::Sendable))
    InverseRequirement::expandDefaults(ctx, needsDefaultRequirements, defaults);

  applyInverses(ctx, needsDefaultRequirements, inverses, defaults, errors);
  result.append(defaults);

  diagnoseRequirementErrors(ctx, errors,
                            AllowConcreteTypePolicy::NestedAssocTypes);

  return ctx.AllocateCopy(result);
}

/// This request primarily emits diagnostics about typealiases and associated
/// type declarations that override another associated type, and can better be
/// expressed as requirements in the 'where' clause.
///
/// It also implements a compatibility behavior where sometimes typealiases in
/// protocol extensions would introduce requirements in the
/// GenericSignatureBuilder, if they had the same name as an inherited
/// associated type.
ArrayRef<Requirement>
TypeAliasRequirementsRequest::evaluate(Evaluator &evaluator,
                                       ProtocolDecl *proto) const {
  // @objc protocols don't have associated types, so all of the below
  // becomes a trivial no-op.
  if (proto->isObjC())
    return ArrayRef<Requirement>();

  ASSERT(!proto->hasLazyRequirementSignature());

  SmallVector<Requirement, 2> result;
  SmallVector<RequirementError, 2> errors;
  SmallVector<InverseRequirement, 4> ignoredInverses;

  auto &ctx = proto->getASTContext();

  auto getStructuralType = [](TypeDecl *typeDecl) -> Type {
    if (auto typealias = dyn_cast<TypeAliasDecl>(typeDecl)) {
      // If the type alias was parsed from a user-written type representation,
      // request a structural type to avoid unnecessary type checking work.
      if (typealias->getUnderlyingTypeRepr() != nullptr)
        return typealias->getStructuralType();
      return typealias->getUnderlyingType();
    }

    return typeDecl->getDeclaredInterfaceType();
  };

  auto isSuitableType = [&](TypeDecl *req) -> bool {
    // Ignore generic types.
    if (auto genReq = dyn_cast<GenericTypeDecl>(req))
      if (genReq->isGeneric())
        return false;

    // Ignore typealiases with UnboundGenericType, since they
    // are like generic typealiases.
    if (auto *typeAlias = dyn_cast<TypeAliasDecl>(req))
      if (getStructuralType(typeAlias)->is<UnboundGenericType>())
        return false;

    return true;
  };

  // Collect all typealiases from inherited protocols recursively.
  llvm::MapVector<Identifier, TinyPtrVector<TypeDecl *>> inheritedTypeDecls;
  for (auto *inheritedProto : proto->getAllInheritedProtocols()) {
    for (auto req : inheritedProto->getMembers()) {
      if (auto *typeReq = dyn_cast<TypeDecl>(req)) {
        if (!isSuitableType(typeReq))
          continue;

        inheritedTypeDecls[typeReq->getName()].push_back(typeReq);
      }
    }
  }

  // An inferred same-type requirement between the two type declarations
  // within this protocol or a protocol it inherits.
  auto recordInheritedTypeRequirement = [&](TypeDecl *first, TypeDecl *second) {
    auto firstType = getStructuralType(first);
    auto secondType = getStructuralType(second);
    ASSERT(!firstType->is<UnboundGenericType>());
    ASSERT(!secondType->is<UnboundGenericType>());

    desugarRequirement(Requirement(RequirementKind::SameType, firstType, secondType),
                       SourceLoc(), result, ignoredInverses, errors);
  };

  // Local function to find the insertion point for the protocol's "where"
  // clause, as well as the string to start the insertion ("where" or ",");
  auto getProtocolWhereLoc = [&]() -> Located<const char *> {
    // Already has a trailing where clause.
    if (auto trailing = proto->getTrailingWhereClause())
      return { ", ", trailing->getRequirements().back().getSourceRange().End };

    // Inheritance clause.
    return { " where ", proto->getInherited().getEndLoc() };
  };

  // Retrieve the set of requirements that a given associated type declaration
  // produces, in the form that would be seen in the where clause.
  const auto getAssociatedTypeReqs = [&](const AssociatedTypeDecl *assocType,
                                         const char *start) {
    std::string result;
    {
      llvm::raw_string_ostream out(result);
      out << start;
      llvm::interleave(
          assocType->getInherited().getEntries(),
          [&](TypeLoc inheritedType) {
            out << assocType->getName() << ": ";
            if (auto inheritedTypeRepr = inheritedType.getTypeRepr())
              inheritedTypeRepr->print(out);
            else
              inheritedType.getType().print(out);
          },
          [&] { out << ", "; });

      if (const auto whereClause = assocType->getTrailingWhereClause()) {
        if (!assocType->getInherited().empty())
          out << ", ";

        whereClause->print(out, /*printWhereKeyword*/false);
      }
    }
    return result;
  };

  // Retrieve the requirement that a given typealias introduces when it
  // overrides an inherited associated type with the same name, as a string
  // suitable for use in a where clause.
  auto getConcreteTypeReq = [&](TypeDecl *type, const char *start) {
    std::string result;
    {
      llvm::raw_string_ostream out(result);
      out << start;
      out << type->getName() << " == ";
      if (auto typealias = dyn_cast<TypeAliasDecl>(type)) {
        if (auto underlyingTypeRepr = typealias->getUnderlyingTypeRepr())
          underlyingTypeRepr->print(out);
        else
          typealias->getUnderlyingType().print(out);
      } else {
        type->print(out);
      }
    }
    return result;
  };

  for (auto assocTypeDecl : proto->getAssociatedTypeMembers()) {
    // Check whether we inherited any types with the same name.
    auto knownInherited =
      inheritedTypeDecls.find(assocTypeDecl->getName());
    if (knownInherited == inheritedTypeDecls.end()) continue;

    bool shouldWarnAboutRedeclaration =
      !assocTypeDecl->getAttrs().hasAttribute<NonOverrideAttr>() &&
      !assocTypeDecl->getAttrs().hasAttribute<OverrideAttr>() &&
      !assocTypeDecl->hasDefaultDefinitionType() &&
      (!assocTypeDecl->getInherited().empty() ||
        assocTypeDecl->getTrailingWhereClause() ||
        ctx.LangOpts.WarnImplicitOverrides);
    for (auto inheritedType : knownInherited->second) {
      // If we have inherited associated type...
      if (auto inheritedAssocTypeDecl =
            dyn_cast<AssociatedTypeDecl>(inheritedType)) {
        // Complain about the first redeclaration.
        if (shouldWarnAboutRedeclaration) {
          auto inheritedFromProto = inheritedAssocTypeDecl->getProtocol();
          auto fixItWhere = getProtocolWhereLoc();
          ctx.Diags.diagnose(assocTypeDecl,
                             diag::inherited_associated_type_redecl,
                             assocTypeDecl->getName(),
                             inheritedFromProto->getDeclaredInterfaceType())
            .fixItInsertAfter(
                      fixItWhere.Loc,
                      getAssociatedTypeReqs(assocTypeDecl, fixItWhere.Item))
            .fixItRemove(assocTypeDecl->getSourceRange());

          ctx.Diags.diagnose(inheritedAssocTypeDecl, diag::decl_declared_here,
                             inheritedAssocTypeDecl);

          shouldWarnAboutRedeclaration = false;
        }

        continue;
      }

      // We inherited a type; this associated type will be identical
      // to that typealias.
      auto inheritedOwningDecl =
          inheritedType->getDeclContext()->getSelfNominalTypeDecl();
      ctx.Diags.diagnose(assocTypeDecl,
                         diag::associated_type_override_typealias,
                         assocTypeDecl->getName(),
                         inheritedOwningDecl->getDescriptiveKind(),
                         inheritedOwningDecl->getDeclaredInterfaceType());

      recordInheritedTypeRequirement(assocTypeDecl, inheritedType);
    }

    inheritedTypeDecls.erase(knownInherited);
  }

  // Check all remaining inherited type declarations to determine if
  // this protocol has a non-associated-type type with the same name.
  inheritedTypeDecls.remove_if(
    [&](const std::pair<Identifier, TinyPtrVector<TypeDecl *>> &inherited) {
      const auto name = inherited.first;
      for (auto found : proto->lookupDirect(name)) {
        // We only want concrete type declarations.
        auto typeReq = dyn_cast<TypeDecl>(found);
        if (!typeReq || isa<AssociatedTypeDecl>(typeReq)) continue;

        // Ignore nominal types. They're always invalid declarations.
        if (isa<NominalTypeDecl>(typeReq))
          continue;

        // Ignore generic type aliases.
        if (!isSuitableType(typeReq))
          continue;

        // ... from the same module as the protocol.
        if (typeReq->getModuleContext() != proto->getModuleContext()) continue;

        // Ignore types defined in constrained extensions; their equivalence
        // to the associated type would have to be conditional, which we cannot
        // model.
        if (auto ext = dyn_cast<ExtensionDecl>(typeReq->getDeclContext())) {
          // FIXME: isConstrainedExtension() can cause request cycles because it
          // computes a generic signature. getTrailingWhereClause() should be good
          // enough for protocol extensions, which cannot specify constraints in
          // any other way right now (eg, via requirement inference or by
          // extending a bound generic type).
          //
          // FIXME: Protocol extensions with noncopyable generics can!
          if (ext->getTrailingWhereClause()) continue;
        }

        // We found something.
        bool shouldWarnAboutRedeclaration = true;

        for (auto inheritedType : inherited.second) {
          // If we have inherited associated type...
          if (auto inheritedAssocTypeDecl =
                dyn_cast<AssociatedTypeDecl>(inheritedType)) {
            // Infer a same-type requirement between the typealias' underlying
            // type and the inherited associated type.
            recordInheritedTypeRequirement(inheritedAssocTypeDecl, typeReq);

            // Warn that one should use where clauses for this.
            if (shouldWarnAboutRedeclaration) {
              auto inheritedFromProto = inheritedAssocTypeDecl->getProtocol();
              auto fixItWhere = getProtocolWhereLoc();
              ctx.Diags.diagnose(typeReq,
                                 diag::typealias_override_associated_type,
                                 name,
                                 inheritedFromProto->getDeclaredInterfaceType())
                .fixItInsertAfter(fixItWhere.Loc,
                                  getConcreteTypeReq(typeReq, fixItWhere.Item))
                .fixItRemove(typeReq->getSourceRange());
              ctx.Diags.diagnose(inheritedAssocTypeDecl, diag::decl_declared_here,
                                 inheritedAssocTypeDecl);

              shouldWarnAboutRedeclaration = false;
            }

            continue;
          }

          // Two typealiases that should be the same.
          recordInheritedTypeRequirement(inheritedType, typeReq);
        }

        // We can remove this entry.
        return true;
      }

      return false;
  });

  // Infer same-type requirements among inherited type declarations.
  for (auto &entry : inheritedTypeDecls) {
    if (entry.second.size() < 2) continue;

    auto firstDecl = entry.second.front();
    for (auto otherDecl : ArrayRef<TypeDecl *>(entry.second).slice(1)) {
      recordInheritedTypeRequirement(firstDecl, otherDecl);
    }
  }

  diagnoseRequirementErrors(ctx, errors,
                            AllowConcreteTypePolicy::NestedAssocTypes);

  return ctx.AllocateCopy(result);
}

ArrayRef<ProtocolDecl *>
ProtocolDependenciesRequest::evaluate(Evaluator &evaluator,
                                      ProtocolDecl *proto) const {
  auto &ctx = proto->getASTContext();
  llvm::SmallSetVector<ProtocolDecl *, 4> result;

  // If we have a serialized requirement signature, deserialize it and
  // look at conformance requirements.
  if (proto->hasLazyRequirementSignature()) {
    for (auto req : proto->getRequirementSignature().getRequirements()) {
      if (req.getKind() == RequirementKind::Conformance) {
        result.insert(req.getProtocolDecl());
      }
    }

    return ctx.AllocateCopy(result);
  }

  // Otherwise, we can't ask for the requirement signature, because
  // this request is used as part of *building* the requirement
  // signature. Look at the structural requirements instead.
  for (auto req : proto->getStructuralRequirements()) {
    if (req.req.getKind() == RequirementKind::Conformance)
      result.insert(req.req.getProtocolDecl());
  }

  return ctx.AllocateCopy(result);
}
