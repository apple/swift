//===--- RequirementMatch.h - Result of conformance check -------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_AST_REQUIREMENTMATCH_H
#define SWIFT_AST_REQUIREMENTMATCH_H

#include "swift/AST/RequirementEnvironment.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "swift/AST/Witness.h"
#include "swift/Basic/Debug.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

/// The result of matching a particular declaration to a given
/// requirement.
enum class MatchKind : uint8_t {
  /// The witness matched the requirement exactly.
  ExactMatch,

  /// The witness has fewer effects than the requirement, which is okay.
  FewerEffects,

  /// The witness is @Sendable and the requirement is not. Okay in certain
  /// language modes.
  RequiresNonSendable,

  /// There is a difference in optionality.
  OptionalityConflict,

  /// The witness matched the requirement with some renaming.
  RenamedMatch,

  /// The witness is invalid or has an invalid type.
  WitnessInvalid,

  /// The witness is currently being type checked and this type checking in turn
  /// triggered conformance checking, so the witness cannot be considered as a
  /// candidate.
  Circularity,

  /// The kind of the witness and requirement differ, e.g., one
  /// is a function and the other is a variable.
  KindConflict,

  /// The types conflict.
  TypeConflict,

  /// The witness would match if an additional requirement were met.
  MissingRequirement,

  /// The witness and requirement disagree on 'async'.
  AsyncConflict,

  /// The witness throws, but the requirement does not.
  ThrowsConflict,

  /// The witness did not match due to static/non-static differences.
  StaticNonStaticConflict,

  /// The witness is not settable, but the requirement is.
  SettableConflict,

  /// The witness did not match due to prefix/non-prefix differences.
  PrefixNonPrefixConflict,

  /// The witness did not match due to postfix/non-postfix differences.
  PostfixNonPostfixConflict,

  /// The witness did not match because of mutating conflicts.
  MutatingConflict,

  /// The witness did not match because of nonmutating conflicts.
  NonMutatingConflict,

  /// The witness did not match because of __consuming conflicts.
  ConsumingConflict,

  /// The witness throws unconditionally, but the requirement rethrows.
  RethrowsConflict,

  /// The witness rethrows via conformance, but the requirement rethrows
  /// via closure and is not in a '@rethrows' protocol.
  RethrowsByConformanceConflict,

  /// The witness is explicitly @nonobjc but the requirement is @objc.
  NonObjC,

  /// The witness is missing a `@differentiable` attribute from the requirement.
  MissingDifferentiableAttr,
  
  /// The witness did not match because it is an enum case with
  /// associated values.
  EnumCaseWithAssociatedValues,

  /// The witness did not match due to _const/non-_const differences.
  CompileTimeConstConflict,
};

// Describes the kind of optional adjustment performed when
/// comparing two types.
enum class OptionalAdjustmentKind {
  // No adjustment required.
  None,

  /// The witness can produce a 'nil' that won't be handled by
  /// callers of the requirement. This is a type-safety problem.
  ProducesUnhandledNil,

  /// Callers of the requirement can provide 'nil', but the witness
  /// does not handle it. This is a type-safety problem.
  ConsumesUnhandledNil,

  /// The witness handles 'nil', but won't ever be given a 'nil'.
  /// This is not a type-safety problem.
  WillNeverConsumeNil,

  /// Callers of the requirement can expect to receive 'nil', but
  /// the witness will never produce one. This is not a type-safety
  /// problem.
  WillNeverProduceNil,

  /// The witness has an IUO that can be removed, because the
  /// protocol doesn't need it. This is not a type-safety problem.
  RemoveIUO,

  /// The witness has an IUO that should be translated into a true
  /// optional. This is not a type-safety problem.
  IUOToOptional,
};

/// Describes an optional adjustment made to a witness.
class OptionalAdjustment {
  /// The kind of adjustment.
  unsigned Kind : 16;

  /// Whether this is a parameter adjustment (with an index) vs. a
  /// result or value type adjustment (no index needed).
  unsigned IsParameterAdjustment : 1;

