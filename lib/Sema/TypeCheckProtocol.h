//===--- TypeCheckProtocol.h - Constraint-based Type Checking ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file provides the constraint-based type checker, anchored by the
// \c ConstraintSystem class, which provides type checking and type
// inference for expressions.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_SEMA_PROTOCOL_H
#define SWIFT_SEMA_PROTOCOL_H

#include "TypeChecker.h"
#include "swift/AST/AccessScope.h"
#include "swift/AST/RequirementEnvironment.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "swift/AST/Witness.h"
#include "swift/Basic/Debug.h"
#include "swift/Sema/ConstraintSystem.h"
#include "llvm/ADT/ScopedHashTable.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

class AccessScope;
class AssociatedTypeDecl;
class AvailabilityContext;
class DeclContext;
class FuncDecl;
class NormalProtocolConformance;
class ProtocolDecl;
class TypeRepr;
class ValueDecl;

/// A conflict between two inferred type witnesses for the same
/// associated type.
struct TypeWitnessConflict {
  /// The associated type.
  AssociatedTypeDecl *AssocType;

  /// The first type.
  Type FirstType;

  /// The requirement to which the first witness was matched.
  ValueDecl *FirstRequirement;

  /// The witness from which the first type witness was inferred.
  ValueDecl *FirstWitness;

  /// The second type.
  Type SecondType;

  /// The requirement to which the second witness was matched.
  ValueDecl *SecondRequirement;

  /// The witness from which the second type witness was inferred.
  ValueDecl *SecondWitness;
};

/// Describes the result of checking a type witness.
///
/// This class evaluates true if an error occurred.
class CheckTypeWitnessResult {
  Type Requirement;

public:
  CheckTypeWitnessResult() { }
  CheckTypeWitnessResult(Type reqt) : Requirement(reqt) {}

  Type getRequirement() const { return Requirement; }
  bool isConformanceRequirement() const {
    return Requirement->isExistentialType();
  }
  bool isSuperclassRequirement() const {
    return !isConformanceRequirement();
  }
  bool isError() const {
    return Requirement->is<ErrorType>();
  }
  explicit operator bool() const { return !Requirement.isNull(); }
};

/// Check whether the given type witness can be used for the given
/// associated type in the given conformance.
///
/// \returns an empty result on success, or a description of the error.
CheckTypeWitnessResult checkTypeWitness(Type type,
                                        AssociatedTypeDecl *assocType,
                                        const NormalProtocolConformance *Conf,
                                        SubstOptions options = llvm::None);

/// A type witness inferred without the aid of a specific potential
/// value witness.
class AbstractTypeWitness {
  AssociatedTypeDecl *AssocType;
  Type TheType;

  /// The defaulted associated type that was used to infer this type witness.
  /// Need not necessarily match \c AssocType, but their names must.
  AssociatedTypeDecl *DefaultedAssocType;

public:
  AbstractTypeWitness(AssociatedTypeDecl *AssocType, Type TheType,
                      AssociatedTypeDecl *DefaultedAssocType = nullptr)
      : AssocType(AssocType), TheType(TheType),
        DefaultedAssocType(DefaultedAssocType) {
    assert(AssocType && TheType);
    assert(!DefaultedAssocType ||
           (AssocType->getName() == DefaultedAssocType->getName()));
  }

  AssociatedTypeDecl *getAssocType() const { return AssocType; }

  Type getType() const { return TheType; }

  AssociatedTypeDecl *getDefaultedAssocType() const {
    return DefaultedAssocType;
  }
};

/// The set of associated types that have been inferred by matching
/// the given value witness to its corresponding requirement.
struct InferredAssociatedTypesByWitness {
  /// The witness we matched.
  ValueDecl *Witness = nullptr;

  /// The associated types inferred from matching this witness.
  SmallVector<std::pair<AssociatedTypeDecl *, Type>, 4> Inferred;

  /// Inferred associated types that don't meet the associated type
  /// requirements.
  SmallVector<std::tuple<AssociatedTypeDecl *, Type, CheckTypeWitnessResult>,
              2> NonViable;

  void dump(llvm::raw_ostream &out, unsigned indent) const;

  SWIFT_DEBUG_DUMP;
};

/// The set of witnesses that were considered when attempting to
/// infer associated types.
using InferredAssociatedTypesByWitnesses =
    SmallVector<InferredAssociatedTypesByWitness, 2>;

