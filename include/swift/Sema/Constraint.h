//===--- Constraint.h - Constraint in the Type Checker ----------*- C++ -*-===//
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
// This file provides the \c Constraint class and its related types,
// which is used by the constraint-based type checker to describe a
// constraint that must be solved.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_SEMA_CONSTRAINT_H
#define SWIFT_SEMA_CONSTRAINT_H

#include "swift/AST/ASTNode.h"
#include "swift/AST/FunctionRefKind.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeLoc.h"
#include "swift/Basic/Debug.h"
#include "swift/Sema/ConstraintLocator.h"
#include "swift/Sema/ContextualTypeInfo.h"
#include "swift/Sema/OverloadChoice.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/Support/TrailingObjects.h"

namespace llvm {

class raw_ostream;

}

namespace swift {

class ProtocolDecl;
class SourceManager;
class TypeVariableType;

namespace constraints {

class ConstraintFix;
class ConstraintLocator;
class ConstraintSystem;
enum class TrailingClosureMatching;

/// Describes the kind of constraint placed on one or more types.
enum class ConstraintKind : char {
  /// The two types must be bound to the same type. This is the only
  /// truly symmetric constraint.
  Bind,
  /// The two types must be bound to the same type, dropping
  /// lvalueness when comparing a type variable to a type.
  Equal,
  /// The first type is the type of a function parameter; the second
  /// type is the type of a reference to that parameter from within the
  /// function body. Specifically, the left type is an inout type iff the right
  /// type is an lvalue type with the same object type. Otherwise, the two
  /// types must be the same type.
  BindParam,
  /// Binds the first type to the element type of the second type.
  BindToPointerType,
  /// The first type is a subtype of the second type, i.e., a value
  /// of the type of the first type can be used wherever a value of the
  /// second type is expected.
  Subtype,
  /// The first type is convertible to the second type.
  Conversion,
  /// The first type can be bridged to the second type.
  BridgingConversion,
  /// The first type is the element of an argument tuple that is
  /// convertible to the second type (which represents the corresponding
  /// parameter type).
  ArgumentConversion,
  /// The first type is convertible to the second type, including inout.
  OperatorArgumentConversion,
  /// The first type must be a subclass of the second type (which is a
  /// class type).
  SubclassOf,
  /// The first type must conform to the second type (which is a
  /// protocol type).
  ConformsTo,
  /// The first type describes a literal that conforms to the second
  /// type, which is one of the known expressible-by-literal protocols.
  LiteralConformsTo,
  /// A checked cast from the first type to the second.
  CheckedCast,
  /// The first type can act as the Self type of the second type (which
  /// is a protocol).
  ///
  /// This constraint is slightly looser than a conforms-to constraint, because
  /// an existential can be used as the Self of any protocol within the
  /// existential, even if it doesn't conform to that protocol (e.g., due to
  /// the use of associated types).
  SelfObjectOfProtocol,
  /// Both types are function types. The first function type's
  /// input is the value being passed to the function and its output
  /// is a type variable that describes the output. The second
  /// function type is expected to become a function type. Note, we
  /// do not require the function type attributes to match.
  ApplicableFunction,
  /// The first type is a function type whose input is the value passed
  /// to the function and whose output is a type variable describing the output.
  /// The second type is either a `@dynamicCallable` nominal type or the
  /// function type of a `dynamicallyCall` method defined on a
  /// `@dynamicCallable` nominal type.
  DynamicCallableApplicableFunction,
  /// The first type is the type of the dynamicType member of the
  /// second type.
  DynamicTypeOf,
  /// Binds the left-hand type to a particular overload choice.
  BindOverload,
  /// The first type has a member with the given name, and the
  /// type of that member, when referenced as a value, is the second type.
  ValueMember,
  /// The first type (which is implicit) has a member with the given
  /// name, and the type of that member, when referenced as a value, is the
  /// second type.
  UnresolvedValueMember,
  /// The first type conforms to the protocol in which the member requirement
  /// resides. Once the conformance is resolved, the value witness will be
  /// determined, and the type of that witness, when referenced as a value,
  /// will be bound to the second type.
  ValueWitness,
  /// The first type can be defaulted to the second (which currently
  /// cannot be dependent).  This is more like a type property than a
  /// relational constraint.
  Defaultable,
  /// A disjunction constraint that specifies that one or more of the
  /// stored constraints must hold.
  Disjunction,
  /// A conjunction constraint that specifies that all of the stored
  /// constraints must hold.
  Conjunction,
  /// The first type is an optional type whose object type is the second
  /// type, preserving lvalue-ness.
  OptionalObject,
  /// The first type is the same function type as the second type, but
  /// made @escaping.
  EscapableFunctionOf,
  /// The first type is an opened type from the second type (which is
  /// an existential).
  OpenedExistentialOf,
  /// A relation between three types. The first is the key path type,
  /// the second is the root type, and the third is the projected value type.
  /// The second and third types can be lvalues depending on the kind of key
  /// path.
  KeyPathApplication,
  /// A relation between three types. The first is the key path type,
  /// the second is its root type, and the third is the projected value type.
  /// The key path type is chosen based on the selection of overloads for the
  /// member references along the path.
  KeyPath,
  /// The first type will be equal to the second type, but only when the
  /// second type has been fully determined (and mapped down to a concrete
  /// type). At that point, this constraint will be treated like an `Equal`
  /// constraint.
  OneWayEqual,
  /// The second type is the type of a function parameter, and the first type
  /// is the type of a reference to that function parameter within the body.
  /// Once the second type has been fully determined (and mapped down to a
  /// concrete type), this constraint will be treated like a 'BindParam'
  /// constraint.
  OneWayBindParam,
  /// If there is no contextual info e.g. `_ = { 42 }` default first type
  /// to a second type. This is effectively a `Defaultable` constraint
  /// which one significant difference:
  ///
  /// - Handled specially by binding inference, specifically contributes
  ///   to the bindings only if there are no contextual types available.
  FallbackType,
  /// The first type represents a result of an unresolved member chain,
  /// and the second type is its base type. This constraint acts almost
  /// like `Equal` but also enforces following semantics:
  ///
  /// - It's possible to infer a base from a result type by looking through
  ///   this constraint, but it's only solved when both types are bound.
  ///
  /// - If base is a protocol metatype, this constraint becomes a conformance
  ///   check instead of an equality.
  UnresolvedMemberChainBase,
  /// The first type is a property wrapper with a wrapped-value type
  /// equal to the second type.
  PropertyWrapper,
  /// The first type (or its optional or pointer version) must conform to a
  /// second type (protocol type). This is not a direct requirement but one
  /// inferred from a conversion, so the check is more relax comparing to
  /// `ConformsTo`.
  TransitivelyConformsTo,
  /// Represents an AST node contained in a body of a function/closure.
  /// It only has an AST node to generate constraints and infer the type for.
  SyntacticElement,
  /// The first type is the opened pack element type of the second type, which
  /// is the pattern of a pack expansion type.
  PackElementOf,
  /// Do not add new uses of this, it only exists to retain compatibility for
  /// rdar://85263844.
  ///
  /// Binds the RHS type to a tuple of the params of a function typed LHS. Note
  /// this discards function parameter flags.
  BindTupleOfFunctionParams,
  /// The first type is a reduced shape of the second type (represented as a
  /// pack type).
  ShapeOf,
  /// Represents explicit generic arguments provided for a reference to
  /// a declaration.
  ///
  /// The first type is the type variable describing the bound type of
  /// an overload. The second type is a PackType containing the explicit
  /// generic arguments.
  ExplicitGenericArguments,
  /// Both (first and second) pack types should have the same reduced shape.
  SameShape,
  /// The first type is a tuple containing a single unlabeled element that is a
  /// pack expansion. The second type is that pack expansion.
  MaterializePackExpansion,
};

/// Classification of the different kinds of constraints.
enum class ConstraintClassification : char {
  /// A relational constraint, which relates two types.
  Relational,

