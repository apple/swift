//===--- ConcreteContraction.cpp - Preprocessing concrete conformances ----===//
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
//
// The concrete contraction pass runs after requirement desugaring and before
// rewrite rules are built from desugared requirements when constructing a
// rewrite system for a user-written generic signature.
//
// This is an imperfect hack to deal with two issues:
//
// a) When a generic parameter is subject to both a conformance requirement and
//    a concrete type requirement (or a superclass requirement where the
//    superclass also conforms to the protocol), the conformance requirement
//    becomes redundant during property map construction.
//
//    However, unlike other kinds of requirements, dropping a conformance
//    requirement can change canonical types in the rewrite system, and in
//    particular, it can change the canonical types of other minimal
//    requirements, if the protocol in the conformance requirement has
//    associated types.
//
//    Consider this example:
//
//        protocol P {
//          associatedtype T
//        }
//
//        protocol Q {}
//
//        struct S<T> : P {}
//
//        struct Holder<A : P, B : P> where A.T : Q {}
//
//        extension Holder where A == S<B.T> {}
//
//    The signature of the extension is built from these requirements:
//
//    - A : P
//    - B : P
//    - A.T : Q
//    - A == S<B.T>
//
//    In this rewrite system, the canonical type of 'B.T' is 'A.T', so the
//    requirements canonicalize as follows:
//
//    - A : P
//    - B : P
//    - A.T : Q
//    - A == S<A.T>
//
//    Also, the first requirement 'A : P' is redundant in this rewrite system,
//    because 'A == S<B.T>' and S conforms to P.
//
//    However, simply dropping 'A : P' is not enough. If the user instead
//    directly wrote a signature with the original requirements omitting
//    'A : P', we would have:
//
//    - B : P
//    - A == S<B.T>
//    - B.T : Q
//
//    Indeed, computing canonical types from the first rewrite system produces
//    different results, because 'B.T' canonicalizes to 'A.T' and not 'B.T'.
//
// b) The GenericSignatureBuilder permitted references to "fully concrete"
//    member types of a dependent type that were not associated types from
//    conformance requirements.
//
//    That is, the following was permitted:
//
//    class C {
//      typealias A = Int
//    }
//
//    <T, U where T : C, U == T.A>
//
//    The requirement 'U == T.A' was resolved to 'U == Int', despite 'T' not
//    declaring any protocol conformance requirements with an associated type
//    named 'A' (or any conformance requirements at all).
//
// The GenericSignatureBuilder dealt with a) using a "rebuilding" pass that
// build a new generic signature after dropping redundant conformance
// requirements, feeding the original requirements back in. The behavior b)
// was a consequence of how requirement resolution was implemented, by calling
// into name lookup.
//
// The Requirement Machine's approach to both a) and b) doesn't handle as many
// cases, but is much simpler and hopefully more robust. Before building the
// rewrite system, we pre-process the requirements and identify generic
// parameters subject to a superclass or concrete type requirement. Then, we
// substitute this generic parameter for the superclass or concrete type,
// respectively, in all other requirements.
//
// In the above example, it produces the following list of requirements:
//
//    - S<B.T> : P
//    - B : P
//    - S<B.T>.T : Q
//    - A == S<B.T>
//
// The requirements are fed back into desugarRequirements(), and we get:
//
//    - B : P
//    - B.T : Q
//    - A == S<B.T>
//
// This also handles b), where the original requirements are:
//
//    - T : C
//    - U == T.A
//
// and after concrete contraction, we get
//
//    - T : C
//    - U == Int
//
// Since this is all a heuristic that is applied before the rewrite system is
// built, it doesn't handle cases where a nested type of a generic parameter is
// subject to both a concrete type and conformance requirement, nor does it
// handle more complex cases where the redundant conformance is only discovered
// via an intermediate same-type requirement, such as the following:
//
//    <T, U, V where T == S<V>, T == U, U : P>
//
// If concrete contraction fails, the minimized generic signature will fail
// verification if it still contains incorrectly-canonicalized types.
//
// We might need a more general solution eventually, but for now this is good
// enough to handle the cases where this arises in practice.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/AST/Requirement.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "NameLookup.h"
#include "RequirementLowering.h"

