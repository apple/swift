//===--- CSFix.h - Constraint Fixes ---------------------------------------===//
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
// This file provides necessary abstractions for constraint fixes.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SEMA_CSFIX_H
#define SWIFT_SEMA_CSFIX_H

#include "swift/AST/ASTNode.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Debug.h"
#include "swift/Sema/ConstraintLocator.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/TrailingObjects.h"
#include <string>

namespace llvm {
class raw_ostream;
}

namespace swift {

class SourceManager;

namespace constraints {

class OverloadChoice;
class ConstraintSystem;
class ConstraintLocator;
class ConstraintLocatorBuilder;
enum class ConversionRestrictionKind;
class Solution;
struct MemberLookupResult;

/// Describes the kind of fix to apply to the given constraint before
/// visiting it.
///
/// Note: values 0 and 1 are reserved for empty and tombstone kinds.
enum class FixKind : uint8_t {
  /// Introduce a '!' to force an optional unwrap.
  ForceOptional = 2,

  /// Unwrap an optional base when we have a member access.
  UnwrapOptionalBase,
  UnwrapOptionalBaseWithOptionalResult,

  /// Append 'as! T' to force a downcast to the specified type.
  ForceDowncast,

  /// Introduce a '&' to take the address of an lvalue.
  AddressOf,
  /// Remove extraneous use of `&`.
  RemoveAddressOf,

  /// Replace a coercion ('as') with a forced checked cast ('as!').
  CoerceToCheckedCast,

  /// Mark function type as explicitly '@escaping'.
  ExplicitlyEscaping,

  /// Mark function type as having a particular global actor.
  MarkGlobalActorFunction,

  /// Arguments have labeling failures - missing/extraneous or incorrect
  /// labels attached to the, fix it by suggesting proper labels.
  RelabelArguments,

  /// Treat rvalue as lvalue
  TreatRValueAsLValue,

  /// Add a new conformance to the type to satisfy a requirement.
  AddConformance,

  /// Skip same-type generic requirement constraint,
  /// and assume that types are equal.
  SkipSameTypeRequirement,

  /// Skip superclass generic requirement constraint,
  /// and assume that types are related.
  SkipSuperclassRequirement,

  /// Fix up one of the sides of conversion to make it seem
  /// like the types are aligned.
  ContextualMismatch,

  /// Fix up the generic arguments of two types so they match each other.
  GenericArgumentsMismatch,

  /// Fix up @autoclosure argument to the @autoclosure parameter,
  /// to for a call to be able to foward it properly, since
  /// @autoclosure conversions are unsupported starting from
  /// Swift version 5.
  AutoClosureForwarding,

  /// Remove `!` or `?` because base is not an optional type.
  RemoveUnwrap,

  /// Add explicit `()` at the end of function or member to call it.
  InsertCall,

  /// Add '$' or '_' to refer to the property wrapper or storage instead
  /// of the wrapped property type.
  UsePropertyWrapper,

  /// Remove '$' or '_' to refer to the wrapped property type instead of
  /// the storage or property wrapper.
  UseWrappedValue,

  /// Allow a type that is not a property wrapper to be used as a property
  /// wrapper.
  AllowInvalidPropertyWrapperType,

  /// Remove the '$' prefix from an argument label or parameter name.
  RemoveProjectedValueArgument,

  /// Instead of spelling out `subscript` directly, use subscript operator.
  UseSubscriptOperator,

  /// Requested name is not associated with a give base type,
  /// fix this issue by pretending that member exists and matches
  /// given arguments/result types exactly.
  DefineMemberBasedOnUse,

  /// Allow access to type member on instance or instance member on type
  AllowTypeOrInstanceMember,

  /// Allow expressions where 'mutating' method is only partially applied,
  /// which means either not applied at all e.g. `Foo.bar` or only `Self`
  /// is applied e.g. `foo.bar` or `Foo.bar(&foo)`.
  ///
  /// Allow expressions where initializer call (either `self.init` or
  /// `super.init`) is only partially applied.
  AllowInvalidPartialApplication,

  /// Non-required constructors may not be not inherited. Therefore when
  /// constructing a class object, either the metatype must be statically
  /// derived (rather than an arbitrary value of metatype type) or the
  /// referenced constructor must be required.
  AllowInvalidInitRef,

  /// Allow a tuple to be destructured with mismatched arity, or mismatched
  /// types.
  AllowTupleTypeMismatch,

  /// Allow a function type to be destructured with mismatched parameter types
  /// or return type.
  AllowFunctionTypeMismatch,

  /// Allow an invalid member access on a value of protocol type as if
  /// that protocol type were a generic constraint requiring conformance
  /// to that protocol.
  AllowMemberRefOnExistential,

  /// If there are fewer arguments than parameters, let's fix that up
  /// by adding new arguments to the list represented as type variables.
  AddMissingArguments,

  /// If there are more arguments than parameters, let's fix that up
  /// by removing extraneous arguments.
  RemoveExtraneousArguments,

  /// Allow single tuple closure parameter destructuring into N arguments.
  AllowClosureParameterDestructuring,

  /// If there is out-of-order argument, let's fix that by re-ordering.
  MoveOutOfOrderArgument,

  /// If there is a matching inaccessible member - allow it as if there
  /// no access control.
  AllowInaccessibleMember,

  /// Allow KeyPaths to use AnyObject as root type
  AllowAnyObjectKeyPathRoot,

  /// Using subscript references in the keypath requires that each
  /// of the index arguments to be Hashable.
  TreatKeyPathSubscriptIndexAsHashable,

  /// Allow an invalid reference to a member declaration as part
  /// of a key path component.
  AllowInvalidRefInKeyPath,

  /// Remove `return` or default last expression of single expression
  /// function to `Void` to conform to expected result type.
  RemoveReturn,

  /// Default ambiguous generic arguments to \c Any
  DefaultGenericArgument,

  /// Skip any unhandled constructs that occur within a closure argument that
  /// matches up with a parameter that has a result builder.
  SkipUnhandledConstructInResultBuilder,

  /// Allow invalid reference to a member declared as `mutating`
  /// when base is an r-value type.
  AllowMutatingMemberOnRValueBase,

  /// Allow a single tuple parameter to be matched with N arguments
  /// by forming all of the given arguments into a single tuple.
  AllowTupleSplatForSingleParameter,

  /// Allow a single argument type mismatch. This is the most generic
  /// failure related to argument-to-parameter conversions.
  AllowArgumentTypeMismatch,

  /// Explicitly construct type conforming to `RawRepresentable` protocol
  /// via forming `Foo(rawValue:)` instead of using its `RawValue` directly.
  ExplicitlyConstructRawRepresentable,

  /// Use raw value type associated with raw representable, accessible
  /// using `.rawValue` member.
  UseRawValue,

  /// If an array was passed to a variadic argument, give a specific diagnostic
  /// and offer to drop the brackets if it's a literal.
  ExpandArrayIntoVarargs,

  /// Remove extraneous call to something which can't be invoked e.g.
  /// a variable, a property etc.
  RemoveCall,

  /// Allow an ephemeral argument conversion for a parameter marked as being
  /// non-ephemeral.
  TreatEphemeralAsNonEphemeral,

  /// Base type in reference to the contextual member e.g. `.foo` couldn't be
  /// inferred and has to be specified explicitly.
  SpecifyBaseTypeForContextualMember,

  /// Type of the closure parameter used in the body couldn't be inferred
  /// and has to be specified explicitly.
  SpecifyClosureParameterType,

  /// Closure return type has to be explicitly specified because it can't be
  /// inferred in current context e.g. because it's a multi-statement closure.
  SpecifyClosureReturnType,

  /// Object literal type coudn't be inferred because the module where
  /// the default type that implements the associated literal protocol
  /// is declared was not imported.
  SpecifyObjectLiteralTypeImport,

  /// Allow any type (and not just class or class-constrained type) to
  /// be convertible to AnyObject.
  AllowNonClassTypeToConvertToAnyObject,

  /// Member shadows a top-level name, such a name could only be accessed by
  /// prefixing it with a module name.
  AddQualifierToAccessTopLevelName,

  /// A warning fix that allows a coercion to perform a force-cast.
  AllowCoercionToForceCast,

  /// Allow key path root type mismatch when applying a key path that has a
  /// root type not convertible to the type of the base instance.
  AllowKeyPathRootTypeMismatch,

  /// Allow key path to be bound to a function type with more than 1 argument
  AllowMultiArgFuncKeyPathMismatch,

  /// Specify key path root type when it cannot be infered from context.
  SpecifyKeyPathRootType,

  /// Unwrap optional base on key path application.
  UnwrapOptionalBaseKeyPathApplication,

  /// Explicitly specify a label to match trailing closure to a certain
  /// parameter in the call.
  SpecifyLabelToAssociateTrailingClosure,

  /// Allow key path expressions with no components.
  AllowKeyPathWithoutComponents,

  /// Ignore result builder body which fails `pre-check` call.
  IgnoreInvalidResultBuilderBody,

  /// Resolve type of `nil` by providing a contextual type.
  SpecifyContextualTypeForNil,

