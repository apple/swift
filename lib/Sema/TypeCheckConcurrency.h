//===--- TypeCheckConcurrency.h - Concurrency -------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file provides type checking support for Swift's concurrency model.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SEMA_TYPECHECKCONCURRENCY_H
#define SWIFT_SEMA_TYPECHECKCONCURRENCY_H

#include "swift/AST/ConcreteDeclRef.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/Type.h"
#include <cassert>

namespace swift {

class AbstractFunctionDecl;
class ActorIsolation;
class AnyFunctionType;
class ASTContext;
class ClassDecl;
class ClosureActorIsolation;
class ClosureExpr;
class ConcreteDeclRef;
class CustomAttr;
class Decl;
class DeclContext;
class EnumElementDecl;
class Expr;
class FuncDecl;
class Initializer;
class PatternBindingDecl;
class ProtocolConformance;
class TopLevelCodeDecl;
class TypeBase;
class ValueDecl;

/// Add notes suggesting the addition of 'async' or '@asyncHandler', as
/// appropriate, to a diagnostic for a function that isn't an async context.
void addAsyncNotes(AbstractFunctionDecl const* func);

/// Check actor isolation rules.
void checkTopLevelActorIsolation(TopLevelCodeDecl *decl);
void checkFunctionActorIsolation(AbstractFunctionDecl *decl);
void checkInitializerActorIsolation(Initializer *init, Expr *expr);
void checkEnumElementActorIsolation(EnumElementDecl *element, Expr *expr);
void checkPropertyWrapperActorIsolation(
    PatternBindingDecl *binding, Expr *expr);

/// Determine the isolation of a particular closure.
///
/// This forwards to \c ActorIsolationChecker::determineClosureActorIsolation
/// and thus assumes that enclosing closures have already had their isolation
/// checked.
///
/// This does not set the closure's actor isolation
ClosureActorIsolation
determineClosureActorIsolation(AbstractClosureExpr *closure);

/// Describes the kind of operation that introduced the concurrent refernece.
enum class ConcurrentReferenceKind {
  /// A synchronous operation that was "promoted" to an asynchronous call
  /// because it was out of the actor's domain.
  SynchronousAsAsyncCall,
  /// A cross-actor reference.
  CrossActor,
  /// A local capture referenced from concurrent code.
  LocalCapture,
  /// Concurrent function
  ConcurrentFunction,
  /// Nonisolated declaration.
  Nonisolated,
};

/// The isolation restriction in effect for a given declaration that is
/// referenced from source.
class ActorIsolationRestriction {
public:
  enum Kind {
    /// There is no restriction on references to the given declaration.
    Unrestricted,

    /// Access to the declaration is unsafe in any concurrent context.
    Unsafe,

    /// References to this entity are allowed from anywhere, but doing so
    /// may cross an actor boundary if it is not from within the same actor's
    /// isolation domain.
    CrossActorSelf,

    /// References to this member of an actor are only permitted from within
    /// the actor's isolation domain.
    ActorSelf,

    /// References to a declaration that is part of a global actor are
    /// permitted from other declarations with that same global actor or
    /// are permitted from elsewhere as a cross-actor reference.
    GlobalActor,

    /// References to a declaration that is part of a global actor are
    /// permitted from other declarations with that same global actor or
    /// are permitted from elsewhere as a cross-actor reference, but
    /// contexts with unspecified isolation won't diagnose anything.
    GlobalActorUnsafe,
  };

private:
  union {
    /// The local context that an entity is tied to.
    DeclContext *localContext;

    /// The actor that the entity is declared in.
    NominalTypeDecl *actorType;

    /// The global actor type.
    TypeBase *globalActor;
  } data;

  explicit ActorIsolationRestriction(Kind kind, bool isCrossActor)
      : kind(kind), isCrossActor(isCrossActor) { }

public:
  /// The kind of restriction.
  const Kind kind;

  /// Whether referencing this from another actor constitutes a cross-acter
  /// reference.
  const bool isCrossActor;

  Kind getKind() const { return kind; }

  /// Retrieve the actor type that the declaration is within.
  NominalTypeDecl *getActorType() const {
    assert(kind == ActorSelf || kind == CrossActorSelf);
    return data.actorType;
  }

