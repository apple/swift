//===--- RewriteSystem.cpp - Generics with term rewriting -----------------===//
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

#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <vector>
#include "PropertyMap.h"
#include "RewriteContext.h"
#include "RewriteLoop.h"
#include "RewriteSystem.h"
#include "Rule.h"
#include "Trie.h"

using namespace swift;
using namespace rewriting;

RewriteSystem::RewriteSystem(RewriteContext &ctx)
    : Context(ctx), Debug(ctx.getDebugOptions()) {
  Initialized = 0;
  Complete = 0;
  Minimized = 0;
  Frozen = 0;
  RecordLoops = 0;
  LongestInitialRule = 0;
}

RewriteSystem::~RewriteSystem() {
  Trie.updateHistograms(Context.RuleTrieHistogram,
                        Context.RuleTrieRootHistogram);
}

/// Initialize the rewrite system using rewrite rules built by the RuleBuilder.
///
/// - recordLoops: Whether this is a rewrite system built from user-written
///   requirements, in which case we will perform minimization using rewrite
///   loops recorded during completion.
///
/// - protos: If this is a rewrite system built from a protocol connected
///   component, this contains the members of the protocol. For a rewrite
///   system built from a generic signature, this is empty. Used by
///   RewriteSystem::isInMinimizationDomain().
///
/// These parameters should be populated from the corresponding fields of the
/// RuleBuilder instance:
///
/// - writtenRequirements: The user-written requirements, if any, used to
///   track source locations for redundancy diagnostics.
///
/// - importedRules: Rewrite rules for referenced protocols. These come from
///   the Requirement Machine instances for these protocols' connected
///   components, so they are already confluent and can be imported verbatim.
///
/// - permanentRules: Permanent rules, such as associated type introduction
///   rules for associated types defined in protocols in this connected
///   component.
///
/// - requirementRules: Rules corresponding to generic requirements written
///   by the user.
///
/// This can only be called once. It adds the rules to the rewrite system,
/// allowing computeConfluentCompletion() to be called to compute the
/// complete rewrite system.
void RewriteSystem::initialize(
    bool recordLoops, ArrayRef<const ProtocolDecl *> protos,
    std::vector<StructuralRequirement> &&writtenRequirements,
    std::vector<Rule> &&importedRules,
    std::vector<std::pair<MutableTerm, MutableTerm>> &&permanentRules,
    std::vector<std::tuple<MutableTerm, MutableTerm, llvm::Optional<unsigned>>>
        &&requirementRules) {
  assert(!Initialized);
  Initialized = 1;

  RecordLoops = recordLoops;
  Protos = protos;
  WrittenRequirements = std::move(writtenRequirements);

  addRules(std::move(importedRules),
           std::move(permanentRules),
           std::move(requirementRules));

  for (const auto &rule : getLocalRules()) {
    LongestInitialRule = std::max(LongestInitialRule, rule.getDepth());
  }
}

/// Reduce a term by applying all rewrite rules until fixed point.
///
/// If \p path is non-null, records the series of rewrite steps taken.
bool RewriteSystem::simplify(MutableTerm &term, RewritePath *path) const {
  bool changed = false;

  MutableTerm original;
  RewritePath subpath;

  bool debug = false;
  if (Debug.contains(DebugFlags::Simplify)) {
    original = term;
    debug = true;
  }

  while (true) {
    bool tryAgain = false;

    auto from = term.begin();
    auto end = term.end();
    while (from < end) {
      auto ruleID = Trie.find(from, end);
      if (ruleID) {
        const auto &rule = getRule(*ruleID);

        auto to = from + rule.getLHS().size();
        assert(std::equal(from, to, rule.getLHS().begin()));

        unsigned startOffset = (unsigned)(from - term.begin());
        unsigned endOffset = term.size() - rule.getLHS().size() - startOffset;

        term.rewriteSubTerm(from, to, rule.getRHS());

        if (path || debug) {
          subpath.add(RewriteStep::forRewriteRule(startOffset, endOffset, *ruleID,
                                                  /*inverse=*/false));
        }

        changed = true;
        tryAgain = true;
        break;
      }

      ++from;
    }

    if (!tryAgain)
      break;
  }

  if (debug) {
    if (changed) {
      llvm::dbgs() << "= Simplified " << original << " to " << term << " via ";
      subpath.dump(llvm::dbgs(), original, *this);
      llvm::dbgs() << "\n";
    } else {
      llvm::dbgs() << "= Irreducible term: " << term << "\n";
    }
  }

  if (path != nullptr) {
    assert(changed != subpath.empty());
    path->append(subpath);
  }

  return changed;
}

