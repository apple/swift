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

#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsRefactoring.h"
#include "swift/AST/SourceFile.h"
#include "swift/Basic/Assertions.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/IDE/IDEBridging.h"
#include "swift/Parse/Lexer.h"
#include "swift/Refactoring/Refactoring.h"

using namespace swift;
using namespace swift::ide;

#if SWIFT_BUILD_SWIFT_SYNTAX
std::vector<ResolvedAndRenameLoc>
swift::ide::resolveRenameLocations(ArrayRef<RenameLoc> RenameLocs,
                                   StringRef NewName, SourceFile &SF,
                                   DiagnosticEngine &Diags) {
  SourceManager &SM = SF.getASTContext().SourceMgr;
  unsigned BufferID = SF.getBufferID().value();

  std::vector<SourceLoc> UnresolvedLocs;
  for (const RenameLoc &RenameLoc : RenameLocs) {
    DeclNameViewer OldName(RenameLoc.OldName);
    SourceLoc Location =
        SM.getLocForLineCol(BufferID, RenameLoc.Line, RenameLoc.Column);

    if (!OldName.isValid()) {
      Diags.diagnose(Location, diag::invalid_name, RenameLoc.OldName);
      return {};
    }

    if (!NewName.empty()) {
      DeclNameViewer NewDeclName(NewName);
      ArrayRef<StringRef> ParamNames = NewDeclName.args();
      bool newOperator = Lexer::isOperator(NewDeclName.base());
      bool NewNameIsValid =
          NewDeclName.isValid() &&
          (Lexer::isIdentifier(NewDeclName.base()) || newOperator) &&
          std::all_of(ParamNames.begin(), ParamNames.end(),
                      [](StringRef Label) {
                        return Label.empty() || Lexer::isIdentifier(Label);
                      });

      if (!NewNameIsValid) {
        Diags.diagnose(Location, diag::invalid_name, NewName);
        return {};
      }

      if (NewDeclName.partsCount() != OldName.partsCount()) {
        Diags.diagnose(Location, diag::arity_mismatch, NewName,
                       RenameLoc.OldName);
        return {};
      }

      if (RenameLoc.Usage == RenameLocUsage::Call && !OldName.isFunction()) {
        Diags.diagnose(Location, diag::name_not_functionlike, NewName);
        return {};
      }
    }

    UnresolvedLocs.push_back({Location});
  }

  assert(UnresolvedLocs.size() == RenameLocs.size());

  std::vector<ResolvedLoc> resolvedLocsInSourceOrder =
      runNameMatcher(SF, UnresolvedLocs);

  // Callers need to corrolate the `ResolvedLoc` with the `RenameLoc` that they
  // originated from. Match them.
  std::vector<ResolvedAndRenameLoc> resolvedAndRenameLocs;
  for (auto [unresolvedLoc, renameLoc] :
       llvm::zip_equal(UnresolvedLocs, RenameLocs)) {
    auto found = llvm::find_if(
        resolvedLocsInSourceOrder,
        [unresolvedLoc = unresolvedLoc](const ResolvedLoc &resolved) {
          return resolved.range.getStart() == unresolvedLoc;
        });
    ResolvedLoc resolvedLoc;
    if (found == resolvedLocsInSourceOrder.end()) {
      resolvedLoc =
          ResolvedLoc(CharSourceRange(),
                      /*LabelRanges=*/{}, std::nullopt, LabelRangeType::None,
                      /*IsActive=*/true, ResolvedLocContext::Comment);
    } else {
      resolvedLoc = *found;
    }
    resolvedAndRenameLocs.push_back({renameLoc, resolvedLoc});
  }
  return resolvedAndRenameLocs;
}
#endif

CancellableResult<std::vector<SyntacticRenameRangeDetails>>
swift::ide::findSyntacticRenameRanges(SourceFile *SF,
                                      ArrayRef<RenameLoc> RenameLocs,
                                      StringRef NewName) {
  using ResultType =
      CancellableResult<std::vector<SyntacticRenameRangeDetails>>;
#if !SWIFT_BUILD_SWIFT_SYNTAX
  return ResultType::failure("find-syntactic-rename-ranges is not supported because sourcekitd was built without swift-syntax");
#else
  assert(SF && "null source file");

  SourceManager &SM = SF->getASTContext().SourceMgr;
  DiagnosticEngine DiagEngine(SM);
  std::string ErrBuffer;
  llvm::raw_string_ostream DiagOS(ErrBuffer);
  swift::PrintingDiagnosticConsumer DiagConsumer(DiagOS);
  DiagEngine.addConsumer(DiagConsumer);

  auto ResolvedAndRenameLocs =
      resolveRenameLocations(RenameLocs, NewName, *SF, DiagEngine);
  if (ResolvedAndRenameLocs.size() != RenameLocs.size() ||
      DiagConsumer.didErrorOccur())
    return ResultType::failure(ErrBuffer);

  std::vector<SyntacticRenameRangeDetails> Result;
  for (const ResolvedAndRenameLoc &Loc : ResolvedAndRenameLocs) {
    SyntacticRenameRangeDetails Details = getSyntacticRenameRangeDetails(
        SM, Loc.renameLoc.OldName, Loc.resolved, Loc.renameLoc);
    if (Details.Type == RegionType::Mismatch) {
      DiagEngine.diagnose(Loc.resolved.range.getStart(),
                          diag::mismatched_rename, NewName);
      Result.emplace_back(SyntacticRenameRangeDetails{Details.Type, {}});
    } else {
      Result.push_back(Details);
    }
  }

  if (DiagConsumer.didErrorOccur())
    return ResultType::failure(ErrBuffer);

  return ResultType::success(Result);
#endif
}
