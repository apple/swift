//===--- InterfaceType.cpp - Type to term conversion ----------------------===//
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
// This file implements routines for converting Swift AST interface types to
// rewrite system terms.
//
// A type parameter in Swift is a GenericTypeParamType wrapped in zero or
// more DependentMemberTypes. DependentMemberTypes come in two flavors,
// "unresolved" and "resolved". Unresolved DependentMemberTypes store an
// identifier. Resolved DependentMemberTypes store an associated type
// declaration.
//
// In the rewrite system, unresolved DependentMemberTypes map to name symbols;
// resolved DependentMemberTypes map to associated type symbols.
//
// The mapping of the root generic parameter depends on the specific usage:
//
// - If the type is understood to be the subject type of a requirement in a
//   protocol, the root generic parameter, which must equal τ_0_0, maps to a
//   protocol symbol for the protocol in question.
//
// - If the type is part of a top-level generic signature, the root generic
//   parameter maps to the corresponding generic parameter symbol.
//
// - If the type was derived from a superclass or concrete type symbol, the
//   root generic parameter, which must equal τ_0_N for some N, maps to the
//   Nth substitution stored in the superclass or concrete type symbol.
//
// The rewrite system's reduction order differs from the canonical type order
// used by Swift's ABI and name mangling. What this means in practice is that
// converting a canonical type to a term does not necessarily produce a
// canonical term; the term must further be simplified to produce a canonical
// term. Converting a canonical term back to a type, however, does produce a
// canonical type.
//
// Type to term conversion is implemented on the RewriteContext, and does not
// depend on the specific RewriteSystem used.
//
// Term to type conversion is implemented on the PropertyMap, and must only
// be performed after completion. This is because it relies on the property map
// to map associated type symbols back to Swift types.
//
// The specific difference between the canonical type order and the reduction
// order is as follows. In both orders, an associated type P1.T1 orders before
// an associated type P2.T2 if T1 < T2, or T1 == T2 and P1 < P2. The difference
// is in the protocol order relation P1 < P2.
//
// In the canonical type order, P1 < P2 based on the protocol names alone (or
// their module names, if the unqualified names are the same). In the reduction
// order, we want P1 < P2 to also hold if P1 inherits from P2.
//
// The following diagram shows the relationship between the two directions of
// the type to term mapping:
//
//      ---------------------
//     / Non-canonical Type /
//     ---------------------
//               |
//               v
//     +------------------+
//     | getTermForType() |
//     +------------------+
//               |
//               v
//     ---------------------
//    / Non-canonical Term /
//    ---------------------
//               |
//               v
//        +------------+
//        | simplify() |
//        +------------+
//               |
//               v
//       -----------------
//      / Canonical Term /
//      -----------------
//               |
//               v
//     +------------------+
//     | getTypeForTerm() |
//     +------------------+
//               |
//               v
//       -----------------
//      / Canonical Type /
//      -----------------
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "PropertyMap.h"
#include "RewriteSystem.h"
#include "RewriteContext.h"

using namespace swift;
using namespace rewriting;

Term RewriteContext::getTermForType(CanType paramType,
                                    const ProtocolDecl *proto) {
  return Term::get(getMutableTermForType(paramType, proto), *this);
}