/// Adds a rewrite rule, returning true if the new rule was non-trivial.
///
/// If both sides simplify to the same term, the rule is trivial and discarded,
/// and this method returns false.
///
/// If \p path is non-null, the new rule is derived from existing rules in the
/// rewrite system; the path records a series of rewrite steps which transform
/// \p lhs to \p rhs.
bool RewriteSystem::addRule(MutableTerm lhs, MutableTerm rhs,
                            const RewritePath *path) {
  assert(!Frozen);

  assert(!lhs.empty());
  assert(!rhs.empty());

  if (Debug.contains(DebugFlags::Add)) {
    llvm::dbgs() << "# Adding rule " << lhs << " == " << rhs << "\n\n";
  }

  // Now simplify both sides as much as possible with the rules we have so far.
  //
  // This avoids unnecessary work in the completion algorithm.
  RewritePath lhsPath;
  RewritePath rhsPath;

  simplify(lhs, &lhsPath);
  simplify(rhs, &rhsPath);

  RewritePath loop;
  if (path) {
    // Produce a path from the simplified lhs to the simplified rhs.

    // (1) First, apply lhsPath in reverse to produce the original lhs.
    lhsPath.invert();
    loop.append(lhsPath);

    // (2) Now, apply the path from the original lhs to the original rhs
    // given to us by the completion procedure.
    loop.append(*path);

    // (3) Finally, apply rhsPath to produce the simplified rhs, which
    // is the same as the simplified lhs.
    loop.append(rhsPath);
  }

  // If the left hand side and right hand side are already equivalent, we're
  // done.
  llvm::Optional<int> result = lhs.compare(rhs, Context);
  if (*result == 0) {
    // If this rule is a consequence of existing rules, add a homotopy
    // generator.
    if (path) {
      // We already have a loop, since the simplified lhs is identical to the
      // simplified rhs.
      recordRewriteLoop(lhs, loop);

      if (Debug.contains(DebugFlags::Add)) {
        llvm::dbgs() << "## Recorded trivial loop at " << lhs << ": ";
        loop.dump(llvm::dbgs(), lhs, *this);
        llvm::dbgs() << "\n\n";
      }
    }

    return false;
  }

  // Orient the two terms so that the left hand side is greater than the
  // right hand side.
  if (*result < 0) {
    std::swap(lhs, rhs);
    loop.invert();
  }

  assert(*lhs.compare(rhs, Context) > 0);

  if (Debug.contains(DebugFlags::Add)) {
    llvm::dbgs() << "## Simplified and oriented rule " << lhs << " => " << rhs << "\n\n";
  }

  unsigned newRuleID = Rules.size();

  Rules.emplace_back(Term::get(lhs, Context), Term::get(rhs, Context));

  if (path) {
    // We have a rewrite path from the simplified lhs to the simplified rhs;
    // add a rewrite step applying the new rule in reverse to close the loop.
    loop.add(RewriteStep::forRewriteRule(/*startOffset=*/0, /*endOffset=*/0,
                                         newRuleID, /*inverse=*/true));
    recordRewriteLoop(lhs, loop);

    if (Debug.contains(DebugFlags::Add)) {
      llvm::dbgs() << "## Recorded non-trivial loop at " << lhs << ": ";
      loop.dump(llvm::dbgs(), lhs, *this);
      llvm::dbgs() << "\n\n";
    }
  }

  auto oldRuleID = Trie.insert(lhs.begin(), lhs.end(), newRuleID);
  if (oldRuleID) {
    llvm::errs() << "Duplicate rewrite rule!\n";
    const auto &oldRule = getRule(*oldRuleID);
    llvm::errs() << "Old rule #" << *oldRuleID << ": ";
    oldRule.dump(llvm::errs());
    llvm::errs() << "\nTrying to replay what happened when I simplified this term:\n";
    Debug |= DebugFlags::Simplify;
    MutableTerm term = lhs;
    simplify(lhs);

    dump(llvm::errs());
    abort();
  }

  // Tell the caller that we added a new rule.
  return true;
}

