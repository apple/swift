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
#include "swift/AST/DiagnosticsRefactoring.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Stmt.h"
#include "swift/Basic/Assertions.h"

using namespace swift::refactoring;

static EnumDecl *getEnumDeclFromSwitchStmt(SwitchStmt *SwitchS) {
  if (auto SubjectTy = SwitchS->getSubjectExpr()->getType()) {
    // FIXME: Support more complex subject like '(Enum1, Enum2)'.
    return dyn_cast_or_null<EnumDecl>(SubjectTy->getAnyNominal());
  }
  return nullptr;
}

static bool performCasesExpansionInSwitchStmt(SwitchStmt *SwitchS,
                                              DiagnosticEngine &DiagEngine,
                                              SourceLoc ExpandedStmtLoc,
                                              EditorConsumerInsertStream &OS) {
  // Assume enum elements are not handled in the switch statement.
  auto EnumDecl = getEnumDeclFromSwitchStmt(SwitchS);
  assert(EnumDecl);
  llvm::DenseSet<EnumElementDecl *> UnhandledElements;
  EnumDecl->getAllElements(UnhandledElements);
  for (auto Current : SwitchS->getCases()) {
    if (Current->isDefault()) {
      continue;
    }
    // For each handled enum element, remove it from the bucket.
    for (auto Item : Current->getCaseLabelItems()) {
      if (auto *EEP = dyn_cast_or_null<EnumElementPattern>(Item.getPattern())) {
        UnhandledElements.erase(EEP->getElementDecl());
      }
    }
  }

  // If all enum elements are handled in the switch statement, issue error.
  if (UnhandledElements.empty()) {
    DiagEngine.diagnose(ExpandedStmtLoc, diag::no_remaining_cases);
    return true;
  }

  printEnumElementsAsCases(UnhandledElements, OS);
  return false;
}

// Finds SwitchStmt that contains given CaseStmt.
static SwitchStmt *findEnclosingSwitchStmt(CaseStmt *CS, SourceFile *SF,
                                           DiagnosticEngine &DiagEngine) {
  auto IsSwitch = [](ASTNode Node) {
    return Node.is<Stmt *>() &&
           Node.get<Stmt *>()->getKind() == StmtKind::Switch;
  };
  ContextFinder Finder(*SF, CS, IsSwitch);
  Finder.resolve();

  // If failed to find the switch statement, issue error.
  if (Finder.getContexts().empty()) {
    DiagEngine.diagnose(CS->getStartLoc(), diag::no_parent_switch);
    return nullptr;
  }
  auto *SwitchS =
      static_cast<SwitchStmt *>(Finder.getContexts().back().get<Stmt *>());
  // Make sure that CaseStmt is included in switch that was found.
  auto Cases = SwitchS->getCases();
  auto Default = std::find(Cases.begin(), Cases.end(), CS);
  if (Default == Cases.end()) {
    DiagEngine.diagnose(CS->getStartLoc(), diag::no_parent_switch);
    return nullptr;
  }
  return SwitchS;
}

bool RefactoringActionExpandDefault::isApplicable(
    ResolvedCursorInfoPtr CursorInfo, DiagnosticEngine &Diag) {
  auto Exit = [&](bool Applicable) {
    if (!Applicable)
      Diag.diagnose(SourceLoc(), diag::invalid_default_location);
    return Applicable;
  };
  auto StmtStartInfo = dyn_cast<ResolvedStmtStartCursorInfo>(CursorInfo);
  if (!StmtStartInfo)
    return Exit(false);
  if (auto *CS = dyn_cast<CaseStmt>(StmtStartInfo->getTrailingStmt())) {
    auto EnclosingSwitchStmt =
        findEnclosingSwitchStmt(CS, CursorInfo->getSourceFile(), Diag);
    if (!EnclosingSwitchStmt)
      return false;
    auto EnumD = getEnumDeclFromSwitchStmt(EnclosingSwitchStmt);
    auto IsApplicable = CS->isDefault() && EnumD != nullptr;
    return IsApplicable;
  }
  return Exit(false);
}

bool RefactoringActionExpandDefault::performChange() {
  // If we've not seen the default statement inside the switch statement, issue
  // error.
  auto StmtStartInfo = cast<ResolvedStmtStartCursorInfo>(CursorInfo);
  auto *CS = static_cast<CaseStmt *>(StmtStartInfo->getTrailingStmt());
  auto *SwitchS = findEnclosingSwitchStmt(CS, TheFile, DiagEngine);
  assert(SwitchS);
  EditorConsumerInsertStream OS(
      EditConsumer, SM,
      Lexer::getCharSourceRangeFromSourceRange(SM, CS->getLabelItemsRange()));
  return performCasesExpansionInSwitchStmt(SwitchS, DiagEngine,
                                           CS->getStartLoc(), OS);
}

bool RefactoringActionExpandSwitchCases::isApplicable(
    ResolvedCursorInfoPtr CursorInfo, DiagnosticEngine &DiagEngine) {
  auto StmtStartInfo = dyn_cast<ResolvedStmtStartCursorInfo>(CursorInfo);
  if (!StmtStartInfo || !StmtStartInfo->getTrailingStmt())
    return false;
  if (auto *Switch = dyn_cast<SwitchStmt>(StmtStartInfo->getTrailingStmt())) {
    return getEnumDeclFromSwitchStmt(Switch);
  }
  return false;
}

bool RefactoringActionExpandSwitchCases::performChange() {
  auto StmtStartInfo = cast<ResolvedStmtStartCursorInfo>(CursorInfo);
  auto *SwitchS = dyn_cast<SwitchStmt>(StmtStartInfo->getTrailingStmt());
  assert(SwitchS);

  auto InsertRange = CharSourceRange();
  auto Cases = SwitchS->getCases();
  auto Default = std::find_if(Cases.begin(), Cases.end(),
                              [](CaseStmt *Stmt) { return Stmt->isDefault(); });
  if (Default != Cases.end()) {
    auto DefaultRange = (*Default)->getLabelItemsRange();
    InsertRange = Lexer::getCharSourceRangeFromSourceRange(SM, DefaultRange);
  } else {
    auto RBraceLoc = SwitchS->getRBraceLoc();
    InsertRange = CharSourceRange(SM, RBraceLoc, RBraceLoc);
  }
  EditorConsumerInsertStream OS(EditConsumer, SM, InsertRange);
  if (SM.getLineAndColumnInBuffer(SwitchS->getLBraceLoc()).first ==
      SM.getLineAndColumnInBuffer(SwitchS->getRBraceLoc()).first) {
    OS << "\n";
  }
  auto Result = performCasesExpansionInSwitchStmt(SwitchS, DiagEngine,
                                                  SwitchS->getStartLoc(), OS);
  return Result;
}
