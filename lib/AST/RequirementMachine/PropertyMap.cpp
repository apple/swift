//===--- PropertyMap.cpp - Collects properties of type parameters ---------===//
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
// The property map is used to answer generic signature queries. It also
// implements special behaviors of layout, superclass, and concrete type
// requirements in the Swift language.
//
// # Property map construction
//
// Property map construction can add new rewrite rules when performing
// property unification and nested type concretization, so it is iterated
// until fixed point with the Knuth-Bendix algorithm. A third step, known as
// substitution simplification is also performed.
//
// The Knuth-Bendix completion procedure is implemented in KnuthBendix.cpp.
// Substitution simplification is implemented in SimplifySubstitutions.cpp.
//
// # Property map theory
//
// In the rewrite system, a conformance requirement 'T : P' is represented as
// rewrite rule of the form:
//
//    T.[P] => T
//
// Similarly, layout, superclass, and concrete-type requirements are represented
// by a rewrite rule of the form:
//
//    T.[p] => T
//
// Where [p] is a "property symbol": [layout: L], [superclass: Foo],
// [concrete: Bar].
//
// Given an arbitrary type T and a property [p], we can check if T satisfies the
// property by checking if the two terms T.[p] and T reduce to the same term T'.
// That is, if our rewrite rules allow us to eliminate the [p] suffix, we know
// the type satisfies [p].
//
// However, the question then becomes, given an arbitrary type T, how do we find
// *all* properties [p] satisfied by T?
//
// The trick is that we can take advantage of confluence here.
//
// If T.[p] => T', and T => T'', then it must follow that T''.[p] => T'.
// Furthermore, since T'' is fully reduced, T'' == T'. So T'' == UV for some
// terms U and V, and there exist be a rewrite rule V.[p] => V' in the system.
//
// Therefore, in order to find all [p] satisfied by T, we start by fully reducing
// T, then we look for rules of the form V.[p] => V' where V is fully reduced,
// and a suffix of T.
//
// This is the idea behind the property map. We collect all rules of the form
// V.[p] => V into a multi-map keyed by V. Then given an arbitrary type T,
// we can reduce it and look up successive suffixes to find all properties [p]
// satisfied by T.
//
// # Property map implementation
//
// A set of property rules (V.[p1] => V), (V.[p2] => V), ... become a single
// entry in the property map corresponding to V that stores information about
// the properties [pN].
//
// The property map is indexed by a suffix trie, where the properties of a term
// T are found by traversing a trie, starting from the _last_ symbol of T, which
// is a key for the root of the trie. This is done since we might have an entry
// for a suffix of T, but not T itself.
//
// For example, if a conformance requirement 'A : Q' in protocol P becomes a
// rule ([P:A].[Q] => [P:A]). The term τ_0_0.[P:A], corresponding to the nested
// type 'A' of a generic parameter 'τ_0_0', might not have a property map entry
// of its own, if the only requirements on it are those implied by [P:A].
//
// In this case, a property map lookup for τ_0_0.[P:A] will find an entry for
// the term [P:A].
//
// If multiple suffixes of a term T appear in the property map, the lookup
// returns the entry for the _longest_ matching suffix. An important invariant
// maintained during property map construction is that the contents of a
// property map entry from a key V are copied into the entry for a key T
// where T == U.V for some U. This means property map entries for longer
// suffixes "inherit" the contents of entries for shorter suffixes.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Decl.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Types.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <vector>

#include "PropertyMap.h"

using namespace swift;
using namespace rewriting;

void PropertyBag::dump(llvm::raw_ostream &out) const {
  out << Key << " => {";

  if (!ConformsTo.empty()) {
    out << " conforms_to: [";
    bool first = true;
    for (const auto *proto : ConformsTo) {
      if (first)
        first = false;
      else
        out << " ";
      out << proto->getName();
    }
    out << "]";
  }

  if (Layout) {
    out << " layout: " << Layout;
  }

  if (hasSuperclassBound()) {
    const auto &superclassReq = getSuperclassRequirement();
    out << " superclass: " << *superclassReq.SuperclassType;
  }

  if (isConcreteType()) {
    out << " concrete_type: " << *ConcreteType;
  }

  out << " }";
}

