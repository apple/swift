//===--- GeneratingConformances.cpp - Reasoning about conformance rules ---===//
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
// This file implements an algorithm to find a minimal set of "generating
// conformances", which are rules (V1.[P1] => V1), ..., (Vn.[Pn] => Vn) such
// that any valid term of the form T.[P] can be written as a product of terms
// (Vi.[Pi]), where each Vi.[Pi] is a left hand side of a generating
// conformance.
//
// A "conformance-valid" rewrite system is one where if we can write
// T == U.V for arbitrary non-empty U and V, then U.[domain(V)] is joinable
// with U.
//
// If this holds, then starting with a term T.[P] that is joinable with T, we
// can reduce T to canonical form T', and find the unique rule (V.[P] => V) such
// that T' == U.V. Then we repeat this process with U.[domain(V)], which is
// known to be joinable with U, since T is conformance-valid.
//
// Iterating this process produces a decomposition of T.[P] as a product of
// left hand sides of conformance rules. Some of those rules are not minimal;
// they are added by completion, or they are redundant rules written by the
// user.
//
// Using the 3-cells that generate the homotopy relation on rewrite paths,
// decompositions can be found for all "derived" conformance rules, producing
// a minimal set of generating conformances.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Decl.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Range.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include "RewriteSystem.h"

using namespace swift;
using namespace rewriting;

/// Finds all protocol conformance rules appearing in a 3-cell, both without
/// context, and with a non-empty left context. Applications of rules with a
/// non-empty right context are ignored.
///
/// The rules are organized by protocol. For each protocol, the first element
/// of the pair stores conformance rules that appear without context. The
/// second element of the pair stores rules that appear with non-empty left
/// context. For each such rule, the left prefix is also stored alongside.
void HomotopyGenerator::findProtocolConformanceRules(
    llvm::SmallDenseMap<const ProtocolDecl *,
                        std::pair<SmallVector<unsigned, 2>,
                                  SmallVector<std::pair<MutableTerm, unsigned>, 2>>>
                        &result,
    const RewriteSystem &system) const {

  auto redundantRules = Path.findRulesAppearingOnceInEmptyContext();

  bool foundAny = false;
  for (unsigned ruleID : redundantRules) {
    const auto &rule = system.getRule(ruleID);
    if (auto *proto = rule.isProtocolConformanceRule()) {
      result[proto].first.push_back(ruleID);
      foundAny = true;
    }
  }

  if (!foundAny)
    return;

  MutableTerm term = Basepoint;

  // Now look for rewrite steps with conformance rules in empty right context,
  // that is something like X.(Y.[P] => Z) (or it's inverse, X.(Z => Y.[P])).
  for (const auto &step : Path) {
    switch (step.Kind) {
    case RewriteStep::ApplyRewriteRule: {
      const auto &rule = system.getRule(step.RuleID);
      if (auto *proto = rule.isProtocolConformanceRule()) {
        if (step.StartOffset > 0 &&
            step.EndOffset == 0) {
          // Record the prefix term that is left unchanged by this rewrite step.
          //
          // In the above example where the rewrite step is X.(Y.[P] => Z),
          // the prefix term is 'X'.
          MutableTerm prefix(term.begin(), term.begin() + step.StartOffset);
          result[proto].second.emplace_back(prefix, step.RuleID);
        }
      }

      break;
    }

    case RewriteStep::AdjustConcreteType:
      break;
    }

    step.apply(term, system);
  }
}

/// Write the term as a product of left hand sides of protocol conformance
/// rules.
///
/// The term should be irreducible, except for a protocol symbol at the end.
void
RewriteSystem::decomposeTermIntoConformanceRuleLeftHandSides(
    MutableTerm term, SmallVectorImpl<unsigned> &result) const {
  assert(term.back().getKind() == Symbol::Kind::Protocol);

  // If T is canonical and T.[P] => T, then by confluence, T.[P]
  // reduces to T in a single step, via a rule V.[P] => V, where
  // T == U.V.
  RewritePath steps;
  bool simplified = simplify(term, &steps);
  if (!simplified) {
    llvm::errs() << "Term does not conform to protocol: " << term << "\n";
    abort();
  }

  assert(steps.size() == 1 &&
         "Canonical conformance term should simplify in one step");

  const auto &step = *steps.begin();
  assert(step.Kind == RewriteStep::ApplyRewriteRule);
  assert(step.EndOffset == 0);
  assert(!step.Inverse);

  // If |U| > 0, recurse with the term U.[domain(V)]. Since T is
  // canonical, we know that U is canonical as well.
  if (step.StartOffset > 0) {
    // Build the term U.
    MutableTerm prefix(term.begin(), term.begin() + step.StartOffset);

    decomposeTermIntoConformanceRuleLeftHandSides(prefix, step.RuleID, result);
  } else {
    result.push_back(step.RuleID);
  }
}