/// Add a new rule, marking it permanent.
bool RewriteSystem::addPermanentRule(MutableTerm lhs, MutableTerm rhs) {
  bool added = addRule(std::move(lhs), std::move(rhs));
  if (added)
    Rules.back().markPermanent();

  return added;
}

/// Add a new rule, marking it explicit.
bool RewriteSystem::addExplicitRule(MutableTerm lhs, MutableTerm rhs,
                                    llvm::Optional<unsigned> requirementID) {
  bool added = addRule(std::move(lhs), std::move(rhs));
  if (added) {
    Rules.back().markExplicit();
    Rules.back().setRequirementID(requirementID);
  }

  return added;
}

/// Add a set of rules from a RuleBuilder.
///
/// This is used when building a rewrite system in initialize() above.
///
/// It is also used when conditional requirement inference pulls in additional
/// protocols after the fact.
void RewriteSystem::addRules(
    std::vector<Rule> &&importedRules,
    std::vector<std::pair<MutableTerm, MutableTerm>> &&permanentRules,
    std::vector<std::tuple<MutableTerm, MutableTerm, llvm::Optional<unsigned>>>
        &&requirementRules) {
  unsigned ruleCount = Rules.size();

  if (ruleCount == 0) {
    // Fast path if this is called from initialization; just steal the
    // underlying storage of the imported rule vector.
    Rules = std::move(importedRules);
  }
  else {
    // Otherwise, copy the imported rules in.
    Rules.insert(Rules.end(), importedRules.begin(), importedRules.end());
  }

  // If this is the initial call, note the first non-imported rule so that
  // we can skip over imported rules later.
  if (ruleCount == 0)
    FirstLocalRule = Rules.size();

  // Add the imported rules to the trie.
  for (unsigned newRuleID = ruleCount, e = Rules.size();
       newRuleID < e; ++newRuleID) {
    const auto &newRule = Rules[newRuleID];
    // Skip simplified rules. At the very least we need to skip RHS-simplified
    // rules since their left hand sides might duplicate existing rules; the
    // others are skipped purely as an optimization. We can't skip subst-
    // simplified rules, since property map construction considers them.
    if (newRule.isLHSSimplified() ||
        newRule.isRHSSimplified())
      continue;

    auto oldRuleID = Trie.insert(newRule.getLHS().begin(),
                                 newRule.getLHS().end(),
                                 newRuleID);
    if (oldRuleID) {
      llvm::errs() << "Imported rules have duplicate left hand sides!\n";
      llvm::errs() << "New rule #" << newRuleID << ": " << newRule << "\n";
      const auto &oldRule = getRule(*oldRuleID);
      llvm::errs() << "Old rule #" << *oldRuleID << ": " << oldRule << "\n\n";
      dump(llvm::errs());
      abort();
    }
  }

  // Now add our own rules.
  for (const auto &rule : permanentRules)
    addPermanentRule(rule.first, rule.second);

  for (const auto &rule : requirementRules) {
    auto lhs = std::get<0>(rule);
    auto rhs = std::get<1>(rule);
    auto requirementID = std::get<2>(rule);

    // When this is called while adding conditional requirements, there
    // shouldn't be any new structural requirement IDs.
    assert(ruleCount == 0 || !requirementID.has_value());

    addExplicitRule(lhs, rhs, requirementID);
  }
}

