//===--- ConcreteTypeWitness.cpp - Nested types of concrete conformances --===//
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
// This file implements "nested type concretization", which introduces concrete
// type requirements on nested types of type parameters which are subject to
// both a protocol conformance and a concrete type (or superclass) requirement.
//
// This process runs during property map construction. It may introduce new
// rewrite rules, together with rewrite loops relating the new rules to existing
// rules via relations.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"
#include <algorithm>
#include <vector>
#include "PropertyMap.h"
#include "RequirementLowering.h"
#include "RuleBuilder.h"

using namespace swift;
using namespace rewriting;

void PropertyMap::concretizeNestedTypesFromConcreteParents() {
  for (auto *props : Entries) {
    if (props->getConformsTo().empty())
      continue;

    if (Debug.contains(DebugFlags::ConcretizeNestedTypes)) {
      if (props->isConcreteType() ||
          props->hasSuperclassBound()) {
        llvm::dbgs() << "^ Concretizing nested types of ";
        props->dump(llvm::dbgs());
        llvm::dbgs() << "\n";
      }
    }

    if (props->isConcreteType()) {
      if (Debug.contains(DebugFlags::ConcretizeNestedTypes)) {
        llvm::dbgs() << "- via concrete type requirement\n";
      }

      for (auto pair : props->ConcreteTypeRules) {
        concretizeNestedTypesFromConcreteParent(
            props->getKey(),
            RequirementKind::SameType,
            pair.second,
            pair.first.getConcreteType(),
            pair.first.getSubstitutions(),
            props->ConformsToRules,
            props->ConformsTo);
      }
    }

    if (props->hasSuperclassBound()) {
      if (Debug.contains(DebugFlags::ConcretizeNestedTypes)) {
        llvm::dbgs() << "- via superclass requirement\n";
      }

      const auto &superclassReq = props->getSuperclassRequirement();
      for (auto pair : superclassReq.SuperclassRules) {
        concretizeNestedTypesFromConcreteParent(
            props->getKey(),
            RequirementKind::Superclass,
            pair.second,
            pair.first.getConcreteType(),
            pair.first.getSubstitutions(),
            props->ConformsToRules,
            props->ConformsTo);
      }
    }
  }
}