/// Given a term U and a rule (V.[P] => V), write U.[domain(V)] as a
/// product of left hand sdies of conformance rules. The term U should
/// be irreducible.
void
RewriteSystem::decomposeTermIntoConformanceRuleLeftHandSides(
    MutableTerm term, unsigned ruleID,
    SmallVectorImpl<unsigned> &result) const {
  const auto &rule = getRule(ruleID);
  assert(rule.isProtocolConformanceRule());

  // Compute domain(V).
  const auto &lhs = rule.getLHS();
  auto protocols = lhs[0].getProtocols();
  assert(protocols.size() == 1);
  auto protocol = Symbol::forProtocol(protocols[0], Context);

  // A same-type requirement of the form 'Self.Foo == Self' can induce a
  // conformance rule [P].[P] => [P], and we can end up with a generating
  // conformance decomposition of the form
  //
  //   (V.[Q] => V) := [P].(V'.[Q] => V'),
  //
  // where domain(V) == [P]. Don't recurse on [P].[P] here since it won't
  // yield anything useful, instead just return with (V'.[Q] => V').
  if (term.size() == 1 && term[0] == protocol) {
    result.push_back(ruleID);
    return;
  }

  // Build the term U.[domain(V)].
  term.add(protocol);

  decomposeTermIntoConformanceRuleLeftHandSides(term, result);

  // Add the rule V => V.[P].
  result.push_back(ruleID);
}

/// Use homotopy information to discover all ways of writing the left hand side
/// of each conformance rule as a product of left hand sides of other conformance
/// rules.
///
/// Each conformance rule (Vi.[P] => Vi) can always be written in terms of itself,
/// so the first term of each disjunction is always (Vi.[P] => Vi).
///
/// Conformance rules can also be circular, so not every choice of disjunctions
/// produces a valid result; for example, if you have these definitions:
///
///   protocol P {
///     associatedtype T : P
///   }
///
///   struct G<X, Y> where X : P, X.T == Y, Y : P, Y.T == X {}
///
/// We have three conformance rules:
///
///   [P:T].[P] => [P:T]
///   <X>.[P] => <X>
///   <Y>.[P] => <Y>
///
/// The first rule, <X>.[P] => <X> has an alternate conformance path:
///
///   (<Y>.[P]).([P:T].[P])
///
/// The second rule similarly has an alternate conformance path:
///
///   (<X>.[P]).([P:T].[P])
///
/// This gives us the following initial set of candidate conformance paths:
///
///   [P:T].[P] := ([P:T].[P])
///   <X>.[P] := (<X>.[P]) ∨ (<Y>.[P]).([P:T].[P])
///   <Y>.[P] := (<Y>.[P]) ∨ (<X>.[P]).([P:T].[P])
///
/// One valid solution is the following set of assignments:
///
///   [P:T].[P] := ([P:T].[P])
///   <X>.[P] := (<X>.[P])
///   <Y>.[P] := (<X>.[P]).([P:T].[P])
///
/// That is, we can choose to eliminate <X>.[P], but not <Y>.[P], or vice
/// versa; but it is never valid to eliminate both.
void RewriteSystem::computeCandidateConformancePaths(
    llvm::MapVector<unsigned,
                    std::vector<SmallVector<unsigned, 2>>> &conformancePaths) const {
  for (const auto &loop : HomotopyGenerators) {
    if (loop.isDeleted())
      continue;

    llvm::SmallDenseMap<const ProtocolDecl *,
                        std::pair<SmallVector<unsigned, 2>,
                                  SmallVector<std::pair<MutableTerm, unsigned>, 2>>>
        result;

    loop.findProtocolConformanceRules(result, *this);

    if (result.empty())
      continue;

    if (Debug.contains(DebugFlags::GeneratingConformances)) {
      llvm::dbgs() << "Candidate homotopy generator: ";
      loop.dump(llvm::dbgs(), *this);
      llvm::dbgs() << "\n";
    }

    for (const auto &pair : result) {
      const auto *proto = pair.first;
      const auto &notInContext = pair.second.first;
      const auto &inContext = pair.second.second;

      // No rules appear without context.
      if (notInContext.empty())
        continue;

      // No replacement rules.
      if (notInContext.size() == 1 && inContext.empty())
        continue;

      if (Debug.contains(DebugFlags::GeneratingConformances)) {
        llvm::dbgs() << "* Protocol " << proto->getName() << ":\n";
        llvm::dbgs() << "** Conformance rules not in context:\n";
        for (unsigned ruleID : notInContext) {
          llvm::dbgs() << "-- (#" << ruleID << ") " << getRule(ruleID) << "\n";
        }

        llvm::dbgs() << "** Conformance rules in context:\n";
        for (auto pair : inContext) {
          llvm::dbgs() << "-- " << pair.first;
          unsigned ruleID = pair.second;
          llvm::dbgs() << " (#" << ruleID << ") " << getRule(ruleID) << "\n";
        }

        llvm::dbgs() << "\n";
      }

      // Suppose a 3-cell contains a conformance rule (T.[P] => T) in an empty
      // context, and a conformance rule (V.[P] => V) with a possibly non-empty
      // left context U and empty right context.
      //
      // We can decompose U into a product of conformance rules:
      //
      //    (V1.[P1] => V1)...(Vn.[Pn] => Vn),
      //
      // Now, we can record a candidate decomposition of (T.[P] => T) as a
      // product of conformance rules:
      //
      //    (T.[P] => T) := (V1.[P1] => V1)...(Vn.[Pn] => Vn).(V.[P] => V)
      //
      // Now if U is empty, this becomes the trivial candidate:
      //
      //    (T.[P] => T) := (V.[P] => V)
      SmallVector<SmallVector<unsigned, 2>, 2> candidatePaths;
      for (auto pair : inContext) {
        // We have a term U, and a rule V.[P] => V.
        SmallVector<unsigned, 2> conformancePath;

        // Simplify U to get U'.
        MutableTerm term = pair.first;
        (void) simplify(term);

        // Write U'.[domain(V)] as a product of left hand sides of protocol
        // conformance rules.
        decomposeTermIntoConformanceRuleLeftHandSides(term, pair.second,
                                                      conformancePath);

        candidatePaths.push_back(conformancePath);
      }

      for (unsigned candidateRuleID : notInContext) {
        // If multiple conformance rules appear in an empty context, each one
        // can be replaced with any other conformance rule.
        for (unsigned otherRuleID : notInContext) {
          if (otherRuleID == candidateRuleID)
            continue;

          SmallVector<unsigned, 2> path;
          path.push_back(otherRuleID);
          conformancePaths[candidateRuleID].push_back(path);
        }

        // If conformance rules appear in non-empty context, they define a
        // conformance access path for each conformance rule in empty context.
        for (const auto &path : candidatePaths) {
          conformancePaths[candidateRuleID].push_back(path);
        }
      }
    }
  }
}

