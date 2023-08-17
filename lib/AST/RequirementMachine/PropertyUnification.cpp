//===--- PropertyUnification.cpp - Rules added w/ building property map ---===//
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
// This file is the core of the property map construction algorithm.
//
// The primary entry point is the PropertyBag::addProperty() method, which
// unifies multiple layout, superclass and concrete type requirements on a
// single term.
//
// This unification can add new rewrite rules, as well as record rewrite loops
// relating existing rules together. Property map construction is iterated with
// the Knuth-Bendix completion procedure until fixed point.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Decl.h"
#include "swift/AST/LayoutConstraint.h"
#include "swift/AST/Types.h"
#include <algorithm>
#include <vector>
#include "PropertyMap.h"

using namespace swift;
using namespace rewriting;

/// Returns true if we have not processed this rule before.
bool PropertyMap::checkRuleOnce(unsigned ruleID) {
  return CheckedRules.insert(ruleID).second;
}

/// Returns true if we have not processed this pair of rules before.
bool PropertyMap::checkRulePairOnce(unsigned firstRuleID,
                                    unsigned secondRuleID) {
  return CheckedRulePairs.insert(
      std::make_pair(firstRuleID, secondRuleID)).second;
}

/// Given a key T, a rule (V.[p1] => V) where T == U.V, and a property [p2]
/// where [p1] < [p2], record a rule (T.[p2] => T) that is induced by
/// the original rule (V.[p1] => V).
///
/// This is used to define rewrite loops for relating pairs of rules where
/// one implies another:
///
/// - a more specific layout constraint implies a general layout constraint
/// - a more specific superclass bound implies a less specific superclass bound
/// - a superclass bound implies a layout constraint
/// - a concrete type that is a class implies a superclass bound
/// - a concrete type that is a class implies a layout constraint
///
static void recordRelation(Term key,
                           unsigned lhsRuleID,
                           Symbol rhsProperty,
                           RewriteSystem &system,
                           bool debug) {
  const auto &lhsRule = system.getRule(lhsRuleID);
  auto lhsProperty = lhsRule.getLHS().back();

  assert(key.size() >= lhsRule.getRHS().size());

  assert((lhsProperty.getKind() == Symbol::Kind::Layout &&
          rhsProperty.getKind() == Symbol::Kind::Layout) ||
         (lhsProperty.getKind() == Symbol::Kind::Superclass &&
          rhsProperty.getKind() == Symbol::Kind::Superclass) ||
         (lhsProperty.getKind() == Symbol::Kind::Superclass &&
          rhsProperty.getKind() == Symbol::Kind::Layout) ||
         (lhsProperty.getKind() == Symbol::Kind::ConcreteType &&
          rhsProperty.getKind() == Symbol::Kind::Superclass) ||
         (lhsProperty.getKind() == Symbol::Kind::ConcreteType &&
          rhsProperty.getKind() == Symbol::Kind::Layout));

  if (debug) {
    llvm::dbgs() << "%% Recording relation: ";
    llvm::dbgs() << lhsRule.getLHS() << " < " << rhsProperty << "\n";
  }

  unsigned relationID = system.recordRelation(lhsProperty, rhsProperty);

  // Build the following rewrite path:
  //
  //   U.(V => V.[p1]).[p2] ⊗ U.V.Relation([p1].[p2] => [p1]) ⊗ U.(V.[p1] => V).
  //
  RewritePath path;

  // Starting from U.V.[p2], apply the rule in reverse to get U.V.[p1].[p2].
  path.add(RewriteStep::forRewriteRule(
      /*startOffset=*/key.size() - lhsRule.getRHS().size(),
      /*endOffset=*/1,
      /*ruleID=*/lhsRuleID,
      /*inverse=*/true));

  // U.V.Relation([p1].[p2] => [p1]).
  path.add(RewriteStep::forRelation(/*startOffset=*/key.size(),
                                    relationID, /*inverse=*/false));

  // U.(V.[p1] => V).
  path.add(RewriteStep::forRewriteRule(
      /*startOffset=*/key.size() - lhsRule.getRHS().size(),
      /*endOffset=*/0,
      /*ruleID=*/lhsRuleID,
      /*inverse=*/false));

  // Add the rule (T.[p2] => T) with the above rewrite path.
  MutableTerm lhs(key);
  lhs.add(rhsProperty);

  MutableTerm rhs(key);

  (void) system.addRule(lhs, rhs, &path);
}