/// Delete any rules whose left hand sides can be reduced by other rules.
///
/// Must be run after the completion procedure, since the deletion of
/// rules is only valid to perform if the rewrite system is confluent.
void RewriteSystem::simplifyLeftHandSides() {
  assert(Complete);

  for (unsigned ruleID = FirstLocalRule, e = Rules.size(); ruleID < e; ++ruleID) {
    auto &rule = getRule(ruleID);
    if (rule.isLHSSimplified())
      continue;

    // First, see if the left hand side of this rule can be reduced using
    // some other rule.
    auto lhs = rule.getLHS();
    auto begin = lhs.begin();
    auto end = lhs.end();
    while (begin < end) {
      if (auto otherRuleID = Trie.find(begin++, end)) {
        // A rule does not obsolete itself.
        if (*otherRuleID == ruleID)
          continue;

        // Ignore other deleted rules.
        const auto &otherRule = getRule(*otherRuleID);
        if (otherRule.isLHSSimplified())
          continue;

        if (Debug.contains(DebugFlags::Completion)) {
          const auto &otherRule = getRule(*otherRuleID);
          llvm::dbgs() << "$ Deleting rule " << rule << " because "
                       << "its left hand side contains " << otherRule
                       << "\n";
        }

        rule.markLHSSimplified();
        break;
      }
    }
  }
}

/// Reduce the right hand sides of all remaining rules as much as
/// possible.
///
/// Must be run after the completion procedure, since the deletion of
/// rules is only valid to perform if the rewrite system is confluent.
void RewriteSystem::simplifyRightHandSides() {
  assert(Complete);

  for (unsigned ruleID = FirstLocalRule, e = Rules.size(); ruleID < e; ++ruleID) {
    auto &rule = getRule(ruleID);
    if (rule.isRHSSimplified())
      continue;

    // Now, try to reduce the right hand side.
    RewritePath rhsPath;
    MutableTerm rhs(rule.getRHS());
    if (!simplify(rhs, &rhsPath))
      continue;

    auto lhs = rule.getLHS();

    // We're adding a new rule, so the old rule won't apply anymore.
    rule.markRHSSimplified();

    unsigned newRuleID = Rules.size();

    if (Debug.contains(DebugFlags::Add)) {
      llvm::dbgs() << "## RHS simplification adds a rule " << lhs << " => " << rhs << "\n\n";
    }

    // Add a new rule with the simplified right hand side.
    Rules.emplace_back(lhs, Term::get(rhs, Context));
    auto oldRuleID = Trie.insert(lhs.begin(), lhs.end(), newRuleID);
    assert(oldRuleID == ruleID);
    (void) oldRuleID;

    // Produce a loop at the original lhs.
    RewritePath loop;

    // (1) First, apply the original rule to produce the original rhs.
    loop.add(RewriteStep::forRewriteRule(/*startOffset=*/0, /*endOffset=*/0,
                                         ruleID, /*inverse=*/false));

    // (2) Next, apply rhsPath to produce the simplified rhs.
    loop.append(rhsPath);

    // (3) Finally, apply the new rule in reverse to produce the original lhs.
    loop.add(RewriteStep::forRewriteRule(/*startOffset=*/0, /*endOffset=*/0,
                                         newRuleID, /*inverse=*/true));

    if (Debug.contains(DebugFlags::Completion)) {
      llvm::dbgs() << "$ Right hand side simplification recorded a loop at ";
      llvm::dbgs() << lhs << ": ";
      loop.dump(llvm::dbgs(), MutableTerm(lhs), *this);
      llvm::dbgs() << "\n";
    }

    recordRewriteLoop(MutableTerm(lhs), loop);
  }
}

