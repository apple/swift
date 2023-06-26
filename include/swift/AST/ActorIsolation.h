//===--- ActorIsolation.h - Actor isolation ---------------------*- C++ -*-===//
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
// This file provides a description of actor isolation state.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_AST_ACTORISOLATIONSTATE_H
#define SWIFT_AST_ACTORISOLATIONSTATE_H

#include "swift/AST/Type.h"
#include "llvm/ADT/Hashing.h"

namespace llvm {
class raw_ostream;
}

namespace swift {
class DeclContext;
class ModuleDecl;
class VarDecl;
class NominalTypeDecl;
class SubstitutionMap;
class AbstractFunctionDecl;
class AbstractClosureExpr;
class ClosureActorIsolation;

/// Trampoline for AbstractClosureExpr::getActorIsolation.
ClosureActorIsolation
__AbstractClosureExpr_getActorIsolation(AbstractClosureExpr *CE);

/// Returns a function reference to \c __AbstractClosureExpr_getActorIsolation.
/// This is needed so we can use it as a default argument for
/// \c getActorIsolationOfContext without knowing the layout of
/// \c ClosureActorIsolation.
llvm::function_ref<ClosureActorIsolation(AbstractClosureExpr *)>
_getRef__AbstractClosureExpr_getActorIsolation();

/// Determine whether the given types are (canonically) equal, declared here
/// to avoid having to include Types.h.
bool areTypesEqual(Type type1, Type type2);

/// Determine whether the given type is suitable as a concurrent value type.
bool isSendableType(ModuleDecl *module, Type type);

/// Determines if the 'let' can be read from anywhere within the given module,
/// regardless of the isolation or async-ness of the context in which
/// the var is read.
bool isLetAccessibleAnywhere(const ModuleDecl *fromModule, VarDecl *let);

/// Describes the actor isolation of a given declaration, which determines
/// the actors with which it can interact.
class ActorIsolation {
public:
  enum Kind : uint8_t {
    /// The actor isolation has not been specified. It is assumed to be
    /// unsafe to interact with this declaration from any actor.
    Unspecified = 0,
    /// The declaration is isolated to the instance of an actor.
    /// For example, a mutable stored property or synchronous function within
    /// the actor is isolated to the instance of that actor.
    ActorInstance,
    /// The declaration is explicitly specified to be independent of any actor,
    /// meaning that it can be used from any actor but is also unable to
    /// refer to the isolated state of any given actor.
    Independent,
    /// The declaration is isolated to a global actor. It can refer to other
    /// entities with the same global actor.
    GlobalActor,
    /// The declaration is isolated to a global actor but with the "unsafe"
    /// annotation, which means that we only enforce the isolation if we're
    /// coming from something with specific isolation.
    GlobalActorUnsafe,
  };

private:
  union {
    NominalTypeDecl *actor;
    Type globalActor;
    void *pointer;
  };
  unsigned kind : 3;
  unsigned isolatedByPreconcurrency : 1;
  unsigned parameterIndex : 28;

  ActorIsolation(Kind kind, NominalTypeDecl *actor, unsigned parameterIndex)
      : actor(actor), kind(kind), isolatedByPreconcurrency(false),
        parameterIndex(parameterIndex) { }

  ActorIsolation(Kind kind, Type globalActor)
      : globalActor(globalActor), kind(kind), isolatedByPreconcurrency(false),
        parameterIndex(0) { }

public:
  static ActorIsolation forUnspecified() {
    return ActorIsolation(Unspecified, nullptr);
  }

  static ActorIsolation forIndependent() {
    return ActorIsolation(Independent, nullptr);
  }

  static ActorIsolation forActorInstanceSelf(NominalTypeDecl *actor) {
    return ActorIsolation(ActorInstance, actor, 0);
  }

  static ActorIsolation forActorInstanceParameter(NominalTypeDecl *actor,
                                                  unsigned parameterIndex) {
    return ActorIsolation(ActorInstance, actor, parameterIndex + 1);
  }

