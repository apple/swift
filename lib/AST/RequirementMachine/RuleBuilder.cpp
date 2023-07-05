//===--- RuleBuilder.cpp - Lowering desugared requirements to rules -------===//
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
// This file implements lowering of desugared requirements to rewrite rules,
// as well as rule sharing, which imports confluent rewrite rules from a
// protocol connected component into a rewrite system which references that
// protocol.
//
//===----------------------------------------------------------------------===//

#include "RuleBuilder.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Requirement.h"
#include "swift/AST/RequirementSignature.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SetVector.h"
#include "RequirementMachine.h"
#include "RewriteContext.h"
#include "RewriteSystem.h"
#include "Symbol.h"
#include "Term.h"

using namespace swift;
using namespace rewriting;

/// For building a rewrite system for a generic signature from canonical
/// requirements.
void RuleBuilder::initWithGenericSignature(
    ArrayRef<GenericTypeParamType *> genericParams,
    ArrayRef<Requirement> requirements) {
  assert(!Initialized);
  Initialized = 1;

  // Collect all protocols transitively referenced from these requirements.
  for (auto req : requirements) {
    if (req.getKind() == RequirementKind::Conformance) {
      addReferencedProtocol(req.getProtocolDecl());
    }
  }

  collectRulesFromReferencedProtocols();
  collectPackShapeRules(genericParams);

  // Add rewrite rules for all top-level requirements.
  for (const auto &req : requirements)
    addRequirement(req, /*proto=*/nullptr);
}

/// For building a rewrite system for a generic signature from user-written
/// requirements.
void RuleBuilder::initWithWrittenRequirements(
    ArrayRef<GenericTypeParamType *> genericParams,
    ArrayRef<StructuralRequirement> requirements) {
  assert(!Initialized);
  Initialized = 1;

  // Collect all protocols transitively referenced from these requirements.
  for (auto req : requirements) {
    if (req.req.getKind() == RequirementKind::Conformance) {
      addReferencedProtocol(req.req.getProtocolDecl());
    }
  }

  collectRulesFromReferencedProtocols();
  collectPackShapeRules(genericParams);

  // Add rewrite rules for all top-level requirements.
  for (const auto &req : requirements)
    addRequirement(req, /*proto=*/nullptr);
}

/// For building a rewrite system for a protocol connected component from
/// a previously-built requirement signature.
///
/// Will trigger requirement signature computation if we haven't built
/// requirement signatures for this connected component yet, in which case we
/// will recursively end up building another rewrite system for this component
/// using initWithProtocolWrittenRequirements().
void RuleBuilder::initWithProtocolSignatureRequirements(
    ArrayRef<const ProtocolDecl *> protos) {
  assert(!Initialized);
  Initialized = 1;

  // Add all protocols to the referenced set, so that subsequent calls
  // to addReferencedProtocol() with one of these protocols don't add
  // them to the import list.
  for (auto *proto : protos) {
    ReferencedProtocols.insert(proto);
  }

  for (auto *proto : protos) {
    if (Dump) {
      llvm::dbgs() << "protocol " << proto->getName() << " {\n";
    }

    addPermanentProtocolRules(proto);

    auto reqs = proto->getRequirementSignature();

    // If completion failed, we'll have a totally empty requirement signature,
    // but to maintain invariants around what constitutes a valid rewrite term
    // between getTypeForTerm() and isValidTypeParameter(), we need to add rules
    // for inherited protocols.
    if (reqs.getErrors().contains(GenericSignatureErrorFlags::CompletionFailed)) {
      for (auto *inheritedProto : Context.getInheritedProtocols(proto)) {
        Requirement req(RequirementKind::Conformance,
                        proto->getSelfInterfaceType(),
                        inheritedProto->getDeclaredInterfaceType());

        addRequirement(req.getCanonical(), proto);
      }
    }

    for (auto req : reqs.getRequirements())
      addRequirement(req.getCanonical(), proto);
    for (auto alias : reqs.getTypeAliases())
      addTypeAlias(alias, proto);

    for (auto *otherProto : proto->getProtocolDependencies())
      addReferencedProtocol(otherProto);

    if (Dump) {
      llvm::dbgs() << "}\n";
    }
  }

  // Collect all protocols transitively referenced from this connected component
  // of the protocol dependency graph.
  collectRulesFromReferencedProtocols();
}

