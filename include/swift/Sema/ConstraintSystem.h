//===--- ConstraintSystem.h - Constraint-based Type Checking ----*- C++ -*-===//
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
// This file provides the constraint-based type checker, anchored by the
// \c ConstraintSystem class, which provides type checking and type
// inference for expressions.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_SEMA_CONSTRAINT_SYSTEM_H
#define SWIFT_SEMA_CONSTRAINT_SYSTEM_H

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTNode.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/AnyFunctionRef.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/OptionSet.h"
#include "swift/Sema/Constraint.h"
#include "swift/Sema/ConstraintGraph.h"
#include "swift/Sema/ConstraintGraphScope.h"
#include "swift/Sema/ConstraintLocator.h"
#include "swift/Sema/CSBindings.h"
#include "swift/Sema/CSFix.h"
#include "swift/Sema/OverloadChoice.h"
#include "swift/Sema/SolutionResult.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/ilist.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include <cstddef>
#include <functional>

using namespace swift::constraints::inference;

namespace swift {

class Expr;
class FuncDecl;
class BraseStmt;
enum class TypeCheckExprFlags;

namespace constraints {

class ConstraintGraph;
class ConstraintGraphNode;
class ConstraintSystem;
class SolutionApplicationTarget;

} // end namespace constraints

// Forward declare some TypeChecker related functions
// so they could be made friends of ConstraintSystem.
namespace TypeChecker {

Optional<BraceStmt *> applyResultBuilderBodyTransform(FuncDecl *func,
                                                        Type builderType);

Optional<constraints::SolutionApplicationTarget>
typeCheckExpression(constraints::SolutionApplicationTarget &target,
                    OptionSet<TypeCheckExprFlags> options);

} // end namespace TypeChecker

} // end namespace swift

/// Allocate memory within the given constraint system.
void *operator new(size_t bytes, swift::constraints::ConstraintSystem& cs,
                   size_t alignment = 8);

namespace swift {

/// Specify how we handle the binding of underconstrained (free) type variables
/// within a solution to a constraint system.
enum class FreeTypeVariableBinding {
  /// Disallow any binding of such free type variables.
  Disallow,
  /// Allow the free type variables to persist in the solution.
  Allow,
  /// Bind the type variables to UnresolvedType to represent the ambiguity.
  UnresolvedType
};

namespace constraints {

/// Describes the algorithm to use for trailing closure matching.
enum class TrailingClosureMatching {
  /// Match a trailing closure to the first parameter that appears to work.
  Forward,

  /// Match a trailing closure to the last parameter.
  Backward,
};

/// A handle that holds the saved state of a type variable, which
/// can be restored.
class SavedTypeVariableBinding {
  /// The type variable that we saved the state of.
  TypeVariableType *TypeVar;

  /// The saved type variable options.
  unsigned Options;

  /// The parent or fixed type.
  llvm::PointerUnion<TypeVariableType *, TypeBase *> ParentOrFixed;

public:
  explicit SavedTypeVariableBinding(TypeVariableType *typeVar);

  /// Restore the state of the type variable to the saved state.
  void restore();
};

/// A set of saved type variable bindings.
using SavedTypeVariableBindings = SmallVector<SavedTypeVariableBinding, 16>;

class ConstraintLocator;

/// Describes a conversion restriction or a fix.
struct RestrictionOrFix {
  union {
    ConversionRestrictionKind Restriction;
    ConstraintFix *TheFix;
  };
  bool IsRestriction;

public:
  RestrictionOrFix(ConversionRestrictionKind restriction)
  : Restriction(restriction), IsRestriction(true) { }

  RestrictionOrFix(ConstraintFix *fix) : TheFix(fix), IsRestriction(false) {}

  Optional<ConversionRestrictionKind> getRestriction() const {
    if (IsRestriction)
      return Restriction;

    return None;
  }

  Optional<ConstraintFix *> getFix() const {
    if (!IsRestriction)
      return TheFix;

    return None;
  }
};


class ExpressionTimer {
  Expr* E;
  ASTContext &Context;
  llvm::TimeRecord StartTime;

  bool PrintDebugTiming;
  bool PrintWarning;

public:
  ExpressionTimer(Expr *E, ConstraintSystem &CS);

  ~ExpressionTimer();

  unsigned getWarnLimit() const {
    return Context.TypeCheckerOpts.WarnLongExpressionTypeChecking;
  }
  llvm::TimeRecord startedAt() const { return StartTime; }

  /// Return the elapsed process time (including fractional seconds)
  /// as a double.
  double getElapsedProcessTimeInFractionalSeconds() const {
    llvm::TimeRecord endTime = llvm::TimeRecord::getCurrentTime(false);

    return endTime.getProcessTime() - StartTime.getProcessTime();
  }

  // Disable emission of warnings about expressions that take longer
  // than the warning threshold.
  void disableWarning() { PrintWarning = false; }

  bool isExpired(unsigned thresholdInMillis) const {
    auto elapsed = getElapsedProcessTimeInFractionalSeconds();
    return unsigned(elapsed) > thresholdInMillis;
  }
};

} // end namespace constraints

/// Options that describe how a type variable can be used.
enum TypeVariableOptions {
  /// Whether the type variable can be bound to an lvalue type or not.
  TVO_CanBindToLValue = 0x01,

  /// Whether the type variable can be bound to an inout type or not.
  TVO_CanBindToInOut = 0x02,

  /// Whether the type variable can be bound to a non-escaping type or not.
  TVO_CanBindToNoEscape = 0x04,

  /// Whether the type variable can be bound to a hole or not.
  TVO_CanBindToHole = 0x08,

  /// Whether a more specific deduction for this type variable implies a
  /// better solution to the constraint system.
  TVO_PrefersSubtypeBinding = 0x10,
};

/// The implementation object for a type variable used within the
/// constraint-solving type checker.
///
/// The implementation object for a type variable contains information about
/// the type variable, where it was generated, what protocols it must conform
/// to, what specific types it might be and, eventually, the fixed type to
/// which it is assigned.
class TypeVariableType::Implementation {
  /// The locator that describes where this type variable was generated.
  constraints::ConstraintLocator *locator;

  /// Either the parent of this type variable within an equivalence
  /// class of type variables, or the fixed type to which this type variable
  /// type is bound.
  llvm::PointerUnion<TypeVariableType *, TypeBase *> ParentOrFixed;

  /// The corresponding node in the constraint graph.
  constraints::ConstraintGraphNode *GraphNode = nullptr;

  ///  Index into the list of type variables, as used by the
  ///  constraint graph.
  unsigned GraphIndex;

  friend class constraints::SavedTypeVariableBinding;

public:
  /// Retrieve the type variable associated with this implementation.
  TypeVariableType *getTypeVariable() {
    return reinterpret_cast<TypeVariableType *>(this) - 1;
  }

  /// Retrieve the type variable associated with this implementation.
  const TypeVariableType *getTypeVariable() const {
    return reinterpret_cast<const TypeVariableType *>(this) - 1;
  }

  explicit Implementation(constraints::ConstraintLocator *locator,
                          unsigned options)
    : locator(locator), ParentOrFixed(getTypeVariable()) {
    getTypeVariable()->Bits.TypeVariableType.Options = options;
  }

  /// Retrieve the unique ID corresponding to this type variable.
  unsigned getID() const { return getTypeVariable()->getID(); }

  unsigned getRawOptions() const {
    return getTypeVariable()->Bits.TypeVariableType.Options;
  }

  void setRawOptions(unsigned bits) {
    getTypeVariable()->Bits.TypeVariableType.Options = bits;
    assert(getTypeVariable()->Bits.TypeVariableType.Options == bits
           && "Trucation");
  }

  /// Whether this type variable can bind to an lvalue type.
  bool canBindToLValue() const { return getRawOptions() & TVO_CanBindToLValue; }

  /// Whether this type variable can bind to an inout type.
  bool canBindToInOut() const { return getRawOptions() & TVO_CanBindToInOut; }

  /// Whether this type variable can bind to an inout type.
  bool canBindToNoEscape() const { return getRawOptions() & TVO_CanBindToNoEscape; }

  /// Whether this type variable can bind to a hole.
  bool canBindToHole() const { return getRawOptions() & TVO_CanBindToHole; }

  /// Whether this type variable prefers a subtype binding over a supertype
  /// binding.
  bool prefersSubtypeBinding() const {
    return getRawOptions() & TVO_PrefersSubtypeBinding;
  }

  /// Retrieve the corresponding node in the constraint graph.
  constraints::ConstraintGraphNode *getGraphNode() const { return GraphNode; }

  /// Set the corresponding node in the constraint graph.
  void setGraphNode(constraints::ConstraintGraphNode *newNode) { 
    GraphNode = newNode; 
  }

  /// Retrieve the index into the constraint graph's list of type variables.
  unsigned getGraphIndex() const { 
    assert(GraphNode && "Graph node isn't set");
    return GraphIndex;
  }

  /// Set the index into the constraint graph's list of type variables.
  void setGraphIndex(unsigned newIndex) {
    GraphIndex = newIndex;
  }
  
  /// Check whether this type variable either has a representative that
  /// is not itself or has a fixed type binding.
  bool hasRepresentativeOrFixed() const {
    // If we have a fixed type, we're done.
    if (!ParentOrFixed.is<TypeVariableType *>())
      return true;

    // Check whether the representative is different from our own type
    // variable.
    return ParentOrFixed.get<TypeVariableType *>() != getTypeVariable();
  }

  /// Record the current type-variable binding.
  void recordBinding(constraints::SavedTypeVariableBindings &record) {
    record.push_back(constraints::SavedTypeVariableBinding(getTypeVariable()));
  }

  /// Retrieve the locator describing where this type variable was
  /// created.
  constraints::ConstraintLocator *getLocator() const {
    return locator;
  }

  /// Retrieve the generic parameter opened by this type variable.
  GenericTypeParamType *getGenericParameter() const;

  /// Returns the \c ExprKind of this type variable if it's the type of an
  /// atomic literal expression, meaning the literal can't be composed of subexpressions.
  /// Otherwise, returns \c None.
  Optional<ExprKind> getAtomicLiteralKind() const;

  /// Determine whether this type variable represents a closure type.
  bool isClosureType() const;

  /// Determine whether this type variable represents one of the
  /// parameter types associated with a closure.
  bool isClosureParameterType() const;

  /// Determine whether this type variable represents a closure result type.
  bool isClosureResultType() const;

  /// Determine whether this type variable represents
  /// a type of a key path expression.
  bool isKeyPathType() const;

  /// Retrieve the representative of the equivalence class to which this
  /// type variable belongs.
  ///
  /// \param record The record of changes made by retrieving the representative,
  /// which can happen due to path compression. If null, path compression is
  /// not performed.
  TypeVariableType *
  getRepresentative(constraints::SavedTypeVariableBindings *record) {
    // Find the representative type variable.
    auto result = getTypeVariable();
    Implementation *impl = this;
    while (impl->ParentOrFixed.is<TypeVariableType *>()) {
      // Extract the representative.
      auto nextTV = impl->ParentOrFixed.get<TypeVariableType *>();
      if (nextTV == result)
        break;

      result = nextTV;
      impl = &nextTV->getImpl();
    }

    if (impl == this || !record)
      return result;

    // Perform path compression.
    impl = this;
    while (impl->ParentOrFixed.is<TypeVariableType *>()) {
      // Extract the representative.
      auto nextTV = impl->ParentOrFixed.get<TypeVariableType *>();
      if (nextTV == result)
        break;

      // Record the state change.
      impl->recordBinding(*record);

      impl->ParentOrFixed = result;
      impl = &nextTV->getImpl();
    }

    return result;
  }

  /// Merge the equivalence class of this type variable with the
  /// equivalence class of another type variable.
  ///
  /// \param other The type variable to merge with.
  ///
  /// \param record The record of state changes.
  void mergeEquivalenceClasses(TypeVariableType *other,
                               constraints::SavedTypeVariableBindings *record) {
    // Merge the equivalence classes corresponding to these two type
    // variables. Always merge 'up' the constraint stack, because it is simpler.
    if (getID() > other->getImpl().getID()) {
      other->getImpl().mergeEquivalenceClasses(getTypeVariable(), record);
      return;
    }

    auto otherRep = other->getImpl().getRepresentative(record);
    if (record)
      otherRep->getImpl().recordBinding(*record);
    otherRep->getImpl().ParentOrFixed = getTypeVariable();

    if (canBindToLValue() && !otherRep->getImpl().canBindToLValue()) {
      if (record)
        recordBinding(*record);
      getTypeVariable()->Bits.TypeVariableType.Options &= ~TVO_CanBindToLValue;
    }

    if (canBindToInOut() && !otherRep->getImpl().canBindToInOut()) {
      if (record)
        recordBinding(*record);
      getTypeVariable()->Bits.TypeVariableType.Options &= ~TVO_CanBindToInOut;
    }

    if (canBindToNoEscape() && !otherRep->getImpl().canBindToNoEscape()) {
      if (record)
        recordBinding(*record);
      getTypeVariable()->Bits.TypeVariableType.Options &= ~TVO_CanBindToNoEscape;
    }
  }

  /// Retrieve the fixed type that corresponds to this type variable,
  /// if there is one.
  ///
  /// \returns the fixed type associated with this type variable, or a null
  /// type if there is no fixed type.
  ///
  /// \param record The record of changes made by retrieving the representative,
  /// which can happen due to path compression. If null, path compression is
  /// not performed.
  Type getFixedType(constraints::SavedTypeVariableBindings *record) {
    // Find the representative type variable.
    auto rep = getRepresentative(record);
    Implementation &repImpl = rep->getImpl();

    // Return the bound type if there is one, otherwise, null.
    return repImpl.ParentOrFixed.dyn_cast<TypeBase *>();
  }

  /// Assign a fixed type to this equivalence class.
  void assignFixedType(Type type,
                       constraints::SavedTypeVariableBindings *record) {
    assert((!getFixedType(0) || getFixedType(0)->isEqual(type)) &&
           "Already has a fixed type!");
    auto rep = getRepresentative(record);
    if (record)
      rep->getImpl().recordBinding(*record);
    rep->getImpl().ParentOrFixed = type.getPointer();
  }

  void setCanBindToLValue(constraints::SavedTypeVariableBindings *record,
                          bool enabled) {
    auto &impl = getRepresentative(record)->getImpl();
    if (record)
      impl.recordBinding(*record);

    if (enabled)
      impl.getTypeVariable()->Bits.TypeVariableType.Options |=
          TVO_CanBindToLValue;
    else
      impl.getTypeVariable()->Bits.TypeVariableType.Options &=
          ~TVO_CanBindToLValue;
  }

  void setCanBindToNoEscape(constraints::SavedTypeVariableBindings *record,
                            bool enabled) {
    auto &impl = getRepresentative(record)->getImpl();
    if (record)
      impl.recordBinding(*record);

    if (enabled)
      impl.getTypeVariable()->Bits.TypeVariableType.Options |=
          TVO_CanBindToNoEscape;
    else
      impl.getTypeVariable()->Bits.TypeVariableType.Options &=
          ~TVO_CanBindToNoEscape;
  }

  void enableCanBindToHole(constraints::SavedTypeVariableBindings *record) {
    auto &impl = getRepresentative(record)->getImpl();
    if (record)
      impl.recordBinding(*record);

    impl.getTypeVariable()->Bits.TypeVariableType.Options |= TVO_CanBindToHole;
  }

  /// Print the type variable to the given output stream.
  void print(llvm::raw_ostream &OS);
};

namespace constraints {

template <typename T = Expr> T *castToExpr(ASTNode node) {
  return cast<T>(node.get<Expr *>());
}

template <typename T = Expr> T *getAsExpr(ASTNode node) {
  if (node.isNull())
    return nullptr;

  if (auto *E = node.dyn_cast<Expr *>())
    return dyn_cast_or_null<T>(E);

  return nullptr;
}

template <typename T> bool isExpr(ASTNode node) {
  if (node.isNull() || !node.is<Expr *>())
    return false;

  auto *E = node.get<Expr *>();
  return isa<T>(E);
}

template <typename T = Decl> T *getAsDecl(ASTNode node) {
  if (auto *E = node.dyn_cast<Decl *>())
    return dyn_cast_or_null<T>(E);
  return nullptr;
}

SourceLoc getLoc(ASTNode node);
SourceRange getSourceRange(ASTNode node);

/// The result of comparing two constraint systems that are a solutions
/// to the given set of constraints.
enum class SolutionCompareResult {
  /// The two solutions are incomparable, because, e.g., because one
  /// solution has some better decisions and some worse decisions than the
  /// other.
  Incomparable,
  /// The two solutions are identical.
  Identical,
  /// The first solution is better than the second.
  Better,
  /// The second solution is better than the first.
  Worse
};

/// An overload that has been selected in a particular solution.
///
/// A selected overload captures the specific overload choice (e.g., a
/// particular declaration) as well as the type to which the reference to the
/// declaration was opened, which may involve type variables.
struct SelectedOverload {
  /// The overload choice.
  const OverloadChoice choice;

  /// The opened type of the base of the reference to this overload, if
  /// we're referencing a member.
  const Type openedFullType;

  /// The opened type produced by referring to this overload.
  const Type openedType;

  /// The type that this overload binds. Note that this may differ from
  /// openedType, for example it will include any IUO unwrapping that has taken
  /// place.
  const Type boundType;
};

/// Provides information about the application of a function argument to a
/// parameter.
class FunctionArgApplyInfo {
  Expr *ArgListExpr;
  Expr *ArgExpr;
  unsigned ArgIdx;
  Type ArgType;

  unsigned ParamIdx;

  Type FnInterfaceType;
  FunctionType *FnType;
  const ValueDecl *Callee;

public:
  FunctionArgApplyInfo(Expr *argListExpr, Expr *argExpr, unsigned argIdx,
                       Type argType, unsigned paramIdx, Type fnInterfaceType,
                       FunctionType *fnType, const ValueDecl *callee)
      : ArgListExpr(argListExpr), ArgExpr(argExpr), ArgIdx(argIdx),
        ArgType(argType), ParamIdx(paramIdx), FnInterfaceType(fnInterfaceType),
        FnType(fnType), Callee(callee) {}

  /// \returns The list of the arguments used for this application.
  Expr *getArgListExpr() const { return ArgListExpr; }

  /// \returns The argument being applied.
  Expr *getArgExpr() const { return ArgExpr; }

  /// \returns The position of the argument, starting at 1.
  unsigned getArgPosition() const { return ArgIdx + 1; }

  /// \returns The position of the parameter, starting at 1.
  unsigned getParamPosition() const { return ParamIdx + 1; }

  /// \returns The type of the argument being applied, including any generic
  /// substitutions.
  ///
  /// \param withSpecifier Whether to keep the inout or @lvalue specifier of
  /// the argument, if any.
  Type getArgType(bool withSpecifier = false) const {
    return withSpecifier ? ArgType : ArgType->getWithoutSpecifierType();
  }

  /// \returns The label for the argument being applied.
  Identifier getArgLabel() const {
    if (auto *te = dyn_cast<TupleExpr>(ArgListExpr))
      return te->getElementName(ArgIdx);

    assert(isa<ParenExpr>(ArgListExpr));
    return Identifier();
  }

  Identifier getParamLabel() const {
    auto param = FnType->getParams()[ParamIdx];
    return param.getLabel();
  }

  /// \returns A textual description of the argument suitable for diagnostics.
  /// For an argument with an unambiguous label, this will the label. Otherwise
  /// it will be its position in the argument list.
  StringRef getArgDescription(SmallVectorImpl<char> &scratch) const {
    llvm::raw_svector_ostream stream(scratch);

    // Use the argument label only if it's unique within the argument list.
    auto argLabel = getArgLabel();
    auto useArgLabel = [&]() -> bool {
      if (argLabel.empty())
        return false;

      if (auto *te = dyn_cast<TupleExpr>(ArgListExpr))
        return llvm::count(te->getElementNames(), argLabel) == 1;

      return false;
    };

    if (useArgLabel()) {
      stream << "'";
      stream << argLabel;
      stream << "'";
    } else {
      stream << "#";
      stream << getArgPosition();
    }
    return StringRef(scratch.data(), scratch.size());
  }

  /// Whether the argument is a trailing closure.
  bool isTrailingClosure() const {
    if (auto trailingClosureArg =
            ArgListExpr->getUnlabeledTrailingClosureIndexOfPackedArgument())
      return ArgIdx >= *trailingClosureArg;

    return false;
  }

  /// \returns The interface type for the function being applied. Note that this
  /// may not a function type, for example it could be a generic parameter.
  Type getFnInterfaceType() const { return FnInterfaceType; }

  /// \returns The function type being applied, including any generic
  /// substitutions.
  FunctionType *getFnType() const { return FnType; }

  /// \returns The callee for the application.
  const ValueDecl *getCallee() const { return Callee; }

private:
  Type getParamTypeImpl(AnyFunctionType *fnTy,
                        bool lookThroughAutoclosure) const {
    auto param = fnTy->getParams()[ParamIdx];
    auto paramTy = param.getPlainType();
    if (lookThroughAutoclosure && param.isAutoClosure())
      paramTy = paramTy->castTo<FunctionType>()->getResult();
    return paramTy;
  }

public:
  /// \returns The type of the parameter which the argument is being applied to,
  /// including any generic substitutions.
  ///
  /// \param lookThroughAutoclosure Whether an @autoclosure () -> T parameter
  /// should be treated as being of type T.
  Type getParamType(bool lookThroughAutoclosure = true) const {
    return getParamTypeImpl(FnType, lookThroughAutoclosure);
  }

  /// \returns The interface type of the parameter which the argument is being
  /// applied to.
  ///
  /// \param lookThroughAutoclosure Whether an @autoclosure () -> T parameter
  /// should be treated as being of type T.
  Type getParamInterfaceType(bool lookThroughAutoclosure = true) const {
    auto interfaceFnTy = FnInterfaceType->getAs<AnyFunctionType>();
    if (!interfaceFnTy) {
      // If the interface type isn't a function, then just return the resolved
      // parameter type.
      return getParamType(lookThroughAutoclosure)->mapTypeOutOfContext();
    }
    return getParamTypeImpl(interfaceFnTy, lookThroughAutoclosure);
  }

  /// \returns The flags of the parameter which the argument is being applied
  /// to.
  ParameterTypeFlags getParameterFlags() const {
    return FnType->getParams()[ParamIdx].getParameterFlags();
  }

  ParameterTypeFlags getParameterFlagsAtIndex(unsigned idx) const {
    return FnType->getParams()[idx].getParameterFlags();
  }
};

/// Describes an aspect of a solution that affects its overall score, i.e., a
/// user-defined conversions.
enum ScoreKind {
  // These values are used as indices into a Score value.

  /// A fix needs to be applied to the source.
  SK_Fix,
  /// A hole in the constraint system.
  SK_Hole,
  /// A reference to an @unavailable declaration.
  SK_Unavailable,
  /// A reference to an async function in a synchronous context.
  SK_AsyncInSyncMismatch,
  /// Synchronous function in an asynchronous context or a conversion of
  /// a synchronous function to an asynchronous one.
  SK_SyncInAsync,
  /// A use of the "forward" scan for trailing closures.
  SK_ForwardTrailingClosure,
  /// A use of a disfavored overload.
  SK_DisfavoredOverload,
  /// A member for an \c UnresolvedMemberExpr found via unwrapped optional base.
  SK_UnresolvedMemberViaOptional,
  /// An implicit force of an implicitly unwrapped optional value.
  SK_ForceUnchecked,
  /// An implicit conversion from a value of one type (lhs)
  /// to another type (rhs) via implicit initialization of
  /// `rhs` type with an argument of `lhs` value.
  SK_ImplicitValueConversion,
  /// A user-defined conversion.
  SK_UserConversion,
  /// A non-trivial function conversion.
  SK_FunctionConversion,
  /// A literal expression bound to a non-default literal type.
  SK_NonDefaultLiteral,
  /// An implicit upcast conversion between collection types.
  SK_CollectionUpcastConversion,
  /// A value-to-optional conversion.
  SK_ValueToOptional,
  /// A conversion to an empty existential type ('Any' or '{}').
  SK_EmptyExistentialConversion,
  /// A key path application subscript.
  SK_KeyPathSubscript,
  /// A conversion from a string, array, or inout to a pointer.
  SK_ValueToPointerConversion,
  /// A closure/function conversion to an autoclosure parameter.
  SK_FunctionToAutoClosureConversion,

  SK_LastScoreKind = SK_FunctionToAutoClosureConversion,
};

/// The number of score kinds.
const unsigned NumScoreKinds = SK_LastScoreKind + 1;

/// Describes what happened when a result builder transform was applied
/// to a particular closure.
struct AppliedBuilderTransform {
  /// The builder type that was applied to the closure.
  Type builderType;

  /// The result type of the body, to which the returned expression will be
  /// converted.
  Type bodyResultType;

  /// An expression whose value has been recorded for later use.
  struct RecordedExpr {
    /// The temporary value that captures the value of the expression, if
    /// there is one.
    VarDecl *temporaryVar;

