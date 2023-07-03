//===--- ArgumentList.h - Function and subscript argument lists -*- C++ -*-===//
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
//
// This file defines the Argument and ArgumentList classes and support logic.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_ARGUMENTLIST_H
#define SWIFT_AST_ARGUMENTLIST_H

#include "swift/AST/ASTAllocated.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Debug.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/TrailingObjects.h"

namespace swift {
/// Forward declared trampoline for Expr::getType.
Type __Expr_getType(Expr *E);

/// Represents a single argument in an argument list, including its label
/// information and inout-ness.
///
/// \code
/// foo.bar(arg, label: arg2, other: &arg3)
///         ^^^               ^^^^^^^^^^^^
///         an unlabeled      'other' is the label, 'arg3' is the expr,
///         argument          and isInout() is true.
/// \endcode
class Argument final {
  SourceLoc LabelLoc;
  Identifier Label;
  Expr *ArgExpr;
  // TODO: Store inout bit here.

public:
  Argument(SourceLoc labelLoc, Identifier label, Expr *expr)
      : LabelLoc(labelLoc), Label(label), ArgExpr(expr) {}

  /// Make an unlabeled argument.
  static Argument unlabeled(Expr *expr) {
    return Argument(SourceLoc(), Identifier(), expr);
  }

  /// Make an implicit unlabeled 'inout' argument.
  static Argument implicitInOut(ASTContext &ctx, Expr *expr);

  SourceLoc getStartLoc() const { return getSourceRange().Start; }
  SourceLoc getEndLoc() const { return getSourceRange().End; }
  SourceRange getSourceRange() const;

  /// If the argument has a label with location information, returns the
  /// location.
  ///
  /// Note this may be present even if the label is empty, e.g for multiple
  /// trailing closures where labels are required such as '_: {}'.
  SourceLoc getLabelLoc() const { return LabelLoc; }

  /// The argument label written in the call.
  Identifier getLabel() const { return Label; }

  /// Whether the argument has a non-empty label. Note that this returns `false`
  /// for an explicitly specified empty label e.g `_: {}` for a trailing
  /// closure.
  bool hasLabel() const { return !Label.empty(); }

  /// Set a new argument label.
  ///
  /// Note this is marked \c & to prevent its use on an rvalue Argument vended
  /// from an ArgumentList.
  void setLabel(Identifier newLabel) & { Label = newLabel; }

  /// The argument expression.
  Expr *getExpr() const { return ArgExpr; }

  /// Set a new argument expression.
  ///
  /// Note this is marked \c & to prevent its use on an rvalue Argument vended
  /// from an ArgumentList.
  void setExpr(Expr *newExpr) & { ArgExpr = newExpr; }

  /// Whether the argument is \c inout, denoted with the '&' prefix.
  bool isInOut() const;

  /// Whether the argument is a compile-time constant value.
  bool isConst() const;

