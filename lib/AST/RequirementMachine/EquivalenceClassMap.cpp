//===--- EquivalenceClassMap.cpp - Facts about generic parameters ---------===//
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
// Where [p] is a "property atom": [layout: L], [superclass: Foo],
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
// This is the idea behind the equivalence class map. We collect all rules of the
// form V.[p] => V' into a multi-map keyed by V. Then given an arbitrary type T,
// we can reduce it and look up successive suffixes to find all properties [p]
// satisfied by T.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Decl.h"
#include "swift/AST/LayoutConstraint.h"
#include "swift/AST/Module.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeMatcher.h"
#include "swift/AST/Types.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <vector>

#include "EquivalenceClassMap.h"

using namespace swift;
using namespace rewriting;

void EquivalenceClass::dump(llvm::raw_ostream &out) const {
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

  if (Superclass) {
    out << " superclass: " << *Superclass;
  }

  if (ConcreteType) {
    out << " concrete_type: " << *ConcreteType;
  }

  out << " }";
}

/// Concrete type terms are written in terms of generic parameter types that
/// have a depth of 0, and an index into an array of substitution terms.
///
/// See RewriteSystemBuilder::getConcreteSubstitutionSchema().
static unsigned getGenericParamIndex(Type type) {
  auto *paramTy = type->castTo<GenericTypeParamType>();
  assert(paramTy->getDepth() == 0);
  return paramTy->getIndex();
}

/// Reverses the transformation performed by
/// RewriteSystemBuilder::getConcreteSubstitutionSchema().
static Type getTypeFromSubstitutionSchema(Type schema,
                                          ArrayRef<Term> substitutions,
                              TypeArrayView<GenericTypeParamType> genericParams,
                                          const ProtocolGraph &protos,
                                          RewriteContext &ctx) {
  assert(!schema->isTypeParameter() && "Must have a concrete type here");

  if (!schema->hasTypeParameter())
    return schema;

  return schema.transformRec([&](Type t) -> Optional<Type> {
    if (t->is<GenericTypeParamType>()) {
      auto index = getGenericParamIndex(t);

      return ctx.getTypeForTerm(substitutions[index],
                                genericParams, protos);
    }

    assert(!t->isTypeParameter());
    return None;
  });
}

/// Get the concrete type of this equivalence class.
///
/// Asserts if this equivalence class is not concrete.
Type EquivalenceClass::getConcreteType(
    TypeArrayView<GenericTypeParamType> genericParams,
    const ProtocolGraph &protos,
    RewriteContext &ctx) const {
  return getTypeFromSubstitutionSchema(ConcreteType->getConcreteType(),
                                       ConcreteType->getSubstitutions(),
                                       genericParams,
                                       protos,
                                       ctx);
}

/// Computes the term corresponding to a member type access on a substitution.
///
/// The type witness is a type parameter of the form τ_0_n.X.Y.Z,
/// where 'n' is an index into the substitution array.
///
/// If the nth entry in the array is S, this will produce S.X.Y.Z.
///
/// There is a special behavior if the substitution is a term consisting of a
/// single protocol atom [P]. If the innermost associated type in
/// \p typeWitness is [Q:Foo], the result will be [P:Foo], not [P].[Q:Foo] or
/// [Q:Foo].
static MutableTerm getRelativeTermForType(CanType typeWitness,
                                          ArrayRef<Term> substitutions,
                                          RewriteContext &ctx) {
  MutableTerm result;

  // Get the substitution S corresponding to τ_0_n.
  unsigned index = getGenericParamIndex(typeWitness->getRootGenericParam());
  result = MutableTerm(substitutions[index]);

  // If the substitution is a term consisting of a single protocol atom
  // [P], save P for later.
  const ProtocolDecl *proto = nullptr;
  if (result.size() == 1 &&
      result[0].getKind() == Atom::Kind::Protocol) {
    proto = result[0].getProtocol();
  }

  // Collect zero or more member type names in reverse order.
  SmallVector<Atom, 3> atoms;
  while (auto memberType = dyn_cast<DependentMemberType>(typeWitness)) {
    typeWitness = memberType.getBase();

    auto *assocType = memberType->getAssocType();
    assert(assocType != nullptr &&
           "Conformance checking should not produce unresolved member types");

    // If the substitution is a term consisting of a single protocol atom [P],
    // produce [P:Foo] instead of [P].[Q:Foo] or [Q:Foo].
    const auto *thisProto = assocType->getProtocol();
    if (proto && isa<GenericTypeParamType>(typeWitness)) {
      thisProto = proto;

      assert(result.size() == 1);
      assert(result[0].getKind() == Atom::Kind::Protocol);
      assert(result[0].getProtocol() == proto);
      result = MutableTerm();
    }

    atoms.push_back(Atom::forAssociatedType(thisProto,
                                            assocType->getName(), ctx));
  }

  // Add the member type names.
  std::reverse(atoms.begin(), atoms.end());
  for (auto atom : atoms)
    result.add(atom);

  return result;
}

