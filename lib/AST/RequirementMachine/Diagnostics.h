//===--- Diagnostics.h - Requirement machine diagnostics --------*- C++ -*-===//
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

#ifndef SWIFT_REQUIREMENT_DIAGNOSTICS_H
#define SWIFT_REQUIREMENT_DIAGNOSTICS_H

#include "swift/AST/ASTContext.h"
#include "swift/AST/Requirement.h"
#include "swift/AST/Type.h"

namespace swift {

namespace rewriting {

/// Represents an invalid requirement, such as `T: Int`.
///
/// Invalid requirements are recorded while computing the
/// generic signature of a declaration, and diagnosed via
/// \c diagnoseRequirementErrors .
struct RequirementError {
  /// The kind of requirement error.
  enum class Kind {
    /// A constraint to a non-protocol, non-class type, e.g. T: Int.
    InvalidTypeRequirement,
    /// A type requirement on a trivially invalid subject type,
    /// e.g. Bool: Collection.
    InvalidRequirementSubject,
    /// An invalid shape requirement, e.g. T.shape == Int.shape
    InvalidShapeRequirement,
    /// A pair of conflicting requirements, T == Int, T == String
    ConflictingRequirement,
    /// A recursive requirement, e.g. T == G<T.A>.
    RecursiveRequirement,
    /// A redundant requirement, e.g. T == T.
    RedundantRequirement,
    /// A not-yet-supported same-element requirement, e.g. each T == Int.
    UnsupportedSameElement,
  } kind;

  /// The invalid requirement.
  Requirement requirement;

  /// A requirement that conflicts with \c requirement. Both
  /// requirements will have the same subject type.
  llvm::Optional<Requirement> conflictingRequirement;

  SourceLoc loc;

private:
  RequirementError(Kind kind, Requirement requirement, SourceLoc loc)
      : kind(kind), requirement(requirement),
        conflictingRequirement(llvm::None), loc(loc) {}

  RequirementError(Kind kind, Requirement requirement,
                   Requirement conflict,
                   SourceLoc loc)
    : kind(kind), requirement(requirement), conflictingRequirement(conflict), loc(loc) {}

public:
  static RequirementError forInvalidTypeRequirement(Type subjectType,
                                                    Type constraint,
                                                    SourceLoc loc) {
    Requirement requirement(RequirementKind::Conformance, subjectType, constraint);
    return {Kind::InvalidTypeRequirement, requirement, loc};
  }

  static RequirementError forInvalidRequirementSubject(Requirement req,
                                                       SourceLoc loc) {
    return {Kind::InvalidRequirementSubject, req, loc};
  }

  static RequirementError forInvalidShapeRequirement(Requirement req,
                                                     SourceLoc loc) {
    return {Kind::InvalidShapeRequirement, req, loc};
  }

  static RequirementError forConflictingRequirement(Requirement req,
                                                    SourceLoc loc) {
    return {Kind::ConflictingRequirement, req, loc};
  }

  static RequirementError forConflictingRequirement(Requirement first,
                                                    Requirement second,
                                                    SourceLoc loc) {
    return {Kind::ConflictingRequirement, first, second, loc};
  }

  static RequirementError forRedundantRequirement(Requirement req,
                                                  SourceLoc loc) {
    return {Kind::RedundantRequirement, req, loc};
  }

  static RequirementError forRecursiveRequirement(Requirement req,
                                                  SourceLoc loc) {
    return {Kind::RecursiveRequirement, req, loc};
  }

  static RequirementError forSameElement(Requirement req, SourceLoc loc) {
    return {Kind::UnsupportedSameElement, req, loc};
  }
};

/// Policy for the fixit that transforms 'T : S' where 'S' is not a protocol
/// or a class into 'T == S'.
enum AllowConcreteTypePolicy {
  /// Any type parameter can be concrete.
  All,

  /// Only associated types can be concrete.
  AssocTypes,

  /// Only nested associated types can be concrete. This is for protocols,
  /// where we don't want to suggest making an associated type member of
  /// 'Self' concrete.
  NestedAssocTypes
};

bool diagnoseRequirementErrors(ASTContext &ctx,
                               ArrayRef<RequirementError> errors,
                               AllowConcreteTypePolicy concreteTypePolicy);

} // end namespace rewriting

} // end namespace swift

#endif
