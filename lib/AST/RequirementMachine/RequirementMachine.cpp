//===--- RequirementMachine.cpp - Generics with term rewriting --*- C++ -*-===//
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

#include "swift/AST/RequirementMachine.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/Requirement.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/TinyPtrVector.h"
#include <vector>

#include "EquivalenceClassMap.h"
#include "ProtocolGraph.h"
#include "RewriteSystem.h"

using namespace swift;
using namespace rewriting;

namespace {

/// A utility class for bulding a rewrite system from the top-level requirements
/// of a generic signature, and all protocol requirement signatures from all
/// transitively-referenced protocols.
struct RewriteSystemBuilder {
  RewriteContext &Context;
  bool Debug;

  ProtocolGraph Protocols;
  std::vector<std::pair<MutableTerm, MutableTerm>> Rules;

  CanType getConcreteSubstitutionSchema(CanType concreteType,
                                        const ProtocolDecl *proto,
                                        SmallVectorImpl<Term> &result);

  RewriteSystemBuilder(RewriteContext &ctx, bool debug)
    : Context(ctx), Debug(debug) {}
  void addGenericSignature(CanGenericSignature sig);
  void addAssociatedType(const AssociatedTypeDecl *type,
                         const ProtocolDecl *proto);
  void addRequirement(const Requirement &req,
                      const ProtocolDecl *proto);
};

} // end namespace

/// Given a concrete type that may contain type parameters in structural positions,
/// collect all the structural type parameter components, and replace them all with
/// fresh generic parameters. The fresh generic parameters all have a depth of 0,
/// and the index is an index into the 'result' array.
///
/// For example, given the concrete type Foo<X.Y, Array<Z>>, this produces the
/// result type Foo<τ_0_0, Array<τ_0_1>>, with result array {X.Y, Z}.
CanType
RewriteSystemBuilder::getConcreteSubstitutionSchema(CanType concreteType,
                                                    const ProtocolDecl *proto,
                                                    SmallVectorImpl<Term> &result) {
  assert(!concreteType->isTypeParameter() && "Must have a concrete type here");

  if (!concreteType->hasTypeParameter())
    return concreteType;

  return CanType(concreteType.transformRec(
    [&](Type t) -> Optional<Type> {
      if (!t->isTypeParameter())
        return None;

      unsigned index = result.size();
      result.push_back(Context.getTermForType(CanType(t), proto));

      return CanGenericTypeParamType::get(/*depth=*/0, index, Context.getASTContext());
    }));
}

void RewriteSystemBuilder::addGenericSignature(CanGenericSignature sig) {
  // Collect all protocols transitively referenced from the generic signature's
  // requirements.
  Protocols.visitRequirements(sig->getRequirements());
  Protocols.computeTransitiveClosure();
  Protocols.computeLinearOrder();
  Protocols.computeInheritedProtocols();
  Protocols.computeInheritedAssociatedTypes();

  // Add rewrite rules for each protocol.
  for (auto *proto : Protocols.Protocols) {
    if (Debug) {
      llvm::dbgs() << "protocol " << proto->getName() << " {\n";
    }

    const auto &info = Protocols.getProtocolInfo(proto);

    for (auto *type : info.AssociatedTypes)
      addAssociatedType(type, proto);

    for (auto req : info.Requirements)
      addRequirement(req.getCanonical(), proto);

    if (Debug) {
      llvm::dbgs() << "}\n";
    }
  }

  // Add rewrite rules for all requirements in the top-level signature.
  for (const auto &req : sig->getRequirements())
    addRequirement(req, /*proto=*/nullptr);
}

/// For an associated type T in a protocol P, we add a rewrite rule:
///
///   [P].T => [P:T]
///
/// Intuitively, this means "if a type conforms to P, it has a nested type
/// named T".
void RewriteSystemBuilder::addAssociatedType(const AssociatedTypeDecl *type,
                                             const ProtocolDecl *proto) {
  MutableTerm lhs;
  lhs.add(Atom::forProtocol(proto, Context));
  lhs.add(Atom::forName(type->getName(), Context));

  MutableTerm rhs;
  rhs.add(Atom::forAssociatedType(proto, type->getName(), Context));

  Rules.emplace_back(lhs, rhs);
}