/// Suppose a same-type requirement merges two property bags,
/// one of which has a conformance requirement to P and the other
/// one has a concrete type or superclass requirement.
///
/// If the concrete type or superclass conforms to P and P has an
/// associated type A, then we need to infer an equivalence between
/// T.[P:A] and whatever the type witness for 'A' is in the
/// concrete conformance.
///
/// For example, suppose we have a the following definitions,
///
///    protocol Q { associatedtype V }
///    protocol P { associatedtype A; associatedtype C }
///    struct Foo<A, B : Q> : P {
///      typealias C = B.V
///    }
///
/// together with the following property bag:
///
///    T => { conforms_to: [ P ], concrete: Foo<Int, τ_0_0> with <U> }
///
/// The type witness for A in the conformance Foo<Int, τ_0_0> : P is
/// the concrete type 'Int', which induces the following rule:
///
///    T.[P:A].[concrete: Int] => T.[P:A]
///
/// Whereas the type witness for B in the same conformance is the
/// abstract type 'τ_0_0.V', which via the substitutions <U> corresponds
/// to the term 'U.V', and therefore induces the following rule:
///
///    T.[P:B] => U.V
///
void PropertyMap::concretizeNestedTypesFromConcreteParent(
    Term key, RequirementKind requirementKind,
    unsigned concreteRuleID,
    CanType concreteType,
    ArrayRef<Term> substitutions,
    ArrayRef<unsigned> conformsToRules,
    ArrayRef<const ProtocolDecl *> conformsTo) {
  ASSERT(requirementKind == RequirementKind::SameType ||
         requirementKind == RequirementKind::Superclass);
  ASSERT(conformsTo.size() == conformsToRules.size());

  for (unsigned i : indices(conformsTo)) {
    auto *proto = conformsTo[i];
    unsigned conformanceRuleID = conformsToRules[i];

    // If we've already processed this pair of rules, record the conformance
    // and move on.
    //
    // This occurs when a pair of rules are inherited from the property map
    // entry for this key's suffix.
    if (!checkRulePairOnce(concreteRuleID, conformanceRuleID))
      continue;

    // FIXME: Either remove the ModuleDecl entirely from conformance lookup,
    // or pass the correct one down in here.
    auto *module = proto->getParentModule();

    // For conformance to 'Sendable', allow synthesis of a missing conformance
    // if the requirement is a concrete type requirement, that is, if we're
    // looking at a signature of the form 'T == Foo, T : Sendable'.
    //
    // Otherwise, we have a superclass requirement, like 'T : C, T : Sendable'.
    // Don't synthesize the conformance in this case since dropping
    // 'T : Sendable' would be incorrect; we want to ensure that we only admit
    // subclasses of 'C' which are 'Sendable'.
    bool allowMissing = (requirementKind == RequirementKind::SameType);

    auto conformance = module->lookupConformance(concreteType,
                                                 const_cast<ProtocolDecl *>(proto),
                                                 allowMissing);
    if (!allowMissing &&
        proto->isSpecificProtocol(KnownProtocolKind::Sendable) &&
        conformance.hasUnavailableConformance()) {
      conformance = ProtocolConformanceRef::forInvalid();
    }

    if (conformance.isInvalid()) {
      // For superclass rules, it is totally fine to have a signature like:
      //
      // protocol P {}
      // class C {}
      // <T  where T : P, T : C>
      //
      // There is no relation between P and C here.
      //
      // With concrete types, a missing conformance is a conflict.
      if (requirementKind == RequirementKind::SameType)
        System.recordConflict(conformanceRuleID, concreteRuleID);

      if (Debug.contains(DebugFlags::ConcretizeNestedTypes)) {
        llvm::dbgs() << "^^ " << concreteType << " does not conform to "
                     << proto->getName() << "\n";
      }

      continue;
    }

    auto concreteConformanceSymbol = Symbol::forConcreteConformance(
        concreteType, substitutions, proto, Context);

    recordConcreteConformanceRule(concreteRuleID, conformanceRuleID,
                                  requirementKind, concreteConformanceSymbol);

    // This is disabled by default because we fail to produce a convergent
    // rewrite system if the opaque archetype has infinitely-recursive
    // nested types. Fixing this requires a better representation for
    // concrete conformances in the rewrite system.
    if (conformance.isAbstract() &&
        !Context.getASTContext().LangOpts.EnableRequirementMachineOpaqueArchetypes) {
      if (Debug.contains(DebugFlags::ConcretizeNestedTypes)) {
        llvm::dbgs() << "^^ " << "Skipping abstract conformance of "
                     << concreteType << " to " << proto->getName() << "\n";
      }

      continue;
    }

    for (auto *assocType : proto->getAssociatedTypeMembers()) {
      concretizeTypeWitnessInConformance(key, requirementKind,
                                         concreteConformanceSymbol,
                                         conformance, assocType);
    }

    // We only infer conditional requirements in top-level generic signatures,
    // not in protocol requirement signatures.
    if (conformance.isConcrete() &&
        key.getRootProtocol() == nullptr)
      inferConditionalRequirements(conformance.getConcrete(), substitutions);
  }
}

void PropertyMap::concretizeTypeWitnessInConformance(
    Term key, RequirementKind requirementKind,
    Symbol concreteConformanceSymbol,
    ProtocolConformanceRef conformance,
    AssociatedTypeDecl *assocType) const {
  auto concreteType = concreteConformanceSymbol.getConcreteType();
  auto substitutions = concreteConformanceSymbol.getSubstitutions();

  auto *proto = assocType->getProtocol();
  ASSERT(proto == concreteConformanceSymbol.getProtocol());

  if (Debug.contains(DebugFlags::ConcretizeNestedTypes)) {
    llvm::dbgs() << "^^ " << "Looking up type witness for "
                 << proto->getName() << ":" << assocType->getName()
                 << " on " << concreteType << "\n";
  }

  CanType typeWitness;
  if (conformance.isConcrete()) {
    auto t = conformance.getConcrete()->getTypeWitness(assocType);

    if (!t) {
      if (Debug.contains(DebugFlags::ConcretizeNestedTypes)) {
        llvm::dbgs() << "^^ " << "Type witness for " << assocType->getName()
                     << " of " << concreteType << " could not be inferred\n";
      }

      t = ErrorType::get(concreteType);
    }

    typeWitness = t->getCanonicalType();
  } else if (conformance.isAbstract()) {
    auto archetype = concreteType->getAs<OpaqueTypeArchetypeType>();
    if (archetype == nullptr) {
      llvm::errs() << "Should only have an abstract conformance with an "
                   << "opaque archetype type\n";
      llvm::errs() << "Symbol: " << concreteConformanceSymbol << "\n";
      llvm::errs() << "Term: " << key << "\n";
      dump(llvm::errs());
      abort();
    }

    typeWitness = archetype->getNestedType(assocType)->getCanonicalType();
  } else if (conformance.isInvalid()) {
    typeWitness = CanType(ErrorType::get(Context.getASTContext()));
  }

  if (Debug.contains(DebugFlags::ConcretizeNestedTypes)) {
    llvm::dbgs() << "^^ " << "Type witness for " << assocType->getName()
                 << " of " << concreteType << " is " << typeWitness << "\n";
  }

  // Build the term T.[concrete: C : P].[P:X].
  MutableTerm subjectType(key);
  subjectType.add(concreteConformanceSymbol);
  subjectType.add(Symbol::forAssociatedType(proto, assocType->getName(),
                                            Context));

  MutableTerm constraintType;

  RewritePath path;

  constraintType = computeConstraintTermForTypeWitness(
      key, requirementKind, concreteType, typeWitness, subjectType,
      substitutions, path);

  ASSERT(!path.empty());
  (void) System.addRule(constraintType, subjectType, &path);
  if (Debug.contains(DebugFlags::ConcretizeNestedTypes)) {
    llvm::dbgs() << "^^ Induced rule " << constraintType
                 << " => " << subjectType << "\n";
  }
}