/// Given two property rules that conflict because no concrete type
/// can satisfy both, mark one or both rules conflicting.
///
/// The right hand side of one rule must be a suffix of the other
/// (in which case the longer of the two rules is conflicting) or
/// the right hand sides are equal (in which case both will be
/// conflicting).
void RewriteSystem::recordConflict(unsigned existingRuleID,
                                   unsigned newRuleID) {
  ConflictingRules.emplace_back(existingRuleID, newRuleID);

  auto &existingRule = getRule(existingRuleID);
  auto &newRule = getRule(newRuleID);

  if (Debug.contains(DebugFlags::ConflictingRules)) {
    llvm::dbgs() << "Conflicting rules:\n";
    llvm::dbgs() << "- " << existingRule << "\n";
    llvm::dbgs() << "- " << newRule << "\n";
  }

  // The identity conformance rule ([P].[P] => [P]) will conflict with
  // a concrete type requirement in an invalid protocol declaration
  // where 'Self' is constrained to a type that does not conform to
  // the protocol. This rule is permanent, so don't mark it as
  // conflicting in this case.
  if (!existingRule.isIdentityConformanceRule() &&
      existingRule.getRHS().size() >= newRule.getRHS().size())
    existingRule.markConflicting();
  if (!newRule.isIdentityConformanceRule() &&
      newRule.getRHS().size() >= existingRule.getRHS().size())
    newRule.markConflicting();
}

void PropertyMap::addConformanceProperty(
    Term key, Symbol property, unsigned ruleID) {
  auto *props = getOrCreateProperties(key);
  props->ConformsTo.push_back(property.getProtocol());
  props->ConformsToRules.push_back(ruleID);
}

void PropertyMap::addLayoutProperty(
    Term key, Symbol property, unsigned ruleID) {
  auto *props = getOrCreateProperties(key);
  bool debug = Debug.contains(DebugFlags::ConcreteUnification);

  auto newLayout = property.getLayoutConstraint();

  if (!props->Layout) {
    // If we haven't seen a layout requirement before, just record it.
    props->Layout = newLayout;
    props->LayoutRule = ruleID;
    return;
  }

  // Otherwise, compute the intersection.
  assert(props->LayoutRule.has_value());
  auto mergedLayout = props->Layout.merge(property.getLayoutConstraint());

  // If the intersection is invalid, we have a conflict.
  if (!mergedLayout->isKnownLayout()) {
    System.recordConflict(*props->LayoutRule, ruleID);
    return;
  }

  // If the intersection is equal to the existing layout requirement,
  // the new layout requirement is redundant.
  if (mergedLayout == props->Layout) {
    if (checkRulePairOnce(*props->LayoutRule, ruleID)) {
      recordRelation(key, *props->LayoutRule, property, System, debug);
    }

  // If the intersection is equal to the new layout requirement, the
  // existing layout requirement is redundant.
  } else if (mergedLayout == newLayout) {
    if (checkRulePairOnce(ruleID, *props->LayoutRule)) {
      auto oldProperty = System.getRule(*props->LayoutRule).getLHS().back();
      recordRelation(key, ruleID, oldProperty, System, debug);
    }

    props->LayoutRule = ruleID;
  } else {
    llvm::errs() << "Arbitrary intersection of layout requirements is "
                 << "supported yet\n";
    abort();
  }
}