/// When minimizing a generic signature, we only care about loops where the
/// basepoint is a generic parameter symbol.
///
/// When minimizing protocol requirement signatures, we only care about loops
/// where the basepoint is a protocol symbol or associated type symbol whose
/// protocol is part of the connected component.
///
/// All other loops can be discarded since they do not encode redundancies
/// that are relevant to us.
bool RewriteSystem::isInMinimizationDomain(const ProtocolDecl *proto) const {
  assert(Protos.empty() || proto != nullptr);

  if (proto == nullptr && Protos.empty())
    return true;

  if (std::find(Protos.begin(), Protos.end(), proto) != Protos.end())
    return true;

  return false;
}

void RewriteSystem::recordRewriteLoop(MutableTerm basepoint,
                                      RewritePath path) {
  assert(!Frozen);

  RewriteLoop loop(basepoint, path);
  loop.verify(*this);

  if (!RecordLoops)
    return;

  // Ignore the rewrite loop if it is not part of our minimization domain.
  //
  // Completion might record a rewrite loop where the basepoint is just
  // the term [shape]. In this case though, we know it's in our domain,
  // since completion only checks local rules for overlap. Other callers
  // of recordRewriteLoop() always pass in a valid basepoint, so we
  // check.
  if (basepoint[0].getKind() != Symbol::Kind::Shape &&
      !isInMinimizationDomain(basepoint.getRootProtocol())) {
    return;
  }

  Loops.push_back(loop);
}

void RewriteSystem::verifyRewriteRules(ValidityPolicy policy) const {
#define ASSERT_RULE(expr) \
  if (!(expr)) { \
    llvm::errs() << "&&& Malformed rewrite rule: " << rule << "\n"; \
    llvm::errs() << "&&& " << #expr << "\n\n"; \
    dump(llvm::errs()); \
    abort(); \
  }

  for (const auto &rule : getLocalRules()) {
    const auto &lhs = rule.getLHS();
    const auto &rhs = rule.getRHS();

    for (unsigned index : indices(lhs)) {
      auto symbol = lhs[index];

      // The left hand side can contain a single name symbol if it has the form
      // T.N or T.N.[p], where T is some prefix that does not contain name
      // symbols, N is a name symbol, and [p] is an optional property symbol.
      //
      // In the latter case, we have a protocol typealias, or a rule derived
      // via resolving a critical pair involving a protocol typealias.
      //
      // Any other valid occurrence of a name symbol should have been reduced by
      // an associated type introduction rule [P].N, marking the rule as
      // LHS-simplified.
      if (!rule.isLHSSimplified() &&
          (rule.isPropertyRule()
           ? index != lhs.size() - 2
           : index != lhs.size() - 1)) {
        // This is only true if the input requirements were valid.
        if (policy == DisallowInvalidRequirements) {
          ASSERT_RULE(symbol.getKind() != Symbol::Kind::Name);
        } else {
          // FIXME: Assert that we diagnosed an error
        }
      }

      if (index != lhs.size() - 1) {
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::Layout);
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::Superclass);
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::ConcreteType);
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::Shape);
      }

      if (!rule.isLHSSimplified() &&
          index != lhs.size() - 1) {
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::ConcreteConformance);
      }

      if (index != 0) {
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::GenericParam);
      }

      if (!rule.isLHSSimplified() &&
          index != 0 && index != lhs.size() - 1) {
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::Protocol);
      }
    }

    for (unsigned index : indices(rhs)) {
      auto symbol = rhs[index];

      // The right hand side can contain a single name symbol if it has the form
      // T.N, where T is some prefix that does not contain name symbols, and
      // N is a name symbol.
      //
      // In this case, we have a protocol typealias, or a rule derived via
      // resolving a critical pair involving a protocol typealias.
      //
      // Any other valid occurrence of a name symbol should have been reduced by
      // an associated type introduction rule [P].N, marking the rule as
      // RHS-simplified.
      if (!rule.isRHSSimplified() &&
          index != rhs.size() - 1) {
        // This is only true if the input requirements were valid.
        if (policy == DisallowInvalidRequirements) {
          ASSERT_RULE(symbol.getKind() != Symbol::Kind::Name);
        } else {
          // FIXME: Assert that we diagnosed an error
        }
      }

      ASSERT_RULE(symbol.getKind() != Symbol::Kind::Layout);
      ASSERT_RULE(symbol.getKind() != Symbol::Kind::Superclass);
      ASSERT_RULE(symbol.getKind() != Symbol::Kind::ConcreteType);

      if (index != rhs.size() - 1) {
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::Shape);
      }

      // Completion can introduce a rule of the form
      //
      // (T.[P] => T.[concrete: C : P])
      //
      // Such rules are immediately simplified away. Otherwise, we should
      // never see a symbol with substitutions (concrete type, superclass,
      // concrete conformance) on the right hand side of a rule.
      if (!(rule.isRHSSimplified() &&
            index == rhs.size() - 1)) {
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::Superclass);
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::ConcreteType);
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::ConcreteConformance);
      }

      if (index != 0) {
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::GenericParam);
      }

      if (!rule.isRHSSimplified() &&
          index != 0) {
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::Protocol);
      }
    }

    if (rhs.size() == 1 && rhs[0].getKind() == Symbol::Kind::Shape) {
      // We can have a rule like T.[shape] => [shape].
      ASSERT_RULE(lhs.back().getKind() == Symbol::Kind::Shape);
    } else {
      // Otherwise, LHS and RHS must have the same domain.
      auto lhsDomain = lhs.getRootProtocol();
      auto rhsDomain = rhs.getRootProtocol();
      ASSERT_RULE(lhsDomain == rhsDomain);
    }
  }