/// Given the key of a property bag known to have \p concreteType,
/// together with a \p typeWitness from a conformance on that concrete
/// type, return the right hand side of a rewrite rule to relate
/// \p subjectType with a term representing the type witness.
///
/// Suppose the key is T and the subject type is T.[P:A].
///
/// If the type witness is an abstract type U, this produces a rewrite
/// rule
///
///     T.[P:A] => U
///
/// If the type witness is a concrete type Foo, this produces a rewrite
/// rule
///
///     T.[P:A].[concrete: Foo] => T.[P:A]
///
/// However, this also tries to tie off recursion first using a heuristic.
///
/// If the type witness is fully concrete and we've already seen some
/// term V in the same domain with the same concrete type, we produce a
/// rewrite rule:
///
///        T.[P:A] => V
MutableTerm PropertyMap::computeConstraintTermForTypeWitness(
    Term key, RequirementKind requirementKind,
    CanType concreteType, CanType typeWitness,
    const MutableTerm &subjectType,
    ArrayRef<Term> substitutions,
    RewritePath &path) const {
  // If the type witness is abstract, introduce a same-type requirement
  // between two type parameters.
  if (typeWitness->isTypeParameter()) {
    // The type witness is a type parameter of the form τ_0_n.X.Y...Z,
    // where 'n' is an index into the substitution array.
    //
    // Add a rule:
    //
    // T.[concrete: C : P].[P:X] => S[n].X.Y...Z
    //
    // Where S[n] is the nth substitution term.

    auto result = Context.getRelativeTermForType(
        typeWitness, substitutions);

    unsigned relationID = System.recordRelation(
        Term::get(result, Context),
        Term::get(subjectType, Context));
    path.add(RewriteStep::forRelation(
        /*startOffset=*/0, relationID,
        /*inverse=*/false));

    return result;
  }

  // Compute the concrete type symbol [concrete: C.X].
  SmallVector<Term, 3> result;
  auto typeWitnessSchema =
      Context.getRelativeSubstitutionSchemaFromType(typeWitness, substitutions,
                                                    result);
  auto typeWitnessSymbol =
      Symbol::forConcreteType(typeWitnessSchema, result, Context);

  // If the type witness is completely concrete, check if one of our prefix
  // types has the same concrete type, and if so, introduce a same-type
  // requirement between the subject type and the prefix.
  if (!typeWitness->hasTypeParameter()) {
    auto begin = key.begin();
    auto end = key.end();

    while (begin != end) {
      MutableTerm prefix(begin, end);
      if (auto *props = lookUpProperties(prefix)) {
        if (props->isConcreteType() &&
            props->getConcreteType() == typeWitness) {
          // Record a relation U.[concrete: C.X] =>> U.V.[concrete: C : P].[P:X]
          // where U is the parent such that U.[concrete: C:X] => U.
          MutableTerm result(props->getKey());
          result.add(typeWitnessSymbol);

          unsigned relationID = System.recordRelation(
              Term::get(result, Context),
              Term::get(subjectType, Context));
          path.add(RewriteStep::forRelation(
              /*startOffset=*/0, relationID,
              /*inverse=*/false));

          if (Debug.contains(DebugFlags::ConcretizeNestedTypes)) {
             llvm::dbgs() << "^^ Type witness can re-use property bag of "
                          << result << "\n";
          }

          return result;
        }
      }

      --end;
    }
  }

  // Otherwise the type witness is concrete, but may contain type
  // parameters in structural position.

  auto concreteConformanceSymbol = *(subjectType.end() - 2);
  auto associatedTypeSymbol = *(subjectType.end() - 1);

  // Record the relation before simplifying typeWitnessSymbol below.
  unsigned concreteRelationID = System.recordConcreteTypeWitnessRelation(
      concreteConformanceSymbol,
      associatedTypeSymbol,
      typeWitnessSymbol);

  // Simplify the substitution terms in the type witness symbol.
  RewritePath substPath;
  auto differenceID = System.simplifySubstitutions(
      key, typeWitnessSymbol, /*map=*/this,
      &substPath);
  if (differenceID) {
    const auto &difference = System.getTypeDifference(*differenceID);
    ASSERT(difference.LHS == typeWitnessSymbol);
    typeWitnessSymbol = difference.RHS;
    substPath.invert();
  }

  // If it is equal to the parent type, introduce a same-type requirement
  // between the two parameters.
  if (requirementKind == RequirementKind::SameType &&
      typeWitnessSymbol.getConcreteType() == concreteType &&
      typeWitnessSymbol.getSubstitutions() == substitutions) {
    if (Debug.contains(DebugFlags::ConcretizeNestedTypes)) {
      llvm::dbgs() << "^^ Type witness is the same as the concrete type\n";
    }

    // Add a rule T.[concrete: C : P] => T.[concrete: C : P].[P:X].
    MutableTerm result(key);
    result.add(concreteConformanceSymbol);

    unsigned sameRelationID = System.recordSameTypeWitnessRelation(
        concreteConformanceSymbol,
        associatedTypeSymbol);

    // ([concrete: C : P] => [concrete: C : P].[P:X].[concrete: C])
    path.add(RewriteStep::forRelation(
        /*startOffset=*/key.size(), sameRelationID,
        /*inverse=*/true));

    // [concrete: C : P].[P:X].([concrete: C] => [concrete: C.X])
    path.append(substPath);

    // T.([concrete: C : P].[P:X].[concrete: C.X] => [concrete: C : P].[P:X])
    path.add(RewriteStep::forRelation(
        /*startOffset=*/key.size(), concreteRelationID,
        /*inverse=*/false));

    return result;
  }

  // Otherwise, add a concrete type requirement for the type witness.
  //
  // Add a rule:
  //
  // T.[concrete: C : P].[P:X].[concrete: C.X'] => T.[concrete: C : P].[P:X].
  //
  // Where C.X' is the canonical form of C.X.
  MutableTerm constraintType = subjectType;
  constraintType.add(typeWitnessSymbol);

  // T.[concrete: C : P].[P:X].([concrete: C.X'] => [concrete: C.X])
  path.append(substPath);

  // T.([concrete: C : P].[P:X].[concrete: C.X] => [concrete: C : P].[P:X])
  path.add(RewriteStep::forRelation(
      /*startOffset=*/key.size(), concreteRelationID,
      /*inverse=*/false));

  return constraintType;
}