  static ActorIsolation forGlobalActor(Type globalActor, bool unsafe) {
    return ActorIsolation(
        unsafe ? GlobalActorUnsafe : GlobalActor, globalActor);
  }

  Kind getKind() const { return (Kind)kind; }

  operator Kind() const { return getKind(); }

  bool isUnspecified() const { return kind == Unspecified; }
  
  bool isIndependent() const { return kind == Independent; }

  /// Retrieve the parameter to which actor-instance isolation applies.
  ///
  /// Parameter 0 is `self`.
  unsigned getActorInstanceParameter() const {
    assert(getKind() == ActorInstance);
    return parameterIndex;
  }

  bool isActorIsolated() const {
    switch (getKind()) {
    case ActorInstance:
    case GlobalActor:
    case GlobalActorUnsafe:
      return true;

    case Unspecified:
    case Independent:
      return false;
    }
  }

  NominalTypeDecl *getActor() const {
    assert(getKind() == ActorInstance);
    return actor;
  }

  bool isGlobalActor() const {
    return getKind() == GlobalActor || getKind() == GlobalActorUnsafe;
  }

  bool isMainActor() const;

  bool isDistributedActor() const;

  Type getGlobalActor() const {
    assert(isGlobalActor());
    return globalActor;
  }

  bool preconcurrency() const {
    return isolatedByPreconcurrency;
  }

  ActorIsolation withPreconcurrency(bool value) const {
    auto copy = *this;
    copy.isolatedByPreconcurrency = value;
    return copy;
  }

  /// Determine whether this isolation will require substitution to be
  /// evaluated.
  bool requiresSubstitution() const;

  /// Substitute into types within the actor isolation.
  ActorIsolation subst(SubstitutionMap subs) const;

  friend bool operator==(const ActorIsolation &lhs,
                         const ActorIsolation &rhs) {
    if (lhs.isGlobalActor() && rhs.isGlobalActor())
      return areTypesEqual(lhs.globalActor, rhs.globalActor);

    if (lhs.getKind() != rhs.getKind())
      return false;

    switch (lhs.getKind()) {
    case Independent:
    case Unspecified:
      return true;

    case ActorInstance:
      return lhs.actor == rhs.actor && lhs.parameterIndex == rhs.parameterIndex;

    case GlobalActor:
    case GlobalActorUnsafe:
      llvm_unreachable("Global actors handled above");
    }
  }

  friend bool operator!=(const ActorIsolation &lhs,
                         const ActorIsolation &rhs) {
    return !(lhs == rhs);
  }

  friend llvm::hash_code hash_value(const ActorIsolation &state) {
    return llvm::hash_combine(
        state.kind, state.pointer, state.isolatedByPreconcurrency,
        state.parameterIndex);
  }
};

/// Determine how the given value declaration is isolated.
ActorIsolation getActorIsolation(ValueDecl *value);

/// Determine how the given declaration context is isolated.
/// \p getClosureActorIsolation allows the specification of actor isolation for
/// closures that haven't been saved been saved to the AST yet. This is useful
/// for solver-based code completion which doesn't modify the AST but stores the
/// actor isolation of closures in the constraint system solution.
ActorIsolation getActorIsolationOfContext(
    DeclContext *dc,
    llvm::function_ref<ClosureActorIsolation(AbstractClosureExpr *)>
        getClosureActorIsolation =
            _getRef__AbstractClosureExpr_getActorIsolation());

/// Check if both the value, and context are isolated to the same actor.
bool isSameActorIsolated(ValueDecl *value, DeclContext *dc);

/// Determines whether this function's body uses flow-sensitive isolation.
bool usesFlowSensitiveIsolation(AbstractFunctionDecl const *fn);

/// Check if it is safe for the \c globalActor qualifier to be removed from
/// \c ty, when the function value of that type is isolated to that actor.
///
/// In general this is safe in a narrow but common case: a global actor
/// qualifier can be dropped from a function type while in a DeclContext
/// isolated to that same actor, as long as the value is not Sendable.
///
/// \param dc the innermost context in which the cast to remove the global actor
///           is happening.
/// \param globalActor global actor that was dropped from \c ty.
/// \param ty a function type where \c globalActor was removed from it.
/// \param getClosureActorIsolation function that knows how to produce accurate
///        information about the isolation of a closure.
/// \return true if it is safe to drop the global-actor qualifier.
bool safeToDropGlobalActor(
                DeclContext *dc, Type globalActor, Type ty,
                llvm::function_ref<ClosureActorIsolation(AbstractClosureExpr *)>
                    getClosureActorIsolation =
                        _getRef__AbstractClosureExpr_getActorIsolation());

void simple_display(llvm::raw_ostream &out, const ActorIsolation &state);

/// A DeferredSendableDiagnostic wraps a list of closures that emit
/// Diagnostics when called. It is used to allow the logic for forming
/// those diagnostics to take place ahead of time, while delaying the
/// actual emission until several passes later. In particular, diagnostics
/// that identify NonSendable types being sent between isolation domains
/// are deferred thus so that a later flow-sensitive SIL pass can eliminate
/// diagnostics for sends that are provably safe.
struct DeferredSendableDiagnostic {
private:

  // This field indicates whether any errors (as opposed to just warnings
  // and notes) are produced by this DeferredSendableDiagnostic instance.
  // This exists to allow existing control flow through the call stack in
  // ActorIsolationChecker's walk methods. Because that control flow wasn't
  // entirely principled, sometime the use of this field doesn't exactly
  // align with the presence of errors vs warnings, for example in
  // diagnoseReferenceToUnsafeGlobal and diagnoseInOutArg.
  bool ProducesErrors;

  // This field stores a vector, each entry of which is a closure that can be
  // called, in order, to emit diagnostics.
  std::vector<std::function<void()>> Diagnostics;

public:
  DeferredSendableDiagnostic()
      : ProducesErrors(false){}

  // In general, an empty no-op closure should not be passed to
  // Diagnostic here, or ProducesDiagnostics will contain an
  // imprecise value.
  DeferredSendableDiagnostic(
      bool ProducesErrors, std::function<void()> Diagnostic)
      : ProducesErrors(ProducesErrors),
        Diagnostics({Diagnostic}) {
    assert(Diagnostic && "Empty diagnostics function");
  }

  bool producesErrors() const {
    return ProducesErrors;
  }

  bool producesDiagnostics() const {
    return Diagnostics.size() != 0;
  }

  // Idempotent operation: call the contained closures in Diagnostics in order,
  // and clear out the list so subsequent invocations are a no-op
  void produceDiagnostics() {
    for (auto Diagnostic : Diagnostics) {
      Diagnostic();
    }
    ProducesErrors = false;
    Diagnostics = {};
  }

  void setProducesErrors(bool producesErrors) {
    ProducesErrors = producesErrors;
  }

  // In general, an empty no-op closure should not be passed to
  // Diagnostic here, or ProducesDiagnostics will contain an
  // imprecise value.
  void addDiagnostic(std::function<void()> Diagnostic) {
    assert(Diagnostic && "Empty diagnostics function");

    Diagnostics.push_back(Diagnostic);
  }

  /// This variation on addErrorProducingDiagnostic should be called
  /// when the passed lambda will definitely through a diagnostic
  /// for the sake of maintaining existing control flow paths, it
  /// is not used everywhere.
  void addErrorProducingDiagnostic(std::function<void()> produceMoreDiagnostics) {
    addDiagnostic(produceMoreDiagnostics);
    setProducesErrors(true);
  }

  // compose this DeferredSendableDiagnostic with another - calling their
  // wrapped Diagnostics closure in sequence and disjuncting their
  // respective ProducesErrors flags
  void followWith(DeferredSendableDiagnostic other) {
    for (auto Diagnostic : other.Diagnostics) {
      Diagnostics.push_back(Diagnostic);
    }
    ProducesErrors = ProducesErrors || other.ProducesErrors;
  }
};

} // end namespace swift

#endif /* SWIFT_AST_ACTORISOLATIONSTATE_H */