/// Given a term T == U.V, an existing rule (V.[superclass: C] => V), and
/// a superclass declaration D of C, record a new rule (T.[superclass: C'] => T)
/// where C' is the substituted superclass type of C for D.
///
/// For example, suppose we have
///
///    class Derived : Base<Int> {}
///    class Base<T> {}
///
/// Given C == Derived and D == Base, then C' == Base<Int>.
void PropertyMap::recordSuperclassRelation(Term key,
                                           Symbol superclassType,
                                           unsigned superclassRuleID,
                                           const ClassDecl *otherClass) {
  auto derivedType = superclassType.getConcreteType();
  assert(otherClass->isSuperclassOf(derivedType->getClassOrBoundGenericClass()));

  auto baseType = derivedType->getSuperclassForDecl(otherClass)
      ->getCanonicalType();

  SmallVector<Term, 3> baseSubstitutions;
  auto baseSchema = Context.getRelativeSubstitutionSchemaFromType(
      baseType, superclassType.getSubstitutions(),
      baseSubstitutions);

  auto baseSymbol = Symbol::forSuperclass(baseSchema, baseSubstitutions,
                                          Context);

  bool debug = Debug.contains(DebugFlags::ConcreteUnification);
  recordRelation(key, superclassRuleID, baseSymbol, System, debug);
}

/// When a type parameter has two superclasses, we have to both unify the
/// type constructor arguments, and record the most derived superclass.
///
/// For example, if we have this setup:
///
///   class Base<T, T> {}
///   class Middle<U> : Base<T, T> {}
///   class Derived : Middle<Int> {}
///
///   T : Base<U, V>
///   T : Derived
///
/// The most derived superclass requirement is 'T : Derived'.
///
/// The corresponding superclass of 'Derived' is 'Base<Int, Int>', so we
/// unify the type constructor arguments of 'Base<U, V>' and 'Base<Int, Int>',
/// which generates two induced rules:
///
///   U.[concrete: Int] => U
///   V.[concrete: Int] => V
void PropertyMap::addSuperclassProperty(
    Term key, Symbol property, unsigned ruleID) {
  auto *props = getOrCreateProperties(key);
  bool debug = Debug.contains(DebugFlags::ConcreteUnification);

  const auto *superclassDecl = property.getConcreteType()
      ->getClassOrBoundGenericClass();
  assert(superclassDecl != nullptr);

  if (checkRuleOnce(ruleID)) {
    // A rule (T.[superclass: C] => T) induces a rule (T.[layout: L] => T),
    // where L is either AnyObject or _NativeObject.
    auto layout =
        LayoutConstraint::getLayoutConstraint(
          superclassDecl->getLayoutConstraintKind(),
          Context.getASTContext());
    auto layoutSymbol = Symbol::forLayout(layout, Context);

    recordRelation(key, ruleID, layoutSymbol, System, debug);
  }

  // If this is the first superclass requirement we've seen for this term,
  // just record it and we're done.
  if (!props->SuperclassDecl) {
    if (debug) {
      llvm::dbgs() << "% New superclass " << superclassDecl->getName()
                   << " for " << key << "\n";
    }

    props->SuperclassDecl = superclassDecl;

    assert(props->Superclasses.empty());
    auto &req = props->Superclasses[superclassDecl];

    assert(!req.SuperclassType.has_value());
    assert(req.SuperclassRules.empty());

    req.SuperclassType = property;
    req.SuperclassRules.emplace_back(property, ruleID);
    return;
  }

  if (debug) {
    llvm::dbgs() << "% New superclass " << superclassDecl->getName()
                 << " for " << key << " is ";
  }

  // Otherwise, we compare it against the existing superclass requirement.
  assert(!props->Superclasses.empty());

  if (superclassDecl == props->SuperclassDecl) {
    if (debug) {
      llvm::dbgs() << "equal to existing superclass\n";
    }

    // Perform concrete type unification at this level of the class
    // hierarchy.
    auto &req = props->Superclasses[superclassDecl];
    assert(req.SuperclassType.has_value());
    assert(!req.SuperclassRules.empty());

    unifyConcreteTypes(key, req.SuperclassType, req.SuperclassRules,
                       property, ruleID);

  } else if (superclassDecl->isSuperclassOf(props->SuperclassDecl)) {
    if (debug) {
      llvm::dbgs() << "less specific than existing superclass "
                   << props->SuperclassDecl->getName() << "\n";
    }

    // Record a relation where existing superclass implies the new superclass.
    const auto &existingReq = props->Superclasses[props->SuperclassDecl];
    for (auto pair : existingReq.SuperclassRules) {
      if (checkRulePairOnce(pair.second, ruleID)) {
        recordSuperclassRelation(key, pair.first, pair.second,
                                 superclassDecl);
      }
    }

    // Record the new rule at the less specific level of the class
    // hierarchy, performing concrete type unification if we've
    // already seen another rule at that level.
    auto &req = props->Superclasses[superclassDecl];

    unifyConcreteTypes(key, req.SuperclassType, req.SuperclassRules,
                       property, ruleID);

  } else if (props->SuperclassDecl->isSuperclassOf(superclassDecl)) {
    if (debug) {
      llvm::dbgs() << "more specific than existing superclass "
                   << props->SuperclassDecl->getName() << "\n";
    }

    // Record a relation where new superclass implies the existing superclass.
    const auto &existingReq = props->Superclasses[props->SuperclassDecl];
    for (auto pair : existingReq.SuperclassRules) {
      if (checkRulePairOnce(pair.second, ruleID)) {
        recordSuperclassRelation(key, property, ruleID,
                                 props->SuperclassDecl);
      }
    }

    // Record the new rule at the more specific level of the class
    // hierarchy.
    auto &req = props->Superclasses[superclassDecl];
    assert(!req.SuperclassType.has_value());
    assert(req.SuperclassRules.empty());

    req.SuperclassType = property;
    req.SuperclassRules.emplace_back(property, ruleID);

    props->SuperclassDecl = superclassDecl;

  } else {
    if (debug) {
      llvm::dbgs() << "not related to existing superclass "
                   << props->SuperclassDecl->getName() << "\n";
    }

    auto &req = props->Superclasses[props->SuperclassDecl];
    for (const auto &pair : req.SuperclassRules) {
      if (checkRulePairOnce(pair.second, ruleID))
        System.recordConflict(pair.second, ruleID);
    }
  }
}