/// For building a rewrite system for a protocol connected component from
/// user-written requirements. Used when actually building requirement
/// signatures.
void RuleBuilder::initWithProtocolWrittenRequirements(
    ArrayRef<const ProtocolDecl *> component,
    const llvm::DenseMap<const ProtocolDecl *,
                         SmallVector<StructuralRequirement, 4>> protos) {
  assert(!Initialized);
  Initialized = 1;

  // Add all protocols to the referenced set, so that subsequent calls
  // to addReferencedProtocol() with one of these protocols don't add
  // them to the import list.
  for (const auto *proto : component)
    ReferencedProtocols.insert(proto);

  for (const auto *proto : component) {
    auto found = protos.find(proto);
    assert(found != protos.end());
    const auto &reqs = found->second;

    if (Dump) {
      llvm::dbgs() << "protocol " << proto->getName() << " {\n";
    }

    addPermanentProtocolRules(proto);

    for (auto req : reqs)
      addRequirement(req, proto);

    for (auto *otherProto : proto->getProtocolDependencies())
      addReferencedProtocol(otherProto);

    if (Dump) {
      llvm::dbgs() << "}\n";
    }
  }

  // Collect all protocols transitively referenced from this connected component
  // of the protocol dependency graph.
  collectRulesFromReferencedProtocols();
}

/// For adding conditional conformance requirements to an existing rewrite
/// system. This might pull in additional protocols that we haven't seen
/// before.
///
/// The interface types in the requirements are converted to terms relative
/// to the given array of substitutions, using
/// RewriteContext::getRelativeTermForType().
///
/// For example, given a concrete conformance rule:
///
///    X.Y.[concrete: Array<X.Z> : Equatable]
///
/// The substitutions are {τ_0_0 := X.Z}, and the Array : Equatable conformance
/// has a conditional requirement 'τ_0_0 : Equatable', so the following
/// conformance rule will be added:
///
///    X.Z.[Equatable] => X.Z
void RuleBuilder::initWithConditionalRequirements(
    ArrayRef<Requirement> requirements,
    ArrayRef<Term> substitutions) {
  assert(!Initialized);
  Initialized = 1;

  // Collect all protocols transitively referenced from these requirements.
  for (auto req : requirements) {
    if (req.getKind() == RequirementKind::Conformance) {
      addReferencedProtocol(req.getProtocolDecl());
    }
  }

  collectRulesFromReferencedProtocols();

  // Add rewrite rules for all top-level requirements.
  for (const auto &req : requirements)
    addRequirement(req.getCanonical(), /*proto=*/nullptr, substitutions);
}

/// Add permanent rules for a protocol, consisting of:
///
/// - The identity conformance rule [P].[P] => [P].
/// - An associated type introduction rule for each associated type.
/// - An inherited associated type introduction rule for each associated
///   type of each inherited protocol.
void RuleBuilder::addPermanentProtocolRules(const ProtocolDecl *proto) {
  MutableTerm lhs;
  lhs.add(Symbol::forProtocol(proto, Context));
  lhs.add(Symbol::forProtocol(proto, Context));

  MutableTerm rhs;
  rhs.add(Symbol::forProtocol(proto, Context));

  PermanentRules.emplace_back(lhs, rhs);

  for (auto *assocType : proto->getAssociatedTypeMembers())
    addAssociatedType(assocType, proto);

  for (auto *inheritedProto : Context.getInheritedProtocols(proto)) {
    for (auto *assocType : inheritedProto->getAssociatedTypeMembers())
      addAssociatedType(assocType, proto);
  }
}