/// A mapping from requirements to the set of matches with witnesses.
using InferredAssociatedTypes =
    SmallVector<std::pair<ValueDecl *, InferredAssociatedTypesByWitnesses>, 4>;

/// A potential solution to the set of inferred type witnesses.
struct InferredTypeWitnessesSolution {
  /// The set of type witnesses inferred by this solution, along
  /// with the index into the value witnesses where the type was
  /// inferred.
  llvm::SmallDenseMap<AssociatedTypeDecl *, std::pair<Type, unsigned>, 4>
    TypeWitnesses;

  /// The value witnesses selected by this step of the solution.
  SmallVector<std::pair<ValueDecl *, ValueDecl *>, 4> ValueWitnesses;

  /// The number of value witnesses that occur in protocol
  /// extensions.
  unsigned NumValueWitnessesInProtocolExtensions;

#ifndef NDEBUG
  LLVM_ATTRIBUTE_USED
#endif
  void dump() const;
};

class RequirementEnvironment;

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

/// Describes the kind of optional adjustment performed when
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

/// Describes a match between a requirement and a witness.
struct RequirementMatch {
  RequirementMatch(ValueDecl *witness, MatchKind kind,
                   llvm::Optional<RequirementEnvironment> env = llvm::None)
      : Witness(witness), Kind(kind), WitnessType(), ReqEnv(std::move(env)) {
    assert(!hasWitnessType() && "Should have witness type");
  }

  RequirementMatch(ValueDecl *witness, MatchKind kind,
                   const DeclAttribute *attr)
      : Witness(witness), Kind(kind), WitnessType(), UnmetAttribute(attr),
        ReqEnv(llvm::None) {
    assert(!hasWitnessType() && "Should have witness type");
    assert(hasUnmetAttribute() && "Should have unmet attribute");
  }

  RequirementMatch(ValueDecl *witness, MatchKind kind, Type witnessType,
                   llvm::Optional<RequirementEnvironment> env = llvm::None,
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
                   llvm::Optional<RequirementEnvironment> env = llvm::None,
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
  llvm::Optional<Requirement> MissingRequirement;

  /// Unmet attribute from the requirement.
  const DeclAttribute *UnmetAttribute = nullptr;

  /// The requirement environment to use for the witness thunk.
  llvm::Optional<RequirementEnvironment> ReqEnv;

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

  swift::Witness getWitness(ASTContext &ctx) const;
};

struct RequirementCheck;

class WitnessChecker {
public:
  using RequirementEnvironmentCacheKey =
      std::pair<const GenericSignatureImpl *, const ClassDecl *>;
  using RequirementEnvironmentCache =
      llvm::DenseMap<RequirementEnvironmentCacheKey, RequirementEnvironment>;

protected:
  ASTContext &Context;
  ProtocolDecl *Proto;
  Type Adoptee;
  // The conforming context, either a nominal type or extension.
  DeclContext *DC;

  ASTContext &getASTContext() const { return Context; }

  // An auxiliary lookup table to be used for witnesses remapped via
  // @_implements(Protocol, DeclName)
  llvm::DenseMap<DeclName, llvm::TinyPtrVector<ValueDecl *>> ImplementsTable;

  RequirementEnvironmentCache ReqEnvironmentCache;

  llvm::Optional<std::pair<AccessScope, bool>>
      RequiredAccessScopeAndUsableFromInline;

  WitnessChecker(ASTContext &ctx, ProtocolDecl *proto, Type adoptee,
                 DeclContext *dc);

  bool isMemberOperator(FuncDecl *decl, Type type);

  AccessScope getRequiredAccessScope();

  bool isUsableFromInlineRequired() {
    assert(RequiredAccessScopeAndUsableFromInline.has_value() &&
           "must check access first using getRequiredAccessScope");
    return RequiredAccessScopeAndUsableFromInline.value().second;
  }

  /// Gather the value witnesses for the given requirement.
  ///
  /// \param ignoringNames If non-null and there are no value
  /// witnesses with the correct full name, the results will reflect
  /// lookup for just the base name and the pointee will be set to
  /// \c true.
  SmallVector<ValueDecl *, 4> lookupValueWitnesses(ValueDecl *req,
                                                   bool *ignoringNames);