using namespace swift;
using namespace rewriting;

/// Strip associated types from types used as keys to erase differences between
/// resolved types coming from the parent generic signature and unresolved types
/// coming from user-written requirements.
static Type stripBoundDependentMemberTypes(Type t) {
  if (auto *depMemTy = t->getAs<DependentMemberType>()) {
    return DependentMemberType::get(
      stripBoundDependentMemberTypes(depMemTy->getBase()),
      depMemTy->getName());
  }

  return t;
}

/// Returns true if \p lhs appears as the base of a member type in \p rhs.
static bool typeOccursIn(Type lhs, Type rhs) {
  return rhs.findIf([lhs](Type t) -> bool {
    if (auto *memberType = t->getAs<DependentMemberType>())
      return memberType->getBase()->isEqual(lhs);
    return false;
  });
}

namespace {

/// Utility class to store some shared state.
class ConcreteContraction {
  bool Debug;

  llvm::SmallDenseMap<CanType, llvm::SmallDenseSet<Type, 1>> ConcreteTypes;
  llvm::SmallDenseMap<CanType, llvm::SmallDenseSet<Type, 1>> Superclasses;
  llvm::SmallDenseMap<CanType, llvm::SmallVector<ProtocolDecl *, 1>> Conformances;

  enum Position {
    /// Base type of some other type appearing in a requirement.
    BaseType,

    /// Subject type of a conformance requirement.
    ConformanceRequirement,

    /// Subject type of a superclass requirement.
    SuperclassRequirement,

    /// Subject type of a same-type requirement.
    SameTypeRequirement,

    /// Some other position.
    Other
  };

  llvm::Optional<Type> substTypeParameterRec(Type type,
                                             Position position) const;
  Type substTypeParameter(Type type, Position position) const;
  Type substType(Type type) const;
  Requirement substRequirement(const Requirement &req) const;

  bool preserveSameTypeRequirement(const Requirement &req) const;

  bool hasResolvedMemberTypeOfInterestingParameter(Type t) const;

public:
  ConcreteContraction(bool debug) : Debug(debug) {}

  bool performConcreteContraction(
      ArrayRef<StructuralRequirement> requirements,
      SmallVectorImpl<StructuralRequirement> &result,
      SmallVectorImpl<RequirementError> &errors);
};

}  // end namespace

