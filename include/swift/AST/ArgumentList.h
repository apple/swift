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

#include "swift/AST/Types.h"
#include "swift/Basic/Debug.h"
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
///         an unlabelled     'other' is the label, 'arg3' is the expr,
///         argument          and isInout() is true.
/// \endcode
class Argument final {
  SourceLoc LabelLoc;
  Identifier Label;
  Expr *ArgExpr;
  // TODO: Store inout bit here.

public:
  Argument(SourceLoc labelLoc, Identifier label, Expr *expr);

  /// Make an unlabelled argument.
  static Argument unlabelled(Expr *expr) {
    return Argument(SourceLoc(), Identifier(), expr);
  }

  SourceLoc getStartLoc() const { return getSourceRange().Start; }
  SourceLoc getEndLoc() const { return getSourceRange().End; }
  SourceRange getSourceRange() const;

  /// If the argument has a label with location information, returns the
  /// location.
  SourceLoc getLabelLoc() const { return LabelLoc; }

  /// The argument label written in the call.
  Identifier getLabel() const { return Label; }
  void setLabel(Identifier newLabel) { Label = newLabel; }

  /// The argument expression.
  Expr *getExpr() const { return ArgExpr; }
  void setExpr(Expr *newExpr) { ArgExpr = newExpr; }