  void lookupValueWitnessesViaImplementsAttr(ValueDecl *req,
                                             SmallVector<ValueDecl *, 4>
                                             &witnesses);

  bool findBestWitness(ValueDecl *requirement,
                       bool *ignoringNames,
                       NormalProtocolConformance *conformance,
                       SmallVectorImpl<RequirementMatch> &matches,
                       unsigned &numViable,
                       unsigned &bestIdx,
                       bool &doNotDiagnoseMatches);

  bool checkWitnessAccess(ValueDecl *requirement,
                          ValueDecl *witness,
                          bool *isSetter);

  bool checkWitnessAvailability(ValueDecl *requirement,
                                ValueDecl *witness,
                                AvailabilityContext *requirementInfo);

  RequirementCheck checkWitness(ValueDecl *requirement,
                                const RequirementMatch &match);
};

/// The result of attempting to resolve a witness.
enum class ResolveWitnessResult {
  /// The resolution succeeded.
  Success,
  /// There was an explicit witness available, but it failed some
  /// criteria.
  ExplicitFailed,
  /// There was no witness available.
  Missing
};

enum class MissingWitnessDiagnosisKind {
  FixItOnly,
  ErrorOnly,
  ErrorFixIt,
};

class AssociatedTypeInference;
class MultiConformanceChecker;

/// Describes a missing witness during conformance checking.
class MissingWitness {
public:
  /// The requirement that is missing a witness.
  ValueDecl *requirement;

  /// The set of potential matching witnesses.
  std::vector<RequirementMatch> matches;

  MissingWitness(ValueDecl *requirement,
                 ArrayRef<RequirementMatch> matches)
    : requirement(requirement),
      matches(matches.begin(), matches.end()) { }
};

/// Capture missing witnesses that have been delayed and will be stored
/// in the ASTContext for later.
class DelayedMissingWitnesses : public MissingWitnessesBase {
public:
  std::vector<MissingWitness> missingWitnesses;

  DelayedMissingWitnesses(ArrayRef<MissingWitness> missingWitnesses)
      : missingWitnesses(missingWitnesses.begin(), missingWitnesses.end()) { }
};

/// The protocol conformance checker.
///
/// This helper class handles most of the details of checking whether a
/// given type (\c Adoptee) conforms to a protocol (\c Proto).
class ConformanceChecker : public WitnessChecker {
public:
  /// Key that can be used to uniquely identify a particular Objective-C
  /// method.
  using ObjCMethodKey = std::pair<ObjCSelector, char>;

private:
  friend class MultiConformanceChecker;
  friend class AssociatedTypeInference;

  NormalProtocolConformance *Conformance;
  SourceLoc Loc;

  /// Witnesses that are currently being resolved.
  llvm::SmallPtrSet<ValueDecl *, 4> ResolvingWitnesses;

  /// Caches the set of associated types that are referenced in each
  /// requirement.
  llvm::DenseMap<ValueDecl *, llvm::SmallVector<AssociatedTypeDecl *, 2>>
    ReferencedAssociatedTypes;

  /// Keep track of missing witnesses, either type or value, for later
  /// diagnosis emits. This may contain witnesses that are external to the
  /// protocol under checking.
  llvm::SetVector<MissingWitness> &GlobalMissingWitnesses;

  /// Keep track of the slice in GlobalMissingWitnesses that is local to
  /// this protocol under checking.
  unsigned LocalMissingWitnessesStartIndex;

  /// True if we shouldn't complain about problems with this conformance
  /// right now, i.e. if methods are being called outside
  /// checkConformance().
  bool SuppressDiagnostics;

  /// Whether we've already complained about problems with this conformance.
  bool AlreadyComplained = false;

  /// Mapping from Objective-C methods to the set of requirements within this
  /// protocol that have the same selector and instance/class designation.
  llvm::SmallDenseMap<ObjCMethodKey, TinyPtrVector<AbstractFunctionDecl *>, 4>
    objcMethodRequirements;

  /// Whether objcMethodRequirements has been computed.
  bool computedObjCMethodRequirements = false;

  /// Retrieve the associated types that are referenced by the given
  /// requirement with a base of 'Self'.
  ArrayRef<AssociatedTypeDecl *> getReferencedAssociatedTypes(ValueDecl *req);