/// A re-implementation of Type::subst() that also handles unresolved
/// DependentMemberTypes by performing name lookup into the base type.
///
/// When substituting a superclass requirement, we have to handle the
/// case where the superclass might not conform to the protocol in
/// question. For example, you can have a generic signature like this
///
///     <T where T : Sequence, T : SomeClass, T.Element == Int>
///
/// If SomeClass does not conform to Sequence, the type T is understood
/// to be some subclass of SomeClass which does conform to Sequence;
/// this is perfectly valid, and we cannot substitute the 'T.Element'
/// requirement. In this case, this method returns None.
llvm::Optional<Type>
ConcreteContraction::substTypeParameterRec(Type type, Position position) const {

  // If we have a superclass (T : C) or same-type requirement (T == C),
  // don't substitute T, since then we end up with 'C == C' or 'C : C',
  // losing the requirement.
  if (position == Position::BaseType ||
      position == Position::ConformanceRequirement) {
    auto key = stripBoundDependentMemberTypes(type)->getCanonicalType();

    Type concreteType;
    {
      auto found = ConcreteTypes.find(key);
      if (found != ConcreteTypes.end() && found->second.size() == 1)
        concreteType = *found->second.begin();
    }

    Type superclass;
    {
      auto found = Superclasses.find(key);
      if (found != Superclasses.end() && found->second.size() == 1)
        superclass = *found->second.begin();
    }

    // If we have both, prefer the concrete type requirement since it is more
    // specific.
    if (!concreteType && superclass)
      concreteType = superclass;

    if (concreteType) {
      return concreteType;
    }
  }

  if (auto *memberType = type->getAs<DependentMemberType>()) {
    auto baseType = memberType->getBase();
    auto substBaseType = substTypeParameterRec(baseType, Position::BaseType);
    if (!substBaseType)
      return llvm::None;

    // A resolved DependentMemberType stores an associated type declaration.
    //
    // Handle this by looking up the corresponding type witness in the base
    // type's conformance to the associated type's protocol.
    if (auto *assocType = memberType->getAssocType()) {
      auto *proto = assocType->getProtocol();
      auto *module = proto->getParentModule();

      // The 'Sendable' protocol does not declare any associated types, so the
      // 'allowMissing' value here is actually irrelevant.
      auto conformance = ((*substBaseType)->isTypeParameter()
                          ? ProtocolConformanceRef(proto)
                          : module->lookupConformance(
                              *substBaseType, proto,
                              /*allowMissing=*/false));

      if (proto->isSpecificProtocol(KnownProtocolKind::Sendable) &&
          conformance.hasUnavailableConformance()) {
        conformance = ProtocolConformanceRef::forInvalid();
      }

      // The base type doesn't conform, in which case the requirement remains
      // unsubstituted.
      if (!conformance) {
        if (Debug) {
          llvm::dbgs() << "@@@ " << substBaseType << " does not conform to "
                       << proto->getName() << "\n";
        }
        return llvm::None;
      }

      return assocType->getDeclaredInterfaceType()
                      ->castTo<DependentMemberType>()
                      ->substBaseType(module, *substBaseType);
    }

    // An unresolved DependentMemberType stores an identifier. Handle this
    // by performing a name lookup into the base type.
    SmallVector<TypeDecl *> concreteDecls;
    lookupConcreteNestedType(*substBaseType, memberType->getName(), concreteDecls);

    auto *typeDecl = findBestConcreteNestedType(concreteDecls);
    if (typeDecl == nullptr) {
      // The base type doesn't contain a member type with this name, in which
      // case the requirement remains unsubstituted.
      if (Debug) {
        llvm::dbgs() << "@@@ Lookup of " << memberType->getName() << " failed on "
                     << *substBaseType << "\n";
      }
      return llvm::None;
    }

    // Substitute the base type into the member type.
    auto *dc = typeDecl->getDeclContext();
    auto subMap = (*substBaseType)->getContextSubstitutionMap(
        dc->getParentModule(), dc);
    return typeDecl->getDeclaredInterfaceType().subst(subMap);
  }

  return llvm::None;
}

/// Replace the generic parameter at the root of \p type, which must be a
/// type parameter, with the superclass or concrete type requirement that
/// the generic parameter is subject to.
///
/// Note that if the generic parameter has a superclass conformance, we
/// only substitute if it's the root of a member type; the generic parameter
/// itself does not become concrete when it's superclass-constrained, unless
/// it is the subject of a conformance requirement.
Type ConcreteContraction::substTypeParameter(
    Type type, Position position) const {
  assert(type->isTypeParameter());

  auto result = substTypeParameterRec(type, position);
  if (!result)
    return type;

  return *result;
}

/// Substitute all type parameters occurring in structural positions of \p type.
Type ConcreteContraction::substType(Type type) const {
  return type.transformRec([&](Type type) -> llvm::Optional<Type> {
    if (!type->isTypeParameter())
      return llvm::None;

    return substTypeParameter(type, Position::Other);
  });
}