  /// The adjustment index, for parameter adjustments.
  unsigned ParameterAdjustmentIndex : 15;

public:
  /// Create a non-parameter optional adjustment.
  explicit OptionalAdjustment(OptionalAdjustmentKind kind)
    : Kind(static_cast<unsigned>(kind)), IsParameterAdjustment(false),
      ParameterAdjustmentIndex(0) { }

  /// Create an optional adjustment to a parameter.
  OptionalAdjustment(OptionalAdjustmentKind kind,
                     unsigned parameterIndex)
    : Kind(static_cast<unsigned>(kind)), IsParameterAdjustment(true),
      ParameterAdjustmentIndex(parameterIndex) { }

  /// Determine the kind of optional adjustment.
  OptionalAdjustmentKind getKind() const {
    return static_cast<OptionalAdjustmentKind>(Kind);
  }

  /// Determine whether this is a parameter adjustment.
  bool isParameterAdjustment() const {
    return IsParameterAdjustment;
  }

  /// Return the index of a parameter adjustment.
  unsigned getParameterIndex() const {
    assert(isParameterAdjustment() && "Not a parameter adjustment");
    return ParameterAdjustmentIndex;
  }

  /// Determines whether the optional adjustment is an error.
  bool isError() const {
    switch (getKind()) {
    case OptionalAdjustmentKind::None:
      return false;

    case OptionalAdjustmentKind::ProducesUnhandledNil:
    case OptionalAdjustmentKind::ConsumesUnhandledNil:
      return true;

    case OptionalAdjustmentKind::WillNeverConsumeNil:
    case OptionalAdjustmentKind::WillNeverProduceNil:
    case OptionalAdjustmentKind::RemoveIUO:
    case OptionalAdjustmentKind::IUOToOptional:
      // Warnings at most.
      return false;
    }

    llvm_unreachable("Unhandled OptionalAdjustmentKind in switch.");
  }

  /// Retrieve the source location at which the optional is
  /// specified or would be inserted.
  SourceLoc getOptionalityLoc(ValueDecl *witness) const;

  /// Retrieve the optionality location for the given type
  /// representation.
  SourceLoc getOptionalityLoc(TypeRepr *tyR) const;
};

/// Once a witness has been found, there are several reasons it may
/// not be usable.
enum class CheckKind : unsigned {
  /// The witness is OK.
  Success,

  /// The witness is less accessible than the requirement.
  Access,

  /// The witness is storage whose setter is less accessible than the
  /// requirement.
  AccessOfSetter,

  /// The witness needs to be @usableFromInline.
  UsableFromInline,

  /// The witness is less available than the requirement.
  Availability,

  /// The requirement was marked explicitly unavailable.
  Unavailable,

  /// The witness requires optional adjustments.
  OptionalityConflict,

  /// The witness is a constructor which is more failable than the
  /// requirement.
  ConstructorFailability,

  /// The witness itself is inaccessible.
  WitnessUnavailable,

  /// The witness is a deprecated default implementation provided by the
  /// protocol.
  DefaultWitnessDeprecated,
};
/// Describes the suitability of the chosen witness for
/// the requirement.
struct RequirementCheck {
  CheckKind Kind;

  /// The required access scope, if the check failed due to the
  /// witness being less accessible than the requirement.
  AccessScope RequiredAccessScope;

  /// The required availability, if the check failed due to the
  /// witness being less available than the requirement.
  AvailabilityContext RequiredAvailability;

  RequirementCheck(CheckKind kind)
    : Kind(kind), RequiredAccessScope(AccessScope::getPublic()),
      RequiredAvailability(AvailabilityContext::alwaysAvailable()) { }

  RequirementCheck(CheckKind kind, AccessScope requiredAccessScope)
    : Kind(kind), RequiredAccessScope(requiredAccessScope),
      RequiredAvailability(AvailabilityContext::alwaysAvailable()) { }

  RequirementCheck(CheckKind kind, AvailabilityContext requiredAvailability)
    : Kind(kind), RequiredAccessScope(AccessScope::getPublic()),
      RequiredAvailability(requiredAvailability) { }
};