  /// Record a (non-type) witness for the given requirement.
  void recordWitness(ValueDecl *requirement, const RequirementMatch &match);

  /// Record that the given optional requirement has no witness.
  void recordOptionalWitness(ValueDecl *requirement);

  /// Record that the given requirement has no valid witness.
  void recordInvalidWitness(ValueDecl *requirement);

  /// Check for ill-formed uses of Objective-C generics in a type witness.
  bool checkObjCTypeErasedGenerics(AssociatedTypeDecl *assocType,
                                   Type type,
                                   TypeDecl *typeDecl);

  /// Check that the witness and requirement have compatible actor contexts.
  ///
  /// \returns the isolation that needs to be enforced to invoke the witness
  /// from the requirement, used when entering an actor-isolated synchronous
  /// witness from an asynchronous requirement.
  llvm::Optional<ActorIsolation> checkActorIsolation(ValueDecl *requirement,
                                                     ValueDecl *witness);

  /// Record a type witness.
  ///
  /// \param assocType The associated type whose witness is being recorded.
  ///
  /// \param type The witness type.
  ///
  /// \param typeDecl The decl the witness type came from; can be null.
  void recordTypeWitness(AssociatedTypeDecl *assocType, Type type,
                         TypeDecl *typeDecl);

  /// Enforce restrictions on non-final classes witnessing requirements
  /// involving the protocol 'Self' type.
  void checkNonFinalClassWitness(ValueDecl *requirement,
                                 ValueDecl *witness);

  /// Resolve a (non-type) witness via name lookup.
  ResolveWitnessResult resolveWitnessViaLookup(ValueDecl *requirement);

  /// Resolve a (non-type) witness via derivation.
  ResolveWitnessResult resolveWitnessViaDerivation(ValueDecl *requirement);

  /// Resolve a (non-type) witness via default definition or optional.
  ResolveWitnessResult resolveWitnessViaDefault(ValueDecl *requirement);

  /// Resolve a (non-type) witness by trying each standard strategy until one
  /// of them produces a result.
  ResolveWitnessResult
  resolveWitnessTryingAllStrategies(ValueDecl *requirement);

  /// Attempt to resolve a type witness via member name lookup.
  ResolveWitnessResult resolveTypeWitnessViaLookup(
                         AssociatedTypeDecl *assocType);

  /// Check whether all of the protocol's generic requirements are satisfied by
  /// the chosen type witnesses.
  void ensureRequirementsAreSatisfied();

  /// Diagnose or defer a diagnostic, as appropriate.
  ///
  /// \param requirement The requirement with which this diagnostic is
  /// associated, if any.
  ///
  /// \param isError Whether this diagnostic is an error.
  ///
  /// \param fn A function to call to emit the actual diagnostic. If
  /// diagnostics are being deferred,
  void diagnoseOrDefer(const ValueDecl *requirement, bool isError,
                       std::function<void(NormalProtocolConformance *)> fn);

  ArrayRef<MissingWitness> getLocalMissingWitness() {
    return GlobalMissingWitnesses.getArrayRef().
      slice(LocalMissingWitnessesStartIndex,
            GlobalMissingWitnesses.size() - LocalMissingWitnessesStartIndex);
  }

  void clearGlobalMissingWitnesses() {
    GlobalMissingWitnesses.clear();
    LocalMissingWitnessesStartIndex = GlobalMissingWitnesses.size();
  }

public:
  /// Call this to diagnose currently known missing witnesses.
  ///
  /// \returns true if any witnesses were diagnosed.
  bool diagnoseMissingWitnesses(MissingWitnessDiagnosisKind Kind);

  /// Emit any diagnostics that have been delayed.
  void emitDelayedDiags();

  ConformanceChecker(ASTContext &ctx, NormalProtocolConformance *conformance,
                     llvm::SetVector<MissingWitness> &GlobalMissingWitnesses,
                     bool suppressDiagnostics = true);

  ~ConformanceChecker();

  /// Resolve all of the type witnesses.
  void resolveTypeWitnesses();

  /// Resolve all of the non-type witnesses.
  void resolveValueWitnesses();

  /// Resolve the witness for the given non-type requirement as
  /// directly as possible, only resolving other witnesses if
  /// needed, e.g., to determine type witnesses used within the
  /// requirement.
  ///
  /// This entry point is designed to be used when the witness for a
  /// particular requirement and adoptee is required, before the
  /// conformance has been completed checked.
  void resolveSingleWitness(ValueDecl *requirement);