    /// The expression that results from generating constraints with this
    /// particular builder.
    Expr *generatedExpr;
  };

  /// A mapping from expressions whose values are captured by the builder
  /// to information about the temporary variable capturing the
  llvm::DenseMap<Expr *, RecordedExpr> capturedExprs;

  /// A mapping from statements to a pair containing the implicit variable
  /// declaration that captures the result of that expression, and the
  /// set of expressions that can be used to produce a value for that
  /// variable.
  llvm::DenseMap<Stmt *, std::pair<VarDecl *, llvm::TinyPtrVector<Expr *>>>
      capturedStmts;

  /// The return expression, capturing the last value to be emitted.
  Expr *returnExpr = nullptr;
};

/// Describes the fixed score of a solution to the constraint system.
struct Score {
  unsigned Data[NumScoreKinds] = {};

  friend Score &operator+=(Score &x, const Score &y) {
    for (unsigned i = 0; i != NumScoreKinds; ++i) {
      x.Data[i] += y.Data[i];
    }
    return x;
  }

  friend Score operator+(const Score &x, const Score &y) {
    Score result;
    for (unsigned i = 0; i != NumScoreKinds; ++i) {
      result.Data[i] = x.Data[i] + y.Data[i];
    }
    return result;
  }

  friend Score operator-(const Score &x, const Score &y) {
    Score result;
    for (unsigned i = 0; i != NumScoreKinds; ++i) {
      result.Data[i] = x.Data[i] - y.Data[i];
    }
    return result;
  }

  friend Score &operator-=(Score &x, const Score &y) {
    for (unsigned i = 0; i != NumScoreKinds; ++i) {
      x.Data[i] -= y.Data[i];
    }
    return x;
  }

  friend bool operator==(const Score &x, const Score &y) {
    for (unsigned i = 0; i != NumScoreKinds; ++i) {
      if (x.Data[i] != y.Data[i])
        return false;
    }

    return true;
  }

  friend bool operator!=(const Score &x, const Score &y) {
    return !(x == y);
  }

  friend bool operator<(const Score &x, const Score &y) {
    for (unsigned i = 0; i != NumScoreKinds; ++i) {
      if (x.Data[i] < y.Data[i])
        return true;

      if (x.Data[i] > y.Data[i])
        return false;
    }

    return false;
  }

  friend bool operator<=(const Score &x, const Score &y) {
    return !(y < x);
  }

  friend bool operator>(const Score &x, const Score &y) {
    return y < x;
  }

  friend bool operator>=(const Score &x, const Score &y) {
    return !(x < y);
  }

};

/// Display a score.
llvm::raw_ostream &operator<<(llvm::raw_ostream &out, const Score &score);

/// Describes a dependent type that has been opened to a particular type
/// variable.
using OpenedType = std::pair<GenericTypeParamType *, TypeVariableType *>;

using OpenedTypeMap =
    llvm::DenseMap<GenericTypeParamType *, TypeVariableType *>;

/// Describes contextual type information about a particular expression
/// within a constraint system.
struct ContextualTypeInfo {
  TypeLoc typeLoc;
  ContextualTypePurpose purpose;

  ContextualTypeInfo() : typeLoc(TypeLoc()), purpose(CTP_Unused) {}

  ContextualTypeInfo(Type contextualTy, ContextualTypePurpose purpose)
      : typeLoc(TypeLoc::withoutLoc(contextualTy)), purpose(purpose) {}

  ContextualTypeInfo(TypeLoc typeLoc, ContextualTypePurpose purpose)
      : typeLoc(typeLoc), purpose(purpose) {}

  Type getType() const { return typeLoc.getType(); }
};

/// Describes the information about a case label item that needs to be tracked
/// within the constraint system.
struct CaseLabelItemInfo {
  Pattern *pattern;
  Expr *guardExpr;
};

/// Describes information about a for-each loop that needs to be tracked
/// within the constraint system.
struct ForEachStmtInfo {
  ForEachStmt *stmt;

  /// The type of the sequence.
  Type sequenceType;

  /// The type of the iterator.
  Type iteratorType;

  /// The type of an element in the sequence.
  Type elementType;

  /// The type of the pattern that matches the elements.
  Type initType;

  /// The "where" expression, if there is one.
  Expr *whereExpr;
};

/// Key to the constraint solver's mapping from AST nodes to their corresponding
/// solution application targets.
class SolutionApplicationTargetsKey {
public:
  enum class Kind {
    empty,
    tombstone,
    stmtCondElement,
    stmt,
    patternBindingEntry,
    varDecl,
  };

private:
  Kind kind;

  union {
    const StmtConditionElement *stmtCondElement;

    const Stmt *stmt;

    struct PatternBindingEntry {
      const PatternBindingDecl *patternBinding;
      unsigned index;
    } patternBindingEntry;

    const VarDecl *varDecl;
  } storage;

public:
  SolutionApplicationTargetsKey(Kind kind) {
    assert(kind == Kind::empty || kind == Kind::tombstone);
    this->kind = kind;
  }

  SolutionApplicationTargetsKey(const StmtConditionElement *stmtCondElement) {
    kind = Kind::stmtCondElement;
    storage.stmtCondElement = stmtCondElement;
  }

  SolutionApplicationTargetsKey(const Stmt *stmt) {
    kind = Kind::stmt;
    storage.stmt = stmt;
  }

  SolutionApplicationTargetsKey(
      const PatternBindingDecl *patternBinding, unsigned index) {
    kind = Kind::stmt;
    storage.patternBindingEntry.patternBinding = patternBinding;
    storage.patternBindingEntry.index = index;
  }

  SolutionApplicationTargetsKey(const VarDecl *varDecl) {
    kind = Kind::varDecl;
    storage.varDecl = varDecl;
  }

  friend bool operator==(
      SolutionApplicationTargetsKey lhs, SolutionApplicationTargetsKey rhs) {
    if (lhs.kind != rhs.kind)
      return false;

    switch (lhs.kind) {
    case Kind::empty:
    case Kind::tombstone:
      return true;

    case Kind::stmtCondElement:
      return lhs.storage.stmtCondElement == rhs.storage.stmtCondElement;

    case Kind::stmt:
      return lhs.storage.stmt == rhs.storage.stmt;

    case Kind::patternBindingEntry:
      return (lhs.storage.patternBindingEntry.patternBinding
                == rhs.storage.patternBindingEntry.patternBinding) &&
          (lhs.storage.patternBindingEntry.index
             == rhs.storage.patternBindingEntry.index);

    case Kind::varDecl:
      return lhs.storage.varDecl == rhs.storage.varDecl;
    }
    llvm_unreachable("invalid SolutionApplicationTargetsKey kind");
  }

  friend bool operator!=(
      SolutionApplicationTargetsKey lhs, SolutionApplicationTargetsKey rhs) {
    return !(lhs == rhs);
  }

  unsigned getHashValue() const {
    using llvm::hash_combine;
    using llvm::DenseMapInfo;

    switch (kind) {
    case Kind::empty:
    case Kind::tombstone:
      return llvm::DenseMapInfo<unsigned>::getHashValue(static_cast<unsigned>(kind));

    case Kind::stmtCondElement:
      return hash_combine(
          DenseMapInfo<unsigned>::getHashValue(static_cast<unsigned>(kind)),
          DenseMapInfo<void *>::getHashValue(storage.stmtCondElement));

    case Kind::stmt:
      return hash_combine(
          DenseMapInfo<unsigned>::getHashValue(static_cast<unsigned>(kind)),
          DenseMapInfo<void *>::getHashValue(storage.stmt));

    case Kind::patternBindingEntry:
      return hash_combine(
          DenseMapInfo<unsigned>::getHashValue(static_cast<unsigned>(kind)),
          DenseMapInfo<void *>::getHashValue(
              storage.patternBindingEntry.patternBinding),
          DenseMapInfo<unsigned>::getHashValue(
              storage.patternBindingEntry.index));

    case Kind::varDecl:
      return hash_combine(
          DenseMapInfo<unsigned>::getHashValue(static_cast<unsigned>(kind)),
          DenseMapInfo<void *>::getHashValue(storage.varDecl));
    }
    llvm_unreachable("invalid statement kind");
  }
};

/// Describes the arguments to which a parameter binds.
/// FIXME: This is an awful data structure. We want the equivalent of a
/// TinyPtrVector for unsigned values.
using ParamBinding = SmallVector<unsigned, 1>;

/// The result of calling matchCallArguments().
struct MatchCallArgumentResult {
  /// The direction of trailing closure matching that was performed.
  TrailingClosureMatching trailingClosureMatching;

  /// The parameter bindings determined by the match.
  SmallVector<ParamBinding, 4> parameterBindings;

  /// When present, the forward and backward scans each produced a result,
  /// and the parameter bindings are different. The primary result will be
  /// forwarding, and this represents the backward binding.
  Optional<SmallVector<ParamBinding, 4>> backwardParameterBindings;

  friend bool operator==(const MatchCallArgumentResult &lhs,
                         const MatchCallArgumentResult &rhs) {
    if (lhs.trailingClosureMatching != rhs.trailingClosureMatching)
      return false;
    if (lhs.parameterBindings != rhs.parameterBindings)
      return false;
    return lhs.backwardParameterBindings == rhs.backwardParameterBindings;
  }

  /// Generate a result that maps the provided number of arguments to the same
  /// number of parameters via forward match.
  static MatchCallArgumentResult forArity(unsigned argCount) {
    SmallVector<ParamBinding, 4> Bindings;
    for (unsigned i : range(argCount))
      Bindings.push_back({i});
    return {TrailingClosureMatching::Forward, Bindings, None};
  }
};

/// A complete solution to a constraint system.
///
/// A solution to a constraint system consists of type variable bindings to
/// concrete types for every type variable that is used in the constraint
/// system along with a set of mappings from each constraint locator
/// involving an overload set to the selected overload.
class Solution {
  /// The constraint system this solution solves.
  ConstraintSystem *constraintSystem;

  /// The fixed score for this solution.
  Score FixedScore;

public:
  /// Create a solution for the given constraint system.
  Solution(ConstraintSystem &cs, const Score &score)
    : constraintSystem(&cs), FixedScore(score) {}

  // Solution is a non-copyable type for performance reasons.
  Solution(const Solution &other) = delete;
  Solution &operator=(const Solution &other) = delete;

  Solution(Solution &&other) = default;
  Solution &operator=(Solution &&other) = default;

  size_t getTotalMemory() const;

  /// Retrieve the constraint system that this solution solves.
  ConstraintSystem &getConstraintSystem() const { return *constraintSystem; }

  DeclContext *getDC() const;

  /// The set of type bindings.
  llvm::DenseMap<TypeVariableType *, Type> typeBindings;
  
  /// The set of overload choices along with their types.
  llvm::DenseMap<ConstraintLocator *, SelectedOverload> overloadChoices;

  /// The set of constraint restrictions used to arrive at this restriction,
  /// which informs constraint application.
  llvm::DenseMap<std::pair<CanType, CanType>, ConversionRestrictionKind>
    ConstraintRestrictions;

  /// The list of fixes that need to be applied to the initial expression
  /// to make the solution work.
  llvm::SmallVector<ConstraintFix *, 4> Fixes;

  /// For locators associated with call expressions, the trailing closure
  /// matching rule and parameter bindings that were applied.
  llvm::SmallMapVector<ConstraintLocator *, MatchCallArgumentResult, 4>
      argumentMatchingChoices;

  /// The set of disjunction choices used to arrive at this solution,
  /// which informs constraint application.
  llvm::DenseMap<ConstraintLocator *, unsigned> DisjunctionChoices;

  /// The set of opened types for a given locator.
  llvm::DenseMap<ConstraintLocator *, ArrayRef<OpenedType>> OpenedTypes;

  /// The opened existential type for a given locator.
  llvm::DenseMap<ConstraintLocator *, OpenedArchetypeType *>
    OpenedExistentialTypes;

  /// The locators of \c Defaultable constraints whose defaults were used.
  llvm::SmallPtrSet<ConstraintLocator *, 2> DefaultedConstraints;

  /// Implicit value conversions applied for a given locator.
  SmallVector<std::pair<ConstraintLocator *, ConversionRestrictionKind>, 2>
      ImplicitValueConversions;

  /// The node -> type mappings introduced by this solution.
  llvm::DenseMap<ASTNode, Type> nodeTypes;

  /// Contextual types introduced by this solution.
  std::vector<std::pair<ASTNode, ContextualTypeInfo>> contextualTypes;

  /// Maps AST nodes to their solution application targets.
  llvm::MapVector<SolutionApplicationTargetsKey, SolutionApplicationTarget>
    solutionApplicationTargets;

  /// Maps case label items to information tracked about them as they are
  /// being solved.
  llvm::SmallMapVector<const CaseLabelItem *, CaseLabelItemInfo, 4>
      caseLabelItems;

  /// The set of functions that have been transformed by a result builder.
  llvm::MapVector<AnyFunctionRef, AppliedBuilderTransform>
      resultBuilderTransformed;

  /// A map from argument expressions to their applied property wrapper expressions.
  llvm::MapVector<ASTNode, SmallVector<AppliedPropertyWrapper, 2>> appliedPropertyWrappers;

  /// Record a new argument matching choice for given locator that maps a
  /// single argument to a single parameter.
  void recordSingleArgMatchingChoice(ConstraintLocator *locator);

  /// Simplify the given type by substituting all occurrences of
  /// type variables for their fixed types.
  Type simplifyType(Type type) const;

  /// Coerce the given expression to the given type.
  ///
  /// This operation cannot fail.
  ///
  /// \param expr The expression to coerce.
  /// \param toType The type to coerce the expression to.
  /// \param locator Locator used to describe the location of this expression.
  ///
  /// \param typeFromPattern Optionally, the caller can specify the pattern
  /// from where the toType is derived, so that we can deliver better fixit.
  ///
  /// \returns the coerced expression, which will have type \c ToType.
  Expr *coerceToType(Expr *expr, Type toType,
                     ConstraintLocator *locator,
                     Optional<Pattern*> typeFromPattern = None);

  /// Compute the set of substitutions for a generic signature opened at the
  /// given locator.
  ///
  /// \param sig The generic signature.
  ///
  /// \param locator The locator that describes where the substitutions came
  /// from.
  SubstitutionMap computeSubstitutions(GenericSignature sig,
                                       ConstraintLocator *locator) const;

  /// Resolves the contextual substitutions for a reference to a declaration
  /// at a given locator.
  ConcreteDeclRef
  resolveConcreteDeclRef(ValueDecl *decl, ConstraintLocator *locator) const;

  /// Return the disjunction choice for the given constraint location.
  unsigned getDisjunctionChoice(ConstraintLocator *locator) const {
    assert(DisjunctionChoices.count(locator));
    return DisjunctionChoices.find(locator)->second;
  }

  /// Retrieve the fixed score of this solution
  const Score &getFixedScore() const { return FixedScore; }

  /// Retrieve the fixed score of this solution
  Score &getFixedScore() { return FixedScore; }

  /// Retrieve the fixed type for the given type variable.
  Type getFixedType(TypeVariableType *typeVar) const;

  /// Try to resolve the given locator to a declaration within this
  /// solution. Note that this only returns a decl for a direct reference such
  /// as \c x.foo and will not return a decl for \c x.foo().
  ConcreteDeclRef resolveLocatorToDecl(ConstraintLocator *locator) const;

  /// Retrieve the overload choice associated with the given
  /// locator.
  SelectedOverload getOverloadChoice(ConstraintLocator *locator) const {
    return *getOverloadChoiceIfAvailable(locator);
  }

  /// Retrieve the overload choice associated with the given
  /// locator.
  Optional<SelectedOverload>
  getOverloadChoiceIfAvailable(ConstraintLocator *locator) const {
    auto known = overloadChoices.find(locator);
    if (known != overloadChoices.end())
      return known->second;
    return None;
  }

  ConstraintLocator *getCalleeLocator(ConstraintLocator *locator,
                                      bool lookThroughApply = true) const;

  ConstraintLocator *
  getConstraintLocator(ASTNode anchor,
                       ArrayRef<LocatorPathElt> path = {}) const;

  ConstraintLocator *getConstraintLocator(ConstraintLocator *baseLocator,
                                          ArrayRef<LocatorPathElt> path) const;

  void setExprTypes(Expr *expr) const;

  bool hasType(ASTNode node) const;

  /// Retrieve the type of the given node, as recorded in this solution.
  Type getType(ASTNode node) const;

  /// Retrieve the type of the given node as recorded in this solution
  /// and resolve all of the type variables in contains to form a fully
  /// "resolved" concrete type.
  Type getResolvedType(ASTNode node) const;

  /// Resolve type variables present in the raw type, using generic parameter
  /// types where possible.
  Type resolveInterfaceType(Type type) const;

  /// For a given locator describing a function argument conversion, or a
  /// constraint within an argument conversion, returns information about the
  /// application of the argument to its parameter. If the locator is not
  /// for an argument conversion, returns \c None.
  Optional<FunctionArgApplyInfo>
  getFunctionArgApplyInfo(ConstraintLocator *) const;

  /// Retrieve the builder transform that was applied to this function, if any.
  const AppliedBuilderTransform *getAppliedBuilderTransform(
     AnyFunctionRef fn) const {
    auto known = resultBuilderTransformed.find(fn);
    return known != resultBuilderTransformed.end()
        ? &known->second
        : nullptr;
  }

  /// This method implements functionality of `Expr::isTypeReference`
  /// with data provided by a given solution.
  bool isTypeReference(Expr *E) const;

  /// Call Expr::isIsStaticallyDerivedMetatype on the given
  /// expression, using a custom accessor for the type on the
  /// expression that reads the type from the Solution
  /// expression type map.
  bool isStaticallyDerivedMetatype(Expr *E) const;

  SWIFT_DEBUG_DUMP;

  /// Dump this solution.
  void dump(raw_ostream &OS) const LLVM_ATTRIBUTE_USED;
};

/// Describes the differences between several solutions to the same
/// constraint system.
class SolutionDiff {
public:
  /// A difference between two overloads.
  struct OverloadDiff {
    /// The locator that describes where the overload comes from.
    ConstraintLocator *locator;

    /// The choices that each solution made.
    SmallVector<OverloadChoice, 2> choices;
  };

  /// The differences between the overload choices between the
  /// solutions.
  SmallVector<OverloadDiff, 4> overloads;

  /// Compute the differences between the given set of solutions.
  ///
  /// \param solutions The set of solutions.
  explicit SolutionDiff(ArrayRef<Solution> solutions);
};

/// An intrusive, doubly-linked list of constraints.
using ConstraintList = llvm::ilist<Constraint>;

enum class ConstraintSystemFlags {
  /// Whether we allow the solver to attempt fixes to the system.
  AllowFixes = 0x01,

  /// Set if the client wants diagnostics suppressed.
  SuppressDiagnostics = 0x02,

  /// If set, the client wants a best-effort solution to the constraint system,
  /// but can tolerate a solution where all of the constraints are solved, but
  /// not all type variables have been determined.  In this case, the constraint
  /// system is not applied to the expression AST, but the ConstraintSystem is
  /// left in-tact.
  AllowUnresolvedTypeVariables = 0x04,

  /// If set, constraint system always reuses type of pre-typechecked
  /// expression, and doesn't dig into its subexpressions.
  ReusePrecheckedType = 0x08,

  /// If set, verbose output is enabled for this constraint system.
  ///
  /// Note that this flag is automatically applied to all constraint systems
  /// when \c DebugConstraintSolver is set in \c TypeCheckerOptions. It can be
  /// automatically enabled for select constraint solving attempts by setting
  /// \c DebugConstraintSolverAttempt. Finally, it be automatically enabled
  /// for a pre-configured set of expressions on line numbers by setting
  /// \c DebugConstraintSolverOnLines.
  DebugConstraints = 0x10,

  /// Don't try to type check closure bodies, and leave them unchecked. This is
  /// used for source tooling functionalities.
  LeaveClosureBodyUnchecked = 0x20,

  /// If set, we are solving specifically to determine the type of a
  /// CodeCompletionExpr, and should continue in the presence of errors wherever
  /// possible.
  ForCodeCompletion = 0x40,

  /// Include Clang function types when checking equality for function types.
  ///
  /// When LangOptions.UseClangFunctionTypes is set, we can synthesize
  /// different @convention(c) function types with the same parameter and result
  /// types (similarly for blocks). This difference is reflected in the `cType:`
  /// field (@convention(c, cType: ...)). When the cType is different, the types
  /// should be treated as semantically different, as they may have different
  /// calling conventions, say due to Clang attributes such as
  /// `__attribute__((ns_consumed))`.
  UseClangFunctionTypes = 0x80,
};

/// Options that affect the constraint system as a whole.
using ConstraintSystemOptions = OptionSet<ConstraintSystemFlags>;

/// This struct represents the results of a member lookup of
struct MemberLookupResult {
  enum {
    /// This result indicates that we cannot begin to solve this, because the
    /// base expression is a type variable.
    Unsolved,
    
    /// This result indicates that the member reference is erroneous, but was
    /// already diagnosed.  Don't emit another error.
    ErrorAlreadyDiagnosed,
    
    /// This result indicates that the lookup produced candidate lists,
    /// potentially of viable results, potentially of error candidates, and
    /// potentially empty lists, indicating that there were no matches.
    HasResults
  } OverallResult;
  
  /// This is a list of viable candidates that were matched.
  ///
  SmallVector<OverloadChoice, 4> ViableCandidates;
  
  /// If there is a favored candidate in the viable list, this indicates its
  /// index.
  unsigned FavoredChoice = ~0U;

  /// The number of optional unwraps that were applied implicitly in the
  /// lookup, for contexts where that is permitted.
  unsigned numImplicitOptionalUnwraps = 0;

  /// The base lookup type used to find the results, which will be non-null
  /// only when it differs from the provided base type.
  Type actualBaseType;

  /// This enum tracks reasons why a candidate is not viable.
  enum UnviableReason {
    /// This uses a type like Self in its signature that cannot be used on an
    /// existential box.
    UR_UnavailableInExistential,

    /// This is an instance member being accessed through something of metatype
    /// type.
    UR_InstanceMemberOnType,

    /// This is a static/class member being accessed through an instance.
    UR_TypeMemberOnInstance,

    /// This is a mutating member, being used on an rvalue.
    UR_MutatingMemberOnRValue,

    /// The getter for this subscript or computed property is mutating and we
    /// only have an rvalue base.  This is more specific than the former one.
    UR_MutatingGetterOnRValue,

    /// The member is inaccessible (e.g. a private member in another file).
    UR_Inaccessible,

    /// This is a `WritableKeyPath` being used to look up read-only member,
    /// used in situations involving dynamic member lookup via keypath,
    /// because it's not known upfront what access capability would the
    /// member have.
    UR_WritableKeyPathOnReadOnlyMember,

    /// This is a `ReferenceWritableKeyPath` being used to look up mutating
    /// member, used in situations involving dynamic member lookup via keypath,
    /// because it's not known upfront what access capability would the
    /// member have.
    UR_ReferenceWritableKeyPathOnMutatingMember,

    /// This is a KeyPath whose root type is AnyObject
    UR_KeyPathWithAnyObjectRootType,

    /// This is a static member being access through a protocol metatype
    /// but its result type doesn't conform to this protocol.
    UR_InvalidStaticMemberOnProtocolMetatype,
  };

  /// This is a list of considered (but rejected) candidates, along with a
  /// reason for their rejection. Split into separate collections to make
  /// it easier to use in conjunction with viable candidates.
  SmallVector<OverloadChoice, 4> UnviableCandidates;
  SmallVector<UnviableReason, 4> UnviableReasons;

  /// Mark this as being an already-diagnosed error and return itself.
  MemberLookupResult &markErrorAlreadyDiagnosed() {
    OverallResult = ErrorAlreadyDiagnosed;
    return *this;
  }
  
  void addViable(OverloadChoice candidate) {
    ViableCandidates.push_back(candidate);
  }
  
  void addUnviable(OverloadChoice candidate, UnviableReason reason) {
    UnviableCandidates.push_back(candidate);
    UnviableReasons.push_back(reason);
  }

  Optional<unsigned> getFavoredIndex() const {
    return (FavoredChoice == ~0U) ? Optional<unsigned>() : FavoredChoice;
  }
};

/// Stores the required methods for @dynamicCallable types.
struct DynamicCallableMethods {
  llvm::DenseSet<FuncDecl *> argumentsMethods;
  llvm::DenseSet<FuncDecl *> keywordArgumentsMethods;

  void addArgumentsMethod(FuncDecl *method) {
    argumentsMethods.insert(method);
  }

  void addKeywordArgumentsMethod(FuncDecl *method) {
    keywordArgumentsMethods.insert(method);
  }

  /// Returns true if type defines either of the @dynamicCallable
  /// required methods. Returns false iff type does not satisfy @dynamicCallable
  /// requirements.
  bool isValid() const {
    return !argumentsMethods.empty() || !keywordArgumentsMethods.empty();
  }
};

/// Describes the target to which a constraint system's solution can be
/// applied.
class SolutionApplicationTarget {
public:
  enum class Kind {
    expression,
    function,
    stmtCondition,
    caseLabelItem,
    patternBinding,
    uninitializedWrappedVar,
  } kind;

private:
  union {
    struct {
      /// The expression being type-checked.
      Expr *expression;