/// Describes a match between a requirement and a witness.
struct RequirementMatch {
  RequirementMatch(ValueDecl *witness, MatchKind kind,
                   std::optional<RequirementEnvironment> env = std::nullopt)
      : Witness(witness), Kind(kind), WitnessType(), ReqEnv(std::move(env)) {
    assert(!hasWitnessType() && "Should have witness type");
  }

  RequirementMatch(ValueDecl *witness, MatchKind kind,
                   const DeclAttribute *attr)
      : Witness(witness), Kind(kind), WitnessType(), UnmetAttribute(attr),
        ReqEnv(std::nullopt) {
    assert(!hasWitnessType() && "Should have witness type");
    assert(hasUnmetAttribute() && "Should have unmet attribute");
  }

  RequirementMatch(ValueDecl *witness, MatchKind kind, Type witnessType,
                   std::optional<RequirementEnvironment> env = std::nullopt,
                   ArrayRef<OptionalAdjustment> optionalAdjustments = {},
                   GenericSignature derivativeGenSig = GenericSignature())
      : Witness(witness), Kind(kind), WitnessType(witnessType),
        ReqEnv(std::move(env)), OptionalAdjustments(optionalAdjustments.begin(),
                                                    optionalAdjustments.end()),
        DerivativeGenSig(derivativeGenSig) {
    assert(hasWitnessType() == !witnessType.isNull() &&
           "Should (or should not) have witness type");
  }

  RequirementMatch(ValueDecl *witness, MatchKind kind, Requirement requirement,
                   std::optional<RequirementEnvironment> env = std::nullopt,
                   ArrayRef<OptionalAdjustment> optionalAdjustments = {},
                   GenericSignature derivativeGenSig = GenericSignature())
      : Witness(witness), Kind(kind), WitnessType(requirement.getFirstType()),
        MissingRequirement(requirement), ReqEnv(std::move(env)),
        OptionalAdjustments(optionalAdjustments.begin(),
                            optionalAdjustments.end()),
        DerivativeGenSig(derivativeGenSig) {
    assert(hasWitnessType() && hasRequirement() &&
           "Should have witness type and requirement");
  }

  /// The witness that matches the (implied) requirement.
  ValueDecl *Witness;

  /// The kind of match.
  MatchKind Kind;

  /// The type of the witness when it is referenced.
  Type WitnessType;

  /// Requirement not met.
  std::optional<Requirement> MissingRequirement;

  /// Unmet attribute from the requirement.
  const DeclAttribute *UnmetAttribute = nullptr;

  /// The requirement environment to use for the witness thunk.
  std::optional<RequirementEnvironment> ReqEnv;

  /// The set of optional adjustments performed on the witness.
  SmallVector<OptionalAdjustment, 2> OptionalAdjustments;

  /// Substitutions mapping the type of the witness to the requirement
  /// environment.
  SubstitutionMap WitnessSubstitutions;

  /// The matched derivative generic signature.
  GenericSignature DerivativeGenSig;

  /// Determine whether this match is well-formed, meaning that it is any
  /// difference determined by requirement matching is acceptable.
  bool isWellFormed() const {
    switch(Kind) {
    case MatchKind::ExactMatch:
    case MatchKind::FewerEffects:
    case MatchKind::RequiresNonSendable:
      return true;

    case MatchKind::OptionalityConflict:
    case MatchKind::RenamedMatch:
    case MatchKind::WitnessInvalid:
    case MatchKind::Circularity:
    case MatchKind::KindConflict:
    case MatchKind::TypeConflict:
    case MatchKind::MissingRequirement:
    case MatchKind::StaticNonStaticConflict:
    case MatchKind::CompileTimeConstConflict:
    case MatchKind::SettableConflict:
    case MatchKind::PrefixNonPrefixConflict:
    case MatchKind::PostfixNonPostfixConflict:
    case MatchKind::MutatingConflict:
    case MatchKind::NonMutatingConflict:
    case MatchKind::ConsumingConflict:
    case MatchKind::RethrowsConflict:
    case MatchKind::RethrowsByConformanceConflict:
    case MatchKind::AsyncConflict:
    case MatchKind::ThrowsConflict:
    case MatchKind::NonObjC:
    case MatchKind::MissingDifferentiableAttr:
    case MatchKind::EnumCaseWithAssociatedValues:
      return false;
    }

    llvm_unreachable("Unhandled MatchKind in switch.");
  }

