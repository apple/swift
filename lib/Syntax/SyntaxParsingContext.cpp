//===--- SyntaxParsingContext.cpp - Syntax Tree Parsing Support------------===//
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

#include "swift/AST/Module.h"
#include "swift/Basic/Defer.h"
#include "swift/Parse/Parser.h"
#include "swift/Syntax/TokenSyntax.h"
#include "swift/Syntax/SyntaxParsingContext.h"
#include "swift/Syntax/SyntaxFactory.h"

using namespace swift;
using namespace swift::syntax;

namespace {
static TokenSyntax getTokenAtLocation(ArrayRef<RawTokenInfo> Tokens,
                                      SourceLoc Loc) {
  auto It = std::lower_bound(Tokens.begin(), Tokens.end(), Loc,
   [](const RawTokenInfo &Info, SourceLoc Loc) {
     return Info.Loc.getOpaquePointerValue() < Loc.getOpaquePointerValue();
   });
  assert(It->Loc == Loc);
  return make<TokenSyntax>(It->Token);
}

static ExprSyntax getUnknownExpr(ArrayRef<Syntax> SubExpr) {
  RawSyntax::LayoutList Layout;
  std::transform(SubExpr.begin(), SubExpr.end(), std::back_inserter(Layout),
                 [](const Syntax &S) { return S.getRaw(); });
  return make<ExprSyntax>(RawSyntax::make(SyntaxKind::UnknownExpr,
                                          Layout,
                                          SourcePresence::Present));
}
} // End of anonymous namespace

struct SyntaxParsingContext::Implementation {
  SourceFile &File;
  bool Enabled;
  std::vector<Syntax> PendingSyntax;

  Implementation(SourceFile &File, bool Enabled): File(File), Enabled(Enabled) {}
  Optional<TokenSyntax> checkBackToken(tok Kind) {
    if (PendingSyntax.empty())
      return None;
    auto Back = PendingSyntax.back().getAs<TokenSyntax>();
    if (Back.hasValue() && (*Back).getTokenKind() == Kind) {
      PendingSyntax.pop_back();
      return Back;
    }
    return None;
  }

  void addPendingSyntax(ArrayRef<Syntax> More) {
    std::transform(More.begin(), More.end(), std::back_inserter(PendingSyntax),
                 [](const Syntax &S) { return make<Syntax>(S.getRaw()); });
  }

  Syntax popPendingSyntax() {
    assert(!PendingSyntax.empty());
    auto Result = PendingSyntax.back();
    PendingSyntax.pop_back();
    return Result;
  }
};

SyntaxParsingContext::SyntaxParsingContext(SourceFile& File, bool Enabled):
  Impl(*new Implementation(File, Enabled)) {}

SyntaxParsingContext::SyntaxParsingContext(SyntaxParsingContext &Another):
    SyntaxParsingContext(Another.Impl.File, Another.Impl.Enabled) {}

SyntaxParsingContext::~SyntaxParsingContext() { delete &Impl; }

void SyntaxParsingContext::disable() { Impl.Enabled = false; }

SyntaxParsingContextRoot::
SyntaxParsingContextRoot(SourceFile &File, unsigned BufferID):
    SyntaxParsingContext(File, File.shouldKeepTokens()) {
  populateTokenSyntaxMap(File.getASTContext().LangOpts,
                         File.getASTContext().SourceMgr,
                         BufferID, File.AllRawTokenSyntax);
}

SyntaxParsingContextRoot::~SyntaxParsingContextRoot() {
  std::vector<DeclSyntax> AllTopLevel;
  if (Impl.File.SyntaxRoot.hasValue()) {
    for (auto It: Impl.File.getSyntaxRoot().getTopLevelDecls()) {
      AllTopLevel.push_back(It);
    }
  }
  for (auto S: Impl.PendingSyntax) {
    std::vector<StmtSyntax> AllStmts;
    if (S.isDecl()) {
      AllStmts.push_back(SyntaxFactory::makeDeclarationStmt(
        S.getAs<DeclSyntax>().getValue(), None));
    } else if (S.isExpr()) {
      AllStmts.push_back(SyntaxFactory::makeExpressionStmt(
        S.getAs<ExprSyntax>().getValue(), None));
    } else {
      AllStmts.push_back(S.getAs<StmtSyntax>().getValue());
    }
    AllTopLevel.push_back(SyntaxFactory::makeTopLevelCodeDecl(
      SyntaxFactory::makeStmtList(AllStmts)));
  }

  Trivia Leading, Trailing;
  Impl.File.SyntaxRoot.emplace(
    SyntaxFactory::makeSourceFile(SyntaxFactory::makeDeclList(AllTopLevel),
      SyntaxFactory::makeToken(tok::eof, "\n", SourcePresence::Present,
                               Leading, Trailing)));
}

void SyntaxParsingContext::addTokenSyntax(SourceLoc Loc) {
  if (!Impl.Enabled)
    return;
  PendingSyntax.emplace_back(getTokenAtLocation(Impl.File.getSyntaxTokens(),
                                                Loc));
}

SyntaxParsingContextChild::~SyntaxParsingContextChild() {
  Parent->Impl.addPendingSyntax(Impl.PendingSyntax);
  ContextHolder = Parent;
}

void SyntaxParsingContextExpr::makeNode(SyntaxKind Kind) {
  if (!Impl.Enabled)
    return;

  switch (Kind) {
  case SyntaxKind::IntegerLiteralExpr: {
    auto Digit = Impl.popPendingSyntax();
    Impl.PendingSyntax.push_back(SyntaxFactory::makeIntegerLiteralExpr(
      checkBackToken(tok::oper_prefix), Digit));
    break;
  }
  case SyntaxKind::StringLiteralExpr: {
    auto StringToken = Impl.popPendingSyntax();
    Impl.PendingSyntax.push_back(SyntaxFactory::
      makeStringLiteralExpr(StringToken));
    break;
  }

  default:
    break;
  }
}

SyntaxParsingContextExpr::~SyntaxParsingContextExpr() {
  if (PendingSyntax.size() > 1) {
    auto Result = getUnknownExpr(Impl.PendingSyntax);
    Impl.PendingSyntax.clear();
    Impl.PendingSyntax.push_back(Result);
  }
}