/// For an associated type T in a protocol P, we add a rewrite rule:
///
///   [P].T => [P:T]
///
/// Intuitively, this means "if a type conforms to P, it has a nested type
/// named T".
void RuleBuilder::addAssociatedType(const AssociatedTypeDecl *type,
                                    const ProtocolDecl *proto) {
  MutableTerm lhs;
  lhs.add(Symbol::forProtocol(proto, Context));
  lhs.add(Symbol::forName(type->getName(), Context));

  MutableTerm rhs;
  rhs.add(Symbol::forAssociatedType(proto, type->getName(), Context));

  PermanentRules.emplace_back(lhs, rhs);
}

/// Lowers a desugared generic requirement to a rewrite rule.
///
/// Convert a requirement to a rule and add it to the builder.
///
/// The types in the requirement must be canonical.
///
/// If \p proto is null and \p substitutions is None, this is a generic
/// requirement from the top-level generic signature. The added rewrite
/// rule will be rooted in a generic parameter symbol.
///
/// If \p proto is non-null, this is a generic requirement in the protocol's
/// requirement signature. The added rewrite rule will be rooted in a
/// protocol symbol.
///
/// If \p substitutions is not None, this is a conditional requirement
/// added by conditional requirement inference. The added rewrite rule
/// will be added in the corresponding term from the substitution array.
void RuleBuilder::addRequirement(const Requirement &req,
                                 const ProtocolDecl *proto,
                                 llvm::Optional<ArrayRef<Term>> substitutions,
                                 llvm::Optional<unsigned> requirementID) {
  if (Dump) {
    llvm::dbgs() << "+ ";
    req.dump(llvm::dbgs());
    llvm::dbgs() << "\n";
  }

  assert(!substitutions.has_value() || proto == nullptr && "Can't have both");

  // Compute the left hand side.
  auto subjectType = CanType(req.getFirstType());
  auto subjectTerm = (substitutions
                      ? Context.getRelativeTermForType(
                          subjectType, *substitutions)
                      : Context.getMutableTermForType(
                          subjectType, proto));

  // Compute the right hand side.
  MutableTerm constraintTerm;

  switch (req.getKind()) {
  case RequirementKind::SameShape: {
    // A same-shape requirement T.shape == U.shape
    // becomes a rewrite rule:
    //
    //    T.[shape] => U.[shape]
    auto otherType = CanType(req.getSecondType());
    assert(otherType->isParameterPack());

    constraintTerm = (substitutions
                      ? Context.getRelativeTermForType(
                            otherType, *substitutions)
                      : Context.getMutableTermForType(
                            otherType, proto));

    // Add the [shape] symbol to both sides.
    subjectTerm.add(Symbol::forShape(Context));
    constraintTerm.add(Symbol::forShape(Context));
    break;
  }

  case RequirementKind::Conformance: {
    // A conformance requirement T : P becomes a rewrite rule
    //
    //   T.[P] == T
    //
    // Intuitively, this means "any type ending with T conforms to P".
    auto *proto = req.getProtocolDecl();

    constraintTerm = subjectTerm;
    constraintTerm.add(Symbol::forProtocol(proto, Context));
    break;
  }

  case RequirementKind::Superclass: {
    // A superclass requirement T : C<X, Y> becomes a rewrite rule
    //
    //   T.[superclass: C<X, Y>] => T
    auto otherType = CanType(req.getSecondType());

    // Build the symbol [superclass: C<X, Y>].
    SmallVector<Term, 1> result;
    otherType = (substitutions
                 ? Context.getRelativeSubstitutionSchemaFromType(
                    otherType, *substitutions, result)
                 : Context.getSubstitutionSchemaFromType(
                    otherType, proto, result));
    auto superclassSymbol = Symbol::forSuperclass(otherType, result, Context);

    // Build the term T.[superclass: C<X, Y>].
    constraintTerm = subjectTerm;
    constraintTerm.add(superclassSymbol);
    break;
  }

  case RequirementKind::Layout: {
    // A layout requirement T : L becomes a rewrite rule
    //
    //   T.[layout: L] == T
    constraintTerm = subjectTerm;
    constraintTerm.add(Symbol::forLayout(req.getLayoutConstraint(), Context));
    break;
  }

  case RequirementKind::SameType: {
    auto otherType = CanType(req.getSecondType());

    if (!otherType->isTypeParameter()) {
      // A concrete same-type requirement T == C<X, Y> becomes a
      // rewrite rule
      //
      //   T.[concrete: C<X, Y>] => T
      SmallVector<Term, 1> result;
      otherType = (substitutions
                   ? Context.getRelativeSubstitutionSchemaFromType(
                        otherType, *substitutions, result)
                   : Context.getSubstitutionSchemaFromType(
                        otherType, proto, result));

      constraintTerm = subjectTerm;
      constraintTerm.add(Symbol::forConcreteType(otherType, result, Context));
      break;
    }

    constraintTerm = (substitutions
                      ? Context.getRelativeTermForType(
                            otherType, *substitutions)
                      : Context.getMutableTermForType(
                            otherType, proto));
    break;
  }
  }

  RequirementRules.emplace_back(
      std::move(subjectTerm), std::move(constraintTerm),
      requirementID);
}

