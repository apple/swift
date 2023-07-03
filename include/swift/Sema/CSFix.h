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
#include "swift/Sema/FixBehavior.h"
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
enum ScoreKind: unsigned int;
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

  /// Skip same-shape generic requirement constraint,
  /// and assume that pack types have the same shape.
  SkipSameShapeRequirement,

  /// Skip superclass generic requirement constraint,
  /// and assume that types are related.
  SkipSuperclassRequirement,

  /// Fix up one of the sides of conversion to make it seem
  /// like the types are aligned.
  ContextualMismatch,

  /// Fix up the generic arguments of two types so they match each other.
  GenericArgumentsMismatch,

  /// Fix up @autoclosure argument to the @autoclosure parameter,
  /// to for a call to be able to forward it properly, since
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

  /// Object literal type couldn't be inferred because the module where
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

  /// Specify key path root type when it cannot be inferred from context.
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

  /// Ignore result builder body if it has `return` statements.
  IgnoreResultBuilderWithReturnStmts,

  /// Ignore `ErrorExpr` or `ErrorType` during pre-check.
  IgnoreInvalidASTNode,

  /// Ignore a named or `_` pattern whose type we couldn't infer.
  /// This issue should already have been diagnosed elsewhere.
  IgnoreUnresolvedPatternVar,

  /// Ignore a nested UnresolvedPatternExpr in an ExprPattern, which is invalid.
  IgnoreInvalidPatternInExpr,

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

  /// Warn about runtime checked cast that is statically known to always
  /// succeed.
  AllowNoopCheckedCast,

  /// Warn about special runtime case where statically known
  /// checked cast from existentials to CFType always succeed.
  AllowNoopExistentialToCFTypeCheckedCast,

  /// Allow a runtime checked cast where at compile time the from is
  /// convertible, but runtime does not support such conversions. e.g.
  /// function type casts.
  AllowUnsupportedRuntimeCheckedCast,

  /// Allow a runtime checked cast where it is known at compile time
  /// always fails.
  AllowCheckedCastToUnrelated,

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
  SpecifyTypeForPlaceholder,

  /// Allow Swift -> C pointer conversion in an argument position
  /// of a Swift function.
  AllowSwiftToCPointerConversion,

  /// Allow `weak` declarations to be bound to a non-optional type.
  AllowNonOptionalWeak,

  /// Fix conversion from non-Sendable to Sendable by adding explicit
  /// @Sendable attribute to the source function.
  AddSendableAttribute,

  /// Fix conversion from throwing to non-throwing by removing explicit
  /// `throws` attribute from the source function.
  DropThrowsAttribute,

  /// Fix conversion from async to sync function by removing explicit
  /// `async` attribute from the source function.
  DropAsyncAttribute,

  /// Allow invalid pointer conversions for autoclosure result types as if the
  /// pointer type is a function parameter rather than an autoclosure result.
  AllowAutoClosurePointerConversion,

  /// Ignore externally imposed type.
  IgnoreContextualType,

  /// Ignore a type imposed by an assignment destination e.g. `let x: Int = ...`
  IgnoreAssignmentDestinationType,

  /// Allow argument-to-parameter subtyping even when parameter type
  /// is marked as `inout`.
  AllowConversionThroughInOut,

  /// Ignore either capability (read/write) or type mismatch in conversion
  /// between two key path types.
  IgnoreKeyPathContextualMismatch,

  /// Ignore a type mismatch between deduced element type and externally
  /// imposed one.
  IgnoreCollectionElementContextualMismatch,

  /// Produce a warning for a tuple label mismatch.
  AllowTupleLabelMismatch,

  /// Allow an associated value mismatch for an enum element pattern.
  AllowAssociatedValueMismatch,

  /// Produce an error for not getting a compile-time constant
  NotCompileTimeConst,

  /// Ignore a type mismatch while trying to infer generic parameter type
  /// from default expression.
  IgnoreDefaultExprTypeMismatch,

  /// Coerce a result type of a call to a particular existential type
  /// by adding `as any <#Type#>`.
  AddExplicitExistentialCoercion,

  /// For example `.a(let x), .b(let x)` where `x` gets bound to different
  /// types.
  RenameConflictingPatternVariables,

  /// Macro without leading #.
  MacroMissingPound,

  /// Allow function type actor mismatch e.g. `@MainActor () -> Void`
  /// vs.`@OtherActor () -> Void`
  AllowGlobalActorMismatch,

  /// Produce an error about a type that must be Copyable
  MustBeCopyable,

  /// Allow 'each' applied to a non-pack type.
  AllowInvalidPackElement,

  /// Allow pack references outside of pack expansions.
  AllowInvalidPackReference,

  /// Allow pack expansion expressions in a context that does not support them.
  AllowInvalidPackExpansion,

  /// Allow a pack expansion parameter of N elements to be matched
  /// with a single tuple literal argument of the same arity.
  DestructureTupleToMatchPackExpansionParameter,

  /// Allow value pack expansion without pack references.
  AllowValueExpansionWithoutPackReferences,

  /// Ignore missing 'each' keyword before value pack reference.
  IgnoreMissingEachKeyword,

  /// Ignore the fact that member couldn't be referenced within init accessor
  /// because its name doesn't appear in 'initializes' or 'accesses' attributes.
  AllowInvalidMemberReferenceInInitAccessor,

  /// Ignore an attempt to specialize non-generic type.
  AllowConcreteTypeSpecialization,

  /// Ignore situations when provided number of generic arguments didn't match
  /// expected number of parameters.
  IgnoreGenericSpecializationArityMismatch,
};

class ConstraintFix {
  ConstraintSystem &CS;
  FixKind Kind;
  ConstraintLocator *Locator;

public:
  /// The behavior limit to apply to the diagnostics emitted.
  const FixBehavior fixBehavior;

  ConstraintFix(ConstraintSystem &cs, FixKind kind, ConstraintLocator *locator,
                FixBehavior fixBehavior = FixBehavior::Error)
      : CS(cs), Kind(kind), Locator(locator), fixBehavior(fixBehavior) {}

