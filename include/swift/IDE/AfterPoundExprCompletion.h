//===--- AfterPoundExprCompletion.h ---------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IDE_AFTERPOUNDEXPRCOMPLETION_H
#define SWIFT_IDE_AFTERPOUNDEXPRCOMPLETION_H

#include "swift/IDE/CodeCompletionConsumer.h"
#include "swift/IDE/CodeCompletionContext.h"
#include "swift/IDE/TypeCheckCompletionCallback.h"

namespace swift {
namespace ide {

/// Used to collect and store information needed to perform unresolved member
/// completion (\c CompletionKind::UnresolvedMember ) from the solutions
/// formed during expression type-checking.
class AfterPoundExprCompletion : public TypeCheckCompletionCallback {
  struct Result {
    Type ExpectedTy;
    bool IsImplicitSingleExpressionReturn;

    /// Whether the surrounding context is async and thus calling async
    /// functions is supported.
    bool IsInAsyncContext;
  };

  CodeCompletionExpr *CompletionExpr;
  DeclContext *DC;
  llvm::Optional<StmtKind> ParentStmtKind;

  SmallVector<Result, 4> Results;

  void sawSolutionImpl(const constraints::Solution &solution) override;

public:
  AfterPoundExprCompletion(CodeCompletionExpr *CompletionExpr, DeclContext *DC,
                           llvm::Optional<StmtKind> ParentStmtKind)
      : CompletionExpr(CompletionExpr), DC(DC), ParentStmtKind(ParentStmtKind) {
  }

  void deliverResults(ide::CodeCompletionContext &CompletionCtx,
                      CodeCompletionConsumer &Consumer);
};

} // end namespace ide
} // end namespace swift

#endif // SWIFT_IDE_AFTERPOUNDEXPRCOMPLETION_H
