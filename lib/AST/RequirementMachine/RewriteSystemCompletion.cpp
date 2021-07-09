//===--- RewriteSystemCompletion.cpp - Confluent completion procedure -----===//
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
// This implements completion in the rewriting system sense, not code
// completion.
//
// We use a variation of the Knuth-Bendix algorithm 
/// (https://en.wikipedia.org/wiki/Knuth–Bendix_completion_algorithm).
//
// The goal is to find 'overlapping' rules which would allow the same term to
// be rewritten in two different ways. These two different irreducible
// reductions are called a 'critical pair'; the completion procedure introduces
// new rewrite rules to eliminate critical pairs by rewriting one side of the
// pair to the other. This can introduce more overlaps with existing rules, and
// the process iterates until fixed point.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Defer.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <deque>
#include <vector>

#include "RewriteSystem.h"

using namespace swift;
using namespace rewriting;

/// For a superclass or concrete type atom
///
///   [concrete: Foo<X1, ..., Xn>]
///   [superclass: Foo<X1, ..., Xn>]
///
/// Return a new atom where the prefix T is prepended to each of the
/// substitutions:
///
///   [concrete: Foo<T.X1, ..., T.Xn>]
///   [superclass: Foo<T.X1, ..., T.Xn>]
///
/// Asserts if this is not a superclass or concrete type atom.
Atom Atom::prependPrefixToConcreteSubstitutions(
    const MutableTerm &prefix,
    RewriteContext &ctx) const {
  if (prefix.empty())
    return *this;

  return transformConcreteSubstitutions(
    [&](Term term) -> Term {
      MutableTerm mutTerm;
      mutTerm.append(prefix);
      mutTerm.append(term);

      return Term::get(mutTerm, ctx);
    }, ctx);
}

/// Check if this term overlaps with \p other for the purposes
/// of the Knuth-Bendix completion algorithm.
///
/// An overlap occurs if one of the following two cases holds:
///
/// 1) If this == TUV and other == U.
/// 2) If this == TU and other == UV.
///
/// In both cases, we return the subterms T and V, together with
/// an 'overlap kind' identifying the first or second case.
///
/// If both rules have identical left hand sides, either case could
/// apply, but we arbitrarily pick case 1.
///
/// Note that this relation is not commutative; we need to check
/// for overlap between both (X and Y) and (Y and X).
OverlapKind
MutableTerm::checkForOverlap(const MutableTerm &other,
                             MutableTerm &t,
                             MutableTerm &v) const {
  assert(!empty());
  assert(!other.empty());

  if (*this == other) {
    // If this term is equal to the other term, we have an overlap.
    t = MutableTerm();
    v = MutableTerm();
    return OverlapKind::First;
  }

  if (size() > other.size()) {
    // If this term is longer than the other term, check if it contains
    // the other term.
    auto first1 = begin();
    while (first1 <= end() - other.size()) {
      if (std::equal(other.begin(), other.end(), first1)) {
        // We have an overlap.
        t = MutableTerm(begin(), first1);
        v = MutableTerm(first1 + other.size(), end());

        // If both T and V are empty, we have two equal terms, which
        // should have been handled above.
        assert(!t.empty() || !v.empty());
        assert(t.size() + other.size() + v.size() == size());

        return OverlapKind::First;
      }

      ++first1;
    }
  }

  // Finally, check if a suffix of this term is equal to a prefix of
  // the other term.
  unsigned count = std::min(size(), other.size());
  auto first1 = end() - count;
  auto last2 = other.begin() + count;

  // Initial state, depending on size() <=> other.size():
  //
  // ABC   -- count = 3, first1 = this[0], last2 = other[3]
  // XYZ
  //
  // ABC   -- count = 2, first1 = this[1], last2 = other[2]
  //  XY
  //
  // ABC   -- count = 3, first1 = this[0], last2 = other[3]
  // XYZW

  // Advance by 1, since we don't need to check for full containment
  ++first1;
  --last2;

  while (last2 != other.begin()) {
    if (std::equal(other.begin(), last2, first1)) {
      t = MutableTerm(begin(), first1);
      v = MutableTerm(last2, other.end());

      assert(!t.empty());
      assert(!v.empty());
      assert(t.size() + other.size() - v.size() == size());

      return OverlapKind::Second;
    }

    ++first1;
    --last2;
  }

  return OverlapKind::None;
}