  virtual ~ConstraintFix();

  template <typename Fix>
  const Fix *getAs() const {
    return Fix::classof(this) ? static_cast<const Fix *>(this) : nullptr;
  }

  template <typename Fix>
  const Fix *castTo() const {
    assert(Fix::classof(this));
    return static_cast<const Fix *>(this);
  }

  FixKind getKind() const { return Kind; }

  /// Whether this fix fatal for the constraint solver, meaning that it cannot
  /// produce a usable type-checked AST.
  bool isFatal() const {
    switch (fixBehavior) {
    case FixBehavior::AlwaysWarning:
    case FixBehavior::DowngradeToWarning:
    case FixBehavior::Suppress:
      return false;

    case FixBehavior::Error:
      return true;
    }
  }

  /// Determine the impact of this fix on the solution score, if any.
  llvm::Optional<ScoreKind> impact() const;

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
  /// any simplification attempts.
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

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::TreatRValueAsLValue;
  }
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

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override;

  static RelabelArguments *create(ConstraintSystem &cs,
                                  llvm::ArrayRef<Identifier> correctLabels,
                                  ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::RelabelArguments;
  }

private:
  MutableArrayRef<Identifier> getLabelsBuffer() {
    return {getTrailingObjects<Identifier>(), NumLabels};
  }
};

class RequirementFix : public ConstraintFix {
protected:
  Type LHS;
  Type RHS;

  RequirementFix(ConstraintSystem &cs, FixKind kind, Type lhs, Type rhs,
                 ConstraintLocator *locator)
      : ConstraintFix(cs, kind, locator), LHS(lhs), RHS(rhs) {}

public:
  std::string getName() const override = 0;

  Type lhsType() const { return LHS; }
  Type rhsType() const { return RHS; }

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override;

  bool diagnose(const Solution &solution,
                bool asNote = false) const override = 0;
};

/// Add a new conformance to the type to satisfy a requirement.
class MissingConformance final : public RequirementFix {
  // Determines whether given protocol type comes from the context e.g.
  // assignment destination or argument comparison.
  bool IsContextual;

  MissingConformance(ConstraintSystem &cs, bool isContextual, Type type,
                     Type protocolType, ConstraintLocator *locator)
      : RequirementFix(cs, FixKind::AddConformance, type, protocolType,
                       locator),
        IsContextual(isContextual) {}

public:
  std::string getName() const override {
    return "add missing protocol conformance";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static MissingConformance *forRequirement(ConstraintSystem &cs, Type type,
                                            Type protocolType,
                                            ConstraintLocator *locator);

  static MissingConformance *forContextual(ConstraintSystem &cs, Type type,
                                           Type protocolType,
                                           ConstraintLocator *locator);

  Type getNonConformingType() const { return LHS; }

  Type getProtocolType() const { return RHS; }

  bool isEqual(const ConstraintFix *other) const;

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AddConformance;
  }
};