/// Substitute all type parameters occurring in the given requirement.
Requirement
ConcreteContraction::substRequirement(const Requirement &req) const {
  auto firstType = req.getFirstType();

  switch (req.getKind()) {
  case RequirementKind::SameShape: {
    auto substFirstType = substType(firstType);
    auto substSecondType = substType(req.getSecondType());

    return Requirement(req.getKind(), substFirstType, substSecondType);
  }

  case RequirementKind::Superclass:
  case RequirementKind::SameType: {
    auto position = (req.getKind() == RequirementKind::Superclass
                     ? Position::SuperclassRequirement
                     : Position::SameTypeRequirement);
    auto substFirstType = substTypeParameter(firstType, position);

    auto secondType = req.getSecondType();
    auto substSecondType = substType(secondType);

    return Requirement(req.getKind(),
                       substFirstType,
                       substSecondType);
  }

  case RequirementKind::Conformance: {
    auto substFirstType = substTypeParameter(
        firstType, Position::ConformanceRequirement);

    auto *proto = req.getProtocolDecl();
    auto *module = proto->getParentModule();

    // For conformance to 'Sendable', allow synthesis of a missing conformance
    // if the generic parameter is concrete, that is, if we're looking at a
    // signature of the form 'T == Foo, T : Sendable'.
    //
    // Otherwise, we have a superclass requirement, like 'T : C, T : Sendable';
    // don't synthesize the conformance in this case since dropping
    // 'T : Sendable' would be incorrect; we want to ensure that we only admit
    // subclasses of 'C' which are 'Sendable'.
    bool allowMissing = false;
    auto key = stripBoundDependentMemberTypes(firstType)->getCanonicalType();
    if (ConcreteTypes.count(key) > 0)
      allowMissing = true;

    if (!substFirstType->isTypeParameter()) {
      auto conformance = module->lookupConformance(substFirstType, proto,
                                                   allowMissing);

      if (!allowMissing &&
          proto->isSpecificProtocol(KnownProtocolKind::Sendable) &&
          conformance.hasUnavailableConformance()) {
        conformance = ProtocolConformanceRef::forInvalid();
      }

      if (!conformance) {
        // Handle the case of <T where T : P, T : C> where C is a class and
        // C does not conform to P and only substitute the parent type of T
        // by pretending we have a same-type requirement here.
        substFirstType = substTypeParameter(
            firstType, Position::SameTypeRequirement);
      }
    }

    // Otherwise, replace the generic parameter in the conformance
    // requirement with the concrete type. It will desugar to nothing
    // (if the conformance is conditional) or to zero or more
    // conditional requirements needed to satisfy the conformance.
    return Requirement(req.getKind(),
                       substFirstType,
                       req.getSecondType());
  }

  case RequirementKind::Layout: {
    auto substFirstType = substTypeParameter(firstType, Position::Other);
    if (!substFirstType->isTypeParameter() &&
        !substFirstType->satisfiesClassConstraint() &&
        req.getLayoutConstraint()->isClass()) {
      // If the concrete type doesn't satisfy the layout constraint, produce
      // a better diagnostic and only substitute the parent type by pretending
      // we have a same-type requirement here.
      substFirstType = substTypeParameter(
          firstType, Position::SameTypeRequirement);
    }

    return Requirement(req.getKind(),
                       substFirstType,
                       req.getLayoutConstraint());
  }
  }
}

bool ConcreteContraction::
hasResolvedMemberTypeOfInterestingParameter(Type type) const {
  return type.findIf([&](Type t) -> bool {
    if (auto *memberTy = t->getAs<DependentMemberType>()) {
      if (memberTy->getAssocType() == nullptr)
        return false;

      auto key = stripBoundDependentMemberTypes(memberTy->getBase())
          ->getCanonicalType();
      Type concreteType;
      {
        auto found = ConcreteTypes.find(key);
        if (found != ConcreteTypes.end() && found->second.size() == 1)
          return true;
      }

      Type superclass;
      {
        auto found = Superclasses.find(key);
        if (found != Superclasses.end() && found->second.size() == 1)
          return true;
      }
    }

    return false;
  });
}

/// Another silly GenericSignatureBuilder compatibility hack.
///
/// Consider this code:
///
///     class C<T> {
///       typealias A = T
///     }
///
///     protocol P {
///       associatedtype A
///     }
///
///     func f<X, T>(_: X, _: T) where X : P, X : C<T>, X.A == T {}
///
/// The GenericSignatureBuilder would introduce an equivalence between
/// typealias A in class C and associatedtype A in protocol P, so the
/// requirement 'X.A == T' would effectively constrain _both_.
///
/// Simulate this by keeping both the original and substituted same-type
/// requirement in a narrow case.
bool ConcreteContraction::preserveSameTypeRequirement(
    const Requirement &req) const {
  if (req.getKind() != RequirementKind::SameType)
    return false;

  // One of the parent types of this type parameter should be subject
  // to a superclass requirement.
  auto type = stripBoundDependentMemberTypes(req.getFirstType())
      ->getCanonicalType();
  while (true) {
    if (Superclasses.find(type) != Superclasses.end())
      break;

    if (auto memberType = dyn_cast<DependentMemberType>(type)) {
      type = memberType.getBase();
      continue;
    }

    return false;
  }

  if (hasResolvedMemberTypeOfInterestingParameter(req.getFirstType()) ||
      hasResolvedMemberTypeOfInterestingParameter(req.getSecondType()))
    return false;

  return true;
}