      /// The declaration context in which the expression is being
      /// type-checked.
      DeclContext *dc;

      /// The purpose of the contextual type.
      ContextualTypePurpose contextualPurpose;

      /// The type to which the expression should be converted.
      TypeLoc convertType;

      /// When initializing a pattern from the expression, this is the
      /// pattern.
      Pattern *pattern;

      struct {
        /// The variable to which property wrappers have been applied, if
        /// this is an initialization involving a property wrapper.
        VarDecl *wrappedVar;

        /// The innermost call to \c init(wrappedValue:), if this is an
        /// initialization involving a property wrapper.
        ApplyExpr *innermostWrappedValueInit;

        /// Whether this property wrapper has an initial wrapped value specified
        /// via \c = .
        bool hasInitialWrappedValue;
      } propertyWrapper;

      /// Whether the expression result will be discarded at the end.
      bool isDiscarded;

      /// Whether to bind the variables encountered within the pattern to
      /// fresh type variables via one-way constraints.
      bool bindPatternVarsOneWay;

      union {
        struct {
          /// The pattern binding declaration for an initialization, if any.
          PatternBindingDecl *patternBinding;

          /// The index into the pattern binding declaration, if any.
          unsigned patternBindingIndex;
        } initialization;

        ForEachStmtInfo forEachStmt;
      };
    } expression;

    struct {
      AnyFunctionRef function;
      BraceStmt *body;
    } function;

    struct {
      StmtCondition stmtCondition;
      DeclContext *dc;
    } stmtCondition;

    struct {
      CaseLabelItem *caseLabelItem;
      DeclContext *dc;
    } caseLabelItem;

    PatternBindingDecl *patternBinding;

    VarDecl *uninitializedWrappedVar;
  };

  // If the pattern contains a single variable that has an attached
  // property wrapper, set up the initializer expression to initialize
  // the backing storage.
  void maybeApplyPropertyWrapper();

public:
  SolutionApplicationTarget(Expr *expr, DeclContext *dc,
                            ContextualTypePurpose contextualPurpose,
                            Type convertType, bool isDiscarded)
      : SolutionApplicationTarget(expr, dc, contextualPurpose,
                                  TypeLoc::withoutLoc(convertType),
                                  isDiscarded) { }

  SolutionApplicationTarget(Expr *expr, DeclContext *dc,
                            ContextualTypePurpose contextualPurpose,
                            TypeLoc convertType, bool isDiscarded);

  SolutionApplicationTarget(AnyFunctionRef fn)
      : SolutionApplicationTarget(fn, fn.getBody()) { }

  SolutionApplicationTarget(StmtCondition stmtCondition, DeclContext *dc) {
    kind = Kind::stmtCondition;
    this->stmtCondition.stmtCondition = stmtCondition;
    this->stmtCondition.dc = dc;
  }

  SolutionApplicationTarget(AnyFunctionRef fn, BraceStmt *body) {
    kind = Kind::function;
    function.function = fn;
    function.body = body;
  }

  SolutionApplicationTarget(CaseLabelItem *caseLabelItem, DeclContext *dc) {
    kind = Kind::caseLabelItem;
    this->caseLabelItem.caseLabelItem = caseLabelItem;
    this->caseLabelItem.dc = dc;
  }

  SolutionApplicationTarget(PatternBindingDecl *patternBinding) {
    kind = Kind::patternBinding;
    this->patternBinding = patternBinding;
  }

  SolutionApplicationTarget(VarDecl *wrappedVar) {
    kind = Kind::uninitializedWrappedVar;
    this->uninitializedWrappedVar= wrappedVar;
  }

  /// Form a target for the initialization of a pattern from an expression.
  static SolutionApplicationTarget forInitialization(
      Expr *initializer, DeclContext *dc, Type patternType, Pattern *pattern,
      bool bindPatternVarsOneWay);

  /// Form a target for the initialization of a pattern binding entry from
  /// an expression.
  static SolutionApplicationTarget forInitialization(
      Expr *initializer, DeclContext *dc, Type patternType,
      PatternBindingDecl *patternBinding, unsigned patternBindingIndex,
      bool bindPatternVarsOneWay);

  /// Form a target for a for-each loop.
  static SolutionApplicationTarget forForEachStmt(
      ForEachStmt *stmt, ProtocolDecl *sequenceProto, DeclContext *dc,
      bool bindPatternVarsOneWay);

  /// Form a target for a property with an attached property wrapper that is
  /// initialized out-of-line.
  static SolutionApplicationTarget forUninitializedWrappedVar(
      VarDecl *wrappedVar);

  Expr *getAsExpr() const {
    switch (kind) {
    case Kind::expression:
      return expression.expression;

    case Kind::function:
    case Kind::stmtCondition:
    case Kind::caseLabelItem:
    case Kind::patternBinding:
    case Kind::uninitializedWrappedVar:
      return nullptr;
    }
    llvm_unreachable("invalid expression type");
  }

  DeclContext *getDeclContext() const {
    switch (kind) {
    case Kind::expression:
      return expression.dc;

    case Kind::function:
      return function.function.getAsDeclContext();

    case Kind::stmtCondition:
      return stmtCondition.dc;

    case Kind::caseLabelItem:
      return caseLabelItem.dc;

    case Kind::patternBinding:
      return patternBinding->getDeclContext();

    case Kind::uninitializedWrappedVar:
      return uninitializedWrappedVar->getDeclContext();
    }
    llvm_unreachable("invalid decl context type");
  }

  ContextualTypePurpose getExprContextualTypePurpose() const {
    assert(kind == Kind::expression);
    return expression.contextualPurpose;
  }

  Type getExprContextualType() const {
    return getExprContextualTypeLoc().getType();
  }

  TypeLoc getExprContextualTypeLoc() const {
    assert(kind == Kind::expression);

    // For an @autoclosure parameter, the conversion type is
    // the result of the function type.
    if (FunctionType *autoclosureParamType = getAsAutoclosureParamType()) {
      return TypeLoc(expression.convertType.getTypeRepr(),
                     autoclosureParamType->getResult());
    }

    return expression.convertType;
  }

  /// Retrieve the type to which an expression should be converted, or
  /// a NULL type if no conversion constraint should be generated.
  Type getExprConversionType() const {
    if (contextualTypeIsOnlyAHint())
      return Type();
    return getExprContextualType();
  }

  /// Returns the autoclosure parameter type, or \c nullptr if the
  /// expression has a different kind of context.
  FunctionType *getAsAutoclosureParamType() const {
    assert(kind == Kind::expression);
    if (expression.contextualPurpose == CTP_AutoclosureDefaultParameter)
      return expression.convertType.getType()->castTo<FunctionType>();
    return nullptr;
  }

  void setExprConversionType(Type type) {
    assert(kind == Kind::expression);
    expression.convertType = TypeLoc::withoutLoc(type);
  }

  void setExprConversionTypeLoc(TypeLoc type) {
    assert(kind == Kind::expression);
    expression.convertType = type;
  }

  /// For a pattern initialization target, retrieve the pattern.
  Pattern *getInitializationPattern() const {
    assert(kind == Kind::expression);
    assert(expression.contextualPurpose == CTP_Initialization);
    return expression.pattern;
  }

  /// For a pattern initialization target, retrieve the contextual pattern.
  ContextualPattern getContextualPattern() const;

  /// Whether this target is for a for-in statement.
  bool isForEachStmt() const {
    return kind == Kind::expression &&
           getExprContextualTypePurpose() == CTP_ForEachStmt;
  }

  /// Whether this is an initialization for an Optional.Some pattern.
  bool isOptionalSomePatternInit() const {
    return kind == Kind::expression &&
        expression.contextualPurpose == CTP_Initialization &&
        isa<OptionalSomePattern>(expression.pattern) &&
        !expression.pattern->isImplicit();
  }

  /// Whether to bind the types of any variables within the pattern via
  /// one-way constraints.
  bool shouldBindPatternVarsOneWay() const {
    return kind == Kind::expression && expression.bindPatternVarsOneWay;
  }

  /// Whether or not an opaque value placeholder should be injected into the
  /// first \c wrappedValue argument of an apply expression so the initializer
  /// expression can be turned into a property wrapper generator function.
  bool shouldInjectWrappedValuePlaceholder(ApplyExpr *apply) const {
    if (kind != Kind::expression ||
        expression.contextualPurpose != CTP_Initialization)
      return false;

    auto *wrappedVar = expression.propertyWrapper.wrappedVar;
    if (!apply || !wrappedVar)
      return false;

    // Don't create property wrapper generator functions for static variables and
    // local variables with initializers.
    if (wrappedVar->isStatic() || wrappedVar->getDeclContext()->isLocalContext())
      return false;

    return expression.propertyWrapper.innermostWrappedValueInit == apply;
  }

  /// Whether this target is for initialization of a property wrapper
  /// with an initial wrapped value specified via \c = .
  bool propertyWrapperHasInitialWrappedValue() const {
    return (kind == Kind::expression &&
            expression.propertyWrapper.hasInitialWrappedValue);
  }

  /// Retrieve the wrapped variable when initializing a pattern with a
  /// property wrapper.
  VarDecl *getInitializationWrappedVar() const {
    assert(kind == Kind::expression);
    assert(expression.contextualPurpose == CTP_Initialization);
    return expression.propertyWrapper.wrappedVar;
  }

  PatternBindingDecl *getInitializationPatternBindingDecl() const {
    assert(kind == Kind::expression);
    assert(expression.contextualPurpose == CTP_Initialization);
    return expression.initialization.patternBinding;
  }

  unsigned getInitializationPatternBindingIndex() const {
    assert(kind == Kind::expression);
    assert(expression.contextualPurpose == CTP_Initialization);
    return expression.initialization.patternBindingIndex;
  }

  const ForEachStmtInfo &getForEachStmtInfo() const {
    assert(isForEachStmt());
    return expression.forEachStmt;
  }

  ForEachStmtInfo &getForEachStmtInfo() {
    assert(isForEachStmt());
    return expression.forEachStmt;
  }

  /// Whether this context infers an opaque return type.
  bool infersOpaqueReturnType() const;

  /// Whether the contextual type is only a hint, rather than a type
  bool contextualTypeIsOnlyAHint() const;

  bool isDiscardedExpr() const {
    assert(kind == Kind::expression);
    return expression.isDiscarded;
  }

  void setExpr(Expr *expr) {
    assert(kind == Kind::expression);
    expression.expression = expr;
  }

  void setPattern(Pattern *pattern) {
    assert(kind == Kind::expression);
    assert(expression.contextualPurpose == CTP_Initialization ||
           expression.contextualPurpose == CTP_ForEachStmt);
    expression.pattern = pattern;
  }

  Optional<AnyFunctionRef> getAsFunction() const {
    switch (kind) {
    case Kind::expression:
    case Kind::stmtCondition:
    case Kind::caseLabelItem:
    case Kind::patternBinding:
    case Kind::uninitializedWrappedVar:
      return None;

    case Kind::function:
      return function.function;
    }
    llvm_unreachable("invalid function kind");
  }

  Optional<StmtCondition> getAsStmtCondition() const {
    switch (kind) {
    case Kind::expression:
    case Kind::function:
    case Kind::caseLabelItem:
    case Kind::patternBinding:
    case Kind::uninitializedWrappedVar:
      return None;

    case Kind::stmtCondition:
      return stmtCondition.stmtCondition;
    }
    llvm_unreachable("invalid statement kind");
  }

  Optional<CaseLabelItem *> getAsCaseLabelItem() const {
    switch (kind) {
    case Kind::expression:
    case Kind::function:
    case Kind::stmtCondition:
    case Kind::patternBinding:
    case Kind::uninitializedWrappedVar:
      return None;

    case Kind::caseLabelItem:
      return caseLabelItem.caseLabelItem;
    }
    llvm_unreachable("invalid case label type");
  }

  PatternBindingDecl *getAsPatternBinding() const {
    switch (kind) {
    case Kind::expression:
    case Kind::function:
    case Kind::stmtCondition:
    case Kind::caseLabelItem:
    case Kind::uninitializedWrappedVar:
      return nullptr;

    case Kind::patternBinding:
      return patternBinding;
    }
    llvm_unreachable("invalid case label type");
  }

  VarDecl *getAsUninitializedWrappedVar() const {
    switch (kind) {
    case Kind::expression:
    case Kind::function:
    case Kind::stmtCondition:
    case Kind::caseLabelItem:
    case Kind::patternBinding:
      return nullptr;

    case Kind::uninitializedWrappedVar:
      return uninitializedWrappedVar;
    }
    llvm_unreachable("invalid case label type");
  }

  BraceStmt *getFunctionBody() const {
    assert(kind == Kind::function);
    return function.body;
  }

  void setFunctionBody(BraceStmt *stmt) {
    assert(kind == Kind::function);
    function.body = stmt;
  }

  /// Retrieve the source range of the target.
  SourceRange getSourceRange() const {
    switch (kind) {
    case Kind::expression:
      return expression.expression->getSourceRange();

    case Kind::function:
      return function.body->getSourceRange();

    case Kind::stmtCondition:
      return SourceRange(stmtCondition.stmtCondition.front().getStartLoc(),
                         stmtCondition.stmtCondition.back().getEndLoc());

    case Kind::caseLabelItem:
      return caseLabelItem.caseLabelItem->getSourceRange();

    case Kind::patternBinding:
      return patternBinding->getSourceRange();

    case Kind::uninitializedWrappedVar:
      return uninitializedWrappedVar->getSourceRange();
    }
    llvm_unreachable("invalid target type");
  }

  /// Retrieve the source location for the target.
  SourceLoc getLoc() const {
    switch (kind) {
    case Kind::expression:
      return expression.expression->getLoc();

    case Kind::function:
      return function.function.getLoc();

    case Kind::stmtCondition:
      return stmtCondition.stmtCondition.front().getStartLoc();

    case Kind::caseLabelItem:
      return caseLabelItem.caseLabelItem->getStartLoc();

    case Kind::patternBinding:
      return patternBinding->getLoc();

    case Kind::uninitializedWrappedVar:
      return uninitializedWrappedVar->getLoc();
    }
    llvm_unreachable("invalid target type");
  }

  /// Walk the contents of the application target.
  SolutionApplicationTarget walk(ASTWalker &walker);
};

/// A function that rewrites a solution application target in the context
/// of solution application.
using RewriteTargetFn = std::function<
    Optional<SolutionApplicationTarget> (SolutionApplicationTarget)>;

enum class ConstraintSystemPhase {
  ConstraintGeneration,
  Solving,
  Diagnostics,
  Finalization
};

/// Describes the result of applying a solution to a given function.
enum class SolutionApplicationToFunctionResult {
  /// Application of the solution succeeded.
  Success,
  /// Application of the solution failed.
  /// TODO: This should probably go away entirely.
  Failure,
  /// The solution could not be applied immediately, and type checking for
  /// this function should be delayed until later.
  Delay,
};

/// Describes a system of constraints on type variables, the
/// solution of which assigns concrete types to each of the type variables.
/// Constraint systems are typically generated given an (untyped) expression.
class ConstraintSystem {
  ASTContext &Context;

public:
  DeclContext *DC;
  ConstraintSystemOptions Options;
  Optional<ExpressionTimer> Timer;

  friend class Solution;
  friend class ConstraintFix;
  friend class OverloadChoice;
  friend class ConstraintGraph;
  friend class DisjunctionChoice;
  friend class Component;
  friend class FailureDiagnostic;
  friend class TypeVarBindingProducer;
  friend class TypeVariableBinding;
  friend class StepScope;
  friend class SolverStep;
  friend class SplitterStep;
  friend class ComponentStep;
  friend class TypeVariableStep;
  friend class RequirementFailure;
  friend class MissingMemberFailure;

  class SolverScope;

  /// Expressions that are known to be unevaluated.
  /// Note: this is only used to support ObjCSelectorExpr at the moment.
  llvm::SmallPtrSet<Expr *, 2> UnevaluatedRootExprs;

  /// The total number of disjunctions created.
  unsigned CountDisjunctions = 0;

private:
  /// A constraint that has failed along the current solver path.
  /// Do not set directly, call \c recordFailedConstraint instead.
  Constraint *failedConstraint = nullptr;

  /// Current phase of the constraint system lifetime.
  ConstraintSystemPhase Phase = ConstraintSystemPhase::ConstraintGeneration;

  /// The set of expressions for which we have generated constraints.
  llvm::SetVector<Expr *> InputExprs;

  /// The number of input expressions whose parents and depths have
  /// been entered into \c ExprWeights.
  unsigned NumInputExprsInWeights = 0;

  llvm::DenseMap<Expr *, std::pair<unsigned, Expr *>> ExprWeights;

  /// Allocator used for all of the related constraint systems.
  llvm::BumpPtrAllocator Allocator;

  /// Arena used for memory management of constraint-checker-related
  /// allocations.
  ConstraintCheckerArenaRAII Arena;

  /// Counter for type variables introduced.
  unsigned TypeCounter = 0;

  /// The number of scopes created so far during the solution
  /// of this constraint system.
  ///
  /// This is a measure of complexity of the solution space. A new
  /// scope is created every time we attempt a type variable binding
  /// or explore an option in a disjunction.
  unsigned CountScopes = 0;

  /// High-water mark of measured memory usage in any sub-scope we
  /// explored.
  size_t MaxMemory = 0;

  /// Cached member lookups.
  llvm::DenseMap<std::pair<Type, DeclNameRef>, Optional<LookupResult>>
    MemberLookups;

  /// Cached sets of "alternative" literal types.
  static const unsigned NumAlternativeLiteralTypes = 13;
  Optional<ArrayRef<Type>> AlternativeLiteralTypes[NumAlternativeLiteralTypes];

  /// Folding set containing all of the locators used in this
  /// constraint system.
  llvm::FoldingSetVector<ConstraintLocator> ConstraintLocators;

  /// The overload sets that have been resolved along the current path.
  llvm::MapVector<ConstraintLocator *, SelectedOverload> ResolvedOverloads;

  /// The current fixed score for this constraint system and the (partial)
  /// solution it represents.
  Score CurrentScore;

  llvm::SetVector<TypeVariableType *> TypeVariables;

  /// Maps expressions to types for choosing a favored overload
  /// type in a disjunction constraint.
  llvm::DenseMap<Expr *, TypeBase *> FavoredTypes;

  /// Maps discovered closures to their types inferred
  /// from declared parameters/result and body.
  llvm::MapVector<const ClosureExpr *, FunctionType *> ClosureTypes;

  /// This is a *global* list of all result builder bodies that have
  /// been determined to be incorrect by failing constraint generation.
  ///
  /// Tracking this information is useful to avoid producing duplicate
  /// diagnostics when result builder has multiple overloads.
  llvm::SmallDenseSet<AnyFunctionRef> InvalidResultBuilderBodies;

  /// Maps node types used within all portions of the constraint
  /// system, instead of directly using the types on the
  /// nodes themselves. This allows us to typecheck and
  /// run through various diagnostics passes without actually mutating
  /// the types on the nodes.
  llvm::MapVector<ASTNode, Type> NodeTypes;
  llvm::DenseMap<std::pair<const KeyPathExpr *, unsigned>, TypeBase *>
      KeyPathComponentTypes;

  /// Maps AST entries to their solution application targets.
  llvm::MapVector<SolutionApplicationTargetsKey, SolutionApplicationTarget>
    solutionApplicationTargets;

  /// Contextual type information for expressions that are part of this
  /// constraint system.
  llvm::MapVector<ASTNode, ContextualTypeInfo> contextualTypes;

  /// Information about each case label item tracked by the constraint system.
  llvm::SmallMapVector<const CaseLabelItem *, CaseLabelItemInfo, 4>
      caseLabelItems;

  /// Maps closure parameters to type variables.
  llvm::DenseMap<const ParamDecl *, TypeVariableType *>
    OpenedParameterTypes;

  /// The set of constraint restrictions used to reach the
  /// current constraint system.
  ///
  /// Constraint restrictions help describe which path the solver took when
  /// there are multiple ways in which one type could convert to another, e.g.,
  /// given class types A and B, the solver might choose either a superclass
  /// conversion or a user-defined conversion.
  std::vector<std::tuple<Type, Type, ConversionRestrictionKind>>
      ConstraintRestrictions;

  /// The set of fixes applied to make the solution work.
  llvm::SmallVector<ConstraintFix *, 4> Fixes;

  /// The set of remembered disjunction choices used to reach
  /// the current constraint system.
  std::vector<std::pair<ConstraintLocator*, unsigned>>
      DisjunctionChoices;

  /// A map from applied disjunction constraints to the corresponding
  /// argument function type.
  llvm::SmallMapVector<ConstraintLocator *, const FunctionType *, 4>
      AppliedDisjunctions;

  /// For locators associated with call expressions, the trailing closure
  /// matching rule and parameter bindings that were applied.
  std::vector<std::pair<ConstraintLocator *, MatchCallArgumentResult>>
      argumentMatchingChoices;

  /// The set of implicit value conversions performed by the solver on
  /// a current path to reach a solution.
  SmallVector<std::pair<ConstraintLocator *, ConversionRestrictionKind>, 2>
      ImplicitValueConversions;

  /// The worklist of "active" constraints that should be revisited
  /// due to a change.
  ConstraintList ActiveConstraints;

  /// The list of "inactive" constraints that still need to be solved,
  /// but will not be revisited until one of their inputs changes.
  ConstraintList InactiveConstraints;

  /// The constraint graph.
  ConstraintGraph &CG;

  /// A mapping from constraint locators to the set of opened types associated
  /// with that locator.
  SmallVector<std::pair<ConstraintLocator *, ArrayRef<OpenedType>>, 4>
    OpenedTypes;

  /// The list of all generic requirements fixed along the current
  /// solver path.
  using FixedRequirement =
      std::tuple<GenericTypeParamType *, unsigned, TypeBase *>;
  llvm::SmallSetVector<FixedRequirement, 4> FixedRequirements;

  bool isFixedRequirement(ConstraintLocator *reqLocator, Type requirementTy);
  void recordFixedRequirement(ConstraintLocator *reqLocator,
                              Type requirementTy);

  /// A mapping from constraint locators to the opened existential archetype
  /// used for the 'self' of an existential type.
  SmallVector<std::pair<ConstraintLocator *, OpenedArchetypeType *>, 4>
    OpenedExistentialTypes;

  /// The nodes for which we have produced types, along with the prior type
  /// each node had before introducing this type.
  llvm::SmallVector<std::pair<ASTNode, Type>, 8> addedNodeTypes;

  /// The set of functions that have been transformed by a result builder.
  std::vector<std::pair<AnyFunctionRef, AppliedBuilderTransform>>
      resultBuilderTransformed;

  /// Cache of the effects any closures visited.
  llvm::SmallDenseMap<ClosureExpr *, FunctionType::ExtInfo, 4> closureEffectsCache;

public:
  /// A map from argument expressions to their applied property wrapper expressions.
  llvm::SmallMapVector<ASTNode, SmallVector<AppliedPropertyWrapper, 2>, 4> appliedPropertyWrappers;

  /// The locators of \c Defaultable constraints whose defaults were used.
  std::vector<ConstraintLocator *> DefaultedConstraints;

  /// A cache that stores the @dynamicCallable required methods implemented by
  /// types.
  llvm::DenseMap<CanType, DynamicCallableMethods> DynamicCallableCache;

private:
  /// Describe the candidate expression for partial solving.
  /// This class used by shrink & solve methods which apply
  /// variation of directional path consistency algorithm in attempt
  /// to reduce scopes of the overload sets (disjunctions) in the system.
  class Candidate {
    Expr *E;
    DeclContext *DC;
    llvm::BumpPtrAllocator &Allocator;

    // Contextual Information.
    Type CT;
    ContextualTypePurpose CTP;

  public:
    Candidate(ConstraintSystem &cs, Expr *expr, Type ct = Type(),
              ContextualTypePurpose ctp = ContextualTypePurpose::CTP_Unused)
        : E(expr), DC(cs.DC), Allocator(cs.Allocator), CT(ct), CTP(ctp) {}

    /// Return underlying expression.
    Expr *getExpr() const { return E; }

    /// Try to solve this candidate sub-expression
    /// and re-write it's OSR domains afterwards.
    ///
    /// \param shrunkExprs The set of expressions which
    /// domains have been successfully shrunk so far.
    ///
    /// \returns true on solver failure, false otherwise.
    bool solve(llvm::SmallSetVector<OverloadSetRefExpr *, 4> &shrunkExprs);

    /// Apply solutions found by solver as reduced OSR sets for
    /// for current and all of it's sub-expressions.
    ///
    /// \param solutions The solutions found by running solver on the
    /// this candidate expression.
    ///
    /// \param shrunkExprs The set of expressions which
    /// domains have been successfully shrunk so far.
    void applySolutions(
        llvm::SmallVectorImpl<Solution> &solutions,
        llvm::SmallSetVector<OverloadSetRefExpr *, 4> &shrunkExprs) const;