/// Skip same-type generic requirement constraint,
/// and assume that types are equal.
class SkipSameTypeRequirement final : public RequirementFix {
  SkipSameTypeRequirement(ConstraintSystem &cs, Type lhs, Type rhs,
                          ConstraintLocator *locator)
      : RequirementFix(cs, FixKind::SkipSameTypeRequirement, lhs, rhs,
                       locator) {}

public:
  std::string getName() const override {
    return "skip same-type generic requirement";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static SkipSameTypeRequirement *create(ConstraintSystem &cs, Type lhs,
                                         Type rhs, ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::SkipSameTypeRequirement;
  }
};

/// Skip a same-shape requirement between two type packs.
///
/// A same shape requirement can be inferred from a generic requirement,
/// or from a pack expansion expression.
class SkipSameShapeRequirement final : public RequirementFix {
  SkipSameShapeRequirement(ConstraintSystem &cs, Type lhs, Type rhs,
                           ConstraintLocator *locator)
      : RequirementFix(cs, FixKind::SkipSameShapeRequirement, lhs, rhs,
                       locator) {}

public:
  std::string getName() const override {
    return "skip same-shape requirement";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static SkipSameShapeRequirement *create(ConstraintSystem &cs, Type lhs,
                                          Type rhs, ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::SkipSameTypeRequirement;
  }
};

/// Skip 'superclass' generic requirement constraint,
/// and assume that types are equal.
class SkipSuperclassRequirement final : public RequirementFix {
  SkipSuperclassRequirement(ConstraintSystem &cs, Type lhs, Type rhs,
                            ConstraintLocator *locator)
      : RequirementFix(cs, FixKind::SkipSuperclassRequirement, lhs, rhs,
                       locator) {}

public:
  std::string getName() const override {
    return "skip superclass generic requirement";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  Type subclassType() { return LHS; }
  Type superclassType() { return RHS; }

  static SkipSuperclassRequirement *
  create(ConstraintSystem &cs, Type lhs, Type rhs, ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::SkipSuperclassRequirement;
  }
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

  ContextualMismatch(ConstraintSystem &cs, Type lhs, Type rhs,
                     ConstraintLocator *locator,
                     FixBehavior fixBehavior)
      : ConstraintFix(cs, FixKind::ContextualMismatch, locator, fixBehavior),
        LHS(lhs), RHS(rhs) {}

protected:
  ContextualMismatch(ConstraintSystem &cs, FixKind kind, Type lhs, Type rhs,
                     ConstraintLocator *locator,
                     FixBehavior fixBehavior = FixBehavior::Error)
      : ConstraintFix(cs, kind, locator, fixBehavior), LHS(lhs), RHS(rhs) {}

public:
  std::string getName() const override { return "fix contextual mismatch"; }

  Type getFromType() const { return LHS; }
  Type getToType() const { return RHS; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool coalesceAndDiagnose(const Solution &solution,
                           ArrayRef<ConstraintFix *> secondaryFixes,
                           bool asNote = false) const override {
    // If the from type or to type is a placeholer type that corresponds to an
    // ErrorExpr, the issue has already been diagnosed. There's no need to
    // produce another diagnostic for the contextual mismatch complainting that
    // a type is not convertible to a placeholder type.
    if (auto fromPlaceholder = getFromType()->getAs<PlaceholderType>()) {
      if (fromPlaceholder->getOriginator().is<ErrorExpr *>()) {
        return true;
      }
    }
    if (auto toPlaceholder = getToType()->getAs<PlaceholderType>()) {
      if (toPlaceholder->getOriginator().is<ErrorExpr *>()) {
        return true;
      }
    }
    return ConstraintFix::coalesceAndDiagnose(solution, secondaryFixes, asNote);
  }

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override;

  static ContextualMismatch *create(ConstraintSystem &cs, Type lhs, Type rhs,
                                    ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::ContextualMismatch;
  }
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
  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static TreatArrayLiteralAsDictionary *attempt(ConstraintSystem &cs,
                                                Type dictionaryTy, Type arrayTy,
                                                ConstraintLocator *loc);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::TreatArrayLiteralAsDictionary;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowWrappedValueMismatch;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::ExplicitlyEscaping;
  }
};

/// Mark function type as being part of a global actor.
class MarkGlobalActorFunction final : public ContextualMismatch {
  MarkGlobalActorFunction(ConstraintSystem &cs, Type lhs, Type rhs,
                          ConstraintLocator *locator,
                          FixBehavior fixBehavior)
      : ContextualMismatch(cs, FixKind::MarkGlobalActorFunction, lhs, rhs,
                           locator, fixBehavior) {
  }

public:
  std::string getName() const override { return "add global actor"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static MarkGlobalActorFunction *create(ConstraintSystem &cs, Type lhs,
                                         Type rhs, ConstraintLocator *locator,
                                         FixBehavior fixBehavior);

  /// Try to apply this fix to the given types.
  ///
  /// \returns \c true if the fix cannot be applied and the solver must fail,
  /// or \c false if the fix has been applied and the solver can continue.
  static bool attempt(ConstraintSystem &cs,
                      ConstraintKind constraintKind,
                      FunctionType *fromType,
                      FunctionType *toType,
                      ConstraintLocatorBuilder locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::MarkGlobalActorFunction;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::ForceOptional;
  }
};

/// This is a contextual mismatch between @Sendable and non-@Sendable
/// function types, repair it by adding @Sendable attribute.
class AddSendableAttribute final : public ContextualMismatch {
  AddSendableAttribute(ConstraintSystem &cs, FunctionType *fromType,
                       FunctionType *toType, ConstraintLocator *locator,
                       FixBehavior fixBehavior)
      : ContextualMismatch(cs, FixKind::AddSendableAttribute, fromType, toType,
                           locator, fixBehavior) {
    assert(fromType->isSendable() != toType->isSendable());
  }

public:
  std::string getName() const override { return "add '@Sendable' attribute"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AddSendableAttribute *create(ConstraintSystem &cs,
                                      FunctionType *fromType,
                                      FunctionType *toType,
                                      ConstraintLocator *locator,
                                      FixBehavior fixBehavior);

  /// Try to apply this fix to the given types.
  ///
  /// \returns \c true if the fix cannot be applied and the solver must fail,
  /// or \c false if the fix has been applied and the solver can continue.
  static bool attempt(ConstraintSystem &cs,
                      ConstraintKind constraintKind,
                      FunctionType *fromType,
                      FunctionType *toType,
                      ConstraintLocatorBuilder locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AddSendableAttribute;
  }
};

/// This is a contextual mismatch between throwing and non-throwing
/// function types, repair it by dropping `throws` attribute.
class DropThrowsAttribute final : public ContextualMismatch {
  DropThrowsAttribute(ConstraintSystem &cs, FunctionType *fromType,
                      FunctionType *toType, ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::DropThrowsAttribute, fromType, toType,
                           locator) {
    assert(fromType->isThrowing() != toType->isThrowing());
  }

public:
  std::string getName() const override { return "drop 'throws' attribute"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static DropThrowsAttribute *create(ConstraintSystem &cs,
                                     FunctionType *fromType,
                                     FunctionType *toType,
                                     ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::DropThrowsAttribute;
  }
};

/// This is a contextual mismatch between async and non-async
/// function types, repair it by dropping `async` attribute.
class DropAsyncAttribute final : public ContextualMismatch {
  DropAsyncAttribute(ConstraintSystem &cs, FunctionType *fromType,
                     FunctionType *toType, ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::DropAsyncAttribute, fromType, toType,
                           locator) {
    assert(fromType->isAsync() != toType->isAsync());
  }

public:
  std::string getName() const override { return "drop 'async' attribute"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static DropAsyncAttribute *create(ConstraintSystem &cs,
                                    FunctionType *fromType,
                                    FunctionType *toType,
                                    ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::DropAsyncAttribute;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::ForceDowncast;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AddressOf;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::RemoveAddressOf;
  }
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

  bool coalesceAndDiagnose(const Solution &solution,
                           ArrayRef<ConstraintFix *> secondaryFixes,
                           bool asNote = false) const override;

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static GenericArgumentsMismatch *create(ConstraintSystem &cs, Type actual,
                                          Type required,
                                          llvm::ArrayRef<unsigned> mismatches,
                                          ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::GenericArgumentsMismatch;
  }

private:
  bool diagnose(const Solution &solution, ArrayRef<unsigned> mismatches,
                bool asNote = false) const;

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
      : ContextualMismatch(cs, FixKind::IgnoreKeyPathContextualMismatch, lhs,
                           rhs, locator) {}

public:
  std::string getName() const override {
    return "fix key path contextual mismatch";
  }

  static KeyPathContextualMismatch *
  create(ConstraintSystem &cs, Type lhs, Type rhs, ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::IgnoreKeyPathContextualMismatch;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AutoClosureForwarding;
  }
};

class AllowAutoClosurePointerConversion final : public ContextualMismatch {
  AllowAutoClosurePointerConversion(ConstraintSystem &cs, Type pointeeType,
                                    Type pointerType,
                                    ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::AllowAutoClosurePointerConversion,
                           pointeeType, pointerType, locator) {}

public:
  std::string getName() const override {
    return "allow pointer conversion for autoclosure result type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowAutoClosurePointerConversion *create(ConstraintSystem &cs,
                                                   Type pointeeType,
                                                   Type pointerType,
                                                   ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowAutoClosurePointerConversion;
  }
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

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static RemoveUnwrap *create(ConstraintSystem &cs, Type baseType,
                              ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::RemoveUnwrap;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::InsertCall;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::UsePropertyWrapper;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::UseWrappedValue;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowInvalidPropertyWrapperType;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::RemoveProjectedValueArgument;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::UseSubscriptOperator;
  }
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

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override;
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowMemberRefOnExistential;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowTypeOrInstanceMember;
  }
};

class AllowInvalidPartialApplication final : public ConstraintFix {
  bool isWarning;

