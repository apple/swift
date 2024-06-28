//===--- Diagnostics.cpp - Requirement conflict diagnostics ---------------===//
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
#include "swift/Basic/Assertions.h"
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
      auto requirement = error.getRequirement();
      if (requirement.hasError())
        break;

      Type subjectType = requirement.getFirstType();
      Type constraint = requirement.getSecondType();

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
      auto requirement = error.getRequirement();
      if (requirement.hasError())
        break;

      auto subjectType = requirement.getFirstType();

      ctx.Diags.diagnose(loc, diag::requires_not_suitable_archetype,
                         subjectType);
      diagnosedError = true;
      break;
    }

    case RequirementError::Kind::InvalidInverseSubject: {
      auto inverse = error.getInverse();
      auto subjectType = inverse.subject;
      auto protoKind = getKnownProtocolKind(inverse.getKind());

      StringRef name = getProtocolName(protoKind);

      if (subjectType->is<DependentMemberType>()) {
        // explain that associated types can't have inverses
        ctx.Diags.diagnose(loc, diag::inverse_associatedtype_restriction,
                           name);
      } else {
        // generic diagnostic
        ctx.Diags.diagnose(loc, diag::requires_not_suitable_inverse_subject,
                           subjectType, name);
      }

      diagnosedError = true;
      break;
    }

    case RequirementError::Kind::InvalidInverseOuterSubject: {
      auto inverse = error.getInverse();
      auto subjectType = inverse.subject;
      auto protoKind = getKnownProtocolKind(inverse.getKind());

      ctx.Diags.diagnose(loc, diag::requires_not_suitable_inverse_outer_subject,
                         subjectType.getString(), getProtocolName(protoKind));
      diagnosedError = true;
      break;
    }

    case RequirementError::Kind::ConflictingInverseRequirement: {
      auto inverse = error.getInverse();
      auto protoKind = getKnownProtocolKind(inverse.getKind());

      ctx.Diags.diagnose(loc, diag::inverse_generic_but_also_conforms,
                         inverse.subject,
                         getProtocolName(protoKind));
      break;
    }

    case RequirementError::Kind::InvalidShapeRequirement: {
      auto requirement = error.getRequirement();
      if (requirement.hasError())
        break;

      auto lhs = requirement.getFirstType();
      auto rhs = requirement.getSecondType();

      // FIXME: Add tailored messages for specific issues.
      ctx.Diags.diagnose(loc, diag::invalid_shape_requirement,
                         lhs, rhs);
      diagnosedError = true;
      break;
    }

    case RequirementError::Kind::ConflictingRequirement: {
      auto requirement = error.getRequirement();
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
      auto requirement = error.getRequirement();

      if (requirement.hasError())
        break;

      ASSERT(requirement.getKind() == RequirementKind::SameType ||
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

    case RequirementError::Kind::UnsupportedSameElement: {
      if (error.getRequirement().hasError())
        break;

      ctx.Diags.diagnose(loc, diag::unsupported_same_element);
      diagnosedError = true;
      break;
    }
    }
  }

  return diagnosedError;
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

    ASSERT(firstRule.isPropertyRule() && secondRule.isPropertyRule());

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

    ASSERT(isInMinimizationDomain(rule.getRHS()[0].getRootProtocol()));

    Type subjectType = propertyMap.getTypeForTerm(rule.getRHS(), genericParams);
    errors.push_back(RequirementError::forRecursiveRequirement(
        getRequirementForDiagnostics(subjectType, *rule.isPropertyRule(),
                                     propertyMap, genericParams, MutableTerm()),
        signatureLoc));
  }
}

void RequirementMachine::computeRequirementDiagnostics(
    SmallVectorImpl<RequirementError> &errors,
    ArrayRef<InverseRequirement> inverses,
    SourceLoc signatureLoc) {
  System.computeConflictingRequirementDiagnostics(errors, signatureLoc, Map,
                                                  getGenericParams());
  System.computeRecursiveRequirementDiagnostics(errors, signatureLoc, Map,
                                                getGenericParams());

  // Check that the generic parameters with inverses truly lack the conformance.
  for (auto const& inverse : inverses) {
    // The Superclass and AnyObject checks here are based on the assumption that
    // a class cannot have an inverse applied to it. As a result, the existence
    // of a superclass bound always implies the existence of the conformance.
    // Thus, an inverse being present is a conflict.
    //
    // While AnyObject doesn't imply the conformance in the signature, we don't
    // want a generic parameter to be a class that can't be copied, since we
    // don't allow that for concrete classes today. Thus, we artificially
    // prevent AnyObject from being mixed with inverses.
    if (requiresProtocol(inverse.subject, inverse.protocol) ||
        getSuperclassBound(inverse.subject, getGenericParams()) ||
        requiresClass(inverse.subject))
      errors.push_back(
          RequirementError::forConflictingInverseRequirement(inverse,
                                                             inverse.loc));
  }
}

std::string RequirementMachine::getRuleAsStringForDiagnostics(
    unsigned ruleID) const {
  const auto &rule = System.getRule(ruleID);

  std::string result;
  llvm::raw_string_ostream out(result);
  out << rule;
  return out.str();
}
