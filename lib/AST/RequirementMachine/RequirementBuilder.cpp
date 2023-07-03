//===--- RequirementBuilder.cpp - Building requirements from rules --------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the final step in generic signature minimization,
// building requirements from a set of minimal, canonical rewrite rules.
//
// The main entry point is RequirementMachine::buildRequirementsFromRules(),
// called from the RequirementSignatureRequest, AbstractGenericSignatureRequest
// and InferredGenericSignatureRequest requests defined in
// RequirementMachineRequests.cpp.
//
//===----------------------------------------------------------------------===//

#include "RequirementMachine.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Requirement.h"
#include "swift/AST/RequirementSignature.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include <vector>

using namespace swift;
using namespace rewriting;

namespace {

/// Represents a set of types related by same-type requirements, and an
/// optional concrete type requirement.
struct ConnectedComponent {
  llvm::SmallVector<Type, 2> Members;
  llvm::SmallVector<Identifier, 1> Aliases;
  Type ConcreteType;

  void buildRequirements(Type subjectType,
                         RequirementKind kind,
                         std::vector<Requirement> &reqs,
                         std::vector<ProtocolTypeAlias> &aliases);
};

/// Case 1: A set of rewrite rules of the form:
///
///   B => A
///   C => A
///   D => A
///
/// Become a series of same-type requirements
///
///   A == B, B == C, C == D
///
/// Case 2: A set of rewrite rules of the form:
///
///   A.[concrete: X] => A
///   B => A
///   C => A
///   D => A
///
/// Become a series of same-type requirements
///
///   A == X, B == X, C == X, D == X
void ConnectedComponent::buildRequirements(Type subjectType,
                                           RequirementKind kind,
                                           std::vector<Requirement> &reqs,
                                           std::vector<ProtocolTypeAlias> &aliases) {
  std::sort(Members.begin(), Members.end(),
            [](Type first, Type second) -> bool {
              return compareDependentTypes(first, second) < 0;
            });

  if (!ConcreteType) {
    for (auto name : Aliases) {
      aliases.emplace_back(name, subjectType);
    }

    for (auto constraintType : Members) {
      reqs.emplace_back(kind, subjectType, constraintType);
      subjectType = constraintType;
    }

  } else {
    // Shape requirements cannot be concrete.
    assert(kind == RequirementKind::SameType);

    // If there are multiple protocol typealiases in the connected component,
    // lower them all to a series of identical concrete-type aliases.
    for (auto name : Aliases) {
      aliases.emplace_back(name, ConcreteType);
    }

    // If the most canonical representative in the connected component is an
    // unresolved DependentMemberType, it must be of the form 'Self.A'
    // where 'A' is an alias. Emit the concrete-type alias itself.
    if (auto *memberTy = subjectType->getAs<DependentMemberType>()) {
      if (memberTy->getAssocType() == nullptr) {
        auto *paramTy = memberTy->getBase()->castTo<GenericTypeParamType>();
        assert(paramTy->getDepth() == 0 && paramTy->getIndex() == 0);
        (void) paramTy;

        aliases.emplace_back(memberTy->getName(), ConcreteType);

        assert(Members.empty());
        return;
      }
    }

    // Otherwise, the most canonical representative must be a resolved
    // associated type. Emit a requirement.
    reqs.emplace_back(RequirementKind::SameType,
                      subjectType, ConcreteType);

    // Finally, emit a concrete type requirement for all resolved type members
    // of the connected component.
    for (auto constraintType : Members) {
      reqs.emplace_back(RequirementKind::SameType,
                        constraintType, ConcreteType);
    }
  }
}

/// Once we're done with minimization, we turn the minimal rules into requirements.
/// This is in a sense the inverse of RuleBuilder in RequirementLowering.cpp.
class RequirementBuilder {
  // Input parameters.
  const RewriteSystem &System;
  const PropertyMap &Map;
  ArrayRef<GenericTypeParamType *> GenericParams;
  bool ReconstituteSugar;
  bool Debug;

  // Temporary state populated by addRequirementRules() and
  // addTypeAliasRules().
  llvm::SmallDenseMap<Term, ConnectedComponent> Components;

public:
  // Results.
  std::vector<Requirement> Reqs;
  std::vector<ProtocolTypeAlias> Aliases;