  /// Retrieve the actor that the declaration is within.
  Type getGlobalActor() const {
    assert(kind == GlobalActor || kind == GlobalActorUnsafe);
    return Type(data.globalActor);
  }

  /// There are no restrictions on the use of the entity.
  static ActorIsolationRestriction forUnrestricted() {
    return ActorIsolationRestriction(Unrestricted, /*isCrossActor=*/false);
  }

  /// Accesses to the given declaration are unsafe.
  static ActorIsolationRestriction forUnsafe() {
    return ActorIsolationRestriction(Unsafe, /*isCrossActor=*/false);
  }

  /// Accesses to the given declaration can only be made via the 'self' of
  /// the current actor or is a cross-actor access.
  static ActorIsolationRestriction forActorSelf(
      NominalTypeDecl *actor, bool isCrossActor) {
    ActorIsolationRestriction result(isCrossActor? CrossActorSelf : ActorSelf,
                                     isCrossActor);
    result.data.actorType = actor;
    return result;
  }

  /// Accesses to the given declaration can only be made via this particular
  /// global actor or is a cross-actor access.
  static ActorIsolationRestriction forGlobalActor(
      Type globalActor, bool isCrossActor, bool isUnsafe) {
    ActorIsolationRestriction result(
        isUnsafe ? GlobalActorUnsafe : GlobalActor, isCrossActor);
    result.data.globalActor = globalActor.getPointer();
    return result;
  }

  /// Determine the isolation rules for a given declaration.
  ///
  /// \param fromExpression Indicates that the reference is coming from an
  /// expression.
  static ActorIsolationRestriction forDeclaration(
      ConcreteDeclRef declRef, const DeclContext *fromDC,
      bool fromExpression = true);

  operator Kind() const { return kind; };
};

/// Check that the actor isolation of an override matches that of its
/// overridden declaration.
void checkOverrideActorIsolation(ValueDecl *value);

/// Determine whether the given context uses concurrency features, such
/// as async functions or actors.
bool contextUsesConcurrencyFeatures(const DeclContext *dc);

/// Diagnose the presence of any non-concurrent types when referencing a
/// given declaration from a particular declaration context.
///
/// This function should be invoked any time that the given declaration
/// reference is will move values of the declaration's types across a
/// concurrency domain, whether in/out of an actor or in/or of a concurrent
/// function or closure.
///
/// \param declRef The declaration being referenced from another concurrency
/// domain, including the substitutions so that (e.g.) we can consider the
/// specific types at the use site.
///
/// \param dc The declaration context from which the reference occurs. This is
/// used to perform lookup of conformances to the \c Sendable protocol.
///
/// \param loc The location at which the reference occurs, which will be
/// used when emitting diagnostics.
///
/// \param refKind Describes what kind of reference is being made, which is
/// used to tailor the diagnostic.
///
/// \returns true if an problem was detected, false otherwise.
bool diagnoseNonConcurrentTypesInReference(
    ConcreteDeclRef declRef, const DeclContext *dc, SourceLoc loc,
    ConcurrentReferenceKind refKind,
    DiagnosticBehavior behavior = DiagnosticBehavior::Unspecified);

/// How the concurrent value check should be performed.
enum class SendableCheck {
  /// Sendable conformance was explicitly stated and should be
  /// fully checked.
  Explicit,

  /// Sendable conformance was implied by one of the standard library
  /// protocols that added Sendable after-the-fact.
  ImpliedByStandardProtocol,

  /// Implicit conformance to Sendable.
  Implicit,
};

/// Given a set of custom attributes, pick out the global actor attributes
/// and perform any necessary resolution and diagnostics, returning the
/// global actor attribute and type it refers to (or \c None).
Optional<std::pair<CustomAttr *, NominalTypeDecl *>>
checkGlobalActorAttributes(
    SourceLoc loc, DeclContext *dc, ArrayRef<CustomAttr *> attrs);

/// Get the explicit global actor specified for a closure.
Type getExplicitGlobalActor(ClosureExpr *closure);

/// Check the correctness of the given Sendable conformance.
///
/// \returns true if an error occurred.
bool checkSendableConformance(
    ProtocolConformance *conformance, SendableCheck check);
} // end namespace swift

#endif /* SWIFT_SEMA_TYPECHECKCONCURRENCY_H */