/// Lowers a generic requirement to a rewrite rule.
///
/// If \p proto is null, this is a generic requirement from the top-level
/// generic signature. The added rewrite rule will be rooted in a generic
/// parameter atom.
///
/// If \p proto is non-null, this is a generic requirement in the protocol's
/// requirement signature. The added rewrite rule will be rooted in a
/// protocol atom.
void RewriteSystemBuilder::addRequirement(const Requirement &req,
                                          const ProtocolDecl *proto) {
  if (Debug) {
    llvm::dbgs() << "+ ";
    req.dump(llvm::dbgs());
    llvm::dbgs() << "\n";
  }

  // Compute the left hand side.
  auto subjectType = CanType(req.getFirstType());
  auto subjectTerm = Context.getMutableTermForType(subjectType, proto);

  // Compute the right hand side.
  MutableTerm constraintTerm;

  switch (req.getKind()) {
  case RequirementKind::Conformance: {
    // A conformance requirement T : P becomes a rewrite rule
    //
    //   T.[P] == T
    //
    // Intuitively, this means "any type ending with T conforms to P".
    auto *proto = req.getProtocolDecl();

    constraintTerm = subjectTerm;
    constraintTerm.add(Atom::forProtocol(proto, Context));
    break;
  }

  case RequirementKind::Superclass: {
    // A superclass requirement T : C<X, Y> becomes a rewrite rule
    //
    //   T.[superclass: C<X, Y>] => T
    auto otherType = CanType(req.getSecondType());

    SmallVector<Term, 1> substitutions;
    otherType = getConcreteSubstitutionSchema(otherType, proto,
                                              substitutions);

    constraintTerm = subjectTerm;
    constraintTerm.add(Atom::forSuperclass(otherType, substitutions,
                                           Context));
    break;
  }

  case RequirementKind::Layout: {
    // A layout requirement T : L becomes a rewrite rule
    //
    //   T.[layout: L] == T
    constraintTerm = subjectTerm;
    constraintTerm.add(Atom::forLayout(req.getLayoutConstraint(),
                                       Context));
    break;
  }

  case RequirementKind::SameType: {
    auto otherType = CanType(req.getSecondType());

    if (!otherType->isTypeParameter()) {
      // A concrete same-type requirement T == C<X, Y> becomes a
      // rewrite rule
      //
      //   T.[concrete: C<X, Y>] => T
      SmallVector<Term, 1> substitutions;
      otherType = getConcreteSubstitutionSchema(otherType, proto,
                                                substitutions);

      constraintTerm = subjectTerm;
      constraintTerm.add(Atom::forConcreteType(otherType, substitutions,
                                               Context));
      break;
    }

    constraintTerm = Context.getMutableTermForType(otherType, proto);
    break;
  }
  }

  Rules.emplace_back(subjectTerm, constraintTerm);
}


/// We use the PIMPL pattern to avoid creeping header dependencies.
struct RequirementMachine::Implementation {
  RewriteContext Context;
  RewriteSystem System;
  EquivalenceClassMap Map;
  CanGenericSignature Sig;
  bool Complete = false;

  explicit Implementation(ASTContext &ctx)
      : Context(ctx),
        System(Context),
        Map(Context, System.getProtocols()) {}
  void verify(const MutableTerm &term);
  void dump(llvm::raw_ostream &out);
};