/// If we have two atoms [P:T] and [Q:T], produce a merged atom:
///
/// - If P inherits from Q, this is just [P:T].
/// - If Q inherits from P, this is just [Q:T].
/// - If P and Q are unrelated, this is [P&Q:T].
Atom RewriteSystem::mergeAssociatedTypes(Atom lhs, Atom rhs) const {
  // Check preconditions that were established by RewriteSystem::addRule().
  assert(lhs.getKind() == Atom::Kind::AssociatedType);
  assert(rhs.getKind() == Atom::Kind::AssociatedType);
  assert(lhs.getName() == rhs.getName());
  assert(lhs.compare(rhs, Protos) > 0);

  auto protos = lhs.getProtocols();
  auto otherProtos = rhs.getProtocols();

  // This must follow from lhs > rhs.
  assert(protos.size() <= otherProtos.size());

  // Compute sorted and merged list of protocols, with duplicates.
  llvm::TinyPtrVector<const ProtocolDecl *> newProtos;
  std::merge(protos.begin(), protos.end(),
             otherProtos.begin(), otherProtos.end(),
             std::back_inserter(newProtos),
             [&](const ProtocolDecl *lhs,
                 const ProtocolDecl *rhs) -> int {
               return Protos.compareProtocols(lhs, rhs) < 0;
             });

  // Prune duplicates and protocols that are inherited by other
  // protocols.
  llvm::TinyPtrVector<const ProtocolDecl *> minimalProtos;
  for (const auto *newProto : newProtos) {
    auto inheritsFrom = [&](const ProtocolDecl *thisProto) {
      return (thisProto == newProto ||
              Protos.inheritsFrom(thisProto, newProto));
    };

    if (std::find_if(minimalProtos.begin(), minimalProtos.end(),
                     inheritsFrom)
        == minimalProtos.end()) {
      minimalProtos.push_back(newProto);
    }
  }

  // The two input sets are minimal already, so the merged set
  // should have at least as many elements as the smallest
  // input set.
  assert(minimalProtos.size() >= std::min(protos.size(), otherProtos.size()));

  // The merged set cannot contain more elements than the union
  // of the two sets.
  assert(minimalProtos.size() <= protos.size() + otherProtos.size());

  return Atom::forAssociatedType(minimalProtos, lhs.getName(), Context);
}