    /// Check if attempt at solving of the candidate makes sense given
    /// the current conditions - number of shrunk domains which is related
    /// to the given candidate over the total number of disjunctions present.
    static bool
    isTooComplexGiven(ConstraintSystem *const cs,
                      llvm::SmallSetVector<OverloadSetRefExpr *, 4> &shrunkExprs) {
      SmallVector<Constraint *, 8> disjunctions;
      cs->collectDisjunctions(disjunctions);

      unsigned unsolvedDisjunctions = disjunctions.size();
      for (auto *disjunction : disjunctions) {
        auto *locator = disjunction->getLocator();
        if (!locator)
          continue;

        if (auto *OSR = getAsExpr<OverloadSetRefExpr>(locator->getAnchor())) {
          if (shrunkExprs.count(OSR) > 0)
            --unsolvedDisjunctions;
        }
      }

      unsigned threshold =
          cs->getASTContext().TypeCheckerOpts.SolverShrinkUnsolvedThreshold;
      return unsolvedDisjunctions >= threshold;
    }
  };

  /// Describes the current solver state.
  struct SolverState {
    SolverState(ConstraintSystem &cs,
                FreeTypeVariableBinding allowFreeTypeVariables);
    ~SolverState();

    /// The constraint system.
    ConstraintSystem &CS;

    FreeTypeVariableBinding AllowFreeTypeVariables;

    /// Depth of the solution stack.
    unsigned depth = 0;

    /// Maximum depth reached so far in exploring solutions.
    unsigned maxDepth = 0;

    /// Whether to record failures or not.
    bool recordFixes = false;

    /// The set of type variable bindings that have changed while
    /// processing this constraint system.
    SavedTypeVariableBindings savedBindings;

     /// The best solution computed so far.
    Optional<Score> BestScore;

    /// The number of the solution attempt we're looking at.
    unsigned SolutionAttempt;

    /// Refers to the innermost partial solution scope.
    SolverScope *PartialSolutionScope = nullptr;

    // Statistics
    #define CS_STATISTIC(Name, Description) unsigned Name = 0;
    #include "ConstraintSolverStats.def"

    /// Check whether there are any retired constraints present.
    bool hasRetiredConstraints() const {
      return !retiredConstraints.empty();
    }

    /// Mark given constraint as retired along current solver path.
    ///
    /// \param constraint The constraint to retire temporarily.
    void retireConstraint(Constraint *constraint) {
      retiredConstraints.push_front(constraint);
    }

    /// Iterate over all of the retired constraints registered with
    /// current solver state.
    ///
    /// \param processor The processor function to be applied to each of
    /// the constraints retrieved.
    void forEachRetired(llvm::function_ref<void(Constraint &)> processor) {
      for (auto &constraint : retiredConstraints)
        processor(constraint);
    }

    /// Add new "generated" constraint along the current solver path.
    ///
    /// \param constraint The newly generated constraint.
    void addGeneratedConstraint(Constraint *constraint) {
      assert(constraint && "Null generated constraint?");
      generatedConstraints.push_back(constraint);
    }

    /// Register given scope to be tracked by the current solver state,
    /// this helps to make sure that all of the retired/generated constraints
    /// are dealt with correctly when the life time of the scope ends.
    ///
    /// \param scope The scope to associate with current solver state.
    void registerScope(SolverScope *scope) {
      ++depth;
      maxDepth = std::max(maxDepth, depth);
      scope->scopeNumber = NumStatesExplored++;

      CS.incrementScopeCounter();
      auto scopeInfo =
        std::make_tuple(scope, retiredConstraints.begin(),
                        generatedConstraints.size());
      scopes.push_back(scopeInfo);
    }

    /// Restore all of the retired/generated constraints to the state
    /// before given scope. This is required because retired constraints have
    /// to be re-introduced to the system in order of arrival (LIFO) and list
    /// of the generated constraints has to be truncated back to the
    /// original size.
    ///
    /// \param scope The solver scope to rollback.
    void rollback(SolverScope *scope) {
      --depth;

      unsigned countScopesExplored = NumStatesExplored - scope->scopeNumber;
      if (countScopesExplored == 1)
        CS.incrementLeafScopes();

      SolverScope *savedScope;
      // The position of last retired constraint before given scope.
      ConstraintList::iterator lastRetiredPos;
      // The original number of generated constraints before given scope.
      unsigned numGenerated;

      std::tie(savedScope, lastRetiredPos, numGenerated) =
        scopes.pop_back_val();

      assert(savedScope == scope && "Scope rollback not in LIFO order!");

      // Restore all of the retired constraints.
      CS.InactiveConstraints.splice(CS.InactiveConstraints.end(),
                                    retiredConstraints,
                                    retiredConstraints.begin(), lastRetiredPos);

      // And remove all of the generated constraints.
      auto genStart = generatedConstraints.begin() + numGenerated,
           genEnd = generatedConstraints.end();
      for (auto genI = genStart; genI != genEnd; ++genI) {
        CS.InactiveConstraints.erase(ConstraintList::iterator(*genI));
      }

      generatedConstraints.erase(genStart, genEnd);

      for (unsigned constraintIdx :
             range(scope->numDisabledConstraints, disabledConstraints.size())) {
        if (disabledConstraints[constraintIdx]->isDisabled())
          disabledConstraints[constraintIdx]->setEnabled();
      }
      disabledConstraints.erase(
          disabledConstraints.begin() + scope->numDisabledConstraints,
          disabledConstraints.end());

      for (unsigned constraintIdx :
             range(scope->numFavoredConstraints, favoredConstraints.size())) {
        if (favoredConstraints[constraintIdx]->isFavored())
          favoredConstraints[constraintIdx]->setFavored(false);
      }
      favoredConstraints.erase(
          favoredConstraints.begin() + scope->numFavoredConstraints,
          favoredConstraints.end());
    }

    /// Check whether constraint system is allowed to form solutions
    /// even with unbound type variables present.
    bool allowsFreeTypeVariables() const {
      return AllowFreeTypeVariables != FreeTypeVariableBinding::Disallow;
    }

    unsigned getNumDisabledConstraints() const {
      return disabledConstraints.size();
    }

    /// Disable the given constraint; this change will be rolled back
    /// when we exit the current solver scope.
    void disableContraint(Constraint *constraint) {
      constraint->setDisabled();
      disabledConstraints.push_back(constraint);
    }

    unsigned getNumFavoredConstraints() const {
      return favoredConstraints.size();
    }

    /// Favor the given constraint; this change will be rolled back
    /// when we exit the current solver scope.
    void favorConstraint(Constraint *constraint) {
      assert(!constraint->isFavored());

      constraint->setFavored();
      favoredConstraints.push_back(constraint);
    }

  private:
    /// The list of constraints that have been retired along the
    /// current path, this list is used in LIFO fashion when constraints
    /// are added back to the circulation.
    ConstraintList retiredConstraints;

    /// The set of constraints which were active at the time of this state
    /// creating, it's used to re-activate them on destruction.
    SmallVector<Constraint *, 4> activeConstraints;

    /// The current set of generated constraints.
    SmallVector<Constraint *, 4> generatedConstraints;

    /// The collection which holds association between solver scope
    /// and position of the last retired constraint and number of
    /// constraints generated before registration of given scope,
    /// this helps to rollback all of the constraints retired/generated
    /// each of the registered scopes correct (LIFO) order.
    llvm::SmallVector<
      std::tuple<SolverScope *, ConstraintList::iterator, unsigned>, 4> scopes;

    SmallVector<Constraint *, 4> disabledConstraints;
    SmallVector<Constraint *, 4> favoredConstraints;
  };

  class CacheExprTypes : public ASTWalker {
    Expr *RootExpr;
    ConstraintSystem &CS;
    bool ExcludeRoot;

  public:
    CacheExprTypes(Expr *expr, ConstraintSystem &cs, bool excludeRoot)
        : RootExpr(expr), CS(cs), ExcludeRoot(excludeRoot) {}

    Expr *walkToExprPost(Expr *expr) override {
      if (ExcludeRoot && expr == RootExpr) {
        assert(!expr->getType() && "Unexpected type in root of expression!");
        return expr;
      }

      if (expr->getType())
        CS.cacheType(expr);

      if (auto kp = dyn_cast<KeyPathExpr>(expr))
        for (auto i : indices(kp->getComponents()))
          if (kp->getComponents()[i].getComponentType())
            CS.cacheType(kp, i);

      return expr;
    }

    /// Ignore statements.
    std::pair<bool, Stmt *> walkToStmtPre(Stmt *stmt) override {
      return { false, stmt };
    }

    /// Ignore declarations.
    bool walkToDeclPre(Decl *decl) override { return false; }
  };

public:
  /// Retrieve the first constraint that has failed along the solver's path, or
  /// \c nullptr if no constraint has failed.
  Constraint *getFailedConstraint() const { return failedConstraint; }

  ConstraintSystemPhase getPhase() const { return Phase; }

  /// Move constraint system to a new phase of its lifetime.
  void setPhase(ConstraintSystemPhase newPhase) {
    if (Phase == newPhase)
      return;

#ifndef NDEBUG
    switch (Phase) {
    case ConstraintSystemPhase::ConstraintGeneration:
      assert(newPhase == ConstraintSystemPhase::Solving);
      break;

    case ConstraintSystemPhase::Solving:
      // We can come back to constraint generation phase while
      // processing result builder body.
      assert(newPhase == ConstraintSystemPhase::ConstraintGeneration ||
             newPhase == ConstraintSystemPhase::Diagnostics ||
             newPhase == ConstraintSystemPhase::Finalization);
      break;

    case ConstraintSystemPhase::Diagnostics:
      assert(newPhase == ConstraintSystemPhase::Solving ||
             newPhase == ConstraintSystemPhase::Finalization);
      break;

    case ConstraintSystemPhase::Finalization:
      assert(newPhase == ConstraintSystemPhase::Diagnostics);
      break;
    }
#endif

    Phase = newPhase;
  }

  /// Cache the types of the given expression and all subexpressions.
  void cacheExprTypes(Expr *expr) {
    bool excludeRoot = false;
    expr->walk(CacheExprTypes(expr, *this, excludeRoot));
  }

  /// Cache the types of the expressions under the given expression
  /// (but not the type of the given expression).
  void cacheSubExprTypes(Expr *expr) {
    bool excludeRoot = true;
    expr->walk(CacheExprTypes(expr, *this, excludeRoot));
  }

  /// The current solver state.
  ///
  /// This will be non-null when we're actively solving the constraint
  /// system, and carries temporary state related to the current path
  /// we're exploring.
  SolverState *solverState = nullptr;

  struct ArgumentInfo {
    ArrayRef<Identifier> Labels;
    Optional<unsigned> UnlabeledTrailingClosureIndex;
  };

  /// A mapping from the constraint locators for references to various
  /// names (e.g., member references, normal name references, possible
  /// constructions) to the argument labels provided in the call to
  /// that locator.
  llvm::DenseMap<ConstraintLocator *, ArgumentInfo> ArgumentInfos;

  /// Form a locator that can be used to retrieve argument information cached in
  /// the constraint system for the callee described by the anchor of the
  /// passed locator.
  ConstraintLocator *getArgumentInfoLocator(ConstraintLocator *locator);

  /// Retrieve the argument info that is associated with a member
  /// reference at the given locator.
  Optional<ArgumentInfo> getArgumentInfo(ConstraintLocator *locator);

  Optional<SelectedOverload>
  findSelectedOverloadFor(ConstraintLocator *locator) const {
    auto result = ResolvedOverloads.find(locator);
    if (result == ResolvedOverloads.end())
      return None;
    return result->second;
  }

  Optional<SelectedOverload> findSelectedOverloadFor(Expr *expr) {
    // Retrieve the callee locator for this expression, making sure not to
    // look through applies in order to ensure we only return the "direct"
    // callee.
    auto *loc = getConstraintLocator(expr);
    auto *calleeLoc = getCalleeLocator(loc, /*lookThroughApply*/ false);
    return findSelectedOverloadFor(calleeLoc);
  }

private:
  unsigned assignTypeVariableID() {
    return TypeCounter++;
  }

  void incrementScopeCounter();
  void incrementLeafScopes();

public:
  /// Introduces a new solver scope, which any changes to the
  /// solver state or constraint system are temporary and will be undone when
  /// this object is destroyed.
  ///
  ///
  class SolverScope {
    ConstraintSystem &cs;

    /// The length of \c TypeVariables.
    unsigned numTypeVariables;

    /// The length of \c SavedBindings.
    unsigned numSavedBindings;

    /// The length of \c ConstraintRestrictions.
    unsigned numConstraintRestrictions;

    /// The length of \c Fixes.
    unsigned numFixes;

    /// The length of \c FixedRequirements.
    unsigned numFixedRequirements;

    /// The length of \c DisjunctionChoices.
    unsigned numDisjunctionChoices;

    /// The length of \c AppliedDisjunctions.
    unsigned numAppliedDisjunctions;

    /// The length of \c argumentMatchingChoices.
    unsigned numArgumentMatchingChoices;

    /// The length of \c OpenedTypes.
    unsigned numOpenedTypes;

    /// The length of \c OpenedExistentialTypes.
    unsigned numOpenedExistentialTypes;

    /// The length of \c DefaultedConstraints.
    unsigned numDefaultedConstraints;

    unsigned numAddedNodeTypes;

    unsigned numDisabledConstraints;

    unsigned numFavoredConstraints;

    unsigned numResultBuilderTransformed;

    /// The length of \c appliedPropertyWrappers
    unsigned numAppliedPropertyWrappers;

    /// The length of \c ResolvedOverloads.
    unsigned numResolvedOverloads;

    /// The length of \c ClosureTypes.
    unsigned numInferredClosureTypes;

    /// The length of \c contextualTypes.
    unsigned numContextualTypes;

    /// The length of \c solutionApplicationTargets.
    unsigned numSolutionApplicationTargets;

    /// The length of \c caseLabelItems.
    unsigned numCaseLabelItems;

    /// The length of \c ImplicitValueConversions.
    unsigned numImplicitValueConversions;

    /// The previous score.
    Score PreviousScore;

    /// The scope number of this scope. Set when the scope is registered.
    unsigned scopeNumber = 0;

    /// Constraint graph scope associated with this solver scope.
    ConstraintGraphScope CGScope;

    SolverScope(const SolverScope &) = delete;
    SolverScope &operator=(const SolverScope &) = delete;

    friend class ConstraintSystem;

  public:
    explicit SolverScope(ConstraintSystem &cs);
    ~SolverScope();
  };

  ConstraintSystem(DeclContext *dc,
                   ConstraintSystemOptions options);
  ~ConstraintSystem();

  /// Retrieve the constraint graph associated with this constraint system.
  ConstraintGraph &getConstraintGraph() const { return CG; }

  /// Retrieve the AST context.
  ASTContext &getASTContext() const { return Context; }

  /// Determine whether this constraint system has any free type
  /// variables.
  bool hasFreeTypeVariables();

  /// Check whether constraint solver is running in "debug" mode,
  /// which should output diagnostic information.
  bool isDebugMode() const {
    return Options.contains(ConstraintSystemFlags::DebugConstraints);
  }

private:
  /// Finalize this constraint system; we're done attempting to solve
  /// it.
  ///
  /// \returns the solution.
  Solution finalize();

  /// Apply the given solution to the current constraint system.
  ///
  /// This operation is used to take a solution computed based on some
  /// subset of the constraints and then apply it back to the
  /// constraint system for further exploration.
  void applySolution(const Solution &solution);

  // FIXME: Perhaps these belong on ConstraintSystem itself.
  friend Optional<BraceStmt *>
  swift::TypeChecker::applyResultBuilderBodyTransform(FuncDecl *func,
                                                        Type builderType);

  friend Optional<SolutionApplicationTarget>
  swift::TypeChecker::typeCheckExpression(
      SolutionApplicationTarget &target, OptionSet<TypeCheckExprFlags> options);

  /// Emit the fixes computed as part of the solution, returning true if we were
  /// able to emit an error message, or false if none of the fixits worked out.
  bool applySolutionFixes(const Solution &solution);

  /// If there is more than one viable solution,
  /// attempt to pick the best solution and remove all of the rest.
  ///
  /// \param solutions The set of solutions to filter.
  ///
  /// \param minimize The flag which idicates if the
  /// set of solutions should be filtered even if there is
  /// no single best solution, see `findBestSolution` for
  /// more details.
  void
  filterSolutions(SmallVectorImpl<Solution> &solutions,
                  bool minimize = false) {
    if (solutions.size() < 2)
      return;

    if (auto best = findBestSolution(solutions, minimize)) {
      if (*best != 0)
        solutions[0] = std::move(solutions[*best]);
      solutions.erase(solutions.begin() + 1, solutions.end());
    }
  }

  /// Restore the type variable bindings to what they were before
  /// we attempted to solve this constraint system.
  ///
  /// \param numBindings The number of bindings to restore, from the end of
  /// the saved-binding stack.
  void restoreTypeVariableBindings(unsigned numBindings);

  /// Retrieve the set of saved type variable bindings, if available.
  ///
  /// \returns null when we aren't currently solving the system.
  SavedTypeVariableBindings *getSavedBindings() const {
    return solverState ? &solverState->savedBindings : nullptr;
  }

  /// Add a new type variable that was already created.
  void addTypeVariable(TypeVariableType *typeVar);
  
  /// Add a constraint from the subscript base to the root of the key
  /// path literal to the constraint system.
  void addKeyPathApplicationRootConstraint(Type root, ConstraintLocatorBuilder locator);

public:
  /// Lookup for a member with the given name in the given base type.
  ///
  /// This routine caches the results of member lookups in the top constraint
  /// system, to avoid.
  ///
  /// FIXME: This caching should almost certainly be performed at the
  /// module level, since type checking occurs after import resolution,
  /// and no new names are introduced after that point.
  ///
  /// \returns A reference to the member-lookup result.
  LookupResult &lookupMember(Type base, DeclNameRef name);

  /// Retrieve the set of "alternative" literal types that we'll explore
  /// for a given literal protocol kind.
  ArrayRef<Type> getAlternativeLiteralTypes(KnownProtocolKind kind);

  /// Create a new type variable.
  TypeVariableType *createTypeVariable(ConstraintLocator *locator,
                                       unsigned options);

  /// Retrieve the set of active type variables.
  ArrayRef<TypeVariableType *> getTypeVariables() const {
    return TypeVariables.getArrayRef();
  }

  /// Whether the given type variable is active in the constraint system at
  /// the moment.
  bool isActiveTypeVariable(TypeVariableType *typeVar) const {
    return TypeVariables.count(typeVar) > 0;
  }

  /// Whether the given expression's source range contains the code
  /// completion location.
  bool containsCodeCompletionLoc(Expr *expr) const;

  void setClosureType(const ClosureExpr *closure, FunctionType *type) {
    assert(closure);
    assert(type && "Expected non-null type");
    assert(ClosureTypes.count(closure) == 0 && "Cannot reset closure type");
    ClosureTypes.insert({closure, type});
  }

  FunctionType *getClosureType(const ClosureExpr *closure) const {
    auto result = ClosureTypes.find(closure);
    assert(result != ClosureTypes.end());
    return result->second;
  }

  TypeBase* getFavoredType(Expr *E) {
    assert(E != nullptr);
    return this->FavoredTypes[E];
  }
  void setFavoredType(Expr *E, TypeBase *T) {
    assert(E != nullptr);
    this->FavoredTypes[E] = T;
  }

  /// Set the type in our type map for the given node.
  ///
  /// The side tables are used through the expression type checker to avoid mutating nodes until
  /// we know we have successfully type-checked them.
  void setType(ASTNode node, Type type) {
    assert(!node.isNull() && "Cannot set type information on null node");
    assert(type && "Expected non-null type");

    // Record the type.
    Type &entry = NodeTypes[node];
    Type oldType = entry;
    entry = type;

    // Record the fact that we ascribed a type to this node.
    addedNodeTypes.push_back({node, oldType});
  }

  /// Set the type in our type map for a given expression. The side
  /// map is used throughout the expression type checker in order to
  /// avoid mutating expressions until we know we have successfully
  /// type-checked them.
  void setType(KeyPathExpr *KP, unsigned I, Type T) {
    assert(KP && "Expected non-null key path parameter!");
    assert(T && "Expected non-null type!");
    KeyPathComponentTypes[std::make_pair(KP, I)] = T.getPointer();
  }

  /// Check to see if we have a type for a node.
  bool hasType(ASTNode node) const {
    assert(!node.isNull() && "Expected non-null node");
    return NodeTypes.count(node) > 0;
  }

  bool hasType(const KeyPathExpr *KP, unsigned I) const {
    assert(KP && "Expected non-null key path parameter!");
    return KeyPathComponentTypes.find(std::make_pair(KP, I))
              != KeyPathComponentTypes.end();
  }

  /// Get the type for an node.
  Type getType(ASTNode node) const {
    assert(hasType(node) && "Expected type to have been set!");
    // FIXME: lvalue differences
    //    assert((!E->getType() ||
    //            E->getType()->isEqual(ExprTypes.find(E)->second)) &&
    //           "Mismatched types!");
    return NodeTypes.find(node)->second;
  }

  Type getType(const KeyPathExpr *KP, unsigned I) const {
    assert(hasType(KP, I) && "Expected type to have been set!");
    return KeyPathComponentTypes.find(std::make_pair(KP, I))->second;
  }

  /// Retrieve the type of the node, if known.
  Type getTypeIfAvailable(ASTNode node) const {
    auto known = NodeTypes.find(node);
    if (known == NodeTypes.end())
      return Type();

    return known->second;
  }

  /// Retrieve type type of the given declaration to be used in
  /// constraint system, this is better than calling `getType()`
  /// directly because it accounts of constraint system flags.
  Type getVarType(const VarDecl *var);

  /// Cache the type of the expression argument and return that same
  /// argument.
  template <typename T>
  T *cacheType(T *E) {
    assert(E->getType() && "Expected a type!");
    setType(E, E->getType());
    return E;
  }

  /// Cache the type of the expression argument and return that same
  /// argument.
  KeyPathExpr *cacheType(KeyPathExpr *E, unsigned I) {
    auto componentTy = E->getComponents()[I].getComponentType();
    assert(componentTy && "Expected a type!");
    setType(E, I, componentTy);
    return E;
  }

  void setContextualType(ASTNode node, TypeLoc T,
                         ContextualTypePurpose purpose) {
    assert(bool(node) && "Expected non-null expression!");
    assert(contextualTypes.count(node) == 0 &&
           "Already set this contextual type");
    contextualTypes[node] = {T, purpose};
  }

  Optional<ContextualTypeInfo> getContextualTypeInfo(ASTNode node) const {
    auto known = contextualTypes.find(node);
    if (known == contextualTypes.end())
      return None;
    return known->second;
  }

  Type getContextualType(ASTNode node) const {
    auto result = getContextualTypeInfo(node);
    if (result)
      return result->typeLoc.getType();
    return Type();
  }

  TypeLoc getContextualTypeLoc(ASTNode node) const {
    auto result = getContextualTypeInfo(node);
    if (result)
      return result->typeLoc;
    return TypeLoc();
  }

  ContextualTypePurpose getContextualTypePurpose(ASTNode node) const {
    auto result = getContextualTypeInfo(node);
    if (result)
      return result->purpose;
    return CTP_Unused;
  }

  void setSolutionApplicationTarget(
      SolutionApplicationTargetsKey key, SolutionApplicationTarget target) {
    assert(solutionApplicationTargets.count(key) == 0 &&
           "Already set this solution application target");
    solutionApplicationTargets.insert({key, target});
  }

  Optional<SolutionApplicationTarget> getSolutionApplicationTarget(
      SolutionApplicationTargetsKey key) const {
    auto known = solutionApplicationTargets.find(key);
    if (known == solutionApplicationTargets.end())
      return None;
    return known->second;
  }

  void setCaseLabelItemInfo(const CaseLabelItem *item, CaseLabelItemInfo info) {
    assert(item != nullptr);
    assert(caseLabelItems.count(item) == 0);
    caseLabelItems[item] = info;
  }

  Optional<CaseLabelItemInfo> getCaseLabelItemInfo(
      const CaseLabelItem *item) const {
    auto known = caseLabelItems.find(item);
    if (known == caseLabelItems.end())
      return None;

    return known->second;
  }

  /// Retrieve the constraint locator for the given anchor and
  /// path, uniqued.
  ConstraintLocator *
  getConstraintLocator(ASTNode anchor,
                       ArrayRef<ConstraintLocator::PathElement> path,
                       unsigned summaryFlags);

  /// Retrive the constraint locator for the given anchor and
  /// path, uniqued and automatically infer the summary flags
  ConstraintLocator *
  getConstraintLocator(ASTNode anchor,
                       ArrayRef<ConstraintLocator::PathElement> path);

  /// Retrieve the constraint locator for the given anchor and
  /// an empty path, uniqued.
  ConstraintLocator *getConstraintLocator(ASTNode anchor) {
    return getConstraintLocator(anchor, {}, 0);
  }