/// Map an interface type to a term.
///
/// If \p proto is null, this is a term relative to a generic
/// parameter in a top-level signature. The term is rooted in a generic
/// parameter symbol.
///
/// If \p proto is non-null, this is a term relative to a protocol's
/// 'Self' type. The term is rooted in a protocol symbol for this protocol,
/// or an associated type symbol for some associated type in this protocol.
///
/// Resolved DependentMemberTypes map to associated type symbols.
/// Unresolved DependentMemberTypes map to name symbols.
///
/// Note the behavior of the root term is special if it is an associated
/// type symbol. The protocol of the associated type is always mapped to
/// \p proto if it was provided. This ensures we get the correct behavior
/// if a protocol places a constraint on an associated type inherited from
/// another protocol:
///
/// protocol P {
///   associatedtype Foo
/// }
///
/// protocol Q : P where Foo : R {}
///
/// protocol R {}
///
/// The DependentMemberType in the requirement signature of Q refers to
/// P::Foo.
///
/// However, we want Q's requirement signature to introduce the rewrite rule
///
///   [Q:Foo].[R] => [Q:Foo]
///
/// and not
///
///   [P:Foo].[R] => [P:Foo]
///
/// This is because the rule only applies to Q's logical override of Foo, and
/// not P's Foo.
///
/// To handle this, getMutableTermForType() behaves as follows:
///
/// Self.P::Foo with proto = P         => [P:Foo]
/// Self.P::Foo with proto = Q         => [Q:Foo]
/// τ_0_0.P::Foo with proto == nullptr => τ_0_0.[P:Foo]
///
MutableTerm RewriteContext::getMutableTermForType(CanType paramType,
                                                  const ProtocolDecl *proto) {
  assert(paramType->isTypeParameter());

  // Collect zero or more nested type names in reverse order.
  bool innermostAssocTypeWasResolved = false;

  SmallVector<Symbol, 3> symbols;
  while (auto memberType = dyn_cast<DependentMemberType>(paramType)) {
    paramType = memberType.getBase();

    if (auto *assocType = memberType->getAssocType()) {
      const auto *thisProto = assocType->getProtocol();
      if (proto && isa<GenericTypeParamType>(paramType)) {
        thisProto = proto;
        innermostAssocTypeWasResolved = true;
      }
      symbols.push_back(Symbol::forAssociatedType(thisProto,
                                                  assocType->getName(),
                                                  *this));
    } else {
      symbols.push_back(Symbol::forName(memberType->getName(), *this));
      innermostAssocTypeWasResolved = false;
    }
  }

  // Add the root symbol at the end.
  if (proto) {
    assert(proto->getSelfInterfaceType()->isEqual(paramType));

    // Self.Foo becomes [P].Foo
    // Self.Q::Foo becomes [P:Foo] (not [Q:Foo] or [P].[Q:Foo])
    if (!innermostAssocTypeWasResolved)
      symbols.push_back(Symbol::forProtocol(proto, *this));
  } else {
    symbols.push_back(Symbol::forGenericParam(
        cast<GenericTypeParamType>(paramType), *this));
  }

  std::reverse(symbols.begin(), symbols.end());

  return MutableTerm(symbols);
}

/// Find the most canonical associated type declaration with the given
/// name among a set of conforming protocols stored in this property map
/// entry.
AssociatedTypeDecl *PropertyBag::getAssociatedType(Identifier name) {
  auto found = AssocTypes.find(name);
  if (found != AssocTypes.end())
    return found->second;

  AssociatedTypeDecl *assocType = nullptr;

  for (auto *proto : ConformsTo) {
    auto checkOtherAssocType = [&](AssociatedTypeDecl *otherAssocType) {
      otherAssocType = otherAssocType->getAssociatedTypeAnchor();

      if (otherAssocType->getName() == name &&
          (assocType == nullptr ||
           TypeDecl::compare(otherAssocType->getProtocol(),
                             assocType->getProtocol()) < 0)) {
        assocType = otherAssocType;
      }
    };

    for (auto *otherAssocType : proto->getAssociatedTypeMembers()) {
      checkOtherAssocType(otherAssocType);
    }
  }

  assert(assocType != nullptr && "Need to look harder");

  auto inserted = AssocTypes.insert(std::make_pair(name, assocType)).second;
  assert(inserted);
  (void) inserted;

  return assocType;
}