/// This method takes a concrete type that was derived from a concrete type
/// produced by RewriteSystemBuilder::getConcreteSubstitutionSchema(),
/// either by extracting a structural sub-component or performing a (Swift AST)
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
static CanType
remapConcreteSubstitutionSchema(CanType concreteType,
                                ArrayRef<Term> substitutions,
                                RewriteContext &ctx,
                                SmallVectorImpl<Term> &result) {
  assert(!concreteType->isTypeParameter() && "Must have a concrete type here");

  if (!concreteType->hasTypeParameter())
    return concreteType;

  return CanType(concreteType.transformRec(
    [&](Type t) -> Optional<Type> {
      if (!t->isTypeParameter())
        return None;

      auto term = getRelativeTermForType(CanType(t), substitutions, ctx);

      unsigned newIndex = result.size();
      result.push_back(Term::get(term, ctx));

      return CanGenericTypeParamType::get(/*depth=*/0, newIndex,
                                          ctx.getASTContext());
    }));
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
///   T.[concrete: Foo<τ_0_0, τ_0_1, String> with {X.Y, Z}]
///   T.[concrete: Foo<Int, τ_0_0, τ_0_1> with {A.B, W}]
///
/// The two concrete type atoms will be added to the equivalence class of 'T',
/// and we will eventually end up in this method, where we will generate three
/// induced rules:
///
///   X.Y.[concrete: Int] => X.Y
///   A.B => Z
///   W.[concrete: String] => W
///
/// Returns the left hand side on success (it could also return the right hand
/// side; since we unified the type constructor arguments, it doesn't matter).
///
/// Returns true if a conflict was detected.
static bool unifyConcreteTypes(
    Atom lhs, Atom rhs, RewriteContext &ctx,
    SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules,
    bool debug) {
  auto lhsType = lhs.getConcreteType();
  auto rhsType = rhs.getConcreteType();

  if (debug) {
    llvm::dbgs() << "% Unifying " << lhs << " with " << rhs << "\n";
  }

  class Matcher : public TypeMatcher<Matcher> {
    ArrayRef<Term> lhsSubstitutions;
    ArrayRef<Term> rhsSubstitutions;
    RewriteContext &ctx;
    SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules;
    bool debug;

  public:
    Matcher(ArrayRef<Term> lhsSubstitutions,
            ArrayRef<Term> rhsSubstitutions,
            RewriteContext &ctx,
            SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules,
            bool debug)
        : lhsSubstitutions(lhsSubstitutions),
          rhsSubstitutions(rhsSubstitutions),
          ctx(ctx), inducedRules(inducedRules), debug(debug) {}

    bool alwaysMismatchGenericParams() const { return true; }

    bool mismatch(TypeBase *firstType, TypeBase *secondType,
                  Type sugaredFirstType) {
      if (isa<GenericTypeParamType>(firstType) &&
          isa<GenericTypeParamType>(secondType)) {
        // Both sides are type parameters; add a same-type requirement.
        unsigned lhsIndex = getGenericParamIndex(firstType);
        unsigned rhsIndex = getGenericParamIndex(secondType);
        if (lhsSubstitutions[lhsIndex] != rhsSubstitutions[rhsIndex]) {
          MutableTerm lhsTerm(lhsSubstitutions[lhsIndex]);
          MutableTerm rhsTerm(rhsSubstitutions[rhsIndex]);
          if (debug) {
            llvm::dbgs() << "%% Induced rule " << lhsTerm
                         << " == " << rhsTerm << "\n";
          }
          inducedRules.emplace_back(lhsTerm, rhsTerm);
        }
        return true;
      }

      if (isa<GenericTypeParamType>(firstType) &&
          !isa<GenericTypeParamType>(secondType)) {
        // A type parameter is equated with a concrete type; add a concrete
        // type requirement.
        unsigned lhsIndex = getGenericParamIndex(firstType);
        MutableTerm subjectTerm(lhsSubstitutions[lhsIndex]);

        SmallVector<Term, 3> result;
        auto concreteType = remapConcreteSubstitutionSchema(CanType(secondType),
                                                            rhsSubstitutions,
                                                            ctx, result);

        MutableTerm constraintTerm(subjectTerm);
        constraintTerm.add(Atom::forConcreteType(concreteType, result, ctx));

        if (debug) {
          llvm::dbgs() << "%% Induced rule " << subjectTerm
                       << " == " << constraintTerm << "\n";
        }
        inducedRules.emplace_back(subjectTerm, constraintTerm);
        return true;
      }

      if (!isa<GenericTypeParamType>(firstType) &&
          isa<GenericTypeParamType>(secondType)) {
        // A concrete type is equated with a type parameter; add a concrete
        // type requirement.
        unsigned rhsIndex = getGenericParamIndex(secondType);
        MutableTerm subjectTerm(rhsSubstitutions[rhsIndex]);

        SmallVector<Term, 3> result;
        auto concreteType = remapConcreteSubstitutionSchema(CanType(firstType),
                                                            lhsSubstitutions,
                                                            ctx, result);

        MutableTerm constraintTerm(subjectTerm);
        constraintTerm.add(Atom::forConcreteType(concreteType, result, ctx));

        if (debug) {
          llvm::dbgs() << "%% Induced rule " << subjectTerm
                       << " == " << constraintTerm << "\n";
        }
        inducedRules.emplace_back(subjectTerm, constraintTerm);
        return true;
      }

      // Any other kind of type mismatch involves different concrete types on
      // both sides, which can only happen on invalid input.
      return false;
    }
  };

  Matcher matcher(lhs.getSubstitutions(),
                  rhs.getSubstitutions(),
                  ctx, inducedRules, debug);
  if (!matcher.match(lhsType, rhsType)) {
    // FIXME: Diagnose the conflict
    if (debug) {
      llvm::dbgs() << "%% Concrete type conflict\n";
    }
    return true;
  }

  return false;
}

void EquivalenceClass::addProperty(
    Atom property, RewriteContext &ctx,
    SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules,
    bool debug) {

  switch (property.getKind()) {
  case Atom::Kind::Protocol:
    ConformsTo.push_back(property.getProtocol());
    return;

  case Atom::Kind::Layout:
    if (!Layout)
      Layout = property.getLayoutConstraint();
    else
      Layout = Layout.merge(property.getLayoutConstraint());

    return;

  case Atom::Kind::Superclass: {
    auto superclass = property.getSuperclass();

    // A superclass requirement implies a layout requirement.
    auto layout =
      LayoutConstraint::getLayoutConstraint(
        superclass->getClassOrBoundGenericClass()->isObjC()
          ? LayoutConstraintKind::Class
          : LayoutConstraintKind::NativeClass,
        ctx.getASTContext());
    addProperty(Atom::forLayout(layout, ctx), ctx, inducedRules, debug);

    // FIXME: This needs to find the most derived subclass and also call
    // unifyConcreteTypes()
    Superclass = property;
    return;
  }

  case Atom::Kind::ConcreteType: {
    if (ConcreteType) {
      (void) unifyConcreteTypes(*ConcreteType, property,
                                ctx, inducedRules, debug);
    } else {
      ConcreteType = property;
    }

    return;
  }

  case Atom::Kind::Name:
  case Atom::Kind::GenericParam:
  case Atom::Kind::AssociatedType:
    break;
  }

  llvm_unreachable("Bad atom kind");
}

void EquivalenceClass::copyPropertiesFrom(const EquivalenceClass *next,
                                          RewriteContext &ctx) {
  // If this is the equivalence class of T and 'next' is the
  // equivalence class of V, then T := UV for some non-empty U.
  int prefixLength = Key.size() - next->Key.size();
  assert(prefixLength > 0);
  assert(std::equal(Key.begin() + prefixLength, Key.end(),
                    next->Key.begin()));

  // Conformances and the layout constraint, if any, can be copied over
  // unmodified.
  ConformsTo = next->ConformsTo;
  Layout = next->Layout;

  // If the equivalence class of V has superclass or concrete type
  // substitutions {X1, ..., Xn}, then the equivalence class of
  // T := UV should have substitutions {UX1, ..., UXn}.
  MutableTerm prefix(Key.begin(), Key.begin() + prefixLength);

  if (next->Superclass) {
    Superclass = next->Superclass->prependPrefixToConcreteSubstitutions(
        prefix, ctx);
  }

  if (next->ConcreteType) {
    ConcreteType = next->ConcreteType->prependPrefixToConcreteSubstitutions(
        prefix, ctx);
  }
}

/// Look for an equivalence class corresponding to the given key, returning nullptr
/// if one has not been recorded.
EquivalenceClass *
EquivalenceClassMap::getEquivalenceClassIfPresent(const MutableTerm &key) const {
  assert(!key.empty());
 
  for (const auto &equivClass : Map) {
    int compare = equivClass->getKey().compare(key, Protos);
    if (compare == 0)
      return equivClass.get();
    if (compare > 0)
      return nullptr;
  }

  return nullptr;
}

/// Look for an equivalence class corresponding to a suffix of the given key.
///
/// Returns nullptr if no information is known about this key.
EquivalenceClass *
EquivalenceClassMap::lookUpEquivalenceClass(const MutableTerm &key) const {
  if (auto *equivClass = getEquivalenceClassIfPresent(key))
    return equivClass;

  auto begin = key.begin() + 1;
  auto end = key.end();

  while (begin != end) {
    MutableTerm suffix(begin, end);

    if (auto *suffixClass = getEquivalenceClassIfPresent(suffix))
      return suffixClass;

    ++begin;
  }

  return nullptr;
}

/// Look for an equivalence class corresponding to the given key, creating a new
/// equivalence class if necessary.
///
/// This must be called in monotonically non-decreasing key order.
EquivalenceClass *
EquivalenceClassMap::getOrCreateEquivalenceClass(const MutableTerm &key) {
  assert(!key.empty());

  if (!Map.empty()) {
    const auto &lastEquivClass = Map.back();
    int compare = lastEquivClass->getKey().compare(key, Protos);
    if (compare == 0)
      return lastEquivClass.get();

    assert(compare < 0 && "Must record equivalence classes in sorted order");
  }

  auto *equivClass = new EquivalenceClass(key);

  // Look for the longest suffix of the key that has an equivalence class,
  // recording it as the next equivalence class if we find one.
  //
  // For example, if our rewrite system contains the following three rules:
  //
  //   A.[P] => A
  //   B.A.[Q] => B.A
  //   C.A.[R] => C.A
  //
  // Then we have three equivalence classes:
  //
  //   A => { [P] }
  //   B.A => { [Q] }
  //   C.A => { [R] }
  //
  // The next equivalence class of both 'B.A' and 'C.A' is 'A'; conceptually,
  // the set of properties satisfied by 'B.A' is a superset of the properties
  // satisfied by 'A'; analogously for 'C.A'.
  //
  // Since 'A' has no proper suffix with additional properties, the next
  // equivalence class of 'A' is nullptr.
  if (auto *next = lookUpEquivalenceClass(key))
    equivClass->copyPropertiesFrom(next, Context);

  Map.emplace_back(equivClass);

  return equivClass;
}

void EquivalenceClassMap::clear() {
  Map.clear();
}

/// Record a protocol conformance, layout or superclass constraint on the given
/// key. Must be called in monotonically non-decreasing key order.
void EquivalenceClassMap::addProperty(
    const MutableTerm &key, Atom property,
    SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules) {
  assert(property.isProperty());
  auto *equivClass = getOrCreateEquivalenceClass(key);
  equivClass->addProperty(property, Context,
                          inducedRules, DebugConcreteUnification);
}

void EquivalenceClassMap::concretizeNestedTypesFromConcreteParents(
    SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules) const {
  for (const auto &equivClass : Map) {
    if (equivClass->getConformsTo().empty())
      continue;

    if (DebugConcretizeNestedTypes) {
      if (equivClass->isConcreteType() ||
          equivClass->hasSuperclassBound()) {
        llvm::dbgs() << "^ Concretizing nested types of ";
        equivClass->dump(llvm::dbgs());
        llvm::dbgs() << "\n";
      }
    }

    if (equivClass->isConcreteType()) {
      if (DebugConcretizeNestedTypes) {
        llvm::dbgs() << "- via concrete type requirement\n";
      }

      concretizeNestedTypesFromConcreteParent(
          equivClass->getKey(),
          equivClass->ConcreteType->getConcreteType(),
          equivClass->ConcreteType->getSubstitutions(),
          equivClass->getConformsTo(),
          inducedRules);
    }

    if (equivClass->hasSuperclassBound()) {
      if (DebugConcretizeNestedTypes) {
        llvm::dbgs() << "- via superclass requirement\n";
      }

      concretizeNestedTypesFromConcreteParent(
          equivClass->getKey(),
          equivClass->Superclass->getSuperclass(),
          equivClass->Superclass->getSubstitutions(),
          equivClass->getConformsTo(),
          inducedRules);
    }
  }
}

/// Suppose a same-type requirement merges two equivalence classes,
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
/// together with the following equivalence class:
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
void EquivalenceClassMap::concretizeNestedTypesFromConcreteParent(
    const MutableTerm &key,
    CanType concreteType, ArrayRef<Term> substitutions,
    ArrayRef<const ProtocolDecl *> conformsTo,
    SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules) const {
  for (auto *proto : conformsTo) {
    // FIXME: Either remove the ModuleDecl entirely from conformance lookup,
    // or pass the correct one down in here.
    auto *module = proto->getParentModule();

    auto conformance = module->lookupConformance(concreteType,
                                                 const_cast<ProtocolDecl *>(proto));
    if (conformance.isInvalid()) {
      // FIXME: Diagnose conflict
      if (DebugConcretizeNestedTypes) {
        llvm::dbgs() << "^^ " << concreteType << " does not conform to "
                     << proto->getName() << "\n";
      }

      continue;
    }

    // FIXME: Maybe this can happen if the concrete type is an
    // opaque result type?
    assert(!conformance.isAbstract());

    auto assocTypes = Protos.getProtocolInfo(proto).AssociatedTypes;
    if (assocTypes.empty())
      continue;

    auto *concrete = conformance.getConcrete();

    for (auto *assocType : assocTypes) {
      if (DebugConcretizeNestedTypes) {
        llvm::dbgs() << "^^ " << "Looking up type witness for "
                     << proto->getName() << ":" << assocType->getName()
                     << " on " << concreteType << "\n";
      }

      auto t = concrete->getTypeWitness(assocType);
      if (!t) {
        if (DebugConcretizeNestedTypes) {
          llvm::dbgs() << "^^ " << "Type witness for " << assocType->getName()
                       << " of " << concreteType << " could not be inferred\n";
        }

        t = ErrorType::get(concreteType);
      }

      auto typeWitness = t->getCanonicalType();

      if (DebugConcretizeNestedTypes) {
        llvm::dbgs() << "^^ " << "Type witness for " << assocType->getName()
                     << " of " << concreteType << " is " << typeWitness << "\n";
      }

      auto nestedType = Atom::forAssociatedType(proto, assocType->getName(),
                                                Context);

      MutableTerm subjectType = key;
      subjectType.add(nestedType);

      MutableTerm constraintType;

      if (concreteType == typeWitness) {
        if (DebugConcretizeNestedTypes) {
          llvm::dbgs() << "^^ Type witness is the same as the concrete type\n";
        }

        // Add a rule T.[P:A] => T.
        constraintType = key;

      } else if (typeWitness->isTypeParameter()) {
        // The type witness is a type parameter of the form τ_0_n.X.Y...Z,
        // where 'n' is an index into the substitution array.
        //
        // Add a rule T => S.X.Y...Z, where S is the nth substitution term.
        constraintType = getRelativeTermForType(typeWitness, substitutions,
                                                Context);

      } else {
        // The type witness is a concrete type.
        constraintType = subjectType;

        SmallVector<Term, 3> result;
        auto typeWitnessSchema =
            remapConcreteSubstitutionSchema(typeWitness, substitutions,
                                            Context, result);

        // Add a rule T.[P:A].[concrete: Foo.A] => T.[P:A].
        constraintType.add(
            Atom::forConcreteType(
                typeWitnessSchema, result, Context));
      }

      inducedRules.emplace_back(subjectType, constraintType);
      if (DebugConcretizeNestedTypes) {
        llvm::dbgs() << "^^ Induced rule " << constraintType
                     << " => " << subjectType << "\n";
      }
    }
  }
}

void EquivalenceClassMap::dump(llvm::raw_ostream &out) const {
  out << "Equivalence class map: {\n";
  for (const auto &equivClass : Map) {
    out << "  ";
    equivClass->dump(out);
    out << "\n";
  }
  out << "}\n";
}

/// Build the equivalence class map from all rules of the form T.[p] => T, where
/// [p] is a property atom.
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
RewriteSystem::buildEquivalenceClassMap(EquivalenceClassMap &map,
                                        unsigned maxIterations,
                                        unsigned maxDepth) {
  map.clear();

  std::vector<std::pair<MutableTerm, Atom>> properties;

  for (const auto &rule : Rules) {
    if (rule.isDeleted())
      continue;

    const auto &lhs = rule.getLHS();

    // Collect all rules of the form T.[p] => T where T is canonical.
    auto property = lhs.back();
    if (!property.isProperty())
      continue;

    MutableTerm key(lhs.begin(), lhs.end() - 1);
    if (key != rule.getRHS())
      continue;

#ifndef NDEBUG
    assert(!simplify(key) &&
           "Right hand side of a property rule should already be reduced");
#endif

    properties.emplace_back(key, property);
  }

  // EquivalenceClassMap::addRule() requires that shorter rules are added
  // before longer rules, so that it can perform lookups on suffixes and call
  // EquivalenceClass::copyPropertiesFrom().
  std::sort(properties.begin(), properties.end(),
            [&](const std::pair<MutableTerm, Atom> &lhs,
                const std::pair<MutableTerm, Atom> &rhs) -> bool {
              return lhs.first.compare(rhs.first, Protos) < 0;
            });

  // Merging multiple superclass or concrete type rules can induce new rules
  // to unify concrete type constructor arguments.
  SmallVector<std::pair<MutableTerm, MutableTerm>, 3> inducedRules;

  for (auto pair : properties) {
    map.addProperty(pair.first, pair.second, inducedRules);
  }

  // We also need to merge concrete type rules with conformance rules, by
  // concretizing the associated type witnesses of the concrete type.
  map.concretizeNestedTypesFromConcreteParents(inducedRules);

  // Some of the induced rules might be trivial; only count the induced rules
  // where the left hand side is not already equivalent to the right hand side.
  unsigned addedNewRules = 0;
  for (auto pair : inducedRules) {
    if (addRule(pair.first, pair.second)) {
      ++addedNewRules;

      const auto &newRule = Rules.back();
      if (newRule.getLHS().size() > maxDepth)
        return std::make_pair(CompletionResult::MaxDepth, addedNewRules);
    }
  }

  if (Rules.size() > maxIterations)
    return std::make_pair(CompletionResult::MaxIterations, addedNewRules);

  return std::make_pair(CompletionResult::Success, addedNewRules);
}