  /// Retrieve the constraint locator for the given anchor and
  /// path element.
  ConstraintLocator *
  getConstraintLocator(ASTNode anchor, ConstraintLocator::PathElement pathElt) {
    return getConstraintLocator(anchor, llvm::makeArrayRef(pathElt),
                                pathElt.getNewSummaryFlags());
  }

  /// Extend the given constraint locator with a path element.
  ConstraintLocator *
  getConstraintLocator(ConstraintLocator *locator,
                       ConstraintLocator::PathElement pathElt) {
    ConstraintLocatorBuilder builder(locator);
    return getConstraintLocator(builder.withPathElement(pathElt));
  }

  /// Extend the given constraint locator with an array of path elements.
  ConstraintLocator *
  getConstraintLocator(ConstraintLocator *locator,
                       ArrayRef<ConstraintLocator::PathElement> newElts);

  /// Retrieve the locator described by a given builder extended by an array of
  /// path elements.
  ConstraintLocator *
  getConstraintLocator(const ConstraintLocatorBuilder &builder,
                       ArrayRef<ConstraintLocator::PathElement> newElts);

  /// Retrieve the constraint locator described by the given
  /// builder.
  ConstraintLocator *
  getConstraintLocator(const ConstraintLocatorBuilder &builder);

  /// Lookup and return parent associated with given expression.
  Expr *getParentExpr(Expr *expr) {
    if (auto result = getExprDepthAndParent(expr))
      return result->second;
    return nullptr;
  }

  /// Retrieve the depth of the given expression.
  Optional<unsigned> getExprDepth(Expr *expr) {
    if (auto result = getExprDepthAndParent(expr))
      return result->first;
    return None;
  }

  /// Retrieve the depth and parent expression of the given expression.
  Optional<std::pair<unsigned, Expr *>> getExprDepthAndParent(Expr *expr);

  /// Returns a locator describing the callee for the anchor of a given locator.
  ///
  /// - For an unresolved dot/member anchor, this will be a locator describing
  /// the member.
  ///
  /// - For a subscript anchor, this will be a locator describing the subscript
  /// member.
  ///
  /// - For a key path anchor with a property/subscript component path element,
  /// this will be a locator describing the decl referenced by the component.
  ///
  /// - For a function application anchor, this will be a locator describing the
  /// 'direct callee' of the call. For example, for the expression \c x.foo?()
  /// the returned locator will describe the member \c foo.
  ///
  /// Note that because this function deals with the anchor, given a locator
  /// anchored on \c functionA(functionB()) with path elements pointing to the
  /// argument \c functionB(), the returned callee locator will describe
  /// \c functionA rather than \c functionB.
  ///
  /// \param locator The input locator.
  /// \param lookThroughApply Whether to look through applies. If false, a
  /// callee locator will only be returned for a direct reference such as
  /// \c x.foo rather than \c x.foo().
  /// \param getType The callback to fetch a type for given expression.
  /// \param simplifyType The callback to attempt to resolve any type
  ///                     variables which appear in the given type.
  /// \param getOverloadFor The callback to fetch overload for a given
  ///                       locator if available.
  ConstraintLocator *getCalleeLocator(
      ConstraintLocator *locator, bool lookThroughApply,
      llvm::function_ref<Type(Expr *)> getType,
      llvm::function_ref<Type(Type)> simplifyType,
      llvm::function_ref<Optional<SelectedOverload>(ConstraintLocator *)>
          getOverloadFor);

  ConstraintLocator *getCalleeLocator(ConstraintLocator *locator,
                                      bool lookThroughApply = true) {
    return getCalleeLocator(
        locator, lookThroughApply,
        [&](Expr *expr) -> Type { return getType(expr); },
        [&](Type type) -> Type { return simplifyType(type)->getRValueType(); },
        [&](ConstraintLocator *locator) -> Optional<SelectedOverload> {
          return findSelectedOverloadFor(locator);
        });
  }

  /// Determine whether the given declaration is unavailable from the
  /// current context.
  bool isDeclUnavailable(const Decl *D,
                         ConstraintLocator *locator = nullptr) const;

  /// Determine whether the given conformance is unavailable from the
  /// current context.
  bool isConformanceUnavailable(ProtocolConformanceRef conformance,
                                ConstraintLocator *locator = nullptr) const;

public:

  /// Whether we should attempt to fix problems.
  bool shouldAttemptFixes() const {
    if (!(Options & ConstraintSystemFlags::AllowFixes))
      return false;

    return !solverState || solverState->recordFixes;
  }

  ArrayRef<ConstraintFix *> getFixes() const { return Fixes; }

  bool shouldSuppressDiagnostics() const {
    return Options.contains(ConstraintSystemFlags::SuppressDiagnostics);
  }

  bool shouldReusePrecheckedType() const {
    return Options.contains(ConstraintSystemFlags::ReusePrecheckedType);
  }

  /// Whether we are solving to determine the possible types of a
  /// \c CodeCompletionExpr.
  bool isForCodeCompletion() const {
    return Options.contains(ConstraintSystemFlags::ForCodeCompletion);
  }

  /// Log and record the application of the fix. Return true iff any
  /// subsequent solution would be worse than the best known solution.
  bool recordFix(ConstraintFix *fix, unsigned impact = 1);

  void recordPotentialHole(Type type);

  void recordMatchCallArgumentResult(ConstraintLocator *locator,
                                     MatchCallArgumentResult result);

  /// Walk a closure AST to determine its effects.
  ///
  /// \returns a function's extended info describing the effects, as
  /// determined syntactically.
  FunctionType::ExtInfo closureEffects(ClosureExpr *expr);

  /// Determine whether the given context is asynchronous, e.g., an async
  /// function or closure.
  bool isAsynchronousContext(DeclContext *dc);

  /// Determine whether constraint system already has a fix recorded
  /// for a particular location.
  bool hasFixFor(ConstraintLocator *locator,
                 Optional<FixKind> expectedKind = None) const {
    return llvm::any_of(
        Fixes, [&locator, &expectedKind](const ConstraintFix *fix) {
          if (fix->getLocator() == locator) {
            return !expectedKind || fix->getKind() == *expectedKind;
          }
          return false;
        });
  }

  /// If an UnresolvedDotExpr, SubscriptMember, etc has been resolved by the
  /// constraint system, return the decl that it references.
  ValueDecl *findResolvedMemberRef(ConstraintLocator *locator);

  /// Try to salvage the constraint system by applying (speculative)
  /// fixes.
  SolutionResult salvage();
  
  /// Mine the active and inactive constraints in the constraint
  /// system to generate a plausible diagnosis of why the system could not be
  /// solved.
  ///
  /// \param target The solution target whose constraints we're investigating
  /// for a better diagnostic.
  ///
  /// Assuming that this constraint system is actually erroneous, this *always*
  /// emits an error message.
  void diagnoseFailureFor(SolutionApplicationTarget target);

  bool diagnoseAmbiguity(ArrayRef<Solution> solutions);
  bool diagnoseAmbiguityWithFixes(SmallVectorImpl<Solution> &solutions);

  /// Add a constraint to the constraint system.
  void addConstraint(ConstraintKind kind, Type first, Type second,
                     ConstraintLocatorBuilder locator,
                     bool isFavored = false);

  /// Add a requirement as a constraint to the constraint system.
  void addConstraint(Requirement req, ConstraintLocatorBuilder locator,
                     bool isFavored = false);

  /// Add the appropriate constraint for a contextual conversion.
  void addContextualConversionConstraint(
      Expr *expr, Type conversionType, ContextualTypePurpose purpose,
      bool isOpaqueReturnType);

  /// Convenience function to pass an \c ArrayRef to \c addJoinConstraint
  Type addJoinConstraint(ConstraintLocator *locator,
                         ArrayRef<std::pair<Type, ConstraintLocator *>> inputs,
                         Optional<Type> supertype = None) {
    return addJoinConstraint<decltype(inputs)::iterator>(
        locator, inputs.begin(), inputs.end(), supertype, [](auto it) { return *it; });
  }

  /// Add a "join" constraint between a set of types, producing the common
  /// supertype.
  ///
  /// Currently, a "join" is modeled by a set of conversion constraints to
  /// a new type variable or a specified supertype. At some point, we may want
  /// a new constraint kind to cover the join.
  ///
  /// \note This method will merge any input type variables for atomic literal
  /// expressions of the same kind. It assumes that if same-kind literal type
  /// variables are joined, there will be no differing constraints on those
  /// type variables.
  ///
  /// \returns the joined type, which is generally a new type variable, unless there are
  /// fewer than 2 input types or the \c supertype parameter is specified.
  template<typename Iterator>
  Type addJoinConstraint(ConstraintLocator *locator,
                         Iterator begin, Iterator end,
                         Optional<Type> supertype,
                         std::function<std::pair<Type, ConstraintLocator *>(Iterator)> getType) {
    if (begin == end)
      return Type();

    // No need to generate a new type variable if there's only one type to join
    if ((begin + 1 == end) && !supertype.hasValue())
      return getType(begin).first;

    // The type to capture the result of the join, which is either the specified supertype,
    // or a new type variable.
    Type resultTy = supertype.hasValue() ? supertype.getValue() :
                    createTypeVariable(locator, (TVO_PrefersSubtypeBinding | TVO_CanBindToNoEscape));

    using RawExprKind = uint8_t;
    llvm::SmallDenseMap<RawExprKind, TypeVariableType *> representativeForKind;

    // Join the input types.
    while (begin != end) {
      Type type;
      ConstraintLocator *locator;
      std::tie(type, locator) = getType(begin++);

      // We can merge the type variables of same-kind atomic literal expressions because they
      // will all have the same set of constraints and therefore can never resolve to anything
      // different.
      if (auto *typeVar = type->getAs<TypeVariableType>()) {
        if (auto literalKind = typeVar->getImpl().getAtomicLiteralKind()) {
          auto *&originalRep = representativeForKind[RawExprKind(*literalKind)];
          auto *currentRep = getRepresentative(typeVar);

          if (originalRep) {
            if (originalRep != currentRep)
              mergeEquivalenceClasses(currentRep, originalRep, /*updateWorkList=*/false);
            continue;
          }

          originalRep = currentRep;
        }
      }

      // Introduce conversions from each input type to the supertype.
      addConstraint(ConstraintKind::Conversion, type, resultTy, locator);
    }

    return resultTy;
  }

  /// Add a constraint to the constraint system with an associated fix.
  void addFixConstraint(ConstraintFix *fix, ConstraintKind kind,
                        Type first, Type second,
                        ConstraintLocatorBuilder locator,
                        bool isFavored = false);

  /// Add a key path application constraint to the constraint system.
  void addKeyPathApplicationConstraint(Type keypath, Type root, Type value,
                                       ConstraintLocatorBuilder locator,
                                       bool isFavored = false);

  /// Add a key path constraint to the constraint system.
  void addKeyPathConstraint(Type keypath, Type root, Type value,
                            ArrayRef<TypeVariableType *> componentTypeVars,
                            ConstraintLocatorBuilder locator,
                            bool isFavored = false);

  /// Add a new constraint with a restriction on its application.
  void addRestrictedConstraint(ConstraintKind kind,
                               ConversionRestrictionKind restriction,
                               Type first, Type second,
                               ConstraintLocatorBuilder locator);

  /// Add a constraint that binds an overload set to a specific choice.
  void addBindOverloadConstraint(Type boundTy, OverloadChoice choice,
                                 ConstraintLocator *locator,
                                 DeclContext *useDC) {
    resolveOverload(locator, boundTy, choice, useDC);
  }

  /// Add a value member constraint to the constraint system.
  void addValueMemberConstraint(Type baseTy, DeclNameRef name, Type memberTy,
                                DeclContext *useDC,
                                FunctionRefKind functionRefKind,
                                ArrayRef<OverloadChoice> outerAlternatives,
                                ConstraintLocatorBuilder locator) {
    assert(baseTy);
    assert(memberTy);
    assert(name);
    assert(useDC);
    switch (simplifyMemberConstraint(
        ConstraintKind::ValueMember, baseTy, name, memberTy, useDC,
        functionRefKind, outerAlternatives, TMF_GenerateConstraints, locator)) {
    case SolutionKind::Unsolved:
      llvm_unreachable("Unsolved result when generating constraints!");

    case SolutionKind::Solved:
      break;

    case SolutionKind::Error:
      if (shouldRecordFailedConstraint()) {
        recordFailedConstraint(Constraint::createMemberOrOuterDisjunction(
            *this, ConstraintKind::ValueMember, baseTy, memberTy, name, useDC,
            functionRefKind, outerAlternatives, getConstraintLocator(locator)));
      }
      break;
    }
  }

  /// Add a value member constraint for an UnresolvedMemberRef
  /// to the constraint system.
  void addUnresolvedValueMemberConstraint(Type baseTy, DeclNameRef name,
                                          Type memberTy, DeclContext *useDC,
                                          FunctionRefKind functionRefKind,
                                          ConstraintLocatorBuilder locator) {
    assert(baseTy);
    assert(memberTy);
    assert(name);
    assert(useDC);
    switch (simplifyMemberConstraint(ConstraintKind::UnresolvedValueMember,
                                     baseTy, name, memberTy,
                                     useDC, functionRefKind,
                                     /*outerAlternatives=*/{},
                                     TMF_GenerateConstraints, locator)) {
    case SolutionKind::Unsolved:
      llvm_unreachable("Unsolved result when generating constraints!");

    case SolutionKind::Solved:
      break;

    case SolutionKind::Error:
      if (shouldRecordFailedConstraint()) {
        recordFailedConstraint(
          Constraint::createMember(*this, ConstraintKind::UnresolvedValueMember,
                                   baseTy, memberTy, name,
                                   useDC, functionRefKind,
                                   getConstraintLocator(locator)));
      }
      break;
    }
  }

  /// Add a value witness constraint to the constraint system.
  void addValueWitnessConstraint(
      Type baseTy, ValueDecl *requirement, Type memberTy, DeclContext *useDC,
      FunctionRefKind functionRefKind, ConstraintLocatorBuilder locator) {
    assert(baseTy);
    assert(memberTy);
    assert(requirement);
    assert(useDC);
    switch (simplifyValueWitnessConstraint(
        ConstraintKind::ValueWitness, baseTy, requirement, memberTy, useDC,
        functionRefKind, TMF_GenerateConstraints, locator)) {
    case SolutionKind::Unsolved:
      llvm_unreachable("Unsolved result when generating constraints!");

    case SolutionKind::Solved:
    case SolutionKind::Error:
      break;
    }
  }

  /// Add an explicit conversion constraint (e.g., \c 'x as T').
  ///
  /// \param fromType The type of the expression being converted.
  /// \param toType The type to convert to.
  /// \param rememberChoice Whether the conversion disjunction should record its
  /// choice.
  /// \param locator The locator.
  /// \param compatFix A compatibility fix that can be applied if the conversion
  /// fails.
  void addExplicitConversionConstraint(Type fromType, Type toType,
                                       RememberChoice_t rememberChoice,
                                       ConstraintLocatorBuilder locator,
                                       ConstraintFix *compatFix = nullptr);

  /// Add a disjunction constraint.
  void
  addDisjunctionConstraint(ArrayRef<Constraint *> constraints,
                           ConstraintLocatorBuilder locator,
                           RememberChoice_t rememberChoice = ForgetChoice) {
    auto constraint =
      Constraint::createDisjunction(*this, constraints,
                                    getConstraintLocator(locator),
                                    rememberChoice);

    addUnsolvedConstraint(constraint);
  }

  /// Whether we should record the failure of a constraint.
  bool shouldRecordFailedConstraint() const {
    // If we're debugging, always note a failure so we can print it out.
    if (isDebugMode())
      return true;

    // Otherwise, only record it if we don't already have a failed constraint.
    // This avoids allocating unnecessary constraints.
    return !failedConstraint;
  }

  /// Note that a particular constraint has failed, setting \c failedConstraint
  /// if necessary.
  void recordFailedConstraint(Constraint *constraint) {
    assert(!constraint->isActive());
    if (!failedConstraint)
      failedConstraint = constraint;

    if (isDebugMode()) {
      auto &log = llvm::errs();
      log.indent(solverState ? solverState->depth * 2 : 0)
          << "(failed constraint ";
      constraint->print(log, &getASTContext().SourceMgr);
      log << ")\n";
    }
  }

  /// Remove a constraint from the system that has failed, setting
  /// \c failedConstraint if necessary.
  void retireFailedConstraint(Constraint *constraint) {
    retireConstraint(constraint);
    recordFailedConstraint(constraint);
  }

  /// Add a newly-generated constraint that is known not to be solvable
  /// right now.
  void addUnsolvedConstraint(Constraint *constraint) {
    // We couldn't solve this constraint; add it to the pile.
    InactiveConstraints.push_back(constraint);

    // Add this constraint to the constraint graph.
    CG.addConstraint(constraint);

    // Record this as a newly-generated constraint.
    if (solverState)
      solverState->addGeneratedConstraint(constraint);
  }

  /// Remove an inactive constraint from the current constraint graph.
  void removeInactiveConstraint(Constraint *constraint) {
    CG.removeConstraint(constraint);
    InactiveConstraints.erase(constraint);

    if (solverState)
      solverState->retireConstraint(constraint);
  }

  /// Transfer given constraint from to active list
  /// for solver to attempt its simplification.
  void activateConstraint(Constraint *constraint) {
    assert(!constraint->isActive() && "Constraint is already active");
    ActiveConstraints.splice(ActiveConstraints.end(), InactiveConstraints,
                             constraint);
    constraint->setActive(true);
  }

  void deactivateConstraint(Constraint *constraint) {
    assert(constraint->isActive() && "Constraint is already inactive");
    InactiveConstraints.splice(InactiveConstraints.end(),
                               ActiveConstraints, constraint);
    constraint->setActive(false);
  }

  void retireConstraint(Constraint *constraint) {
    if (constraint->isActive())
      deactivateConstraint(constraint);
    removeInactiveConstraint(constraint);
  }

  /// Note that this constraint is "favored" within its disjunction, and
  /// should be tried first to the exclusion of non-favored constraints in
  /// the same disjunction.
  void favorConstraint(Constraint *constraint) {
    if (constraint->isFavored())
      return;

    if (solverState) {
      solverState->favorConstraint(constraint);
    } else {
      constraint->setFavored();
    }
  }

  /// Retrieve the list of inactive constraints.
  ConstraintList &getConstraints() { return InactiveConstraints; }

  /// The worklist of "active" constraints that should be revisited
  /// due to a change.
  ConstraintList &getActiveConstraints() { return ActiveConstraints; }

  void findConstraints(SmallVectorImpl<Constraint *> &found,
                       llvm::function_ref<bool(const Constraint &)> pred) {
    filterConstraints(ActiveConstraints, pred, found);
    filterConstraints(InactiveConstraints, pred, found);
  }

  /// Retrieve the representative of the equivalence class containing
  /// this type variable.
  TypeVariableType *getRepresentative(TypeVariableType *typeVar) const {
    return typeVar->getImpl().getRepresentative(getSavedBindings());
  }

  /// Find if the given type variable is representative for a type
  /// variable which last locator path element is of the specified kind.
  /// If true returns the type variable which it is the representative for.
  TypeVariableType *
  isRepresentativeFor(TypeVariableType *typeVar,
                      ConstraintLocator::PathElementKind kind) const;

  /// Gets the VarDecl associateed with resolvedOverload, and the type of the
  /// projection if the decl has an associated property wrapper with a projectedValue.
  Optional<std::pair<VarDecl *, Type>>
  getPropertyWrapperProjectionInfo(SelectedOverload resolvedOverload);

  /// Gets the VarDecl associateed with resolvedOverload, and the type of the
  /// backing storage if the decl has an associated property wrapper.
  Optional<std::pair<VarDecl *, Type>>
  getPropertyWrapperInformation(SelectedOverload resolvedOverload);

  /// Gets the VarDecl, and the type of the type property that it wraps if
  /// resolved overload has a decl which is the backing storage for a
  /// property wrapper.
  Optional<std::pair<VarDecl *, Type>>
  getWrappedPropertyInformation(SelectedOverload resolvedOverload);

  /// Merge the equivalence sets of the two type variables.
  ///
  /// Note that both \c typeVar1 and \c typeVar2 must be the
  /// representatives of their equivalence classes, and must be
  /// distinct.
  void mergeEquivalenceClasses(TypeVariableType *typeVar1,
                               TypeVariableType *typeVar2,
                               bool updateWorkList);

  /// Flags that direct type matching.
  enum TypeMatchFlags {
    /// Indicates that we are in a context where we should be
    /// generating constraints for any unsolvable problems.
    ///
    /// This flag is automatically introduced when type matching destructures
    /// a type constructor (tuple, function type, etc.), solving that
    /// constraint while potentially generating others.
    TMF_GenerateConstraints = 0x01,

    /// Indicates that we are applying a fix.
    TMF_ApplyingFix = 0x02,

    /// Indicates that we are attempting a possible type for
    /// a type variable.
    TMF_BindingTypeVariable = 0x04,
  };

  /// Options that govern how type matching should proceed.
  using TypeMatchOptions = OptionSet<TypeMatchFlags>;

  /// Retrieve the fixed type corresponding to the given type variable,
  /// or a null type if there is no fixed type.
  Type getFixedType(TypeVariableType *typeVar) const {
    return typeVar->getImpl().getFixedType(getSavedBindings());
  }

  /// Retrieve the fixed type corresponding to a given type variable,
  /// recursively, until we hit something that isn't a type variable
  /// or a type variable that doesn't have a fixed type.
  ///
  /// \param type The type to simplify.
  ///
  /// \param wantRValue Whether this routine should look through
  /// lvalues at each step.
  Type getFixedTypeRecursive(Type type, bool wantRValue) const {
    TypeMatchOptions flags = None;
    return getFixedTypeRecursive(type, flags, wantRValue);
  }

  /// Retrieve the fixed type corresponding to a given type variable,
  /// recursively, until we hit something that isn't a type variable
  /// or a type variable that doesn't have a fixed type.
  ///
  /// \param type The type to simplify.
  ///
  /// \param flags When simplifying one of the types that is part of a
  /// constraint we are examining, the set of flags that governs the
  /// simplification. The set of flags may be both queried and mutated.
  ///
  /// \param wantRValue Whether this routine should look through
  /// lvalues at each step.
  Type getFixedTypeRecursive(Type type, TypeMatchOptions &flags,
                             bool wantRValue) const;

  /// Determine whether the given type variable occurs within the given type.
  ///
  /// This routine assumes that the type has already been fully simplified.
  ///
  /// \param involvesOtherTypeVariables if non-null, records whether any other
  /// type variables are present in the type.
  static bool typeVarOccursInType(TypeVariableType *typeVar, Type type,
                                  bool *involvesOtherTypeVariables = nullptr);

  /// Given the fact that contextual type is now available for the type
  /// variable representing one of the closures, let's set pre-determined
  /// closure type and generate constraints for its body, iff it's a
  /// single-statement closure.
  ///
  /// \param typeVar The type variable representing a function type of the
  /// closure expression.
  /// \param contextualType The contextual type this closure would be
  /// converted to.
  /// \param locator The locator associated with contextual type.
  ///
  /// \returns `true` if it was possible to generate constraints for
  /// the body and assign fixed type to the closure, `false` otherwise.
  bool resolveClosure(TypeVariableType *typeVar, Type contextualType,
                      ConstraintLocatorBuilder locator);

  /// Assign a fixed type to the given type variable.
  ///
  /// \param typeVar The type variable to bind.
  ///
  /// \param type The fixed type to which the type variable will be bound.
  ///
  /// \param updateState Whether to update the state based on this binding.
  /// False when we're only assigning a type as part of reconstructing
  /// a complete solution from partial solutions.
  ///
  /// \param notifyBindingInference Whether to notify binding inference about
  /// the change to this type variable.
  void assignFixedType(TypeVariableType *typeVar, Type type,
                       bool updateState = true,
                       bool notifyBindingInference = true);

  /// Determine if the type in question is an Array<T> and, if so, provide the
  /// element type of the array.
  static Optional<Type> isArrayType(Type type);

  /// Determine whether the given type is a dictionary and, if so, provide the
  /// key and value types for the dictionary.
  static Optional<std::pair<Type, Type>> isDictionaryType(Type type);

  /// Determine if the type in question is a Set<T> and, if so, provide the
  /// element type of the set.
  static Optional<Type> isSetType(Type t);

  /// Determine if the type in question is AnyHashable.
  static bool isAnyHashableType(Type t);

  /// Call Expr::isTypeReference on the given expression, using a
  /// custom accessor for the type on the expression that reads the
  /// type from the ConstraintSystem expression type map.
  bool isTypeReference(Expr *E);

  /// Call Expr::isIsStaticallyDerivedMetatype on the given
  /// expression, using a custom accessor for the type on the
  /// expression that reads the type from the ConstraintSystem
  /// expression type map.
  bool isStaticallyDerivedMetatype(Expr *E);