/// Substitute all occurrences of generic parameters subject to superclass
/// or concrete type requirements with their corresponding superclass or
/// concrete type.
///
/// If this returns false, \p result should be ignored and the requirements
/// remain unchanged. If this returns true, \p result should replace the
/// original \p requirements.
bool ConcreteContraction::performConcreteContraction(
    ArrayRef<StructuralRequirement> requirements,
    SmallVectorImpl<StructuralRequirement> &result,
    SmallVectorImpl<RequirementError> &errors) {

  // Phase 1 - collect concrete type and superclass requirements where the
  // subject type is a generic parameter.
  for (auto req : requirements) {
    auto subjectType = req.req.getFirstType();
    assert(subjectType->isTypeParameter() &&
           "You forgot to call desugarRequirement()");

    auto kind = req.req.getKind();
    switch (kind) {
    case RequirementKind::SameShape:
      assert(req.req.getSecondType()->isTypeParameter());
      continue;

    case RequirementKind::SameType: {
      auto constraintType = req.req.getSecondType();

      // Same-type requirements between type parameters are not interesting
      // to this pass.
      if (constraintType->isTypeParameter())
        break;

      subjectType = stripBoundDependentMemberTypes(subjectType);
      if (typeOccursIn(subjectType,
                       stripBoundDependentMemberTypes(constraintType))) {
        if (Debug) {
          llvm::dbgs() << "@ Subject type of same-type requirement "
                       << subjectType << " == " << constraintType << " "
                       << "occurs in the constraint type, skipping\n";
        }
        break;
      }
      ConcreteTypes[subjectType->getCanonicalType()].insert(constraintType);
      break;
    }
    case RequirementKind::Superclass: {
      auto constraintType = req.req.getSecondType();
      assert(!constraintType->isTypeParameter() &&
             "You forgot to call desugarRequirement()");

      subjectType = stripBoundDependentMemberTypes(subjectType);
      if (typeOccursIn(subjectType,
                       stripBoundDependentMemberTypes(constraintType))) {
        if (Debug) {
          llvm::dbgs() << "@ Subject type of superclass requirement "
                       << subjectType << " : " << constraintType << " "
                       << "occurs in the constraint type, skipping\n";
        }
        break;
      }
      Superclasses[subjectType->getCanonicalType()].insert(constraintType);
      break;
    }
    case RequirementKind::Conformance: {
      auto *protoDecl = req.req.getProtocolDecl();
      subjectType = stripBoundDependentMemberTypes(subjectType);
      Conformances[subjectType->getCanonicalType()].push_back(protoDecl);

      break;
    }
    case RequirementKind::Layout:
      break;
    }
  }

  // Block concrete contraction if a generic parameter conforms to a protocol P
  // which has a superclass bound C which again conforms to P. This is a really
  // silly edge case, but we go to great pains to produce the same minimized
  // signature as the GenericSignatureBuilder in this case, <T : P>, and not the
  // more logical <T : C>.
  for (const auto &pair : Conformances) {
    auto subjectType = pair.first;
    auto found = Superclasses.find(subjectType);
    if (found == Superclasses.end() || found->second.size() != 1)
      continue;

    auto superclassTy = *found->second.begin();

    for (const auto *proto : pair.second) {
      if (auto otherSuperclassTy = proto->getSuperclass()) {
        if (Debug) {
          llvm::dbgs() << "@ Subject type of superclass requirement "
                       << subjectType << " : " << superclassTy
                       << " conforms to "<< proto->getName()
                       << " which has a superclass bound "
                       << otherSuperclassTy << "\n";
        }

        if (superclassTy->isEqual(otherSuperclassTy)) {
          Superclasses.erase(subjectType);
          break;
        }
      }
    }
  }

  // If there's nothing to do just return.
  if (ConcreteTypes.empty() && Superclasses.empty())
    return false;

  if (Debug) {
    llvm::dbgs() << "@ Concrete types: @\n";
    for (auto pair : ConcreteTypes) {
      llvm::dbgs() << pair.first;
      if (pair.second.size() == 1) {
        llvm::dbgs() << " == " << *pair.second.begin() << "\n";
      } else {
        llvm::dbgs() << " has duplicate concrete type requirements\n";
      }
    }

    llvm::dbgs() << "@ Superclasses: @\n";
    for (auto pair : Superclasses) {
      llvm::dbgs() << pair.first;
      if (pair.second.size() == 1) {
        llvm::dbgs() << " : " << *pair.second.begin() << "\n";
      } else {
        llvm::dbgs() << " has duplicate superclass requirements\n";
      }
    }
  }

  // Phase 2: Replace each concretely-conforming generic parameter with its
  // concrete type.
  for (auto req : requirements) {
    if (Debug) {
      llvm::dbgs() << "@ Original requirement: ";
      req.req.dump(llvm::dbgs());
      llvm::dbgs() << "\n";
    }

    // Substitute the requirement.
    auto substReq = substRequirement(req.req);

    if (Debug) {
      llvm::dbgs() << "@ Substituted requirement: ";
      substReq.dump(llvm::dbgs());
      llvm::dbgs() << "\n";
    }

    // Otherwise, desugar the requirement again, since we might now have a
    // requirement where the left hand side is not a type parameter.
    SmallVector<Requirement, 4> reqs;
    if (req.inferred) {
      // Discard errors from desugaring a substituted requirement that
      // was inferred. For example, if we have something like
      //
      //   <T, U where T == Int, U == Set<T>>
      //
      // The inferred requirement 'T : Hashable' from 'Set<>' will
      // be substituted with 'T == Int' to get 'Int : Hashable'.
      //
      // Desugaring will diagnose a redundant conformance requirement,
      // but we want to ignore that, since the user did not explicitly
      // write 'Int : Hashable' (or 'T : Hashable') anywhere.
      SmallVector<RequirementError, 4> discardErrors;
      desugarRequirement(substReq, SourceLoc(), reqs, discardErrors);
    } else {
      desugarRequirement(substReq, req.loc, reqs, errors);
    }

    for (auto desugaredReq : reqs) {
      if (Debug) {
        llvm::dbgs() << "@@ Desugared requirement: ";
        desugaredReq.dump(llvm::dbgs());
        llvm::dbgs() << "\n";
      }
      result.push_back({desugaredReq, req.loc, req.inferred});
    }

    if (preserveSameTypeRequirement(req.req) &&
        (!req.req.getFirstType()->isEqual(substReq.getFirstType()) ||
         !req.req.getSecondType()->isEqual(substReq.getSecondType()))) {
      if (Debug) {
        llvm::dbgs() << "@ Preserving original requirement: ";
        req.req.dump(llvm::dbgs());
        llvm::dbgs() << "\n";
      }

      // Make the duplicated requirement 'inferred' so that we don't diagnose
      // it as redundant.
      result.push_back({req.req, SourceLoc(), /*inferred=*/true});
    }
  }

  if (Debug) {
    llvm::dbgs() << "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n";
    llvm::dbgs() << "@ Concrete contraction succeeded @\n";
    llvm::dbgs() << "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n";
  }

  return true;
}

/// Substitute all occurrences of generic parameters subject to superclass
/// or concrete type requirements with their corresponding superclass or
/// concrete type.
///
/// If this returns false, \p result should be ignored and the requirements
/// remain unchanged. If this returns true, \p result should replace the
/// original \p requirements.
bool swift::rewriting::performConcreteContraction(
    ArrayRef<StructuralRequirement> requirements,
    SmallVectorImpl<StructuralRequirement> &result,
    SmallVectorImpl<RequirementError> &errors,
    bool debug) {
  ConcreteContraction concreteContraction(debug);
  return concreteContraction.performConcreteContraction(
      requirements, result, errors);
}