  /// Resolve the type witness for the given associated type as
  /// directly as possible.
  void resolveSingleTypeWitness(AssociatedTypeDecl *assocType);

  /// Check the entire protocol conformance, ensuring that all
  /// witnesses are resolved and emitting any diagnostics.
  void checkConformance(MissingWitnessDiagnosisKind Kind);

  /// Retrieve the Objective-C method key from the given function.
  ObjCMethodKey getObjCMethodKey(AbstractFunctionDecl *func);

  /// Retrieve the Objective-C requirements in this protocol that have the
  /// given Objective-C method key.
  ArrayRef<AbstractFunctionDecl *> getObjCRequirements(ObjCMethodKey key);

  /// @returns a non-null requirement if the given requirement is part of a
  /// group of ObjC requirements that share the same ObjC method key.
  /// The first such requirement that the predicate function returns true for
  /// is the requirement required by this function. Otherwise, nullptr is
  /// returned.
  ValueDecl *getObjCRequirementSibling(ValueDecl *requirement,
                    llvm::function_ref<bool(AbstractFunctionDecl *)>predicate);
};

/// A system for recording and probing the integrity of a type witness solution
/// for a set of unresolved associated type declarations.
///
/// Right now can reason only about abstract type witnesses, i.e., same-type
/// constraints, default type definitions, and bindings to generic parameters.
class TypeWitnessSystem final {
  /// Equivalence classes are used on demand to express equivalences between
  /// witness candidates and reflect changes to resolved types across their
  /// members.
  class EquivalenceClass final {
    /// The pointer:
    /// - The resolved type for witness candidates belonging to this equivalence
    ///   class. The resolved type may be a type parameter, but cannot directly
    ///   pertain to a name variable in the owning system; instead, witness
    ///   candidates that should resolve to the same type share an equivalence
    ///   class.
    /// The int:
    /// - A flag indicating whether the resolved type is ambiguous. When set,
    ///   the resolved type is null.
    llvm::PointerIntPair<Type, 1, bool> ResolvedTyAndIsAmbiguous;

  public:
    EquivalenceClass(Type ty) : ResolvedTyAndIsAmbiguous(ty, false) {}

    EquivalenceClass(const EquivalenceClass &) = delete;
    EquivalenceClass(EquivalenceClass &&) = delete;
    EquivalenceClass &operator=(const EquivalenceClass &) = delete;
    EquivalenceClass &operator=(EquivalenceClass &&) = delete;

    Type getResolvedType() const {
      return ResolvedTyAndIsAmbiguous.getPointer();
    }
    void setResolvedType(Type ty);

    bool isAmbiguous() const {
      return ResolvedTyAndIsAmbiguous.getInt();
    }
    void setAmbiguous() {
      ResolvedTyAndIsAmbiguous = {nullptr, true};
    }
  };

  /// A type witness candidate for a name variable.
  struct TypeWitnessCandidate final {
    /// The defaulted associated type declaration correlating with this
    /// candidate, if present.
    AssociatedTypeDecl *DefaultedAssocType;

    /// The equivalence class of this candidate.
    EquivalenceClass *EquivClass;
  };

  /// The set of equivalence classes in the system.
  llvm::SmallPtrSet<EquivalenceClass *, 4> EquivalenceClasses;

  /// The mapping from name variables (the names of unresolved associated
  /// type declarations) to their corresponding type witness candidates.
  llvm::SmallDenseMap<Identifier, TypeWitnessCandidate, 4> TypeWitnesses;

public:
  TypeWitnessSystem(ArrayRef<AssociatedTypeDecl *> assocTypes);
  ~TypeWitnessSystem();

  TypeWitnessSystem(const TypeWitnessSystem &) = delete;
  TypeWitnessSystem(TypeWitnessSystem &&) = delete;
  TypeWitnessSystem &operator=(const TypeWitnessSystem &) = delete;
  TypeWitnessSystem &operator=(TypeWitnessSystem &&) = delete;

  /// Get the resolved type witness for the associated type with the given name.
  Type getResolvedTypeWitness(Identifier name) const;
  bool hasResolvedTypeWitness(Identifier name) const;