  /// Call TypeExpr::getInstanceType on the given expression, using a
  /// custom accessor for the type on the expression that reads the
  /// type from the ConstraintSystem expression type map.
  Type getInstanceType(TypeExpr *E);

  /// Call AbstractClosureExpr::getResultType on the given expression,
  /// using a custom accessor for the type on the expression that
  /// reads the type from the ConstraintSystem expression type map.
  Type getResultType(const AbstractClosureExpr *E);

private:
  /// Introduce the constraints associated with the given type variable
  /// into the worklist.
  void addTypeVariableConstraintsToWorkList(TypeVariableType *typeVar);

  static void
  filterConstraints(ConstraintList &constraints,
                    llvm::function_ref<bool(const Constraint &)> pred,
                    SmallVectorImpl<Constraint *> &found) {
    for (auto &constraint : constraints) {
      if (pred(constraint))
        found.push_back(&constraint);
    }
  }

public:

  /// Coerce the given expression to an rvalue, if it isn't already.
  Expr *coerceToRValue(Expr *expr);

  /// Add implicit "load" expressions to the given expression.
  Expr *addImplicitLoadExpr(Expr *expr);

  /// "Open" the unbound generic type represented by the given declaration and
  /// parent type by introducing fresh type variables for generic parameters
  /// and constructing a bound generic type from these type variables.
  ///
  /// \returns The opened type.
  Type openUnboundGenericType(GenericTypeDecl *decl, Type parentTy,
                              ConstraintLocatorBuilder locator);

  /// Replace placeholder types with fresh type variables, and unbound generic
  /// types with bound generic types whose generic args are fresh type
  /// variables.
  ///
  /// \param type The type on which to perform the conversion.
  ///
  /// \returns The converted type.
  Type replaceInferableTypesWithTypeVars(Type type,
                                         ConstraintLocatorBuilder locator);

  /// "Open" the given type by replacing any occurrences of generic
  /// parameter types and dependent member types with fresh type variables.
  ///
  /// \param type The type to open.
  ///
  /// \returns The opened type, or \c type if there are no archetypes in it.
  Type openType(Type type, OpenedTypeMap &replacements);

  /// "Open" the given function type.
  ///
  /// If the function type is non-generic, this is equivalent to calling
  /// openType(). Otherwise, it calls openGeneric() on the generic
  /// function's signature first.
  ///
  /// \param funcType The function type to open.
  ///
  /// \param replacements The mapping from opened types to the type
  /// variables to which they were opened.
  ///
  /// \param outerDC The generic context containing the declaration.
  ///
  /// \returns The opened type, or \c type if there are no archetypes in it.
  FunctionType *openFunctionType(AnyFunctionType *funcType,
                                 ConstraintLocatorBuilder locator,
                                 OpenedTypeMap &replacements,
                                 DeclContext *outerDC);

  /// Open the generic parameter list and its requirements,
  /// creating type variables for each of the type parameters.
  void openGeneric(DeclContext *outerDC,
                   GenericSignature signature,
                   ConstraintLocatorBuilder locator,
                   OpenedTypeMap &replacements);

  /// Open the generic parameter list creating type variables for each of the
  /// type parameters.
  void openGenericParameters(DeclContext *outerDC,
                             GenericSignature signature,
                             OpenedTypeMap &replacements,
                             ConstraintLocatorBuilder locator);

  /// Given generic signature open its generic requirements,
  /// using substitution function, and record them in the
  /// constraint system for further processing.
  void openGenericRequirements(DeclContext *outerDC,
                               GenericSignature signature,
                               bool skipProtocolSelfConstraint,
                               ConstraintLocatorBuilder locator,
                               llvm::function_ref<Type(Type)> subst);

  /// Record the set of opened types for the given locator.
  void recordOpenedTypes(
         ConstraintLocatorBuilder locator,
         const OpenedTypeMap &replacements);

  /// Retrieve the type of a reference to the given value declaration.
  ///
  /// For references to polymorphic function types, this routine "opens up"
  /// the type by replacing each instance of an archetype with a fresh type
  /// variable.
  ///
  /// \param decl The declarations whose type is being computed.
  ///
  /// \returns a pair containing the full opened type (if applicable) and
  /// opened type of a reference to declaration.
  std::pair<Type, Type> getTypeOfReference(
                          ValueDecl *decl,
                          FunctionRefKind functionRefKind,
                          ConstraintLocatorBuilder locator,
                          DeclContext *useDC);

  /// Return the type-of-reference of the given value.
  ///
  /// \param baseType if non-null, return the type of a member reference to
  ///   this value when the base has the given type
  ///
  /// \param UseDC The context of the access.  Some variables have different
  ///   types depending on where they are used.
  ///
  /// \param memberLocator The locator anchored at this value reference, when
  /// it is a member reference.
  ///
  /// \param wantInterfaceType Whether we want the interface type, if available.
  Type getUnopenedTypeOfReference(VarDecl *value, Type baseType,
                                  DeclContext *UseDC,
                                  ConstraintLocator *memberLocator = nullptr,
                                  bool wantInterfaceType = false);

  /// Return the type-of-reference of the given value.
  ///
  /// \param baseType if non-null, return the type of a member reference to
  ///   this value when the base has the given type
  ///
  /// \param UseDC The context of the access.  Some variables have different
  ///   types depending on where they are used.
  ///
  /// \param memberLocator The locator anchored at this value reference, when
  /// it is a member reference.
  ///
  /// \param wantInterfaceType Whether we want the interface type, if available.
  ///
  /// \param getType Optional callback to extract a type for given declaration.
  static Type
  getUnopenedTypeOfReference(VarDecl *value, Type baseType, DeclContext *UseDC,
                             llvm::function_ref<Type(VarDecl *)> getType,
                             ConstraintLocator *memberLocator = nullptr,
                             bool wantInterfaceType = false);

  /// Retrieve the type of a reference to the given value declaration,
  /// as a member with a base of the given type.
  ///
  /// For references to generic function types or members of generic types,
  /// this routine "opens up" the type by replacing each instance of a generic
  /// parameter with a fresh type variable.
  ///
  /// \param isDynamicResult Indicates that this declaration was found via
  /// dynamic lookup.
  ///
  /// \returns a pair containing the full opened type (which includes the opened
  /// base) and opened type of a reference to this member.
  std::pair<Type, Type> getTypeOfMemberReference(
                          Type baseTy, ValueDecl *decl, DeclContext *useDC,
                          bool isDynamicResult,
                          FunctionRefKind functionRefKind,
                          ConstraintLocator *locator,
                          OpenedTypeMap *replacements = nullptr);

  /// Retrieve a list of generic parameter types solver has "opened" (replaced
  /// with a type variable) along the current path.
  ArrayRef<std::pair<ConstraintLocator *, ArrayRef<OpenedType>>>
  getOpenedTypes() const {
    return OpenedTypes;
  }

private:
  /// Adjust the constraint system to accomodate the given selected overload, and
  /// recompute the type of the referenced declaration.
  ///
  /// \returns a pair containing the adjusted opened type of a reference to
  /// this member and a bit indicating whether or not a bind constraint was added.
  std::pair<Type, bool> adjustTypeOfOverloadReference(
      const OverloadChoice &choice, ConstraintLocator *locator, Type boundType,
      Type refType);

  /// Add the constraints needed to bind an overload's type variable.
  void bindOverloadType(
      const SelectedOverload &overload, Type boundType,
      ConstraintLocator *locator, DeclContext *useDC,
      llvm::function_ref<void(unsigned int, Type, ConstraintLocator *)>
          verifyThatArgumentIsHashable);

  /// Describes a direction of optional wrapping, either increasing optionality
  /// or decreasing optionality.
  enum class OptionalWrappingDirection {
    /// Unwrap an optional type T? to T.
    Unwrap,

    /// Promote a type T to optional type T?.
    Promote
  };

  /// Attempts to find a constraint that involves \p typeVar and satisfies
  /// \p predicate, looking through optional object constraints if necessary. If
  /// multiple candidates are found, returns the first one.
  ///
  /// \param optionalDirection The direction to travel through optional object
  /// constraints, either increasing or decreasing optionality.
  ///
  /// \param predicate Checks whether a given constraint is the one being
  /// searched for. The type variable passed is the current representative
  /// after looking through the optional object constraints.
  ///
  /// \returns The constraint found along with the number of optional object
  /// constraints looked through, or \c None if no constraint was found.
  Optional<std::pair<Constraint *, unsigned>> findConstraintThroughOptionals(
      TypeVariableType *typeVar, OptionalWrappingDirection optionalDirection,
      llvm::function_ref<bool(Constraint *, TypeVariableType *)> predicate);

  /// Attempt to simplify the set of overloads corresponding to a given
  /// function application constraint.
  ///
  /// \param disjunction The disjunction for the set of overloads.
  ///
  /// \param fnTypeVar The type variable that describes the set of
  /// overloads for the function.
  ///
  /// \param argFnType The call signature, which includes the call arguments
  /// (as the function parameters) and the expected result type of the
  /// call.
  ///
  /// \param numOptionalUnwraps The number of unwraps required to get the
  /// underlying function from the overload choice.
  ///
  /// \returns \c true if an error was encountered, \c false otherwise.
  bool simplifyAppliedOverloadsImpl(Constraint *disjunction,
                                    TypeVariableType *fnTypeVar,
                                    FunctionType *argFnType,
                                    unsigned numOptionalUnwraps,
                                    ConstraintLocatorBuilder locator);

public:
  /// Attempt to simplify the set of overloads corresponding to a given
  /// bind overload disjunction.
  ///
  /// \param disjunction The disjunction for the set of overloads.
  ///
  /// \returns \c true if an error was encountered, \c false otherwise.
  bool simplifyAppliedOverloads(Constraint *disjunction,
                                ConstraintLocatorBuilder locator);

  /// Attempt to simplify the set of overloads corresponding to a given
  /// function application constraint.
  ///
  /// \param fnType The type that describes the set of overloads for the
  /// function.
  ///
  /// \param argFnType The call signature, which includes the call arguments
  /// (as the function parameters) and the expected result type of the
  /// call.
  ///
  /// \returns \c true if an error was encountered, \c false otherwise.
  bool simplifyAppliedOverloads(Type fnType, FunctionType *argFnType,
                                ConstraintLocatorBuilder locator);

  /// Retrieve the type that will be used when matching the given overload.
  Type getEffectiveOverloadType(ConstraintLocator *locator,
                                const OverloadChoice &overload,
                                bool allowMembers,
                                DeclContext *useDC);

  /// Add a new overload set to the list of unresolved overload
  /// sets.
  void addOverloadSet(Type boundType, ArrayRef<OverloadChoice> choices,
                      DeclContext *useDC, ConstraintLocator *locator,
                      Optional<unsigned> favoredIndex = None);

  void addOverloadSet(ArrayRef<Constraint *> choices,
                      ConstraintLocator *locator);

  /// Retrieve the allocator used by this constraint system.
  llvm::BumpPtrAllocator &getAllocator() { return Allocator; }

  template <typename It>
  ArrayRef<typename std::iterator_traits<It>::value_type>
  allocateCopy(It start, It end) {
    using T = typename std::iterator_traits<It>::value_type;
    T *result = (T*)getAllocator().Allocate(sizeof(T)*(end-start), alignof(T));
    unsigned i;
    for (i = 0; start != end; ++start, ++i)
      new (result+i) T(*start);
    return ArrayRef<T>(result, i);
  }

  template<typename T>
  ArrayRef<T> allocateCopy(ArrayRef<T> array) {
    return allocateCopy(array.begin(), array.end());
  }

  template<typename T>
  ArrayRef<T> allocateCopy(SmallVectorImpl<T> &vec) {
    return allocateCopy(vec.begin(), vec.end());
  }

  /// Generate constraints for the given solution target.
  ///
  /// \returns true if an error occurred, false otherwise.
  bool generateConstraints(SolutionApplicationTarget &target,
                           FreeTypeVariableBinding allowFreeTypeVariables);

  /// Generate constraints for the body of the given closure.
  ///
  /// \param closure the closure expression
  /// \param resultType the closure's result type
  ///
  /// \returns \c true if constraint generation failed, \c false otherwise
  bool generateConstraints(ClosureExpr *closure, Type resultType);

  /// Generate constraints for the given (unchecked) expression.
  ///
  /// \returns a possibly-sanitized expression, or null if an error occurred.
  Expr *generateConstraints(Expr *E, DeclContext *dc,
                            bool isInputExpression = true);

  /// Generate constraints for binding the given pattern to the
  /// value of the given expression.
  ///
  /// \returns a possibly-sanitized initializer, or null if an error occurred.
  Type generateConstraints(Pattern *P, ConstraintLocatorBuilder locator,
                           bool bindPatternVarsOneWay,
                           PatternBindingDecl *patternBinding,
                           unsigned patternIndex);

  /// Generate constraints for a statement condition.
  ///
  /// \returns true if there was an error in constraint generation, false
  /// if generation succeeded.
  bool generateConstraints(StmtCondition condition, DeclContext *dc);

  /// Generate constraints for a case statement.
  ///
  /// \param subjectType The type of the "subject" expression in the enclosing
  /// switch statement.
  ///
  /// \returns true if there was an error in constraint generation, false
  /// if generation succeeded.
  bool generateConstraints(CaseStmt *caseStmt, DeclContext *dc,
                           Type subjectType, ConstraintLocator *locator);

  /// Generate constraints for a given set of overload choices.
  ///
  /// \param constraints The container of generated constraint choices.
  ///
  /// \param type The type each choice should be bound to.
  ///
  /// \param choices The set of choices to convert into bind overload
  /// constraints so solver could attempt each one.
  ///
  /// \param useDC The declaration context where each choice is used.
  ///
  /// \param locator The locator to use when generating constraints.
  ///
  /// \param favoredIndex If there is a "favored" or preferred choice
  /// this is its index in the set of choices.
  ///
  /// \param requiresFix Determines whether choices require a fix to
  /// be included in the result. If the fix couldn't be provided by
  /// `getFix` for any given choice, such choice would be filtered out.
  ///
  /// \param getFix Optional callback to determine a fix for a given
  /// choice (first argument is a position of current choice,
  /// second - the choice in question).
  void generateConstraints(
      SmallVectorImpl<Constraint *> &constraints, Type type,
      ArrayRef<OverloadChoice> choices, DeclContext *useDC,
      ConstraintLocator *locator, Optional<unsigned> favoredIndex = None,
      bool requiresFix = false,
      llvm::function_ref<ConstraintFix *(unsigned, const OverloadChoice &)>
          getFix = [](unsigned, const OverloadChoice &) { return nullptr; });

  /// Propagate constraints in an effort to enforce local
  /// consistency to reduce the time to solve the system.
  ///
  /// \returns true if the system is known to be inconsistent (have no
  /// solutions).
  bool propagateConstraints();

  /// The result of attempting to resolve a constraint or set of
  /// constraints.
  enum class SolutionKind : char {
    /// The constraint has been solved completely, and provides no
    /// more information.
    Solved,
    /// The constraint could not be solved at this point.
    Unsolved,
    /// The constraint uncovers an inconsistency in the system.
    Error
  };

  class TypeMatchResult {
    SolutionKind Kind;

  public:
    inline bool isSuccess() const { return Kind == SolutionKind::Solved; }
    inline bool isFailure() const { return Kind == SolutionKind::Error; }
    inline bool isAmbiguous() const { return Kind == SolutionKind::Unsolved; }

    static TypeMatchResult success(ConstraintSystem &cs) {
      return {SolutionKind::Solved};
    }

    static TypeMatchResult failure(ConstraintSystem &cs,
                                   ConstraintLocatorBuilder location) {
      return {SolutionKind::Error};
    }

    static TypeMatchResult ambiguous(ConstraintSystem &cs) {
      return {SolutionKind::Unsolved};
    }

    operator SolutionKind() { return Kind; }
  private:
    TypeMatchResult(SolutionKind result) : Kind(result) {}
  };

  /// Attempt to repair typing failures and record fixes if needed.
  /// \return true if at least some of the failures has been repaired
  /// successfully, which allows type matcher to continue.
  bool repairFailures(Type lhs, Type rhs, ConstraintKind matchKind,
                      SmallVectorImpl<RestrictionOrFix> &conversionsOrFixes,
                      ConstraintLocatorBuilder locator);

  /// Subroutine of \c matchTypes(), which matches up two tuple types.
  ///
  /// \returns the result of performing the tuple-to-tuple conversion.
  TypeMatchResult matchTupleTypes(TupleType *tuple1, TupleType *tuple2,
                                  ConstraintKind kind, TypeMatchOptions flags,
                                  ConstraintLocatorBuilder locator);

  /// Subroutine of \c matchTypes(), which matches a scalar type to
  /// a tuple type.
  ///
  /// \returns the result of performing the scalar-to-tuple conversion.
  TypeMatchResult matchScalarToTupleTypes(Type type1, TupleType *tuple2,
                                          ConstraintKind kind,
                                          TypeMatchOptions flags,
                                          ConstraintLocatorBuilder locator);

  /// Subroutine of \c matchTypes(), which matches up two function
  /// types.
  TypeMatchResult matchFunctionTypes(FunctionType *func1, FunctionType *func2,
                                     ConstraintKind kind, TypeMatchOptions flags,
                                     ConstraintLocatorBuilder locator);
  
  /// Subroutine of \c matchTypes(), which matches up a value to a
  /// superclass.
  TypeMatchResult matchSuperclassTypes(Type type1, Type type2,
                                       TypeMatchOptions flags,
                                       ConstraintLocatorBuilder locator);

  /// Subroutine of \c matchTypes(), which matches up two types that
  /// refer to the same declaration via their generic arguments.
  TypeMatchResult matchDeepEqualityTypes(Type type1, Type type2,
                                         ConstraintLocatorBuilder locator);

  /// Subroutine of \c matchTypes(), which matches up a value to an
  /// existential type.
  ///
  /// \param kind Either ConstraintKind::SelfObjectOfProtocol or
  /// ConstraintKind::ConformsTo. Usually this uses SelfObjectOfProtocol,
  /// but when matching the instance type of a metatype with the instance type
  /// of an existential metatype, since we want an actual conformance check.
  TypeMatchResult matchExistentialTypes(Type type1, Type type2,
                                        ConstraintKind kind,
                                        TypeMatchOptions flags,
                                        ConstraintLocatorBuilder locator);

  /// Subroutine of \c matchTypes(), used to bind a type to a
  /// type variable.
  TypeMatchResult matchTypesBindTypeVar(
      TypeVariableType *typeVar, Type type, ConstraintKind kind,
      TypeMatchOptions flags, ConstraintLocatorBuilder locator,
      llvm::function_ref<TypeMatchResult()> formUnsolvedResult);

public: // FIXME: public due to statics in CSSimplify.cpp
  /// Attempt to match up types \c type1 and \c type2, which in effect
  /// is solving the given type constraint between these two types.
  ///
  /// \param type1 The first type, which is on the left of the type relation.
  ///
  /// \param type2 The second type, which is on the right of the type relation.
  ///
  /// \param kind The kind of type match being performed, e.g., exact match,
  /// trivial subtyping, subtyping, or conversion.
  ///
  /// \param flags A set of flags composed from the TMF_* constants, which
  /// indicates how the constraint should be simplified.
  ///
  /// \param locator The locator that will be used to track the location of
  /// the specific types being matched.
  ///
  /// \returns the result of attempting to solve this constraint.
  TypeMatchResult matchTypes(Type type1, Type type2, ConstraintKind kind,
                             TypeMatchOptions flags,
                             ConstraintLocatorBuilder locator);

  TypeMatchResult getTypeMatchSuccess() {
    return TypeMatchResult::success(*this);
  }

  TypeMatchResult getTypeMatchFailure(ConstraintLocatorBuilder locator) {
    return TypeMatchResult::failure(*this, locator);
  }

  TypeMatchResult getTypeMatchAmbiguous() {
    return TypeMatchResult::ambiguous(*this);
  }

public:
  /// Given a function type where the eventual result type is an optional,
  /// where "eventual result type" is defined as:
  ///   1. The result type is an optional
  ///   2. The result type is a function type with an eventual result
  ///      type that is an optional.
  ///
  /// return the same function type but with the eventual result type
  /// replaced by its underlying type.
  ///
  /// i.e. return (S) -> T for (S) -> T?
  //       return (X) -> () -> Y for (X) -> () -> Y?
  Type replaceFinalResultTypeWithUnderlying(AnyFunctionType *fnTy) {
    auto resultTy = fnTy->getResult();
    if (auto *resultFnTy = resultTy->getAs<AnyFunctionType>())
      resultTy = replaceFinalResultTypeWithUnderlying(resultFnTy);
    else {
      auto objType =
          resultTy->getWithoutSpecifierType()->getOptionalObjectType();
      // Preserve l-value through force operation.
      resultTy =
          resultTy->is<LValueType>() ? LValueType::get(objType) : objType;
    }

    assert(resultTy);

    if (auto *genericFn = fnTy->getAs<GenericFunctionType>()) {
      return GenericFunctionType::get(genericFn->getGenericSignature(),
                                      genericFn->getParams(), resultTy,
                                      genericFn->getExtInfo());
    }

    return FunctionType::get(fnTy->getParams(), resultTy, fnTy->getExtInfo());
  }

  // Build a disjunction that attempts both T? and T for a particular
  // type binding. The choice of T? is preferred, and we will not
  // attempt T if we can type check with T?
  void
  buildDisjunctionForOptionalVsUnderlying(Type boundTy, Type type,
                                          ConstraintLocator *locator) {
    // NOTE: If we use other locator kinds for these disjunctions, we
    // need to account for it in solution scores for forced-unwraps.
    assert(locator->getPath().back().getKind() ==
               ConstraintLocator::ImplicitlyUnwrappedDisjunctionChoice ||
           locator->getPath().back().getKind() ==
               ConstraintLocator::DynamicLookupResult);

    // Create the constraint to bind to the optional type and make it
    // the favored choice.
    auto *bindToOptional =
      Constraint::create(*this, ConstraintKind::Bind, boundTy, type, locator);
    bindToOptional->setFavored();

    Type underlyingType;
    if (auto *fnTy = type->getAs<AnyFunctionType>())
      underlyingType = replaceFinalResultTypeWithUnderlying(fnTy);
    else
      underlyingType = type->getWithoutSpecifierType()->getOptionalObjectType();

    assert(underlyingType);

    if (type->is<LValueType>())
      underlyingType = LValueType::get(underlyingType);
    assert(!type->is<InOutType>());

    auto *bindToUnderlying = Constraint::create(
        *this, ConstraintKind::Bind, boundTy, underlyingType, locator);

    llvm::SmallVector<Constraint *, 2> choices = {bindToOptional,
                                                  bindToUnderlying};

    // Create the disjunction
    addDisjunctionConstraint(choices, locator, RememberChoice);
  }

  // Build a disjunction for types declared IUO.
  void
  buildDisjunctionForImplicitlyUnwrappedOptional(Type boundTy, Type type,
                                                 ConstraintLocator *locator) {
    auto *disjunctionLocator = getConstraintLocator(
        locator, ConstraintLocator::ImplicitlyUnwrappedDisjunctionChoice);
    buildDisjunctionForOptionalVsUnderlying(boundTy, type, disjunctionLocator);
  }

  // Build a disjunction for dynamic lookup results, which are
  // implicitly unwrapped if needed.
  void buildDisjunctionForDynamicLookupResult(Type boundTy, Type type,
                                              ConstraintLocator *locator) {
    auto *dynamicLocator =
        getConstraintLocator(locator, ConstraintLocator::DynamicLookupResult);
    buildDisjunctionForOptionalVsUnderlying(boundTy, type, dynamicLocator);
  }

  /// Resolve the given overload set to the given choice.
  void resolveOverload(ConstraintLocator *locator, Type boundType,
                       OverloadChoice choice, DeclContext *useDC);

  /// Simplify a type, by replacing type variables with either their
  /// fixed types (if available) or their representatives.
  ///
  /// The resulting types can be compared canonically, so long as additional
  /// type equivalence requirements aren't introduced between comparisons.
  Type simplifyType(Type type) const;

  /// Simplify a type, by replacing type variables with either their
  /// fixed types (if available) or their representatives.
  ///
  /// \param flags If the simplified type has changed, this will be updated
  /// to include \c TMF_GenerateConstraints.
  ///
  /// The resulting types can be compared canonically, so long as additional
  /// type equivalence requirements aren't introduced between comparisons.
  Type simplifyType(Type type, TypeMatchOptions &flags) {
    Type result = simplifyType(type);
    if (result.getPointer() != type.getPointer())
      flags |= TMF_GenerateConstraints;
    return result;
  }

  /// Given a ValueMember, UnresolvedValueMember, or TypeMember constraint,
  /// perform a lookup into the specified base type to find a candidate list.
  /// The list returned includes the viable candidates as well as the unviable
  /// ones (along with reasons why they aren't viable).
  ///
  /// If includeInaccessibleMembers is set to true, this burns compile time to
  /// try to identify and classify inaccessible members that may be being
  /// referenced.
  MemberLookupResult performMemberLookup(ConstraintKind constraintKind,
                                         DeclNameRef memberName, Type baseTy,
                                         FunctionRefKind functionRefKind,
                                         ConstraintLocator *memberLocator,
                                         bool includeInaccessibleMembers);