  RequirementBuilder(const RewriteSystem &system, const PropertyMap &map,
                     ArrayRef<GenericTypeParamType *> genericParams,
                     bool reconstituteSugar)
    : System(system), Map(map),
      GenericParams(genericParams),
      ReconstituteSugar(reconstituteSugar),
      Debug(System.getDebugOptions().contains(DebugFlags::Minimization)) {}

  void addRequirementRules(ArrayRef<unsigned> rules);
  void addTypeAliasRules(ArrayRef<unsigned> rules);

  void processConnectedComponents();

  void sortRequirements();
  void sortTypeAliases();
};

}  // end namespace

static Type replaceTypeParametersWithErrorTypes(Type type) {
  return type.transformRec([](Type t) -> llvm::Optional<Type> {
    if (t->isTypeParameter())
      return ErrorType::get(t->getASTContext());
    return llvm::None;
  });
}

void RequirementBuilder::addRequirementRules(ArrayRef<unsigned> rules) {
  // Convert a rewrite rule into a requirement.
  auto createRequirementFromRule = [&](const Rule &rule) {
    if (auto prop = rule.isPropertyRule()) {
      auto subjectType = Map.getTypeForTerm(rule.getRHS(), GenericParams);

      switch (prop->getKind()) {
      case Symbol::Kind::Protocol:
        Reqs.emplace_back(RequirementKind::Conformance,
                          subjectType,
                          prop->getProtocol()->getDeclaredInterfaceType());
        return;

      case Symbol::Kind::Layout:
        Reqs.emplace_back(RequirementKind::Layout,
                          subjectType,
                          prop->getLayoutConstraint());
        return;

      case Symbol::Kind::Superclass: {
        // Requirements containing unresolved name symbols originate from
        // invalid code and should not appear in the generic signature.
        for (auto term : prop->getSubstitutions()) {
          if (term.containsUnresolvedSymbols())
            return;
        }

        Type superclassType = Map.getTypeFromSubstitutionSchema(
                                prop->getConcreteType(),
                                prop->getSubstitutions(),
                                GenericParams, MutableTerm());
        if (rule.isRecursive())
          superclassType = replaceTypeParametersWithErrorTypes(superclassType);

        if (ReconstituteSugar)
          superclassType = superclassType->reconstituteSugar(/*recursive=*/true);

        Reqs.emplace_back(RequirementKind::Superclass,
                          subjectType, superclassType);
        return;
      }

      case Symbol::Kind::ConcreteType: {
        // Requirements containing unresolved name symbols originate from
        // invalid code and should not appear in the generic signature.
        for (auto term : prop->getSubstitutions()) {
          if (term.containsUnresolvedSymbols())
            return;
        }

        Type concreteType = Map.getTypeFromSubstitutionSchema(
                                prop->getConcreteType(),
                                prop->getSubstitutions(),
                                GenericParams, MutableTerm());
        if (rule.isRecursive())
          concreteType = replaceTypeParametersWithErrorTypes(concreteType);

        if (ReconstituteSugar)
          concreteType = concreteType->reconstituteSugar(/*recursive=*/true);

        auto &component = Components[rule.getRHS()];
        assert(!component.ConcreteType);
        component.ConcreteType = concreteType;
        return;
      }

      case Symbol::Kind::ConcreteConformance:
        // "Concrete conformance requirements" are not recorded in the generic
        // signature.
        return;

      case Symbol::Kind::Name:
      case Symbol::Kind::AssociatedType:
      case Symbol::Kind::GenericParam:
      case Symbol::Kind::Shape:
        break;
      }

      llvm_unreachable("Invalid symbol kind");
    }

    assert(rule.getLHS().back().getKind() != Symbol::Kind::Protocol);

    MutableTerm constraintTerm(rule.getLHS());
    if (constraintTerm.back().getKind() == Symbol::Kind::Shape) {
      assert(rule.getRHS().back().getKind() == Symbol::Kind::Shape);
      // Strip off the shape symbol from the constraint term.
      constraintTerm = MutableTerm(constraintTerm.begin(),
                                   constraintTerm.end() - 1);
    }

    auto constraintType = Map.getTypeForTerm(constraintTerm, GenericParams);
    Components[rule.getRHS()].Members.push_back(constraintType);
  };

  if (Debug) {
    llvm::dbgs() << "\nMinimized rules:\n";
  }

  // Build the list of requirements, storing same-type requirements off
  // to the side.
  for (unsigned ruleID : rules) {
    const auto &rule = System.getRule(ruleID);

    if (Debug) {
      llvm::dbgs() << "- " << rule << "\n";
    }

    createRequirementFromRule(rule);
  }
}