#undef ASSERT_RULE
}

/// Free up memory by purging unused data structures after completion
/// (for a rewrite system built from a generic signature) or minimization
/// (for a rewrite system built from user-written requirements).
void RewriteSystem::freeze() {
  assert(Complete);
  assert(!Frozen);

  for (unsigned ruleID = FirstLocalRule, e = Rules.size();
       ruleID < e; ++ruleID) {
    getRule(ruleID).freeze();
  }

  WrittenRequirements.clear();
  CheckedOverlaps.clear();
  RelationMap.clear();
  Relations.clear();
  DifferenceMap.clear();
  Differences.clear();
  CheckedDifferences.clear();
  Loops.clear();
  RedundantRules.clear();
  ConflictingRules.clear();
}

void RewriteSystem::dump(llvm::raw_ostream &out) const {
  out << "Rewrite system: {\n";
  for (const auto &rule : Rules) {
    out << "- " << rule;
    if (auto ID = rule.getRequirementID()) {
      auto requirement = WrittenRequirements[*ID];
      out << " [ID: " << *ID << " - ";
      requirement.req.dump(out);
      out << " at ";
      requirement.loc.print(out, Context.getASTContext().SourceMgr);
      out << "]";
    }
    out << "\n";
  }
  out << "}\n";
  if (!Relations.empty()) {
    out << "Relations: {\n";
    for (const auto &relation : Relations) {
      out << "- " << relation.first << " =>> " << relation.second << "\n";
    }
    out << "}\n";
  }
  if (!Differences.empty()) {
    out << "Type differences: {\n";
    for (const auto &difference : Differences) {
      difference.dump(out);
      out << "\n";
    }
    out << "}\n";
  }
  if (!Loops.empty()) {
    out << "Rewrite loops: {\n";
    for (unsigned loopID : indices(Loops)) {
      const auto &loop = Loops[loopID];
      if (loop.isDeleted())
        continue;

      out << "- (#" << loopID << ") ";
      loop.dump(out, *this);
      out << "\n";
    }
    out << "}\n";
  }
  if (!WrittenRequirements.empty()) {
    out << "Written requirements: {\n";

    for (unsigned reqID : indices(WrittenRequirements)) {
      out << " - ID: " << reqID << " - ";
      const auto &requirement = WrittenRequirements[reqID];
      requirement.req.dump(out);
      out << " at ";
      requirement.loc.print(out, Context.getASTContext().SourceMgr);
      out << "\n";
    }
  }
  out << "}\n";
}