void PropertyMap::recordConcreteConformanceRule(
    unsigned concreteRuleID,
    unsigned conformanceRuleID,
    RequirementKind requirementKind,
    Symbol concreteConformanceSymbol) const {
  const auto &concreteRule = System.getRule(concreteRuleID);
  const auto &conformanceRule = System.getRule(conformanceRuleID);

  RewritePath path;

  // We have a pair of rules T.[P] and T'.[concrete: C].
  // Either T == T', or T is a prefix of T', or T' is a prefix of T.
  //
  // Let T'' be the longest of T and T'.
  MutableTerm rhs(concreteRule.getRHS().size() > conformanceRule.getRHS().size()
                  ? concreteRule.getRHS()
                  : conformanceRule.getRHS());

  // First, apply the conformance rule in reverse to obtain T''.[P].
  path.add(RewriteStep::forRewriteRule(
      /*startOffset=*/rhs.size() - conformanceRule.getRHS().size(),
      /*endOffset=*/0,
      /*ruleID=*/conformanceRuleID,
      /*inverse=*/true));

  // Now, apply the concrete type rule in reverse to obtain T''.[concrete: C].[P].
  path.add(RewriteStep::forRewriteRule(
      /*startOffset=*/rhs.size() - concreteRule.getRHS().size(),
      /*endOffset=*/1,
      /*ruleID=*/concreteRuleID,
      /*inverse=*/true));

  // If T' is a suffix of T, prepend the prefix to the concrete type's
  // substitutions.
  auto concreteSymbol = *concreteRule.isPropertyRule();
  unsigned prefixLength = rhs.size() - concreteRule.getRHS().size();

  if (prefixLength > 0 &&
      !concreteConformanceSymbol.getSubstitutions().empty()) {
    path.add(RewriteStep::forPrefixSubstitutions(prefixLength, /*endOffset=*/1,
                                                 /*inverse=*/false));

    MutableTerm prefix(rhs.begin(), rhs.begin() + prefixLength);
    concreteSymbol = concreteSymbol.prependPrefixToConcreteSubstitutions(
        prefix, Context);
  }

  auto protocolSymbol = *conformanceRule.isPropertyRule();

  // Now, transform T''.[concrete: C].[P] into T''.[concrete: C].[concrete: C : P].
  unsigned relationID = System.recordConcreteConformanceRelation(
      concreteSymbol, protocolSymbol, concreteConformanceSymbol);

  path.add(RewriteStep::forRelation(
      /*startOffset=*/rhs.size(), relationID,
      /*inverse=*/false));

  // If T' is a suffix of T, prepend the prefix to the concrete type's
  // substitutions.
  if (prefixLength > 0 &&
      !concreteConformanceSymbol.getSubstitutions().empty()) {
    path.add(RewriteStep::forPrefixSubstitutions(prefixLength, /*endOffset=*/1,
                                                 /*inverse=*/true));
  }

  // Finally, apply the concrete type rule to obtain T''.[concrete: C : P].
  path.add(RewriteStep::forRewriteRule(
      /*startOffset=*/rhs.size() - concreteRule.getRHS().size(),
      /*endOffset=*/1,
      /*ruleID=*/concreteRuleID,
      /*inverse=*/false));

  MutableTerm lhs(rhs);
  lhs.add(concreteConformanceSymbol);

  // The path turns T'' (RHS) into T''.[concrete: C : P] (LHS), but we need
  // it to go in the other direction.
  path.invert();

  (void) System.addRule(std::move(lhs), std::move(rhs), &path);
}

