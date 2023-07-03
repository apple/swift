//===--- Diagnostics.cpp - Redundancy and conflict diagnostics ------------===//
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

#include "Diagnostics.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Requirement.h"
#include "swift/AST/Type.h"
#include "RequirementMachine.h"
#include "RewriteSystem.h"

using namespace swift;
using namespace rewriting;

static bool shouldSuggestConcreteTypeFixit(
    Type type, AllowConcreteTypePolicy concreteTypePolicy) {
  switch (concreteTypePolicy) {
  case AllowConcreteTypePolicy::All:
    return true;

  case AllowConcreteTypePolicy::AssocTypes:
    return type->is<DependentMemberType>();

  case AllowConcreteTypePolicy::NestedAssocTypes:
    if (auto *memberType = type->getAs<DependentMemberType>())
      return memberType->getBase()->is<DependentMemberType>();

    return false;
  }
}

/// Emit diagnostics for the given \c RequirementErrors.
///
/// \param ctx The AST context in which to emit diagnostics.
/// \param errors The set of requirement diagnostics to be emitted.
/// \param concreteTypePolicy Whether fix-its should be offered to turn
/// invalid type requirements, e.g. T: Int, into same-type requirements.
///
/// \returns true if any errors were emitted, and false otherwise (including
/// when only warnings were emitted).
bool swift::rewriting::diagnoseRequirementErrors(
    ASTContext &ctx, ArrayRef<RequirementError> errors,
    AllowConcreteTypePolicy concreteTypePolicy) {
  bool diagnosedError = false;

  for (auto error : errors) {
    SourceLoc loc = error.loc;
    if (!loc.isValid())
      continue;

    switch (error.kind) {
    case RequirementError::Kind::InvalidTypeRequirement: {
      if (error.requirement.hasError())
        break;

      Type subjectType = error.requirement.getFirstType();
      Type constraint = error.requirement.getSecondType();

      ctx.Diags.diagnose(loc, diag::requires_conformance_nonprotocol,
                         subjectType, constraint);
      diagnosedError = true;

      auto getNameWithoutSelf = [&](std::string subjectTypeName) {
        std::string selfSubstring = "Self.";

        if (subjectTypeName.rfind(selfSubstring, 0) == 0) {
          return subjectTypeName.erase(0, selfSubstring.length());
        }

        return subjectTypeName;
      };

      if (shouldSuggestConcreteTypeFixit(subjectType, concreteTypePolicy)) {
        auto options = PrintOptions::forDiagnosticArguments();
        auto subjectTypeName = subjectType.getString(options);
        auto subjectTypeNameWithoutSelf = getNameWithoutSelf(subjectTypeName);
        ctx.Diags.diagnose(loc, diag::requires_conformance_nonprotocol_fixit,
                           subjectTypeNameWithoutSelf,
                           constraint.getString(options))
             .fixItReplace(loc, " == ");
      }

      break;
    }

    case RequirementError::Kind::InvalidRequirementSubject: {
      if (error.requirement.hasError())
        break;

      auto subjectType = error.requirement.getFirstType();

      ctx.Diags.diagnose(loc, diag::requires_not_suitable_archetype,
                         subjectType);
      diagnosedError = true;
      break;
    }

    case RequirementError::Kind::InvalidShapeRequirement: {
      if (error.requirement.hasError())
        break;

      auto lhs = error.requirement.getFirstType();
      auto rhs = error.requirement.getSecondType();

      // FIXME: Add tailored messages for specific issues.
      ctx.Diags.diagnose(loc, diag::invalid_shape_requirement,
                         lhs, rhs);
      diagnosedError = true;
      break;
    }

    case RequirementError::Kind::ConflictingRequirement: {
      auto requirement = error.requirement;
      auto conflict = error.conflictingRequirement;

      if (requirement.hasError())
        break;

      if (!conflict) {
        ctx.Diags.diagnose(loc, diag::requires_same_concrete_type,
                           requirement.getFirstType(),
                           requirement.getSecondType());
      } else {
        if (conflict->hasError())
          break;

        auto options = PrintOptions::forDiagnosticArguments();
        std::string requirements;
        llvm::raw_string_ostream OS(requirements);
        OS << "'";
        requirement.print(OS, options);
        OS << "' and '";
        conflict->print(OS, options);
        OS << "'";

        ctx.Diags.diagnose(loc, diag::requirement_conflict,
                           requirement.getFirstType(), requirements);
      }

      diagnosedError = true;
      break;
    }

    case RequirementError::Kind::RecursiveRequirement: {
      auto requirement = error.requirement;

      if (requirement.hasError())
        break;

      assert(requirement.getKind() == RequirementKind::SameType ||
             requirement.getKind() == RequirementKind::Superclass);

      ctx.Diags.diagnose(loc,
                         (requirement.getKind() == RequirementKind::SameType ?
                          diag::recursive_same_type_constraint :
                          diag::recursive_superclass_constraint),
                         requirement.getFirstType(),
                         requirement.getSecondType());

      diagnosedError = true;
      break;
    }

    case RequirementError::Kind::RedundantRequirement: {
      // We only emit redundant requirement warnings if the user passed
      // the -warn-redundant-requirements frontend flag.
      if (!ctx.LangOpts.WarnRedundantRequirements)
        break;

      auto requirement = error.requirement;
      if (requirement.hasError())
        break;

      switch (requirement.getKind()) {
      case RequirementKind::SameShape:
        llvm_unreachable("Same-shape requirement not supported here");

      case RequirementKind::SameType:
        ctx.Diags.diagnose(loc, diag::redundant_same_type_to_concrete,
                           requirement.getFirstType(),
                           requirement.getSecondType());
        break;
      case RequirementKind::Conformance:
        ctx.Diags.diagnose(loc, diag::redundant_conformance_constraint,
                           requirement.getFirstType(),
                           requirement.getProtocolDecl());
        break;
      case RequirementKind::Superclass:
        ctx.Diags.diagnose(loc, diag::redundant_superclass_constraint,
                           requirement.getFirstType(),
                           requirement.getSecondType());
        break;
      case RequirementKind::Layout:
        ctx.Diags.diagnose(loc, diag::redundant_layout_constraint,
                           requirement.getFirstType(),
                           requirement.getLayoutConstraint());
        break;
      }

      break;
    }

    case RequirementError::Kind::UnsupportedSameElement: {
      if (error.requirement.hasError())
        break;

      ctx.Diags.diagnose(loc, diag::unsupported_same_element);
      diagnosedError = true;
      break;
    }
    }
  }

  return diagnosedError;
}