/// Determines if \p path can be expressed without any of the conformance
/// rules appearing in \p redundantConformances, by possibly substituting
/// any occurrences of the redundant rules with alternate definitions
/// appearing in \p conformancePaths.
///
/// The \p conformancePaths map sends conformance rules to a list of
/// disjunctions, where each disjunction is a product of other conformance
/// rules.
bool RewriteSystem::isValidConformancePath(
    llvm::SmallDenseSet<unsigned, 4> &visited,
    llvm::DenseSet<unsigned> &redundantConformances,
    const llvm::SmallVectorImpl<unsigned> &path,
    const llvm::MapVector<unsigned, SmallVector<unsigned, 2>> &parentPaths,
    const llvm::MapVector<unsigned,
                          std::vector<SmallVector<unsigned, 2>>>
        &conformancePaths) const {
  for (unsigned ruleID : path) {
    if (visited.count(ruleID) > 0)
      return false;

    if (redundantConformances.count(ruleID)) {
      SWIFT_DEFER {
        visited.erase(ruleID);
      };
      visited.insert(ruleID);

      auto found = conformancePaths.find(ruleID);
      assert(found != conformancePaths.end());

      bool foundValidConformancePath = false;
      for (const auto &otherPath : found->second) {
        if (isValidConformancePath(visited, redundantConformances, otherPath,
                                   parentPaths, conformancePaths)) {
          foundValidConformancePath = true;
          break;
        }
      }

      if (!foundValidConformancePath)
        return false;
    }

    auto found = parentPaths.find(ruleID);
    if (found != parentPaths.end()) {
      SWIFT_DEFER {
        visited.erase(ruleID);
      };
      visited.insert(ruleID);

      // If 'req' is based on some other conformance requirement
      // `T.[P.]A : Q', we want to make sure that we have a
      // non-redundant derivation for 'T : P'.
      if (!isValidConformancePath(visited, redundantConformances, found->second,
                                  parentPaths, conformancePaths)) {
        return false;
      }
    }
  }

  return true;
}

