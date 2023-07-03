//===--- Rule.cpp - An oriented rewrite rule in a rewrite system ----------===//
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

#include "Rule.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "swift/AST/TypeWalker.h"
#include "llvm/Support/raw_ostream.h"
#include "RewriteContext.h"
#include "Term.h"
#include "Symbol.h"

using namespace swift;
using namespace rewriting;

/// If this is a rule of the form T.[p] => T where [p] is a property symbol,
/// returns the symbol. Otherwise, returns None.
///
/// Note that this is meant to be used with a simplified rewrite system,
/// where the right hand sides of rules are canonical, since this also means
/// that T is canonical.
llvm::Optional<Symbol> Rule::isPropertyRule() const {
  auto property = LHS.back();

  if (!property.isProperty())
    return llvm::None;

  if (LHS.size() - 1 != RHS.size())
    return llvm::None;

  if (!std::equal(RHS.begin(), RHS.end(), LHS.begin()))
    return llvm::None;

  return property;
}

/// If this is a rule of the form T.[P] => T where [P] is a protocol symbol,
/// return the protocol P, otherwise return nullptr.
const ProtocolDecl *Rule::isProtocolConformanceRule() const {
  if (auto property = isPropertyRule()) {
    if (property->getKind() == Symbol::Kind::Protocol)
      return property->getProtocol();
  }

  return nullptr;
}

/// If this is a rule of the form T.[concrete: C : P] => T where
/// [concrete: C : P] is a concrete conformance symbol, return the protocol P,
/// otherwise return nullptr.
const ProtocolDecl *Rule::isAnyConformanceRule() const {
  if (auto property = isPropertyRule()) {
    switch (property->getKind()) {
    case Symbol::Kind::ConcreteConformance:
    case Symbol::Kind::Protocol:
      return property->getProtocol();

    case Symbol::Kind::Layout:
    case Symbol::Kind::Superclass:
    case Symbol::Kind::ConcreteType:
      return nullptr;

    case Symbol::Kind::Name:
    case Symbol::Kind::AssociatedType:
    case Symbol::Kind::GenericParam:
    case Symbol::Kind::Shape:
      break;
    }

    llvm_unreachable("Bad symbol kind");
  }

  return nullptr;
}

/// If this is a rule of the form [P].[P] => [P] where [P] is a protocol
/// symbol, return true, otherwise return false.
bool Rule::isIdentityConformanceRule() const {
  return (LHS.size() == 2 &&
          RHS.size() == 1 &&
          LHS[0] == RHS[0] &&
          LHS[0] == LHS[1] &&
          LHS[0].getKind() == Symbol::Kind::Protocol);
}

/// If this is a rule of the form [P].[Q] => [P] where [P] and [Q] are
/// protocol symbols, return true, otherwise return false.
bool Rule::isProtocolRefinementRule(RewriteContext &ctx) const {
  if (LHS.size() == 2 &&
      RHS.size() == 1 &&
      LHS[0] == RHS[0] &&
      LHS[0].getKind() == Symbol::Kind::Protocol &&
      (LHS[1].getKind() == Symbol::Kind::Protocol ||
       LHS[1].getKind() == Symbol::Kind::ConcreteConformance) &&
      LHS[0] != LHS[1]) {

    // A protocol refinement rule must be from a directly-stated
    // inheritance clause entry. It can only become redundant if it is
    // written in terms of other protocol refinement rules; otherwise, it
    // must appear in the protocol's requirement signature.
    //
    // See RewriteSystem::isValidRefinementPath() for an explanation.
    auto *proto = LHS[0].getProtocol();
    auto *otherProto = LHS[1].getProtocol();

    auto inherited = ctx.getInheritedProtocols(proto);
    return (std::find(inherited.begin(), inherited.end(), otherProto)
            != inherited.end());
  }

  return false;
}

/// If this is a rule of the form [P].[concrete: C : Q] => [P] where
/// [P] is a protocol symbol, return true.
///
/// This means that P constrains 'Self' to a concrete type that conforms
/// to some Q with P : Q. We don't consider this to be a valid conformance
/// path element, to ensure compatibility with the GSB in an odd edge
/// case:
///
///    protocol P : C {}
///    class C : P {}
///
/// The GSB minimizes the signature <T where T : P> to <T where T : P>,
/// whereas the minimal conformances algorithm would otherwise minimize
/// it down to <T where T : C> on account of the (T.[P] => T) conformance
/// rule being redundantly expressed via [P].[concrete: C : P].
bool Rule::isCircularConformanceRule() const {
  if (LHS.size() != 2 || RHS.size() != 1 || LHS[0] != RHS[0])
    return false;

  if (LHS[0].getKind() != Symbol::Kind::Protocol ||
      LHS[1].getKind() != Symbol::Kind::ConcreteConformance)
    return false;

  return true;
}