/// Determine whether this is a redundantly inheritable Objective-C protocol.
///
/// A redundantly-inheritable Objective-C protocol is one where we will
/// silently accept a directly-stated redundant conformance to this protocol,
/// and emit this protocol in the list of "inherited" protocols. There are
/// two cases where we allow this:
///
//    1) For a protocol defined in Objective-C, so that we will match Clang's
///      behavior, and
///   2) For an @objc protocol defined in Swift that directly inherits from
///      JavaScriptCore's JSExport, which depends on this behavior.
static bool isRedundantlyInheritableObjCProtocol(const ProtocolDecl *inheritingProto,
                                                 const ProtocolDecl *proto) {
  if (!proto->isObjC()) return false;

  // Check the two conditions in which we will suppress the diagnostic and
  // emit the redundant inheritance.
  if (!inheritingProto->hasClangNode() && !proto->getName().is("JSExport"))
    return false;

  // If the inheriting protocol already has @_restatedObjCConformance with
  // this protocol, we're done.
  for (auto *attr : inheritingProto->getAttrs()
                      .getAttributes<RestatedObjCConformanceAttr>()) {
    if (attr->Proto == proto) return true;
  }

  // Otherwise, add @_restatedObjCConformance.
  auto &ctx = proto->getASTContext();
  const_cast<ProtocolDecl *>(inheritingProto)
      ->getAttrs().add(new (ctx) RestatedObjCConformanceAttr(
          const_cast<ProtocolDecl *>(proto)));
  return true;
}