/// Given two concrete type rules, record a rewrite loop relating them,
/// record induced rules, and relate the induced rules to the concrete
/// type rules.
void PropertyMap::unifyConcreteTypes(Term key,
                                     Symbol lhsProperty, unsigned lhsRuleID,
                                     Symbol rhsProperty, unsigned rhsRuleID) {
  if (!checkRulePairOnce(lhsRuleID, rhsRuleID))
    return;

  auto &lhsRule = System.getRule(lhsRuleID);
  auto &rhsRule = System.getRule(rhsRuleID);

  assert(rhsRule.getRHS() == key);

  bool debug = Debug.contains(DebugFlags::ConcreteUnification);

  if (debug) {
    llvm::dbgs() << "% Unifying " << lhsProperty
                 << " with " << rhsProperty << "\n";
  }

  llvm::Optional<unsigned> lhsDifferenceID;
  llvm::Optional<unsigned> rhsDifferenceID;

  bool conflict = System.computeTypeDifference(key,
                                               lhsProperty,
                                               rhsProperty,
                                               lhsDifferenceID,
                                               rhsDifferenceID);

  if (conflict) {
    // FIXME: Diagnose the conflict
    if (debug) {
      llvm::dbgs() << "%% Concrete type conflict\n";
    }
    System.recordConflict(lhsRuleID, rhsRuleID);
    return;
  }

  // Handle the case where (LHS ∧ RHS) is distinct from both LHS and RHS:
  // - First, record a new rule.
  // - Next, process the LHS -> (LHS ∧ RHS) difference.
  // - Finally, process the RHS -> (LHS ∧ RHS) difference.
  if (lhsDifferenceID && rhsDifferenceID) {
    const auto &lhsDifference = System.getTypeDifference(*lhsDifferenceID);
    const auto &rhsDifference = System.getTypeDifference(*rhsDifferenceID);
    assert(lhsDifference.RHS == rhsDifference.RHS);

    auto newProperty = lhsDifference.RHS;
    assert(newProperty == rhsDifference.RHS);

    MutableTerm rhsTerm(key);
    MutableTerm lhsTerm(key);
    lhsTerm.add(newProperty);

    // This rule does not need a rewrite path because it will be related
    // to the two existing rules by the processTypeDifference() calls below.
    System.addRule(lhsTerm, rhsTerm);

    // Recover a rewrite path from T to T.[LHS ∧ RHS].
    RewritePath path;
    System.buildRewritePathForJoiningTerms(rhsTerm, lhsTerm, &path);

    if (debug) {
      llvm::dbgs() << "%% Induced rule " << lhsTerm
                   << " == " << rhsTerm << "\n";
    }

    // Process LHS -> (LHS ∧ RHS).
    System.processTypeDifference(lhsDifference, *lhsDifferenceID,
                                 lhsRuleID, path);

    // Process RHS -> (LHS ∧ RHS).
    System.processTypeDifference(rhsDifference, *rhsDifferenceID,
                                 rhsRuleID, path);

    return;
  }

  // Handle the case where RHS == (LHS ∧ RHS) by processing LHS -> (LHS ∧ RHS).
  if (lhsDifferenceID) {
    assert(!rhsDifferenceID);

    const auto &lhsDifference = System.getTypeDifference(*lhsDifferenceID);
    assert(lhsProperty == lhsDifference.LHS);
    assert(rhsProperty == lhsDifference.RHS);

    // Build a rewrite path (T.[RHS] => T).
    RewritePath path;

    path.add(RewriteStep::forRewriteRule(
        /*startOffset=*/0, /*endOffset=*/0,
        /*ruleID=*/rhsRuleID, /*inverse=*/false));

    System.processTypeDifference(lhsDifference, *lhsDifferenceID,
                                 lhsRuleID, path);

    return;
  }

  // Handle the case where LHS == (LHS ∧ RHS) by processing RHS -> (LHS ∧ RHS).
  if (rhsDifferenceID) {
    assert(!lhsDifferenceID);

    const auto &rhsDifference = System.getTypeDifference(*rhsDifferenceID);
    assert(rhsProperty == rhsDifference.LHS);
    assert(lhsProperty == rhsDifference.RHS);

    // Build a rewrite path (T.[LHS] => T).
    RewritePath path;

    unsigned lhsPrefix = key.size() - lhsRule.getRHS().size();
    if (lhsPrefix > 0) {
      path.add(RewriteStep::forPrefixSubstitutions(
          lhsPrefix, /*endOffset=*/0, /*inverse=*/true));
    }

    path.add(RewriteStep::forRewriteRule(
        /*startOffset=*/lhsPrefix, /*endOffset=*/0,
        /*ruleID=*/lhsRuleID, /*inverse=*/false));

    System.processTypeDifference(rhsDifference, *rhsDifferenceID,
                                 rhsRuleID, path);

    return;
  }

  assert(lhsProperty == rhsProperty);

  if (lhsRuleID != rhsRuleID) {
    // If the rules are different but the concrete types are identical, then
    // the key is some term U.V, the existing rule is a rule of the form:
    //
    //    V.[concrete: G<...> with <X, Y>]
    //
    // and the new rule is a rule of the form:
    //
    //    U.V.[concrete: G<...> with <U.X, U.Y>]
    //
    // Record a loop relating the two rules via a rewrite step to prefix 'U' to
    // the symbol's substitutions.
    //
    // Since the new rule appears without context, it becomes redundant.
    RewritePath path;
    path.add(RewriteStep::forRewriteRule(
        /*startOffset=*/0, /*endOffset=*/0,
        /*ruleID=*/rhsRuleID, /*inverse=*/false));

    RewritePath unificationPath;
    System.buildRewritePathForUnifier(key, lhsRuleID, path, &unificationPath);
    System.recordRewriteLoop(MutableTerm(rhsRule.getLHS()), unificationPath);
  }
}