/// Rules of the form [P].[Q] => [P] encode protocol refinement and can only
/// be redundant if they're equivalent to a sequence of other protocol
/// refinements.
///
/// This helps ensure that the inheritance clause of a protocol is complete
/// and correct, allowing name lookup to find associated types of inherited
/// protocols while building the protocol requirement signature.
bool RewriteSystem::isValidRefinementPath(
    const llvm::SmallVectorImpl<unsigned> &path) const {
  for (unsigned ruleID : path) {
    if (!getRule(ruleID).isProtocolRefinementRule())
      return false;
  }

  return true;
}

void RewriteSystem::dumpConformancePath(
    llvm::raw_ostream &out,
    const SmallVectorImpl<unsigned> &path) const {
  for (unsigned ruleID : path)
    out << "(" << getRule(ruleID).getLHS() << ")";
}

void RewriteSystem::dumpGeneratingConformanceEquation(
    llvm::raw_ostream &out,
    unsigned baseRuleID,
    const std::vector<SmallVector<unsigned, 2>> &paths) const {
  out << getRule(baseRuleID).getLHS() << " := ";

  bool first = true;
  for (const auto &path : paths) {
    if (!first)
      out << " ∨ ";
    else
      first = false;

    dumpConformancePath(out, path);
  }
}

void RewriteSystem::verifyGeneratingConformanceEquations(
    const llvm::MapVector<unsigned,
                          std::vector<SmallVector<unsigned, 2>>>
        &conformancePaths) const {
#ifndef NDEBUG
  for (const auto &pair : conformancePaths) {
    const auto &rule = getRule(pair.first);
    auto *proto = rule.getLHS().back().getProtocol();

    MutableTerm baseTerm(rule.getLHS());
    (void) simplify(baseTerm);

    for (const auto &path : pair.second) {
      const auto &otherRule = getRule(path.back());
      auto *otherProto = otherRule.getLHS().back().getProtocol();

      if (proto != otherProto) {
        llvm::errs() << "Invalid equation: ";
        dumpGeneratingConformanceEquation(llvm::errs(),
                                          pair.first, pair.second);
        llvm::errs() << "\n";
        llvm::errs() << "Mismatched conformance:\n";
        llvm::errs() << "Base rule: " << rule << "\n";
        llvm::errs() << "Final rule: " << otherRule << "\n\n";
        dump(llvm::errs());
        abort();
      }

      MutableTerm otherTerm;
      for (unsigned otherRuleID : path) {
        otherTerm.append(getRule(otherRuleID).getLHS());
      }

      (void) simplify(otherTerm);

      if (baseTerm != otherTerm) {
        llvm::errs() << "Invalid equation: ";
        llvm::errs() << "\n";
        dumpGeneratingConformanceEquation(llvm::errs(),
                                          pair.first, pair.second);
        llvm::errs() << "Invalid conformance path:\n";
        llvm::errs() << "Expected: " << baseTerm << "\n";
        llvm::errs() << "Got: " << otherTerm << "\n\n";
        dump(llvm::errs());
        abort();
      }
    }
  }
#endif
}

