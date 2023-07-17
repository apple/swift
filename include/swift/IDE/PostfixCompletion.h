//===--- DotExprCodeCompletion.h ------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IDE_POSTFIXCOMPLETION_H
#define SWIFT_IDE_POSTFIXCOMPLETION_H

#include "swift/IDE/CodeCompletionConsumer.h"
#include "swift/IDE/CodeCompletionContext.h"
#include "swift/IDE/TypeCheckCompletionCallback.h"

namespace swift {
namespace ide {

/// Used to collect and store information needed to perform postfix completion
/// (either <base>.#^COMPLETE^# or <base> #^COMPLETE^#).
class PostfixCompletionCallback : public TypeCheckCompletionCallback {
  struct Result {
    /// The type that we are completing on. Will never be null.
    Type BaseTy;

    /// The decl that we are completing on. Is \c nullptr if we are completing
    /// on an expression.
    ValueDecl *BaseDecl;

    /// Whether \c BaseDecl refers to a function that has not been called yet.
    /// In such cases, we know that \p BaseTy is the type of \p BaseDecl and we
    /// can use \p BaseDecl for more detailed call pattern completions.
    bool IsBaseDeclUnapplied;

    /// If the expression we are completing on statically refers to a metatype,
    /// that is if it's something like 'MyType'. In such cases we want to offer
    /// constructor call pattern completions and don't want to suggeste
    /// operators that work on metatypes.
    bool BaseIsStaticMetaType;

    /// The types that the completion is expected to produce.
    SmallVector<Type, 4> ExpectedTypes;

    /// Whether results that produce 'Void' should be disfavored. This happens
    /// if the context is requiring a value. Once a completion produces 'Void',
    /// we know that we can't retrieve a value from it anymore.
    bool ExpectsNonVoid;

    /// If the code completion expression occurs as a single statement in a
    /// single-expression closure. In such cases we don't want to disfavor
    /// results that produce 'Void' because the user might intend to make the
    /// closure a multi-statment closure, in which case this expression is no
    /// longer implicitly returned.
    bool IsImplicitSingleExpressionReturn;

    /// Whether the surrounding context is async and thus calling async
    /// functions is supported.
    bool IsInAsyncContext;

    /// Actor isolations that were determined during constraint solving but that
    /// haven't been saved to the AST.
    llvm::DenseMap<AbstractClosureExpr *, ClosureActorIsolation>
        ClosureActorIsolations;

    /// Checks whether this result has the same \c BaseTy and \c BaseDecl as
    /// \p Other and if the two can thus be merged to be one value lookup in
    /// \c deliverResults.
    bool canBeMergedWith(const Result &Other, DeclContext &DC) const;

    /// Merge this result with \p Other. Assumes that they can be merged.
    void merge(const Result &Other, DeclContext &DC);
  };

  CodeCompletionExpr *CompletionExpr;
  DeclContext *DC;

  SmallVector<Result, 4> Results;

  /// Add a result to \c Results, merging it with an existing result, if
  /// possible.
  void addResult(const Result &Res);

  void sawSolutionImpl(const constraints::Solution &solution) override;

public:
  PostfixCompletionCallback(CodeCompletionExpr *CompletionExpr, DeclContext *DC)
      : CompletionExpr(CompletionExpr), DC(DC) {}

  /// Typecheck the code completion expression in isolation, calling
  /// \c sawSolution for each solution formed.
  void fallbackTypeCheck(DeclContext *DC) override;

  /// Deliver code completion results that were discoverd by \c sawSolution to
  /// \p Consumer.
  /// \param DotLoc If we are completing after a dot, the location of the dot,
  ///               otherwise an invalid SourceLoc.
  /// \param IsInSelector Whether we are completing in an Objective-C selector.
  /// \param IncludeOperators If operators should be suggested. Assumes that
  ///                         \p DotLoc is invalid
  /// \param HasLeadingSpace Whether there is a space separating the exiting
  ///                        expression and the code completion token.
  void deliverResults(SourceLoc DotLoc, bool IsInSelector,
                      bool IncludeOperators, bool HasLeadingSpace,
                      CodeCompletionContext &CompletionCtx,
                      CodeCompletionConsumer &Consumer);
};

} // end namespace ide
} // end namespace swift

#endif // SWIFT_IDE_POSTFIXCOMPLETION_H