/// Compute the interface type for a range of symbols.
static Type
getTypeForSymbolRange(const Symbol *begin, const Symbol *end,
                      ArrayRef<GenericTypeParamType *> genericParams,
                      const PropertyMap &map) {
  auto &ctx = map.getRewriteContext();
  Type result;

  auto handleRoot = [&](GenericTypeParamType *genericParam) {
    assert(genericParam->isCanonical());

    if (!genericParams.empty()) {
      // Return a sugared GenericTypeParamType if we're given an array of
      // sugared types to substitute.
      unsigned index = GenericParamKey(genericParam).findIndexIn(genericParams);

      if (index == genericParams.size()) {
        llvm::errs() << "Cannot build interface type for term "
                     << MutableTerm(begin, end) << "\n";
        llvm::errs() << "Invalid generic parameter: "
                     << Type(genericParam) << "\n";
        llvm::errs() << "Valid generic parameters: ";
        for (auto *otherParam : genericParams)
          llvm::errs() << " " << otherParam->getCanonicalType();
        llvm::errs() << "\n";
        abort();
      }

      result = genericParams[index];
      return;
    }

    // Otherwise, we're going to return a canonical type.
    result = genericParam;
  };

  for (auto *iter = begin; iter != end; ++iter) {
    auto symbol = *iter;

    if (!result) {
      // A valid term always begins with a generic parameter, protocol or
      // associated type symbol.
      switch (symbol.getKind()) {
      case Symbol::Kind::GenericParam:
        handleRoot(symbol.getGenericParam());
        continue;

      case Symbol::Kind::Protocol:
        handleRoot(GenericTypeParamType::get(/*isParameterPack*/ false, 0, 0,
                                             ctx.getASTContext()));
        continue;

      case Symbol::Kind::AssociatedType:
        handleRoot(GenericTypeParamType::get(/*isParameterPack*/ false, 0, 0,
                                             ctx.getASTContext()));

        // An associated type symbol at the root means we have a dependent
        // member type rooted at Self; handle the associated type below.
        break;

      case Symbol::Kind::Name:
      case Symbol::Kind::Layout:
      case Symbol::Kind::Superclass:
      case Symbol::Kind::ConcreteType:
      case Symbol::Kind::ConcreteConformance:
      case Symbol::Kind::Shape:
        llvm::errs() << "Invalid root symbol: " << MutableTerm(begin, end) << "\n";
        abort();
      }
    }

    // An unresolved type can appear if we have invalid requirements.
    if (symbol.getKind() == Symbol::Kind::Name) {
      result = DependentMemberType::get(result, symbol.getName());
      continue;
    }

    // We can end up with an unsimplified term like this:
    //
    // X.[P].[P:X]
    //
    // Simplification will rewrite X.[P] to X, so just ignore a protocol symbol
    // in the middle of a term.
    if (symbol.getKind() == Symbol::Kind::Protocol) {
#ifndef NDEBUG
      // Ensure that the domain of the suffix contains P.
      if (iter + 1 < end) {
        auto proto = (iter + 1)->getProtocol();
        assert(proto == symbol.getProtocol());
      }
#endif
      continue;
    }

    assert(symbol.getKind() == Symbol::Kind::AssociatedType);

    MutableTerm prefix;
    if (begin == iter) {
      // If the term begins with an associated type symbol, look for the
      // associated type in the protocol itself.
      prefix.add(Symbol::forProtocol(symbol.getProtocol(), ctx));
    } else {
      // The protocol stored in an associated type symbol appearing in a
      // canonical term is not necessarily the right protocol to look for
      // an associated type declaration to get a canonical _type_, because
      // the reduction order on terms is different than the canonical order
      // on types.
      //
      // Instead, find all protocols that the prefix conforms to, and look
      // for an associated type in those protocols.
      prefix.append(begin, iter);
    }

    auto *props = map.lookUpProperties(prefix.rbegin(), prefix.rend());
    if (props == nullptr) {
      llvm::errs() << "Cannot build interface type for term "
                   << MutableTerm(begin, end) << "\n";
      llvm::errs() << "Prefix does not conform to any protocols: "
                   << prefix << "\n\n";
      map.dump(llvm::errs());
      abort();
    }

    // Assert that the associated type's protocol appears among the set
    // of protocols that the prefix conforms to.
#ifndef NDEBUG
    auto conformsTo = props->getConformsTo();
    assert(std::find(conformsTo.begin(), conformsTo.end(),
                     symbol.getProtocol())
           != conformsTo.end());
#endif

    auto *assocType = props->getAssociatedType(symbol.getName());
    if (assocType == nullptr) {
      llvm::errs() << "Cannot build interface type for term "
                   << MutableTerm(begin, end) << "\n";
      llvm::errs() << "Prefix term does not have a nested type named "
                   << symbol.getName() << ": "
                   << prefix << "\n";
      llvm::errs() << "Property map entry: ";
      props->dump(llvm::errs());
      llvm::errs() << "\n\n";
      map.dump(llvm::errs());
      abort();
    }

    result = DependentMemberType::get(result, assocType);
  }

  return result;
}

Type PropertyMap::getTypeForTerm(Term term,
                      ArrayRef<GenericTypeParamType *> genericParams) const {
  return getTypeForSymbolRange(term.begin(), term.end(), genericParams, *this);
}

Type PropertyMap::getTypeForTerm(const MutableTerm &term,
                      ArrayRef<GenericTypeParamType *> genericParams) const {
  return getTypeForSymbolRange(term.begin(), term.end(), genericParams, *this);
}

