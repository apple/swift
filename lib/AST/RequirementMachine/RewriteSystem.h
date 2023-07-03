//===--- RewriteSystem.h - Generics with term rewriting ---------*- C++ -*-===//
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

#ifndef SWIFT_REWRITESYSTEM_H
#define SWIFT_REWRITESYSTEM_H

#include "swift/AST/Requirement.h"
#include "swift/AST/TypeCheckRequests.h"
#include "llvm/ADT/DenseSet.h"

#include "Debug.h"
#include "Diagnostics.h"
#include "RewriteLoop.h"
#include "Rule.h"
#include "Symbol.h"
#include "Term.h"
#include "Trie.h"
#include "TypeDifference.h"

namespace llvm {
  class raw_ostream;
}

namespace swift {

namespace rewriting {

class PropertyMap;
class RewriteContext;
class RewriteSystem;

/// Result type for RequirementMachine::computeCompletion().
enum class CompletionResult {
  /// Completion was successful.
  Success,

  /// Maximum number of rules exceeded.
  MaxRuleCount,

  /// Maximum rule length exceeded.
  MaxRuleLength,

  /// Maximum concrete type nesting depth exceeded.
  MaxConcreteNesting
};

/// A term rewrite system for working with types in a generic signature.
///
/// Out-of-line methods are documented in RewriteSystem.cpp.
class RewriteSystem final {
  /// Rewrite context for memory allocation.
  RewriteContext &Context;

  /// If this is a rewrite system for a connected component of protocols,
  /// this array is non-empty. Otherwise, it is a rewrite system for a
  /// top-level generic signature and this array is empty.
  ArrayRef<const ProtocolDecl *> Protos;

  /// The requirements written in source code.
  std::vector<StructuralRequirement> WrittenRequirements;

  /// The rules added so far, including rules from our client, as well
  /// as rules introduced by the completion procedure.
  std::vector<Rule> Rules;

  unsigned FirstLocalRule = 0;

  /// A prefix trie of rule left hand sides to optimize lookup. The value
  /// type is an index into the Rules array defined above.
  Trie<unsigned, MatchKind::Shortest> Trie;

  /// The set of protocols known to this rewrite system.
  ///
  /// See RuleBuilder::ReferencedProtocols for a more complete explanation.
  ///
  /// For the most part, this is only used while building the rewrite system,
  /// but conditional requirement inference forces us to be able to add new
  /// protocols to the rewrite system after the fact, so this little bit of
  /// RuleBuilder state outlives the initialization phase.
  llvm::DenseSet<const ProtocolDecl *> ReferencedProtocols;

  DebugOptions Debug;

  /// Whether we've initialized the rewrite system with a call to initialize().
  unsigned Initialized : 1;

  /// Whether we've computed the confluent completion at least once.
  ///
  /// It might be computed multiple times if the property map's concrete type
  /// unification procedure adds new rewrite rules.
  unsigned Complete : 1;

  /// Whether we've minimized the rewrite system.
  unsigned Minimized : 1;

  /// Whether the rewrite system is finalized, immutable, and ready for
  /// generic signature queries.
  unsigned Frozen : 1;

  /// If set, the completion procedure records rewrite loops describing the
  /// identities among rewrite rules discovered while resolving critical pairs.
  unsigned RecordLoops : 1;

  /// The length of the longest initial rule, used for the MaxRuleLength
  /// completion non-termination heuristic.
  unsigned LongestInitialRule : 16;

public:
  explicit RewriteSystem(RewriteContext &ctx);
  ~RewriteSystem();

  RewriteSystem(const RewriteSystem &) = delete;
  RewriteSystem(RewriteSystem &&) = delete;
  RewriteSystem &operator=(const RewriteSystem &) = delete;
  RewriteSystem &operator=(RewriteSystem &&) = delete;

  /// Return the rewrite context used for allocating memory.
  RewriteContext &getRewriteContext() const { return Context; }

  llvm::DenseSet<const ProtocolDecl *> &getReferencedProtocols() {
    return ReferencedProtocols;
  }

  DebugOptions getDebugOptions() const { return Debug; }

  void initialize(
      bool recordLoops, ArrayRef<const ProtocolDecl *> protos,
      std::vector<StructuralRequirement> &&writtenRequirements,
      std::vector<Rule> &&importedRules,
      std::vector<std::pair<MutableTerm, MutableTerm>> &&permanentRules,
      std::vector<std::tuple<MutableTerm, MutableTerm,
                             llvm::Optional<unsigned>>> &&requirementRules);

  unsigned getLongestInitialRule() const {
    return LongestInitialRule;
  }

  ArrayRef<const ProtocolDecl *> getProtocols() const {
    return Protos;
  }

  bool isKnownProtocol(const ProtocolDecl *proto) const {
    return ReferencedProtocols.count(proto) > 0;
  }

  unsigned getRuleID(const Rule &rule) const {
    assert((unsigned)(&rule - &*Rules.begin()) < Rules.size());
    return (unsigned)(&rule - &*Rules.begin());
  }