  bool operator==(const Argument &other) {
    return LabelLoc == other.LabelLoc && Label == other.Label &&
           ArgExpr == other.ArgExpr;
  }
  bool operator!=(const Argument &other) { return !(*this == other); }
};

/// Represents the argument list of a function call or subscript access.
/// \code
/// foo.bar(arg, label: arg2, other: &arg3)
///        ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
/// \endcode
///
/// Note that arguments are split up and stored in separate buffers to optimize
/// ArgumentList's memory footprint for the unlabeled cases.
class alignas(Argument) ArgumentList final
    : public ASTAllocated<ArgumentList>,
      private llvm::TrailingObjects<ArgumentList, Expr *, Identifier,
                                    SourceLoc, ArgumentList *> {
  friend TrailingObjects;

  SourceLoc LParenLoc;
  SourceLoc RParenLoc;

  /// The number of arguments in the list.
  unsigned NumArgs : 16;

  /// The index of the first trailing closure, or \c NumArgs if there are
  /// either no trailing closures, or the trailing closure info is present in
  /// the original args.
  unsigned RawFirstTrailingClosureIndex : 16;

  /// Whether an original set of arguments are available.
  bool HasOriginalArgs : 1;

  /// Whether the argument list was implicitly generated.
  bool IsImplicit : 1;

  /// Whether any of the arguments are labeled.
  bool HasLabels : 1;

  /// Whether any of the arguments have labels with location info.
  bool HasLabelLocs : 1;

  ArgumentList(SourceLoc lParenLoc, SourceLoc rParenLoc, unsigned numArgs,
               llvm::Optional<unsigned> firstTrailingClosureIndex,
               ArgumentList *originalArgs, bool isImplicit, bool hasLabels,
               bool hasLabelLocs)
      : LParenLoc(lParenLoc), RParenLoc(rParenLoc), NumArgs(numArgs),
        HasOriginalArgs(originalArgs), IsImplicit(isImplicit),
        HasLabels(hasLabels), HasLabelLocs(hasLabelLocs) {
    assert(LParenLoc.isValid() == RParenLoc.isValid());
    assert(!(firstTrailingClosureIndex && originalArgs) &&
           "Cannot have trailing closure info if original args present");
    assert(!(originalArgs && originalArgs->getStoredOriginalArgs()) &&
           "Cannot chain original argument lists");
    assert(NumArgs == numArgs && "Num args truncated");
    assert(!firstTrailingClosureIndex || *firstTrailingClosureIndex < numArgs &&
           "Invalid trailing closure index");
    RawFirstTrailingClosureIndex =
        firstTrailingClosureIndex.value_or(numArgs);
  }

  ArgumentList(const ArgumentList &) = delete;
  ArgumentList &operator=(const ArgumentList &) = delete;

  size_t numTrailingObjects(OverloadToken<Expr *>) const {
    return NumArgs;
  }
  size_t numTrailingObjects(OverloadToken<Identifier>) const {
    return HasLabels ? NumArgs : 0;
  }
  size_t numTrailingObjects(OverloadToken<SourceLoc>) const {
    return HasLabelLocs ? NumArgs : 0;
  }
  size_t numTrailingObjects(OverloadToken<ArgumentList *>) const {
    return HasOriginalArgs ? 1 : 0;
  }

  ArrayRef<Expr *> getExprsBuffer() const {
    return {getTrailingObjects<Expr *>(), NumArgs};
  }
  MutableArrayRef<Expr *> getExprsBuffer() {
    return {getTrailingObjects<Expr *>(), NumArgs};
  }

  ArrayRef<Identifier> getLabelsBuffer() const {
    assert(HasLabels);
    return {getTrailingObjects<Identifier>(), NumArgs};
  }
  MutableArrayRef<Identifier> getLabelsBuffer() {
    assert(HasLabels);
    return {getTrailingObjects<Identifier>(), NumArgs};
  }

  ArrayRef<SourceLoc> getLabelLocsBuffer() const {
    assert(HasLabelLocs);
    return {getTrailingObjects<SourceLoc>(), NumArgs};
  }
  MutableArrayRef<SourceLoc> getLabelLocsBuffer() {
    assert(HasLabelLocs);
    return {getTrailingObjects<SourceLoc>(), NumArgs};
  }

  ArgumentList *getStoredOriginalArgs() const {
    if (!HasOriginalArgs)
      return nullptr;
    return *getTrailingObjects<ArgumentList *>();
  }

public:
  /// Create a new ArgumentList.
  ///
  /// \param lParenLoc The location of the opening '('. Note that for a
  /// subscript argument list, this will be for the opening '['.
  /// \param args The list of arguments in the call.
  /// \param rParenLoc The location of the closing ')'. Note that for a
  /// subscript argument list, this will be for the closing ']'.
  /// \param firstTrailingClosureIndex The index of the first trailing closure.
  /// \param isImplicit Whether this is an implicitly generated argument list.
  /// \param originalArgs An original argument list prior to being rewritten by
  /// the expression type-checker. Note \p firstTrailingClosureIndex must be
  /// \c None if an original argument list is available.
  /// \param arena The arena to allocate the ArgumentList in.
  static ArgumentList *
  create(ASTContext &ctx, SourceLoc lParenLoc, ArrayRef<Argument> args,
         SourceLoc rParenLoc,
         llvm::Optional<unsigned> firstTrailingClosureIndex, bool isImplicit,
         ArgumentList *originalArgs = nullptr,
         AllocationArena arena = AllocationArena::Permanent);

  /// Create a new explicit parsed ArgumentList.
  ///
  /// \param lParenLoc The location of the opening '('. Note that for a
  /// subscript argument list, this will be for the opening '['.
  /// \param args The list of arguments in the call.
  /// \param rParenLoc The location of the closing ')'. Note that for a
  /// subscript argument list, this will be for the closing ']'.
  /// \param firstTrailingClosureIndex The index of the first trailing closure.
  static ArgumentList *
  createParsed(ASTContext &ctx, SourceLoc lParenLoc, ArrayRef<Argument> args,
               SourceLoc rParenLoc,
               llvm::Optional<unsigned> firstTrailingClosureIndex);

  /// Create a new type-checked ArgumentList from an original set of arguments.
  ///
  /// \param originalArgs An original argument list prior to being rewritten by
  /// the type-checker.
  /// \param newArgs The new set of arguments for the call.
  static ArgumentList *createTypeChecked(ASTContext &ctx,
                                         ArgumentList *originalArgs,
                                         ArrayRef<Argument> newArgs);

  /// Create a new implicit ArgumentList.
  ///
  /// \param lParenLoc The location of the opening '('. Note that for a
  /// subscript argument list, this will be for the opening '['.
  /// \param args The list of arguments in the call.
  /// \param rParenLoc The location of the closing ')'. Note that for a
  /// subscript argument list, this will be for the closing ']'.
  /// \param arena The arena to allocate the ArgumentList in.
  static ArgumentList *createImplicit(
      ASTContext &ctx, SourceLoc lParenLoc, ArrayRef<Argument> args,
      SourceLoc rParenLoc,
      llvm::Optional<unsigned> firstTrailingClosureIndex = llvm::None,
      AllocationArena arena = AllocationArena::Permanent);

  /// Create a new implicit ArgumentList with a set of \p args.
  static ArgumentList *createImplicit(
      ASTContext &ctx, ArrayRef<Argument> args,
      llvm::Optional<unsigned> firstTrailingClosureIndex = llvm::None,
      AllocationArena arena = AllocationArena::Permanent);

  /// Create a new implicit ArgumentList with a single labeled argument
  /// expression.
  static ArgumentList *forImplicitSingle(ASTContext &ctx, Identifier label,
                                         Expr *arg);

  /// Create a new implicit ArgumentList with a set of unlabeled arguments.
  static ArgumentList *forImplicitUnlabeled(ASTContext &ctx,
                                            ArrayRef<Expr *> argExprs);

  /// Create a new implicit ArgumentList with a set of argument expressions,
  /// and a DeclNameRef from which to infer the argument labels.
  static ArgumentList *forImplicitCallTo(DeclNameRef fnNameRef,
                                         ArrayRef<Expr *> argExprs,
                                         ASTContext &ctx);

  /// Create a new implicit ArgumentList with a set of argument expressions,
  /// and a ParameterList from which to infer the argument labels.
  static ArgumentList *forImplicitCallTo(ParameterList *params,
                                         ArrayRef<Expr *> argExprs,
                                         ASTContext &ctx);

  /// The location of the opening '('. Note that for a subscript argument list,
  /// this will be for the opening '['.
  SourceLoc getLParenLoc() const { return LParenLoc; }

  /// The location of the closing ')'. Note that for a subscript argument list,
  /// this will be for the closing ']'.
  SourceLoc getRParenLoc() const { return RParenLoc; }

  /// Whether this is an implicitly generated argument list, not written in the
  /// source.
  bool isImplicit() const { return IsImplicit; }

  /// Whether the argument list has any non-empty argument labels.
  bool hasAnyArgumentLabels() const { return HasLabels; }

  SourceLoc getLoc() const;
  SourceLoc getStartLoc() const { return getSourceRange().Start; }
  SourceLoc getEndLoc() const { return getSourceRange().End; }
  SourceRange getSourceRange() const;

  /// Retrieve the argument expression at a given index.
  Expr *getExpr(unsigned idx) const { return getExprsBuffer()[idx]; }
  void setExpr(unsigned idx, Expr *newExpr) { getExprsBuffer()[idx] = newExpr; }

  /// Retrieve the argument label at a given index.
  Identifier getLabel(unsigned idx) const {
    if (!HasLabels)
      return Identifier();
    return getLabelsBuffer()[idx];
  }

  /// Retrieve the argument label location at a given index.
  SourceLoc getLabelLoc(unsigned idx) const {
    if (!HasLabelLocs)
      return SourceLoc();
    return getLabelLocsBuffer()[idx];
  }

  /// Retrieve the argument at a given index.
  Argument get(unsigned idx) const {
    return Argument(getLabelLoc(idx), getLabel(idx), getExpr(idx));
  }

  // MARK: Iterator Logic

  unsigned size() const { return NumArgs; }
  bool empty() const { return size() == 0; }

  class iterator final
      : public llvm::indexed_accessor_iterator<iterator, const ArgumentList *,
                                               Argument, const Argument *,
                                               Argument> {
  public:
    iterator(const ArgumentList *argList, unsigned idx)
        : indexed_accessor_iterator(argList, idx) {}

    Argument operator*() const {
      assert(getBase());
      return getBase()->get(getIndex());
    }
    Argument operator[](unsigned i) const {
      assert(getBase());
      return getBase()->get(getIndex() + i);
    }
  };

  iterator begin() const { return {this, 0}; }
  iterator end() const { return {this, size()}; }

  Argument front() const { return get(0); }
  Argument back() const { return get(size() - 1); }

  Argument operator[](unsigned idx) const { return get(idx); }

  // MARK: Trailing Closure Queries
  //
  // Trailing closure queries cannot be done on a type-checked argument list,
  // instead they must be done on the original argument list prior to rewriting
  // in CSApply. This can be obtained by querying the \c getOriginalArgs method.
  //
  // This restriction is due to the fact that trailing closures in type-checked
  // argument lists can have an awkward representation. They may be:
  // - Nested within variadic expansion exprs, and may not be the first element
  //   of that expansion.
  // - In a position *before* default argument expressions, or even explicitly
  //   written non-trailing arguments in certain backward scanning argument
  //   matching cases.

  /// Returns the index of the first trailing closure in the argument list, or
  /// \c None if there are no trailing closures.
  ///
  /// Note for a type-checked argument list, this must be queried on
  /// \c getOriginalArgs instead.
  llvm::Optional<unsigned> getFirstTrailingClosureIndex() const {
    assert(!HasOriginalArgs && "Query original args instead");
    if (RawFirstTrailingClosureIndex == NumArgs)
      return llvm::None;
    return RawFirstTrailingClosureIndex;
  }

  /// The number of trailing closures in the argument list.
  ///
  /// Note for a type-checked argument list, this must be queried on
  /// \c getOriginalArgs instead.
  unsigned getNumTrailingClosures() const {
    assert(!HasOriginalArgs && "Query original args instead");
    if (!getFirstTrailingClosureIndex())
      return 0;
    return size() - *getFirstTrailingClosureIndex();
  }

  /// Whether any unlabeled or labeled trailing closures are present.
  bool hasAnyTrailingClosures() const {
    return getOriginalArgs()->getFirstTrailingClosureIndex().has_value();
  }

  /// Whether a given index is for an unlabeled trailing closure, which is the
  /// first trailing closure in the argument list.
  ///
  /// Note for a type-checked argument list, this must be queried on
  /// \c getOriginalArgs instead.
  bool isUnlabeledTrailingClosureIndex(unsigned i) const {
    assert(!HasOriginalArgs && "Query original args instead");
    return hasAnyTrailingClosures() && *getFirstTrailingClosureIndex() == i;
  }

  /// Whether a given index is for a labeled trailing closure in an argument
  /// list with multiple trailing closures.
  ///
  /// Note for a type-checked argument list, this must be queried on
  /// \c getOriginalArgs instead.
  bool isLabeledTrailingClosureIndex(unsigned i) const {
    assert(!HasOriginalArgs && "Query original args instead");
    if (!hasAnyTrailingClosures())
      return false;
    return i > *getFirstTrailingClosureIndex() && i < size();
  }

  /// Whether a given index is for a labeled or unlabeled trailing closure.
  ///
  /// Note for a type-checked argument list, this must be queried on
  /// \c getOriginalArgs instead.
  bool isTrailingClosureIndex(unsigned i) const {
    assert(!HasOriginalArgs && "Query original args instead");
    if (!hasAnyTrailingClosures())
      return false;
    return i >= *getFirstTrailingClosureIndex() && i < size();
  }

  /// Returns the range of arguments excluding any trailing closures.
  ///
  /// Note for a type-checked argument list, this must be queried on
  /// \c getOriginalArgs instead.
  iterator_range<iterator> getNonTrailingArgs() const {
    assert(!HasOriginalArgs && "Query original args instead");
    auto idx = getFirstTrailingClosureIndex();
    if (!idx.has_value())
      return *this;

    return {begin(), iterator(this, *idx)};
  }

  /// Returns the range of trailing closure arguments.
  ///
  /// Note for a type-checked argument list, this must be queried on
  /// \c getOriginalArgs instead.
  iterator_range<iterator> getTrailingClosures() const {
    assert(!HasOriginalArgs && "Query original args instead");
    auto idx = getFirstTrailingClosureIndex();
    if (!idx.has_value())
      return {end(), end()};

    return {iterator(this, *idx), end()};
  }

  /// Retrieve the first trailing closure argument in the argument list, or
  /// \c None if there are no trailing closures.
  ///
  /// Note for a type-checked argument list, this must be queried on
  /// \c getOriginalArgs instead.
  llvm::Optional<Argument> getFirstTrailingClosure() const {
    assert(!HasOriginalArgs && "Query original args instead");
    auto idx = getFirstTrailingClosureIndex();
    if (!idx.has_value())
      return llvm::None;
    return get(*idx);
  }

  /// Retrieve the source range of any trailing closure arguments, or an empty
  /// range if there are no trailing closures.
  ///
  /// Note for a type-checked argument list, this must be queried on
  /// \c getOriginalArgs instead.
  SourceRange getTrailingSourceRange() const {
    assert(!HasOriginalArgs && "Query original args instead");
    auto firstClosure = getFirstTrailingClosure();
    if (!firstClosure)
      return SourceRange();

    return SourceRange(firstClosure->getStartLoc(), back().getEndLoc());
  }

  // MARK: Misc Queries

  /// Whether any of the arguments in the argument are inout.
  bool hasAnyInOutArgs() const {
    return llvm::any_of(*this, [](auto arg) { return arg.isInOut(); });
  }

  /// Whether the argument list has a single argument, which may be labeled or
  /// unlabeled.
  bool isUnary() const { return size() == 1; }

  /// Whether the argument list has a single unlabeled argument.
  bool isUnlabeledUnary() const { return isUnary() && getLabel(0).empty(); }

  /// If the argument list has a single argument, which may be labeled or
  /// unlabeled, returns its expression, or \c nullptr if this isn't a unary
  /// argument list.
  Expr *getUnaryExpr() const { return isUnary() ? getExpr(0) : nullptr; }

  /// If the argument list has a single unlabeled argument, returns its
  /// expression, or \c nullptr if this isn't an unlabeled unary argument list.
  Expr *getUnlabeledUnaryExpr() const {
    return isUnlabeledUnary() ? getExpr(0) : nullptr;
  }

  /// Retrieve an array of the argument expressions used in the argument list.
  ArrayRef<Expr *> getArgExprs() const { return getExprsBuffer(); }

  /// Retrieve an array of the argument labels used in the argument list.
  ArrayRef<Identifier>
  getArgumentLabels(SmallVectorImpl<Identifier> &scratch) const;

  /// If the provided expression appears as one of the argument list's
  /// arguments, returns its index. Otherwise returns \c None. By default this
  /// will match against semantic sub-expressions, but that may be disabled by
  /// passing \c false for \c allowSemantic.
  llvm::Optional<unsigned> findArgumentExpr(Expr *expr,
                                            bool allowSemantic = true) const;

  /// Creates a TupleExpr or ParenExpr that holds the argument exprs. A
  /// ParenExpr will be returned for a single argument, otherwise a TupleExpr.
  /// Don't use this function unless you actually need an AST transform.
  Expr *packIntoImplicitTupleOrParen(
      ASTContext &ctx,
      llvm::function_ref<Type(Expr *)> getType = __Expr_getType) const;

  /// Whether the argument list matches a given parameter list. This will return
  /// \c false if the arity doesn't match, or any of the canonical types or
  /// labels don't match. Note that this expects types to be present for the
  /// arguments and parameters.
  bool matches(ArrayRef<AnyFunctionType::Param> params,
               llvm::function_ref<Type(Expr *)> getType = __Expr_getType) const;

  /// Returns the argument list prior to being rewritten by the expression
  /// type-checker. If the argument list either hasn't been type-checked yet,
  /// or was implicitly generated as type-checked, it returns itself.
  ///
  /// Note that returned argument list will still have rewritten argument
  /// expressions with types. However the order of the arguments will reflect
  /// the call-site as written by the user, default arguments will be stripped,
  /// and variadic arguments will be expanded.
  ArgumentList *getOriginalArgs() {
    if (auto *originalArgs = getStoredOriginalArgs())
      return originalArgs;
    return this;
  }

  /// Returns the argument list prior to being rewritten by the expression
  /// type-checker. If the argument list either hasn't been type-checked yet,
  /// or was implicitly generated as type-checked, it returns itself.
  ///
  /// Note that returned argument list will still have rewritten argument
  /// expressions with types. However the order of the arguments will reflect
  /// the call-site as written by the user, default arguments will be stripped,
  /// and variadic arguments will be expanded.
  const ArgumentList *getOriginalArgs() const {
    if (auto *originalArgs = getStoredOriginalArgs())
      return originalArgs;
    return this;
  }

  ArgumentList *walk(ASTWalker &walker);

  SWIFT_DEBUG_DUMP;
  void dump(raw_ostream &OS, unsigned Indent = 0) const;
};

} // end namespace swift

#endif