/// Concrete type terms are written in terms of generic parameter types that
/// have a depth of 0, and an index into an array of substitution terms.
///
/// See RewriteSystemBuilder::getSubstitutionSchemaFromType().
unsigned RewriteContext::getGenericParamIndex(Type type) {
  auto *paramTy = type->castTo<GenericTypeParamType>();
  assert(paramTy->getDepth() == 0);
  return paramTy->getIndex();
}

/// Computes the term corresponding to a member type access on a substitution.
///
/// The type witness is a type parameter of the form τ_0_n.X.Y.Z,
/// where 'n' is an index into the substitution array.
///
/// If the nth entry in the array is S, this will produce S.X.Y.Z.
///
/// There is a special behavior if the substitution is a term consisting of a
/// single protocol symbol [P]. If the innermost associated type in
/// \p typeWitness is [Q:Foo], the result will be [P:Foo], not [P].[Q:Foo] or
/// [Q:Foo].
MutableTerm
RewriteContext::getRelativeTermForType(CanType typeWitness,
                                       ArrayRef<Term> substitutions) {
  MutableTerm result;

  // Get the substitution S corresponding to τ_0_n.
  unsigned index = getGenericParamIndex(typeWitness->getRootGenericParam());
  result = MutableTerm(substitutions[index]);
  assert(result.back().getKind() != Symbol::Kind::Shape);

  // If the substitution is a term consisting of a single protocol symbol
  // [P], save P for later.
  const ProtocolDecl *proto = nullptr;
  if (result.size() == 1 &&
      result[0].getKind() == Symbol::Kind::Protocol) {
    proto = result[0].getProtocol();
  }

  // Collect zero or more member type names in reverse order.
  SmallVector<Symbol, 3> symbols;
  while (auto memberType = dyn_cast<DependentMemberType>(typeWitness)) {
    typeWitness = memberType.getBase();

    auto *assocType = memberType->getAssocType();
    assert(assocType != nullptr &&
           "Conformance checking should not produce unresolved member types");

    // If the substitution is a term consisting of a single protocol symbol [P],
    // produce [P:Foo] instead of [P].[Q:Foo] or [Q:Foo].
    const auto *thisProto = assocType->getProtocol();
    if (proto && isa<GenericTypeParamType>(typeWitness)) {
      thisProto = proto;

      assert(result.size() == 1);
      assert(result[0].getKind() == Symbol::Kind::Protocol);
      assert(result[0].getProtocol() == proto);
      result = MutableTerm();
    }

    symbols.push_back(Symbol::forAssociatedType(thisProto,
                                                assocType->getName(), *this));
  }

  // Add the member type names.
  for (auto iter = symbols.rbegin(), end = symbols.rend(); iter != end; ++iter)
    result.add(*iter);

  return result;
}

/// Reverses the transformation performed by
/// RewriteSystemBuilder::getSubstitutionSchemaFromType().
Type PropertyMap::getTypeFromSubstitutionSchema(
    Type schema, ArrayRef<Term> substitutions,
    ArrayRef<GenericTypeParamType *> genericParams,
    const MutableTerm &prefix) const {
  assert(!schema->isTypeParameter() && "Must have a concrete type here");

  if (!schema->hasTypeParameter())
    return schema;

  return schema.transformWithPosition(
      TypePosition::Invariant,
      [&](Type t, TypePosition pos) -> llvm::Optional<Type> {
    if (t->is<GenericTypeParamType>()) {
      auto index = RewriteContext::getGenericParamIndex(t);
      auto substitution = substitutions[index];

      bool isShapePosition = (pos == TypePosition::Shape);
      bool isShapeTerm = (substitution.back() == Symbol::forShape(Context));
      if (isShapePosition != isShapeTerm) {
        llvm::errs() << "Shape vs. type mixup\n\n";
        schema.dump(llvm::errs());
        llvm::errs() << "Substitutions:\n";
        for (auto otherSubst : substitutions) {
          llvm::errs() << "- ";
          otherSubst.dump(llvm::errs());
          llvm::errs() << "\n";
        }
        llvm::errs() << "\n";
        dump(llvm::errs());

        abort();
      }

      // Undo the thing where the count type of a PackExpansionType
      // becomes a shape term.
      if (isShapeTerm) {
        MutableTerm mutTerm(substitution.begin(),
                            substitution.end() - 1);
        substitution = Term::get(mutTerm, Context);
      }

      // Prepend the prefix of the lookup key to the substitution.
      if (prefix.empty()) {
        // Skip creation of a new MutableTerm in the case where the
        // prefix is empty.
        return getTypeForTerm(substitution, genericParams);
      } else {
        // Otherwise build a new term by appending the substitution
        // to the prefix.
        MutableTerm result(prefix);
        result.append(substitution);
        return getTypeForTerm(result, genericParams);
      }
    }

    assert(!t->isTypeParameter());
    return llvm::None;
  });
}