/// If \p key is fixed to a concrete type and is also subject to a conformance
/// requirement, the concrete type might conform conditionally. In this case,
/// introduce rules for any conditional requirements in the given conformance.
void PropertyMap::inferConditionalRequirements(
    ProtocolConformance *concrete, ArrayRef<Term> substitutions) const {

  auto conditionalRequirements = concrete->getConditionalRequirements();

  if (Debug.contains(DebugFlags::ConditionalRequirements)) {
    if (conditionalRequirements.empty())
      llvm::dbgs() << "@@ No conditional requirements from ";
    else
      llvm::dbgs() << "@@ Inferring conditional requirements from ";

    llvm::dbgs() << concrete->getType() << " : ";
    llvm::dbgs() << concrete->getProtocol()->getName() << "\n";
  }

  if (conditionalRequirements.empty())
    return;

  SmallVector<Requirement, 2> desugaredRequirements;

  // FIXME: Do we need to diagnose these errors?
  SmallVector<RequirementError, 2> errors;
  SmallVector<InverseRequirement, 2> ignoredInverses;

  // First, desugar all conditional requirements.
  for (auto req : conditionalRequirements) {
    if (Debug.contains(DebugFlags::ConditionalRequirements)) {
      llvm::dbgs() << "@@@ Original requirement: ";
      req.dump(llvm::dbgs());
      llvm::dbgs() << "\n";
    }

    desugarRequirement(req, SourceLoc(), desugaredRequirements,
                       ignoredInverses, errors);
  }

  // Now, convert desugared conditional requirements to rules.

  // This will update System.getReferencedProtocols() with any new
  // protocols that were imported.
  RuleBuilder builder(Context, System.getReferencedProtocols());
  builder.initWithConditionalRequirements(desugaredRequirements,
                                          substitutions);

  ASSERT(builder.PermanentRules.empty());

  System.addRules(std::move(builder.ImportedRules),
                  std::move(builder.PermanentRules),
                  std::move(builder.RequirementRules));
}