  /// A member constraint, which names a member of a type and assigns
  /// it a reference type.
  Member,

  /// A property of a single type, such as whether it is defaultable to
  /// a particular type.
  TypeProperty,

  /// A disjunction constraint.
  Disjunction,

  /// A conjunction constraint.
  Conjunction,

  /// An element of a closure/function body.
  SyntacticElement,
};

/// Specifies a restriction on the kind of conversion that should be
/// performed between the types in a constraint.
///
/// It's common for there to be multiple potential conversions that can
/// apply between two types, e.g., given class types A and B, there might be
/// a superclass conversion from A to B or there might be a user-defined
/// conversion from A to B. The solver may need to explore both paths.
enum class ConversionRestrictionKind {
  /// Deep equality comparison.
  DeepEquality,
  /// Subclass-to-superclass conversion.
  Superclass,
  /// Class metatype to AnyObject conversion.
  ClassMetatypeToAnyObject,
  /// Existential metatype to AnyObject conversion.
  ExistentialMetatypeToAnyObject,
  /// Protocol value metatype to Protocol class conversion.
  ProtocolMetatypeToProtocolClass,
  /// Inout-to-pointer conversion.
  InoutToPointer,
  /// Converting from `inout` to a C pointer has `PointerToCPointer` semantics.
  InoutToCPointer,
  /// Array-to-pointer conversion.
  ArrayToPointer,
  /// Converting from array to a C pointer has `PointerToCPointer` semantics.
  ArrayToCPointer,
  /// String-to-pointer conversion.
  StringToPointer,
  /// Pointer-to-pointer conversion.
  PointerToPointer,
  /// Value to existential value conversion, or existential erasure.
  Existential,
  /// Metatype to existential metatype conversion.
  MetatypeToExistentialMetatype,
  /// Existential metatype to metatype conversion.
  ExistentialMetatypeToMetatype,
  /// T -> U? value to optional conversion (or to implicitly unwrapped
  /// optional).
  ValueToOptional,
  /// T? -> U? optional to optional conversion (or unchecked to unchecked).
  OptionalToOptional,
  /// Implicit upcast conversion of array types.
  ArrayUpcast,
  /// Implicit upcast conversion of dictionary types, which includes
  /// bridging.
  DictionaryUpcast,
  /// Implicit upcast conversion of set types, which includes bridging.
  SetUpcast,
  /// T:Hashable -> AnyHashable conversion.
  HashableToAnyHashable,
  /// Implicit conversion from a CF type to its toll-free-bridged Objective-C
  /// class type.
  CFTollFreeBridgeToObjC,
  /// Implicit conversion from an Objective-C class type to its
  /// toll-free-bridged CF type.
  ObjCTollFreeBridgeToCF,
  /// Implicit conversion from a value of Double to a value of CGFloat type via
  /// an implicit CGFloat initializer call.
  DoubleToCGFloat,
  /// Implicit conversion from a value of CGFloat type to a value of Double type
  /// via an implicit Double initializer call passing a CGFloat value.
  CGFloatToDouble,
  /// Implicit conversion between Swift and C pointers:
  ///    - Unsafe[Mutable]RawPointer -> Unsafe[Mutable]Pointer<[U]Int>
  ///    - Unsafe[Mutable]Pointer<Int{8, 16, ...}> <-> Unsafe[Mutable]Pointer<UInt{8, 16, ...}>
  PointerToCPointer,
};

/// Specifies whether a given conversion requires the creation of a temporary
/// value which is only valid for a limited scope. For example, the
/// array-to-pointer conversion produces a pointer that is only valid for the
/// duration of the call that it's passed to. Such ephemeral conversions cannot
/// be passed to non-ephemeral parameters.
enum class ConversionEphemeralness {
  /// The conversion requires the creation of a temporary value.
  Ephemeral,
  /// The conversion does not require the creation of a temporary value.
  NonEphemeral,
  /// It is not currently known whether the conversion will produce a temporary
  /// value or not. This can occur for example with an inout-to-pointer
  /// conversion of a member whose base type is an unresolved type variable.
  Unresolved,
};

/// Return a string representation of a conversion restriction.
llvm::StringRef getName(ConversionRestrictionKind kind);

/// Should we record which choice was taken in this disjunction for
/// the purposes of applying it later?
enum RememberChoice_t : bool {
  ForgetChoice = false,
  RememberChoice = true
};

/// A constraint between two type variables.
class Constraint final : public llvm::ilist_node<Constraint>,
    private llvm::TrailingObjects<Constraint, TypeVariableType *> {
  friend TrailingObjects;

  /// The kind of constraint.
  ConstraintKind Kind : 8;

  /// The kind of restriction placed on this constraint.
  ConversionRestrictionKind Restriction : 8;

  /// The fix to be applied to the constraint before visiting it.
  ConstraintFix *TheFix = nullptr;

  /// Whether the \c Restriction field is valid.
  unsigned HasRestriction : 1;

  /// Whether this constraint is currently active, i.e., stored in the worklist.
  unsigned IsActive : 1;

  /// Was this constraint was determined to be inconsistent with the
  /// constraint graph during constraint propagation?
  unsigned IsDisabled : 1;

  /// Constraint is disabled in performance mode only, could be attempted
  /// for diagnostic purposes.
  unsigned IsDisabledForPerformance : 1;

  /// Whether the choice of this disjunction should be recorded in the
  /// solver state.
  unsigned RememberChoice : 1;
  
  /// Whether or not this constraint is 'favored' in the sense that, if
  /// successfully applied, it should be preferred over any other constraints
  /// in its disjunction.
  unsigned IsFavored : 1;

  /// Whether or not this constraint should be solved in isolation from
  /// the rest of the constraint system. Currently only applies to conjunctions.
  unsigned IsIsolated : 1;

  /// The number of type variables referenced by this constraint.
  ///
  /// The type variables themselves are tail-allocated.
  unsigned NumTypeVariables : 11;

  /// The kind of function reference, for member references.
  unsigned TheFunctionRefKind : 2;

  /// The trailing closure matching for an applicable function constraint,
  /// if any. 0 = None, 1 = Forward, 2 = Backward.
  unsigned trailingClosureMatching : 2;

  union {
    struct {
      /// The first type.
      Type First;

      /// The second type.
      Type Second;

      /// The third type, if any.
      Type Third;
    } Types;

    struct {
      /// The type of the base.
      Type First;

      /// The type of the member.
      Type Second;

      union {
        /// If non-null, the name of a member of the first type is that
        /// being related to the second type.
        ///
        /// Used for ValueMember an UnresolvedValueMember constraints.
        DeclNameRef Name;

        /// If non-null, the member being referenced.
        ///
        /// Used for ValueWitness constraints.
        ValueDecl *Ref;
      } Member;

      /// The DC in which the use appears.
      DeclContext *UseDC;
    } Member;

    /// The set of constraints for a disjunction.
    ArrayRef<Constraint *> Nested;

    struct {
      /// The first type
      Type First;

      /// The overload choice
      OverloadChoice Choice;

      /// The DC in which the use appears.
      DeclContext *UseDC;
    } Overload;

    struct {
      /// The node itself.
      ASTNode Element;
      /// Contextual information associated with the element (if any).
      ContextualTypeInfo Context;
      /// Identifies whether result of this node is unused.
      bool IsDiscarded;
    } SyntacticElement;
  };

  /// The locator that describes where in the expression this
  /// constraint applies.
  ConstraintLocator *Locator;

  /// Constraints are always allocated within a given constraint
  /// system.
  void *operator new(size_t) = delete;

  Constraint(ConstraintKind kind, ArrayRef<Constraint *> constraints,
             bool isIsolated, ConstraintLocator *locator,
             SmallPtrSetImpl<TypeVariableType *> &typeVars);

  /// Construct a new constraint.
  Constraint(ConstraintKind kind, Type first, Type second,
             ConstraintLocator *locator,
             SmallPtrSetImpl<TypeVariableType *> &typeVars);

  /// Construct a new constraint.
  Constraint(ConstraintKind kind, Type first, Type second, Type third,
             ConstraintLocator *locator,
             SmallPtrSetImpl<TypeVariableType *> &typeVars);

  /// Construct a new member constraint.
  Constraint(ConstraintKind kind, Type first, Type second, DeclNameRef member,
             DeclContext *useDC, FunctionRefKind functionRefKind,
             ConstraintLocator *locator,
             SmallPtrSetImpl<TypeVariableType *> &typeVars);

  /// Construct a new value witness constraint.
  Constraint(ConstraintKind kind, Type first, Type second,
             ValueDecl *requirement, DeclContext *useDC,
             FunctionRefKind functionRefKind, ConstraintLocator *locator,
             SmallPtrSetImpl<TypeVariableType *> &typeVars);

  /// Construct a new overload-binding constraint, which might have a fix.
  Constraint(Type type, OverloadChoice choice, DeclContext *useDC,
             ConstraintFix *fix, ConstraintLocator *locator,
             SmallPtrSetImpl<TypeVariableType *> &typeVars);

  /// Construct a restricted constraint.
  Constraint(ConstraintKind kind, ConversionRestrictionKind restriction,
             Type first, Type second, ConstraintLocator *locator,
             SmallPtrSetImpl<TypeVariableType *> &typeVars);

  /// Construct a relational constraint with a fix.
  Constraint(ConstraintKind kind, ConstraintFix *fix, Type first, Type second,
             ConstraintLocator *locator,
             SmallPtrSetImpl<TypeVariableType *> &typeVars);

  /// Construct a closure body element constraint.
  Constraint(ASTNode node, ContextualTypeInfo context, bool isDiscarded,
             ConstraintLocator *locator,
             SmallPtrSetImpl<TypeVariableType *> &typeVars);

  /// Retrieve the type variables buffer, for internal mutation.
  MutableArrayRef<TypeVariableType *> getTypeVariablesBuffer() {
    return { getTrailingObjects<TypeVariableType *>(), NumTypeVariables };
  }

public:
  /// Create a new constraint.
  static Constraint *create(ConstraintSystem &cs, ConstraintKind Kind,
                            Type First, Type Second, ConstraintLocator *locator,
                            ArrayRef<TypeVariableType *> extraTypeVars = {});

  /// Create a new constraint.
  static Constraint *create(ConstraintSystem &cs, ConstraintKind Kind, 
                            Type First, Type Second, Type Third,
                            ConstraintLocator *locator,
                            ArrayRef<TypeVariableType *> extraTypeVars = { });

  /// Create a new member constraint, or a disjunction of that with the outer
  /// alternatives.
  static Constraint *createMemberOrOuterDisjunction(
      ConstraintSystem &cs, ConstraintKind kind, Type first, Type second,
      DeclNameRef member, DeclContext *useDC, FunctionRefKind functionRefKind,
      ArrayRef<OverloadChoice> outerAlternatives, ConstraintLocator *locator);

  /// Create a new member constraint.
  static Constraint *createMember(ConstraintSystem &cs, ConstraintKind kind,
                                  Type first, Type second, DeclNameRef member,
                                  DeclContext *useDC,
                                  FunctionRefKind functionRefKind,
                                  ConstraintLocator *locator);

  /// Create a new value witness constraint.
  static Constraint *createValueWitness(
      ConstraintSystem &cs, ConstraintKind kind, Type first, Type second,
      ValueDecl *requirement, DeclContext *useDC,
      FunctionRefKind functionRefKind, ConstraintLocator *locator);

  /// Create an overload-binding constraint.
  static Constraint *createBindOverload(ConstraintSystem &cs, Type type, 
                                        OverloadChoice choice, 
                                        DeclContext *useDC,
                                        ConstraintLocator *locator);

  /// Create a restricted relational constraint.
  static Constraint *createRestricted(ConstraintSystem &cs, ConstraintKind kind,
                                      ConversionRestrictionKind restriction,
                                      Type first, Type second, 
                                      ConstraintLocator *locator);

  /// Create a relational constraint with a fix.
  static Constraint *createFixed(ConstraintSystem &cs, ConstraintKind kind,
                                 ConstraintFix *fix, Type first, Type second,
                                 ConstraintLocator *locator);

  /// Create a bind overload choice with a fix.
  /// Note: This constraint is going to be disabled by default.
  static Constraint *createFixedChoice(ConstraintSystem &cs, Type type,
                                       OverloadChoice choice,
                                       DeclContext *useDC, ConstraintFix *fix,
                                       ConstraintLocator *locator);

  /// Create a new disjunction constraint.
  static Constraint *createDisjunction(ConstraintSystem &cs,
                                       ArrayRef<Constraint *> constraints,
                                       ConstraintLocator *locator,
                                       RememberChoice_t shouldRememberChoice
                                         = ForgetChoice);

  /// Create a new conjunction constraint.
  ///
  /// \param isIsolated - Indicates whether given constraint should be
  /// solved in isolation from the rest of the constraint system i.e.
  /// by removing all of the unrelated type variables and constraints.
  static Constraint *
  createConjunction(ConstraintSystem &cs, ArrayRef<Constraint *> constraints,
                    bool isIsolated, ConstraintLocator *locator,
                    ArrayRef<TypeVariableType *> referencedVars = {});

  /// Create a new Applicable Function constraint.
  static Constraint *createApplicableFunction(
      ConstraintSystem &cs, Type argumentFnType, Type calleeType,
      llvm::Optional<TrailingClosureMatching> trailingClosureMatching,
      ConstraintLocator *locator);

  static Constraint *createSyntacticElement(ConstraintSystem &cs,
                                              ASTNode node,
                                              ConstraintLocator *locator,
                                              bool isDiscarded = false);

  static Constraint *createSyntacticElement(ConstraintSystem &cs,
                                              ASTNode node,
                                              ContextualTypeInfo context,
                                              ConstraintLocator *locator,
                                              bool isDiscarded = false);

  /// Determine the kind of constraint.
  ConstraintKind getKind() const { return Kind; }

  /// Retrieve the restriction placed on this constraint.
  llvm::Optional<ConversionRestrictionKind> getRestriction() const {
    if (!HasRestriction)
      return llvm::None;

    return Restriction;
  }

  /// Retrieve the fix associated with this constraint.
  ConstraintFix *getFix() const { return TheFix; }

  /// Whether this constraint is active, i.e., in the worklist.
  bool isActive() const { return IsActive; }

  /// Set whether this constraint is active or not.
  void setActive(bool active) {
    assert(!isDisabled() && "Cannot activate a constraint that is disabled!");
    IsActive = active;
  }

  /// Whether this constraint is disabled and shouldn't be attempted by the
  /// solver.
  bool isDisabled() const { return IsDisabled || IsDisabledForPerformance; }

  /// Whether this constraint is disabled and shouldn't be attempted by the
  /// solver only in "performance" mode.
  bool isDisabledInPerformanceMode() const { return IsDisabledForPerformance; }

  /// Set whether this constraint is active or not.
  void setDisabled(bool enableForDiagnostics = false) {
    assert(!isActive() && "Cannot disable constraint marked as active!");
    if (enableForDiagnostics)
      IsDisabledForPerformance = true;
    else
      IsDisabled = true;
  }

  void setEnabled() {
    assert(isDisabled() && "Can't re-enable already active constraint!");
    IsDisabled = false;
    IsDisabledForPerformance = false;
  }

  /// Mark or retrieve whether this constraint should be favored in the system.
  void setFavored(bool favored = true) { IsFavored = favored; }
  bool isFavored() const { return IsFavored; }

  /// Whether the solver should remember which choice was taken for
  /// this constraint.
  bool shouldRememberChoice() const { return RememberChoice; }

  /// Retrieve the set of type variables referenced by this constraint.
  ArrayRef<TypeVariableType *> getTypeVariables() const {
    return {getTrailingObjects<TypeVariableType*>(), NumTypeVariables};
  }

  /// Determine the classification of this constraint, providing
  /// a broader categorization than \c getKind().
  ConstraintClassification getClassification() const {
    switch (Kind) {
    case ConstraintKind::Bind:
    case ConstraintKind::Equal:
    case ConstraintKind::BindParam:
    case ConstraintKind::BindToPointerType:
    case ConstraintKind::Subtype:
    case ConstraintKind::Conversion:
    case ConstraintKind::BridgingConversion:
    case ConstraintKind::ArgumentConversion:
    case ConstraintKind::OperatorArgumentConversion:
    case ConstraintKind::SubclassOf:
    case ConstraintKind::ConformsTo:
    case ConstraintKind::LiteralConformsTo:
    case ConstraintKind::TransitivelyConformsTo:
    case ConstraintKind::CheckedCast:
    case ConstraintKind::SelfObjectOfProtocol:
    case ConstraintKind::ApplicableFunction:
    case ConstraintKind::DynamicCallableApplicableFunction:
    case ConstraintKind::BindOverload:
    case ConstraintKind::OptionalObject:
    case ConstraintKind::OneWayEqual:
    case ConstraintKind::OneWayBindParam:
    case ConstraintKind::FallbackType:
    case ConstraintKind::UnresolvedMemberChainBase:
    case ConstraintKind::PackElementOf:
    case ConstraintKind::SameShape:
    case ConstraintKind::MaterializePackExpansion:
      return ConstraintClassification::Relational;

    case ConstraintKind::ValueMember:
    case ConstraintKind::UnresolvedValueMember:
    case ConstraintKind::ValueWitness:
    case ConstraintKind::PropertyWrapper:
      return ConstraintClassification::Member;

    case ConstraintKind::DynamicTypeOf:
    case ConstraintKind::EscapableFunctionOf:
    case ConstraintKind::OpenedExistentialOf:
    case ConstraintKind::KeyPath:
    case ConstraintKind::KeyPathApplication:
    case ConstraintKind::Defaultable:
    case ConstraintKind::BindTupleOfFunctionParams:
    case ConstraintKind::ShapeOf:
    case ConstraintKind::ExplicitGenericArguments:
      return ConstraintClassification::TypeProperty;

    case ConstraintKind::Disjunction:
      return ConstraintClassification::Disjunction;

    case ConstraintKind::Conjunction:
      return ConstraintClassification::Conjunction;

    case ConstraintKind::SyntacticElement:
      return ConstraintClassification::SyntacticElement;
    }

    llvm_unreachable("Unhandled ConstraintKind in switch.");
  }

  /// Retrieve the first type in the constraint.
  Type getFirstType() const {
    switch (getKind()) {
    case ConstraintKind::Disjunction:
      llvm_unreachable("disjunction constraints have no type operands");

    case ConstraintKind::Conjunction:
      llvm_unreachable("conjunction constraints have no type operands");

    case ConstraintKind::BindOverload:
      return Overload.First;

    case ConstraintKind::ValueMember:
    case ConstraintKind::UnresolvedValueMember:
    case ConstraintKind::ValueWitness:
      return Member.First;

    case ConstraintKind::SyntacticElement:
      llvm_unreachable("closure body element constraint has no type operands");

    default:
      return Types.First;
    }
  }

  /// Retrieve the second type in the constraint.
  Type getSecondType() const {
    switch (getKind()) {
    case ConstraintKind::Disjunction:
    case ConstraintKind::Conjunction:
    case ConstraintKind::BindOverload:
    case ConstraintKind::SyntacticElement:
      llvm_unreachable("constraint has no second type");

    case ConstraintKind::ValueMember:
    case ConstraintKind::UnresolvedValueMember:
    case ConstraintKind::ValueWitness:
      return Member.Second;

    default:
      return Types.Second;
    }
  }

  /// Retrieve the third type in the constraint.
  Type getThirdType() const {
    switch (getKind()) {
    case ConstraintKind::KeyPath:
    case ConstraintKind::KeyPathApplication:
      return Types.Third;
    default:
      llvm_unreachable("no third type");
    }
  }

  /// Retrieve the protocol in a conformance constraint.
  ProtocolDecl *getProtocol() const;

  /// Retrieve the name of the member for a member constraint.
  DeclNameRef getMember() const {
    assert(Kind == ConstraintKind::ValueMember ||
           Kind == ConstraintKind::UnresolvedValueMember);
    return Member.Member.Name;
  }

  /// Retrieve the requirement being referenced by a value witness constraint.
  ValueDecl *getRequirement() const {
    assert(Kind == ConstraintKind::ValueWitness);
    return Member.Member.Ref;
  }

  /// Determine the kind of function reference we have for a member reference.
  FunctionRefKind getFunctionRefKind() const {
    if (Kind == ConstraintKind::ValueMember ||
        Kind == ConstraintKind::UnresolvedValueMember ||
        Kind == ConstraintKind::ValueWitness)
      return static_cast<FunctionRefKind>(TheFunctionRefKind);

    // Conservative answer: drop all of the labels.
    return FunctionRefKind::Compound;
  }

  /// Retrieve the set of constraints in a disjunction.
  ArrayRef<Constraint *> getNestedConstraints() const {
    assert(Kind == ConstraintKind::Disjunction ||
           Kind == ConstraintKind::Conjunction);
    return Nested;
  }

  unsigned countFavoredNestedConstraints() const {
    return llvm::count_if(Nested, [](const Constraint *constraint) {
      return constraint->isFavored() && !constraint->isDisabled();
    });
  }

  unsigned countActiveNestedConstraints() const {
    return llvm::count_if(Nested, [](const Constraint *constraint) {
      return !constraint->isDisabled();
    });
  }

  /// Returns the number of resolved argument types for an applied disjunction
  /// constraint. This is always zero for disjunctions that do not represent
  /// an applied overload.
  unsigned countResolvedArgumentTypes(ConstraintSystem &cs) const;

  /// Determine if this constraint represents explicit conversion,
  /// e.g. coercion constraint "as X" which forms a disjunction.
  bool isExplicitConversion() const;

  /// Determine whether this constraint should be solved in isolation
  /// from the rest of the constraint system.
  bool isIsolated() const { return IsIsolated; }

  /// Whether this is a one-way constraint.
  bool isOneWayConstraint() const {
    return Kind == ConstraintKind::OneWayEqual ||
        Kind == ConstraintKind::OneWayBindParam;
  }

  /// Retrieve the overload choice for an overload-binding constraint.
  OverloadChoice getOverloadChoice() const {
    assert(Kind == ConstraintKind::BindOverload);
    return Overload.Choice;
  }

  /// Retrieve the DC in which the overload was used.
  DeclContext *getOverloadUseDC() const {
    assert(Kind == ConstraintKind::BindOverload);
    return Overload.UseDC;
  }

  /// Retrieve the DC in which the member was used.
  DeclContext *getMemberUseDC() const {
    assert(Kind == ConstraintKind::ValueMember ||
           Kind == ConstraintKind::UnresolvedValueMember ||
           Kind == ConstraintKind::ValueWitness);
    return Member.UseDC;
  }

  ASTNode getSyntacticElement() const {
    assert(Kind == ConstraintKind::SyntacticElement);
    return SyntacticElement.Element;
  }

  ContextualTypeInfo getElementContext() const {
    assert(Kind == ConstraintKind::SyntacticElement);
    return SyntacticElement.Context;
  }

  bool isDiscardedElement() const {
    assert(Kind == ConstraintKind::SyntacticElement);
    return SyntacticElement.IsDiscarded;
  }

  /// For an applicable function constraint, retrieve the trailing closure
  /// matching rule.
  llvm::Optional<TrailingClosureMatching> getTrailingClosureMatching() const;

  /// Retrieve the locator for this constraint.
  ConstraintLocator *getLocator() const { return Locator; }

  /// Clone the given constraint.
  Constraint *clone(ConstraintSystem &cs) const;

  /// Print constraint placed on type and constraint properties.
  ///
  /// \c skipLocator skips printing of locators.
  void print(llvm::raw_ostream &Out, SourceManager *sm, unsigned indent = 0,
             bool skipLocator = false) const;

  SWIFT_DEBUG_DUMPER(dump(SourceManager *SM));

  SWIFT_DEBUG_DUMPER(dump(ConstraintSystem *CS));

  void *operator new(size_t bytes, ConstraintSystem& cs,
                     size_t alignment = alignof(Constraint));

  inline void operator delete(void *, const ConstraintSystem &cs, size_t) {}

  void *operator new(size_t bytes, void *mem) { return mem; }
  void operator delete(void *mem) { }
};

} // end namespace constraints
} // end namespace swift

namespace llvm {

/// Specialization of \c ilist_traits for constraints.
template<>
struct ilist_traits<swift::constraints::Constraint>
         : public ilist_node_traits<swift::constraints::Constraint> {
  using Element = swift::constraints::Constraint;

  static Element *createNode(const Element &V) = delete;
  static void deleteNode(Element *V) { /* never deleted */ }
};

} // end namespace llvm

#endif // LLVM_SWIFT_SEMA_CONSTRAINT_H