/// Computes the set of explicit redundant requirements to
/// emit warnings for in the source code.
void RewriteSystem::computeRedundantRequirementDiagnostics(
    SmallVectorImpl<RequirementError> &errors) {
  // Collect all rule IDs for each unique requirement ID.
  llvm::SmallDenseMap<unsigned, llvm::SmallVector<unsigned, 2>>
      rulesPerRequirement;

  // Collect non-explicit requirements that are not redundant.
  llvm::SmallDenseSet<unsigned, 2> nonExplicitNonRedundantRules;

  for (unsigned ruleID = FirstLocalRule, e = Rules.size();
       ruleID < e; ++ruleID) {
    auto &rule = getRules()[ruleID];

    if (rule.isPermanent())
      continue;

    if (!isInMinimizationDomain(rule.getLHS().getRootProtocol()))
      continue;

    // Concrete conformance rules do not map to requirements in the minimized
    // signature; we don't consider them to be 'non-explicit non-redundant',
    // so that a conformance rule (T.[P] => T) expressed in terms of a concrete
    // conformance (T.[concrete: C : P] => T) is still diagnosed as redundant.
    if (auto optSymbol = rule.isPropertyRule()) {
      if (optSymbol->getKind() == Symbol::Kind::ConcreteConformance)
        continue;
    }

    auto requirementID = rule.getRequirementID();

    if (!requirementID.has_value()) {
      if (!rule.isRedundant())
        nonExplicitNonRedundantRules.insert(ruleID);

      continue;
    }

    rulesPerRequirement[*requirementID].push_back(ruleID);
  }

  // Compute the set of redundant rules which transitively reference a
  // non-explicit non-redundant rule. This updates nonExplicitNonRedundantRules.
  //
  // Since earlier redundant paths might reference rules which appear later in
  // the list but not vice versa, walk the redundant paths in reverse order.
  for (const auto &pair : llvm::reverse(RedundantRules)) {
    // Pre-condition: the replacement path only references redundant rules
    // which we have already seen. If any of those rules transitively reference
    // a non-explicit, non-redundant rule, they have been inserted into the
    // nonExplicitNonRedundantRules set on previous iterations.
    unsigned ruleID = pair.first;
    const auto &rewritePath = pair.second;

    // Check if this rewrite path references a rule that is already known to
    // either be non-explicit and non-redundant, or reference such a rule via
    // it's redundancy path.
    for (auto step : rewritePath) {
      switch (step.Kind) {
      case RewriteStep::Rule: {
        if (nonExplicitNonRedundantRules.count(step.getRuleID())) {
          nonExplicitNonRedundantRules.insert(ruleID);
          continue;
        }

        break;
      }

      case RewriteStep::LeftConcreteProjection:
      case RewriteStep::Decompose:
      case RewriteStep::PrefixSubstitutions:
      case RewriteStep::Shift:
      case RewriteStep::Relation:
      case RewriteStep::DecomposeConcrete:
      case RewriteStep::RightConcreteProjection:
        break;
      }
    }

    // Post-condition: If the current replacement path transitively references
    // any non-explicit, non-redundant rules, then nonExplicitNonRedundantRules
    // contains the current rule.
  }

  // We diagnose a redundancy if the rule is redundant, and if its replacement
  // path does not transitively involve any non-explicit, non-redundant rules.
  auto isRedundantRule = [&](unsigned ruleID) {
    const auto &rule = getRules()[ruleID];

    if (!rule.isRedundant())
      return false;

    if (nonExplicitNonRedundantRules.count(ruleID) > 0)
      return false;

    if (rule.isProtocolRefinementRule(Context) &&
        isRedundantlyInheritableObjCProtocol(rule.getLHS()[0].getProtocol(),
                                             rule.getLHS()[1].getProtocol()))
      return false;

    return true;
  };

  // Finally walk through the written requirements, diagnosing any that are
  // redundant.
  for (auto requirementID : indices(WrittenRequirements)) {
    auto requirement = WrittenRequirements[requirementID];

    // Inferred requirements can be re-stated without warning.
    if (requirement.inferred)
      continue;

    auto pairIt = rulesPerRequirement.find(requirementID);

    // If there are no rules for this structural requirement, then the
    // requirement is unnecessary in the source code.
    //
    // This means the requirement was determined to be vacuous by
    // requirement lowering and produced no rules, or the rewrite rules were
    // trivially simplified by RewriteSystem::addRule().
    if (pairIt == rulesPerRequirement.end()) {
      errors.push_back(
          RequirementError::forRedundantRequirement(requirement.req,
                                                    requirement.loc));
      continue;
    }

    // If all rules derived from this structural requirement are redundant,
    // then the requirement is unnecessary in the source code.
    //
    // This means the rules derived from this requirement were all
    // determined to be redundant by homotopy reduction.
    const auto &ruleIDs = pairIt->second;
    if (llvm::all_of(ruleIDs, isRedundantRule)) {
      auto requirement = WrittenRequirements[requirementID];
      errors.push_back(
          RequirementError::forRedundantRequirement(requirement.req,
                                                    requirement.loc));
    }
  }
}