  /// Get the defaulted associated type relating to the resolved type witness
  /// for the associated type with the given name, if present.
  AssociatedTypeDecl *getDefaultedAssocType(Identifier name) const;

  /// Record a type witness for the given associated type name.
  ///
  /// \note This need not lead to the resolution of a type witness, e.g.
  /// an associated type may be defaulted to another.
  void addTypeWitness(Identifier name, Type type);

  /// Record a default type witness.
  ///
  /// \param defaultedAssocType The specific associated type declaration that
  /// defines the given default type.
  ///
  /// \note This need not lead to the resolution of a type witness.
  void addDefaultTypeWitness(Type type, AssociatedTypeDecl *defaultedAssocType);

  /// Record the given same-type requirement, if regarded of interest to
  /// the system.
  ///
  /// \note This need not lead to the resolution of a type witness.
  void addSameTypeRequirement(const Requirement &req);

  void dump(llvm::raw_ostream &out,
            const NormalProtocolConformance *conformance) const;

private:
  /// Form an equivalence between the given name variables.
  void addEquivalence(Identifier name1, Identifier name2);

  /// Merge \p equivClass2 into \p equivClass1.
  ///
  /// \note This will delete \p equivClass2 after migrating its members to
  /// \p equivClass1.
  void mergeEquivalenceClasses(EquivalenceClass *equivClass1,
                               const EquivalenceClass *equivClass2);

  /// The result of comparing two resolved types targeting a single equivalence
  /// class, in terms of their relative impact on solving the system.
  enum class ResolvedTypeComparisonResult {
    /// The first resolved type is a better choice than the second one.
    Better,

    /// The first resolved type is an equivalent or worse choice than the
    /// second one.
    EquivalentOrWorse,

    /// Both resolved types are concrete and mutually exclusive.
    Ambiguity
  };

  /// Compare the given resolved types as targeting a single equivalence class,
  /// in terms of the their relative impact on solving the system.
  static ResolvedTypeComparisonResult compareResolvedTypes(Type ty1, Type ty2);
};

/// Captures the state needed to infer associated types.
class AssociatedTypeInference {
  /// The type checker we'll need to validate declarations etc.
  ASTContext &ctx;

  /// The conformance for which we are inferring associated types.
  NormalProtocolConformance *conformance;

  /// The protocol for which we are inferring associated types.
  ProtocolDecl *proto;

  /// The declaration context in which conformance to the protocol is
  /// declared.
  DeclContext *dc;

  /// The type that is adopting the protocol.
  Type adoptee;

  /// The set of type witnesses inferred from value witnesses.
  InferredAssociatedTypes inferred;

  /// Hash table containing the type witnesses that we've inferred for
  /// each associated type, as well as an indication of how we inferred them.
  llvm::ScopedHashTable<AssociatedTypeDecl *, std::pair<Type, unsigned>>
    typeWitnesses;

  /// Information about a failed, defaulted associated type.
  const AssociatedTypeDecl *failedDefaultedAssocType = nullptr;
  Type failedDefaultedWitness;
  CheckTypeWitnessResult failedDefaultedResult;

  /// Information about a failed, derived associated type.
  AssociatedTypeDecl *failedDerivedAssocType = nullptr;
  Type failedDerivedWitness;

  // Which type witness was missing?
  AssociatedTypeDecl *missingTypeWitness = nullptr;

  // Was there a conflict in type witness deduction?
  llvm::Optional<TypeWitnessConflict> typeWitnessConflict;
  unsigned numTypeWitnessesBeforeConflict = 0;

public:
  AssociatedTypeInference(ASTContext &ctx,
                          NormalProtocolConformance *conformance);

private:
  /// Retrieve the AST context.
  ASTContext &getASTContext() const { return ctx; }

  /// Infer associated type witnesses for the given tentative
  /// requirement/witness match.
  InferredAssociatedTypesByWitness inferTypeWitnessesViaValueWitness(
                                     ValueDecl *req,
                                     ValueDecl *witness);

  /// Infer associated type witnesses for the given value requirement.
  InferredAssociatedTypesByWitnesses inferTypeWitnessesViaValueWitnesses(
                   ConformanceChecker &checker,
                   const llvm::SetVector<AssociatedTypeDecl *> &allUnresolved,
                   ValueDecl *req);