  AllowInvalidPartialApplication(bool isWarning, ConstraintSystem &cs,
                                 ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowInvalidPartialApplication, locator,
                      isWarning ? FixBehavior::AlwaysWarning
                                : FixBehavior::Error),
        isWarning(isWarning) {}

public:
  std::string getName() const override {
    return "allow partially applied 'mutating' method";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowInvalidPartialApplication *create(bool isWarning,
                                                ConstraintSystem &cs,
                                                ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowInvalidPartialApplication;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowInvalidInitRef;
  }

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
  llvm::Optional<unsigned> Index;

  AllowTupleTypeMismatch(ConstraintSystem &cs, Type lhs, Type rhs,
                         ConstraintLocator *locator,
                         llvm::Optional<unsigned> index)
      : ContextualMismatch(cs, FixKind::AllowTupleTypeMismatch, lhs, rhs,
                           locator),
        Index(index) {}

public:
  static AllowTupleTypeMismatch *
  create(ConstraintSystem &cs, Type lhs, Type rhs, ConstraintLocator *locator,
         llvm::Optional<unsigned> index = llvm::None);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowTupleTypeMismatch;
  }

  std::string getName() const override {
    return "fix tuple mismatches in type and arity";
  }

  bool isElementMismatch() const {
    return Index.has_value();
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowMutatingMemberOnRValueBase;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowClosureParameterDestructuring;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::RemoveExtraneousArguments;
  }

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

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static AllowInaccessibleMember *create(ConstraintSystem &cs, Type baseType,
                                         ValueDecl *member, DeclNameRef name,
                                         ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowInaccessibleMember;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowAnyObjectKeyPathRoot;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowMultiArgFuncKeyPathMismatch;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::TreatKeyPathSubscriptIndexAsHashable;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::RemoveReturn;
  }
};

class NotCompileTimeConst final : public ContextualMismatch {
  NotCompileTimeConst(ConstraintSystem &cs, Type paramTy, ConstraintLocator *locator);

public:
  std::string getName() const override { return "replace with an literal"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static NotCompileTimeConst *create(ConstraintSystem &cs,
                                     Type paramTy,
                                     ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::NotCompileTimeConst;
  }
};

/// Describes the reason why the type must be copyable
struct NoncopyableMatchFailure {
  enum Kind {
    CopyableConstraint,
    ExistentialCast,
  };

private:
  Kind reason;
  union {
    Type type;
  };

  NoncopyableMatchFailure(Kind reason, Type type)
      : reason(reason), type(type) {}

public:
  Kind getKind() const { return reason; }

  Type getType() const {
    switch (reason) {
    case ExistentialCast:
      return type;

    case CopyableConstraint:
      llvm_unreachable("no type payload");
    };
  }

  static NoncopyableMatchFailure forCopyableConstraint() {
    return NoncopyableMatchFailure(CopyableConstraint, Type());
  }

  static NoncopyableMatchFailure forExistentialCast(Type existential) {
    assert(existential->isAnyExistentialType());
    return NoncopyableMatchFailure(ExistentialCast, existential);
  }
};

class MustBeCopyable final : public ConstraintFix {
  Type noncopyableTy;
  NoncopyableMatchFailure failure;

  MustBeCopyable(ConstraintSystem &cs,
                 Type noncopyableTy,
                 NoncopyableMatchFailure failure,
                 ConstraintLocator *locator);

public:
  std::string getName() const override { return "remove move-only from type"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override;

  static MustBeCopyable *create(ConstraintSystem &cs,
                             Type noncopyableTy,
                             NoncopyableMatchFailure failure,
                             ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::MustBeCopyable;
  }
};

class AllowInvalidPackElement final : public ConstraintFix {
  Type packElementType;

  AllowInvalidPackElement(ConstraintSystem &cs, Type packElementType,
                          ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowInvalidPackElement, locator),
        packElementType(packElementType) {}

public:
  std::string getName() const override {
    return "allow concrete pack element";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowInvalidPackElement *create(ConstraintSystem &cs,
                                         Type packElementType,
                                         ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowInvalidPackElement;
  }
};

class AllowInvalidPackReference final : public ConstraintFix {
  Type packType;