  /// Whether the argument is inout, denoted with the '&' prefix.
  bool isInOut() const;

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
class alignas(Argument) ArgumentList final
    : private llvm::TrailingObjects<ArgumentList, Argument> {
  friend TrailingObjects;

  SourceLoc LParenLoc;
  SourceLoc RParenLoc;

  /// The number of arguments in the list.
  unsigned NumArgs : 16;

  /// The index of the first trailing closure, or \c NumArgs if there is no
  /// trailing closure.
  unsigned FirstTrailingClosureAt : 16;

  /// Whether the argument list was implicitly generated.
  bool IsImplicit : 1;

  ArgumentList(SourceLoc lParenLoc, SourceLoc rParenLoc, unsigned numArgs,
               Optional<unsigned> firstTrailingClosureAt, bool isImplicit)
      : LParenLoc(lParenLoc), RParenLoc(rParenLoc), NumArgs(numArgs),
        IsImplicit(isImplicit) {
    assert(NumArgs == numArgs && "Num args truncated");
    if (firstTrailingClosureAt) {
      assert(*firstTrailingClosureAt < numArgs);
      FirstTrailingClosureAt = *firstTrailingClosureAt;
    } else {
      FirstTrailingClosureAt = numArgs;
    }
  }

  // Only allow allocation in an ASTContext.
  void *operator new(size_t bytes) = delete;
  void *operator new(size_t bytes, void *mem) = delete;
  void operator delete(void *data) = delete;

  ArgumentList(const ArgumentList &) = delete;
  ArgumentList &operator=(const ArgumentList &) = delete;

public:
  /// Creates a new ArgumentList.
  ///
  /// \param lParenLoc The location of the opening '('. Note that for a
  /// subscript argument list, this will be for the opening '['.
  /// \param args The list of arguments in the call.
  /// \param rParenLoc The location of the closing ')'. Note that for a
  /// subscript argument list, this will be for the closing ']'.
  /// \param firstTrailingClosureAt If present, the index of the first trailing
  /// closure argument.
  /// \param isImplicit Whether this is an implicitly generated argument list.
  /// \param arena The arena to allocate the ArgumentList in.
  static ArgumentList *create(ASTContext &ctx, SourceLoc lParenLoc,
                              ArrayRef<Argument> args, SourceLoc rParenLoc,
                              Optional<unsigned> firstTrailingClosureAt,
                              bool isImplicit,
                              Optional<AllocationArena> arena = None);

  /// Creates a new implicit ArgumentList.
  ///
  /// \param lParenLoc The location of the opening '('. Note that for a
  /// subscript argument list, this will be for the opening '['.
  /// \param args The list of arguments in the call.
  /// \param rParenLoc The location of the closing ')'. Note that for a
  /// subscript argument list, this will be for the closing ']'.
  /// \param arena The arena to allocate the ArgumentList in.
  static ArgumentList *createImplicit(ASTContext &ctx, SourceLoc lParenLoc,
                                      ArrayRef<Argument> args,
                                      SourceLoc rParenLoc,
                                      Optional<AllocationArena> arena = None);

  /// Creates a new implicit ArgumentList.
  static ArgumentList *createImplicit(ASTContext &ctx, ArrayRef<Argument> args,
                                      Optional<AllocationArena> arena = None);

  /// Creates a new implicit ArgumentList with a single argument expression and
  /// a given label.
  static ArgumentList *forImplicitSingle(ASTContext &ctx, Identifier label,
                                         Expr *arg);

  /// Creates a new implicit ArgumentList with a set of unlabelled arguments.
  static ArgumentList *forImplicitUnlabelled(ASTContext &ctx,
                                             ArrayRef<Expr *> argExprs);

  /// Creates a new implicit ArgumentList with a set of argument expressions,
  /// and a DeclNameRef from which to infer the argument labels. Note that this
  /// will not introduce any inout arguments.
  static ArgumentList *forImplicitCallTo(DeclNameRef fnNameRef,
                                         ArrayRef<Expr *> argExprs,
                                         ASTContext &ctx);

  /// Creates a new implicit ArgumentList with a set of argument expressions,
  /// and a ParameterList from which to infer the argument labels and
  /// inout-ness.
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

  SourceLoc getLoc() const;
  SourceLoc getStartLoc() const { return getSourceRange().Start; }
  SourceLoc getEndLoc() const { return getSourceRange().End; }
  SourceRange getSourceRange() const;

  MutableArrayRef<Argument> getArray() {
    return {getTrailingObjects<Argument>(), NumArgs};
  }
  ArrayRef<Argument> getArray() const {
    return {getTrailingObjects<Argument>(), NumArgs};
  }

  typedef MutableArrayRef<Argument>::iterator iterator;
  typedef ArrayRef<Argument>::iterator const_iterator;
  iterator begin() { return getArray().begin(); }
  iterator end() { return getArray().end(); }
  const_iterator begin() const { return getArray().begin(); }
  const_iterator end() const { return getArray().end(); }

  const Argument &front() const { return getArray().front(); }
  const Argument &back() const { return getArray().back(); }
  Argument &front() { return getArray().front(); }
  Argument &back() { return getArray().back(); }

  unsigned size() const { return NumArgs; }
  bool empty() const { return NumArgs == 0; }

  const Argument &get(unsigned idx) const { return getArray()[idx]; }
  Argument &get(unsigned idx) { return getArray()[idx]; }

  const Argument &operator[](unsigned idx) const { return get(idx); }
  Argument &operator[](unsigned idx) { return get(idx); }

  /// Retrieve the argument expression at a given index.
  Expr *getExpr(unsigned idx) const { return get(idx).getExpr(); }

  /// Retrieve the argument label at a given index.
  Identifier getLabel(unsigned idx) const { return get(idx).getLabel(); }

  /// Retrieve the argument label location at a given index.
  SourceLoc getLabelLoc(unsigned idx) const { return get(idx).getLabelLoc(); }

  /// Whether the argument list has any labelled arguments.
  bool hasAnyArgumentLabels() const {
    return llvm::any_of(getArray(),
                        [](auto &arg) { return !arg.getLabel().empty(); });
  }

  /// Returns the index of the first trailing closure in the argument list, or
  /// \c None if there are no trailing closures.
  Optional<unsigned> getFirstTrailingClosureIndex() const {
    if (FirstTrailingClosureAt == NumArgs)
      return None;
    return FirstTrailingClosureAt;
  }

  /// The number of trailing closures in the argument list.
  unsigned getNumTrailingClosures() const {
    if (auto idx = getFirstTrailingClosureIndex())
      return size() - *idx;
    return 0;
  }

  /// Whether any trailing closures are present in the argument list.
  bool hasAnyTrailingClosures() const {
    return (bool)getFirstTrailingClosureIndex();
  }

  /// Whether this argument list has a single trailing closure that appears
  /// after the regular arguments.
  bool hasSingleTrailingClosure() const {
    return getNumTrailingClosures() == 1;
  }

  /// Whether the argument list has multiple trailing closures.
  bool hasMultipleTrailingClosures() const {
    return getNumTrailingClosures() > 1;
  }

  /// Returns the first trailing closure expression, or \c nullptr if there are
  /// no trailing closures.
  Expr *getFirstTrailingClosureExpr() const {
    if (auto idx = getFirstTrailingClosureIndex())
      return getExpr(*idx);
    return nullptr;
  }

  /// Whether any of the arguments in the argument are inout.
  bool hasAnyInOutArgs() const {
    return llvm::any_of(*this, [](auto &arg) { return arg.isInOut(); });
  }

  /// Whether the argument list has a single argument, which may be labelled or
  /// unlabelled.
  bool isUnary() const { return size() == 1; }

  /// Whether the argument list has a single unlabelled argument.
  bool isUnlabelledUnary() const { return isUnary() && getLabel(0).empty(); }

  /// If the argument list has a single argument, which may be labelled or
  /// unlabelled, returns its expression, or \c nullptr if this isn't a unary
  /// argument list.
  Expr *getUnaryArgExpr() const { return isUnary() ? getExpr(0) : nullptr; }

  /// If the argument list has a single unlabelled argument, returns its
  /// expression, or \c nullptr if this isn't an unlabelled unary argument list.
  Expr *getUnlabelledUnaryExpr() const {
    return isUnlabelledUnary() ? getExpr(0) : nullptr;
  }

  /// Retrieve an array of the argument labels used in the argument list.
  ArrayRef<Identifier>
  getArgumentLabels(SmallVectorImpl<Identifier> &scratch) const;

  /// Retrieve an array of the argument expressions used in the argument list.
  ArrayRef<Expr *> getArgExprs(SmallVectorImpl<Expr *> &scratch) const;

  /// If the provided expression appears as one of the argument list's
  /// arguments, returns its index. Otherwise returns \c None. By default this
  /// will match against semantic sub-expressions, but that may be disabled by
  /// passing \c false for \c allowSemantic.
  Optional<unsigned> findArgumentExpr(Expr *expr,
                                      bool allowSemantic = true) const;

  /// Creates a TupleExpr or ParenExpr that holds the argument exprs. A
  /// ParenExpr will be returned for a single argument, otherwise a TupleExpr.
  /// Don't use this function unless you actually need an AST transform.
  Expr *packIntoImplicitTupleOrParen(
      ASTContext &ctx,
      llvm::function_ref<Type(Expr *)> getType = __Expr_getType) const;

  /// Creates a TupleType or ParenType representing the types in the argument
  /// list. A ParenType will be returned for a single argument, otherwise a
  /// TupleType. Don't add new usages of this unless you have to.
  Type composeTupleOrParenType(
      ASTContext &ctx,
      llvm::function_ref<Type(Expr *)> getType = __Expr_getType) const;

  /// Whether the argument list matches a given parameter list. This will return
  /// \c false if the arity doesn't match, or any of the canonical types or
  /// labels don't match. Note that this expects types to be present for the
  /// arguments and parameters.
  bool matches(ArrayRef<AnyFunctionType::Param> params,
               llvm::function_ref<Type(Expr *)> getType = __Expr_getType) const;

  /// A utility for transforming the arguments of an argument list that allows
  /// the new trailing closure index to be computed.
  ///
  /// Note this assumes that a trailing closure is either removed, or turned
  /// into another trailing closure. Do not transform a trailing closure into
  /// a non-trailing argument.
  ///
  /// \return \c true if any of the arguments changed, \c false otherwise.
  bool transformInto(SmallVectorImpl<Argument> &newArgs,
                     Optional<unsigned> &newTrailingClosureIndex,
                     llvm::function_ref<void(const Argument &)> fn) const;

  /// When applying a solution to a constraint system, the type checker rewrites
  /// argument lists of calls to insert default arguments and collect varargs.
  /// Sometimes for diagnostics we want to work on the original arguments as
  /// written by the user; this performs the reverse transformation.
  ///
  /// \param newTrailingClosureIndex If non-null, the pointee will be set to
  /// the new trailing closure index for the arguments.
  ///
  /// \return \c true if any of the arguments changed, \c false otherwise.
  bool getOriginalArguments(
      SmallVectorImpl<Argument> &newArgs,
      Optional<unsigned> *newTrailingClosureIndex = nullptr) const;

  ArgumentList *walk(ASTWalker &walker);

  SWIFT_DEBUG_DUMP;
  void dump(raw_ostream &OS, unsigned Indent = 0) const;
};

} // end namespace swift

#endif