/// Given a term \p lookupTerm whose suffix must equal this property bag's
/// key, return a new term with that suffix stripped off. Will be empty if
/// \p lookupTerm exactly equals the key.
MutableTerm
PropertyBag::getPrefixAfterStrippingKey(const MutableTerm &lookupTerm) const {
  assert(lookupTerm.size() >= Key.size());
  auto prefixBegin = lookupTerm.begin();
  auto prefixEnd = lookupTerm.end() - Key.size();
  assert(std::equal(prefixEnd, lookupTerm.end(), Key.begin()) &&
         "This is not the bag you're looking for");
  return MutableTerm(prefixBegin, prefixEnd);
}

/// Get the superclass bound for \p lookupTerm, whose suffix must be the term
/// represented by this property bag.
///
/// The original \p lookupTerm is important in case the concrete type has
/// substitutions. For example, if \p lookupTerm is [P:A].[U:B], and this
/// property bag records that the suffix [U:B] has a superclass symbol
/// [superclass: Cache<τ_0_0> with <[U:C]>], then we actually need to
/// apply the substitution τ_0_0 := [P:A].[U:C] to the type 'Cache<τ_0_0>'.
///
/// Asserts if this property bag does not have a superclass bound.
Type PropertyBag::getSuperclassBound(
    ArrayRef<GenericTypeParamType *> genericParams,
    const MutableTerm &lookupTerm,
    const PropertyMap &map) const {
  MutableTerm prefix = getPrefixAfterStrippingKey(lookupTerm);

  const auto &req = getSuperclassRequirement();
  return map.getTypeFromSubstitutionSchema(req.SuperclassType->getConcreteType(),
                                           req.SuperclassType->getSubstitutions(),
                                           genericParams, prefix);
}

/// Get the concrete type of the term represented by this property bag.
///
/// The original \p lookupTerm is important in case the concrete type has
/// substitutions. For example, if \p lookupTerm is [P:A].[U:B], and this
/// property bag records that the suffix [U:B] has a concrete type symbol
/// [concrete: Array<τ_0_0> with <[U:C]>], then we actually need to
/// apply the substitution τ_0_0 := [P:A].[U:C] to the type 'Array<τ_0_0>'.
///
/// Asserts if this property bag is not concrete.
Type PropertyBag::getConcreteType(
    ArrayRef<GenericTypeParamType *> genericParams,
    const MutableTerm &lookupTerm,
    const PropertyMap &map) const {
  MutableTerm prefix = getPrefixAfterStrippingKey(lookupTerm);
  return map.getTypeFromSubstitutionSchema(ConcreteType->getConcreteType(),
                                           ConcreteType->getSubstitutions(),
                                           genericParams, prefix);
}

void PropertyBag::copyPropertiesFrom(const PropertyBag *next,
                                     RewriteContext &ctx) {
  // If this is the property bag of T and 'next' is the
  // property bag of V, then T := UV for some non-empty U.
  int prefixLength = Key.size() - next->Key.size();
  assert(prefixLength > 0);
  assert(std::equal(Key.begin() + prefixLength, Key.end(),
                    next->Key.begin()));

  // Conformances and the layout constraint, if any, can be copied over
  // unmodified.
  ConformsTo = next->ConformsTo;
  ConformsToRules = next->ConformsToRules;
  Layout = next->Layout;
  LayoutRule = next->LayoutRule;

  // If the property bag of V has superclass or concrete type
  // substitutions {X1, ..., Xn}, then the property bag of
  // T := UV should have substitutions {UX1, ..., UXn}.
  MutableTerm prefix(Key.begin(), Key.begin() + prefixLength);

  if (next->ConcreteType) {
    ConcreteType = next->ConcreteType->prependPrefixToConcreteSubstitutions(
        prefix, ctx);
    ConcreteTypeRules = next->ConcreteTypeRules;
    for (auto &pair : ConcreteTypeRules) {
      pair.first = pair.first.prependPrefixToConcreteSubstitutions(
          prefix, ctx);
    }
  }

  // Copy over class hierarchy information.
  SuperclassDecl = next->SuperclassDecl;
  if (!next->Superclasses.empty()) {
    Superclasses = next->Superclasses;

    for (auto &req : Superclasses) {
      req.second.SuperclassType =
          req.second.SuperclassType->prependPrefixToConcreteSubstitutions(
              prefix, ctx);
      for (auto &pair : req.second.SuperclassRules) {
        pair.first = pair.first.prependPrefixToConcreteSubstitutions(
            prefix, ctx);
      }
    }
  }
}