/// Consider the following example:
///
///   protocol P1 { associatedtype T : P1 }
///   protocol P2 { associatedtype T : P2 }
///   struct G<T : P1 & P2> {}
///
/// We start with these rewrite rules:
///
///   [P1].T => [P1:T]
///   [P2].T => [P2:T]
///   [P1:T].[P1] => [P1:T]
///   [P2:T].[P1] => [P2:T]
///   <T>.[P1] => <T>
///   <T>.[P2] => <T>
///   <T>.T => <T>.[P1:T]
///   <T>.[P2:T] => <T>.[P1:T]
///
/// The completion procedure ends up adding an infinite series of rules of the
/// form
///
///   <T>.[P1:T].[P2]                 => <T>.[P1:T]
///   <T>.[P1:T].[P2:T]               => <T>.[P1:T].[P1:T]
///
///   <T>.[P1:T].[P1:T].[P2]          => <T>.[P1:T].[P1:T]
///   <T>.[P1:T].[P1:T].[P2:T]        => <T>.[P1:T].[P1:T].[P1:T]
///
///   <T>.[P1:T].[P1:T].[P1:T].[P2]   => <T>.[P1:T].[P1:T].[P1.T]
///   <T>.[P1:T].[P1:T].[P1:T].[P2:T] => <T>.[P1:T].[P1:T].[P1:T].[P1.T]
///
/// The difficulty here stems from the fact that an arbitrary sequence of
/// [P1:T] following a <T> is known to conform to P2, but P1:T itself
/// does not conform to P2.
///
/// We use a heuristic to compute a completion in this case by using
/// merged associated type terms.
///
/// The key is the following rewrite rule:
///
///   <T>.[P2:T] => <T>.[P1:T]
///
/// When we add this rule, we introduce a new merged atom [P1&P2:T] in
/// a pair of new rules:
///
///   <T>.[P1:T] => <T>.[P1&P2:T]
///   <T>.[P2:T] => <T>.[P1&P2:T]
///
/// We also look for any existing rules of the form [P1:T].[Q] => [P1:T]
/// or [P2:T].[Q] => [P2:T], and introduce a new rule:
///
///   [P1&P2:T].[Q] => [P1&P2:T]
///
/// In the above example, we have such a rule for Q == P1 and Q == P2, so
/// in total we end up adding the following four rules:
///
///   <T>.[P1:T] => <T>.[P1&P2:T]
///   <T>.[P2:T] => <T>.[P1&P2:T]
///   [P1&P2:T].[P1] => [P1&P2:T]
///   [P1&P2:T].[P2] => [P1&P2:T]
///
/// Intuitively, since the conformance requirements on the merged term
/// are not prefixed by the root <T>, they apply at any level; we've
/// "tied off" the recursion, and now the rewrite system has a confluent
/// completion.
void RewriteSystem::processMergedAssociatedTypes() {
  if (MergedAssociatedTypes.empty())
    return;

  unsigned i = 0;

  // Chase the end of the vector; calls to RewriteSystem::addRule()
  // can theoretically add new elements below.
  while (i < MergedAssociatedTypes.size()) {
    auto pair = MergedAssociatedTypes[i++];
    const auto &lhs = pair.first;
    const auto &rhs = pair.second;

    // If we have X.[P1:T] => Y.[P2:T], add a new pair of rules:
    // X.[P1:T] => X.[P1&P2:T]
    // X.[P2:T] => X.[P1&P2:T]
    if (DebugMerge) {
      llvm::dbgs() << "## Associated type merge candidate ";
      llvm::dbgs() << lhs << " => " << rhs << "\n";
    }

    auto mergedAtom = mergeAssociatedTypes(lhs.back(), rhs.back());
    if (DebugMerge) {
      llvm::dbgs() << "### Merged atom " << mergedAtom << "\n";
    }

    // Build the term X.[P1&P2:T].
    MutableTerm mergedTerm = lhs;
    mergedTerm.back() = mergedAtom;

    // Add the rule X.[P1:T] => X.[P1&P2:T].
    addRule(lhs, mergedTerm);

    // Add the rule X.[P1:T] => X.[P1&P2:T].
    addRule(rhs, mergedTerm);

    // Look for conformance requirements on [P1:T] and [P2:T].
    for (const auto &otherRule : Rules) {
      const auto &otherLHS = otherRule.getLHS();
      if (otherLHS.size() == 2 &&
          otherLHS[1].getKind() == Atom::Kind::Protocol) {
        if (otherLHS[0] == lhs.back() ||
            otherLHS[0] == rhs.back()) {
          // We have a rule of the form
          //
          //   [P1:T].[Q] => [P1:T]
          //
          // or
          //
          //   [P2:T].[Q] => [P2:T]
          if (DebugMerge) {
            llvm::dbgs() << "### Lifting conformance rule " << otherRule << "\n";
          }

          // We know that [P1:T] or [P2:T] conforms to Q, therefore the
          // merged type [P1&P2:T] must conform to Q as well. Add a new rule
          // of the form:
          //
          //   [P1&P2].[Q] => [P1&P2]
          //
          MutableTerm newLHS;
          newLHS.add(mergedAtom);
          newLHS.add(otherLHS[1]);

          MutableTerm newRHS;
          newRHS.add(mergedAtom);

          addRule(newLHS, newRHS);
        }
      }
    }
  }

  MergedAssociatedTypes.clear();
}

/// Compute a critical pair from two rewrite rules.
///
/// There are two cases:
///
/// 1) lhs == TUV -> X, rhs == U -> Y. The overlapped term is TUV;
///    applying lhs and rhs, respectively, yields the critical pair
///    (X, TYV).
///
/// 2) lhs == TU -> X, rhs == UV -> Y. The overlapped term is once
///    again TUV; applying lhs and rhs, respectively, yields the
///    critical pair (XV, TY).
///
/// If lhs and rhs have identical left hand sides, either case could
/// apply, but we arbitrarily pick case 1.
///
/// There is also an additional wrinkle. If we're in case 2, and the
/// last atom of V is a superclass or concrete type atom A, we prepend
/// T to each substitution of A.
///
/// For example, suppose we have the following two rules:
///
/// A.B -> C
/// B.[concrete: Foo<X>] -> B
///
/// The overlapped term is A.B.[concrete: Foo<X>], so the critical pair
/// is (C.[concrete: Foo<A.X>], A.B). We prepended 'A' to the
/// concrete substitution 'X' to get 'A.X'; the new concrete term
/// is now rooted at the same level as A.B in the rewrite system,
/// not just B.
Optional<std::pair<MutableTerm, MutableTerm>>
RewriteSystem::computeCriticalPair(const Rule &lhs, const Rule &rhs) const {
  MutableTerm t, v;

  switch (lhs.checkForOverlap(rhs, t, v)) {
  case OverlapKind::None:
    return None;

  case OverlapKind::First: {
    // lhs == TUV -> X, rhs == U -> Y.

    // Note: This includes the case where the two rules have exactly
    // equal left hand sides; that is, lhs == U -> X, rhs == U -> Y.
    //
    // In this case, T and V are both empty.

    // Compute the term TYV.
    t.append(rhs.getRHS());
    t.append(v);
    return std::make_pair(lhs.getRHS(), t);
  }

  case OverlapKind::Second: {
    // lhs == TU -> X, rhs == UV -> Y.

    if (v.back().isSuperclassOrConcreteType()) {
      v.back() = v.back().prependPrefixToConcreteSubstitutions(
          t, Context);
    }

    // Compute the term XV.
    MutableTerm xv;
    xv.append(lhs.getRHS());
    xv.append(v);

    // Compute the term TY.
    t.append(rhs.getRHS());
    return std::make_pair(xv, t);
  }
  }

  llvm_unreachable("Bad overlap kind");
}