  /// Infer associated type witnesses for the given associated type.
  InferredAssociatedTypesByWitnesses inferTypeWitnessesViaAssociatedType(
                   ConformanceChecker &checker,
                   const llvm::SetVector<AssociatedTypeDecl *> &allUnresolved,
                   AssociatedTypeDecl *assocType);

  /// Infer associated type witnesses for all relevant value requirements.
  ///
  /// \param assocTypes The set of associated types we're interested in.
  InferredAssociatedTypes
  inferTypeWitnessesViaValueWitnesses(
    ConformanceChecker &checker,
    const llvm::SetVector<AssociatedTypeDecl *> &assocTypes);

  /// Compute a "fixed" type witness for an associated type, e.g.,
  /// if the refined protocol requires it to be equivalent to some other type.
  Type computeFixedTypeWitness(AssociatedTypeDecl *assocType);

  /// Compute the default type witness from an associated type default,
  /// if there is one.
  llvm::Optional<AbstractTypeWitness>
  computeDefaultTypeWitness(AssociatedTypeDecl *assocType) const;

  /// Compute the "derived" type witness for an associated type that is
  /// known to the compiler.
  std::pair<Type, TypeDecl *>
  computeDerivedTypeWitness(AssociatedTypeDecl *assocType);

  /// Compute a type witness without using a specific potential witness.
  llvm::Optional<AbstractTypeWitness>
  computeAbstractTypeWitness(AssociatedTypeDecl *assocType);

  /// Collect abstract type witnesses and feed them to the given system.
  void collectAbstractTypeWitnesses(
      TypeWitnessSystem &system,
      ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes) const;

  /// Substitute the current type witnesses into the given interface type.
  Type substCurrentTypeWitnesses(Type type);

  /// Retrieve substitution options with a tentative type witness
  /// operation that queries the current set of type witnesses.
  SubstOptions getSubstOptionsWithCurrentTypeWitnesses();

  /// Check whether the current set of type witnesses meets the
  /// requirements of the protocol.
  bool checkCurrentTypeWitnesses(
         const SmallVectorImpl<std::pair<ValueDecl *, ValueDecl *>>
           &valueWitnesses);

  /// Check the current type witnesses against the
  /// requirements of the given constrained extension.
  bool checkConstrainedExtension(ExtensionDecl *ext);

  /// Attempt to infer abstract type witnesses for the given set of associated
  /// types.
  ///
  /// \returns \c nullptr, or the associated type that failed.
  AssociatedTypeDecl *inferAbstractTypeWitnesses(
      ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes, unsigned reqDepth);

  /// Top-level operation to find solutions for the given unresolved
  /// associated types.
  void findSolutions(
                 ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes,
                 SmallVectorImpl<InferredTypeWitnessesSolution> &solutions);

  /// Explore the solution space to find both viable and non-viable solutions.
  void findSolutionsRec(
         ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes,
         SmallVectorImpl<InferredTypeWitnessesSolution> &solutions,
         SmallVectorImpl<InferredTypeWitnessesSolution> &nonViableSolutions,
         SmallVector<std::pair<ValueDecl *, ValueDecl *>, 4> &valueWitnesses,
         unsigned numTypeWitnesses,
         unsigned numValueWitnessesInProtocolExtensions,
         unsigned reqDepth);

  /// Determine whether the first solution is better than the second
  /// solution.
  bool isBetterSolution(const InferredTypeWitnessesSolution &first,
                        const InferredTypeWitnessesSolution &second);

  /// Find the best solution.
  ///
  /// \param solutions All of the solutions to consider. On success,
  /// this will contain only the best solution.
  ///
  /// \returns \c false if there was a single best solution,
  /// \c true if no single best solution exists.
  bool findBestSolution(
                SmallVectorImpl<InferredTypeWitnessesSolution> &solutions);

  /// Emit a diagnostic for the case where there are no solutions at all
  /// to consider.
  ///
  /// \returns true if a diagnostic was emitted, false otherwise.
  bool diagnoseNoSolutions(
                     ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes,
                     ConformanceChecker &checker);

  /// Emit a diagnostic when there are multiple solutions.
  ///
  /// \returns true if a diagnostic was emitted, false otherwise.
  bool diagnoseAmbiguousSolutions(
                ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes,
                ConformanceChecker &checker,
                SmallVectorImpl<InferredTypeWitnessesSolution> &solutions);

