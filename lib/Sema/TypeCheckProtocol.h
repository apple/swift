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
#ifndef SWIFT_SEMA_PROTOCOL_H
#define SWIFT_SEMA_PROTOCOL_H

#include "TypeChecker.h"
#include "swift/AST/AccessScope.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/RequirementEnvironment.h"
#include "swift/AST/RequirementMatch.h"
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

class AssociatedTypeDecl;
class AvailabilityContext;
class DeclContext;
class FuncDecl;
class NormalProtocolConformance;
class ProtocolDecl;
class TypeRepr;
class ValueDecl;

class RequirementEnvironment;

/// Gather the value witnesses for the given requirement.
///
/// \param DC A nominal type or extension context where the conformance
/// was declared.
/// \param req A member of a protocol that DC conforms to.
/// \param ignoringNames If non-null and there are no value
/// witnesses with the correct full name, the results will reflect
/// lookup for just the base name and the pointee will be set to
/// \c true.
SmallVector<ValueDecl *, 4> lookupValueWitnesses(DeclContext *DC,
                                                 ValueDecl *req,
                                                 bool *ignoringNames);

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

  RequirementEnvironmentCache ReqEnvironmentCache;

  WitnessChecker(ASTContext &ctx, ProtocolDecl *proto, Type adoptee,
                 DeclContext *dc);

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

/// The protocol conformance checker.
///
/// This helper class handles most of the details of checking whether a
/// given type (\c Adoptee) conforms to a protocol (\c Proto).
class ConformanceChecker : public WitnessChecker {
  /// Whether we already suggested adding `@preconcurrency`.
  bool suggestedPreconcurrency = false;

public:
  NormalProtocolConformance *Conformance;
  SourceLoc Loc;

  /// Record a (non-type) witness for the given requirement.
  void recordWitness(ValueDecl *requirement, const RequirementMatch &match);

  /// Record that the given optional requirement has no witness.
  void recordOptionalWitness(ValueDecl *requirement);

  /// Record that the given requirement has no valid witness.
  void recordInvalidWitness(ValueDecl *requirement);

  /// Check that the witness and requirement have compatible actor contexts.
  ///
  /// \param usesPreconcurrency Will be set true if the conformance is
  /// @preconcurrency and we made use of that fact.
  ///
  /// \returns the isolation that needs to be enforced to invoke the witness
  /// from the requirement, used when entering an actor-isolated synchronous
  /// witness from an asynchronous requirement.
  std::optional<ActorIsolation> checkActorIsolation(ValueDecl *requirement,
                                                    ValueDecl *witness,
                                                    bool &usesPreconcurrency);

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

  ConformanceChecker(ASTContext &ctx, NormalProtocolConformance *conformance);

  ~ConformanceChecker();

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
};

/// Match the given witness to the given requirement.
///
/// \returns the result of performing the match.
RequirementMatch matchWitness(
    DeclContext *dc, ValueDecl *req, ValueDecl *witness,
    llvm::function_ref<
        std::tuple<std::optional<RequirementMatch>, Type, Type>(void)>
        setup,
    llvm::function_ref<std::optional<RequirementMatch>(Type, Type)> matchTypes,
    llvm::function_ref<RequirementMatch(bool, ArrayRef<OptionalAdjustment>)>
        finalize);

RequirementMatch
matchWitness(WitnessChecker::RequirementEnvironmentCache &reqEnvCache,
             ProtocolDecl *proto, RootProtocolConformance *conformance,
             DeclContext *dc, ValueDecl *req, ValueDecl *witness);

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

/// Find an associated type declaration that provides a default definition.
AssociatedTypeDecl *findDefaultedAssociatedType(
    DeclContext *dc, NominalTypeDecl *adoptee,
    AssociatedTypeDecl *assocType);

/// Determine whether this witness has an `@_implements` attribute whose
/// name matches that of the given requirement.
bool witnessHasImplementsAttrForRequiredName(ValueDecl *witness,
                                             ValueDecl *requirement);

/// Determine whether this witness has an `@_implements` attribute whose name
/// and protocol match that of the requirement exactly.
bool witnessHasImplementsAttrForExactRequirement(ValueDecl *witness,
                                                 ValueDecl *requirement);

}

#endif // SWIFT_SEMA_PROTOCOL_H
