//===----------------------------------------------------------------------===//
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

#include "swift/Refactoring/Refactoring.h"
#include "RefactoringActions.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/SourceFile.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/SourceManager.h"
#include "swift/IDE/IDERequests.h"
#include "swift/Parse/Lexer.h"

using namespace swift;
using namespace swift::ide;
using namespace swift::refactoring;

StringRef getDefaultPreferredName(RefactoringKind Kind) {
  switch(Kind) {
    case RefactoringKind::None:
      llvm_unreachable("Should be a valid refactoring kind");
    case RefactoringKind::GlobalRename:
    case RefactoringKind::LocalRename:
      return "newName";
    case RefactoringKind::ExtractExpr:
    case RefactoringKind::ExtractRepeatedExpr:
      return "extractedExpr";
    case RefactoringKind::ExtractFunction:
      return "extractedFunc";
    default:
      return "";
  }
}

static SmallVector<RefactorAvailabilityInfo, 0>
collectRefactoringsAtCursor(SourceFile *SF, unsigned Line, unsigned Column,
                            ArrayRef<DiagnosticConsumer *> DiagConsumers) {
  // Prepare the tool box.
  ASTContext &Ctx = SF->getASTContext();
  SourceManager &SM = Ctx.SourceMgr;
  DiagnosticEngine DiagEngine(SM);
  std::for_each(DiagConsumers.begin(), DiagConsumers.end(),
                [&](DiagnosticConsumer *Con) { DiagEngine.addConsumer(*Con); });
  SourceLoc Loc = SM.getLocForLineCol(SF->getBufferID().value(), Line, Column);
  if (Loc.isInvalid())
    return {};

  ResolvedCursorInfoPtr Tok =
      evaluateOrDefault(SF->getASTContext().evaluator,
                        CursorInfoRequest{CursorInfoOwner(
                            SF, Lexer::getLocForStartOfToken(SM, Loc))},
                        new ResolvedCursorInfo());
  return collectRefactorings(Tok, /*ExcludeRename=*/false);
}

static bool collectRangeStartRefactorings(const ResolvedRangeInfo &Info) {
  switch (Info.Kind) {
  case RangeKind::SingleExpression:
  case RangeKind::SingleStatement:
  case RangeKind::SingleDecl:
  case RangeKind::PartOfExpression:
    return true;
  case RangeKind::MultiStatement:
  case RangeKind::MultiTypeMemberDecl:
  case RangeKind::Invalid:
    return false;
  }
}

StringRef swift::ide::
getDescriptiveRefactoringKindName(RefactoringKind Kind) {
    switch(Kind) {
      case RefactoringKind::None:
        llvm_unreachable("Should be a valid refactoring kind");
#define REFACTORING(KIND, NAME, ID) case RefactoringKind::KIND: return NAME;
#include "swift/Refactoring/RefactoringKinds.def"
    }
    llvm_unreachable("unhandled kind");
  }

  StringRef swift::ide::getDescriptiveRenameUnavailableReason(
      RefactorAvailableKind Kind) {
    switch(Kind) {
    case RefactorAvailableKind::Available:
      return "";
    case RefactorAvailableKind::Unavailable_system_symbol:
      return "symbol from system module cannot be renamed";
    case RefactorAvailableKind::Unavailable_has_no_location:
      return "symbol without a declaration location cannot be renamed";
    case RefactorAvailableKind::Unavailable_has_no_name:
      return "cannot find the name of the symbol";
    case RefactorAvailableKind::Unavailable_has_no_accessibility:
      return "cannot decide the accessibility of the symbol";
    case RefactorAvailableKind::Unavailable_decl_from_clang:
      return "cannot rename a Clang symbol from its Swift reference";
    case RefactorAvailableKind::Unavailable_decl_in_macro:
      return "cannot rename a symbol declared in a macro";
    }
    llvm_unreachable("unhandled kind");
  }

SourceLoc swift::ide::RangeConfig::getStart(SourceManager &SM) {
  return SM.getLocForLineCol(BufferID, Line, Column);
}

SourceLoc swift::ide::RangeConfig::getEnd(SourceManager &SM) {
  return getStart(SM).getAdvancedLoc(Length);
}