void RuleBuilder::addRequirement(const StructuralRequirement &req,
                                 const ProtocolDecl *proto) {
  WrittenRequirements.push_back(req);
  unsigned requirementID = WrittenRequirements.size() - 1;
  addRequirement(req.req.getCanonical(), proto, /*substitutions=*/llvm::None,
                 requirementID);
}

/// Lowers a protocol typealias to a rewrite rule.
void RuleBuilder::addTypeAlias(const ProtocolTypeAlias &alias,
                               const ProtocolDecl *proto) {
  // Build the term [P].T, where P is the protocol and T is a name symbol.
  MutableTerm subjectTerm;
  subjectTerm.add(Symbol::forProtocol(proto, Context));
  subjectTerm.add(Symbol::forName(alias.getName(), Context));

  auto constraintType = alias.getUnderlyingType()->getCanonicalType();
  MutableTerm constraintTerm;

  if (constraintType->isTypeParameter()) {
    // If the underlying type of the typealias is a type parameter X, build
    // a rule [P].T => X, where X,
    constraintTerm = Context.getMutableTermForType(
        constraintType, proto);
  } else {
    // If the underlying type of the typealias is a concrete type C, build
    // a rule [P].T.[concrete: C] => [P].T.
    constraintTerm = subjectTerm;

    SmallVector<Term, 1> result;
    auto concreteType =
        Context.getSubstitutionSchemaFromType(
            constraintType, proto, result);

    constraintTerm.add(Symbol::forConcreteType(concreteType, result, Context));
  }

  RequirementRules.emplace_back(subjectTerm, constraintTerm,
                                /*requirementID=*/llvm::None);
}

/// If we haven't seen this protocol yet, save it for later so that we can
/// import the rewrite rules from its connected component.
void RuleBuilder::addReferencedProtocol(const ProtocolDecl *proto) {
  if (ReferencedProtocols.insert(proto).second)
    ProtocolsToImport.push_back(proto);
}