void RequirementMachine::Implementation::verify(const MutableTerm &term) {
#ifndef NDEBUG
  MutableTerm erased;

  // First, "erase" resolved associated types from the term, and try
  // to simplify it again.
  for (auto atom : term) {
    if (erased.empty()) {
      switch (atom.getKind()) {
      case Atom::Kind::Protocol:
      case Atom::Kind::GenericParam:
        erased.add(atom);
        continue;

      case Atom::Kind::AssociatedType:
        erased.add(Atom::forProtocol(atom.getProtocols()[0], Context));
        break;

      case Atom::Kind::Name:
      case Atom::Kind::Layout:
      case Atom::Kind::Superclass:
      case Atom::Kind::ConcreteType:
        llvm::errs() << "Bad initial atom in " << term << "\n";
        abort();
        break;
      }
    }

    switch (atom.getKind()) {
    case Atom::Kind::Name:
      assert(!erased.empty());
      erased.add(atom);
      break;

    case Atom::Kind::AssociatedType:
      erased.add(Atom::forName(atom.getName(), Context));
      break;

    case Atom::Kind::Protocol:
    case Atom::Kind::GenericParam:
    case Atom::Kind::Layout:
    case Atom::Kind::Superclass:
    case Atom::Kind::ConcreteType:
      llvm::errs() << "Bad interior atom " << atom << " in " << term << "\n";
      abort();
      break;
    }
  }

  MutableTerm simplified = erased;
  System.simplify(simplified);

  // We should end up with the same term.
  if (simplified != term) {
    llvm::errs() << "Term verification failed\n";
    llvm::errs() << "Initial term:    " << term << "\n";
    llvm::errs() << "Erased term:     " << erased << "\n";
    llvm::errs() << "Simplified term: " << simplified << "\n";
    llvm::errs() << "\n";
    dump(llvm::errs());
    abort();
  }
#endif
}

void RequirementMachine::Implementation::dump(llvm::raw_ostream &out) {
  out << "Requirement machine for " << Sig << "\n";
  System.dump(out);
  Map.dump(out);
}

RequirementMachine::RequirementMachine(ASTContext &ctx) : Context(ctx) {
  Impl = new Implementation(ctx);
}

RequirementMachine::~RequirementMachine() {
  delete Impl;
}

void RequirementMachine::addGenericSignature(CanGenericSignature sig) {
  Impl->Sig = sig;

  PrettyStackTraceGenericSignature debugStack("building rewrite system for", sig);

  auto *Stats = Context.Stats;

  if (Stats)
    ++Stats->getFrontendCounters().NumRequirementMachines;

  FrontendStatsTracer tracer(Stats, "build-rewrite-system");

  if (Context.LangOpts.DebugRequirementMachine) {
    llvm::dbgs() << "Adding generic signature " << sig << " {\n";
  }

  // Collect the top-level requirements, and all transtively-referenced
  // protocol requirement signatures.
  RewriteSystemBuilder builder(Impl->Context,
                               Context.LangOpts.DebugRequirementMachine);
  builder.addGenericSignature(sig);

  // Add the initial set of rewrite rules to the rewrite system, also
  // providing the protocol graph to use for the linear order on terms.
  Impl->System.initialize(std::move(builder.Rules),
                          std::move(builder.Protocols));

  computeCompletion();

  if (Context.LangOpts.DebugRequirementMachine) {
    llvm::dbgs() << "}\n";
  }
}

/// Attempt to obtain a confluent rewrite system using the completion
/// procedure.
void RequirementMachine::computeCompletion() {
  while (true) {
    // First, run the Knuth-Bendix algorithm to resolve overlapping rules.
    auto result = Impl->System.computeConfluentCompletion(
        Context.LangOpts.RequirementMachineStepLimit,
        Context.LangOpts.RequirementMachineDepthLimit);

    if (Context.Stats) {
      Context.Stats->getFrontendCounters()
          .NumRequirementMachineCompletionSteps += result.second;
    }

    // Check for failure.
    auto checkCompletionResult = [&]() {
      switch (result.first) {
      case RewriteSystem::CompletionResult::Success:
        break;

      case RewriteSystem::CompletionResult::MaxIterations:
        llvm::errs() << "Generic signature " << Impl->Sig
                     << " exceeds maximum completion step count\n";
        Impl->System.dump(llvm::errs());
        abort();

      case RewriteSystem::CompletionResult::MaxDepth:
        llvm::errs() << "Generic signature " << Impl->Sig
                     << " exceeds maximum completion depth\n";
        Impl->System.dump(llvm::errs());
        abort();
      }
    };

    checkCompletionResult();

    // Simplify right hand sides in preparation for building the
    // equivalence class map.
    Impl->System.simplifyRightHandSides();

    // Build the equivalence class map, which performs concrete term
    // unification; if this added any new rules, run the completion
    // procedure again.
    result = Impl->System.buildEquivalenceClassMap(
        Impl->Map,
        Context.LangOpts.RequirementMachineStepLimit,
        Context.LangOpts.RequirementMachineDepthLimit);

    if (Context.Stats) {
      Context.Stats->getFrontendCounters()
        .NumRequirementMachineUnifiedConcreteTerms += result.second;
    }

    checkCompletionResult();

    // If buildEquivalenceClassMap() added new rules, we run another
    // round of Knuth-Bendix, and build the equivalence class map again.
    if (result.second == 0)
      break;
  }

  if (Context.LangOpts.DebugRequirementMachine) {
    dump(llvm::dbgs());
  }

  assert(!Impl->Complete);
  Impl->Complete = true;
}