SmallVector<RefactorAvailabilityInfo, 0>
swift::ide::collectRefactorings(ResolvedCursorInfoPtr CursorInfo,
                                bool ExcludeRename) {
  SmallVector<RefactorAvailabilityInfo, 0> Infos;

  DiagnosticEngine DiagEngine(
      CursorInfo->getSourceFile()->getASTContext().SourceMgr);

  // Only macro expansion is available within generated buffers
  if (CursorInfo->getSourceFile()->Kind == SourceFileKind::MacroExpansion) {
    if (RefactoringActionInlineMacro::isApplicable(CursorInfo, DiagEngine)) {
      Infos.emplace_back(RefactoringKind::InlineMacro,
                         RefactorAvailableKind::Available);
    }
    return Infos;
  }

  if (!ExcludeRename) {
    if (auto Info = getRenameInfo(CursorInfo)) {
      Infos.push_back(std::move(Info->Availability));
    }
  }

#define CURSOR_REFACTORING(KIND, NAME, ID)                                     \
  if (RefactoringKind::KIND != RefactoringKind::LocalRename &&                 \
      RefactoringAction##KIND::isApplicable(CursorInfo, DiagEngine))           \
    Infos.emplace_back(RefactoringKind::KIND, RefactorAvailableKind::Available);
#include "swift/Refactoring/RefactoringKinds.def"

  return Infos;
}

SmallVector<RefactorAvailabilityInfo, 0>
swift::ide::collectRefactorings(SourceFile *SF, RangeConfig Range,
                                bool &CollectRangeStartRefactorings,
                                ArrayRef<DiagnosticConsumer *> DiagConsumers) {
  if (Range.Length == 0)
    return collectRefactoringsAtCursor(SF, Range.Line, Range.Column,
                                       DiagConsumers);

  // No refactorings are available within generated buffers
  if (SF->Kind == SourceFileKind::MacroExpansion ||
      SF->Kind == SourceFileKind::DefaultArgument)
    return {};

  // Prepare the tool box.
  ASTContext &Ctx = SF->getASTContext();
  SourceManager &SM = Ctx.SourceMgr;
  DiagnosticEngine DiagEngine(SM);
  std::for_each(DiagConsumers.begin(), DiagConsumers.end(),
    [&](DiagnosticConsumer *Con) { DiagEngine.addConsumer(*Con); });
  ResolvedRangeInfo Result = evaluateOrDefault(SF->getASTContext().evaluator,
    RangeInfoRequest(RangeInfoOwner({SF,
                      Range.getStart(SF->getASTContext().SourceMgr),
                      Range.getEnd(SF->getASTContext().SourceMgr)})),
                                               ResolvedRangeInfo());

  bool enableInternalRefactoring = getenv("SWIFT_ENABLE_INTERNAL_REFACTORING_ACTIONS");

  SmallVector<RefactorAvailabilityInfo, 0> Infos;

#define RANGE_REFACTORING(KIND, NAME, ID)                                      \
  if (RefactoringAction##KIND::isApplicable(Result, DiagEngine))               \
    Infos.emplace_back(RefactoringKind::KIND, RefactorAvailableKind::Available);
#define INTERNAL_RANGE_REFACTORING(KIND, NAME, ID)                            \
  if (enableInternalRefactoring)                                              \
    RANGE_REFACTORING(KIND, NAME, ID)
#include "swift/Refactoring/RefactoringKinds.def"

  CollectRangeStartRefactorings = collectRangeStartRefactorings(Result);

  return Infos;
}

bool swift::ide::
refactorSwiftModule(ModuleDecl *M, RefactoringOptions Opts,
                    SourceEditConsumer &EditConsumer,
                    DiagnosticConsumer &DiagConsumer) {
  assert(Opts.Kind != RefactoringKind::None && "should have a refactoring kind.");

  // Use the default name if not specified.
  if (Opts.PreferredName.empty()) {
    Opts.PreferredName = getDefaultPreferredName(Opts.Kind).str();
  }

  switch (Opts.Kind) {
#define SEMANTIC_REFACTORING(KIND, NAME, ID)                                   \
case RefactoringKind::KIND: {                                                  \
      RefactoringAction##KIND Action(M, Opts, EditConsumer, DiagConsumer);     \
      if (RefactoringKind::KIND == RefactoringKind::LocalRename ||             \
          RefactoringKind::KIND == RefactoringKind::ExpandMacro ||             \
          Action.isApplicable())                                               \
        return Action.performChange();                                         \
      return true;                                                             \
  }
#include "swift/Refactoring/RefactoringKinds.def"
    case RefactoringKind::LocalRename:
    case RefactoringKind::GlobalRename:
    case RefactoringKind::FindGlobalRenameRanges:
    case RefactoringKind::FindLocalRenameRanges:
      llvm_unreachable("not a valid refactoring kind");
    case RefactoringKind::None:
      llvm_unreachable("should not enter here.");
  }
  llvm_unreachable("unhandled kind");
}