  /// Get an array of all rewrite rules.
  ArrayRef<Rule> getRules() const {
    return Rules;
  }

  /// Get an array of rewrite rules, not including rewrite rules imported
  /// from referenced protocols.
  ArrayRef<Rule> getLocalRules() const {
    return getRules().slice(FirstLocalRule);
  }

  /// Get the rewrite rule at the given index. Note that this is an index
  /// into getRules(), *NOT* getLocalRules().
  Rule &getRule(unsigned ruleID) {
    return Rules[ruleID];
  }

  const Rule &getRule(unsigned ruleID) const {
    return Rules[ruleID];
  }

  bool addRule(MutableTerm lhs, MutableTerm rhs,
               const RewritePath *path=nullptr);

  bool addPermanentRule(MutableTerm lhs, MutableTerm rhs);

  bool addExplicitRule(MutableTerm lhs, MutableTerm rhs,
                       llvm::Optional<unsigned> requirementID);

  void addRules(
      std::vector<Rule> &&importedRules,
      std::vector<std::pair<MutableTerm, MutableTerm>> &&permanentRules,
      std::vector<std::tuple<MutableTerm, MutableTerm,
                             llvm::Optional<unsigned>>> &&requirementRules);

  bool simplify(MutableTerm &term, RewritePath *path=nullptr) const;

  llvm::Optional<unsigned> simplifySubstitutions(Term baseTerm, Symbol symbol,
                                                 const PropertyMap *map,
                                                 RewritePath *path = nullptr);

  //////////////////////////////////////////////////////////////////////////////
  ///
  /// Completion
  ///
  //////////////////////////////////////////////////////////////////////////////

  /// Pairs of rules which have already been checked for overlap.
  llvm::DenseSet<std::pair<unsigned, unsigned>> CheckedOverlaps;

  std::pair<CompletionResult, unsigned>
  computeConfluentCompletion(unsigned maxRuleCount,
                             unsigned maxRuleLength);

  void simplifyLeftHandSides();

  void simplifyRightHandSides();

  void simplifyLeftHandSideSubstitutions(const PropertyMap *map);

  enum ValidityPolicy {
    AllowInvalidRequirements,
    DisallowInvalidRequirements
  };

  void verifyRewriteRules(ValidityPolicy policy) const;

  //////////////////////////////////////////////////////////////////////////////
  ///
  /// Diagnostics
  ///
  //////////////////////////////////////////////////////////////////////////////

  void computeRedundantRequirementDiagnostics(SmallVectorImpl<RequirementError> &errors);

  void computeConflictingRequirementDiagnostics(SmallVectorImpl<RequirementError> &errors,
                                                SourceLoc signatureLoc,
                                                const PropertyMap &map,
                                                ArrayRef<GenericTypeParamType *> genericParams);

  void computeRecursiveRequirementDiagnostics(SmallVectorImpl<RequirementError> &errors,
                                              SourceLoc signatureLoc,
                                              const PropertyMap &map,
                                              ArrayRef<GenericTypeParamType *> genericParams);

private:
  struct CriticalPair {
    MutableTerm LHS;
    MutableTerm RHS;
    RewritePath Path;

    CriticalPair(MutableTerm lhs, MutableTerm rhs, RewritePath path)
      : LHS(lhs), RHS(rhs), Path(path) {}
  };

  bool
  computeCriticalPair(
      ArrayRef<Symbol>::const_iterator from,
      const Rule &lhs, const Rule &rhs,
      std::vector<CriticalPair> &pairs,
      std::vector<RewriteLoop> &loops) const;

  //////////////////////////////////////////////////////////////////////////////
  ///
  /// Relations are "pseudo-rules" introduced by the property map
  ///
  //////////////////////////////////////////////////////////////////////////////

public:
  /// The left hand side is known to be smaller than the right hand side.
  using Relation = std::pair<Term, Term>;

private:
  /// The map's values are indices into the vector. The map is used for
  /// uniquing, then the index is returned and lookups are performed into
  /// the vector.
  llvm::DenseMap<Relation, unsigned> RelationMap;
  std::vector<Relation> Relations;

public:
  unsigned recordRelation(Term lhs, Term rhs);
  Relation getRelation(unsigned index) const;

  unsigned recordRelation(Symbol lhs, Symbol rhs);

  unsigned recordConcreteConformanceRelation(
      Symbol concreteSymbol, Symbol protocolSymbol,
      Symbol concreteConformanceSymbol);

  unsigned recordConcreteTypeWitnessRelation(
      Symbol concreteConformanceSymbol,
      Symbol associatedTypeSymbol,
      Symbol typeWitnessSymbol);

  unsigned recordSameTypeWitnessRelation(
      Symbol concreteConformanceSymbol,
      Symbol associatedTypeSymbol);

private:
  /// The map's values are indices into the vector. The map is used for
  /// uniquing, then the index is returned and lookups are performed into
  /// the vector.
  llvm::DenseMap<std::tuple<Term, Symbol, Symbol>, unsigned> DifferenceMap;
  std::vector<TypeDifference> Differences;