static Requirement
getRequirementForDiagnostics(Type subject, Symbol property,
                             const PropertyMap &map,
                             ArrayRef<GenericTypeParamType *> genericParams,
                             const MutableTerm &prefix) {
  switch (property.getKind()) {
  case Symbol::Kind::ConcreteType: {
    auto concreteType = map.getTypeFromSubstitutionSchema(
        property.getConcreteType(), property.getSubstitutions(),
        genericParams, prefix);
    return Requirement(RequirementKind::SameType, subject, concreteType);
  }

  case Symbol::Kind::Superclass: {
    auto concreteType = map.getTypeFromSubstitutionSchema(
        property.getConcreteType(), property.getSubstitutions(),
        genericParams, prefix);
    return Requirement(RequirementKind::Superclass, subject, concreteType);
  }

  case Symbol::Kind::Protocol:
    return Requirement(RequirementKind::Conformance, subject,
                       property.getProtocol()->getDeclaredInterfaceType());

  case Symbol::Kind::Layout:
    return Requirement(RequirementKind::Layout, subject,
                       property.getLayoutConstraint());

  default:
    llvm::errs() << "Bad property symbol: " << property << "\n";
    abort();
  }
}

void RewriteSystem::computeConflictingRequirementDiagnostics(
    SmallVectorImpl<RequirementError> &errors, SourceLoc signatureLoc,
    const PropertyMap &propertyMap,
    ArrayRef<GenericTypeParamType *> genericParams) {
  for (auto pair : ConflictingRules) {
    const auto &firstRule = getRule(pair.first);
    const auto &secondRule = getRule(pair.second);

    assert(firstRule.isPropertyRule() && secondRule.isPropertyRule());

    if (firstRule.isSubstitutionSimplified() ||
        secondRule.isSubstitutionSimplified())
      continue;

    bool chooseFirstRule = firstRule.getRHS().size() > secondRule.getRHS().size();
    auto subjectRule = chooseFirstRule ? firstRule : secondRule;
    auto subjectTerm = subjectRule.getRHS();

    auto suffixRule = chooseFirstRule ? secondRule : firstRule;
    auto suffixTerm = suffixRule.getRHS();

    // If the root protocol of the subject term isn't in this minimization
    // domain, the conflict was already diagnosed.
    if (!isInMinimizationDomain(subjectTerm[0].getRootProtocol()))
      continue;

    Type subject = propertyMap.getTypeForTerm(subjectTerm, genericParams);
    MutableTerm prefix(subjectTerm.begin(), subjectTerm.end() - suffixTerm.size());
    errors.push_back(RequirementError::forConflictingRequirement(
        getRequirementForDiagnostics(subject, *subjectRule.isPropertyRule(),
                                     propertyMap, genericParams, MutableTerm()),
        getRequirementForDiagnostics(subject, *suffixRule.isPropertyRule(),
                                     propertyMap, genericParams, prefix),
        signatureLoc));
  }
}

void RewriteSystem::computeRecursiveRequirementDiagnostics(
    SmallVectorImpl<RequirementError> &errors, SourceLoc signatureLoc,
    const PropertyMap &propertyMap,
    ArrayRef<GenericTypeParamType *> genericParams) {
  for (unsigned ruleID : RecursiveRules) {
    const auto &rule = getRule(ruleID);

    assert(isInMinimizationDomain(rule.getRHS()[0].getRootProtocol()));

    Type subjectType = propertyMap.getTypeForTerm(rule.getRHS(), genericParams);
    errors.push_back(RequirementError::forRecursiveRequirement(
        getRequirementForDiagnostics(subjectType, *rule.isPropertyRule(),
                                     propertyMap, genericParams, MutableTerm()),
        signatureLoc));
  }
}

void RequirementMachine::computeRequirementDiagnostics(
    SmallVectorImpl<RequirementError> &errors, SourceLoc signatureLoc) {
  System.computeRedundantRequirementDiagnostics(errors);
  System.computeConflictingRequirementDiagnostics(errors, signatureLoc, Map,
                                                  getGenericParams());
  System.computeRecursiveRequirementDiagnostics(errors, signatureLoc, Map,
                                                getGenericParams());
}

std::string RequirementMachine::getRuleAsStringForDiagnostics(
    unsigned ruleID) const {
  const auto &rule = System.getRule(ruleID);

  std::string result;
  llvm::raw_string_ostream out(result);
  out << rule;
  return out.str();
}