/// Relate a concrete type rule to all existing concrete type rules for this
/// key, and recompute the best concrete type property and rule seen so far.
///
/// Used by addSuperclassProperty() and addConcreteTypeProperty().
void PropertyMap::unifyConcreteTypes(
    Term key, llvm::Optional<Symbol> &bestProperty,
    llvm::SmallVectorImpl<std::pair<Symbol, unsigned>> &existingRules,
    Symbol property, unsigned ruleID) {
  // Unify this rule with all other concrete type rules we've seen so far,
  // to record rewrite loops relating the rules and their projections.
  for (auto pair : existingRules) {
    unifyConcreteTypes(key, pair.first, pair.second, property, ruleID);
  }

  // Record the new rule.
  existingRules.emplace_back(property, ruleID);

  // Now, figure out the best concrete type seen so far. If this is the
  // first rule, it's the best one.
  if (!bestProperty) {
    bestProperty = property;
    return;
  }

  // Otherwise, compute the meet with the existing best property.
  llvm::Optional<unsigned> lhsDifferenceID;
  llvm::Optional<unsigned> rhsDifferenceID;

  bool conflict = System.computeTypeDifference(key,
                                               *bestProperty, property,
                                               lhsDifferenceID,
                                               rhsDifferenceID);
  if (conflict)
    return;

  if (lhsDifferenceID) {
    bestProperty = System.getTypeDifference(*lhsDifferenceID).RHS;
  } else if (rhsDifferenceID) {
    bestProperty = System.getTypeDifference(*rhsDifferenceID).RHS;
  } else {
    assert(*bestProperty == property);
  }
}