/// A protocol typealias rule takes one of the following two forms,
/// where T is a name symbol:
///
/// 1) [P].T => X
/// 2) [P].T.[concrete: C] => [P].T
///
/// The first case is where the protocol's underlying type is another
/// type parameter. The second case is where the protocol's underlying
/// type is a concrete type.
///
/// In the first case, X must be fully resolved, that is, it must not
/// contain any name symbols.
///
/// If this rule is a protocol typealias rule, returns its name. Otherwise
/// returns None.
llvm::Optional<Identifier> Rule::isProtocolTypeAliasRule() const {
  if (LHS.size() != 2 && LHS.size() != 3)
    return llvm::None;

  if (LHS[0].getKind() != Symbol::Kind::Protocol ||
      LHS[1].getKind() != Symbol::Kind::Name)
    return llvm::None;

  if (LHS.size() == 2) {
    // This is the case where the underlying type is a type parameter.
    //
    // We shouldn't have unresolved symbols on the right hand side;
    // they should have been simplified away.
    if (RHS.containsUnresolvedSymbols()) {
      if (RHS.size() != 2 ||
          RHS[0] != LHS[0] ||
          RHS[1].getKind() != Symbol::Kind::Name) {
        return llvm::None;
      }
    }
  } else {
    // This is the case where the underlying type is concrete.
    assert(LHS.size() == 3);

    auto prop = isPropertyRule();
    if (!prop || prop->getKind() != Symbol::Kind::ConcreteType)
      return llvm::None;
  }

  return LHS[1].getName();
}

/// A rule was derived from a concrete protocol typealias if it
/// takes the following form:
///
/// T.A.[concrete: C] => T.A
///
/// Where the prefix term T does not contain any name symbols, and
/// A is a name symbol.
bool Rule::isDerivedFromConcreteProtocolTypeAliasRule() const {
  auto optSymbol = isPropertyRule();
  if (!optSymbol || optSymbol->getKind() != Symbol::Kind::ConcreteType)
    return false;

  for (unsigned i = 0, e = RHS.size() - 1; i < e; ++i) {
    if (RHS[i].getKind() == Symbol::Kind::Name)
      return false;
  }

  if (RHS.back().getKind() != Symbol::Kind::Name)
    return false;

  return true;
}

/// Returns the length of the left hand side.
unsigned Rule::getDepth() const {
  auto result = LHS.size();

  if (LHS.back().hasSubstitutions()) {
    for (auto substitution : LHS.back().getSubstitutions()) {
      result = std::max(result, substitution.size());
    }
  }

  return result;
}

/// Returns the nesting depth of the concrete symbol at the end of the
/// left hand side, or 0 if there isn't one.
unsigned Rule::getNesting() const {
  if (LHS.back().hasSubstitutions()) {
    auto type = LHS.back().getConcreteType();

    struct Walker : TypeWalker {
      unsigned Nesting = 0;
      unsigned MaxNesting = 0;

      Action walkToTypePre(Type ty) override {
        ++Nesting;
        MaxNesting = std::max(Nesting, MaxNesting);

        return Action::Continue;
      }

      Action walkToTypePost(Type ty) override {
        --Nesting;

        return Action::Continue;
      }
    };

    Walker walker;
    type.walk(walker);

    return walker.MaxNesting;
  }

  return 0;
}

/// Linear order on rules; compares LHS followed by RHS.
llvm::Optional<int> Rule::compare(const Rule &other,
                                  RewriteContext &ctx) const {
  llvm::Optional<int> compare = LHS.compare(other.LHS, ctx);
  if (!compare.has_value() || *compare != 0)
    return compare;

  return RHS.compare(other.RHS, ctx);
}

void Rule::dump(llvm::raw_ostream &out) const {
  out << LHS << " => " << RHS;
  if (Permanent)
    out << " [permanent]";
  if (Explicit)
    out << " [explicit]";
  if (LHSSimplified)
    out << " [lhs↓]";
  if (RHSSimplified)
    out << " [rhs↓]";
  if (SubstitutionSimplified)
    out << " [subst↓]";
  if (Redundant)
    out << " [redundant]";
  if (Conflicting)
    out << " [conflicting]";
  if (Recursive)
    out << " [recursive]";
}