void RequirementBuilder::addTypeAliasRules(ArrayRef<unsigned> rules) {
  for (unsigned ruleID : rules) {
    const auto &rule = System.getRule(ruleID);
    auto name = *rule.isProtocolTypeAliasRule();

    if (auto prop = rule.isPropertyRule()) {
      assert(prop->getKind() == Symbol::Kind::ConcreteType);

      // Requirements containing unresolved name symbols originate from
      // invalid code and should not appear in the generic signature.
      for (auto term : prop->getSubstitutions()) {
        if (term.containsUnresolvedSymbols())
          continue;
      }

      Type concreteType = Map.getTypeFromSubstitutionSchema(
                               prop->getConcreteType(),
                               prop->getSubstitutions(),
                               GenericParams, MutableTerm());
      if (rule.isRecursive())
        concreteType = replaceTypeParametersWithErrorTypes(concreteType);

      if (ReconstituteSugar)
        concreteType = concreteType->reconstituteSugar(/*recursive=*/true);

      auto &component = Components[rule.getRHS()];
      assert(!component.ConcreteType);
      Components[rule.getRHS()].ConcreteType = concreteType;
    } else {
      Components[rule.getRHS()].Aliases.push_back(name);
    }
  }
}

void RequirementBuilder::processConnectedComponents() {
  // Now, convert each connected component into a series of same-type
  // requirements.
  for (auto &pair : Components) {
    MutableTerm subjectTerm(pair.first);
    RequirementKind kind;
    if (subjectTerm.back().getKind() == Symbol::Kind::Shape) {
      kind = RequirementKind::SameShape;
      // Strip off the shape symbol from the subject term.
      subjectTerm = MutableTerm(subjectTerm.begin(), subjectTerm.end() - 1);
    } else {
      kind = RequirementKind::SameType;
    }

    auto subjectType = Map.getTypeForTerm(subjectTerm, GenericParams);
    pair.second.buildRequirements(subjectType, kind, Reqs, Aliases);
  }
}

void RequirementBuilder::sortRequirements() {
  llvm::array_pod_sort(Reqs.begin(), Reqs.end(),
                       [](const Requirement *lhs, const Requirement *rhs) -> int {
                         return lhs->compare(*rhs);
                       });

  if (Debug) {
    llvm::dbgs() << "Requirements:\n";
    for (const auto &req : Reqs) {
      req.dump(llvm::dbgs());
      llvm::dbgs() << "\n";
    }
  }
}

void RequirementBuilder::sortTypeAliases() {
  llvm::array_pod_sort(Aliases.begin(), Aliases.end(),
                       [](const ProtocolTypeAlias *lhs,
                          const ProtocolTypeAlias *rhs) -> int {
                         return lhs->getName().compare(rhs->getName());
                       });

  if (Debug) {
    llvm::dbgs() << "\nMinimized type aliases:\n";
    for (const auto &alias : Aliases) {
      PrintOptions opts;
      opts.ProtocolQualifiedDependentMemberTypes = true;

      llvm::dbgs() << "- " << alias.getName() << " == ";
      alias.getUnderlyingType().print(llvm::dbgs(), opts);
      llvm::dbgs() << "\n";
    }
  }
}

/// Convert a list of non-permanent, non-redundant rewrite rules into a list of
/// requirements sorted in canonical order. The \p genericParams are used to
/// produce sugared types.
void
RequirementMachine::buildRequirementsFromRules(
    ArrayRef<unsigned> requirementRules,
    ArrayRef<unsigned> typeAliasRules,
    ArrayRef<GenericTypeParamType *> genericParams,
    bool reconstituteSugar,
    std::vector<Requirement> &reqs,
    std::vector<ProtocolTypeAlias> &aliases) const {
  RequirementBuilder builder(System, Map, genericParams, reconstituteSugar);

  builder.addRequirementRules(requirementRules);
  builder.addTypeAliasRules(typeAliasRules);
  builder.processConnectedComponents();
  builder.sortRequirements();
  builder.sortTypeAliases();

  reqs = std::move(builder.Reqs);
  aliases = std::move(builder.Aliases);
}