  /// Build implicit autoclosure expression wrapping a given expression.
  /// Given expression represents computed result of the closure.
  Expr *buildAutoClosureExpr(Expr *expr, FunctionType *closureType,
                             bool isDefaultWrappedValue = false,
                             bool isAsyncLetWrapper = false);

  /// Builds a type-erased return expression that can be used in dynamic
  /// replacement.
  ///
  /// An expression needs type erasure if:
  ///  1. The expression is a return value.
  ///  2. The enclosing function is dynamic or a dynamic replacement.
  ///  3. The enclosing function returns an opaque type.
  ///  4. The opaque type conforms to (exactly) one protocol, and the protocol
  ///     has a declared type eraser.
  ///
  /// \returns the transformed return expression, or the original expression if
  /// no type erasure is needed.
  Expr *buildTypeErasedExpr(Expr *expr, DeclContext *dc, Type contextualType,
                            ContextualTypePurpose purpose);

private:
  /// Determines whether or not a given conversion at a given locator requires
  /// the creation of a temporary value that's only valid for a limited scope.
  /// Such ephemeral conversions, such as array-to-pointer, cannot be passed to
  /// non-ephemeral parameters.
  ConversionEphemeralness
  isConversionEphemeral(ConversionRestrictionKind conversion,
                        ConstraintLocatorBuilder locator);

  /// Simplifies a type by replacing type variables with the result of
  /// \c getFixedTypeFn and performing lookup on dependent member types.
  Type simplifyTypeImpl(Type type,
      llvm::function_ref<Type(TypeVariableType *)> getFixedTypeFn) const;

  /// Attempt to simplify the given construction constraint.
  ///
  /// \param valueType The type being constructed.
  ///
  /// \param fnType The argument type that will be the input to the
  /// valueType initializer and the result type will be the result of
  /// calling that initializer.
  ///
  /// \param flags A set of flags composed from the TMF_* constants, which
  /// indicates how the constraint should be simplified.
  /// 
  /// \param locator Locator describing where this construction
  /// occurred.
  SolutionKind simplifyConstructionConstraint(Type valueType, 
                                              FunctionType *fnType,
                                              TypeMatchOptions flags,
                                              DeclContext *DC,
                                              FunctionRefKind functionRefKind,
                                              ConstraintLocator *locator);

  /// Attempt to simplify the given conformance constraint.
  ///
  /// \param type The type being tested.
  /// \param protocol The protocol to which the type should conform.
  /// \param kind Either ConstraintKind::SelfObjectOfProtocol or
  /// ConstraintKind::ConformsTo.
  /// \param locator Locator describing where this constraint occurred.
  SolutionKind simplifyConformsToConstraint(Type type, ProtocolDecl *protocol,
                                            ConstraintKind kind,
                                            ConstraintLocatorBuilder locator,
                                            TypeMatchOptions flags);

  /// Attempt to simplify the given conformance constraint.
  ///
  /// \param type The type being tested.
  /// \param protocol The protocol or protocol composition type to which the
  /// type should conform.
  /// \param locator Locator describing where this constraint occurred.
  ///
  /// \param kind If this is SelfTypeOfProtocol, we allow an existential type
  /// that contains the protocol but does not conform to it (eg, due to
  /// associated types).
  SolutionKind simplifyConformsToConstraint(Type type, Type protocol,
                                            ConstraintKind kind,
                                            ConstraintLocatorBuilder locator,
                                            TypeMatchOptions flags);

  /// Similar to \c simplifyConformsToConstraint but also checks for
  /// optional and pointer derived a given type.
  SolutionKind simplifyTransitivelyConformsTo(Type type, Type protocol,
                                              ConstraintLocatorBuilder locator,
                                              TypeMatchOptions flags);

