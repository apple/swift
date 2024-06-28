//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "ContextFinder.h"
#include "RefactoringActions.h"
#include "swift/AST/Stmt.h"
#include "swift/Basic/Assertions.h"

using namespace swift::refactoring;

static CharSourceRange
findSourceRangeToWrapInCatch(const ResolvedExprStartCursorInfo &CursorInfo,
                             SourceFile *TheFile, SourceManager &SM) {
  Expr *E = CursorInfo.getTrailingExpr();
  if (!E)
    return CharSourceRange();
  auto Node = ASTNode(E);
  auto NodeChecker = [](ASTNode N) { return N.isStmt(StmtKind::Brace); };
  ContextFinder Finder(*TheFile, Node, NodeChecker);
  Finder.resolve();
  auto Contexts = Finder.getContexts();
  if (Contexts.empty())
    return CharSourceRange();
  auto TargetNode = Contexts.back();
  BraceStmt *BStmt = dyn_cast<BraceStmt>(TargetNode.dyn_cast<Stmt *>());
  auto ConvertToCharRange = [&SM](SourceRange SR) {
    return Lexer::getCharSourceRangeFromSourceRange(SM, SR);
  };
  assert(BStmt);
  auto ExprRange = ConvertToCharRange(E->getSourceRange());
  // Check elements of the deepest BraceStmt, pick one that covers expression.
  for (auto Elem : BStmt->getElements()) {
    auto ElemRange = ConvertToCharRange(Elem.getSourceRange());
    if (ElemRange.contains(ExprRange))
      TargetNode = Elem;
  }
  return ConvertToCharRange(TargetNode.getSourceRange());
}

bool RefactoringActionConvertToDoCatch::isApplicable(ResolvedCursorInfoPtr Tok,
                                                     DiagnosticEngine &Diag) {
  auto ExprStartInfo = dyn_cast<ResolvedExprStartCursorInfo>(Tok);
  if (!ExprStartInfo || !ExprStartInfo->getTrailingExpr())
    return false;
  return isa<ForceTryExpr>(ExprStartInfo->getTrailingExpr());
}

bool RefactoringActionConvertToDoCatch::performChange() {
  auto ExprStartInfo = cast<ResolvedExprStartCursorInfo>(CursorInfo);
  auto *TryExpr = dyn_cast<ForceTryExpr>(ExprStartInfo->getTrailingExpr());
  assert(TryExpr);
  auto Range = findSourceRangeToWrapInCatch(*ExprStartInfo, TheFile, SM);
  if (!Range.isValid())
    return true;
  // Wrap given range in do catch block.
  EditConsumer.accept(SM, Range.getStart(), "do {\n");
  EditorConsumerInsertStream OS(EditConsumer, SM, Range.getEnd());
  OS << "\n} catch {\n" << getCodePlaceholder() << "\n}";

  // Delete ! from try! expression
  auto ExclaimLen = getKeywordLen(tok::exclaim_postfix);
  auto ExclaimRange = CharSourceRange(TryExpr->getExclaimLoc(), ExclaimLen);
  EditConsumer.remove(SM, ExclaimRange);
  return false;
}