Symbol PropertyBag::concretelySimplifySubstitution(const MutableTerm &mutTerm,
                                                   RewriteContext &ctx,
                                                   RewritePath *path) const {
  assert(!ConcreteTypeRules.empty());
  auto &pair = ConcreteTypeRules.front();

  // The property map entry might apply to a suffix of the substitution
  // term, so prepend the appropriate prefix to its own substitutions.
  auto prefix = getPrefixAfterStrippingKey(mutTerm);
  auto concreteSymbol =
    pair.first.prependPrefixToConcreteSubstitutions(
        prefix, ctx);

  // If U.V is the substitution term and V is the property map key,
  // apply the rewrite step U.(V => V.[concrete: C]) followed by
  // prepending the prefix U to each substitution in the concrete type
  // symbol if |U| > 0.
  if (path) {
    path->add(RewriteStep::forRewriteRule(/*startOffset=*/prefix.size(),
                                          /*endOffset=*/0,
                                          /*ruleID=*/pair.second,
                                          /*inverse=*/true));

    if (!prefix.empty()) {
      path->add(RewriteStep::forPrefixSubstitutions(/*length=*/prefix.size(),
                                                    /*endOffset=*/0,
                                                    /*inverse=*/false));
    }
  }

  return concreteSymbol;
}

void PropertyBag::verify(const RewriteSystem &system) const {
#ifndef NDEBUG
  assert(ConformsTo.size() == ConformsToRules.size());
  for (unsigned i : indices(ConformsTo)) {
    auto symbol = system.getRule(ConformsToRules[i]).getLHS().back();
    assert(symbol.getKind() == Symbol::Kind::Protocol);
    assert(symbol.getProtocol() == ConformsTo[i]);
  }

  // FIXME: Add asserts requiring that the layout, superclass and
  // concrete type symbols match, as above
  assert(!Layout.isNull() == LayoutRule.has_value());
  assert(ConcreteType.has_value() == !ConcreteTypeRules.empty());

  assert((SuperclassDecl == nullptr) == Superclasses.empty());
  for (const auto &pair : Superclasses) {
    const auto &req = pair.second;
    assert(req.SuperclassType.has_value());
    assert(!req.SuperclassRules.empty());
  }

#endif
}

PropertyMap::~PropertyMap() {
  Trie.updateHistograms(Context.PropertyTrieHistogram,
                        Context.PropertyTrieRootHistogram);
  clear();
}

/// Look for a property bag corresponding to a suffix of the given range.
///
/// The symbol range must correspond to a term that has already been
/// simplified.
///
/// Returns nullptr if no information is known about this key.
PropertyBag *
PropertyMap::lookUpProperties(std::reverse_iterator<const Symbol *> begin,
                              std::reverse_iterator<const Symbol *> end) const {
  if (auto result = Trie.find(begin, end))
    return *result;

  return nullptr;
}

/// Look for a property bag corresponding to a suffix of the given key.
///
/// The term must have already been simplified.
///
/// Returns nullptr if no information is known about this key.
PropertyBag *
PropertyMap::lookUpProperties(const MutableTerm &key) const {
  return lookUpProperties(key.rbegin(), key.rend());
}