  /// Attempt to simplify a checked-cast constraint.
  SolutionKind simplifyCheckedCastConstraint(Type fromType, Type toType,
                                             TypeMatchOptions flags,
                                             ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given member constraint.
  SolutionKind simplifyMemberConstraint(
      ConstraintKind kind, Type baseType, DeclNameRef member, Type memberType,
      DeclContext *useDC, FunctionRefKind functionRefKind,
      ArrayRef<OverloadChoice> outerAlternatives, TypeMatchOptions flags,
      ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given value witness constraint.
  SolutionKind simplifyValueWitnessConstraint(
      ConstraintKind kind, Type baseType, ValueDecl *member, Type memberType,
      DeclContext *useDC, FunctionRefKind functionRefKind,
      TypeMatchOptions flags, ConstraintLocatorBuilder locator);

  /// Attempt to simplify the optional object constraint.
  SolutionKind simplifyOptionalObjectConstraint(
                                          Type first, Type second,
                                          TypeMatchOptions flags,
                                          ConstraintLocatorBuilder locator);

  /// Attempt to simplify a function input or result constraint.
  SolutionKind simplifyFunctionComponentConstraint(
                                          ConstraintKind kind,
                                          Type first, Type second,
                                          TypeMatchOptions flags,
                                          ConstraintLocatorBuilder locator);

  /// Attempt to simplify an OpaqueUnderlyingType constraint.
  SolutionKind simplifyOpaqueUnderlyingTypeConstraint(Type type1,
                                              Type type2,
                                              TypeMatchOptions flags,
                                              ConstraintLocatorBuilder locator);
  
  /// Attempt to simplify the BridgingConversion constraint.
  SolutionKind simplifyBridgingConstraint(Type type1,
                                         Type type2,
                                         TypeMatchOptions flags,
                                         ConstraintLocatorBuilder locator);

  /// Attempt to simplify the ApplicableFunction constraint.
  SolutionKind simplifyApplicableFnConstraint(
      Type type1, Type type2,
      Optional<TrailingClosureMatching> trailingClosureMatching,
      TypeMatchOptions flags, ConstraintLocatorBuilder locator);

  /// Attempt to simplify the DynamicCallableApplicableFunction constraint.
  SolutionKind simplifyDynamicCallableApplicableFnConstraint(
                                      Type type1,
                                      Type type2,
                                      TypeMatchOptions flags,
                                      ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given DynamicTypeOf constraint.
  SolutionKind simplifyDynamicTypeOfConstraint(
                                         Type type1, Type type2,
                                         TypeMatchOptions flags,
                                         ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given EscapableFunctionOf constraint.
  SolutionKind simplifyEscapableFunctionOfConstraint(
                                         Type type1, Type type2,
                                         TypeMatchOptions flags,
                                         ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given OpenedExistentialOf constraint.
  SolutionKind simplifyOpenedExistentialOfConstraint(
                                         Type type1, Type type2,
                                         TypeMatchOptions flags,
                                         ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given KeyPathApplication constraint.
  SolutionKind simplifyKeyPathApplicationConstraint(
                                         Type keyPath,
                                         Type root,
                                         Type value,
                                         TypeMatchOptions flags,
                                         ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given KeyPath constraint.
  SolutionKind simplifyKeyPathConstraint(
      Type keyPath,
      Type root,
      Type value,
      ArrayRef<TypeVariableType *> componentTypeVars,
      TypeMatchOptions flags,
      ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given defaultable constraint.
  SolutionKind simplifyDefaultableConstraint(Type first, Type second,
                                             TypeMatchOptions flags,
                                             ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given defaultable closure type constraint.
  SolutionKind simplifyDefaultClosureTypeConstraint(
      Type closureType, Type inferredType,
      ArrayRef<TypeVariableType *> referencedOuterParameters,
      TypeMatchOptions flags, ConstraintLocatorBuilder locator);

  /// Attempt to simplify a property wrapper constraint.
  SolutionKind simplifyPropertyWrapperConstraint(Type wrapperType, Type wrappedValueType,
                                                 TypeMatchOptions flags,
                                                 ConstraintLocatorBuilder locator);

  /// Attempt to simplify a one-way constraint.
  SolutionKind simplifyOneWayConstraint(ConstraintKind kind,
                                        Type first, Type second,
                                        TypeMatchOptions flags,
                                        ConstraintLocatorBuilder locator);

  /// Simplify an equality constraint between result and base types of
  /// an unresolved member chain.
  SolutionKind simplifyUnresolvedMemberChainBaseConstraint(
      Type first, Type second, TypeMatchOptions flags,
      ConstraintLocatorBuilder locator);

  /// Simplify a conversion constraint by applying the given
  /// reduction rule, which is known to apply at the outermost level.
  SolutionKind simplifyRestrictedConstraintImpl(
                 ConversionRestrictionKind restriction,
                 Type type1, Type type2,
                 ConstraintKind matchKind,
                 TypeMatchOptions flags,
                 ConstraintLocatorBuilder locator);

  /// Simplify a conversion constraint by applying the given
  /// reduction rule, which is known to apply at the outermost level.
  SolutionKind simplifyRestrictedConstraint(
                 ConversionRestrictionKind restriction,
                 Type type1, Type type2,
                 ConstraintKind matchKind,
                 TypeMatchOptions flags,
                 ConstraintLocatorBuilder locator);

public: // FIXME: Public for use by static functions.
  /// Simplify a conversion constraint with a fix applied to it.
  SolutionKind simplifyFixConstraint(ConstraintFix *fix, Type type1, Type type2,
                                     ConstraintKind matchKind,
                                     TypeMatchOptions flags,
                                     ConstraintLocatorBuilder locator);

public:
  /// Simplify the system of constraints, by breaking down complex
  /// constraints into simpler constraints.
  ///
  /// The result of simplification is a constraint system consisting of
  /// only simple constraints relating type variables to each other or
  /// directly to fixed types. There are no constraints that involve
  /// type constructors on both sides. The simplified constraint system may,
  /// of course, include type variables for which we have constraints but
  /// no fixed type. Such type variables are left to the solver to bind.
  ///
  /// \returns true if an error occurred, false otherwise.
  bool simplify();

  /// Simplify the given constraint.
  SolutionKind simplifyConstraint(const Constraint &constraint);
  /// Simplify the given disjunction choice.
  void simplifyDisjunctionChoice(Constraint *choice);

  /// Apply the given result builder to the closure expression.
  ///
  /// \returns \c None when the result builder cannot be applied at all,
  /// otherwise the result of applying the result builder.
  Optional<TypeMatchResult> matchResultBuilder(
      AnyFunctionRef fn, Type builderType, Type bodyResultType,
      ConstraintKind bodyResultConstraintKind,
      ConstraintLocatorBuilder locator);

  /// Matches a wrapped or projected value parameter type to its backing
  /// property wrapper type by applying the property wrapper.
  TypeMatchResult applyPropertyWrapperToParameter(
      Type wrapperType, Type paramType, ParamDecl *param, Identifier argLabel,
      ConstraintKind matchKind, ConstraintLocatorBuilder locator);

  Optional<BindingSet> determineBestBindings();

  /// Get bindings for the given type variable based on current
  /// state of the constraint system.
  BindingSet getBindingsFor(TypeVariableType *typeVar, bool finalize = true);

private:
  /// Add a constraint to the constraint system.
  SolutionKind addConstraintImpl(ConstraintKind kind, Type first, Type second,
                                 ConstraintLocatorBuilder locator,
                                 bool isFavored);

  /// Adds a constraint for the conversion of an argument to a parameter. Do not
  /// call directly, use \c addConstraint instead.
  SolutionKind
  addArgumentConversionConstraintImpl(ConstraintKind kind, Type first,
                                      Type second,
                                      ConstraintLocatorBuilder locator);

  /// Collect the current inactive disjunction constraints.
  void collectDisjunctions(SmallVectorImpl<Constraint *> &disjunctions);

  /// Record a particular disjunction choice of
  void recordDisjunctionChoice(ConstraintLocator *disjunctionLocator,
                               unsigned index) {
    DisjunctionChoices.push_back({disjunctionLocator, index});
  }

  /// Filter the set of disjunction terms, keeping only those where the
  /// predicate returns \c true.
  ///
  /// The terms of the disjunction that are filtered out will be marked as
  /// "disabled" so they won't be visited later. If only one term remains
  /// enabled, the disjunction itself will be returned and that term will
  /// be made active.
  ///
  /// \param restoreOnFail If true, then all of the disabled terms will
  /// be re-enabled when this function returns \c Error.
  ///
  /// \returns One of \c Solved (only a single term remained),
  /// \c Unsolved (more than one disjunction terms remain), or
  /// \c Error (all terms were filtered out).
  SolutionKind filterDisjunction(Constraint *disjunction,
                                  bool restoreOnFail,
                                  llvm::function_ref<bool(Constraint *)> pred);

  bool isReadOnlyKeyPathComponent(const AbstractStorageDecl *storage,
                                  SourceLoc referenceLoc);

public:
  // Given a type variable, attempt to find the disjunction of
  // bind overloads associated with it. This may return null in cases where
  // the disjunction has either not been created or binds the type variable
  // in some manner other than by binding overloads.
  ///
  /// \param numOptionalUnwraps If non-null, this will receive the number
  /// of "optional object of" constraints that this function looked through
  /// to uncover the disjunction. The actual overloads will have this number
  /// of optionals wrapping the type.
  Constraint *getUnboundBindOverloadDisjunction(
    TypeVariableType *tyvar,
    unsigned *numOptionalUnwraps = nullptr);

private:
  /// Solve the system of constraints after it has already been
  /// simplified.
  ///
  /// \param solutions The set of solutions to this system of constraints.
  ///
  /// \returns true if an error occurred, false otherwise.
  bool solveSimplified(SmallVectorImpl<Solution> &solutions);

  /// Find reduced domains of disjunction constraints for given
  /// expression, this is achieved to solving individual sub-expressions
  /// and combining resolving types. Such algorithm is called directional
  /// path consistency because it goes from children to parents for all
  /// related sub-expressions taking union of their domains.
  ///
  /// \param expr The expression to find reductions for.
  void shrink(Expr *expr);

  /// Pick a disjunction from the InactiveConstraints list.
  ///
  /// \returns The selected disjunction.
  Constraint *selectDisjunction();

  /// Solve the system of constraints generated from provided expression.
  ///
  /// \param target The target to generate constraints from.
  /// \param allowFreeTypeVariables How to bind free type variables in
  /// the solution.
  SolutionResult solveImpl(SolutionApplicationTarget &target,
                           FreeTypeVariableBinding allowFreeTypeVariables
                             = FreeTypeVariableBinding::Disallow);

public:
  /// Pre-check the expression, validating any types that occur in the
  /// expression and folding sequence expressions.
  ///
  /// \param replaceInvalidRefsWithErrors Indicates whether it's allowed
  /// to replace any discovered invalid member references with `ErrorExpr`.
  static bool preCheckExpression(Expr *&expr, DeclContext *dc,
                                 bool replaceInvalidRefsWithErrors);

  /// Solve the system of constraints generated from provided target.
  ///
  /// \param target The target that we'll generate constraints from, which
  /// may be updated by the solving process.
  /// \param allowFreeTypeVariables How to bind free type variables in
  /// the solution.
  ///
  /// \returns the set of solutions, if any were found, or \c None if an
  /// error occurred. When \c None, an error has been emitted.
  Optional<std::vector<Solution>> solve(
      SolutionApplicationTarget &target,
      FreeTypeVariableBinding allowFreeTypeVariables
        = FreeTypeVariableBinding::Disallow);

  /// Solve the system of constraints.
  ///
  /// \param solutions The set of solutions to this system of constraints.
  ///
  /// \param allowFreeTypeVariables How to bind free type variables in
  /// the solution.
  ///
  /// \returns true if an error occurred, false otherwise.  Note that multiple
  /// ambiguous solutions for the same constraint system are considered to be
  /// success by this API.
  bool solve(SmallVectorImpl<Solution> &solutions,
             FreeTypeVariableBinding allowFreeTypeVariables =
                 FreeTypeVariableBinding::Disallow);

  /// Solve the system of constraints.
  ///
  /// \param allowFreeTypeVariables How to bind free type variables in
  /// the solution.
  ///
  /// \param allowFixes Whether to allow fixes in the solution.
  ///
  /// \returns a solution if a single unambiguous one could be found, or None if
  /// ambiguous or unsolvable.
  Optional<Solution> solveSingle(FreeTypeVariableBinding allowFreeTypeVariables
                                 = FreeTypeVariableBinding::Disallow,
                                 bool allowFixes = false);

  /// Construct and solve a system of constraints based on the given expression
  /// and its contextual information.
  ///
  /// This method is designed to be used for code completion which means that
  /// it doesn't mutate given expression, even if there is a single valid
  /// solution, and constraint solver is allowed to produce partially correct
  /// solutions. Such solutions can have any number of holes in them.
  ///
  /// \param target The expression involved in code completion.
  ///
  /// \param solutions The solutions produced for the given target without
  /// filtering.
  ///
  /// \returns `false` if this call fails (e.g. pre-check or constraint
  /// generation fails), `true` otherwise.
  bool solveForCodeCompletion(SolutionApplicationTarget &target,
                              SmallVectorImpl<Solution> &solutions);

private:
  /// Solve the system of constraints.
  ///
  /// This method responsible for running search/solver algorithm.
  /// It doesn't filter solutions, that's the job of top-level `solve` methods.
  ///
  /// \param solutions The set of solutions to this system of constraints.
  void solveImpl(SmallVectorImpl<Solution> &solutions);

  /// Compare two solutions to the same set of constraints.
  ///
  /// \param cs The constraint system.
  /// \param solutions All of the solutions to the system.
  /// \param diff The differences among the solutions.
  /// \param idx1 The index of the first solution.
  /// \param idx2 The index of the second solution.
  static SolutionCompareResult
  compareSolutions(ConstraintSystem &cs, ArrayRef<Solution> solutions,
                   const SolutionDiff &diff, unsigned idx1, unsigned idx2);

public:
  /// Increase the score of the given kind for the current (partial) solution
  /// along the.
  void increaseScore(ScoreKind kind, unsigned value = 1);

  /// Determine whether this solution is guaranteed to be worse than the best
  /// solution found so far.
  bool worseThanBestSolution() const;

  /// Given a set of viable solutions, find the best
  /// solution.
  ///
  /// \param solutions The set of viable solutions to consider.
  ///
  /// \param minimize If true, then in the case where there is no single
  /// best solution, minimize the set of solutions by removing any solutions
  /// that are identical to or worse than some other solution. This operation
  /// is quadratic.
  ///
  /// \returns The index of the best solution, or nothing if there was no
  /// best solution.
  Optional<unsigned>
  findBestSolution(SmallVectorImpl<Solution> &solutions,
                   bool minimize);

public:
  /// Apply a given solution to the target, producing a fully
  /// type-checked target or \c None if an error occurred.
  ///
  /// \param target the target to which the solution will be applied.
  Optional<SolutionApplicationTarget> applySolution(
      Solution &solution, SolutionApplicationTarget target);

  /// Apply the given solution to the given statement-condition.
  Optional<StmtCondition> applySolution(
      Solution &solution, StmtCondition condition, DeclContext *dc);

  /// Apply the given solution to the given function's body and, for
  /// closure expressions, the expression itself.
  ///
  /// \param solution The solution to apply.
  /// \param fn The function to which the solution is being applied.
  /// \param currentDC The declaration context in which transformations
  /// will be applied.
  /// \param rewriteTarget Function that performs a rewrite of any
  /// solution application target within the context.
  ///
  SolutionApplicationToFunctionResult applySolution(
      Solution &solution, AnyFunctionRef fn,
      DeclContext *&currentDC,
      std::function<
        Optional<SolutionApplicationTarget> (SolutionApplicationTarget)>
          rewriteTarget);

  /// Reorder the disjunctive clauses for a given expression to
  /// increase the likelihood that a favored constraint will be successfully
  /// resolved before any others.
  void optimizeConstraints(Expr *e);
  
  /// Determine if we've already explored too many paths in an
  /// attempt to solve this expression.
  bool isExpressionAlreadyTooComplex = false;
  bool getExpressionTooComplex(size_t solutionMemory) {
    if (isExpressionAlreadyTooComplex)
      return true;

    auto used = getASTContext().getSolverMemory() + solutionMemory;
    MaxMemory = std::max(used, MaxMemory);
    auto threshold = getASTContext().TypeCheckerOpts.SolverMemoryThreshold;
    if (MaxMemory > threshold) {
      return isExpressionAlreadyTooComplex= true;
    }

    const auto timeoutThresholdInMillis =
        getASTContext().TypeCheckerOpts.ExpressionTimeoutThreshold;
    if (Timer && Timer->isExpired(timeoutThresholdInMillis)) {
      // Disable warnings about expressions that go over the warning
      // threshold since we're arbitrarily ending evaluation and
      // emitting an error.
      Timer->disableWarning();

      return isExpressionAlreadyTooComplex = true;
    }

    // Bail out once we've looked at a really large number of
    // choices.
    if (CountScopes > getASTContext().TypeCheckerOpts.SolverBindingThreshold) {
      return isExpressionAlreadyTooComplex = true;
    }

    return false;
  }

  bool getExpressionTooComplex(SmallVectorImpl<Solution> const &solutions) {
    size_t solutionMemory = 0;
    for (auto const& s : solutions) {
      solutionMemory += s.getTotalMemory();
    }
    return getExpressionTooComplex(solutionMemory);
  }

  // If the given constraint is an applied disjunction, get the argument function
  // that the disjunction is applied to.
  const FunctionType *getAppliedDisjunctionArgumentFunction(const Constraint *disjunction) {
    assert(disjunction->getKind() == ConstraintKind::Disjunction);
    return AppliedDisjunctions[disjunction->getLocator()];
  }

  /// The overload sets that have already been resolved along the current path.
  const llvm::MapVector<ConstraintLocator *, SelectedOverload> &
  getResolvedOverloads() const {
    return ResolvedOverloads;
  }

  /// If we aren't certain that we've emitted a diagnostic, emit a fallback
  /// diagnostic.
  void maybeProduceFallbackDiagnostic(SolutionApplicationTarget target) const;

  /// Check whether given AST node represents an argument of an application
  /// of some sort (call, operator invocation, subscript etc.)
  /// and return AST node representing and argument index. E.g. for regular
  /// calls `test(42)` passing `42` should return node representing
  /// entire call and index `0`.
  Optional<std::pair<Expr *, unsigned>> isArgumentExpr(Expr *expr);

  SWIFT_DEBUG_DUMP;
  SWIFT_DEBUG_DUMPER(dump(Expr *));

  void print(raw_ostream &out) const;
  void print(raw_ostream &out, Expr *) const;
};

/// A function object suitable for use as an \c OpenUnboundGenericTypeFn that
/// "opens" the given unbound type by introducing fresh type variables for
/// generic parameters and constructing a bound generic type from these
/// type variables.
class OpenUnboundGenericType {
  ConstraintSystem &cs;
  const ConstraintLocatorBuilder &locator;

public:
  explicit OpenUnboundGenericType(ConstraintSystem &cs,
                                  const ConstraintLocatorBuilder &locator)
      : cs(cs), locator(locator) {}

  Type operator()(UnboundGenericType *unboundTy) const {
    return cs.openUnboundGenericType(unboundTy->getDecl(),
                                     unboundTy->getParent(), locator);
  }
};

class HandlePlaceholderType {
  ConstraintSystem &cs;
  ConstraintLocator *locator;

public:
  explicit HandlePlaceholderType(ConstraintSystem &cs,
                                 const ConstraintLocatorBuilder &locator)
      : cs(cs) {
    this->locator = cs.getConstraintLocator(locator);
  }

  Type operator()(PlaceholderTypeRepr *placeholderRepr) const {
    return cs.createTypeVariable(
        cs.getConstraintLocator(
            locator, LocatorPathElt::PlaceholderType(placeholderRepr)),
        TVO_CanBindToNoEscape | TVO_PrefersSubtypeBinding |
            TVO_CanBindToHole);
  }
};

/// Compute the shuffle required to map from a given tuple type to
/// another tuple type.
///
/// \param fromTuple The tuple type we're converting from, as represented by its
/// TupleTypeElt members.
///
/// \param toTuple The tuple type we're converting to, as represented by its
/// TupleTypeElt members.
///
/// \param sources Will be populated with information about the source of each
/// of the elements for the result tuple. The indices into this array are the
/// indices of the tuple type we're converting to, while the values are
/// an index into the source tuple.
///
/// \returns true if no tuple conversion is possible, false otherwise.
bool computeTupleShuffle(ArrayRef<TupleTypeElt> fromTuple,
                         ArrayRef<TupleTypeElt> toTuple,
                         SmallVectorImpl<unsigned> &sources);
static inline bool computeTupleShuffle(TupleType *fromTuple,
                                       TupleType *toTuple,
                                       SmallVectorImpl<unsigned> &sources){
  return computeTupleShuffle(fromTuple->getElements(), toTuple->getElements(),
                             sources);
}

/// Class used as the base for listeners to the \c matchCallArguments process.
///
/// By default, none of the callbacks do anything.
class MatchCallArgumentListener {
public:
  virtual ~MatchCallArgumentListener();

  /// Indicates that the argument at the given index does not match any
  /// parameter.
  ///
  /// \param argIdx The index of the extra argument.
  ///
  /// \returns true to indicate that this should cause a failure, false
  /// otherwise.
  virtual bool extraArgument(unsigned argIdx);

  /// Indicates that no argument was provided for the parameter at the given
  /// indices.
  ///
  /// \param paramIdx The index of the parameter that is missing an argument.
  /// \param argInsertIdx The index in the argument list where this argument was
  /// expected.
  virtual Optional<unsigned> missingArgument(unsigned paramIdx,
                                             unsigned argInsertIdx);

  /// Indicate that there was no label given when one was expected by parameter.
  ///
  /// \param paramIndex The index of the parameter that is missing a label.
  ///
  /// \returns true to indicate that this should cause a failure, false
  /// otherwise.
  virtual bool missingLabel(unsigned paramIndex);

  /// Indicate that there was label given when none was expected by parameter.
  ///
  /// \param paramIndex The index of the parameter that wasn't expecting a label.
  ///
  /// \returns true to indicate that this should cause a failure, false
  /// otherwise.
  virtual bool extraneousLabel(unsigned paramIndex);

  /// Indicate that there was a label given with a typo(s) in it.
  ///
  /// \param paramIndex The index of the parameter with misspelled label.
  ///
  /// \returns true to indicate that this should cause a failure, false
  /// otherwise.
  virtual bool incorrectLabel(unsigned paramIndex);

  /// Indicates that an argument is out-of-order with respect to a previously-
  /// seen argument.
  ///
  /// \param argIdx The argument that came too late in the argument list.
  /// \param prevArgIdx The argument that the \c argIdx should have preceded.
  ///
  /// \returns true to indicate that this should cause a failure, false
  /// otherwise.
  virtual bool outOfOrderArgument(
      unsigned argIdx, unsigned prevArgIdx, ArrayRef<ParamBinding> bindings);

  /// Indicates that the arguments need to be relabeled to match the parameters.
  ///
  /// \returns true to indicate that this should cause a failure, false
  /// otherwise.
  virtual bool relabelArguments(ArrayRef<Identifier> newNames);
};

/// Match the call arguments (as described by the given argument type) to
/// the parameters (as described by the given parameter type).
///
/// \param args The arguments.
/// \param params The parameters.
/// \param paramInfo Declaration-level information about the parameters.
/// \param unlabeledTrailingClosureIndex The index of an unlabeled trailing closure,
///   if any.
/// \param allowFixes Whether to allow fixes when matching arguments.
///
/// \param listener Listener that will be notified when certain problems occur,
/// e.g., to produce a diagnostic.
///
/// \param trailingClosureMatching If specified, the trailing closure matching
/// direction to use. Otherwise, the matching direction will be determined
/// based on language mode.
///
/// \returns the bindings produced by performing this matching, or \c None if
/// the match failed.
Optional<MatchCallArgumentResult>
matchCallArguments(
    SmallVectorImpl<AnyFunctionType::Param> &args,
    ArrayRef<AnyFunctionType::Param> params,
    const ParameterListInfo &paramInfo,
    Optional<unsigned> unlabeledTrailingClosureIndex,
    bool allowFixes,
    MatchCallArgumentListener &listener,
    Optional<TrailingClosureMatching> trailingClosureMatching);

ConstraintSystem::TypeMatchResult
matchCallArguments(ConstraintSystem &cs,
                   FunctionType *contextualType,
                   ArrayRef<AnyFunctionType::Param> args,
                   ArrayRef<AnyFunctionType::Param> params,
                   ConstraintKind subKind,
                   ConstraintLocatorBuilder locator,
                   Optional<TrailingClosureMatching> trailingClosureMatching);

/// Given an expression that is the target of argument labels (for a call,
/// subscript, etc.), find the underlying target expression.
Expr *getArgumentLabelTargetExpr(Expr *fn);

/// Returns true if a reference to a member on a given base type will apply
/// its curried self parameter, assuming it has one.
///
/// This is true for most member references, however isn't true for things
/// like an instance member being referenced on a metatype, where the
/// curried self parameter remains unapplied.
bool doesMemberRefApplyCurriedSelf(Type baseTy, const ValueDecl *decl);

/// Simplify the given locator by zeroing in on the most specific
/// subexpression described by the locator.
///
/// This routine can also find the corresponding "target" locator, which
/// typically provides the other end of a relational constraint. For example,
/// if the primary locator refers to a function argument, the target locator
/// will be set to refer to the corresponding function parameter.
///
/// \param cs The constraint system in which the locator will be simplified.
///
/// \param locator The locator to simplify.
///
/// \param range Will be populated with an "interesting" range.
///
/// \return the simplified locator.
ConstraintLocator *simplifyLocator(ConstraintSystem &cs,
                                   ConstraintLocator *locator,
                                   SourceRange &range);

void simplifyLocator(ASTNode &anchor, ArrayRef<LocatorPathElt> &path,
                     SourceRange &range);

/// Simplify the given locator down to a specific anchor expression,
/// if possible.
///
/// \returns the anchor expression if it fully describes the locator, or
/// null otherwise.
ASTNode simplifyLocatorToAnchor(ConstraintLocator *locator);

/// Retrieve argument at specified index from given node.
/// The expression could be "application", "subscript" or "member" call.
///
/// \returns argument expression or `nullptr` if given "base" expression
/// wasn't of one of the kinds listed above.
Expr *getArgumentExpr(ASTNode node, unsigned index);

/// Determine whether given locator points to one of the arguments
/// associated with the call to an operator. If the operator name
/// is empty `true` is returned for any kind of operator.
bool isOperatorArgument(ConstraintLocator *locator,
                        StringRef expectedOperator = "");

/// Determine whether given locator points to one of the arguments
/// associated with implicit `~=` (pattern-matching) operator
bool isArgumentOfPatternMatchingOperator(ConstraintLocator *locator);

/// Determine whether given locator points to one of the arguments
/// associated with `===` and `!==` operators.
bool isArgumentOfReferenceEqualityOperator(ConstraintLocator *locator);

/// Determine whether the given AST node is a reference to a
/// pattern-matching operator `~=`
bool isPatternMatchingOperator(ASTNode node);

/// Determine whether the given AST node is a reference to a
/// "standard" comparison operator such as "==", "!=", ">" etc.
bool isStandardComparisonOperator(ASTNode node);

/// If given expression references operator overlaod(s)
/// extract and produce name of the operator.
Optional<Identifier> getOperatorName(Expr *expr);

// Check whether argument of the call at given position refers to
// parameter marked as `@autoclosure`. This function is used to
// maintain source compatibility with Swift versions < 5,
// previously examples like following used to type-check:
//
// func foo(_ x: @autoclosure () -> Int) {}
// func bar(_ y: @autoclosure () -> Int) {
//   foo(y)
// }
bool isAutoClosureArgument(Expr *argExpr);

/// Checks whether referencing the given overload choice results in the self
/// parameter being applied, meaning that it's dropped from the type of the
/// reference.
bool hasAppliedSelf(ConstraintSystem &cs, const OverloadChoice &choice);
bool hasAppliedSelf(const OverloadChoice &choice,
                    llvm::function_ref<Type(Type)> getFixedType);

/// Check whether type conforms to a given known protocol.
bool conformsToKnownProtocol(DeclContext *dc, Type type,
                             KnownProtocolKind protocol);

/// Check whether given type conforms to `RawPepresentable` protocol
/// and return witness type.
Type isRawRepresentable(ConstraintSystem &cs, Type type);
/// Check whether given type conforms to a specific known kind
/// `RawPepresentable` protocol and return witness type.
Type isRawRepresentable(ConstraintSystem &cs, Type type,
                        KnownProtocolKind rawRepresentableProtocol);

/// Compute the type that shall stand in for dynamic 'Self' in a member
/// reference with a base of the given object type.
///
/// \param memberLocator The locator of the member constraint; used to retrieve
/// the expression that the locator is anchored to.
Type getDynamicSelfReplacementType(Type baseObjTy, const ValueDecl *member,
                                   ConstraintLocator *memberLocator);

ValueDecl *getOverloadChoiceDecl(Constraint *choice);

class DisjunctionChoice {
  ConstraintSystem &CS;
  unsigned Index;
  Constraint *Choice;
  bool ExplicitConversion;
  bool IsBeginningOfPartition;

public:
  DisjunctionChoice(ConstraintSystem &cs, unsigned index, Constraint *choice,
                    bool explicitConversion, bool isBeginningOfPartition)
      : CS(cs), Index(index), Choice(choice),
        ExplicitConversion(explicitConversion),
        IsBeginningOfPartition(isBeginningOfPartition) {}

  unsigned getIndex() const { return Index; }

  bool attempt(ConstraintSystem &cs) const;

  bool isDisabled() const {
    if (!Choice->isDisabled())
      return false;

    // If solver is in a diagnostic mode, let's allow
    // constraints that have fixes or have been disabled
    // in attempt to produce a solution faster for
    // well-formed expressions.
    if (CS.shouldAttemptFixes()) {
      return !(hasFix() || Choice->isDisabledInPerformanceMode());
    }

    return true;
  }

  bool hasFix() const {
    return bool(Choice->getFix());
  }

  bool isUnavailable() const {
    if (auto *decl = getOverloadChoiceDecl(Choice))
      return CS.isDeclUnavailable(decl, Choice->getLocator());
    return false;
  }

  bool isBeginningOfPartition() const { return IsBeginningOfPartition; }

  // FIXME: All three of the accessors below are required to support
  //        performance optimization hacks in constraint solver.

  bool isGenericOperator() const;
  bool isSymmetricOperator() const;
  bool isUnaryOperator() const;

  void print(llvm::raw_ostream &Out, SourceManager *SM) const {
    Out << "disjunction choice ";
    Choice->print(Out, SM);
  }

  operator Constraint *() { return Choice; }
  operator Constraint *() const { return Choice; }

private:
  /// If associated disjunction is an explicit conversion,
  /// let's try to propagate its type early to prune search space.
  void propagateConversionInfo(ConstraintSystem &cs) const;

  static ValueDecl *getOperatorDecl(Constraint *choice) {
    auto *decl = getOverloadChoiceDecl(choice);
    if (!decl)
      return nullptr;

    return decl->isOperator() ? decl : nullptr;
  }
};

class TypeVariableBinding {
  TypeVariableType *TypeVar;
  PotentialBinding Binding;

public:
  TypeVariableBinding(TypeVariableType *typeVar, PotentialBinding &binding)
      : TypeVar(typeVar), Binding(binding) {}

  bool isDefaultable() const { return Binding.isDefaultableBinding(); }

  bool hasDefaultedProtocol() const {
    return Binding.hasDefaultedLiteralProtocol();
  }

  bool attempt(ConstraintSystem &cs) const;

  /// Determine what fix (if any) needs to be introduced into a
  /// constraint system as part of resolving type variable as a hole.
  Optional<std::pair<ConstraintFix *, unsigned>>
  fixForHole(ConstraintSystem &cs) const;

  void print(llvm::raw_ostream &Out, SourceManager *) const {
    PrintOptions PO;
    PO.PrintTypesForDebugging = true;
    Out << "type variable " << TypeVar->getString(PO)
        << " := " << Binding.BindingType->getString(PO);
  }
};

template<typename Choice>
class BindingProducer {
  ConstraintLocator *Locator;

protected:
  ConstraintSystem &CS;

public:
  BindingProducer(ConstraintSystem &cs, ConstraintLocator *locator)
      : Locator(locator), CS(cs) {}

  virtual ~BindingProducer() {}
  virtual Optional<Choice> operator()() = 0;

  ConstraintLocator *getLocator() const { return Locator; }

  /// Check whether generator would have to compute next
  /// batch of bindings because it freshly ran out of current one.
  /// This is useful to be able to exhaustively attempt bindings
  /// for type variables found at one level, before proceeding to
  /// supertypes or literal defaults etc.
  virtual bool needsToComputeNext() const { return false; }
};

class TypeVarBindingProducer : public BindingProducer<TypeVariableBinding> {
  using BindingKind = AllowedBindingKind;
  using Binding = PotentialBinding;

  TypeVariableType *TypeVar;
  llvm::SmallVector<Binding, 8> Bindings;
  /// The set of defaults to attempt once producer
  /// runs out of direct & transitive bindings.
  llvm::SmallVector<Constraint *, 4> DelayedDefaults;

  // The index pointing to the offset in the bindings
  // generator is currently at, `numTries` represents
  // the number of times bindings have been recomputed.
  unsigned Index = 0, NumTries = 0;

  llvm::SmallPtrSet<CanType, 4> ExploredTypes;
  llvm::SmallPtrSet<TypeBase *, 4> BoundTypes;

  /// Determines whether this type variable has a
  /// `ExpressibleByNilLiteral` requirement which
  /// means that bindings have to either conform
  /// to that protocol or be wrapped in an optional.
  bool CanBeNil;

public:
  using Element = TypeVariableBinding;

  TypeVarBindingProducer(BindingSet &bindings);

  /// Retrieve a set of bindings available in the current state.
  ArrayRef<Binding> getCurrentBindings() const { return Bindings; }

  Optional<Element> operator()() override {
    // Once we reach the end of the current bindings
    // let's try to compute new ones, e.g. supertypes,
    // literal defaults, if that fails, we are done.
    if (needsToComputeNext() && !computeNext())
      return None;

    return TypeVariableBinding(TypeVar, Bindings[Index++]);
  }

  bool needsToComputeNext() const override { return Index >= Bindings.size(); }

private:
  /// Compute next batch of bindings if possible, this could
  /// be supertypes extracted from one of the current bindings
  /// or default literal types etc.
  ///
  /// \returns true if some new bindings were sucessfully computed,
  /// false otherwise.
  bool computeNext();

  /// Check whether binding type is required to either conform to
  /// `ExpressibleByNilLiteral` protocol or be wrapped into an optional type.
  bool requiresOptionalAdjustment(const Binding &binding) const;

  Binding getDefaultBinding(Constraint *constraint) const;
};

/// Iterator over disjunction choices, makes it
/// easy to work with disjunction and encapsulates
/// some other important information such as locator.
class DisjunctionChoiceProducer : public BindingProducer<DisjunctionChoice> {
  // The disjunction choices that this producer will iterate through.
  ArrayRef<Constraint *> Choices;

  // The ordering of disjunction choices. We index into Choices
  // through this vector in order to visit the disjunction choices in
  // the order we want to visit them.
  SmallVector<unsigned, 8> Ordering;

  // The index of the first element in a partition of the disjunction
  // choices. The choices are split into partitions where we will
  // visit all elements within a single partition before moving to the
  // elements of the next partition. If we visit all choices within a
  // single partition and have found a successful solution with one of
  // the choices in that partition, we stop looking for other
  // solutions.
  SmallVector<unsigned, 4> PartitionBeginning;

  // The index in the current partition of disjunction choices that we
  // are iterating over.
  unsigned PartitionIndex = 0;

  bool IsExplicitConversion;

  Constraint *Disjunction;

  unsigned Index = 0;

  bool needsGenericOperatorOrdering = true;

public:
  using Element = DisjunctionChoice;

  DisjunctionChoiceProducer(ConstraintSystem &cs, Constraint *disjunction)
      : BindingProducer(cs, disjunction->shouldRememberChoice()
                                ? disjunction->getLocator()
                                : nullptr),
        Choices(disjunction->getNestedConstraints()),
        IsExplicitConversion(disjunction->isExplicitConversion()),
        Disjunction(disjunction) {
    assert(disjunction->getKind() == ConstraintKind::Disjunction);
    assert(!disjunction->shouldRememberChoice() || disjunction->getLocator());

    // Order and partition the disjunction choices.
    partitionDisjunction(Ordering, PartitionBeginning);
  }

  void setNeedsGenericOperatorOrdering(bool flag) {
    needsGenericOperatorOrdering = flag;
  }

  Optional<Element> operator()() override {
    unsigned currIndex = Index;
    if (currIndex >= Choices.size())
      return None;

    bool isBeginningOfPartition = PartitionIndex < PartitionBeginning.size() &&
                                  PartitionBeginning[PartitionIndex] == Index;
    if (isBeginningOfPartition)
      ++PartitionIndex;

    ++Index;

    auto choice = DisjunctionChoice(CS, currIndex, Choices[Ordering[currIndex]],
                                    IsExplicitConversion, isBeginningOfPartition);
    // Partition the generic operators before producing the first generic
    // operator disjunction choice.
    if (needsGenericOperatorOrdering && choice.isGenericOperator()) {
      unsigned nextPartitionIndex = (PartitionIndex < PartitionBeginning.size() ?
                                     PartitionBeginning[PartitionIndex] : Ordering.size());
      partitionGenericOperators(Ordering.begin() + currIndex,
                                Ordering.begin() + nextPartitionIndex);
      needsGenericOperatorOrdering = false;
    }

    return DisjunctionChoice(CS, currIndex, Choices[Ordering[currIndex]],
                             IsExplicitConversion, isBeginningOfPartition);
  }

private:
  // Partition the choices in the disjunction into groups that we will
  // iterate over in an order appropriate to attempt to stop before we
  // have to visit all of the options.
  void partitionDisjunction(SmallVectorImpl<unsigned> &Ordering,
                            SmallVectorImpl<unsigned> &PartitionBeginning);

  /// Partition the choices in the range \c first to \c last into groups and
  /// order the groups in the best order to attempt based on the argument
  /// function type that the operator is applied to.
  void partitionGenericOperators(SmallVectorImpl<unsigned>::iterator first,
                                 SmallVectorImpl<unsigned>::iterator last);
};

/// Determine whether given type is a known one
/// for a key path `{Writable, ReferenceWritable}KeyPath`.
bool isKnownKeyPathType(Type type);

/// Determine whether given declaration is one for a key path
/// `{Writable, ReferenceWritable}KeyPath`.
bool isKnownKeyPathDecl(ASTContext &ctx, ValueDecl *decl);

/// Determine whether givne closure has any explicit `return`
/// statements that could produce non-void result.
bool hasExplicitResult(ClosureExpr *closure);

/// Emit diagnostics for syntactic restrictions within a given solution
/// application target.
void performSyntacticDiagnosticsForTarget(
    const SolutionApplicationTarget &target, bool isExprStmt);

/// Given a member of a protocol, check whether `Self` type of that
/// protocol is contextually bound to some concrete type via same-type
/// generic requirement and if so return that type or null type otherwise.
Type getConcreteReplacementForProtocolSelfType(ValueDecl *member);

/// Determine whether given disjunction constraint represents a set
/// of operator overload choices.
bool isOperatorDisjunction(Constraint *disjunction);

/// Find out whether closure body has any `async` or `await` expressions,
/// declarations, or statements directly in its body (no in other closures
/// or nested declarations).
ASTNode findAsyncNode(ClosureExpr *closure);

} // end namespace constraints

template<typename ...Args>
TypeVariableType *TypeVariableType::getNew(const ASTContext &C, unsigned ID,
                                           Args &&...args) {
  // Allocate memory
  void *mem = C.Allocate(sizeof(TypeVariableType) + sizeof(Implementation),
                         alignof(TypeVariableType),
                         AllocationArena::ConstraintSolver);

  // Construct the type variable.
  auto *result = ::new (mem) TypeVariableType(C, ID);

  // Construct the implementation object.
  new (result+1) TypeVariableType::Implementation(std::forward<Args>(args)...);

  return result;
}

/// If the expression has the effect of a forced downcast, find the
/// underlying forced downcast expression.
ForcedCheckedCastExpr *findForcedDowncast(ASTContext &ctx, Expr *expr);

// Count the number of overload sets present
// in the expression and all of the children.
class OverloadSetCounter : public ASTWalker {
  unsigned &NumOverloads;

public:
  OverloadSetCounter(unsigned &overloads)
  : NumOverloads(overloads)
  {}

  std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {
    if (auto applyExpr = dyn_cast<ApplyExpr>(expr)) {
      // If we've found function application and it's
      // function is an overload set, count it.
      if (isa<OverloadSetRefExpr>(applyExpr->getFn()))
        ++NumOverloads;
    }

    // Always recur into the children.
    return { true, expr };
  }
};

/// Matches array of function parameters to candidate inputs,
/// which can be anything suitable (e.g., parameters, arguments).
///
/// It claims inputs sequentially and tries to pair between an input
/// and the next appropriate parameter. The detailed matching behavior
/// of each pair is specified by a custom function (i.e., pairMatcher).
/// It considers variadic and defaulted arguments when forming proper
/// input-parameter pairs; however, other information like label and
/// type information is not directly used here. It can be implemented
/// in the custom function when necessary.
class InputMatcher {
  size_t NumSkippedParameters;
  const ParameterListInfo &ParamInfo;
  const ArrayRef<AnyFunctionType::Param> Params;

public:
  enum Result {
    /// The specified inputs are successfully matched.
    IM_Succeeded,
    /// There are input(s) left unclaimed while all parameters are matched.
    IM_HasUnclaimedInput,
    /// There are parateter(s) left unmatched while all inputs are claimed.
    IM_HasUnmatchedParam,
    /// Custom pair matcher function failure.
    IM_CustomPairMatcherFailed,
  };

  InputMatcher(const ArrayRef<AnyFunctionType::Param> params,
               const ParameterListInfo &paramInfo);

  /// Matching a given array of inputs.
  ///
  /// \param numInputs The number of inputs.
  /// \param pairMatcher Custom matching behavior of an input-parameter pair.
  /// \return the matching result.
  Result
  match(int numInputs,
        std::function<bool(unsigned inputIdx, unsigned paramIdx)> pairMatcher);

  size_t getNumSkippedParameters() const { return NumSkippedParameters; }
};

// Return true if, when replacing "<expr>" with "<expr> ?? T", parentheses need
// to be added around <expr> first in order to maintain the correct precedence.
bool exprNeedsParensBeforeAddingNilCoalescing(DeclContext *DC,
                                              Expr *expr);

// Return true if, when replacing "<expr>" with "<expr> as T", parentheses need
// to be added around the new expression in order to maintain the correct
// precedence.
bool exprNeedsParensAfterAddingNilCoalescing(DeclContext *DC,
                                             Expr *expr,
                                             Expr *rootExpr);

/// Return true if, when replacing "<expr>" with "<expr> op <something>",
/// parentheses must be added around "<expr>" to allow the new operator
/// to bind correctly.
bool exprNeedsParensInsideFollowingOperator(DeclContext *DC,
                                            Expr *expr,
                                            PrecedenceGroupDecl *followingPG);

/// Return true if, when replacing "<expr>" with "<expr> op <something>"
/// within the given root expression, parentheses must be added around
/// the new operator to prevent it from binding incorrectly in the
/// surrounding context.
bool exprNeedsParensOutsideFollowingOperator(
    DeclContext *DC, Expr *expr, Expr *rootExpr,
    PrecedenceGroupDecl *followingPG);

/// Determine whether this is a SIMD operator.
bool isSIMDOperator(ValueDecl *value);

std::string describeGenericType(ValueDecl *GP, bool includeName = false);

/// Apply the given result builder transform within a specific solution
/// to produce the rewritten body.
///
/// \param solution The solution to use during application, providing the
/// specific types for each type variable.
/// \param applied The applied builder transform.
/// \param body The body to transform
/// \param dc The context in which the transform occurs.
/// \param rewriteTarget Rewrites a solution application target to its final,
/// type-checked version.
///
/// \returns the transformed body
BraceStmt *applyResultBuilderTransform(
    const constraints::Solution &solution,
    constraints::AppliedBuilderTransform applied,
    BraceStmt *body,
    DeclContext *dc,
    std::function<
        Optional<constraints::SolutionApplicationTarget> (
          constraints::SolutionApplicationTarget)>
            rewriteTarget);

/// Determine whether the given closure expression should be type-checked
/// within the context of its enclosing expression. Otherwise, it will be
/// separately type-checked once its enclosing expression has determined all
/// of the parameter and result types without looking at the body.
bool shouldTypeCheckInEnclosingExpression(ClosureExpr *expr);

/// Visit each subexpression that will be part of the constraint system
/// of the given expression, including those in closure bodies that will be
/// part of the constraint system.
void forEachExprInConstraintSystem(
    Expr *expr, llvm::function_ref<Expr *(Expr *)> callback);

/// Whether the given parameter requires an argument.
bool parameterRequiresArgument(
    ArrayRef<AnyFunctionType::Param> params,
    const ParameterListInfo &paramInfo,
    unsigned paramIdx);

} // end namespace swift

namespace llvm {
template<>
struct DenseMapInfo<swift::constraints::SolutionApplicationTargetsKey> {
  using Key = swift::constraints::SolutionApplicationTargetsKey;

  static inline Key getEmptyKey() {
    return Key(Key::Kind::empty);
  }
  static inline Key getTombstoneKey() {
    return Key(Key::Kind::tombstone);
  }
  static inline unsigned getHashValue(Key key) {
    return key.getHashValue();
  }
  static bool isEqual(Key a, Key b) {
    return a == b;
  }
};
} // end namespace llvm

#endif // LLVM_SWIFT_SEMA_CONSTRAINT_SYSTEM_H