  /// Avoid duplicate work when simplifying substitutions or rebuilding
  /// the property map.
  llvm::DenseSet<unsigned> CheckedDifferences;

public:
  unsigned recordTypeDifference(const TypeDifference &difference);

  bool computeTypeDifference(Term term, Symbol lhs, Symbol rhs,
                             llvm::Optional<unsigned> &lhsDifferenceID,
                             llvm::Optional<unsigned> &rhsDifferenceID);

  const TypeDifference &getTypeDifference(unsigned index) const;

  void processTypeDifference(const TypeDifference &difference,
                             unsigned differenceID,
                             unsigned lhsRuleID,
                             const RewritePath &rhsPath);

  void buildRewritePathForJoiningTerms(MutableTerm lhsTerm,
                                       MutableTerm rhsTerm,
                                       RewritePath *path) const;

  void buildRewritePathForUnifier(Term key,
                                  unsigned lhsRuleID,
                                  const RewritePath &rhsPath,
                                  RewritePath *path) const;

private:
  //////////////////////////////////////////////////////////////////////////////
  ///
  /// Homotopy reduction
  ///
  //////////////////////////////////////////////////////////////////////////////

  /// Homotopy generators for this rewrite system. These are the
  /// rewrite loops which rewrite a term back to itself.
  ///
  /// In the category theory interpretation, a rewrite rule is a generating
  /// 2-cell, and a rewrite path is a 2-cell made from a composition of
  /// generating 2-cells.
  ///
  /// Homotopy generators, in turn, are 3-cells. The special case of a
  /// 3-cell discovered during completion can be viewed as two parallel
  /// 2-cells; this is actually represented as a single 2-cell forming a
  /// loop around a base point.
  ///
  /// This data is used by the homotopy reduction and minimal conformances
  /// algorithms.
  std::vector<RewriteLoop> Loops;

  /// A list of pairs where the first element is a rule number and the second
  /// element is an equivalent rewrite path in terms of non-redundant rules.
  std::vector<std::pair<unsigned, RewritePath>> RedundantRules;

  /// Pairs of rules which together preclude a concrete type from satisfying the
  /// requirements of the generic signature.
  ///
  /// Conflicts are detected in property map construction. Conflicts are
  /// diagnosed and one of the rules in each pair is dropped during
  /// minimization.
  std::vector<std::pair<unsigned, unsigned>> ConflictingRules;

  /// A 'recursive' rule is a concrete type or superclass rule where the right
  /// hand side occurs as a prefix of one of its substitutions.
  ///
  /// Populated by computeRecursiveRules().
  std::vector<unsigned> RecursiveRules;

  void propagateExplicitBits();

  void propagateRedundantRequirementIDs();

  void computeRecursiveRules();

  using EliminationPredicate = llvm::function_ref<bool(unsigned loopID,
                                                       unsigned ruleID)>;

  llvm::Optional<std::pair<unsigned, unsigned>>
  findRuleToDelete(EliminationPredicate isRedundantRuleFn);

  void deleteRule(unsigned ruleID, const RewritePath &replacementPath);

  void performHomotopyReduction(EliminationPredicate isRedundantRuleFn);

public:
  // Utilities for minimal conformances algorithm, defined in
  // MinimalConformances.cpp.

  void decomposeTermIntoConformanceRuleLeftHandSides(
      MutableTerm term,
      SmallVectorImpl<unsigned> &result) const;
  void decomposeTermIntoConformanceRuleLeftHandSides(
      MutableTerm term, unsigned ruleID,
      SmallVectorImpl<unsigned> &result) const;

  void computeCandidateConformancePaths(
      const PropertyMap &map,
      llvm::MapVector<unsigned,
                      std::vector<SmallVector<unsigned, 2>>> &paths) const;

private:
  void computeMinimalConformances(
      const PropertyMap &map,
      llvm::DenseSet<unsigned> &redundantConformances) const;

public:
  void recordRewriteLoop(MutableTerm basepoint,
                         RewritePath path);

  void recordConflict(unsigned existingRuleID, unsigned newRuleID);

  bool isInMinimizationDomain(const ProtocolDecl *proto) const;

  ArrayRef<RewriteLoop> getLoops() const {
    return Loops;
  }

  void minimizeRewriteSystem(const PropertyMap &map);

  GenericSignatureErrors getErrors() const;

  struct MinimizedProtocolRules {
    std::vector<unsigned> Requirements;
    std::vector<unsigned> TypeAliases;
  };

  llvm::DenseMap<const ProtocolDecl *, MinimizedProtocolRules>
  getMinimizedProtocolRules() const;

  std::vector<unsigned> getMinimizedGenericSignatureRules() const;

private:
  void verifyRewriteLoops() const;

  void verifyRedundantConformances(
      const llvm::DenseSet<unsigned> &redundantConformances) const;

  void verifyMinimizedRules(
      const llvm::DenseSet<unsigned> &redundantConformances) const;

public:
  void freeze();

  void dump(llvm::raw_ostream &out) const;
};

} // end namespace rewriting

} // end namespace swift

#endif