  AllowInvalidPackReference(ConstraintSystem &cs, Type packType,
                            ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowInvalidPackReference, locator),
        packType(packType) {}

public:
  std::string getName() const override {
    return "allow pack outside pack expansion";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowInvalidPackReference *create(ConstraintSystem &cs,
                                           Type packType,
                                           ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowInvalidPackReference;
  }
};

class AllowInvalidPackExpansion final : public ConstraintFix {
  AllowInvalidPackExpansion(ConstraintSystem &cs,
                            ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowInvalidPackExpansion, locator) {}

public:
  std::string getName() const override {
    return "allow invalid pack expansion";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowInvalidPackExpansion *create(ConstraintSystem &cs,
                                           ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowInvalidPackExpansion;
  }
};

class CollectionElementContextualMismatch final
    : public ContextualMismatch,
      private llvm::TrailingObjects<CollectionElementContextualMismatch,
                                    Expr *> {
  friend TrailingObjects;

  unsigned NumElements;

  CollectionElementContextualMismatch(ConstraintSystem &cs,
                                      ArrayRef<Expr *> affectedElements,
                                      Type srcType, Type dstType,
                                      ConstraintLocator *locator)
      : ContextualMismatch(cs,
                           FixKind::IgnoreCollectionElementContextualMismatch,
                           srcType, dstType, locator),
        NumElements(affectedElements.size()) {
    std::uninitialized_copy(affectedElements.begin(), affectedElements.end(),
                            getElementBuffer().begin());
  }

public:
  std::string getName() const override {
    return "fix collection element contextual mismatch";
  }

  ArrayRef<Expr *> getElements() const {
    return {getTrailingObjects<Expr *>(), NumElements};
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static CollectionElementContextualMismatch *
  create(ConstraintSystem &cs, Type srcType, Type dstType,
         ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::IgnoreCollectionElementContextualMismatch;
  }

private:
  MutableArrayRef<Expr *> getElementBuffer() {
    return {getTrailingObjects<Expr *>(), NumElements};
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::SkipUnhandledConstructInResultBuilder;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowTupleSplatForSingleParameter;
  }
};

class IgnoreContextualType : public ContextualMismatch {
  IgnoreContextualType(ConstraintSystem &cs, Type resultTy, Type specifiedTy,
                       ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::IgnoreContextualType, resultTy,
                           specifiedTy, locator) {}

public:
  std::string getName() const override {
    return "ignore specified contextual type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static IgnoreContextualType *create(ConstraintSystem &cs, Type resultTy,
                                      Type specifiedTy,
                                      ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::IgnoreContextualType;
  }
};

class IgnoreAssignmentDestinationType final : public ContextualMismatch {
  IgnoreAssignmentDestinationType(ConstraintSystem &cs, Type sourceTy,
                                  Type destTy, ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::IgnoreAssignmentDestinationType,
                           sourceTy, destTy, locator) {}

public:
  std::string getName() const override {
    return "ignore type of the assignment destination";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override;

  static IgnoreAssignmentDestinationType *create(ConstraintSystem &cs,
                                                 Type sourceTy, Type destTy,
                                                 ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::IgnoreAssignmentDestinationType;
  }
};

/// If this is an argument-to-parameter conversion which is associated with
/// `inout` parameter, subtyping is not permitted, types have to
/// be identical.
class AllowInOutConversion final : public ContextualMismatch {
  AllowInOutConversion(ConstraintSystem &cs, Type argType, Type paramType,
                       ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::AllowConversionThroughInOut, argType,
                           paramType, locator) {}

public:
  std::string getName() const override {
    return "allow conversions between argument/parameter marked as `inout`";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowInOutConversion *create(ConstraintSystem &cs, Type argType,
                                      Type paramType,
                                      ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowConversionThroughInOut;
  }
};

class AllowArgumentMismatch : public ContextualMismatch {
protected:
  AllowArgumentMismatch(ConstraintSystem &cs, Type argType, Type paramType,
                        ConstraintLocator *locator)
      : AllowArgumentMismatch(cs, FixKind::AllowArgumentTypeMismatch, argType,
                              paramType, locator) {}

  AllowArgumentMismatch(ConstraintSystem &cs, FixKind kind, Type argType,
                        Type paramType, ConstraintLocator *locator,
                        FixBehavior fixBehavior = FixBehavior::Error)
      : ContextualMismatch(
            cs, kind, argType, paramType, locator, fixBehavior) {}

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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::ExpandArrayIntoVarargs;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::ExplicitlyConstructRawRepresentable;
  }
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

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static UseRawValue *create(ConstraintSystem &cs, Type rawReprType,
                             Type expectedType, ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::UseRawValue;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::CoerceToCheckedCast;
  }
};

class RemoveInvalidCall final : public ConstraintFix {
  RemoveInvalidCall(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::RemoveCall, locator) {}

public:
  std::string getName() const override {
    return "remove extraneous call from value of non-function type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static RemoveInvalidCall *create(ConstraintSystem &cs,
                                   ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::RemoveCall;
  }
};

class TreatEphemeralAsNonEphemeral final : public AllowArgumentMismatch {
  ConversionRestrictionKind ConversionKind;

  TreatEphemeralAsNonEphemeral(ConstraintSystem &cs, ConstraintLocator *locator,
                               Type srcType, Type dstType,
                               ConversionRestrictionKind conversionKind,
                               FixBehavior fixBehavior)
      : AllowArgumentMismatch(cs, FixKind::TreatEphemeralAsNonEphemeral,
                              srcType, dstType, locator, fixBehavior),
        ConversionKind(conversionKind) {}

public:
  ConversionRestrictionKind getConversionKind() const { return ConversionKind; }
  std::string getName() const override;

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static TreatEphemeralAsNonEphemeral *
  create(ConstraintSystem &cs, ConstraintLocator *locator, Type srcType,
         Type dstType, ConversionRestrictionKind conversionKind,
         bool downgradeToWarning);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::TreatEphemeralAsNonEphemeral;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::SpecifyBaseTypeForContextualMember;
  }
};

class SpecifyClosureParameterType final : public ConstraintFix {
  SpecifyClosureParameterType(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::SpecifyClosureParameterType, locator) {}

public:
  std::string getName() const override;

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static SpecifyClosureParameterType *create(ConstraintSystem &cs,
                                             ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::SpecifyClosureParameterType;
  }
};

class SpecifyClosureReturnType final : public ConstraintFix {
  SpecifyClosureReturnType(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::SpecifyClosureReturnType, locator) {}

public:
  std::string getName() const override {
    return "specify closure return type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static SpecifyClosureReturnType *create(ConstraintSystem &cs,
                                          ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::SpecifyClosureReturnType;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::SpecifyObjectLiteralTypeImport;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AddQualifierToAccessTopLevelName;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowNonClassTypeToConvertToAnyObject;
  }
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
                           toType, locator, FixBehavior::AlwaysWarning) {}

public:
  std::string getName() const override {
    return "allow coercion to be treated as a force-cast";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowCoercionToForceCast *create(ConstraintSystem &cs, Type fromType,
                                          Type toType,
                                          ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowCoercionToForceCast;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowKeyPathRootTypeMismatch;
  }
};

class SpecifyKeyPathRootType final : public ConstraintFix {
    SpecifyKeyPathRootType(ConstraintSystem &cs, ConstraintLocator *locator)
        : ConstraintFix(cs, FixKind::SpecifyKeyPathRootType, locator) {}

public:
  std::string getName() const override { return "specify key path root type"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static SpecifyKeyPathRootType *create(ConstraintSystem &cs,
                                        ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::SpecifyKeyPathRootType;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::UnwrapOptionalBaseKeyPathApplication;
  }
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
                      locator, FixBehavior::AlwaysWarning) {}

public:
  std::string getName() const override {
    return "specify a label to associate trailing closure with parameter";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static SpecifyLabelToAssociateTrailingClosure *
  create(ConstraintSystem &cs, ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::SpecifyLabelToAssociateTrailingClosure;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowKeyPathWithoutComponents;
  }
};

class IgnoreInvalidResultBuilderBody : public ConstraintFix {
  IgnoreInvalidResultBuilderBody(ConstraintSystem &cs,
                                 ConstraintLocator *locator)
      : IgnoreInvalidResultBuilderBody(
            cs, FixKind::IgnoreInvalidResultBuilderBody, locator) {}

protected:
  IgnoreInvalidResultBuilderBody(ConstraintSystem &cs, FixKind kind,
                                 ConstraintLocator *locator)
      : ConstraintFix(cs, kind, locator) {}

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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::IgnoreInvalidResultBuilderBody;
  }
};

class IgnoreResultBuilderWithReturnStmts final
    : public IgnoreInvalidResultBuilderBody {
  Type BuilderType;

  IgnoreResultBuilderWithReturnStmts(ConstraintSystem &cs, Type builderTy,
                                     ConstraintLocator *locator)
      : IgnoreInvalidResultBuilderBody(
            cs, FixKind::IgnoreResultBuilderWithReturnStmts, locator),
        BuilderType(builderTy) {}

public:
  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static IgnoreResultBuilderWithReturnStmts *
  create(ConstraintSystem &cs, Type builderTy, ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::IgnoreResultBuilderWithReturnStmts;
  }
};

class IgnoreInvalidASTNode final : public ConstraintFix {
  IgnoreInvalidASTNode(ConstraintSystem &cs, ConstraintLocator *locator)
      : IgnoreInvalidASTNode(cs, FixKind::IgnoreInvalidASTNode, locator) {}

protected:
  IgnoreInvalidASTNode(ConstraintSystem &cs, FixKind kind,
                       ConstraintLocator *locator)
      : ConstraintFix(cs, kind, locator) {}

public:
  std::string getName() const override { return "ignore invalid AST node"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static IgnoreInvalidASTNode *create(ConstraintSystem &cs,
                                      ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::IgnoreInvalidASTNode;
  }
};

class IgnoreUnresolvedPatternVar final : public ConstraintFix {
  IgnoreUnresolvedPatternVar(ConstraintSystem &cs, Pattern *pattern,
                             ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::IgnoreUnresolvedPatternVar, locator) {}

public:
  std::string getName() const override {
    return "specify type for pattern match";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static IgnoreUnresolvedPatternVar *
  create(ConstraintSystem &cs, Pattern *pattern, ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::IgnoreUnresolvedPatternVar;
  }
};

class IgnoreInvalidPatternInExpr final : public ConstraintFix {
  Pattern *P;

  IgnoreInvalidPatternInExpr(ConstraintSystem &cs, Pattern *pattern,
                             ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::IgnoreInvalidPatternInExpr, locator),
        P(pattern) {}

public:
  std::string getName() const override {
    return "ignore invalid Pattern nested in Expr";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static IgnoreInvalidPatternInExpr *
  create(ConstraintSystem &cs, Pattern *pattern, ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::IgnoreInvalidPatternInExpr;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::SpecifyContextualTypeForNil;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::SpecifyTypeForPlaceholder;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowRefToInvalidDecl;
  }
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
                      locator, FixBehavior::AlwaysWarning),
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() ==
           FixKind::SpecifyBaseTypeForOptionalUnresolvedMember;
  }
};

class CheckedCastContextualMismatchWarning : public ContextualMismatch {
protected:
  CheckedCastContextualMismatchWarning(ConstraintSystem &cs, FixKind fixKind,
                                       Type fromType, Type toType,
                                       CheckedCastKind kind,
                                       ConstraintLocator *locator)
      : ContextualMismatch(cs, fixKind, fromType, toType, locator,
                           FixBehavior::AlwaysWarning),
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowCheckedCastCoercibleOptionalType;
  }
};

class AllowNoopCheckedCast final : public CheckedCastContextualMismatchWarning {
  AllowNoopCheckedCast(ConstraintSystem &cs, Type fromType, Type toType,
                       CheckedCastKind kind, ConstraintLocator *locator)
      : CheckedCastContextualMismatchWarning(cs, FixKind::AllowNoopCheckedCast,
                                             fromType, toType, kind, locator) {}

public:
  std::string getName() const override {
    return "checked cast always succeeds";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowNoopCheckedCast *create(ConstraintSystem &cs, Type fromType,
                                      Type toType, CheckedCastKind kind,
                                      ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowNoopCheckedCast;
  }
};

class AllowNoopExistentialToCFTypeCheckedCast final
    : public CheckedCastContextualMismatchWarning {
  AllowNoopExistentialToCFTypeCheckedCast(ConstraintSystem &cs, Type fromType,
                                          Type toType, CheckedCastKind kind,
                                          ConstraintLocator *locator)
      : CheckedCastContextualMismatchWarning(
            cs, FixKind::AllowNoopExistentialToCFTypeCheckedCast, fromType,
            toType, kind, locator) {}

public:
  std::string getName() const override {
    return "checked cast from existential to CFType always succeeds";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowNoopExistentialToCFTypeCheckedCast *
  attempt(ConstraintSystem &cs, Type fromType, Type toType,
          CheckedCastKind kind, ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowNoopExistentialToCFTypeCheckedCast;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowUnsupportedRuntimeCheckedCast;
  }
};

class AllowCheckedCastToUnrelated final
    : public CheckedCastContextualMismatchWarning {
  AllowCheckedCastToUnrelated(ConstraintSystem &cs, Type fromType, Type toType,
                              CheckedCastKind kind, ConstraintLocator *locator)
      : CheckedCastContextualMismatchWarning(
            cs, FixKind::AllowCheckedCastToUnrelated, fromType, toType, kind,
            locator) {}

public:
  std::string getName() const override { return "checked cast always fails"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowCheckedCastToUnrelated *attempt(ConstraintSystem &cs,
                                              Type fromType, Type toType,
                                              CheckedCastKind kind,
                                              ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowCheckedCastToUnrelated;
  }
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

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() ==
           FixKind::AllowInvalidStaticMemberRefOnProtocolMetatype;
  }
};

/// Emit a warning for mismatched tuple labels.
class AllowTupleLabelMismatch final : public ContextualMismatch {
  AllowTupleLabelMismatch(ConstraintSystem &cs, Type fromType, Type toType,
                          ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::AllowTupleLabelMismatch, fromType,
                           toType, locator, FixBehavior::AlwaysWarning) {}

public:
  std::string getName() const override { return "allow tuple label mismatch"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowTupleLabelMismatch *create(ConstraintSystem &cs, Type fromType,
                                         Type toType,
                                         ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowTupleLabelMismatch;
  }
};

class AllowAssociatedValueMismatch final : public ContextualMismatch {
  AllowAssociatedValueMismatch(ConstraintSystem &cs, Type fromType, Type toType,
                               ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::AllowAssociatedValueMismatch, fromType,
                           toType, locator) {}

public:
  std::string getName() const override {
    return "allow associated value mismatch";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowAssociatedValueMismatch *create(ConstraintSystem &cs,
                                              Type fromType, Type toType,
                                              ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowAssociatedValueMismatch;
  }
};

class AllowNonOptionalWeak final : public ConstraintFix {
  AllowNonOptionalWeak(ConstraintSystem &cs, ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowNonOptionalWeak, locator) {}

public:
  std::string getName() const override {
    return "allow `weak` with non-optional type";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowNonOptionalWeak *create(ConstraintSystem &cs,
                                      ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowNonOptionalWeak;
  }
};

class AllowSwiftToCPointerConversion final : public ConstraintFix {
  AllowSwiftToCPointerConversion(ConstraintSystem &cs,
                                 ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowSwiftToCPointerConversion, locator) {}

public:
  std::string getName() const override {
    return "allow implicit Swift -> C pointer conversion";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowSwiftToCPointerConversion *create(ConstraintSystem &cs,
                                                ConstraintLocator *locator);
};

class IgnoreDefaultExprTypeMismatch : public AllowArgumentMismatch {
protected:
  IgnoreDefaultExprTypeMismatch(ConstraintSystem &cs, Type argType,
                                Type paramType, ConstraintLocator *locator)
      : AllowArgumentMismatch(cs, FixKind::IgnoreDefaultExprTypeMismatch,
                              argType, paramType, locator) {}

public:
  std::string getName() const override {
    return "allow default expression conversion mismatch";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static IgnoreDefaultExprTypeMismatch *create(ConstraintSystem &cs,
                                               Type argType, Type paramType,
                                               ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::IgnoreDefaultExprTypeMismatch;
  }
};

class AddExplicitExistentialCoercion final : public ConstraintFix {
  Type ErasedResultType;

  AddExplicitExistentialCoercion(ConstraintSystem &cs, Type erasedResultTy,
                                 ConstraintLocator *locator,
                                 FixBehavior fixBehavior)
      : ConstraintFix(cs, FixKind::AddExplicitExistentialCoercion, locator,
                      fixBehavior),
        ErasedResultType(erasedResultTy) {}

public:
  std::string getName() const override {
    return "add explicit existential type coercion";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static bool
  isRequired(ConstraintSystem &cs, Type resultTy,
             ArrayRef<std::pair<TypeVariableType *, OpenedArchetypeType *>>
                 openedExistentials,
             ConstraintLocatorBuilder locator);

  static bool
  isRequired(ConstraintSystem &cs, Type resultTy,
             llvm::function_ref<llvm::Optional<Type>(TypeVariableType *)>
                 findExistentialType,
             ConstraintLocatorBuilder locator);

  static AddExplicitExistentialCoercion *create(ConstraintSystem &cs,
                                                Type resultTy,
                                                ConstraintLocator *locator);
};

class RenameConflictingPatternVariables final
    : public ConstraintFix,
      private llvm::TrailingObjects<RenameConflictingPatternVariables,
                                    VarDecl *> {
  friend TrailingObjects;

  Type ExpectedType;
  unsigned NumConflicts;

  RenameConflictingPatternVariables(ConstraintSystem &cs, Type expectedTy,
                                    ArrayRef<VarDecl *> conflicts,
                                    ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::RenameConflictingPatternVariables, locator),
        ExpectedType(expectedTy), NumConflicts(conflicts.size()) {
    std::uninitialized_copy(conflicts.begin(), conflicts.end(),
                            getConflictingBuffer().begin());
  }

  MutableArrayRef<VarDecl *> getConflictingBuffer() {
    return {getTrailingObjects<VarDecl *>(), NumConflicts};
  }

public:
  std::string getName() const override { return "rename pattern variables"; }

  ArrayRef<VarDecl *> getConflictingVars() const {
    return {getTrailingObjects<VarDecl *>(), NumConflicts};
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static RenameConflictingPatternVariables *
  create(ConstraintSystem &cs, Type expectedTy, ArrayRef<VarDecl *> conflicts,
         ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::RenameConflictingPatternVariables;
  }
};

class MacroMissingPound final : public ConstraintFix {
  MacroDecl *macro;

  MacroMissingPound(ConstraintSystem &cs, MacroDecl *macro,
                    ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::MacroMissingPound, locator),
        macro(macro) { }

public:
  std::string getName() const override { return "macro missing pound"; }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static MacroMissingPound *
  create(ConstraintSystem &cs, MacroDecl *macro,
         ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::MacroMissingPound;
  }
};

/// Allow mismatch between function types global actors.
/// e.g.  `@MainActor () -> Void` vs.`@OtherActor () -> Void`
class AllowGlobalActorMismatch final : public ContextualMismatch {
  AllowGlobalActorMismatch(ConstraintSystem &cs, Type fromType, Type toType,
                           ConstraintLocator *locator)
      : ContextualMismatch(cs, FixKind::AllowGlobalActorMismatch, fromType,
                           toType, locator) {}

public:
  std::string getName() const override {
    return "allow function type actor mismatch";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static AllowGlobalActorMismatch *create(ConstraintSystem &cs, Type fromType,
                                          Type toType,
                                          ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowGlobalActorMismatch;
  }
};

/// Passing an argument of tuple type to a value pack expansion parameter
/// that expected N distinct elements.
class DestructureTupleToMatchPackExpansionParameter final
    : public ConstraintFix {
  PackType *ParamShape;

  DestructureTupleToMatchPackExpansionParameter(ConstraintSystem &cs,
                                                PackType *paramShapeTy,
                                                ConstraintLocator *locator)
      : ConstraintFix(cs,
                      FixKind::DestructureTupleToMatchPackExpansionParameter,
                      locator),
        ParamShape(paramShapeTy) {
    assert(locator->isLastElement<LocatorPathElt::ApplyArgToParam>());
  }

public:
  std::string getName() const override {
    return "allow pack expansion to match tuple argument";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  static DestructureTupleToMatchPackExpansionParameter *
  create(ConstraintSystem &cs, PackType *paramShapeTy,
         ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() ==
           FixKind::DestructureTupleToMatchPackExpansionParameter;
  }
};

class AllowValueExpansionWithoutPackReferences final : public ConstraintFix {
  AllowValueExpansionWithoutPackReferences(ConstraintSystem &cs,
                                           ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowValueExpansionWithoutPackReferences,
                      locator) {}

public:
  std::string getName() const override {
    return "allow value pack expansion without pack references";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static AllowValueExpansionWithoutPackReferences *
  create(ConstraintSystem &cs, ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowValueExpansionWithoutPackReferences;
  }
};

class IgnoreMissingEachKeyword final : public ConstraintFix {
  Type ValuePackType;

  IgnoreMissingEachKeyword(ConstraintSystem &cs, Type valuePackTy,
                           ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::IgnoreMissingEachKeyword, locator),
        ValuePackType(valuePackTy) {}

public:
  std::string getName() const override {
    return "allow value pack reference without 'each'";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static IgnoreMissingEachKeyword *
  create(ConstraintSystem &cs, Type valuePackTy, ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::IgnoreMissingEachKeyword;
  }
};

class AllowInvalidMemberReferenceInInitAccessor final : public ConstraintFix {
  DeclNameRef MemberName;

  AllowInvalidMemberReferenceInInitAccessor(ConstraintSystem &cs,
                                            DeclNameRef memberName,
                                            ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowInvalidMemberReferenceInInitAccessor,
                      locator),
        MemberName(memberName) {}

public:
  std::string getName() const override {
    llvm::SmallVector<char, 16> scratch;
    auto memberName = MemberName.getString(scratch);
    return "allow reference to member '" + memberName.str() +
           "' in init accessor";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static AllowInvalidMemberReferenceInInitAccessor *
  create(ConstraintSystem &cs, DeclNameRef memberName,
         ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowInvalidMemberReferenceInInitAccessor;
  }
};

class AllowConcreteTypeSpecialization final : public ConstraintFix {
  Type ConcreteType;

  AllowConcreteTypeSpecialization(ConstraintSystem &cs, Type concreteTy,
                                  ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::AllowConcreteTypeSpecialization, locator),
        ConcreteType(concreteTy) {}

public:
  std::string getName() const override {
    return "allow concrete type specialization";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static AllowConcreteTypeSpecialization *
  create(ConstraintSystem &cs, Type concreteTy, ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::AllowConcreteTypeSpecialization;
  }
};

class IgnoreGenericSpecializationArityMismatch final : public ConstraintFix {
  ValueDecl *D;
  unsigned NumParams;
  unsigned NumArgs;
  bool HasParameterPack;

  IgnoreGenericSpecializationArityMismatch(ConstraintSystem &cs,
                                           ValueDecl *decl, unsigned numParams,
                                           unsigned numArgs,
                                           bool hasParameterPack,
                                           ConstraintLocator *locator)
      : ConstraintFix(cs, FixKind::IgnoreGenericSpecializationArityMismatch,
                      locator),
        D(decl), NumParams(numParams), NumArgs(numArgs),
        HasParameterPack(hasParameterPack) {}

public:
  std::string getName() const override {
    return "ignore generic specialization mismatch";
  }

  bool diagnose(const Solution &solution, bool asNote = false) const override;

  bool diagnoseForAmbiguity(CommonFixesArray commonFixes) const override {
    return diagnose(*commonFixes.front().first);
  }

  static IgnoreGenericSpecializationArityMismatch *
  create(ConstraintSystem &cs, ValueDecl *decl, unsigned numParams,
         unsigned numArgs, bool hasParameterPack, ConstraintLocator *locator);

  static bool classof(const ConstraintFix *fix) {
    return fix->getKind() == FixKind::IgnoreGenericSpecializationArityMismatch;
  }
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