  /// Determine whether this match is viable, meaning that we could generate
  /// a witness for it, even though there might be semantic errors.
  bool isViable() const {
    switch(Kind) {
    case MatchKind::ExactMatch:
    case MatchKind::FewerEffects:
    case MatchKind::RequiresNonSendable:
    case MatchKind::OptionalityConflict:
    case MatchKind::RenamedMatch:
      return true;

    case MatchKind::WitnessInvalid:
    case MatchKind::Circularity:
    case MatchKind::KindConflict:
    case MatchKind::TypeConflict:
    case MatchKind::MissingRequirement:
    case MatchKind::StaticNonStaticConflict:
    case MatchKind::CompileTimeConstConflict:
    case MatchKind::SettableConflict:
    case MatchKind::PrefixNonPrefixConflict:
    case MatchKind::PostfixNonPostfixConflict:
    case MatchKind::MutatingConflict:
    case MatchKind::NonMutatingConflict:
    case MatchKind::ConsumingConflict:
    case MatchKind::RethrowsConflict:
    case MatchKind::RethrowsByConformanceConflict:
    case MatchKind::AsyncConflict:
    case MatchKind::ThrowsConflict:
    case MatchKind::NonObjC:
    case MatchKind::MissingDifferentiableAttr:
    case MatchKind::EnumCaseWithAssociatedValues:
      return false;
    }

    llvm_unreachable("Unhandled MatchKind in switch.");
  }

  /// Determine whether this requirement match has a witness type.
  bool hasWitnessType() const {
    switch(Kind) {
    case MatchKind::ExactMatch:
    case MatchKind::FewerEffects:
    case MatchKind::RequiresNonSendable:
    case MatchKind::RenamedMatch:
    case MatchKind::TypeConflict:
    case MatchKind::MissingRequirement:
    case MatchKind::OptionalityConflict:
      return true;

    case MatchKind::WitnessInvalid:
    case MatchKind::Circularity:
    case MatchKind::KindConflict:
    case MatchKind::StaticNonStaticConflict:
    case MatchKind::CompileTimeConstConflict:
    case MatchKind::SettableConflict:
    case MatchKind::PrefixNonPrefixConflict:
    case MatchKind::PostfixNonPostfixConflict:
    case MatchKind::MutatingConflict:
    case MatchKind::NonMutatingConflict:
    case MatchKind::ConsumingConflict:
    case MatchKind::RethrowsConflict:
    case MatchKind::RethrowsByConformanceConflict:
    case MatchKind::AsyncConflict:
    case MatchKind::ThrowsConflict:
    case MatchKind::NonObjC:
    case MatchKind::MissingDifferentiableAttr:
    case MatchKind::EnumCaseWithAssociatedValues:
      return false;
    }

    llvm_unreachable("Unhandled MatchKind in switch.");
  }

  /// Determine whether this requirement match has a requirement.
  bool hasRequirement() { return Kind == MatchKind::MissingRequirement; }

  /// Determine whether this requirement match has an unmet attribute.
  bool hasUnmetAttribute() {
    return Kind == MatchKind::MissingDifferentiableAttr;
  }

  swift::Witness getWitness(ASTContext &ctx) const {
    return swift::Witness(this->Witness, WitnessSubstitutions,
                          ReqEnv->getWitnessThunkSignature(),
                          ReqEnv->getRequirementToWitnessThunkSubs(),
                          DerivativeGenSig, std::nullopt);
  }
};

}

#endif // SWIFT_AST_REQUIREMENTMATCH_H