/// Compute the transitive closure of the set of all protocols referenced from
/// the right hand sides of conformance requirements, and import the rewrite
/// rules from the requirement machine for each protocol component.
void RuleBuilder::collectRulesFromReferencedProtocols() {
  // Compute the transitive closure.
  unsigned i = 0;
  while (i < ProtocolsToImport.size()) {
    auto *proto = ProtocolsToImport[i++];
    for (auto *depProto : proto->getProtocolDependencies()) {
      addReferencedProtocol(depProto);
    }
  }

  // If this is a rewrite system for a generic signature, add rewrite rules for
  // each referenced protocol.
  //
  // if this is a rewrite system for a connected component of the protocol
  // dependency graph, add rewrite rules for each referenced protocol not part
  // of this connected component.
  llvm::DenseSet<RequirementMachine *> machines;

  // Now visit each protocol component requirement machine and pull in its rules.
  for (auto *proto : ProtocolsToImport) {
    // This will trigger requirement signature computation for this protocol,
    // if necessary, which will cause us to re-enter into a new RuleBuilder
    // instance under RuleBuilder::initWithProtocolWrittenRequirements().
    if (Dump) {
      llvm::dbgs() << "importing protocol " << proto->getName() << "\n";
    }

    auto *machine = Context.getRequirementMachine(proto);
    if (!machines.insert(machine).second) {
      // We've already seen this protocol component.
      continue;
    }

    // We grab the machine's local rules, not *all* of its rules, to avoid
    // duplicates in case multiple machines share a dependency on a downstream
    // protocol component.
    auto localRules = machine->getLocalRules();
    ImportedRules.insert(ImportedRules.end(),
                         localRules.begin(),
                         localRules.end());
  }
}

void RuleBuilder::collectPackShapeRules(ArrayRef<GenericTypeParamType *> genericParams) {
  if (Dump) {
    llvm::dbgs() << "adding shape rules\n";
  }

  if (!llvm::any_of(genericParams,
                    [](GenericTypeParamType *t) {
                      return t->isParameterPack();
                    })) {
    return;
  }

  // Each non-pack generic parameter is part of the "scalar shape class", represented
  // by the empty term.
  for (auto *genericParam : genericParams) {
    if (genericParam->isParameterPack())
      continue;

    // Add the rule (τ_d_i.[shape] => [shape]).
    MutableTerm lhs;
    lhs.add(Symbol::forGenericParam(
        cast<GenericTypeParamType>(genericParam->getCanonicalType()), Context));
    lhs.add(Symbol::forShape(Context));

    MutableTerm rhs;
    rhs.add(Symbol::forShape(Context));

    PermanentRules.emplace_back(lhs, rhs);
  }

  // A member type T.[P:A] is part of the same shape class as its base type T.
  llvm::DenseSet<Symbol> visited;

  auto addMemberShapeRule = [&](const ProtocolDecl *proto, AssociatedTypeDecl *assocType) {
    auto symbol = Symbol::forAssociatedType(proto, assocType->getName(), Context);
    if (!visited.insert(symbol).second)
      return;

    // Add the rule ([P:A].[shape] => [shape]).
    MutableTerm lhs;
    lhs.add(symbol);
    lhs.add(Symbol::forShape(Context));

    MutableTerm rhs;
    rhs.add(Symbol::forShape(Context));

    // Consider it an imported rule, since it is not part of our minimization
    // domain. It would be more logical if we added these in the protocol component
    // machine for this protocol, but instead we add them in the "leaf" generic
    // signature machine. This avoids polluting machines that do not involve
    // parameter packs with these extra rules, which would otherwise just slow
    // things down.
    Rule rule(Term::get(lhs, Context), Term::get(rhs, Context));
    rule.markPermanent();
    ImportedRules.push_back(rule);
  };

  for (auto *proto : ProtocolsToImport) {
    if (Dump) {
      llvm::dbgs() << "adding member shape rules for protocol " << proto->getName() << "\n";
    }

    for (auto *assocType : proto->getAssociatedTypeMembers()) {
      addMemberShapeRule(proto, assocType);
    }

    for (auto *inheritedProto : Context.getInheritedProtocols(proto)) {
      for (auto *assocType : inheritedProto->getAssociatedTypeMembers()) {
        addMemberShapeRule(proto, assocType);
      }
    }
  }
}