  /// Allow expressions to reference invalid declarations by turning
  /// them into holes.
  AllowRefToInvalidDecl,

  /// Treat empty and single-element array literals as if they were incomplete
  /// dictionary literals when used as such.
  TreatArrayLiteralAsDictionary,

  /// Explicitly specify the type to disambiguate between possible member base
  /// types.
  SpecifyBaseTypeForOptionalUnresolvedMember,

  /// Allow a runtime checked cast from an optional type where we statically
  /// know the result is always succeed.
  AllowCheckedCastCoercibleOptionalType,

  /// Allow a runtime checked cast where we statically know the result
  /// is always succeed.
  AllowAlwaysSucceedCheckedCast,

  /// Allow a runtime checked cast where at compile time the from is
  /// convertible, but runtime does not support such convertions. e.g.
  /// function type casts.
  AllowUnsupportedRuntimeCheckedCast,

  /// Allow reference to a static member on a protocol metatype
  /// even though result type of the reference doesn't conform
  /// to an expected protocol.
  AllowInvalidStaticMemberRefOnProtocolMetatype,

  /// Allow the wrappedValue type of any property wrapper that is a
  /// part of a composed property wrapper to mismatch the type of
  /// another property wrapper that is a part of the same composed
  /// property wrapper.
  AllowWrappedValueMismatch,

  /// Specify a type for an explicitly written placeholder that could not be
  /// resolved.
  SpecifyTypeForPlaceholder
};

class ConstraintFix {
  ConstraintSystem &CS;
  FixKind Kind;
  ConstraintLocator *Locator;

  /// Determines whether this fix is simplify a warning which doesn't
  /// require immediate source changes.
  bool IsWarning;

public:
  ConstraintFix(ConstraintSystem &cs, FixKind kind, ConstraintLocator *locator,
                bool warning = false)
      : CS(cs), Kind(kind), Locator(locator), IsWarning(warning) {}

  virtual ~ConstraintFix();

  template <typename Fix>
  const Fix *getAs() const {
    return Fix::classof(this) ? static_cast<const Fix *>(this) : nullptr;
  }

  FixKind getKind() const { return Kind; }

  bool isWarning() const { return IsWarning; }

  virtual std::string getName() const = 0;

  /// Coalesce this fix with the given secondary fixes and diagnose the failure.
  ///
  /// The default implementation ignores \c secondaryFixes and calls
  /// \c diagnose.
  virtual bool coalesceAndDiagnose(const Solution &solution,
                                   ArrayRef<ConstraintFix *> secondaryFixes,
                                   bool asNote = false) const {
    return diagnose(solution, asNote);
  }

  /// Diagnose a failure associated with this fix.
  virtual bool diagnose(const Solution &solution,
                        bool asNote = false) const = 0;

  using CommonFixesArray =
      ArrayRef<std::pair<const Solution *, const ConstraintFix *>>;

  virtual bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const {
    return false;
  }

  void print(llvm::raw_ostream &Out) const;

  SWIFT_DEBUG_DUMP;

  /// Retrieve anchor expression associated with this fix.
  /// NOTE: such anchor comes directly from locator without
  /// any simplication attempts.
  ASTNode getAnchor() const;
  ConstraintLocator *getLocator() const { return Locator; }

protected:
  ConstraintSystem &getConstraintSystem() const { return CS; }
};

/// Unwrap an optional base when we have a member access.
class UnwrapOptionalBase final : public ConstraintFix {
  DeclNameRef MemberName;
  Type MemberBaseType;