bool RequirementMachine::isComplete() const {
  return Impl->Complete;
}

void RequirementMachine::dump(llvm::raw_ostream &out) const {
  Impl->dump(out);
}

bool RequirementMachine::requiresClass(Type depType) const {
  auto term = Impl->Context.getMutableTermForType(depType->getCanonicalType(),
                                                  /*proto=*/nullptr);
  Impl->System.simplify(term);
  Impl->verify(term);

  auto *equivClass = Impl->Map.lookUpEquivalenceClass(term);
  if (!equivClass)
    return false;

  if (equivClass->isConcreteType())
    return false;

  auto layout = equivClass->getLayoutConstraint();
  return (layout && layout->isClass());
}

LayoutConstraint RequirementMachine::getLayoutConstraint(Type depType) const {
  auto term = Impl->Context.getMutableTermForType(depType->getCanonicalType(),
                                                  /*proto=*/nullptr);
  Impl->System.simplify(term);
  Impl->verify(term);

  auto *equivClass = Impl->Map.lookUpEquivalenceClass(term);
  if (!equivClass)
    return LayoutConstraint();

  return equivClass->getLayoutConstraint();
}

bool RequirementMachine::requiresProtocol(Type depType,
                                          const ProtocolDecl *proto) const {
  auto term = Impl->Context.getMutableTermForType(depType->getCanonicalType(),
                                                  /*proto=*/nullptr);
  Impl->System.simplify(term);
  Impl->verify(term);

  auto *equivClass = Impl->Map.lookUpEquivalenceClass(term);
  if (!equivClass)
    return false;

  if (equivClass->isConcreteType())
    return false;

  for (auto *otherProto : equivClass->getConformsTo()) {
    if (otherProto == proto)
      return true;
  }

  return false;
}

GenericSignature::RequiredProtocols
RequirementMachine::getRequiredProtocols(Type depType) const {
  auto term = Impl->Context.getMutableTermForType(depType->getCanonicalType(),
                                                  /*proto=*/nullptr);
  Impl->System.simplify(term);
  Impl->verify(term);

  auto *equivClass = Impl->Map.lookUpEquivalenceClass(term);
  if (!equivClass)
    return { };

  if (equivClass->isConcreteType())
    return { };

  GenericSignature::RequiredProtocols result;
  for (auto *otherProto : equivClass->getConformsTo()) {
    result.push_back(const_cast<ProtocolDecl *>(otherProto));
  }

  ProtocolType::canonicalizeProtocols(result);

  return result;
}

bool RequirementMachine::isConcreteType(Type depType) const {
  auto term = Impl->Context.getMutableTermForType(depType->getCanonicalType(),
                                                  /*proto=*/nullptr);
  Impl->System.simplify(term);
  Impl->verify(term);

  auto *equivClass = Impl->Map.lookUpEquivalenceClass(term);
  if (!equivClass)
    return false;

  return equivClass->isConcreteType();
}

bool RequirementMachine::areSameTypeParameterInContext(Type depType1,
                                                       Type depType2) const {
  auto term1 = Impl->Context.getMutableTermForType(depType1->getCanonicalType(),
                                                   /*proto=*/nullptr);
  Impl->System.simplify(term1);
  Impl->verify(term1);

  auto term2 = Impl->Context.getMutableTermForType(depType2->getCanonicalType(),
                                                   /*proto=*/nullptr);
  Impl->System.simplify(term2);
  Impl->verify(term2);

  return (term1 == term2);
}