/// When a type parameter has two concrete types, we have to unify the
/// type constructor arguments.
///
/// For example, suppose that we have two concrete same-type requirements:
///
///   T == Foo<X.Y, Z, String>
///   T == Foo<Int, A.B, W>
///
/// These lower to the following two rules:
///
///   T.[concrete: Foo<τ_0_0, τ_0_1, String> with {X.Y, Z}] => T
///   T.[concrete: Foo<Int, τ_0_0, τ_0_1> with {A.B, W}] => T
///
/// The two concrete type symbols will be added to the property bag of 'T',
/// and we will eventually end up in this method, where we will generate three
/// induced rules:
///
///   X.Y.[concrete: Int] => X.Y
///   A.B => Z
///   W.[concrete: String] => W
void PropertyMap::addConcreteTypeProperty(
    Term key, Symbol property, unsigned ruleID) {
  auto *props = getOrCreateProperties(key);

  unifyConcreteTypes(key,
                     props->ConcreteType,
                     props->ConcreteTypeRules,
                     property, ruleID);
}

/// Record a protocol conformance, layout or superclass constraint on the given
/// key. Must be called in monotonically non-decreasing key order.
void PropertyMap::addProperty(
    Term key, Symbol property, unsigned ruleID) {
  assert(property.isProperty());
  assert(*System.getRule(ruleID).isPropertyRule() == property);

  switch (property.getKind()) {
  case Symbol::Kind::Protocol:
    addConformanceProperty(key, property, ruleID);
    return;

  case Symbol::Kind::Layout:
    addLayoutProperty(key, property, ruleID);
    return;

  case Symbol::Kind::Superclass:
    addSuperclassProperty(key, property, ruleID);
    return;

  case Symbol::Kind::ConcreteType:
    addConcreteTypeProperty(key, property, ruleID);
    return;

  case Symbol::Kind::ConcreteConformance:
    // Concrete conformance rules are not recorded in the property map, since
    // they're not needed for unification, and generic signature queries don't
    // care about them.
    return;

  case Symbol::Kind::Name:
  case Symbol::Kind::GenericParam:
  case Symbol::Kind::AssociatedType:
  case Symbol::Kind::Shape:
    break;
  }

  llvm_unreachable("Bad symbol kind");
}

