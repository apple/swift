//===--- UnresolvedMemberCompletion.h -------------------------------------===//
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

#ifndef SWIFT_IDE_UNRESOLVEDMEMBERCOMPLETION_H
#define SWIFT_IDE_UNRESOLVEDMEMBERCOMPLETION_H

#include "swift/IDE/CodeCompletionConsumer.h"
#include "swift/IDE/CodeCompletionContext.h"
#include "swift/IDE/TypeCheckCompletionCallback.h"

namespace swift {
namespace ide {

/// Used to collect and store information needed to perform unresolved member
/// completion (\c CompletionKind::UnresolvedMember ) from the solutions
/// formed during expression type-checking.
class UnresolvedMemberTypeCheckCompletionCallback
    : public TypeCheckCompletionCallback {
  struct Result {
    Type ExpectedTy;
    bool IsImplicitSingleExpressionReturn;

    /// Whether the surrounding context is async and thus calling async
    /// functions is supported.
    bool IsInAsyncContext;

    /// Checks whether this result has the same \c BaseTy and \c BaseDecl as
    /// \p Other and if the two can thus be merged to be one value lookup in
    /// \c deliverResults.
    bool canBeMergedWith(const Result &Other, DeclContext &DC) const;

    /// Merge this result with \p Other. Assumes that they can be merged.
    void merge(const Result &Other, DeclContext &DC);
  };

  CodeCompletionExpr *CompletionExpr;
  DeclContext *DC;

  SmallVector<Result, 4> ExprResults;
  SmallVector<Result, 1> EnumPatternTypes;

  /// Add a result to \c Results, merging it with an existing result, if
  /// possible.
  void addExprResult(const Result &Res);

  void sawSolutionImpl(const constraints::Solution &solution) override;

public:
  UnresolvedMemberTypeCheckCompletionCallback(
      CodeCompletionExpr *CompletionExpr, DeclContext *DC)
      : CompletionExpr(CompletionExpr), DC(DC) {}

  void deliverResults(DeclContext *DC, SourceLoc DotLoc,
                      ide::CodeCompletionContext &CompletionCtx,
                      CodeCompletionConsumer &Consumer);
};

} // end namespace ide
} // end namespace swift

#endif // SWIFT_IDE_UNRESOLVEDMEMBERCOMPLETION_H