/// Look for a property bag corresponding to the given key, creating a new
/// property bag if necessary.
///
/// This must be called in monotonically non-decreasing key order.
PropertyBag *
PropertyMap::getOrCreateProperties(Term key) {
  auto next = Trie.find(key.rbegin(), key.rend());
  if (next && (*next)->getKey() == key)
    return *next;

  auto *props = new PropertyBag(key);

  // Look for the longest suffix of the key that has a property bag,
  // recording it as the next property bag if we find one.
  //
  // For example, if our rewrite system contains the following three rules:
  //
  //   A.[P] => A
  //   B.A.[Q] => B.A
  //   C.A.[R] => C.A
  //
  // Then we have three property bags:
  //
  //   A => { [P] }
  //   B.A => { [Q] }
  //   C.A => { [R] }
  //
  // The next property bag of both 'B.A' and 'C.A' is 'A'; conceptually,
  // the set of properties satisfied by 'B.A' is a superset of the properties
  // satisfied by 'A'; analogously for 'C.A'.
  //
  // Since 'A' has no proper suffix with additional properties, the next
  // property bag of 'A' is nullptr.
  if (next)
    props->copyPropertiesFrom(*next, Context);

  Entries.push_back(props);
  auto oldProps = Trie.insert(key.rbegin(), key.rend(), props);
  if (oldProps) {
    llvm::errs() << "Duplicate property map entry for " << key << "\n";
    llvm::errs() << "Old:\n";
    (*oldProps)->dump(llvm::errs());
    llvm::errs() << "\n";
    llvm::errs() << "New:\n";
    props->dump(llvm::errs());
    llvm::errs() << "\n";
    abort();
  }

  return props;
}

void PropertyMap::clear() {
  for (auto *props : Entries)
    delete props;

  Trie.clear();
  Entries.clear();
}

/// Build the property map from all rules of the form T.[p] => T, where
/// [p] is a property symbol.
///
/// Also performs property unification, nested type concretization and
/// concrete simplification. These phases can add new rules; if new rules
/// were added, the caller must run another round of Knuth-Bendix
/// completion, and rebuild the property map again.
void PropertyMap::buildPropertyMap() {
  if (System.getDebugOptions().contains(DebugFlags::PropertyMap)) {
    llvm::dbgs() << "-------------------------\n";
    llvm::dbgs() << "- Building property map -\n";
    llvm::dbgs() << "-------------------------\n";
  }

  clear();

  struct Property {
    Term key;
    Symbol symbol;
    unsigned ruleID;
  };

  // PropertyMap::addRule() requires that shorter rules are added
  // before longer rules, so that it can perform lookups on suffixes and call
  // PropertyBag::copyPropertiesFrom(). However, we don't have to perform a
  // full sort by term order here; a bucket sort by term length suffices.
  SmallVector<std::vector<Property>, 4> properties;

  for (const auto &rule : System.getRules()) {
    if (rule.isLHSSimplified() ||
        rule.isRHSSimplified())
      continue;

    // Identity conformances ([P].[P] => [P]) are permanent rules, but we
    // keep them around to ensure that concrete conformance introduction
    // works in the case where the protocol's Self type is itself subject
    // to a superclass or concrete type requirement.
    if (rule.isPermanent() && !rule.isIdentityConformanceRule())
      continue;

    // Collect all rules of the form T.[p] => T where T is canonical.
    auto property = rule.isPropertyRule();
    if (!property)
      continue;

    auto rhs = rule.getRHS();
    unsigned length = rhs.size();
    if (length >= properties.size())
      properties.resize(length + 1);

    unsigned ruleID = System.getRuleID(rule);
    properties[length].push_back({rhs, *property, ruleID});
  }

  for (const auto &bucket : properties) {
    for (auto property : bucket) {
      addProperty(property.key, property.symbol,
                  property.ruleID);
    }
  }

  // Now, check for conflicts between superclass and concrete type rules.
  checkConcreteTypeRequirements();

  // Now, we merge concrete type rules with conformance rules, by adding
  // relations between associated type members of type parameters with
  // the concrete type witnesses in the concrete type's conformance.
  concretizeNestedTypesFromConcreteParents();

  // Finally, a post-processing pass to reduce substitutions down to
  // concrete types.
  System.simplifyLeftHandSideSubstitutions(this);

  // Check invariants of the constructed property map.
  verify();
}

void PropertyMap::dump(llvm::raw_ostream &out) const {
  out << "Property map: {\n";
  for (const auto &props : Entries) {
    out << "  ";
    props->dump(out);
    out << "\n";
  }
  out << "}\n";
}

void PropertyMap::verify() const {
#ifndef NDEBUG
  for (const auto &props : Entries)
    props->verify(System);
#endif
}