//===--- Rule.h - An oriented rewrite rule in a rewrite system --*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_RULE_H
#define SWIFT_RULE_H

#include "swift/Basic/Assertions.h"
#include <optional>

#include "Symbol.h"
#include "Term.h"

namespace llvm {
  class raw_ostream;
}

namespace swift {

namespace rewriting {

class RewriteContext;

/// A rewrite rule that replaces occurrences of LHS with RHS.
///
/// LHS must be greater than RHS in the linear order over terms.
///
/// Out-of-line methods are documented in Rule.cpp.
class Rule final {
  Term LHS;
  Term RHS;

  /// The written requirement ID, which can be used to index into the
  /// \c WrittenRequirements array in the rewrite system to retrieve
  /// the structural requirement.
  ///
  /// This uses a biased representation where an ID of 0 means 'no ID',
  /// otherwise the value is the actual ID plus one.
  unsigned RequirementID : 16;

  /// A 'permanent' rule cannot be deleted by homotopy reduction. These
  /// do not correspond to generic requirements and are re-added when the
  /// rewrite system is built.
  unsigned Permanent : 1;

  /// An 'explicit' rule is a generic requirement written by the user.
  unsigned Explicit : 1;

  /// An 'LHS simplified' rule's left hand side was reduced via another rule.
  /// Set by simplifyLeftHandSides().
  unsigned LHSSimplified : 1;

  /// An 'RHS simplified' rule's right hand side can be reduced via another rule.
  /// Set by simplifyRightHandSides().
  unsigned RHSSimplified : 1;

  /// A 'substitution simplified' rule's left hand side contains substitutions
  /// which can be reduced via another rule.
  /// Set by simplifyLeftHandSideSubstitutions().
  unsigned SubstitutionSimplified : 1;

  /// A 'redundant' rule was eliminated by homotopy reduction. Redundant rules
  /// still participate in term rewriting, but they are not part of the minimal
  /// set of requirements in a generic signature.
  unsigned Redundant : 1;

  /// A 'conflicting' rule is a property rule which cannot be satisfied by any
  /// concrete type because it is mutually exclusive with some other rule.
  /// An example would be a pair of concrete type rules:
  ///
  ///    T.[concrete: Int] => T
  ///    T.[concrete: String] => T
  ///
  /// Conflicting rules are detected in property map construction, and are
  /// dropped from the minimal set of requirements.
  unsigned Conflicting : 1;

  /// A 'recursive' rule is a concrete type or superclass rule where the right
  /// hand side occurs as a proper prefix of one of its substitutions.
  ///
  /// Recursive rules are detected in RewriteSystem::computeRecursiveRules(),
  /// and are dropped from the minimal set of requirements.
  unsigned Recursive : 1;

  /// Whether this rule is now finalized and immutable.
  unsigned Frozen : 1;

public:
  Rule(Term lhs, Term rhs)
      : LHS(lhs), RHS(rhs) {
    RequirementID = 0;
    Permanent = false;
    Explicit = false;
    LHSSimplified = false;
    RHSSimplified = false;
    SubstitutionSimplified = false;
    Redundant = false;
    Conflicting = false;
    Recursive = false;
    Frozen = false;
  }

  const Term &getLHS() const { return LHS; }
  const Term &getRHS() const { return RHS; }

  std::optional<Symbol> isPropertyRule() const;

  const ProtocolDecl *isProtocolConformanceRule() const;

  const ProtocolDecl *isAnyConformanceRule() const;

  bool isIdentityConformanceRule() const;

  bool isProtocolRefinementRule(RewriteContext &ctx) const;

  bool isCircularConformanceRule() const;

  /// See above for an explanation of these predicates.
  bool isPermanent() const {
    return Permanent;
  }

  bool isExplicit() const {
    return Explicit;
  }

  bool isLHSSimplified() const {
    return LHSSimplified;
  }

  bool isRHSSimplified() const {
    return RHSSimplified;
  }

  bool isSubstitutionSimplified() const {
    return SubstitutionSimplified;
  }

  bool isRedundant() const {
    return Redundant;
  }

  bool isConflicting() const {
    return Conflicting;
  }

  bool isRecursive() const {
    return Recursive;
  }

  bool isFrozen() const {
    return Frozen;
  }

  bool containsUnresolvedSymbols() const {
    return (LHS.containsUnresolvedSymbols() ||
            RHS.containsUnresolvedSymbols());
  }

  std::optional<Identifier> isProtocolTypeAliasRule() const;

  bool isDerivedFromConcreteProtocolTypeAliasRule() const;

  void markLHSSimplified() {
    ASSERT(!Frozen);
    ASSERT(!LHSSimplified);
    LHSSimplified = true;
  }

  void markRHSSimplified() {
    ASSERT(!Frozen);
    ASSERT(!RHSSimplified);
    RHSSimplified = true;
  }

  void markSubstitutionSimplified() {
    ASSERT(!Frozen);
    ASSERT(!SubstitutionSimplified);
    SubstitutionSimplified = true;
  }

  void markPermanent() {
    ASSERT(!Frozen);
    ASSERT(!Explicit && !Permanent &&
           "Permanent and explicit are mutually exclusive");
    Permanent = true;
  }

  void markExplicit() {
    ASSERT(!Frozen);
    ASSERT(!Explicit && !Permanent &&
           "Permanent and explicit are mutually exclusive");
    Explicit = true;
  }

  void markRedundant() {
    ASSERT(!Frozen);
    ASSERT(!Redundant);
    Redundant = true;
  }

  void markConflicting() {
    // It's okay to mark a rule as conflicting multiple times.
    if (Conflicting)
      return;

    ASSERT(!Frozen);
    ASSERT(!Permanent && "Permanent rule should not conflict with anything");
    Conflicting = true;
  }

  void markRecursive() {
    ASSERT(!Frozen);
    ASSERT(!Permanent && "Permanent rule should not be recursive");
    ASSERT(!Recursive);
    Recursive = true;
  }

  void freeze() {
    Redundant = false;
    RequirementID = 0;
    Frozen = true;
  }

  unsigned getDepth() const;

  unsigned getNesting() const;

  std::optional<int> compare(const Rule &other, RewriteContext &ctx) const;

  void dump(llvm::raw_ostream &out) const;

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                       const Rule &rule) {
    rule.dump(out);
    return out;
  }
};

} // end namespace rewriting

} // end namespace swift

#endif