/// Post-pass to handle unification and conflict checking between pairs of
/// rules of different kinds:
///
/// - concrete vs superclass
/// - concrete vs layout
///
/// Note that we allow a subclass existential 'any C & P' to satisfy a
/// superclass requirement 'C' as long as 'P' is an @objc protocol.
///
/// This is not fully sound because 'any C & P' is not substitutable for
/// 'C' if the code calls static method or required initializers on 'C',
/// but existing code out there relies on this working.
///
/// A more refined check would ensure that 'C' had no required initializers
/// and that 'P' was self-conforming; or we could ban this entirely in a
/// future -swift-version mode.
void PropertyMap::checkConcreteTypeRequirements() {
  bool debug = Debug.contains(DebugFlags::ConcreteUnification);

  for (auto *props : Entries) {
    for (auto pair : props->ConcreteTypeRules) {
      auto concreteType = pair.first;
      unsigned concreteTypeRule = pair.second;

      // If the concrete type is not a class and we have a superclass
      // requirement, we have a conflict.
      if (!concreteType.getConcreteType()->getClassOrBoundGenericClass() &&
          !(concreteType.getConcreteType()->isObjCExistentialType() &&
            concreteType.getConcreteType()->getSuperclass()) &&
          props->hasSuperclassBound()) {
        const auto &req = props->getSuperclassRequirement();
        for (auto pair : req.SuperclassRules) {
          if (checkRulePairOnce(concreteTypeRule, pair.second))
            System.recordConflict(concreteTypeRule, pair.second);
        }
      }

      // If the concrete type does not satisfy a class layout constraint and
      // we have such a layout requirement, we have a conflict.
      if (!concreteType.getConcreteType()->satisfiesClassConstraint() &&
          props->LayoutRule &&
          props->Layout->isClass()) {
        if (checkRulePairOnce(concreteTypeRule, *props->LayoutRule))
          System.recordConflict(concreteTypeRule, *props->LayoutRule);
      }

      if (checkRuleOnce(concreteTypeRule)) {
        if (concreteType.getConcreteType()->satisfiesClassConstraint()) {
          Type superclassType = concreteType.getConcreteType();
          if (!superclassType->getClassOrBoundGenericClass())
            superclassType = superclassType->getSuperclass();

          if (superclassType) {
            // A rule (T.[concrete: C] => T) where C is a class type induces a rule
            // (T.[superclass: C] => T).
            auto superclassSymbol = Symbol::forSuperclass(
                superclassType->getCanonicalType(),
                concreteType.getSubstitutions(),
                Context);

            recordRelation(props->getKey(), concreteTypeRule,
                           superclassSymbol, System, debug);
          }

          // A rule (T.[concrete: C] => T) where C is a class type induces a rule
          // (T.[layout: L] => T), where L is either AnyObject or _NativeObject.
          auto layoutConstraint = LayoutConstraintKind::Class;
          if (superclassType)
            if (auto *classDecl = superclassType->getClassOrBoundGenericClass())
              layoutConstraint = classDecl->getLayoutConstraintKind();

          auto layout =
              LayoutConstraint::getLayoutConstraint(
                layoutConstraint, Context.getASTContext());
          auto layoutSymbol = Symbol::forLayout(layout, Context);

          recordRelation(props->getKey(), concreteTypeRule,
                         layoutSymbol, System, debug);
        }
      }
    }
  }
}