  UnwrapOptionalBase(ConstraintSystem &cs, FixKind kind, DeclNameRef member,
                     Type memberBaseType, ConstraintLocator *locator)
      : ConstraintFix(cs, kind, locator), MemberName(member),
        MemberBaseType(memberBaseType) {
    assert(kind == FixKind::UnwrapOptionalBase ||
           kind == FixKind::UnwrapOptionalBaseWithOptionalResult);
  }

public:
  std::string getName() const override {
    return "unwrap optional base of member lookup";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static UnwrapOptionalBase *create(ConstraintSystem &cs, DeclNameRef member,
                                    Type memberBaseType,
                                    ConstraintLocator *locator);

  static UnwrapOptionalBase *
  createWithOptionalResult(ConstraintSystem &cs, DeclNameRef member,
                           Type memberBaseType, ConstraintLocator *locator);
};

// Treat rvalue as if it was an lvalue
class TreatRValueAsLValue final : public ConstraintFix {
  TreatRValueAsLValue(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::TreatRValueAsLValue, locator) {}

public:
  std::string getName() const override { return "treat rvalue as lvalue"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static TreatRValueAsLValue *create(ConstraintSystem &cs,
                                     ConstraintLocator *locator);
};

/// Arguments have labeling failures - missing/extraneous or incorrect
/// labels attached to the, fix it by suggesting proper labels.
class RelabelArguments final
    : public ConstraintFix,
      private llvm::TrailingObjects<RelabelArguments, Identifier> {
  friend TrailingObjects;

  unsigned NumLabels;

  RelabelArguments(ConstraintSystem &cs,
                   llvm::ArrayRef<Identifier> correctLabels,
                   ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::RelabelArguments, locator),
        NumLabels(correctLabels.size()) {
    std::uninitialized_copy(correctLabels.begin(), correctLabels.end(),
                            getLabelsBuffer().begin());
  }

public:
  std::string getName() const override { return "re-label argument(s)"; }

  ArrayRef<Identifier> getLabels() const {
    return {getTrailingObjects<Identifier>(), NumLabels};
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static RelabelArguments *create(ConstraintSystem &cs,
                                  llvm::ArrayRef<Identifier> correctLabels,
                                  ConstraintLocator *locator);

private:
  MutableArrayRef<Identifier> getLabelsBuffer() {
    return {getTrailingObjects<Identifier>(), NumLabels};
  }
};

/// Add a new conformance to the type to satisfy a requirement.
class MissingConformance final : public ConstraintFix {
  // Determines whether given protocol type comes from the context e.g.
  // assignment destination or argument comparison.
  bool IsContextual;

  Type NonConformingType;
  // This could either be a protocol or protocol composition.
  Type ProtocolType;

  MissingConformance(ConstraintSystem &cs, bool isContextual, Type type,
                     Type protocolType, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AddConformance, locator),
        IsContextual(isContextual), NonConformingType(type),
        ProtocolType(protocolType) {}

public:
  std::string getName() const override {
    return "add missing protocol conformance";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override;

  static MissingConformance *forRequirement(ConstraintSystem &cs, Type type,
                                            Type protocolType,
                                            ConstraintLocator *locator);

  static MissingConformance *forContextual(ConstraintSystem &cs, Type type,
                                           Type protocolType,
                                           ConstraintLocator *locator);

  Type getNonConformingType() { return NonConformingType; }

  Type getProtocolType() { return ProtocolType; }

  bool isEqual(const ConstraintFix *other) const;

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AddConformance;
  }
};

/// Skip same-type generic requirement constraint,
/// and assume that types are equal.
class SkipSameTypeRequirement final : public ConstraintFix {
  Type LHS, RHS;

  SkipSameTypeRequirement(ConstraintSystem &cs, Type lhs, Type rhs,
                          ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::SkipSameTypeRequirement, locator), LHS(lhs),
        RHS(rhs) {}

public:
  std::string getName() const override {
    return "skip same-type generic requirement";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  Type lhsType() { return LHS; }
  Type rhsType() { return RHS; }

  static SkipSameTypeRequirement *create(ConstraintSystem &cs, Type lhs,
                                         Type rhs, ConstraintLocator *locator);
};

/// Skip 'superclass' generic requirement constraint,
/// and assume that types are equal.
class SkipSuperclassRequirement final : public ConstraintFix {
  Type LHS, RHS;

  SkipSuperclassRequirement(ConstraintSystem &cs, Type lhs, Type rhs,
                            ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::SkipSuperclassRequirement, locator),
        LHS(lhs), RHS(rhs) {}

public:
  std::string getName() const override {
    return "skip superclass generic requirement";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  Type subclassType() { return LHS; }
  Type superclassType() { return RHS; }

  static SkipSuperclassRequirement *
  create(ConstraintSystem &cs, Type lhs, Type rhs, ConstraintLocator *locator);
};

/// For example: Sometimes type returned from the body of the
/// closure doesn't match expected contextual type:
///
/// func foo(_: () -> Int) {}
/// foo { "ultimate question" }
///
/// Body of the closure produces `String` type when `Int` is expected
/// by the context.
class ContextualMismatch : public ConstraintFix {
  Type LHS, RHS;

protected:
  ContextualMismatch(ConstraintSystem &cs, Type lhs, Type rhs,
                     ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::ContextualMismatch, locator), LHS(lhs),
        RHS(rhs) {}
  ContextualMismatch(ConstraintSystem &cs, FixKind kind, Type lhs, Type rhs,
                     ConstraintLocator *locator, bool warning = false)
      : ConstraintFix(cs, kind, locator, warning), LHS(lhs), RHS(rhs) {}

public:
  std::string getName() const override { return "fix contextual mismatch"; }

  Type getFromType() const { return LHS; }
  Type getToType() const { return RHS; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override;

  static ContextualMismatch *create(ConstraintSystem &cs, Type lhs, Type rhs,
                                    ConstraintLocator *locator);
};

class TreatArrayLiteralAsDictionary final : public ContextualMismatch {
  TreatArrayLiteralAsDictionary(ConstraintSystem &cs, Type dictionaryTy,
                                Type arrayTy, ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::TreatArrayLiteralAsDictionary,
                           dictionaryTy, arrayTy, locator) {
      }

public:
  std::string getName() const override {
    return "treat array literal as dictionary";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static TreatArrayLiteralAsDictionary *create(ConstraintSystem &cs,
                                               Type dictionaryTy, Type arrayTy,
                                               ConstraintLocator *loc);
};

class AllowWrappedValueMismatch : public ContextualMismatch {
  AllowWrappedValueMismatch(ConstraintSystem &cs, Type lhs, Type rhs,
                            ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::AllowWrappedValueMismatch, lhs, rhs, locator) {}

public:
  std::string getName() const override { return "fix wrapped value type mismatch"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowWrappedValueMismatch *create(ConstraintSystem &cs, Type lhs, Type rhs,
                                           ConstraintLocator *locator);
};

/// Mark function type as explicitly '@escaping'.
class MarkExplicitlyEscaping final : public ContextualMismatch {
  MarkExplicitlyEscaping(ConstraintSystem &cs, Type lhs, Type rhs,
                         ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::ExplicitlyEscaping, lhs, rhs, locator) {
  }

public:
  std::string getName() const override { return "add @escaping"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static MarkExplicitlyEscaping *create(ConstraintSystem &cs, Type lhs,
                                        Type rhs, ConstraintLocator *locator);
};

/// Mark function type as being part of a global actor.
class MarkGlobalActorFunction final : public ContextualMismatch {
  MarkGlobalActorFunction(ConstraintSystem &cs, Type lhs, Type rhs,
                         ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::MarkGlobalActorFunction, lhs, rhs,
                           locator) {
  }

public:
  std::string getName() const override { return "add @escaping"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static MarkGlobalActorFunction *create(ConstraintSystem &cs, Type lhs,
                                         Type rhs, ConstraintLocator *locator);
};

/// Introduce a '!' to force an optional unwrap.
class ForceOptional final : public ContextualMismatch {
  ForceOptional(ConstraintSystem &cs, Type fromType, Type toType,
                ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::ForceOptional, fromType, toType,
                           locator) {
    assert(fromType && "Base type must not be null");
    assert(fromType->getOptionalObjectType() &&
           "Unwrapped type must not be null");
  }

public:
  std::string getName() const override { return "force optional"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static ForceOptional *create(ConstraintSystem &cs, Type fromType, Type toType,
                               ConstraintLocator *locator);
};

/// This is a contextual mismatch between @Sendable and non-@Sendable
/// function types, repair it by adding @Sendable attribute.
class AddSendableAttribute final : public ContextualMismatch {
  AddSendableAttribute(ConstraintSystem &cs, FunctionType *fromType,
                     FunctionType *toType, ConstraintLocator *locator)
      : ContextualMismatch(cs, fromType, toType, locator) {
    assert(fromType->isSendable() != toType->isSendable());
  }

public:
  std::string getName() const override { return "add '@Sendable' attribute"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AddSendableAttribute *create(ConstraintSystem &cs,
                                    FunctionType *fromType,
                                    FunctionType *toType,
                                    ConstraintLocator *locator);
};

/// This is a contextual mismatch between throwing and non-throwing
/// function types, repair it by dropping `throws` attribute.
class DropThrowsAttribute final : public ContextualMismatch {
  DropThrowsAttribute(ConstraintSystem &cs, FunctionType *fromType,
                      FunctionType *toType, ConstraintLocator *locator)
      : ContextualMismatch(cs, fromType, toType, locator) {
    assert(fromType->isThrowing() != toType->isThrowing());
  }

public:
  std::string getName() const override { return "drop 'throws' attribute"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static DropThrowsAttribute *create(ConstraintSystem &cs,
                                     FunctionType *fromType,
                                     FunctionType *toType,
                                     ConstraintLocator *locator);
};

/// This is a contextual mismatch between async and non-async
/// function types, repair it by dropping `async` attribute.
class DropAsyncAttribute final : public ContextualMismatch {
  DropAsyncAttribute(ConstraintSystem &cs, FunctionType *fromType,
                     FunctionType *toType, ConstraintLocator *locator)
      : ContextualMismatch(cs, fromType, toType, locator) {
    assert(fromType->isAsync() != toType->isAsync());
  }

public:
  std::string getName() const override { return "drop 'async' attribute"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static DropAsyncAttribute *create(ConstraintSystem &cs,
                                    FunctionType *fromType,
                                    FunctionType *toType,
                                    ConstraintLocator *locator);
};

/// Append 'as! T' to force a downcast to the specified type.
class ForceDowncast final : public ContextualMismatch {
  ForceDowncast(ConstraintSystem &cs, Type fromType, Type toType,
                ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::ForceDowncast, fromType, toType,
                           locator) {}

public:
  std::string getName() const override;

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static ForceDowncast *create(ConstraintSystem &cs, Type fromType, Type toType,
                               ConstraintLocator *locator);
};

/// Introduce a '&' to take the address of an lvalue.
class AddAddressOf final : public ContextualMismatch {
  AddAddressOf(ConstraintSystem &cs, Type argTy, Type paramTy,
               ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::AddressOf, argTy, paramTy, locator) {}

public:
  std::string getName() const override { return "add address-of"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AddAddressOf *create(ConstraintSystem &cs, Type argTy, Type paramTy,
                              ConstraintLocator *locator);
};

class RemoveAddressOf final : public ContextualMismatch {
  RemoveAddressOf(ConstraintSystem &cs, Type lhs, Type rhs,
                  ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::RemoveAddressOf, lhs, rhs, locator) {}

public:
  std::string getName() const override {
    return "remove extraneous use of `&`";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static RemoveAddressOf *create(ConstraintSystem &cs, Type lhs, Type rhs,
                                 ConstraintLocator *locator);
};

/// Detect situations where two type's generic arguments must
/// match but are not convertible e.g.
///
/// ```swift
/// struct F<G> {}
/// let _:F<Int> = F<Bool>()
/// ```
class GenericArgumentsMismatch final
    : public ContextualMismatch,
      private llvm::TrailingObjects<GenericArgumentsMismatch, unsigned> {
  friend TrailingObjects;

  unsigned NumMismatches;

protected:
  GenericArgumentsMismatch(ConstraintSystem &cs, Type actual, Type required,
                           llvm::ArrayRef<unsigned> mismatches,
                           ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::GenericArgumentsMismatch, actual,
                           required, locator),
        NumMismatches(mismatches.size()) {
    assert(actual->is<BoundGenericType>());
    assert(required->is<BoundGenericType>());
    std::uninitialized_copy(mismatches.begin(), mismatches.end(),
                            getMismatchesBuf().begin());
  }

public:
  std::string getName() const override {
    return "fix generic argument mismatch";
  }

  ArrayRef<unsigned> getMismatches() const {
    return {getTrailingObjects<unsigned>(), NumMismatches};
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static GenericArgumentsMismatch *create(ConstraintSystem &cs, Type actual,
                                          Type required,
                                          llvm::ArrayRef<unsigned> mismatches,
                                          ConstraintLocator *locator);

private:
  MutableArrayRef<unsigned> getMismatchesBuf() {
    return {getTrailingObjects<unsigned>(), NumMismatches};
  }
};

/// Detect situations where key path doesn't have capability required
/// by the context e.g. read-only vs. writable, or either root or value
/// types are incorrect e.g.
///
/// ```swift
/// struct S { let foo: Int }
/// let _: WritableKeyPath<S, Int> = \.foo
/// ```
///
/// Here context requires a writable key path but `foo` property is
/// read-only.
class KeyPathContextualMismatch final : public ContextualMismatch {
  KeyPathContextualMismatch(ConstraintSystem &cs, Type lhs, Type rhs,
                            ConstraintLocator *locator)
      : ContextualMismatch(cs, lhs, rhs, locator) {}

public:
  std::string getName() const override {
    return "fix key path contextual mismatch";
  }

  static KeyPathContextualMismatch *
  create(ConstraintSystem &cs, Type lhs, Type rhs, ConstraintLocator *locator);
};

/// Detect situations when argument of the @autoclosure parameter is itself
/// marked as @autoclosure and is not applied. Form a fix which suggests a
/// proper way to forward such arguments, e.g.:
///
/// ```swift
/// func foo(_ fn: @autoclosure () -> Int) {}
/// func bar(_ fn: @autoclosure () -> Int) {
///   foo(fn) // error - fn should be called
/// }
/// ```
class AutoClosureForwarding final : public ConstraintFix {
  AutoClosureForwarding(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AutoClosureForwarding, locator) {}

public:
  std::string getName() const override { return "fix @autoclosure forwarding"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AutoClosureForwarding *create(ConstraintSystem &cs,
                                       ConstraintLocator *locator);
};

/// Allow invalid pointer conversions for autoclosure result types as if the
/// pointer type is a function parameter rather than an autoclosure result.
class AllowAutoClosurePointerConversion final : public ContextualMismatch {
  AllowAutoClosurePointerConversion(ConstraintSystem &cs, Type pointeeType,
                                    Type pointerType, ConstraintLocator *locator)
      : ContextualMismatch(cs, pointeeType, pointerType, locator) {}

public:
  std::string getName() const override {
    return "allow pointer conversion for autoclosure result type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowAutoClosurePointerConversion *create(ConstraintSystem &cs,
                                                   Type pointeeType,
                                                   Type pointerType,
                                                   ConstraintLocator *locator);
};

class RemoveUnwrap final : public ConstraintFix {
  Type BaseType;

  RemoveUnwrap(ConstraintSystem &cs, Type baseType, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::RemoveUnwrap, locator), BaseType(baseType) {}

public:
  std::string getName() const override {
    return "remove unwrap operator `!` or `?`";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static RemoveUnwrap *create(ConstraintSystem &cs, Type baseType,
                              ConstraintLocator *locator);
};

class InsertExplicitCall final : public ConstraintFix {
  InsertExplicitCall(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::InsertCall, locator) {}

public:
  std::string getName() const override {
    return "insert explicit `()` to make a call";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static InsertExplicitCall *create(ConstraintSystem &cs,
                                    ConstraintLocator *locator);
};

class UsePropertyWrapper final : public ConstraintFix {
  VarDecl *Wrapped;
  bool UsingProjection;
  Type Base;
  Type Wrapper;

  UsePropertyWrapper(ConstraintSystem &cs, VarDecl *wrapped,
                     bool usingProjection, Type base, Type wrapper,
                     ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::UsePropertyWrapper, locator),
        Wrapped(wrapped), UsingProjection(usingProjection), Base(base),
        Wrapper(wrapper) {}

public:
  std::string getName() const override {
    return "insert '$' or '_' to use property wrapper type instead of wrapped type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static UsePropertyWrapper *create(ConstraintSystem &cs, VarDecl *wrapped,
                                    bool usingProjection, Type base,
                                    Type wrapper, ConstraintLocator *locator);
};

class UseWrappedValue final : public ConstraintFix {
  VarDecl *PropertyWrapper;
  Type Base;
  Type Wrapper;

  UseWrappedValue(ConstraintSystem &cs, VarDecl *propertyWrapper, Type base,
                  Type wrapper, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::UseWrappedValue, locator),
        PropertyWrapper(propertyWrapper), Base(base), Wrapper(wrapper) {}

  bool usingProjection() const {
    auto nameStr = PropertyWrapper->getName().str();
    return !nameStr.startswith("_");
  }

public:
  std::string getName() const override {
    return "remove '$' or _ to use wrapped type instead of wrapper type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static UseWrappedValue *create(ConstraintSystem &cs, VarDecl *propertyWrapper,
                                 Type base, Type wrapper,
                                 ConstraintLocator *locator);
};

class AllowInvalidPropertyWrapperType final : public ConstraintFix {
  Type wrapperType;

  AllowInvalidPropertyWrapperType(ConstraintSystem &cs, Type wrapperType,
                                  ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowInvalidPropertyWrapperType, locator),
        wrapperType(wrapperType) {}

public:
  static AllowInvalidPropertyWrapperType *create(ConstraintSystem &cs, Type wrapperType,
                                                 ConstraintLocator *locator);

  std::string getName() const override {
    return "allow invalid property wrapper type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;
};

class RemoveProjectedValueArgument final : public ConstraintFix {
  Type wrapperType;
  ParamDecl *param;

  RemoveProjectedValueArgument(ConstraintSystem &cs, Type wrapper,
                               ParamDecl *param, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::RemoveProjectedValueArgument, locator),
        wrapperType(wrapper), param(param) {}

public:
  static RemoveProjectedValueArgument *create(ConstraintSystem &cs, Type wrapper,
                                              ParamDecl *param, ConstraintLocator *locator);

  std::string getName() const override {
    return "remove '$' from argument label";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;
};

class UseSubscriptOperator final : public ConstraintFix {
  UseSubscriptOperator(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::UseSubscriptOperator, locator) {}

public:
  std::string getName() const override {
    return "replace '.subscript(...)' with subscript operator";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static UseSubscriptOperator *create(ConstraintSystem &cs,
                                      ConstraintLocator *locator);
};

class DefineMemberBasedOnUse final : public ConstraintFix {
  Type BaseType;
  DeclNameRef Name;

  /// Whether or not the member error is already diagnosed. This can happen
  /// when referencing an erroneous member, and the error is diagnosed at the
  /// member declaration.
  ///
  /// We still want to define erroneous members based on use in order to find
  /// a solution through the new diagnostic infrastructure, but we don't
  /// want to report a second error message.
  bool AlreadyDiagnosed;

  DefineMemberBasedOnUse(ConstraintSystem &cs, Type baseType, DeclNameRef member,
                         bool alreadyDiagnosed, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::DefineMemberBasedOnUse, locator),
        BaseType(baseType), Name(member), AlreadyDiagnosed(alreadyDiagnosed) {}

public:
  std::string getName() const override {
    llvm::SmallVector<char, 16> scratch;
    auto memberName = Name.getString(scratch);
    return "define missing member named '" + memberName.str() +
           "' based on its use";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override;

  static DefineMemberBasedOnUse *create(ConstraintSystem &cs, Type baseType,
                                        DeclNameRef member, bool alreadyDiagnosed,
                                        ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::DefineMemberBasedOnUse;
  }
};

class DefineMemberBasedOnUnintendedGenericParam final : public ConstraintFix {
  Type BaseType;
  DeclNameRef Name;
  Identifier ParamName;

  DefineMemberBasedOnUnintendedGenericParam(ConstraintSystem &cs, Type baseType,
                                            DeclNameRef member,
                                            Identifier paramName,
                                            ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::DefineMemberBasedOnUse, locator),
        BaseType(baseType), Name(member), ParamName(paramName) {}

public:
  std::string getName() const override {
    llvm::SmallVector<char, 16> scratch;
    auto memberName = Name.getString(scratch);
    return "allow access to invalid member '" + memberName.str() +
           "' on archetype presumed intended to conform to protocol";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static DefineMemberBasedOnUnintendedGenericParam *
  create(ConstraintSystem &cs, Type baseType, DeclNameRef member,
         Identifier paramName, ConstraintLocator *locator);
};

class AllowInvalidMemberRef : public ConstraintFix {
  Type BaseType;
  ValueDecl *Member;
  DeclNameRef Name;

protected:
  AllowInvalidMemberRef(ConstraintSystem &cs, FixKind kind, Type baseType,
                        ValueDecl *member, DeclNameRef name,
                        ConstraintLocator *locator)
      : ConstraintFix(cs, kind, locator), BaseType(baseType), Member(member),
        Name(name) {}

public:
  Type getBaseType() const { return BaseType; }

  ValueDecl *getMember() const { return Member; }

  DeclNameRef getMemberName() const { return Name; }
};

class AllowMemberRefOnExistential final : public AllowInvalidMemberRef {
  AllowMemberRefOnExistential(ConstraintSystem &cs, Type baseType,
                              DeclNameRef memberName, ValueDecl *member,
                              ConstraintLocator *locator)
      : AllowInvalidMemberRef(cs, FixKind::AllowMemberRefOnExistential,
                              baseType, member, memberName, locator) {}

public:
  std::string getName() const override {
    llvm::SmallVector<char, 16> scratch;
    auto memberName = getMemberName().getString(scratch);
    return "allow access to invalid member '" + memberName.str() +
           "' on value of protocol type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowMemberRefOnExistential *create(ConstraintSystem &cs,
                                             Type baseType, ValueDecl *member,
                                             DeclNameRef memberName,
                                             ConstraintLocator *locator);
};

class AllowTypeOrInstanceMember final : public AllowInvalidMemberRef {
  AllowTypeOrInstanceMember(ConstraintSystem &cs, Type baseType,
                            ValueDecl *member, DeclNameRef name,
                            ConstraintLocator *locator)
      : AllowInvalidMemberRef(cs, FixKind::AllowTypeOrInstanceMember, baseType,
                              member, name, locator) {
    assert(member);
  }

public:
  std::string getName() const override {
    return "allow access to instance member on type or a type member on instance";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowTypeOrInstanceMember *create(ConstraintSystem &cs, Type baseType,
                                           ValueDecl *member, DeclNameRef usedName,
                                           ConstraintLocator *locator);
};

class AllowInvalidPartialApplication final : public ConstraintFix {
  AllowInvalidPartialApplication(bool isWarning, ConstraintSystem &cs,
                                 ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowInvalidPartialApplication, locator,
                      isWarning) {}

public:
  std::string getName() const override {
    return "allow partially applied 'mutating' method";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowInvalidPartialApplication *create(bool isWarning,
                                                ConstraintSystem &cs,
                                                ConstraintLocator *locator);
};

class AllowInvalidInitRef final : public ConstraintFix {
  enum class RefKind {
    DynamicOnMetatype,
    ProtocolMetatype,
    NonConstMetatype,
  } Kind;

  Type BaseType;
  const ConstructorDecl *Init;
  bool IsStaticallyDerived;
  SourceRange BaseRange;

  AllowInvalidInitRef(ConstraintSystem &cs, RefKind kind, Type baseTy,
                      ConstructorDecl *init, bool isStaticallyDerived,
                      SourceRange baseRange, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowInvalidInitRef, locator), Kind(kind),
        BaseType(baseTy), Init(init), IsStaticallyDerived(isStaticallyDerived),
        BaseRange(baseRange) {}

public:
  std::string getName() const override {
    return "allow invalid initializer reference";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowInvalidInitRef *
  dynamicOnMetatype(ConstraintSystem &cs, Type baseTy, ConstructorDecl *init,
                    SourceRange baseRange, ConstraintLocator *locator);

  static AllowInvalidInitRef *
  onProtocolMetatype(ConstraintSystem &cs, Type baseTy, ConstructorDecl *init,
                     bool isStaticallyDerived, SourceRange baseRange,
                     ConstraintLocator *locator);

  static AllowInvalidInitRef *onNonConstMetatype(ConstraintSystem &cs,
                                                 Type baseTy,
                                                 ConstructorDecl *init,
                                                 ConstraintLocator *locator);

private:
  static AllowInvalidInitRef *create(RefKind kind, ConstraintSystem &cs,
                                     Type baseTy, ConstructorDecl *init,
                                     bool isStaticallyDerived,
                                     SourceRange baseRange,
                                     ConstraintLocator *locator);
};

class AllowTupleTypeMismatch final : public ContextualMismatch {
  /// If this is an element mismatch, \c Index is the element index where the
  /// type mismatch occurred. If this is an arity or label mismatch, \c Index
  /// will be \c None.
  Optional<unsigned> Index;

  AllowTupleTypeMismatch(ConstraintSystem &cs, Type lhs, Type rhs,
                         ConstraintLocator *locator, Optional<unsigned> index)
      : ContextualMismatch(cs, FixKind::AllowTupleTypeMismatch, lhs, rhs,
                           locator), Index(index) {}

public:
  static AllowTupleTypeMismatch *create(ConstraintSystem &cs, Type lhs,
                                        Type rhs, ConstraintLocator *locator,
                                        Optional<unsigned> index = None);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowTupleTypeMismatch;
  }

  std::string getName() const override {
    return "fix tuple mismatches in type and arity";
  }

  bool isElementMismatch() const {
    return Index.hasValue();
  }

  bool coalesceAndDiagnose(const Solution &solution,
                           ArrayRef<ConstraintFix *> secondaryFixes,
                           bool asNote = false) const override;

  bool diagnose(const Solution &solution, bool asNote = false) const override;
};

class AllowFunctionTypeMismatch final : public ContextualMismatch {
  /// The index of the parameter where the type mismatch occurred.
  unsigned ParamIndex;

  AllowFunctionTypeMismatch(ConstraintSystem &cs, Type lhs, Type rhs,
                            ConstraintLocator *locator, unsigned index)
      : ContextualMismatch(cs, FixKind::AllowFunctionTypeMismatch, lhs, rhs,
                           locator), ParamIndex(index) {}

public:
  static AllowFunctionTypeMismatch *create(ConstraintSystem &cs, Type lhs,
                                           Type rhs, ConstraintLocator *locator,
                                           unsigned index);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowFunctionTypeMismatch;
  }

  std::string getName() const override {
    return "allow function type mismatch";
  }

  bool coalesceAndDiagnose(const Solution &solution,
                           ArrayRef<ConstraintFix *> secondaryFixes,
                           bool asNote = false) const override;

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override;
};


class AllowMutatingMemberOnRValueBase final : public AllowInvalidMemberRef {
  AllowMutatingMemberOnRValueBase(ConstraintSystem &cs, Type baseType,
                                  ValueDecl *member, DeclNameRef name,
                                  ConstraintLocator *locator)
      : AllowInvalidMemberRef(cs, FixKind::AllowMutatingMemberOnRValueBase,
                              baseType, member, name, locator) {}

public:
  std::string getName() const override {
    return "allow `mutating` method on r-value base";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowMutatingMemberOnRValueBase *
  create(ConstraintSystem &cs, Type baseType, ValueDecl *member,
         DeclNameRef name, ConstraintLocator *locator);
};

class AllowClosureParamDestructuring final : public ConstraintFix {
  FunctionType *ContextualType;

  AllowClosureParamDestructuring(ConstraintSystem &cs,
                                 FunctionType *contextualType,
                                 ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowClosureParameterDestructuring, locator),
        ContextualType(contextualType) {}

public:
  std::string getName() const override {
    return "allow closure parameter destructuring";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowClosureParamDestructuring *create(ConstraintSystem &cs,
                                                FunctionType *contextualType,
                                                ConstraintLocator *locator);
};

struct SynthesizedArg {
  unsigned paramIdx;
  AnyFunctionType::Param param;
};

class AddMissingArguments final
    : public ConstraintFix,
      private llvm::TrailingObjects<
          AddMissingArguments, SynthesizedArg> {
  friend TrailingObjects;

  unsigned NumSynthesized;

  AddMissingArguments(ConstraintSystem &cs,
                      ArrayRef<SynthesizedArg> synthesizedArgs,
                      ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AddMissingArguments, locator),
        NumSynthesized(synthesizedArgs.size()) {
    std::uninitialized_copy(synthesizedArgs.begin(), synthesizedArgs.end(),
                            getSynthesizedArgumentsBuf().begin());
  }

public:
  std::string getName() const override { return "synthesize missing argument(s)"; }

  ArrayRef<SynthesizedArg> getSynthesizedArguments() const {
    return {getTrailingObjects<SynthesizedArg>(), NumSynthesized};
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static AddMissingArguments *create(ConstraintSystem &cs,
                                     ArrayRef<SynthesizedArg> synthesizedArgs,
                                     ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AddMissingArguments;
  }

private:
  MutableArrayRef<SynthesizedArg> getSynthesizedArgumentsBuf() {
    return {getTrailingObjects<SynthesizedArg>(), NumSynthesized};
  }
};

class RemoveExtraneousArguments final
    : public ConstraintFix,
      private llvm::TrailingObjects<
          RemoveExtraneousArguments,
          std::pair<unsigned, AnyFunctionType::Param>> {
  friend TrailingObjects;

  using IndexedParam = std::pair<unsigned, AnyFunctionType::Param>;

  FunctionType *ContextualType;
  unsigned NumExtraneous;

  RemoveExtraneousArguments(ConstraintSystem &cs, FunctionType *contextualType,
                            llvm::ArrayRef<IndexedParam> extraArgs,
                            ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::RemoveExtraneousArguments, locator),
        ContextualType(contextualType), NumExtraneous(extraArgs.size()) {
    std::uninitialized_copy(extraArgs.begin(), extraArgs.end(),
                            getExtraArgumentsBuf().begin());
  }

public:
  std::string getName() const override { return "remove extraneous argument(s)"; }

  ArrayRef<IndexedParam> getExtraArguments() const {
    return {getTrailingObjects<IndexedParam>(), NumExtraneous};
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  /// FIXME(diagnostics): Once `resolveDeclRefExpr` is gone this
  /// logic would be obsolete.
  ///
  /// Determine whether presence of extraneous arguments indicates
  /// potential name shadowing problem with local `min`/`max` shadowing
  /// global definitions with different number of arguments.
  static bool isMinMaxNameShadowing(ConstraintSystem &cs,
                                    ConstraintLocatorBuilder locator);

  static RemoveExtraneousArguments *
  create(ConstraintSystem &cs, FunctionType *contextualType,
         llvm::ArrayRef<IndexedParam> extraArgs, ConstraintLocator *locator);

private:
  MutableArrayRef<IndexedParam> getExtraArgumentsBuf() {
    return {getTrailingObjects<IndexedParam>(), NumExtraneous};
  }
};

class MoveOutOfOrderArgument final : public ConstraintFix {
  using ParamBinding = SmallVector<unsigned, 1>;

  unsigned ArgIdx;
  unsigned PrevArgIdx;

  SmallVector<ParamBinding, 4> Bindings;

  MoveOutOfOrderArgument(ConstraintSystem &cs, unsigned argIdx,
                         unsigned prevArgIdx, ArrayRef<ParamBinding> bindings,
                         ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::MoveOutOfOrderArgument, locator),
        ArgIdx(argIdx), PrevArgIdx(prevArgIdx),
        Bindings(bindings.begin(), bindings.end()) {}

public:
  std::string getName() const override {
    return "move out-of-order argument to correct position";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override;

  bool isEqual(const ConstraintFix *other) const;

  static MoveOutOfOrderArgument *create(ConstraintSystem &cs,
                                        unsigned argIdx,
                                        unsigned prevArgIdx,
                                        ArrayRef<ParamBinding> bindings,
                                        ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::MoveOutOfOrderArgument;
  }
};

class AllowInaccessibleMember final : public AllowInvalidMemberRef {
  AllowInaccessibleMember(ConstraintSystem &cs, Type baseType,
                          ValueDecl *member, DeclNameRef name,
                          ConstraintLocator *locator)
      : AllowInvalidMemberRef(cs, FixKind::AllowInaccessibleMember, baseType,
                              member, name, locator) {}

public:
  std::string getName() const override {
    return "allow inaccessible member reference";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowInaccessibleMember *create(ConstraintSystem &cs, Type baseType,
                                         ValueDecl *member, DeclNameRef name,
                                         ConstraintLocator *locator);
};

class AllowAnyObjectKeyPathRoot final : public ConstraintFix {

  AllowAnyObjectKeyPathRoot(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowAnyObjectKeyPathRoot, locator) {}

public:
  std::string getName() const override {
    return "allow anyobject as root type for a keypath";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowAnyObjectKeyPathRoot *create(ConstraintSystem &cs,
                                           ConstraintLocator *locator);
};

class AllowMultiArgFuncKeyPathMismatch final : public ConstraintFix {
  Type functionType;

  AllowMultiArgFuncKeyPathMismatch(ConstraintSystem &cs, Type fnType,
                                   ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowMultiArgFuncKeyPathMismatch, locator),
        functionType(fnType) {}

public:
  std::string getName() const override {
    return "allow conversion of a keypath type to a multi-argument function";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowMultiArgFuncKeyPathMismatch *create(ConstraintSystem &cs,
                                                  Type fnType,
                                                  ConstraintLocator *locator);
};

class TreatKeyPathSubscriptIndexAsHashable final : public ConstraintFix {
  Type NonConformingType;

  TreatKeyPathSubscriptIndexAsHashable(ConstraintSystem &cs, Type type,
                                       ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::TreatKeyPathSubscriptIndexAsHashable,
                      locator),
        NonConformingType(type) {}

public:
  std::string getName() const override {
    return "treat keypath subscript index as conforming to Hashable";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static TreatKeyPathSubscriptIndexAsHashable *
  create(ConstraintSystem &cs, Type type, ConstraintLocator *locator);
};

class AllowInvalidRefInKeyPath final : public ConstraintFix {
  enum RefKind {
    // Allow a reference to a static member as a key path component.
    StaticMember,
    // Allow a reference to a declaration with mutating getter as
    // a key path component.
    MutatingGetter,
    // Allow a reference to a method (instance or static) as
    // a key path component.
    Method,
    // Allow a reference to a initializer instance as a key path
    // component.
    Initializer,
    // Allow a reference to an enum case as a key path component.
    EnumCase,
  } Kind;

  ValueDecl *Member;

  AllowInvalidRefInKeyPath(ConstraintSystem &cs, RefKind kind,
                           ValueDecl *member, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowInvalidRefInKeyPath, locator),
        Kind(kind), Member(member) {}

public:
  std::string getName() const override {
    switch (Kind) {
    case RefKind::StaticMember:
      return "allow reference to a static member as a key path component";
    case RefKind::MutatingGetter:
      return "allow reference to a member with mutating getter as a key "
             "path component";
    case RefKind::Method:
      return "allow reference to a method as a key path component";
    case RefKind::Initializer:
      return "allow reference to an init method as a key path component";
    case RefKind::EnumCase:
      return "allow reference to an enum case as a key path component";
    }
    llvm_unreachable("covered switch");
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override;

  /// Determine whether give reference requires a fix and produce one.
  static AllowInvalidRefInKeyPath *
  forRef(ConstraintSystem &cs, ValueDecl *member, ConstraintLocator *locator);

  bool isEqual(const ConstraintFix *other) const;

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowInvalidRefInKeyPath;
  }

private:
  static AllowInvalidRefInKeyPath *create(ConstraintSystem &cs, RefKind kind,
                                          ValueDecl *member,
                                          ConstraintLocator *locator);
};

class RemoveReturn final : public ContextualMismatch {
  RemoveReturn(ConstraintSystem &cs, Type resultTy, ConstraintLocator *locator);

public:
  std::string getName() const override { return "remove or omit return type"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static RemoveReturn *create(ConstraintSystem &cs, Type resultTy,
                              ConstraintLocator *locator);
};

class CollectionElementContextualMismatch final : public ContextualMismatch {
  CollectionElementContextualMismatch(ConstraintSystem &cs, Type srcType,
                                      Type dstType, ConstraintLocator *locator)
      : ContextualMismatch(cs, srcType, dstType, locator) {}

public:
  std::string getName() const override {
    return "fix collection element contextual mismatch";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static CollectionElementContextualMismatch *
  create(ConstraintSystem &cs, Type srcType, Type dstType,
         ConstraintLocator *locator);
};

class DefaultGenericArgument final : public ConstraintFix {
  GenericTypeParamType *Param;

  DefaultGenericArgument(ConstraintSystem &cs, GenericTypeParamType *param,
                         ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::DefaultGenericArgument, locator),
        Param(param) {}

public:
  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::DefaultGenericArgument;
  }

  std::string getName() const override {
    auto paramName = Param->getString();
    return "default generic argument '" + paramName + "' to 'Any'";
  }

  bool coalesceAndDiagnose(const Solution &solution,
                           ArrayRef<ConstraintFix *> secondaryFixes,
                           bool asNote = false) const override;

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static DefaultGenericArgument *create(ConstraintSystem &cs,
                                        GenericTypeParamType *param,
                                        ConstraintLocator *locator);
};

class SkipUnhandledConstructInResultBuilder final : public ConstraintFix {
public:
  using UnhandledNode = llvm::PointerUnion<Stmt *, Decl *>;

private:
  UnhandledNode unhandled;
  NominalTypeDecl *builder;

  SkipUnhandledConstructInResultBuilder(ConstraintSystem &cs,
                                          UnhandledNode unhandled,
                                          NominalTypeDecl *builder,
                                          ConstraintLocator *locator)
    : ConstraintFix(cs, FixKind::SkipUnhandledConstructInResultBuilder,
                    locator),
      unhandled(unhandled), builder(builder) { }

public:
  std::string getName() const override {
    return "skip unhandled constructs when applying a result builder";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static SkipUnhandledConstructInResultBuilder *
  create(ConstraintSystem &cs, UnhandledNode unhandledNode,
         NominalTypeDecl *builder, ConstraintLocator *locator);
};

class AllowTupleSplatForSingleParameter final : public ConstraintFix {
  using Param = AnyFunctionType::Param;

  Type ParamType;

  AllowTupleSplatForSingleParameter(ConstraintSystem &cs, Type paramTy,
                                    ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowTupleSplatForSingleParameter, locator),
        ParamType(paramTy) {}

public:
  std::string getName() const override {
    return "allow single parameter tuple splat";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  /// Apply this fix to given arguments/parameters and return `true`
  /// this fix is not applicable and solver can't continue, `false`
  /// otherwise.
  static bool attempt(ConstraintSystem &cs, SmallVectorImpl<Param> &args,
                      ArrayRef<Param> params,
                      SmallVectorImpl<SmallVector<unsigned, 1>> &bindings,
                      ConstraintLocatorBuilder locator);
};

class IgnoreContextualType : public ContextualMismatch {
  IgnoreContextualType(ConstraintSystem &cs, Type resultTy, Type specifiedTy,
                       ConstraintLocator *locator)
      : ContextualMismatch(cs, resultTy, specifiedTy, locator) {}

public:
  std::string getName() const override {
    return "ignore specified contextual type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static IgnoreContextualType *create(ConstraintSystem &cs, Type resultTy,
                                      Type specifiedTy,
                                      ConstraintLocator *locator);
};

class IgnoreAssignmentDestinationType final : public ContextualMismatch {
  IgnoreAssignmentDestinationType(ConstraintSystem &cs, Type sourceTy,
                                  Type destTy, ConstraintLocator *locator)
      : ContextualMismatch(cs, sourceTy, destTy, locator) {}

public:
  std::string getName() const override {
    return "ignore type of the assignment destination";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override;

  static IgnoreAssignmentDestinationType *create(ConstraintSystem &cs,
                                                 Type sourceTy, Type destTy,
                                                 ConstraintLocator *locator);
};

/// If this is an argument-to-parameter conversion which is associated with
/// `inout` parameter, subtyping is not permitted, types have to
/// be identical.
class AllowInOutConversion final : public ContextualMismatch {
  AllowInOutConversion(ConstraintSystem &cs, Type argType, Type paramType,
                       ConstraintLocator *locator)
      : ContextualMismatch(cs, argType, paramType, locator) {}

public:
  std::string getName() const override {
    return "allow conversions between argument/parameter marked as `inout`";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowInOutConversion *create(ConstraintSystem &cs, Type argType,
                                      Type paramType,
                                      ConstraintLocator *locator);
};

class AllowArgumentMismatch : public ContextualMismatch {
protected:
  AllowArgumentMismatch(ConstraintSystem &cs, Type argType, Type paramType,
                        ConstraintLocator *locator)
      : AllowArgumentMismatch(cs, FixKind::AllowArgumentTypeMismatch, argType,
                              paramType, locator) {}

  AllowArgumentMismatch(ConstraintSystem &cs, FixKind kind, Type argType,
                        Type paramType, ConstraintLocator *locator,
                        bool warning = false)
      : ContextualMismatch(cs, kind, argType, paramType, locator, warning) {}

public:
  std::string getName() const override {
    return "allow argument to parameter type conversion mismatch";
  }

  unsigned getParamIdx() const;

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowArgumentMismatch *create(ConstraintSystem &cs, Type argType,
                                       Type paramType,
                                       ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowArgumentTypeMismatch;
  }
};

class ExpandArrayIntoVarargs final : public AllowArgumentMismatch {

  ExpandArrayIntoVarargs(ConstraintSystem &cs, Type argType, Type paramType,
                         ConstraintLocator *locator)
      : AllowArgumentMismatch(cs, FixKind::ExpandArrayIntoVarargs, argType,
                              paramType, locator) {}

public:
  std::string getName() const override {
    return "cannot pass Array elements as variadic arguments";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static ExpandArrayIntoVarargs *attempt(ConstraintSystem &cs, Type argType,
                                         Type paramType,
                                         ConstraintLocatorBuilder locator);
};

class ExplicitlyConstructRawRepresentable final : public ConstraintFix {
  Type RawReprType;
  Type ExpectedType;

  ExplicitlyConstructRawRepresentable(ConstraintSystem &cs, Type rawReprType,
                                      Type expectedType,
                                      ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::ExplicitlyConstructRawRepresentable,
                      locator),
        RawReprType(rawReprType), ExpectedType(expectedType) {}

public:
  std::string getName() const override {
    return "explicitly construct a raw representable type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static ExplicitlyConstructRawRepresentable *
  create(ConstraintSystem &cs, Type rawTypeRepr, Type expectedType,
         ConstraintLocator *locator);
};

class UseRawValue final : public ConstraintFix {
  Type RawReprType;
  Type ExpectedType;

  UseRawValue(ConstraintSystem &cs, Type rawReprType, Type expectedType,
              ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::UseRawValue, locator),
        RawReprType(rawReprType), ExpectedType(expectedType) {}

public:
  std::string getName() const override {
    return "use `.rawValue` of a raw representable type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static UseRawValue *create(ConstraintSystem &cs, Type rawReprType,
                             Type expectedType, ConstraintLocator *locator);
};

/// Replace a coercion ('as') with runtime checked cast ('as!' or 'as?').
class CoerceToCheckedCast final : public ContextualMismatch {
  CoerceToCheckedCast(ConstraintSystem &cs, Type fromType, Type toType,
                      bool useConditionalCast, ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::CoerceToCheckedCast, fromType, toType,
                           locator),
        UseConditionalCast(useConditionalCast) {}
  bool UseConditionalCast = false;

public:
  std::string getName() const override {
    return UseConditionalCast ? "as to as?" : "as to as!";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static CoerceToCheckedCast *attempt(ConstraintSystem &cs, Type fromType,
                                      Type toType, bool useConditionalCast,
                                      ConstraintLocator *locator);
};

class RemoveInvalidCall final : public ConstraintFix {
  RemoveInvalidCall(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::RemoveCall, locator) {}

public:
  std::string getName() const override {
    return "remove extraneous call from value of non-function type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static RemoveInvalidCall *create(ConstraintSystem &cs,
                                   ConstraintLocator *locator);
};

class TreatEphemeralAsNonEphemeral final : public AllowArgumentMismatch {
  ConversionRestrictionKind ConversionKind;

  TreatEphemeralAsNonEphemeral(ConstraintSystem &cs, ConstraintLocator *locator,
                               Type srcType, Type dstType,
                               ConversionRestrictionKind conversionKind,
                               bool downgradeToWarning)
      : AllowArgumentMismatch(cs, FixKind::TreatEphemeralAsNonEphemeral,
                              srcType, dstType, locator, downgradeToWarning),
        ConversionKind(conversionKind) {}

public:
  ConversionRestrictionKind getConversionKind() const { return ConversionKind; }
  std::string getName() const override;

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static TreatEphemeralAsNonEphemeral *
  create(ConstraintSystem &cs, ConstraintLocator *locator, Type srcType,
         Type dstType, ConversionRestrictionKind conversionKind,
         bool downgradeToWarning);
};

class SpecifyBaseTypeForContextualMember final : public ConstraintFix {
  DeclNameRef MemberName;

  SpecifyBaseTypeForContextualMember(ConstraintSystem &cs, DeclNameRef member,
                                     ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::SpecifyBaseTypeForContextualMember, locator),
        MemberName(member) {}

public:
  std::string getName() const override {
    const auto baseName = MemberName.getBaseName();
    return "specify base type in reference to member '" +
           baseName.userFacingName().str() + "'";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static SpecifyBaseTypeForContextualMember *
  create(ConstraintSystem &cs, DeclNameRef member, ConstraintLocator *locator);
};

class SpecifyClosureParameterType final : public ConstraintFix {
  SpecifyClosureParameterType(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::SpecifyClosureParameterType, locator) {}

public:
  std::string getName() const override;

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static SpecifyClosureParameterType *create(ConstraintSystem &cs,
                                             ConstraintLocator *locator);
};

class SpecifyClosureReturnType final : public ConstraintFix {
  SpecifyClosureReturnType(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::SpecifyClosureReturnType, locator) {}

public:
  std::string getName() const override {
    return "specify closure return type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static SpecifyClosureReturnType *create(ConstraintSystem &cs,
                                          ConstraintLocator *locator);
};

class SpecifyObjectLiteralTypeImport final : public ConstraintFix {
  SpecifyObjectLiteralTypeImport(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::SpecifyObjectLiteralTypeImport, locator) {}

public:
  std::string getName() const override {
    return "import required module to gain access to a default literal type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static SpecifyObjectLiteralTypeImport *create(ConstraintSystem &cs,
                                                ConstraintLocator *locator);
};

class AddQualifierToAccessTopLevelName final : public ConstraintFix {
  AddQualifierToAccessTopLevelName(ConstraintSystem &cs,
                                   ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AddQualifierToAccessTopLevelName, locator) {}

public:
  std::string getName() const override {
    return "qualify reference to access top-level function";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AddQualifierToAccessTopLevelName *create(ConstraintSystem &cs,
                                                  ConstraintLocator *locator);
};

class AllowNonClassTypeToConvertToAnyObject final : public ContextualMismatch {
  AllowNonClassTypeToConvertToAnyObject(ConstraintSystem &cs, Type type,
                                        ConstraintLocator *locator);

public:
  std::string getName() const override {
    return "allow non-class type to convert to 'AnyObject'";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowNonClassTypeToConvertToAnyObject *
  create(ConstraintSystem &cs, Type type, ConstraintLocator *locator);
};

/// A warning fix to maintain compatibility with the following:
///
/// \code
/// func foo(_ arr: [Any]?) {
///  _ = (arr ?? []) as [NSObject]
/// }
/// \endcode
///
/// which performs a force-cast of the array's elements, despite being spelled
/// as a coercion.
class AllowCoercionToForceCast final : public ContextualMismatch {
  AllowCoercionToForceCast(ConstraintSystem &cs, Type fromType, Type toType,
                           ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::AllowCoercionToForceCast, fromType,
                           toType, locator, /*warning*/ true) {}

public:
  std::string getName() const override {
    return "allow coercion to be treated as a force-cast";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowCoercionToForceCast *create(ConstraintSystem &cs, Type fromType,
                                          Type toType,
                                          ConstraintLocator *locator);
};

/// Attempt to fix a key path application where the key path type cannot be
/// applied to a base instance of another type.
///
/// \code
/// func f(_ bar: Bar , keyPath: KeyPath<Foo, Int> ) {
///   bar[keyPath: keyPath]
/// }
/// \endcode
class AllowKeyPathRootTypeMismatch final : public ContextualMismatch {
protected:
  AllowKeyPathRootTypeMismatch(ConstraintSystem &cs, Type lhs, Type rhs,
                               ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::AllowKeyPathRootTypeMismatch, lhs, rhs,
                           locator) {}

public:
  std::string getName() const override {
    return "allow key path root type mismatch";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowKeyPathRootTypeMismatch *
  create(ConstraintSystem &cs, Type lhs, Type rhs, ConstraintLocator *locator);
};

class SpecifyKeyPathRootType final : public ConstraintFix {
    SpecifyKeyPathRootType(ConstraintSystem &cs, ConstraintLocator *locator)
        : ConstraintFix(cs, FixKind::SpecifyKeyPathRootType, locator) {}

  public:
    std::string getName() const override {
      return "specify key path root type";
    }

    bool diagnose(const Solution &solution, bool asNote = false) const override;

    static SpecifyKeyPathRootType *create(ConstraintSystem &cs,
                                          ConstraintLocator *locator);
};

/// Diagnose missing unwrap of optional base type on key path application.
///
/// \code
/// func f(_ bar: Bar? , keyPath: KeyPath<Bar, Int>) {
///   bar[keyPath: keyPath]
/// }
/// \endcode
class UnwrapOptionalBaseKeyPathApplication final : public ContextualMismatch {
protected:
  UnwrapOptionalBaseKeyPathApplication(ConstraintSystem &cs, Type lhs, Type rhs,
                                       ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::UnwrapOptionalBaseKeyPathApplication,
                           lhs, rhs, locator) {}

public:
  std::string getName() const override {
    return "force unwrap base on key path application";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static UnwrapOptionalBaseKeyPathApplication *
  attempt(ConstraintSystem &cs, Type baseTy, Type rootTy,
          ConstraintLocator *locator);
};

/// Diagnose situations when solver used old (backward scan) rule
/// to match trailing closure to a parameter.
///
/// \code
/// func multiple_trailing_with_defaults(
///   duration: Int,
///   animations: (() -> Void)? = nil,
///   completion: (() -> Void)? = nil) {}
///
/// multiple_trailing_with_defaults(duration: 42) {} // picks `completion:`
/// \endcode
class SpecifyLabelToAssociateTrailingClosure final : public ConstraintFix {
  SpecifyLabelToAssociateTrailingClosure(ConstraintSystem &cs,
                                         ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::SpecifyLabelToAssociateTrailingClosure,
                      locator, /*isWarning=*/true) {}

public:
  std::string getName() const override {
    return "specify a label to associate trailing closure with parameter";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static SpecifyLabelToAssociateTrailingClosure *
  create(ConstraintSystem &cs, ConstraintLocator *locator);
};

/// Diagnose situations where we have a key path with no components.
///
/// \code
/// let _ : KeyPath<A, B> = \A
/// \endcode
class AllowKeyPathWithoutComponents final : public ConstraintFix {
  AllowKeyPathWithoutComponents(ConstraintSystem &cs,
                                ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowKeyPathWithoutComponents, locator) {}

public:
  std::string getName() const override { return "key path missing component"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowKeyPathWithoutComponents *create(ConstraintSystem &cs,
                                               ConstraintLocator *locator);
};

class IgnoreInvalidResultBuilderBody : public ConstraintFix {
protected:
  IgnoreInvalidResultBuilderBody(ConstraintSystem &cs,
                                 ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::IgnoreInvalidResultBuilderBody, locator) {}

public:
  std::string getName() const override {
    return "ignore invalid result builder body";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static IgnoreInvalidResultBuilderBody *create(ConstraintSystem &cs,
                                                ConstraintLocator *locator);
};

class IgnoreResultBuilderWithReturnStmts final
    : public IgnoreInvalidResultBuilderBody {
  Type BuilderType;

  IgnoreResultBuilderWithReturnStmts(ConstraintSystem &cs, Type builderTy,
                                     ConstraintLocator *locator)
      : IgnoreInvalidResultBuilderBody(cs, locator), BuilderType(builderTy) {}

public:
  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static IgnoreResultBuilderWithReturnStmts *
  create(ConstraintSystem &cs, Type builderTy, ConstraintLocator *locator);
};

class SpecifyContextualTypeForNil final : public ConstraintFix {
  SpecifyContextualTypeForNil(ConstraintSystem &cs,
                              ConstraintLocator *locator)
    : ConstraintFix(cs, FixKind::SpecifyContextualTypeForNil, locator) {}

public:
  std::string getName() const override {
    return "specify contextual type to resolve `nil` literal";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static SpecifyContextualTypeForNil *create(ConstraintSystem & cs,
                                             ConstraintLocator * locator);
};

class SpecifyTypeForPlaceholder final : public ConstraintFix {
  SpecifyTypeForPlaceholder(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::SpecifyTypeForPlaceholder, locator) {}

public:
  std::string getName() const override {
    return "specify type for placeholder";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static SpecifyTypeForPlaceholder *create(ConstraintSystem &cs,
                                           ConstraintLocator *locator);
};

class AllowRefToInvalidDecl final : public ConstraintFix {
  AllowRefToInvalidDecl(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowRefToInvalidDecl, locator) {}

public:
  std::string getName() const override {
    return "ignore invalid declaration reference";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static AllowRefToInvalidDecl *create(ConstraintSystem &cs,
                                       ConstraintLocator *locator);
};

/// Diagnose if the base type is optional, we're referring to a nominal
/// type member via the dot syntax and the member name matches
/// Optional<T>.{member} or a .none member inferred as non-optional static
/// member e.g. let _ : Foo? = .none where Foo has a static member none.
class SpecifyBaseTypeForOptionalUnresolvedMember final : public ConstraintFix {
  SpecifyBaseTypeForOptionalUnresolvedMember(ConstraintSystem &cs,
                                             DeclNameRef memberName,
                                             ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::SpecifyBaseTypeForOptionalUnresolvedMember,
                      locator, /*isWarning=*/true),
        MemberName(memberName) {}
  DeclNameRef MemberName;

public:
  std::string getName() const override {
    const auto name = MemberName.getBaseName();
    return "specify unresolved member optional base type explicitly '" +
           name.userFacingName().str() + "'";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static SpecifyBaseTypeForOptionalUnresolvedMember *
  attempt(ConstraintSystem &cs, ConstraintKind kind, Type baseTy,
          DeclNameRef memberName, FunctionRefKind functionRefKind,
          MemberLookupResult result, ConstraintLocator *locator);
};

class CheckedCastContextualMismatchWarning : public ContextualMismatch {
protected:
  CheckedCastContextualMismatchWarning(ConstraintSystem &cs, FixKind fixKind,
                                       Type fromType, Type toType,
                                       CheckedCastKind kind,
                                       ConstraintLocator *locator)
      : ContextualMismatch(cs, fixKind, fromType, toType, locator,
                           /*isWarning*/ true),
        CastKind(kind) {}
  CheckedCastKind CastKind;
};

class AllowCheckedCastCoercibleOptionalType final
    : public CheckedCastContextualMismatchWarning {
  AllowCheckedCastCoercibleOptionalType(ConstraintSystem &cs, Type fromType,
                                        Type toType, CheckedCastKind kind,
                                        ConstraintLocator *locator)
      : CheckedCastContextualMismatchWarning(
            cs, FixKind::AllowCheckedCastCoercibleOptionalType, fromType,
            toType, kind, locator) {}

public:
  std::string getName() const override {
    return "checked cast coercible optional";
  }
  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowCheckedCastCoercibleOptionalType *
  create(ConstraintSystem &cs, Type fromType, Type toType, CheckedCastKind kind,
         ConstraintLocator *locator);
};

class AllowAlwaysSucceedCheckedCast final
    : public CheckedCastContextualMismatchWarning {
  AllowAlwaysSucceedCheckedCast(ConstraintSystem &cs, Type fromType,
                                Type toType, CheckedCastKind kind,
                                ConstraintLocator *locator)
      : CheckedCastContextualMismatchWarning(
            cs, FixKind::AllowUnsupportedRuntimeCheckedCast, fromType, toType,
            kind, locator) {}

public:
  std::string getName() const override { return "checked cast always succeed"; }
  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowAlwaysSucceedCheckedCast *create(ConstraintSystem &cs,
                                               Type fromType, Type toType,
                                               CheckedCastKind kind,
                                               ConstraintLocator *locator);
};

class AllowUnsupportedRuntimeCheckedCast final
    : public CheckedCastContextualMismatchWarning {
  AllowUnsupportedRuntimeCheckedCast(ConstraintSystem &cs, Type fromType,
                                     Type toType, CheckedCastKind kind,
                                     ConstraintLocator *locator)
      : CheckedCastContextualMismatchWarning(
            cs, FixKind::AllowUnsupportedRuntimeCheckedCast, fromType, toType,
            kind, locator) {}

public:
  std::string getName() const override {
    return "runtime unsupported checked cast";
  }
  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static bool runtimeSupportedFunctionTypeCast(FunctionType *fnFromType,
                                               FunctionType *fnToType);

  static AllowUnsupportedRuntimeCheckedCast *
  attempt(ConstraintSystem &cs, Type fromType, Type toType,
          CheckedCastKind kind, ConstraintLocator *locator);
};

class AllowInvalidStaticMemberRefOnProtocolMetatype final
    : public ConstraintFix {
  AllowInvalidStaticMemberRefOnProtocolMetatype(ConstraintSystem &cs,
                                                ConstraintLocator *locator)
      : ConstraintFix(cs,
                      FixKind::AllowInvalidStaticMemberRefOnProtocolMetatype,
                      locator) {}

  public:
  std::string getName() const override {
    return "allow invalid static member reference on a protocol metatype";
  }

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowInvalidStaticMemberRefOnProtocolMetatype *
  create(ConstraintSystem &cs, ConstraintLocator *locator);
};

} // end namespace constraints
} // end namespace swift

namespace llvm {
  template <>
  struct DenseMapInfo<swift::constraints::FixKind> {
    using FixKind = swift::constraints::FixKind;
    static inline FixKind getEmptyKey() {
      return static_cast<FixKind>(0);
    }
    static inline FixKind getTombstoneKey() {
      return static_cast<FixKind>(1);
    }
    static unsigned getHashValue(FixKind kind) {
      return static_cast<unsigned>(kind);
    }
    static bool isEqual(FixKind lhs, FixKind rhs) {
      return lhs == rhs;
    }
  };
}

#endif // SWIFT_SEMA_CSFIX_H