/// This method takes a concrete type that was derived from a concrete type
/// produced by RewriteContext::getSubstitutionSchemaFromType() either by
/// extracting a structural sub-component or performing a (Swift AST)
/// substitution using subst(). It returns a new concrete substitution schema
/// and a new list of substitution terms.
///
/// For example, suppose we start with the concrete type
///
///   Dictionary<τ_0_0, Array<τ_0_1>> with substitutions {X.Y, Z}
///
/// We can extract out the structural sub-component Array<τ_0_1>. If we wish
/// to build a new concrete substitution schema, we call this method with
/// Array<τ_0_1> and the original substitutions {X.Y, Z}. This will produce
/// the new schema Array<τ_0_0> with substitutions {Z}.
///
/// As another example, consider we start with the schema Bar<τ_0_0> with
/// original substitutions {X.Y}, and perform a Swift AST subst() to get
/// Foo<τ_0_0.A.B>. We can then call this method with Foo<τ_0_0.A.B> and
/// the original substitutions {X.Y} to produce the new schema Foo<τ_0_0>
/// with substitutions {X.Y.A.B}.
CanType
RewriteContext::getRelativeSubstitutionSchemaFromType(
    CanType concreteType,
    ArrayRef<Term> substitutions,
    SmallVectorImpl<Term> &result) {
  assert(!concreteType->isTypeParameter() && "Must have a concrete type here");

  if (!concreteType->hasTypeParameter())
    return concreteType;

  return CanType(concreteType.transformWithPosition(
      TypePosition::Invariant,
      [&](Type t, TypePosition pos) -> llvm::Optional<Type> {

    if (!t->isTypeParameter())
      return llvm::None;

    auto term = getRelativeTermForType(CanType(t), substitutions);

    // PackExpansionType(pattern=T, count=U) becomes
    // PackExpansionType(pattern=τ_0_0, count=τ_0_1) with
    //
    // τ_0_0 := T
    // τ_0_1 := U.[shape]
    if (pos == TypePosition::Shape) {
      assert(false);
      term.add(Symbol::forShape(*this));
    }

    unsigned index = result.size();

    result.push_back(Term::get(term, *this));

    return CanGenericTypeParamType::get(/*isParameterPack=*/ false,
                                        /*depth=*/ 0, index,
                                        Context);
  }));
}

/// Given a concrete type that may contain type parameters in structural positions,
/// collect all the structural type parameter components, and replace them all with
/// fresh generic parameters. The fresh generic parameters all have a depth of 0,
/// and the index is an index into the 'result' array.
///
/// For example, given the concrete type Foo<X.Y, Array<Z>>, this produces the
/// result type Foo<τ_0_0, Array<τ_0_1>>, with result array {X.Y, Z}.
CanType
RewriteContext::getSubstitutionSchemaFromType(CanType concreteType,
                                              const ProtocolDecl *proto,
                                              SmallVectorImpl<Term> &result) {
  assert(!concreteType->isTypeParameter() && "Must have a concrete type here");

  if (!concreteType->hasTypeParameter())
    return concreteType;

  return CanType(concreteType.transformWithPosition(
      TypePosition::Invariant,
      [&](Type t, TypePosition pos)
          -> llvm::Optional<Type> {

    if (!t->isTypeParameter())
      return llvm::None;

    // PackExpansionType(pattern=T, count=U) becomes
    // PackExpansionType(pattern=τ_0_0, count=τ_0_1) with
    //
    // τ_0_0 := T
    // τ_0_1 := U.[shape]
    MutableTerm term = getMutableTermForType(CanType(t), proto);
    if (pos == TypePosition::Shape)
      term.add(Symbol::forShape(*this));

    unsigned index = result.size();

    result.push_back(Term::get(term, *this));

    return CanGenericTypeParamType::get(/*isParameterPack=*/ false,
                                        /*depth=*/0, index,
                                        Context);
  }));
}