/// Computes the confluent completion using the Knuth-Bendix algorithm.
///
/// Returns a pair consisting of a status and number of iterations executed.
///
/// The status is CompletionResult::MaxIterations if we exceed \p maxIterations
/// iterations.
///
/// The status is CompletionResult::MaxDepth if we produce a rewrite rule whose
/// left hand side has a length exceeding \p maxDepth.
///
/// Otherwise, the status is CompletionResult::Success.
std::pair<RewriteSystem::CompletionResult, unsigned>
RewriteSystem::computeConfluentCompletion(unsigned maxIterations,
                                          unsigned maxDepth) {
  unsigned steps = 0;

  // The worklist must be processed in first-in-first-out order, to ensure
  // that we resolve all overlaps among the initial set of rules before
  // moving on to overlaps between rules introduced by completion.
  while (!Worklist.empty()) {
    // Check if we've already done too much work.
    if (Rules.size() > maxIterations)
      return std::make_pair(CompletionResult::MaxIterations, steps);

    auto next = Worklist.front();
    Worklist.pop_front();

    const auto &lhs = Rules[next.first];
    const auto &rhs = Rules[next.second];

    if (DebugCompletion) {
      llvm::dbgs() << "$ Check for overlap: (#" << next.first << ") ";
      llvm::dbgs() << lhs << "\n";
      llvm::dbgs() << "                -vs- (#" << next.second << ") ";
      llvm::dbgs() << rhs << ":\n";
    }

    auto pair = computeCriticalPair(lhs, rhs);
    if (!pair) {
      if (DebugCompletion) {
        llvm::dbgs() << " no overlap\n\n";
      }
      continue;
    }

    MutableTerm first, second;

    // We have a critical pair (X, Y).
    std::tie(first, second) = *pair;

    if (DebugCompletion) {
      llvm::dbgs() << "$$ First term of critical pair is " << first << "\n";
      llvm::dbgs() << "$$ Second term of critical pair is " << second << "\n\n";
    }
    unsigned i = Rules.size();

    // Try to repair the confluence violation by adding a new rule
    // X == Y.
    if (!addRule(first, second))
      continue;

    // Only count a 'step' once we add a new rule.
    ++steps;

    const auto &newRule = Rules[i];
    if (newRule.getDepth() > maxDepth)
      return std::make_pair(CompletionResult::MaxDepth, steps);

    // Check if the new rule X == Y obsoletes any existing rules.
    for (unsigned j : indices(Rules)) {
      // A rule does not obsolete itself.
      if (i == j)
        continue;

      auto &rule = Rules[j];

      // Ignore rules that have already been obsoleted.
      if (rule.isDeleted())
        continue;

      // If this rule reduces some existing rule, delete the existing rule.
      if (rule.canReduceLeftHandSide(newRule)) {
        if (DebugCompletion) {
          llvm::dbgs() << "$ Deleting rule " << rule << " because "
                       << "its left hand side contains " << newRule
                       << "\n";
        }
        rule.markDeleted();
      }
    }

    // If this new rule merges any associated types, process the merge now
    // before we continue with the completion procedure. This is important
    // to perform incrementally since merging is required to repair confluence
    // violations.
    processMergedAssociatedTypes();
  }

  return std::make_pair(CompletionResult::Success, steps);
}