  /// We may need to determine a type witness, regardless of the existence of a
  /// default value for it, e.g. when a 'distributed actor' is looking up its
  /// 'ID', the default defined in an extension for 'Identifiable' would be
  /// located using the lookup resolve. This would not be correct, since the
  /// type actually must be based on the associated 'ActorSystem'.
  ///
  /// TODO(distributed): perhaps there is a better way to avoid this mixup?
  ///   Note though that this issue seems to only manifest in "real" builds
  ///   involving multiple files/modules, and not in tests within the Swift
  ///   project itself.
  bool canAttemptEagerTypeWitnessDerivation(
      ConformanceChecker &checker,
      AssociatedTypeDecl *assocType);

public:
  /// Describes a mapping from associated type declarations to their
  /// type witnesses (as interface types).
  using InferredTypeWitnesses =
      std::vector<std::pair<AssociatedTypeDecl *, Type>>;

  /// Perform associated type inference.
  ///
  /// \returns \c true if an error occurred, \c false otherwise
  llvm::Optional<InferredTypeWitnesses> solve(ConformanceChecker &checker);

  /// Find an associated type declaration that provides a default definition.
  static AssociatedTypeDecl *findDefaultedAssociatedType(
                                                 AssociatedTypeDecl *assocType);
};

/// Match the given witness to the given requirement.
///
/// \returns the result of performing the match.
RequirementMatch matchWitness(
    DeclContext *dc, ValueDecl *req, ValueDecl *witness,
    llvm::function_ref<
        std::tuple<llvm::Optional<RequirementMatch>, Type, Type>(void)>
        setup,
    llvm::function_ref<llvm::Optional<RequirementMatch>(Type, Type)> matchTypes,
    llvm::function_ref<RequirementMatch(bool, ArrayRef<OptionalAdjustment>)>
        finalize);

RequirementMatch
matchWitness(WitnessChecker::RequirementEnvironmentCache &reqEnvCache,
             ProtocolDecl *proto, RootProtocolConformance *conformance,
             DeclContext *dc, ValueDecl *req, ValueDecl *witness);

/// If the given type is a direct reference to an associated type of
/// the given protocol, return the referenced associated type.
AssociatedTypeDecl *getReferencedAssocTypeOfProtocol(Type type,
                                                     ProtocolDecl *proto);

enum class TypeAdjustment : uint8_t {
  NoescapeToEscaping, NonsendableToSendable
};

/// Perform any necessary adjustments to the inferred associated type to
/// make it suitable for later use.
///
/// \param performed Will be set \c true if this operation performed
/// the adjustment, or \c false if the operation found a type that the
/// adjustment could have applied to but did not actually need to adjust it.
/// Unchanged otherwise.
Type adjustInferredAssociatedType(TypeAdjustment adjustment, Type type,
                                  bool &performed);

/// Find the @objc requirement that are witnessed by the given
/// declaration.
///
/// \param anySingleRequirement If true, returns at most a single requirement,
/// which might be any of the requirements that match.
///
/// \returns the set of requirements to which the given witness is a
/// witness.
llvm::TinyPtrVector<ValueDecl *> findWitnessedObjCRequirements(
                                     const ValueDecl *witness,
                                     bool anySingleRequirement = false);

void diagnoseConformanceFailure(Type T,
                                ProtocolDecl *Proto,
                                DeclContext *DC,
                                SourceLoc ComplainLoc);

}

namespace llvm {

template<>
struct DenseMapInfo<swift::MissingWitness> {
  using MissingWitness = swift::MissingWitness;
  using RequirementPointerTraits = DenseMapInfo<swift::ValueDecl *>;

  static inline MissingWitness getEmptyKey() {
    return MissingWitness(RequirementPointerTraits::getEmptyKey(), {});
  }
  static inline MissingWitness getTombstoneKey() {
    return MissingWitness(RequirementPointerTraits::getTombstoneKey(), {});
  }
  static inline unsigned getHashValue(MissingWitness missing) {
    return RequirementPointerTraits::getHashValue(missing.requirement);
  }
  static bool isEqual(MissingWitness a, MissingWitness b) {
    return a.requirement == b.requirement;
  }
};

}
#endif // SWIFT_SEMA_PROTOCOL_H