/// Computes a minimal set of generating conformances, assuming that homotopy
/// reduction has already eliminated all redundant rewrite rules that are not
/// conformance rules.
void RewriteSystem::computeGeneratingConformances(
    llvm::DenseSet<unsigned> &redundantConformances) {
  // Maps a conformance rule to a conformance path deriving the subject type's
  // base type. For example, consider the following conformance rule:
  //
  //   T.[P:A].[Q:B].[R] => T.[P:A].[Q:B]
  //
  // The subject type is T.[P:A].[Q:B]; in order to derive the metadata, we need
  // the witness table for T.[P:A] : [Q] first, by computing a conformance access
  // path for the term T.[P:A].[Q], known as the 'parent path'.
  llvm::MapVector<unsigned, SmallVector<unsigned, 2>> parentPaths;

  // Maps a conformance rule to a list of paths. Each path in the list is a unique
  // derivation of the conformance in terms of other conformance rules.
  llvm::MapVector<unsigned, std::vector<SmallVector<unsigned, 2>>> conformancePaths;

  // The set of conformance rules which are protocol refinements, that is rules of
  // the form [P].[Q] => [P].
  llvm::DenseSet<unsigned> protocolRefinements;

  // Prepare the initial set of equations: every non-redundant conformance rule
  // can be expressed as itself.
  for (unsigned ruleID : indices(Rules)) {
    const auto &rule = getRule(ruleID);
    if (rule.isRedundant())
      continue;

    if (!rule.isProtocolConformanceRule())
      continue;

    SmallVector<unsigned, 2> path;
    path.push_back(ruleID);
    conformancePaths[ruleID].push_back(path);

    if (rule.isProtocolRefinementRule()) {
      protocolRefinements.insert(ruleID);
      continue;
    }

    auto lhs = rule.getLHS();

    auto parentSymbol = lhs[lhs.size() - 2];

    // The last element is a protocol symbol, because this is a conformance rule.
    // The second to last symbol is either an associated type, protocol or generic
    // parameter symbol.
    switch (parentSymbol.getKind()) {
    case Symbol::Kind::AssociatedType: {
      // If we have a rule of the form X.[P:Y].[Q] => X.[P:Y] wih non-empty X,
      // then the parent type is X.[P].
      if (lhs.size() == 2)
        continue;

      MutableTerm mutTerm(lhs.begin(), lhs.end() - 2);
      assert(!mutTerm.empty());

      const auto protos = parentSymbol.getProtocols();
      assert(protos.size() == 1);

      bool simplified = simplify(mutTerm);
      assert(!simplified || rule.isSimplified());
      (void) simplified;

      mutTerm.add(Symbol::forProtocol(protos[0], Context));

      // Get a conformance path for X.[P] and record it.
      decomposeTermIntoConformanceRuleLeftHandSides(mutTerm, parentPaths[ruleID]);
      continue;
    }

    case Symbol::Kind::GenericParam:
    case Symbol::Kind::Protocol:
      // Don't record a parent path, since the parent type is trivial (either a
      // generic parameter, or the protocol 'Self' type).
      continue;

    case Symbol::Kind::Name:
    case Symbol::Kind::Layout:
    case Symbol::Kind::Superclass:
    case Symbol::Kind::ConcreteType:
      break;
    }

    llvm_unreachable("Bad symbol kind");
  }

  computeCandidateConformancePaths(conformancePaths);

  if (Debug.contains(DebugFlags::GeneratingConformances)) {
    llvm::dbgs() << "Initial set of equations:\n";
    for (const auto &pair : conformancePaths) {
      llvm::dbgs() << "- ";
      dumpGeneratingConformanceEquation(llvm::dbgs(),
                                        pair.first, pair.second);
      llvm::dbgs() << "\n";
    }

    llvm::dbgs() << "Parent paths:\n";
    for (const auto &pair : parentPaths) {
      llvm::dbgs() << "- " << getRule(pair.first).getLHS() << ": ";
      dumpConformancePath(llvm::dbgs(), pair.second);
      llvm::dbgs() << "\n";
    }
  }

  verifyGeneratingConformanceEquations(conformancePaths);

  // Find a minimal set of generating conformances.
  for (const auto &pair : conformancePaths) {
    bool isProtocolRefinement = protocolRefinements.count(pair.first) > 0;

    for (const auto &path : pair.second) {
      // Only consider a protocol refinement rule to be redundant if it is
      // witnessed by a composition of other protocol refinement rules.
      if (isProtocolRefinement && !isValidRefinementPath(path))
        continue;

      llvm::SmallDenseSet<unsigned, 4> visited;
      visited.insert(pair.first);

      if (isValidConformancePath(visited, redundantConformances, path,
                                 parentPaths, conformancePaths)) {
        redundantConformances.insert(pair.first);
        break;
      }
    }
  }

  // Check invariants.
#ifndef NDEBUG
  for (const auto &pair : conformancePaths) {
    if (redundantConformances.count(pair.first) > 0)
      continue;

    const auto &rule = getRule(pair.first);

    if (rule.isRedundant()) {
      llvm::errs() << "Generating conformance is redundant: ";
      llvm::errs() << rule << "\n\n";
      dump(llvm::errs());
      abort();
    }

    if (rule.getLHS().containsUnresolvedSymbols()) {
      llvm::errs() << "Generating conformance contains unresolved symbols: ";
      llvm::errs() << rule << "\n\n";
      dump(llvm::errs());
      abort();
    }
  }
#endif

  if (Debug.contains(DebugFlags::GeneratingConformances)) {
    llvm::dbgs() << "Generating conformances:\n";

    for (const auto &pair : conformancePaths) {
      if (redundantConformances.count(pair.first) > 0)
        continue;

      llvm::dbgs() << "- " << getRule(pair.first) << "\n";
    }
  }
}