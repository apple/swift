//===--- Formatting.cpp ---------------------------------------------------===//
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

#include "swift/AST/ASTWalker.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/TypeRepr.h"
#include "swift/IDE/SourceEntityWalker.h"
#include "swift/Parse/Parser.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Basic/SourceManager.h"
#include "swift/IDE/Indenting.h"
#include "swift/Subsystems.h"

using namespace swift;
using namespace ide;

namespace {

using StringBuilder = llvm::SmallString<64>;

static bool isOnSameLine(SourceManager &SM, SourceLoc L, SourceLoc R) {
  return Lexer::getLocForStartOfLine(SM, L) ==
    Lexer::getLocForStartOfLine(SM, R);
}

static void widenOrSet(SourceRange &First, SourceRange Second) {
  if (Second.isInvalid())
    return;
  if (First.isValid()) {
    First.widen(Second);
  } else {
    First = Second;
  }
}

/// \returns true if \c Loc is the location of the first non-comment token on
///   its line.
static bool isFirstTokenOnLine(SourceManager &SM, SourceLoc Loc) {
  assert(Loc.isValid());
  SourceLoc LineStart = Lexer::getLocForStartOfLine(SM, Loc);
  CommentRetentionMode SkipComments = CommentRetentionMode::None;
  Token First = Lexer::getTokenAtLocation(SM, LineStart, SkipComments);
  return First.getLoc() == Loc;
}

/// \returns the location of the first non-whitespace character on the line
///   containing \c Loc.
static SourceLoc
getLocForContentStartOnSameLine(SourceManager &SM, SourceLoc Loc) {
  assert(Loc.isValid());
  SourceLoc LineStart = Lexer::getLocForStartOfLine(SM, Loc);
  StringRef Indentation = Lexer::getIndentationForLine(SM, LineStart);
  return LineStart.getAdvancedLoc(Indentation.size());
}

/// \returns true if the line at \c Loc is either empty or only contains whitespace
static bool isLineAtLocEmpty(SourceManager &SM, SourceLoc Loc) {
  SourceLoc LineStart = Lexer::getLocForStartOfLine(SM, Loc);
  SourceLoc Next = Lexer::getTokenAtLocation(SM, LineStart, CommentRetentionMode::ReturnAsTokens).getLoc();
  return Next.isInvalid() || !isOnSameLine(SM, Loc, Next);
}

/// \returns the first token after the token at \c Loc.
static llvm::Optional<Token> getTokenAfter(SourceManager &SM, SourceLoc Loc,
                                           bool SkipComments = true) {
  assert(Loc.isValid());
  CommentRetentionMode Mode = SkipComments
    ? CommentRetentionMode::None
    : CommentRetentionMode::ReturnAsTokens;
  assert(Lexer::getTokenAtLocation(SM, Loc, Mode).getLoc() == Loc);
  SourceLoc End = Lexer::getLocForEndOfToken(SM, Loc);
  Token Next = Lexer::getTokenAtLocation(SM, End, Mode);
  if (Next.getKind() == tok::NUM_TOKENS)
    return llvm::None;
  return Next;
}

/// \returns the last token of the given kind in the open range between \c From
///   and \c To.
static llvm::Optional<Token> getLastTokenOfKindInOpenRange(SourceManager &SM,
                                                           tok Kind,
                                                           SourceLoc From,
                                                           SourceLoc To) {
  llvm::Optional<Token> Match;
  while (auto Next = getTokenAfter(SM, From)) {
    if (!Next || !SM.isBeforeInBuffer(Next->getLoc(), To))
      break;
    if (Next->getKind() == Kind)
      Match = Next;
    From = Next->getLoc();
  }
  return Match;
}

/// \returns true if the token at \c Loc is one of the given \c Kinds.
static bool locIsKind(SourceManager &SM, SourceLoc Loc, ArrayRef<tok> Kinds) {
  Token Tok = Lexer::getTokenAtLocation(SM, Loc);
  return Tok.getLoc() == Loc &&
      std::find(Kinds.begin(), Kinds.end(), Tok.getKind()) != Kinds.end();
}

/// \returns the given \c Loc if there is a token at that location that is one
///   of the given \c Kinds and an invalid \c SourceLocation otherwise.
static SourceLoc getLocIfKind(SourceManager &SM, SourceLoc Loc,
                              ArrayRef<tok> Kinds) {
  if (!locIsKind(SM, Loc, Kinds))
    return SourceLoc();
  return Loc;
}

/// \returns the given \c Loc if there is a token at that location that is
///   spelled with the given \c Text and an invalid \c SourceLocation otherwise.
static SourceLoc
getLocIfTokenTextMatches(SourceManager &SM, SourceLoc Loc, StringRef Text) {
  Token Tok = Lexer::getTokenAtLocation(SM, Loc);
  if (Tok.getLoc() != Loc || Tok.getKind() == tok::NUM_TOKENS ||
      Tok.getRawText() != Text)
    return SourceLoc();
  return Loc;
}

/// \returns true if the given \c VarDecl has grouping accessor braces containing
///   one or more explicit accessors.
static bool hasExplicitAccessors(VarDecl *VD) {
  auto Getter = VD->getParsedAccessor(AccessorKind::Get);
  SourceRange Braces = VD->getBracesRange();
  return Braces.isValid() && (!Getter ||
                              Getter->getAccessorKeywordLoc().isValid());
}

static ClosureExpr *findTrailingClosureFromArgument(Expr *arg) {
  if (auto TC = dyn_cast_or_null<ClosureExpr>(arg))
    return TC;
  if (auto TCL = dyn_cast_or_null<CaptureListExpr>(arg))
    return dyn_cast_or_null<ClosureExpr>(TCL->getClosureBody());
  return nullptr;
}

static size_t calcVisibleWhitespacePrefix(StringRef Line,
                                          CodeFormatOptions Options) {
  size_t Indent = 0;
  for (auto Char : Line) {
    if (Char == '\t') {
      Indent += Options.TabWidth;
    } else if (Char == ' ' || Char == '\v' || Char == '\f') {
      Indent++;
    } else {
      break;
    }
  }
  return Indent;
}

/// An indentation context of the target location
struct IndentContext {
  enum ContextKind { Exact, LineStart };

  /// The location to indent relative to.
  SourceLoc ContextLoc;

  /// Indicates whether to indent relative to the extact column of ContextLoc
  /// (Exact) or to the start of the content of the line it appears on (LineStart).
  ContextKind Kind;

  /// The number of levels to indent by.
  unsigned IndentLevel;

  IndentContext(SourceLoc Context, bool AddsIndent,
                ContextKind Kind = LineStart)
  : ContextLoc(Context), Kind(Kind), IndentLevel(AddsIndent ? 1 : 0) {
    assert(Context.isValid());
  }
};


/// A helper class used to optionally override the ContextLoc and Kind of an
/// IndentContext.
class ContextOverride {
  struct Override {
    /// The overriding ContextLoc.
    SourceLoc ContextLoc;
    /// The overriding Kind.
    IndentContext::ContextKind Kind;
    /// The location after which this override takes effect.
    SourceLoc ApplicableFrom;
  };

  /// The current override, if set.
  llvm::Optional<Override> Value;

public:
  /// Clears this override.
  void clear() { Value = llvm::None; }

  /// Sets this override to make an IndentContext indent relative to the exact
  /// column of AlignLoc if the IndentContext's ContextLoc is >= AlignLoc and
  /// on the same line.
  void setExact(SourceManager &SM, SourceLoc AlignLoc) {
    Value = {AlignLoc, IndentContext::Exact, AlignLoc};
  }

  /// Sets this override to propagate the given ContextLoc and Kind along to any
  /// IndentContext with a ContextLoc >= L and on the same line. If this
  /// override's existing value applies to the provided ContextLoc, its
  /// ContextLoc and Kind are propagated instead.
  ///
  /// This propagation is necessary for cases like the trailing closure of 'bar'
  /// in the example below. It's direct ContextLoc is 'bar', but we want
  /// it to be 'foo' (the ContextLoc of its parent tuple expression):
  ///
  /// \code
  /// foo(a: 1,
  ///     b: 2)(45, bar(c: 1,
  ///                   d: 2) {
  ///   fatalError()
  /// })
  /// \endcode
  SourceLoc propagateContext(SourceManager &SM, SourceLoc ContextLoc,
                             IndentContext::ContextKind Kind,
                             SourceLoc L, SourceLoc R) {
    // If the range ends on the same line as it starts, we know up front that
    // no child range can span multiple lines, so there's no need to propagate
    // ContextLoc via this override.
    if (R.isValid() && isOnSameLine(SM, L, R))
      return ContextLoc;

    // Similarly if the ContextLoc and L are on the same line, there's no need
    // to propagate. Overrides applicable to ContextLoc will already apply
    // to child ranges on the same line as L.
    if (isOnSameLine(SM, ContextLoc, L))
      return ContextLoc;

    applyIfNeeded(SM, ContextLoc, Kind);
    Value = {ContextLoc, Kind, L};
    return ContextLoc;
  }

  /// Applies the overriding ContextLoc and Kind to the given IndentContext if it
  /// starts after ApplicableFrom and on the same line.
  void applyIfNeeded(SourceManager &SM, IndentContext &Ctx) {
    // Exactly aligned indent contexts should always set a matching exact
    // alignment context override so child braces/parens/brackets are indented
    // correctly. If the given innermost indent context is Exact and the
    // override doesn't match its Kind and ContextLoc, something is wrong.
    assert((Ctx.Kind != IndentContext::Exact ||
            (Value && Value->Kind == IndentContext::Exact &&
             Value->ContextLoc == Ctx.ContextLoc)) &&
           "didn't set override ctx when exact innermost context was set?");

    applyIfNeeded(SM, Ctx.ContextLoc, Ctx.Kind);
  }

  /// Applies the overriding ContextLoc and Kind to the given Override if its
  /// ContextLoc starts after ApplicableFrom and on the same line.
  void applyIfNeeded(SourceManager &SM, SourceLoc &ContextLoc,
                     IndentContext::ContextKind &Kind) {
    if (!isApplicableTo(SM, ContextLoc))
      return;
    ContextLoc = Value->ContextLoc;
    Kind = Value->Kind;
  }

private:
  bool isApplicableTo(SourceManager &SM, SourceLoc Loc) const {
    return Value && isOnSameLine(SM, Loc, Value->ApplicableFrom) &&
        !SM.isBeforeInBuffer(Loc, Value->ApplicableFrom);
  }
};


class FormatContext {
  SourceManager &SM;
  llvm::Optional<IndentContext> InnermostCtx;
  bool InDocCommentBlock;
  bool InCommentLine;

public:
  FormatContext(SourceManager &SM, llvm::Optional<IndentContext> IndentCtx,
                bool InDocCommentBlock = false, bool InCommentLine = false)
      : SM(SM), InnermostCtx(IndentCtx), InDocCommentBlock(InDocCommentBlock),
        InCommentLine(InCommentLine) {}

  bool IsInDocCommentBlock() {
    return InDocCommentBlock;
  }

  bool IsInCommentLine() {
    return InCommentLine;
  }

  void padToExactColumn(StringBuilder &Builder,
                        const CodeFormatOptions &FmtOptions) {
    assert(isExact() && "Context is not exact?");
    SourceLoc AlignLoc = InnermostCtx->ContextLoc;
    CharSourceRange Range(SM, Lexer::getLocForStartOfLine(SM, AlignLoc),
                          AlignLoc);
    unsigned SpaceLength = 0;
    unsigned TabLength = 0;

    // Calculating space length
    for (auto C: Range.str())
      SpaceLength += C == '\t' ? FmtOptions.TabWidth : 1;
    SpaceLength += InnermostCtx->IndentLevel * FmtOptions.TabWidth;

    // If we're indenting past the exact column, round down to the next tab.
    if (InnermostCtx->IndentLevel)
      SpaceLength -= SpaceLength % FmtOptions.TabWidth;

    // If we are using tabs, calculating the number of tabs and spaces we need
    // to insert.
    if (FmtOptions.UseTabs) {
      TabLength = SpaceLength / FmtOptions.TabWidth;
      SpaceLength = SpaceLength % FmtOptions.TabWidth;
    }
    Builder.append(TabLength, '\t');
    Builder.append(SpaceLength, ' ');
  }

  bool isExact() {
    return InnermostCtx.has_value() &&
        InnermostCtx->Kind == IndentContext::Exact;
  }

  std::pair<unsigned, unsigned> indentLineAndColumn() {
    if (InnermostCtx)
      return SM.getLineAndColumnInBuffer(InnermostCtx->ContextLoc);
    return std::make_pair(0, 0);
  }

  bool shouldAddIndentForLine() const {
    return InnermostCtx.has_value() && InnermostCtx->IndentLevel > 0;
  }

  unsigned numIndentLevels() const {
    if (InnermostCtx)
      return InnermostCtx->IndentLevel;
    return 0;
  }
};


/// Recursively strips any trailing arguments, subscripts, generic
/// specializations, or optional bindings from the given expression.
static Expr *getContextExprOf(SourceManager &SM, Expr *E) {
  assert(E);
  if (auto *USE = dyn_cast<UnresolvedSpecializeExpr>(E)) {
    if (auto *Sub = USE->getSubExpr())
      return getContextExprOf(SM, Sub);
  } else if (auto *CE = dyn_cast<CallExpr>(E)) {
    if (auto *Fn = CE->getFn())
      return getContextExprOf(SM, Fn);
  } else if (auto *SE = dyn_cast<SubscriptExpr>(E)) {
    if (auto *B = SE->getBase())
      return getContextExprOf(SM, B);
  } else if (auto *OBE = dyn_cast<BindOptionalExpr>(E)) {
    if (auto *B = OBE->getSubExpr())
      return getContextExprOf(SM, B);
  } else if (auto *PUE = dyn_cast<PostfixUnaryExpr>(E)) {
    if (auto *B = PUE->getOperand())
      return getContextExprOf(SM, B);
  }
  return E;
}

/// Finds the ContextLoc to use for the argument of the given SubscriptExpr,
/// ApplyExpr, or UnresolvedSpecializeExpr. This is needed as the ContextLoc to
/// align their arguments with (including trailing closures) may be neither the
/// start or end of their function or base expression, as in the SubscriptExpr
/// in the example below, where 'select' is the desired ContextLoc to use.
///
/// \code
/// Base()
///   .select(x: 10
///           y: 20)[10] {
///     print($0)
///   }
///   .count
/// \endcode
static SourceLoc getContextLocForArgs(SourceManager &SM, Expr *E) {
  assert(E->getArgs() || isa<UnresolvedSpecializeExpr>(E));
  Expr *Base = getContextExprOf(SM, E);
  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(Base))
    return UDE->getDotLoc();
  if (auto *UDRE = dyn_cast<UnresolvedDeclRefExpr>(Base))
    return UDRE->getLoc();
  return Base->getStartLoc();
}

/// This is a helper class intended to report every pair of matching parens,
/// braces, angle brackets, and square brackets in a given AST node, along with their ContextLoc.
class RangeWalker: protected ASTWalker {
protected:
  SourceManager &SM;

public:
  explicit RangeWalker(SourceManager &SM) : SM(SM) {}

  /// Called for every range bounded by a pair of parens, braces, square
  /// brackets, or angle brackets.
  ///
  /// \returns true to continue walking.
  virtual bool handleRange(SourceLoc L, SourceLoc R, SourceLoc ContextLoc) = 0;

  /// Called for ranges that have a separate ContextLoc but no bounding tokens.
  virtual void handleImplicitRange(SourceRange Range, SourceLoc ContextLoc) = 0;

private:
  bool handleBraces(SourceLoc L, SourceLoc R, SourceLoc ContextLoc) {
    L = getLocIfKind(SM, L, tok::l_brace);
    R = getLocIfKind(SM, R, tok::r_brace);
    return L.isInvalid() || handleRange(L, R, ContextLoc);
  }

  bool handleBraces(SourceRange Braces, SourceLoc ContextLoc) {
    return handleBraces(Braces.Start, Braces.End, ContextLoc);
  }

  bool handleParens(SourceLoc L, SourceLoc R, SourceLoc ContextLoc) {
    L = getLocIfKind(SM, L, tok::l_paren);
    R = getLocIfKind(SM, R, tok::r_paren);
    return L.isInvalid() || handleRange(L, R, ContextLoc);
  }

  bool handleSquares(SourceLoc L, SourceLoc R, SourceLoc ContextLoc) {
    L = getLocIfKind(SM, L, tok::l_square);
    R = getLocIfKind(SM, R, tok::r_square);
    return L.isInvalid() || handleRange(L, R, ContextLoc);
  }

  bool handleAngles(SourceLoc L, SourceLoc R, SourceLoc ContextLoc) {
    L = getLocIfTokenTextMatches(SM, L, "<");
    R = getLocIfTokenTextMatches(SM, R, ">");
    return L.isInvalid() || handleRange(L, R, ContextLoc);
  }

  bool handleBraceStmt(Stmt *S, SourceLoc ContextLoc) {
    if (auto *BS = dyn_cast_or_null<BraceStmt>(S))
      return handleBraces({BS->getLBraceLoc(), BS->getRBraceLoc()}, ContextLoc);
    return true;
  }

  bool walkCustomAttributes(Decl *D) {
    // CustomAttrs of non-param VarDecls are handled when this method is called
    // on their containing PatternBindingDecls (below).
    if (isa<VarDecl>(D) && !isa<ParamDecl>(D))
      return true;

    if (auto *PBD = dyn_cast<PatternBindingDecl>(D)) {
      if (auto *SingleVar = PBD->getSingleVar()) {
        D = SingleVar;
      } else {
        return true;
      }
    }
    for (auto *customAttr : D->getOriginalAttrs().getAttributes<CustomAttr, true>()) {
      if (auto *Repr = customAttr->getTypeRepr()) {
        if (!Repr->walk(*this))
          return false;
      }
      if (auto *Args = customAttr->getArgs()) {
        if (!Args->walk(*this))
          return false;
      }
    }
    return true;
  }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Arguments;
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    if (!walkCustomAttributes(D))
      return Action::SkipChildren();

    if (D->isImplicit())
      return Action::Continue();

    // Walk into inactive config regions.
    if (auto *ICD = dyn_cast<IfConfigDecl>(D)) {
      for (auto Clause : ICD->getClauses()) {
        for (auto Member : Clause.Elements)
          Member.walk(*this);
      }
      return Action::SkipChildren();
    }

    SourceLoc ContextLoc = D->getStartLoc();

    if (auto *GC = D->getAsGenericContext()) {
      // Asking for generic parameters on decls where they are computed, rather
      // than explicitly defined will trigger an assertion when semantic queries
      // and name lookup are disabled.
      bool SafeToAskForGenerics = !isa<ExtensionDecl>(D) &&
        !isa<ProtocolDecl>(D);
      if (SafeToAskForGenerics) {
        if (auto *GP = GC->getParsedGenericParams()) {
          if (!handleAngles(GP->getLAngleLoc(), GP->getRAngleLoc(), ContextLoc))
            return Action::Stop();
        }
      }
    }

    if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
      if (!handleBraces(NTD->getBraces(), ContextLoc))
        return Action::Stop();
    } else if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
      if (!handleBraces(ED->getBraces(), ContextLoc))
        return Action::Stop();
    } else if (auto *VD = dyn_cast<VarDecl>(D)) {
      if (!handleBraces(VD->getBracesRange(), VD->getNameLoc()))
        return Action::Stop();
    } else if (isa<AbstractFunctionDecl>(D) || isa<SubscriptDecl>(D)) {
      if (isa<SubscriptDecl>(D)) {
        if (!handleBraces(cast<SubscriptDecl>(D)->getBracesRange(), ContextLoc))
          return Action::Stop();
      }
      auto *PL = getParameterList(cast<ValueDecl>(D));
      if (!handleParens(PL->getLParenLoc(), PL->getRParenLoc(), ContextLoc))
        return Action::Stop();
    } else if (auto *PGD = dyn_cast<PrecedenceGroupDecl>(D)) {
      SourceRange Braces(PGD->getLBraceLoc(), PGD->getRBraceLoc());
      if (!handleBraces(Braces, ContextLoc))
        return Action::Stop();
    } else if (auto *PDD = dyn_cast<PoundDiagnosticDecl>(D)) {
      // TODO: add paren locations to PoundDiagnosticDecl
    }

    return Action::Continue();
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    if (S->isImplicit())
      return Action::Continue(S);

    if (auto *LCS = dyn_cast<LabeledConditionalStmt>(S)) {
      for (auto &Elem: LCS->getCond()) {
        if (Elem.getKind() == StmtConditionElement::CK_Availability) {
          PoundAvailableInfo *PA = Elem.getAvailability();
          if (!handleParens(PA->getLParenLoc(), PA->getRParenLoc(),
                            PA->getStartLoc()))
            return Action::Stop();
        }
      }
    }

    SourceLoc ContextLoc = S->getStartLoc();
    if (auto *BS = dyn_cast<BraceStmt>(S)) {
      if (!handleBraceStmt(BS, ContextLoc))
        return Action::Stop();
    } else if (auto *IS = dyn_cast<IfStmt>(S)) {
      if (!handleBraceStmt(IS->getThenStmt(), IS->getIfLoc()))
        return Action::Stop();
    } else if (auto *GS = dyn_cast<GuardStmt>(S)) {
      if (!handleBraceStmt(GS->getBody(), GS->getGuardLoc()))
        return Action::Stop();
    } else if (auto *FS = dyn_cast<ForEachStmt>(S)) {
      if (!handleBraceStmt(FS->getBody(), FS->getForLoc()))
        return Action::Stop();
    } else if (auto *SS = dyn_cast<SwitchStmt>(S)) {
      SourceRange Braces(SS->getLBraceLoc(), SS->getRBraceLoc());
      if (!handleBraces(Braces, SS->getSwitchLoc()))
        return Action::Stop();
    } else if (auto *DS = dyn_cast<DoStmt>(S)) {
      if (!handleBraceStmt(DS->getBody(), DS->getDoLoc()))
        return Action::Stop();
    } else if (auto *DCS = dyn_cast<DoCatchStmt>(S)) {
      if (!handleBraceStmt(DCS->getBody(), DCS->getDoLoc()))
        return Action::Stop();
    } else if (isa<CaseStmt>(S) &&
               cast<CaseStmt>(S)->getParentKind() == CaseParentKind::DoCatch) {
      auto CS = cast<CaseStmt>(S);
      if (!handleBraceStmt(CS->getBody(), CS->getLoc()))
        return Action::Stop();
    } else if (auto *RWS = dyn_cast<RepeatWhileStmt>(S)) {
      if (!handleBraceStmt(RWS->getBody(), RWS->getRepeatLoc()))
        return Action::Stop();
    } else if (auto *WS = dyn_cast<WhileStmt>(S)) {
      if (!handleBraceStmt(WS->getBody(), WS->getWhileLoc()))
        return Action::Stop();
    } else if (auto *PAS = dyn_cast<PoundAssertStmt>(S)) {
      // TODO: add paren locations to PoundAssertStmt
    }

    return Action::Continue(S);
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    // Walk through error expressions.
    if (auto *EE = dyn_cast<ErrorExpr>(E)) {
      if (auto *OE = EE->getOriginalExpr()) {
        llvm::SaveAndRestore<ASTWalker::ParentTy>(Parent, EE);
        OE->walk(*this);
      }
      return Action::Continue(E);
    }

    if (E->isImplicit())
      return Action::Continue(E);

    SourceLoc ContextLoc = E->getStartLoc();
    if (auto *PE = dyn_cast<ParenExpr>(E)) {
      SourceLoc L = getLocIfKind(SM, PE->getLParenLoc(),
                                 {tok::l_paren, tok::l_square});
      SourceLoc R = getLocIfKind(SM, PE->getRParenLoc(),
                                 {tok::r_paren, tok::r_square});
      if (L.isValid() && !handleRange(L, R, ContextLoc))
        return Action::Stop();
    } else if (auto *TE = dyn_cast<TupleExpr>(E)) {
      SourceLoc L = getLocIfKind(SM, TE->getLParenLoc(),
                                 {tok::l_paren, tok::l_square});
      SourceLoc R = getLocIfKind(SM, TE->getRParenLoc(),
                                 {tok::r_paren, tok::r_square});
      if (L.isValid() && !handleRange(L, R, ContextLoc))
        return Action::Stop();
    } else if (auto *CE = dyn_cast<CollectionExpr>(E)) {
      if (!handleSquares(CE->getLBracketLoc(), CE->getRBracketLoc(),
                         ContextLoc))
        return Action::Stop();
    } else if (auto *CE = dyn_cast<ClosureExpr>(E)) {
      if (!handleBraceStmt(CE->getBody(), ContextLoc))
        return Action::Stop();
      SourceRange Capture = CE->getBracketRange();
      if (!handleSquares(Capture.Start, Capture.End, Capture.Start))
        return Action::Stop();
      if (auto *PL = CE->getParameters()) {
        if (!handleParens(PL->getLParenLoc(), PL->getRParenLoc(),
                          PL->getStartLoc()))
          return Action::Stop();
      }
    } else if (auto *USE = dyn_cast<UnresolvedSpecializeExpr>(E)) {
      SourceLoc ContextLoc = getContextLocForArgs(SM, E);
      if (!handleAngles(USE->getLAngleLoc(), USE->getRAngleLoc(), ContextLoc))
        return Action::Stop();
    } else if (isa<CallExpr>(E) || isa<SubscriptExpr>(E)) {
      SourceLoc ContextLoc = getContextLocForArgs(SM, E);
      auto *Args = E->getArgs();

      auto lParenLoc = Args->getLParenLoc();
      auto rParenLoc = Args->getRParenLoc();

      if (isa<SubscriptExpr>(E)) {
        if (!handleSquares(lParenLoc, rParenLoc, ContextLoc))
          return Action::Stop();
      } else {
        if (!handleParens(lParenLoc, rParenLoc, ContextLoc))
          return Action::Stop();
      }

      if (Args->hasAnyTrailingClosures()) {
        if (auto *unaryArg = Args->getUnaryExpr()) {
          if (auto CE = findTrailingClosureFromArgument(unaryArg)) {
            if (!handleBraceStmt(CE->getBody(), ContextLoc))
              return Action::Stop();
          }
        } else {
          handleImplicitRange(Args->getOriginalArgs()->getTrailingSourceRange(),
                              ContextLoc);
        }
      }
    }
    return Action::Continue(E);
  }

  PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override {
    if (P->isImplicit())
      return Action::Continue(P);

    if (isa<TuplePattern>(P) || isa<ParenPattern>(P)) {
      if (!handleParens(P->getStartLoc(), P->getEndLoc(), P->getStartLoc()))
        return Action::Stop();
    }

    return Action::Continue(P);
  }

  PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
    if (auto *TT = dyn_cast<TupleTypeRepr>(T)) {
      SourceRange Parens = TT->getParens();
      if (!handleParens(Parens.Start, Parens.End, Parens.Start))
        return Action::Stop();
    } else if (isa<ArrayTypeRepr>(T) || isa<DictionaryTypeRepr>(T)) {
      if (!handleSquares(T->getStartLoc(), T->getEndLoc(), T->getStartLoc()))
        return Action::Stop();
    } else if (auto *GI = dyn_cast<GenericIdentTypeRepr>(T)) {
      SourceLoc ContextLoc = GI->getNameLoc().getBaseNameLoc();
      SourceRange Brackets = GI->getAngleBrackets();
      if (!handleAngles(Brackets.Start, Brackets.End, ContextLoc))
        return Action::Stop();
    }
    return Action::Continue();
  }

  bool shouldWalkIntoGenericParams() override { return true; }
};

/// Indicates whether a range is an open or closed range.
enum class RangeKind {Closed, Open};

/// A helper class that determines whether a given node, or subrage of a node
/// should indent or not when it spans multiple lines.
class OutdentChecker: protected RangeWalker {
  SourceRange CheckRange; ///< The source range to consider.
  RangeKind CheckRangeKind; ///< Whether \c CheckRange is open or closed.
  bool IsOutdenting = false; ///< Tracks whether a seen range prevents indenting.
  llvm::DenseMap<SourceLoc, ContextOverride> LineStartToOverride;

  explicit OutdentChecker(SourceManager &SM,
                          SourceRange CheckRange,  RangeKind CheckRangeKind)
  : RangeWalker(SM), CheckRange(CheckRange), CheckRangeKind(CheckRangeKind) {
    assert(CheckRange.isValid());
  }

  void handleImplicitRange(SourceRange Range, SourceLoc ContextLoc) override {
    assert(Range.isValid() && ContextLoc.isValid());

    // Ignore ranges outside of the open/closed check range.
    if (!isInCheckRange(Range.Start, Range.End))
      return;

    propagateContextLocs(ContextLoc, Range.Start, Range.End);
  }

  bool handleRange(SourceLoc L, SourceLoc R, SourceLoc ContextLoc) override {
    assert(L.isValid() && ContextLoc.isValid());

    // Ignore parens/braces/brackets outside of the open/closed check range.
    if (!isInCheckRange(L, R))
      return true;

    // The CheckRange is made outdenting by any parens/braces/brackets with:
    // 1) a ContextLoc starts on the same line as the start of CheckRange, and
    // 2) either:
    //   a) an R token that starts its containing line, or
    //   b) an L token that isn't the ContextLoc and starts its containing line.
    //
    // E.g. for an open CheckRange covering the contents of an array literal
    // with various line bresk positions:
    //
    //   // This doesn't outdent because:
    //   [ // The array brackets are outside the open CheckRange so ignored.
    //     (1, (2, 3)), // both parens fail conditions 1 and 2.
    //     (4, (5, 6))  // both parens fail conditions 1 and 2.
    //   ]
    //
    //   // This doesn't outdent because:
    //   [(1, (2, 3)), // both parens fail condition 2.
    //      ( // these parens fail condition 1.
    //        4, (5, 6) // these parens fail conditions 1 and 2.
    //      )]
    //
    //   This outdents because:
    //   [( // These parens meet conditions 1 and 2a.
    //     1, (2, 3)
    //   ), (
    //     4, (5, 6)
    //   )]
    //
    //   This outdents because:
    //   [(1, ( // The inner parens meet conditions 1 and 2a.
    //     2, 3
    //   ), (4, (5, 6)]
    //
    //   This outdents because:
    //   [(1, (2, 3), ( // The inner parens meet conditions 1 and 2a.
    //     4, (5, 6)
    //   )]
    //
    // For a closed CheckRange covering the variable declaration below:
    //
    //   This doesn't outdent because:
    //   var x = foo(1) { // The parens and braces fail condition 2
    //     return 42 }
    //
    //   This outdents because:
    //   var x = foo(1) { // These braces meet conditions 1 and 2a.
    //      return 42
    //   }
    //
    //   This outdents because:
    //   var x = foo(1)
    //   { // These braces meet conditions 1 and 2b (their ContextLoc is 'foo').
    //      return 42
    //   }
    //
    // And for a closed CheckRange covering the call expression below:
    //
    //   This doesn't outdent because:
    //   foo(1, // These parens fail condition 2.
    //     2, 3) { return 42 } // These braces fail condition 1 and 2.
    //
    //   This outdents because:
    //   foo(1, 2, 3)
    //   { // These braces meet conditions 1 and 2b (their ContextLoc is 'foo').
    //     return 42
    //   }

    // The above conditions are not sufficient to handle cases like the below,
    // which we would like to be considered outdenting:
    //   foo(a: 1,
    //       b: 2)[x: bar { // These braces fail condition 1.
    //       return 42
    //   }]
    // To handle them, we propagate the ContextLoc of each parent range down to
    // any child ranges that start on the same line as the parent. The braces
    // above then 'inherit' the ContextLoc of their parent brackets ('foo'), and
    // pass condition 1.
    ContextLoc = propagateContextLocs(ContextLoc, L, R);

    // Ignore parens/braces/brackets that fail Condition 1.
    if (!isOnSameLine(SM, ContextLoc, CheckRange.Start))
      return true;

    // Ignore parens/braces/brackets that can't meet Condition 2.
    if (R.isValid() && isOnSameLine(SM, ContextLoc, R))
      return true;

    // Check condition 2b.
    if (ContextLoc != L && isFirstTokenOnLine(SM, L)) {
      IsOutdenting = true;
    } else if (R.isValid()) {
      // Check condition 2a.
      SourceLoc LineStart = Lexer::getLocForStartOfLine(SM, R);
      Token First = Lexer::getTokenAtLocation(SM, LineStart,
                                              CommentRetentionMode::None);
      IsOutdenting |= First.getLoc() == R;
    }

    // We only need to continue checking if it's not already outdenting.
    return !IsOutdenting;
  }

  SourceLoc propagateContextLocs(SourceLoc ContextLoc, SourceLoc L, SourceLoc R) {
    bool HasSeparateContext = !isOnSameLine(SM, L, ContextLoc);

    // Update ContextLoc for the currently active override on its line.
    ContextOverride &Upstream = getOverrideForLineContaining(ContextLoc);
    IndentContext::ContextKind Kind = IndentContext::LineStart;
    Upstream.applyIfNeeded(SM, ContextLoc, Kind);

    // If the original ContextLoc and L were on the same line, there's no need
    // to propagate anything. Child ranges later on the same line will pick up
    // whatever override we picked up above anyway, and if there wasn't
    // one, their normal ContextLoc should already be correct.
    if (!HasSeparateContext)
      return ContextLoc;

     // Set an override to propagate the context loc onto the line of L.
    ContextOverride &Downstream = getOverrideForLineContaining(L);
    ContextLoc = Downstream.propagateContext(SM, ContextLoc,
                                             Kind, L, R);
    return ContextLoc;
  }

  bool isInCheckRange(SourceLoc L, SourceLoc R) const {
    switch (CheckRangeKind) {
    case RangeKind::Open:
      return SM.isBeforeInBuffer(CheckRange.Start, L) &&
      (R.isInvalid() || SM.isBeforeInBuffer(R, CheckRange.End));
    case RangeKind::Closed:
      return !SM.isBeforeInBuffer(L, CheckRange.Start) &&
        (R.isInvalid() || !SM.isBeforeInBuffer(CheckRange.End, R));
    }
    llvm_unreachable("invalid range kind");
  }

public:
  /// Checks if a source range shouldn't indent when it crosses multiple lines.
  ///
  /// \param SM
  ///   The SourceManager managing the given source range.
  /// \param Range
  ///   The range to check.
  /// \param WalkableParent
  ///   A parent AST node that when walked covers all relevant nodes in the
  ///   given source range.
  /// \param RangeKind
  ///   Whether the given range to check is closed (the default) or open.
  template <typename T>
  static bool hasOutdent(SourceManager &SM, SourceRange Range, T *WalkableParent,
                         RangeKind RangeKind = RangeKind::Closed) {
    assert(Range.isValid());
    if (isOnSameLine(SM, Range.Start, Range.End))
      return false;
    OutdentChecker Checker(SM, Range, RangeKind);
    WalkableParent->walk(Checker);
    return Checker.IsOutdenting;
  }

  /// Checks if an AST node shouldn't indent when it crosses multiple lines.
  ///
  /// \param SM
  ///   The SourceManager managing the given source range.
  /// \param WalkableNode
  ///   The AST node to check.
  /// \param RangeKind
  ///   Whether to check the source range of \c WalkableNode as a closed (the
  ///   default) or open range.
  template <typename T>
  static bool hasOutdent(SourceManager &SM, T *WalkableNode,
                         RangeKind RangeKind = RangeKind::Closed) {
    return hasOutdent(SM, WalkableNode->getSourceRange(), WalkableNode,
                      RangeKind);
  }

private:
  ContextOverride &getOverrideForLineContaining(SourceLoc Loc) {
    SourceLoc LineStart = Lexer::getLocForStartOfLine(SM, Loc);
    auto Ret = LineStartToOverride.insert({LineStart, ContextOverride()});
    return Ret.first->getSecond();
  }
};


/// Information about an indent target that immediately follows a node being walked by
/// a \c FormatWalker instance, or optionally, that follows a trailing comma
/// after such a node.
class TrailingInfo {
  llvm::Optional<Token> TrailingToken;
  TrailingInfo(llvm::Optional<Token> TrailingToken)
      : TrailingToken(TrailingToken) {}

public:
  /// Whether the trailing target is on an empty line.
  bool isEmpty() const { return !TrailingToken.has_value(); }

  /// Whether the trailing target is a token with one of the given kinds.
  bool hasKind(ArrayRef<tok> Kinds) const {
    if (TrailingToken) {
      tok Kind = TrailingToken->getKind();
      return std::find(Kinds.begin(), Kinds.end(), Kind) != Kinds.end();
    }
    return false;
  }

  /// Checks if the target location immediately follows the provided \p EndLoc,
  /// optionally allowing for a single comma in between.
  static llvm::Optional<TrailingInfo> find(SourceManager &SM, SourceLoc EndLoc,
                                           SourceLoc TargetLoc,
                                           bool LookPastTrailingComma = true) {
    // If the target is before the end of the end token, it's not trailing.
    SourceLoc TokenEndLoc = Lexer::getLocForEndOfToken(SM, EndLoc);
    if (SM.isBeforeInBuffer(TargetLoc, TokenEndLoc))
      return llvm::None;

    // If there is no next token, the target directly trails the end token.
    auto Next = getTokenAfter(SM, EndLoc, /*SkipComments=*/false);
    if (!Next)
      return TrailingInfo{llvm::None};

    // If the target is before or at the next token's locations, it directly
    // trails the end token.
    SourceLoc NextTokLoc = Next->getLoc();
    if (NextTokLoc == TargetLoc)
      return TrailingInfo {Next};
    if (SM.isBeforeInBuffer(TargetLoc, Next->getLoc()))
      return TrailingInfo{llvm::None};

    // The target does not directly trail the end token. If we should look past
    // trailing commas, do so.
    if (LookPastTrailingComma && Next->getKind() == tok::comma)
      return find(SM, Next->getLoc(), TargetLoc, false);

    return llvm::None;
  }
};


/// A helper class for aligning list elements and their bounding tokens.
class ListAligner {
  SourceManager &SM;
  SourceLoc TargetLoc; ///< The indent location.
  SourceLoc ContextLoc; ///< The owning indent context's location.
  SourceLoc IntroducerLoc; ///< The opening token before the first list element.
  SourceLoc CloseLoc; ///< The token that closes the list (optional).
  bool CloseRequired; ///< Whether a closing token is expected.
  bool AllowsTrailingSeparator; ///< Whether a final trailing comma is legal.
  bool ElementExpected; ///<Whether at least one element is expected.

  SourceLoc AlignLoc;
  SourceLoc LastEndLoc;
  bool HasOutdent = false;
  bool BreakAlignment = false;

public:

  /// Don't column-align if any element starts on the same line as IntroducerLoc
  /// but ends on a later line.
  bool BreakAlignmentIfSpanning = false;

  /// Constructs a new \c ListAligner for a list bounded by separate opening and
  /// closing tokens, e.g. tuples, array literals, parameter lists, etc.
  ///
  /// \param SM
  ///    The source manager to use.
  /// \param TargetLoc
  ///    The indent target location.
  /// \param ContextLoc
  ///    The location list items should indent relative to.
  /// \param L
  ///    The location of the token before the first item in the list, e.g. '(',
  ///    or \c case.
  /// \param R
  ///    The location of the closing token of the list, e.g. ')', if present.
  /// \param AllowsTrailingSeparator
  ///    Whether a trailing comma is legal, or indicates an incomplete list.
  ListAligner(SourceManager &SM, SourceLoc TargetLoc, SourceLoc ContextLoc,
              SourceLoc L, SourceLoc R, bool AllowsTrailingSeparator = false)
  : SM(SM), TargetLoc(TargetLoc), ContextLoc(ContextLoc),
    IntroducerLoc(L), CloseLoc(R), CloseRequired(true),
    AllowsTrailingSeparator(AllowsTrailingSeparator), ElementExpected(true) {
      assert(ContextLoc.isValid() && IntroducerLoc.isValid());
    }

  /// Constructs a new \c ListAligner for a list with only an introducing token,
  /// e.g. enum case element lists, guard/if condition pattern lists, and
  /// pattern binding declaration lists.
  ///
  /// \param SM
  ///    The source manager to use.
  /// \param TargetLoc
  ///    The indent target location.
  /// \param ContextLoc
  ///    The location list items should indent relative to.
  /// \param IntroducerLoc
  ///    The location of the token before the first item in the list, e.g. '(',
  ///    or \c case.
  /// \param ElementExpected
  ///    Whether at least one item is expected. Currently not true of 'catch'
  ///    pattern lists only.
  ListAligner(SourceManager &SM, SourceLoc TargetLoc, SourceLoc ContextLoc,
              SourceLoc IntroducerLoc, bool ElementExpected = true)
  : SM(SM), TargetLoc(TargetLoc), ContextLoc(ContextLoc),
    IntroducerLoc(IntroducerLoc), CloseLoc(SourceLoc()), CloseRequired(false),
    AllowsTrailingSeparator(false), ElementExpected(ElementExpected) {
      assert(ContextLoc.isValid() && IntroducerLoc.isValid());
    }

  /// Update the alignment for a list element.
  ///
  /// \param Start
  ///   The start location of the element.
  /// \param End
  ///   The end location of the element
  /// \param WalkableParent
  ///   An AST node that is, or contains the element, and is walkable.
  template <typename T>
  void updateAlignment(SourceLoc Start, SourceLoc End, T *WalkableParent) {
    updateAlignment(SourceRange(Start, End), WalkableParent);
  }

  /// Update the alignment for a list element.
  ///
  /// \param Range
  ///   The source range of the element.
  /// \param WalkableParent
  ///   An AST node that is, or contains the element, and is walkable.
  template <typename T>
  void updateAlignment(SourceRange Range, T *WalkableParent) {
    assert(Range.isValid());
    LastEndLoc = Range.End;

    if (isOnSameLine(SM, IntroducerLoc, Range.Start)) {
      HasOutdent |= OutdentChecker::hasOutdent(SM, Range, WalkableParent);
      if (BreakAlignmentIfSpanning)
        BreakAlignment |= !isOnSameLine(SM, IntroducerLoc, Range.End);
    }

    if (HasOutdent || !SM.isBeforeInBuffer(Range.Start, TargetLoc))
      return;
    if (AlignLoc.isValid()) {
      if (isOnSameLine(SM, Range.Start, AlignLoc) ||
          !isFirstTokenOnLine(SM, Range.Start))
        return;
      AlignLoc = getLocForContentStartOnSameLine(SM, Range.Start);
    } else if (isOnSameLine(SM, IntroducerLoc, Range.Start)) {
      AlignLoc = Range.Start;
    }
  }

  /// Returns the list's IndentContext (if applicable to the TargetLoc) and sets
  /// an exact alignment override if needed.
  ///
  /// \note This should only be called after calling \c updateAlignment on every
  ///   element range.
  /// \param Override
  ///   A ContextOverride object to set
  llvm::Optional<IndentContext>
  getContextAndSetAlignment(ContextOverride &Override) {
    // If the target is before the introducer token, or on it and it is also
    // the context loc, the list shouldn't be an indent context.
    if (SM.isBeforeInBuffer(TargetLoc, IntroducerLoc))
      return llvm::None;
    if (TargetLoc == IntroducerLoc && ContextLoc == IntroducerLoc)
      return llvm::None;

    // Get the end location of the (possibly incomplete) list.
    bool HasTrailingComma = false;
    SourceLoc End = getEffectiveEndLoc(HasTrailingComma);
    assert(End.isValid());

    // If the target is past the end of the list we may still be an indent
    // context.
    bool TargetIsTrailing = false;
    if (!SM.isBeforeInBuffer(TargetLoc, Lexer::getLocForEndOfToken(SM, End))) {
      // If the close token is present, we're not.
      if (CloseLoc.isValid())
        return llvm::None;

      // If there's no trailing comma and a close token isn't required, we're
      // only a context if there no elements yet but at least one is expected,
      // e.g. in an if condition list.
      if (!HasTrailingComma && !CloseRequired &&
          (LastEndLoc.isValid() || !ElementExpected))
        return llvm::None;

      // If the target isn't immediately trailing the end loc, we're not.
      if (!TrailingInfo::find(SM, End, TargetLoc,
                          /*LookPastTrailingComma=*/!HasTrailingComma)) {
        return llvm::None;
      }
      TargetIsTrailing = true;
    }

    bool ShouldIndent = shouldIndent(HasTrailingComma, TargetIsTrailing);
    if (ShouldIndent && !BreakAlignment && AlignLoc.isValid()) {
      setAlignmentIfNeeded(Override);
      return IndentContext {AlignLoc, false, IndentContext::Exact};
    }
    return IndentContext {ContextLoc, ShouldIndent};
  }

  /// Sets an exact alignment override for child indent contexts, if needed.
  ///
  /// This should be called before returning an \c IndentContext for a subrange
  /// of the list.
  void setAlignmentIfNeeded(ContextOverride &Override) {
    if (HasOutdent || BreakAlignment || AlignLoc.isInvalid())
      return;
    Override.setExact(SM, AlignLoc);
  }

private:
  bool shouldIndent(bool HasTrailingComma, bool TargetIsTrailing) const {
    if (HasOutdent || TargetLoc == IntroducerLoc)
      return false;
    if (TargetLoc == CloseLoc)
      return !AllowsTrailingSeparator && HasTrailingComma;
    if (TargetIsTrailing) {
      return  CloseLoc.isInvalid() &&
        ((LastEndLoc.isInvalid() && ElementExpected) ||
         HasTrailingComma);
    }
    return true;
  }

  SourceLoc getEffectiveEndLoc(bool &HasTrailingComma) const {
    if (LastEndLoc.isInvalid())
      return CloseLoc.isValid() ? CloseLoc : IntroducerLoc;

    SourceLoc EffectiveEnd = LastEndLoc;
    if (locIsKind(SM, LastEndLoc, tok::comma)) {
      HasTrailingComma = true;
    } else {
      llvm::Optional<Token> AfterLast = getTokenAfter(SM, LastEndLoc);
      if (AfterLast && AfterLast->is(tok::comma)) {
        HasTrailingComma = true;
        EffectiveEnd = AfterLast->getLoc();
      }
    }

    if (CloseLoc.isValid())
      return CloseLoc;
    return EffectiveEnd;
  }
};


/// Walks an AST Node to determine the \c FormatContext of the target indent location.
///
/// It only walks into nodes whose source range overlaps, or immediately
/// precedes the target indent location.
class FormatWalker : public ASTWalker {
  SourceFile &SF;
  SourceManager &SM;
  CodeFormatOptions &FmtOptions;
  ArrayRef<Token> TokenList;

  SourceLoc TargetLocation;
  SourceLoc TargetLineLoc;
  llvm::SmallPtrSet<void *, 16> NodesToSkip;
  ArrayRef<Token>::iterator CurrentTokIt;

  /// The innermost indent context of the target location.
  llvm::Optional<IndentContext> InnermostCtx;
  /// A conditionally applicable indent context override.
  ContextOverride CtxOverride;
  /// Whether the target location appears within a doc comment block.
  bool InDocCommentBlock = false;
  /// Whether the target location appears within a line comment.
  bool InCommentLine = false;

  /// The range of the string literal the target is inside of (if any, invalid otherwise).
  CharSourceRange StringLiteralRange;

public:
  explicit FormatWalker(SourceFile &SF, SourceManager &SM, CodeFormatOptions &Options)
  : SF(SF), SM(SM), FmtOptions(Options), TokenList(SF.getAllTokens()),
    CurrentTokIt(TokenList.begin()) {}

  /// Compute the \c FormatContext of the given source location.
  ///
  /// \note The given location should point to the content start of its line.
  FormatContext walkToLocation(SourceLoc Loc) {
    InnermostCtx = llvm::None;
    CtxOverride.clear();
    TargetLocation = Loc;
    TargetLineLoc = Lexer::getLocForStartOfLine(SM, TargetLocation);
    InDocCommentBlock = InCommentLine = false;
    StringLiteralRange = CharSourceRange();
    NodesToSkip.clear();
    CurrentTokIt = TokenList.begin();

    SF.walk(*this);
    scanTokensUntil(SourceLoc());

    if (InnermostCtx)
      CtxOverride.applyIfNeeded(SM, *InnermostCtx);

    if (StringLiteralRange.isValid()) {
      assert(!InDocCommentBlock && !InCommentLine &&
             "Target is in both a string and comment");
      InnermostCtx = indentWithinStringLiteral();
    } else {
      assert(!InDocCommentBlock || !InCommentLine &&
             "Target is in both a doc comment block and comment line");
    }

    return FormatContext(SM, InnermostCtx, InDocCommentBlock,
                         InCommentLine);
  }

private:
  llvm::Optional<IndentContext> indentWithinStringLiteral() {
    assert(StringLiteralRange.isValid() && "Target is not within a string literal");

    // This isn't ideal since if the user types """""" and then an enter
    // inside after, we won't indent the end quotes. But indenting the end
    // quotes could lead to an error in the rest of the string, so best to
    // avoid it entirely for now.
    if (isOnSameLine(SM, TargetLineLoc, StringLiteralRange.getEnd()))
      return IndentContext {TargetLocation, false, IndentContext::Exact};

    // If there's contents before the end quotes then it's likely the quotes
    // are actually the start quotes of the next string in the file. Pretend
    // they don't exist so their indent doesn't affect the indenting.
    SourceLoc EndLineContentLoc =
        getLocForContentStartOnSameLine(SM, StringLiteralRange.getEnd());
    bool HaveEndQuotes = CharSourceRange(SM, EndLineContentLoc,
                                         StringLiteralRange.getEnd())
        .str().equals(StringRef("\"\"\""));

    if (!HaveEndQuotes) {
      // Indent to the same indentation level as the first non-empty line
      // before the target. If that line is the start line then either use
      // the same indentation of the start quotes if they are on their own
      // line, or an extra indentation otherwise.
      //
      // This will indent lines with content on it as well, which should be
      // fine since it is quite unlikely anyone would format a range that
      // includes an unterminated string.

      SourceLoc AlignLoc = TargetLineLoc;
      while (SM.isBeforeInBuffer(StringLiteralRange.getStart(), AlignLoc)) {
        AlignLoc = Lexer::getLocForStartOfLine(SM, AlignLoc.getAdvancedLoc(-1));
        if (!isLineAtLocEmpty(SM, AlignLoc)) {
          AlignLoc = getLocForContentStartOnSameLine(SM, AlignLoc);
          break;
        }
      }

      if (isOnSameLine(SM, AlignLoc, StringLiteralRange.getStart())) {
        SourceLoc StartLineContentLoc =
            getLocForContentStartOnSameLine(SM, StringLiteralRange.getStart());
        bool StartLineOnlyQuotes = CharSourceRange(SM, StartLineContentLoc,
                                                   StringLiteralRange.getEnd())
            .str().startswith(StringRef("\"\"\""));
        if (!StartLineOnlyQuotes)
          return IndentContext {StringLiteralRange.getStart(), true};

        AlignLoc = StringLiteralRange.getStart();
      }

      return IndentContext {AlignLoc, false, IndentContext::Exact};
    }

    // If there are end quotes, only enforce a minimum indentation. We don't
    // want to add any other indentation since that could add unintended
    // whitespace to existing strings. Could change this if the full range
    // was passed rather than a single line - in that case we *would* indent
    // if the range was a single empty line.

    CharSourceRange TargetIndentRange =
        CharSourceRange(SM, Lexer::getLocForStartOfLine(SM, TargetLocation),
                        TargetLocation);
    CharSourceRange EndIndentRange =
        CharSourceRange(SM, Lexer::getLocForStartOfLine(SM, EndLineContentLoc),
                        EndLineContentLoc);

    size_t TargetIndent =
        calcVisibleWhitespacePrefix(TargetIndentRange.str(), FmtOptions);
    size_t EndIndent =
        calcVisibleWhitespacePrefix(EndIndentRange.str(), FmtOptions);
    if (TargetIndent >= EndIndent)
      return IndentContext {TargetLocation, false, IndentContext::Exact};

    return IndentContext {EndLineContentLoc, false, IndentContext::Exact};
  }

#pragma mark ASTWalker overrides and helpers

  bool walkCustomAttributes(Decl *D) {
    // CustomAttrs of non-param VarDecls are handled when this method is called
    // on their containing PatternBindingDecls (below).
    if (isa<VarDecl>(D) && !isa<ParamDecl>(D))
      return true;

    if (auto *PBD = dyn_cast<PatternBindingDecl>(D)) {
      if (auto *SingleVar = PBD->getSingleVar()) {
        D = SingleVar;
      } else {
        return true;
      }
    }
    for (auto *customAttr : D->getOriginalAttrs().getAttributes<CustomAttr, true>()) {
      if (auto *Repr = customAttr->getTypeRepr()) {
        if (!Repr->walk(*this))
          return false;
      }
      if (auto *Args = customAttr->getArgs()) {
        if (!Args->walk(*this))
          return false;
      }
    }
    return true;
  }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Arguments;
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    if (!walkCustomAttributes(D))
      return Action::SkipChildren();

    auto Action = HandlePre(D, D->isImplicit());
    if (Action.shouldGenerateIndentContext()) {
      if (auto IndentCtx = getIndentContextFrom(D, Action.Trailing))
        InnermostCtx = IndentCtx;
    }

    // Walk accessors via their pattern binding decl. They aren't walked via
    // their VarDecls due to their non-overlapping range, so they'd be skipped
    // otherwise.
    if (auto *PBD = dyn_cast<PatternBindingDecl>(D)) {
      if (Action.shouldVisitChildren()) {
        for (auto I: range(PBD->getNumPatternEntries())) {
          auto *P = PBD->getPattern(I);
          if (!P)
            continue;
          bool Cancelled = false;
          P->forEachVariable([&](VarDecl *VD) {
            if (Cancelled || VD->getBracesRange().isInvalid())
              return;
            for (auto *AD : VD->getAllAccessors()) {
              if (AD->walk(*this)) {
                Cancelled = true;
                break;
              }
            }
          });
        }
      }
    }

    // Walk into inactive config regions.
    if (auto *ICD = dyn_cast<IfConfigDecl>(D)) {
      if (Action.shouldVisitChildren()) {
        for (auto Clause : ICD->getClauses()) {
          for (auto Member : Clause.Elements)
            Member.walk(*this);
        }
      }
      return Action::SkipChildren();
    }

    return Action::VisitChildrenIf(Action.shouldVisitChildren());
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    auto Action = HandlePre(S, S->isImplicit());
    if (Action.shouldGenerateIndentContext()) {
      if (auto IndentCtx = getIndentContextFrom(S, Action.Trailing))
        InnermostCtx = IndentCtx;
    }
    return Action::VisitChildrenIf(Action.shouldVisitChildren(), S);
  }

  PreWalkResult<ArgumentList *>
  walkToArgumentListPre(ArgumentList *Args) override {
    SourceLoc ContextLoc;
    if (auto *E = Parent.getAsExpr()) {
      // Retrieve the context loc from the parent call. Note that we don't
      // perform this if we're walking the ArgumentList directly for e.g an
      // interpolated string literal.
      if (E->getArgs() == Args)
        ContextLoc = getContextLocForArgs(SM, E);
    }

    auto Action = HandlePre(Args, Args->isImplicit());
    if (Action.shouldGenerateIndentContext()) {
      if (auto Ctx = getIndentContextFrom(Args, Action.Trailing, ContextLoc))
        InnermostCtx = Ctx;
    }
    return Action::VisitChildrenIf(Action.shouldVisitChildren(), Args);
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (E->getKind() == ExprKind::StringLiteral &&
        SM.isBeforeInBuffer(E->getStartLoc(), TargetLocation) &&
        SM.isBeforeInBuffer(TargetLocation,
                            Lexer::getLocForEndOfToken(SM, E->getEndLoc()))) {
      StringLiteralRange = Lexer::getCharSourceRangeFromSourceRange(SM, E->getSourceRange());
    }

    // Create a default indent context for all top-level expressions
    if (isStatementListItem()) {
      SourceRange Range = E->getSourceRange();
      if (Range.isValid() && isTargetContext(Range)) {
        InnermostCtx = IndentContext {
          E->getStartLoc(),
          !OutdentChecker::hasOutdent(SM, E)
        };
      }
    }

    auto Action = HandlePre(E, E->isImplicit());
    if (Action.shouldGenerateIndentContext()) {
      if (auto IndentCtx = getIndentContextFrom(E, Action.Trailing))
        InnermostCtx = IndentCtx;
    }

    // Don't visit the child expressions of interpolated strings directly -
    // visit only the argument of each appendInterpolation call instead, and
    // set StringLiteralRange if needed for each segment.
    if (auto *ISL = dyn_cast<InterpolatedStringLiteralExpr>(E)) {
      if (Action.shouldVisitChildren()) {
        llvm::SaveAndRestore<ASTWalker::ParentTy>(Parent, ISL);
        SourceLoc PrevStringStart = ISL->getStartLoc();
        ISL->forEachSegment(SF.getASTContext(),
                            [&](bool IsInterpolation, CallExpr *CE) {
          if (IsInterpolation) {
            // Handle the preceeding string segment.
            CharSourceRange StringRange(SM, PrevStringStart, CE->getStartLoc());
            if (StringRange.contains(TargetLocation)) {
              StringLiteralRange =
                  Lexer::getCharSourceRangeFromSourceRange(SM, E->getSourceRange());
              return;
            }
            // Walk into the interpolation segment.
            CE->getArgs()->walk(*this);
          } else {
            PrevStringStart = CE->getStartLoc();
          }
        });
        // Handle the trailing string segment.
        SourceLoc End = Lexer::getLocForEndOfToken(SM, ISL->getStartLoc());
        CharSourceRange StringRange(SM, PrevStringStart, End);
        if (StringRange.contains(TargetLocation))
          StringLiteralRange =
              Lexer::getCharSourceRangeFromSourceRange(SM, E->getSourceRange());

        return Action::SkipChildren(E);
      }
    }

    // Walk through error expressions.
    if (auto *EE = dyn_cast<ErrorExpr>(E)) {
      if (Action.shouldVisitChildren()) {
        if (auto *OE = EE->getOriginalExpr()) {
          llvm::SaveAndRestore<ASTWalker::ParentTy>(Parent, EE);
          OE->walk(*this);
        }
        return Action::SkipChildren(E);
      }
    }

    return Action::VisitChildrenIf(Action.shouldVisitChildren(), E);
  }

  PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override {
    auto Action = HandlePre(P, P->isImplicit());
    if (Action.shouldGenerateIndentContext()) {
      if (auto IndentCtx = getIndentContextFrom(P, Action.Trailing))
        InnermostCtx = IndentCtx;
    }
    return Action::VisitChildrenIf(Action.shouldVisitChildren(), P);
  }

  PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
    auto Action = HandlePre(T, false);
    if (Action.shouldGenerateIndentContext()) {
      if (auto IndentCtx = getIndentContextFrom(T, Action.Trailing))
        InnermostCtx = IndentCtx;
    }
    return Action::VisitChildrenIf(Action.shouldVisitChildren());
  }

  PostWalkAction walkToDeclPost(Decl *D) override {
    return HandlePost(D).Action;
  }
  PostWalkAction walkToTypeReprPost(TypeRepr *T) override {
    return HandlePost(T).Action;
  }

  PostWalkResult<Stmt *> walkToStmtPost(Stmt *S) override {
    return HandlePost(S);
  }

  PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
    return HandlePost(E);
  }

  PostWalkResult<Pattern *> walkToPatternPost(Pattern *P) override {
    return HandlePost(P);
  }

  bool shouldWalkIntoGenericParams() override { return true; }


#pragma mark Visitation helpers

  struct VisitAction {
    enum : unsigned { Skip, VisitChildren, GetContext } action;
    llvm::Optional<TrailingInfo> Trailing;

    bool shouldVisitChildren() const { return action >= VisitChildren; }
    bool shouldGenerateIndentContext() const { return action >= GetContext; }
  };

  template <class T>
  VisitAction HandlePre(T* Node, bool IsImplicit) {
    SourceLoc Start = Node->getStartLoc(), End = Node->getEndLoc();

    if (Start.isInvalid())
      return {VisitAction::VisitChildren, llvm::None};

    llvm::Optional<TrailingInfo> Trailing =
        TrailingInfo::find(SM, End, TargetLocation);
    scanTokensUntil(Start);

    if (!isTargetContext(Start, End) && !Trailing)
      return {VisitAction::Skip, llvm::None};
    if (!NodesToSkip.count(Node) && !IsImplicit)
      return {VisitAction::GetContext, Trailing};
    return {VisitAction::VisitChildren, Trailing};
  }

  template <typename T>
  PostWalkResult<T *> HandlePost(T *Node) {
    if (SM.isBeforeInBuffer(TargetLocation, Node->getStartLoc()))
      return Action::Stop();

    return Action::Continue(Node);
  }

  void scanTokensUntil(SourceLoc Loc) {
    if (InDocCommentBlock || InCommentLine || StringLiteralRange.isValid())
      return;
    for (auto Invalid = Loc.isInvalid(); CurrentTokIt != TokenList.end() &&
         (Invalid || SM.isBeforeInBuffer(CurrentTokIt->getLoc(), Loc));
         ++CurrentTokIt) {
      if (CurrentTokIt->getKind() == tok::comment) {
        CharSourceRange CommentRange = CurrentTokIt->getRange();
        SourceLoc StartLineLoc = Lexer::getLocForStartOfLine(
            SM, CommentRange.getStart());

        SourceLoc EndLineLoc = Lexer::getLocForStartOfLine(
            SM, CommentRange.getEnd());
        auto TokenStr = CurrentTokIt->getRange().str();
        InDocCommentBlock |= SM.isBeforeInBuffer(StartLineLoc, TargetLineLoc) && !SM.isBeforeInBuffer(EndLineLoc, TargetLineLoc) &&
            TokenStr.startswith("/*");
        InCommentLine |= StartLineLoc == TargetLineLoc &&
            TokenStr.startswith("//");
      } else if (CurrentTokIt->getKind() == tok::unknown &&
                 CurrentTokIt->getRange().str().startswith("\"\"\"")) {
        StringLiteralRange = CurrentTokIt->getRange();
      }
    }
  }

  /// When visiting an expression, returns true if it's a stement level
  /// expression.
  bool isStatementListItem() {
    if (auto *S = Parent.getAsStmt()) {
      if (auto *RS = dyn_cast<ReturnStmt>(S))
        return RS->isImplicit();
      return isa<BraceStmt>(S);
    }
    if (auto *E = Parent.getAsExpr()) {
      return isa<ClosureExpr>(E);
    }
    return false;
  }

  /// Checks whether the given range is an indent context of the target location.
  ///
  /// \return \c Start < \c TargetLocation <= \c End.
  bool isTargetContext(SourceLoc Start, SourceLoc End) const {
    assert(Start.isValid());
    // Start < Target <= End
    return SM.isBeforeInBuffer(Start, TargetLocation) &&
        (End.isInvalid() ||
         SM.isBeforeInBuffer(TargetLocation,
                             Lexer::getLocForEndOfToken(SM, End)));
  }

  /// Checks whether the given range is an indent context of the target location.
  ///
  /// \return \c Range.Start < \c TargetLocation <= \c Range.End.
  bool isTargetContext(SourceRange Range) const {
    return isTargetContext(Range.Start, Range.End);
  }

  /// Checks whether the given range overlaps the target location.
  ///
  /// \return \c Start <= \c TargetLocation <= \c End
  bool overlapsTarget(SourceLoc Start, SourceLoc End) const {
    assert(Start.isValid());
    return !SM.isBeforeInBuffer(TargetLocation, Start) &&
      (End.isInvalid() ||
       SM.isBeforeInBuffer(TargetLocation,
                           Lexer::getLocForEndOfToken(SM, End)));
  }

  /// Checks whether the given range overlaps the target location.
  ///
  /// \return \c Range.Start <= \c TargetLocation <= \c Range.End
  bool overlapsTarget(SourceRange Range) const {
    assert(Range.isValid());
    return overlapsTarget(Range.Start, Range.End);
  }

  /// Checks whether the given range contains the target location.
  ///
  /// \return \c Start < \c TargetLocation < \c End
  bool containsTarget(SourceLoc Start, SourceLoc End) const {
    assert(Start.isValid());
    return SM.isBeforeInBuffer(Start, TargetLocation) &&
      (End.isInvalid() || SM.isBeforeInBuffer(TargetLocation, End));
  }

  /// Checks whether the given range contains the target location.
  ///
  /// \return \c Range.Start < \c TargetLocation < \c Range.End
  bool containsTarget(SourceRange Range) const {
    assert(Range.isValid());
    return containsTarget(Range.Start, Range.End);
  }

#pragma mark Declaration indent contexts

  llvm::Optional<IndentContext>
  getIndentContextFrom(Decl *D, llvm::Optional<TrailingInfo> TrailingTarget) {

    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(D)) {
      SourceLoc ContextLoc = AFD->getStartLoc();
      // If this is a getter without a 'get' loc, the context loc is the start
      // of its storage.
      if (auto *AD = dyn_cast<AccessorDecl>(AFD)) {
        if (AD->isGetter() && AD->getAccessorKeywordLoc().isInvalid()) {
          auto *ASD = AD->getStorage();
          if (auto *VD = dyn_cast_or_null<VarDecl>(ASD)) {
            ContextLoc = VD->getStartLoc();
          } else if (auto *SD = dyn_cast_or_null<SubscriptDecl>(ASD)) {
            ContextLoc = SD->getStartLoc();
          }
        }
      }
      if (auto Ctx = getIndentContextFrom(AFD->getBody(), ContextLoc))
        return Ctx;
      if (auto Ctx = getIndentContextFrom(AFD->getParameters(), ContextLoc))
        return Ctx;
      if (auto Ctx = getIndentContextFrom(AFD->getParsedGenericParams(), ContextLoc, D))
        return Ctx;
      if (auto Ctx = getIndentContextFrom(AFD->getTrailingWhereClause(), ContextLoc, D))
        return Ctx;

      if (TrailingTarget)
        return llvm::None;
      return IndentContext {ContextLoc, false};
    }

    if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
      SourceLoc ContextLoc = NTD->getStartLoc();

      if (auto Ctx = getIndentContextFromInherits(NTD->getInherited(), ContextLoc))
        return Ctx;
      if (auto Ctx = getIndentContextFromBraces(NTD->getBraces(), ContextLoc, NTD))
        return Ctx;
      if (auto Ctx = getIndentContextFrom(NTD->getParsedGenericParams(), ContextLoc, D))
        return Ctx;
      if (auto Ctx = getIndentContextFrom(NTD->getTrailingWhereClause(), ContextLoc, D))
        return Ctx;

      if (TrailingTarget)
        return llvm::None;
      return IndentContext {ContextLoc, false};
    }

    if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
      SourceLoc ContextLoc = ED->getStartLoc();

      if (auto Ctx = getIndentContextFromInherits(ED->getInherited(), ContextLoc))
        return Ctx;
      if (auto Ctx = getIndentContextFromBraces(ED->getBraces(), ContextLoc, ED))
        return Ctx;
      if (auto Ctx = getIndentContextFrom(ED->getTrailingWhereClause(), ContextLoc, D))
        return Ctx;

      if (TrailingTarget)
        return llvm::None;
      return IndentContext {ContextLoc, false};
    }

    if (auto *ECD = dyn_cast<EnumCaseDecl>(D)) {
      SourceLoc CaseLoc = ECD->getLoc();
      ListAligner Aligner(SM, TargetLocation, CaseLoc, CaseLoc);
      for (auto *Elem: ECD->getElements()) {
        if (Elem->isImplicit())
          continue;
        SourceRange ElemRange = Elem->getSourceRange();
        Aligner.updateAlignment(ElemRange, Elem);
      }
      return Aligner.getContextAndSetAlignment(CtxOverride);
    }

    if (auto *EED = dyn_cast<EnumElementDecl>(D)) {
      SourceLoc ContextLoc = EED->getStartLoc();
      if (auto Ctx = getIndentContextFrom(EED->getParameterList()))
        return Ctx;

      if (TrailingTarget)
        return llvm::None;

      return IndentContext {
        ContextLoc,
        !OutdentChecker::hasOutdent(SM, EED)
      };
    }

    if (auto *SD = dyn_cast<SubscriptDecl>(D)) {
      SourceLoc ContextLoc = SD->getStartLoc();

      if (auto Ctx = getIndentContextFromBraces(SD->getBracesRange(), ContextLoc, SD))
        return Ctx;
      if (auto Ctx = getIndentContextFrom(SD->getIndices(), ContextLoc))
        return Ctx;
      if (auto Ctx = getIndentContextFrom(SD->getParsedGenericParams(), ContextLoc, D))
        return Ctx;
      if (auto Ctx = getIndentContextFrom(SD->getTrailingWhereClause(), ContextLoc, D))
        return Ctx;

      if (TrailingTarget)
        return llvm::None;
      return IndentContext {ContextLoc, false};
    }

    if (auto *PGD = dyn_cast<PrecedenceGroupDecl>(D)) {
      SourceLoc ContextLoc = PGD->getStartLoc();
      SourceLoc L = PGD->getLBraceLoc(), R = PGD->getRBraceLoc();

      if (auto Ctx = getIndentContextFromBraces(L, R, ContextLoc, PGD))
        return Ctx;

      if (TrailingTarget)
        return llvm::None;
      return IndentContext {PGD->getStartLoc(), false};
    }

    if (auto *PBD = dyn_cast<PatternBindingDecl>(D)) {
      SourceLoc ContextLoc = PBD->getStartLoc(), IntroducerLoc = PBD->getLoc();

      ListAligner Aligner(SM, TargetLocation, ContextLoc, IntroducerLoc);

      // Don't column align PBD entries if any entry spans from the same line as
      // the IntroducerLoc (var/let) to another line. E.g.
      //
      // let foo = someItem
      //       .getValue(), // Column-alignment looks ok here, but...
      //     bar = otherItem
      //       .getValue()
      //
      // getAThing()
      //   .andDoStuffWithIt()
      // let foo = someItem
      //       .getValue() // looks over-indented here, which is more common...
      // getOtherThing()
      //   .andDoStuffWithIt()
      //
      // getAThing()
      //   .andDoStuffWithIt()
      // let foo = someItem
      //   .getValue() // so break column alignment in this case...
      // doOtherThing()
      //
      // let foo = someItem.getValue(),
      //     bar = otherItem.getValue() // but not in this case.
      //
      // Using this rule, rather than handling single and multi-entry PBDs
      // differently, ensures that the as-typed-out indentation matches the
      // re-indented indentation for multi-entry PBDs.
      Aligner.BreakAlignmentIfSpanning = true;

      for (auto I: range(PBD->getNumPatternEntries())) {
        SourceRange EntryRange = PBD->getEqualLoc(I);
        VarDecl *SingleVar = nullptr;

        if (auto *E = PBD->getOriginalInit(I))
          widenOrSet(EntryRange, E->getSourceRange());
        if (auto *P = PBD->getPattern(I)) {
          widenOrSet(EntryRange, P->getSourceRange());
          if ((SingleVar = P->getSingleVar()))
            widenOrSet(EntryRange, SingleVar->getBracesRange());
        }
        assert(EntryRange.isValid());
        Aligner.updateAlignment(EntryRange, PBD);

        // If the var has explicit accessors, the braces are an indent context.
        if (SingleVar && hasExplicitAccessors(SingleVar)) {
          SourceRange Braces = SingleVar->getBracesRange();
          if (auto Ctx = getIndentContextFromBraces(Braces, EntryRange.Start, PBD)) {
            Aligner.setAlignmentIfNeeded(CtxOverride);
            return Ctx;
          }
        }

        // The pattern entry as whole is also an indent context.
        if (isTargetContext(EntryRange)) {
          Aligner.setAlignmentIfNeeded(CtxOverride);
          return IndentContext {
            EntryRange.Start,
            !OutdentChecker::hasOutdent(SM, EntryRange, PBD)
          };
        }
      }
      return Aligner.getContextAndSetAlignment(CtxOverride);
    }

    // llvm::None of the below declarations can claim trailing targets.
    if (TrailingTarget)
      return llvm::None;

    if (auto *TAD = dyn_cast<TypeAliasDecl>(D)) {
      SourceLoc ContextLoc = TAD->getStartLoc();

      if (auto Ctx = getIndentContextFrom(TAD->getParsedGenericParams(), ContextLoc,
                                          D)) {
        return Ctx;
      }
      if (auto Ctx = getIndentContextFrom(TAD->getTrailingWhereClause(), ContextLoc,
                                          D)) {
        return Ctx;
      }

      return IndentContext {ContextLoc, !OutdentChecker::hasOutdent(SM, D)};
    }

    if (auto *ATD = dyn_cast<AssociatedTypeDecl>(D)) {
      SourceLoc ContextLoc = ATD->getStartLoc();

      if (auto Ctx = getIndentContextFromInherits(ATD->getInherited(),
                                                  ContextLoc)) {
        return Ctx;
      }
      if (auto Ctx = getIndentContextFrom(ATD->getTrailingWhereClause(),
                                          ContextLoc, D)) {
        return Ctx;
      }

      return IndentContext {ContextLoc, !OutdentChecker::hasOutdent(SM, D)};
    }

    if (auto *PDD = dyn_cast<PoundDiagnosticDecl>(D)) {
      SourceLoc ContextLoc = PDD->getStartLoc();
      // FIXME: add paren source locations to the AST Node.
      if (auto *SLE = PDD->getMessage()) {
        SourceRange MessageRange = SLE->getSourceRange();
        if (MessageRange.isValid() && overlapsTarget(MessageRange))
          return IndentContext {ContextLoc, true};
      }
      return IndentContext {ContextLoc, !OutdentChecker::hasOutdent(SM, D)};
    }

    if (auto *ICD = dyn_cast<IfConfigDecl>(D)) {
      for (auto &Clause: ICD->getClauses()) {
        if (Clause.Loc == TargetLocation)
          break;
        if (auto *Cond = Clause.Cond) {
          SourceRange CondRange = Cond->getSourceRange();
          if (CondRange.isValid() && overlapsTarget(CondRange))
            return IndentContext {Clause.Loc, true};
        }
      }
      return IndentContext { ICD->getStartLoc(), false };
    }

    switch (D->getKind()) {
    case DeclKind::InfixOperator:
    case DeclKind::PostfixOperator:
    case DeclKind::PrefixOperator:
    case DeclKind::Import:
    case DeclKind::Param:
      return IndentContext {
        D->getStartLoc(),
        !OutdentChecker::hasOutdent(SM, D)
      };
    default:
      return llvm::None;
    }
  }

  llvm::Optional<IndentContext>
  getIndentContextFromWhereClause(ArrayRef<RequirementRepr> Requirements,
                                  SourceRange Range, SourceLoc ContextLoc,
                                  Decl *WalkableParent) {
    if (Range.isInvalid())
      return llvm::None;

    ListAligner Aligner(SM, TargetLocation, ContextLoc, Range.Start);
    for (auto &Req: Requirements) {
      SourceRange ReqRange = Req.getSourceRange();
      if (ReqRange.isInvalid())
        continue;
      Aligner.updateAlignment(ReqRange, WalkableParent);
      if (isTargetContext(ReqRange)) {
        Aligner.setAlignmentIfNeeded(CtxOverride);
        return IndentContext {
          ReqRange.Start,
          !OutdentChecker::hasOutdent(SM, ReqRange, WalkableParent)
        };
      }
    }
    return Aligner.getContextAndSetAlignment(CtxOverride);
  }

  llvm::Optional<IndentContext> getIndentContextFrom(TrailingWhereClause *TWC,
                                                     SourceLoc ContextLoc,
                                                     Decl *WalkableParent) {
    if (!TWC)
      return llvm::None;
    return getIndentContextFromWhereClause(TWC->getRequirements(),
                                           TWC->getSourceRange(),
                                           ContextLoc, WalkableParent);
  }

  llvm::Optional<IndentContext> getIndentContextFrom(GenericParamList *GP,
                                                     SourceLoc ContextLoc,
                                                     Decl *WalkableParent) {
    if (!GP)
      return llvm::None;

    SourceLoc L = GP->getLAngleLoc();
    SourceLoc R = getLocIfTokenTextMatches(SM, GP->getRAngleLoc(), ">");

    if (L.isValid() && overlapsTarget(L, R)) {
      ListAligner Aligner(SM, TargetLocation, ContextLoc, L, R);
      for (auto *P: GP->getParams()) {
        SourceRange ParamRange = P->getSourceRange();
        Aligner.updateAlignment(ParamRange, WalkableParent);
        if (isTargetContext(ParamRange)) {
          Aligner.setAlignmentIfNeeded(CtxOverride);
          return IndentContext {
            ParamRange.Start,
            !OutdentChecker::hasOutdent(SM, P)
          };
        }
      }
      if (auto Ctx = Aligner.getContextAndSetAlignment(CtxOverride))
        return Ctx;
    }

    return llvm::None;
  }

  llvm::Optional<IndentContext>
  getIndentContextFrom(ParameterList *PL, SourceLoc ContextLoc = SourceLoc()) {
    if (!PL)
      return llvm::None;

    SourceRange Range = PL->getSourceRange();
    if (Range.isInvalid() || locIsKind(SM, Range.Start, tok::l_brace))
      return llvm::None;

    SourceLoc L = getLocIfKind(SM, PL->getLParenLoc(), tok::l_paren);
    SourceLoc R = getLocIfKind(SM, PL->getRParenLoc(), tok::r_paren);
    if (ContextLoc.isInvalid())
      ContextLoc = Range.Start;

    if (L.isValid()) {
      ListAligner Aligner(SM, TargetLocation, ContextLoc, L, R);
      for (auto *PD: *PL)
        Aligner.updateAlignment(PD->getSourceRange(), PD);
      return Aligner.getContextAndSetAlignment(CtxOverride);
    }

    // There are no parens at this point, so if there are no parameters either,
    // this shouldn't be a context (it's an implicit parameter list).
    if (!PL->size())
      return llvm::None;

    ListAligner Aligner(SM, TargetLocation, ContextLoc, Range.Start);
    for (auto *PD: *PL)
      Aligner.updateAlignment(PD->getSourceRange(), PD);
    return Aligner.getContextAndSetAlignment(CtxOverride);
  }

  template <typename T>
  llvm::Optional<IndentContext>
  getIndentContextFromBraces(SourceLoc Open, SourceLoc Close,
                             SourceLoc ContextLoc, T *WalkableParent) {
    SourceLoc L = getLocIfKind(SM, Open, tok::l_brace);
    SourceLoc R = getLocIfKind(SM, Close, tok::r_brace);
    if (L.isInvalid() || !overlapsTarget(L, R))
      return llvm::None;
    return IndentContext {
      ContextLoc,
      containsTarget(L, R) &&
        !OutdentChecker::hasOutdent(SM, SourceRange(Open, Close), WalkableParent,
                                    RangeKind::Open)
    };
  }

  template <typename T>
  llvm::Optional<IndentContext> getIndentContextFromBraces(SourceRange Braces,
                                                           SourceLoc ContextLoc,
                                                           T *WalkableParent) {
    return getIndentContextFromBraces(Braces.Start, Braces.End, ContextLoc,
                                      WalkableParent);
  }

  llvm::Optional<IndentContext>
  getIndentContextFromInherits(ArrayRef<InheritedEntry> Inherits,
                               SourceLoc ContextLoc) {
    if (Inherits.empty())
      return llvm::None;

    SourceLoc StartLoc = Inherits.front().getSourceRange().Start;
    if (StartLoc.isInvalid())
      return llvm::None;

    // FIXME: Add the colon location to the AST.
    auto ColonLoc = getLastTokenOfKindInOpenRange(SM, tok::colon, ContextLoc,
                                                  StartLoc);
    assert(ColonLoc.has_value() && "inherits list without leading colon?");

    ListAligner Aligner(SM, TargetLocation, ContextLoc, ColonLoc->getLoc());
    for (auto TL: Inherits)
      Aligner.updateAlignment(TL.getSourceRange(), TL.getTypeRepr());
    return Aligner.getContextAndSetAlignment(CtxOverride);
  }

#pragma mark Statement indent contexts

  llvm::Optional<IndentContext>
  getIndentContextFrom(Stmt *S, llvm::Optional<TrailingInfo> TrailingTarget) {

    if (auto *BS = dyn_cast<BraceStmt>(S))
      return getIndentContextFrom(BS);

    if (auto *SS = dyn_cast<SwitchStmt>(S)) {
      SourceLoc ContextLoc = SS->getSwitchLoc();
      if (!SM.isBeforeInBuffer(ContextLoc, TargetLocation))
        return llvm::None;

      if (auto *E = SS->getSubjectExpr()) {
        SourceRange Range = E->getSourceRange();
        widenOrSet(Range, ContextLoc);
        if (isTargetContext(Range)) {
          return IndentContext {
            ContextLoc,
            !OutdentChecker::hasOutdent(SM, Range, E)
          };
        }
      }
      SourceLoc L = SS->getLBraceLoc(), R = SS->getRBraceLoc();
      if (FmtOptions.IndentSwitchCase) {
        if (auto Ctx = getIndentContextFromBraces(L, R, ContextLoc, SS))
          return Ctx;
      }

      if (TrailingTarget)
        return llvm::None;
      return IndentContext {ContextLoc, false};
    }

    auto *CS = dyn_cast<CaseStmt>(S);
    if (CS && CS->getParentKind() == CaseParentKind::Switch) {
      SourceLoc CaseLoc = CS->getLoc();
      if (!SM.isBeforeInBuffer(CaseLoc, TargetLocation))
        return llvm::None;

      SourceRange LabelItemsRange = CS->getLabelItemsRange();
      SourceLoc ColonLoc = getLocIfKind(SM, LabelItemsRange.End, tok::colon);

      if (auto Ctx = getIndentContextFromCaseItems(CS, true))
        return Ctx;

      if (ColonLoc.isValid() && isTargetContext(ColonLoc, SourceLoc()) &&
          (!TrailingTarget || TrailingTarget->isEmpty())) {
        SourceRange ColonToEnd = SourceRange(ColonLoc, CS->getEndLoc());
        return IndentContext {
          CaseLoc,
          !OutdentChecker::hasOutdent(SM, ColonToEnd, CS)
        };
      }

      if (TrailingTarget)
        return llvm::None;
      return IndentContext {CaseLoc, false};
    }

    if (auto *DS = dyn_cast<DoStmt>(S)) {
      if (!SM.isBeforeInBuffer(DS->getDoLoc(), TargetLocation))
        return llvm::None;

      if (auto *BS = dyn_cast<BraceStmt>(DS->getBody())) {
        if (auto Ctx = getIndentContextFrom(BS, DS->getStartLoc()))
          return Ctx;
      }
      if (TrailingTarget)
        return llvm::None;
      return IndentContext {DS->getStartLoc(), false};
    }

    if (CS && CS->getParentKind() == CaseParentKind::DoCatch) {
      SourceLoc CatchLoc = CS->getLoc();
      SourceLoc L;

      auto *BS = dyn_cast<BraceStmt>(CS->getBody());
      if (BS) L = getLocIfKind(SM, BS->getLBraceLoc(), tok::l_brace);

      if (auto Ctx = getIndentContextFromCaseItems(CS, false))
        return Ctx;

      if (auto Ctx = getIndentContextFrom(BS, CS->getStartLoc()))
        return Ctx;

      if (TrailingTarget)
        return llvm::None;
      return IndentContext {CatchLoc, false};
    }

    if (auto *IS = dyn_cast<IfStmt>(S)) {
      SourceLoc ContextLoc = IS->getIfLoc();
      if (!SM.isBeforeInBuffer(ContextLoc, TargetLocation))
        return llvm::None;

      if (auto Ctx = getIndentContextFrom(IS->getCond(), ContextLoc, IS))
        return Ctx;
      if (auto *BS = dyn_cast_or_null<BraceStmt>(IS->getThenStmt())) {
        if (auto Ctx = getIndentContextFrom(BS, IS->getStartLoc()))
          return Ctx;
      }
      if (TrailingTarget)
        return llvm::None;
      return IndentContext {ContextLoc, false};
    }

    if (auto *GS = dyn_cast<GuardStmt>(S)) {
      SourceLoc ContextLoc = GS->getGuardLoc();
      if (!SM.isBeforeInBuffer(ContextLoc, TargetLocation))
        return llvm::None;

      if (auto Ctx = getIndentContextFrom(GS->getCond(), ContextLoc, GS))
        return Ctx;
      if (auto *BS = dyn_cast_or_null<BraceStmt>(GS->getBody())) {
        if (auto Ctx = getIndentContextFrom(BS, GS->getStartLoc()))
          return Ctx;
      }

      if (TrailingTarget)
        return llvm::None;
      return IndentContext {GS->getGuardLoc(), false};
    }

    if (auto *RWS = dyn_cast<RepeatWhileStmt>(S)) {
      SourceLoc ContextLoc = RWS->getRepeatLoc();
      if (!SM.isBeforeInBuffer(ContextLoc, TargetLocation))
        return llvm::None;

      if (auto *E = RWS->getCond()) {
        if (overlapsTarget(E->getSourceRange()))
          return IndentContext {RWS->getRepeatLoc(), true};
      }

      if (auto *BS = dyn_cast_or_null<BraceStmt>(RWS->getBody())) {
        if (auto Ctx = getIndentContextFrom(BS, ContextLoc))
          return Ctx;
      }
      if (TrailingTarget)
        return llvm::None;
      return IndentContext {RWS->getRepeatLoc(), false};
    }

    if (auto *WS = dyn_cast<WhileStmt>(S)) {
      SourceLoc ContextLoc = WS->getWhileLoc();
      if (!SM.isBeforeInBuffer(ContextLoc, TargetLocation))
        return llvm::None;

      if (auto Ctx = getIndentContextFrom(WS->getCond(), ContextLoc, WS))
        return Ctx;

      if (auto *BS = dyn_cast_or_null<BraceStmt>(WS->getBody())) {
        if (auto Ctx = getIndentContextFrom(BS, ContextLoc))
          return Ctx;
      }
      if (TrailingTarget)
        return llvm::None;
      return IndentContext {ContextLoc, false};
    }

    if (auto *FS = dyn_cast<ForEachStmt>(S)) {
      SourceLoc ContextLoc = FS->getStartLoc();
      SourceLoc ForLoc = FS->getForLoc();
      if (!SM.isBeforeInBuffer(ForLoc, TargetLocation))
        return llvm::None;

      if (auto *P = FS->getPattern()) {
        SourceRange Range = P->getSourceRange();
        if (Range.isValid() && overlapsTarget(Range))
          return IndentContext {ForLoc, !OutdentChecker::hasOutdent(SM, P)};
      }
      if (auto *E = FS->getParsedSequence()) {
        SourceRange Range = FS->getInLoc();
        widenOrSet(Range, E->getSourceRange());
        if (Range.isValid() && isTargetContext(Range)) {
          return IndentContext {
            Range.Start,
            !OutdentChecker::hasOutdent(SM, E)
          };
        }
      }
      if (auto *WE = FS->getWhere()) {
        SourceLoc WhereLoc = FS->getWhereLoc();
        SourceRange Range = WE->getSourceRange();
        if (Range.isValid() && overlapsTarget(Range))
          return IndentContext {WhereLoc, !OutdentChecker::hasOutdent(SM, WE)};
      }
      if (auto *BS = dyn_cast_or_null<BraceStmt>(FS->getBody())) {
        if (auto Ctx = getIndentContextFrom(BS, FS->getStartLoc()))
          return Ctx;
      }
      if (TrailingTarget)
        return llvm::None;
      return IndentContext {ContextLoc, false};
    }

    // None of the below statements ever claim trailing targets.
    if (TrailingTarget)
      return llvm::None;

    if (auto *RS = dyn_cast<ReturnStmt>(S)) {
      SourceLoc ContextLoc = RS->getReturnLoc();
      SourceRange Range = RS->getSourceRange();
      Expr *Result = RS->getResult();
      return IndentContext {
        ContextLoc,
        Result && !OutdentChecker::hasOutdent(SM, Range, Result)
      };
    }

    if (auto *DCS = dyn_cast<DoCatchStmt>(S)) {
      if (!SM.isBeforeInBuffer(DCS->getDoLoc(), TargetLocation))
        return llvm::None;
      if (auto *BS = dyn_cast<BraceStmt>(DCS->getBody())) {
        if (auto Ctx = getIndentContextFrom(BS))
          return Ctx;
      }
      return IndentContext {DCS->getStartLoc(), false};
    }

    return llvm::None;
  }

  llvm::Optional<IndentContext>
  getIndentContextFrom(BraceStmt *BS, SourceLoc ContextLoc = SourceLoc()) {
    if (!BS)
      return llvm::None;

    SourceLoc L = getLocIfKind(SM, BS->getLBraceLoc(), tok::l_brace);
    SourceLoc R = getLocIfKind(SM, BS->getRBraceLoc(), tok::r_brace);
    if (L.isInvalid() || !overlapsTarget(L, R))
      return llvm::None;

    if (ContextLoc.isInvalid()) {
      ContextLoc = L;
    } else {
      NodesToSkip.insert(static_cast<Stmt*>(BS));
    }
    bool shouldIndent = containsTarget(L, R) &&
      !OutdentChecker::hasOutdent(SM, BS, RangeKind::Open);
    return IndentContext {ContextLoc, shouldIndent};
  }

  template <typename T>
  llvm::Optional<IndentContext> getIndentContextFrom(PoundAvailableInfo *A,
                                                     T *WalkableParent) {
    SourceLoc ContextLoc = A->getStartLoc();
    SourceLoc L = A->getLParenLoc();
    SourceLoc R = getLocIfKind(SM, A->getRParenLoc(), tok::r_paren);
    if (L.isInvalid() || !overlapsTarget(L, R))
      return llvm::None;

    ListAligner Aligner(SM, TargetLocation, ContextLoc, L, R);
    for (auto *Spec: A->getQueries()) {
      SourceRange Range = Spec->getSourceRange();
      if (Range.isValid()) {
        Aligner.updateAlignment(Range, WalkableParent);
        if (isTargetContext(Range)) {
          Aligner.setAlignmentIfNeeded(CtxOverride);
          return IndentContext {Range.Start, true};
        }
      }
    }
    return Aligner.getContextAndSetAlignment(CtxOverride);
  }

  template <typename T>
  llvm::Optional<IndentContext>
  getIndentContextFrom(const StmtCondition &Condition, SourceLoc ContextLoc,
                       T *WalkableParent) {
    ListAligner Aligner(SM, TargetLocation, ContextLoc, ContextLoc);
    for (auto &Elem: Condition) {
      // Skip implicit condition created when there are no explicit ones.
      Expr *BoolExpr = Elem.getBooleanOrNull();
      if (BoolExpr && BoolExpr->isImplicit())
        continue;

      SourceRange ElemRange = Elem.getSourceRange();
      Aligner.updateAlignment(ElemRange, WalkableParent);

      if (Elem.getKind() == StmtConditionElement::CK_Availability) {
        if (auto Ctx = getIndentContextFrom(Elem.getAvailability(),
                                            WalkableParent)) {
          Aligner.setAlignmentIfNeeded(CtxOverride);
          return Ctx;
        }
      }
      if (ElemRange.isValid() && isTargetContext(ElemRange)) {
        Aligner.setAlignmentIfNeeded(CtxOverride);
        return IndentContext {
          ElemRange.Start,
          !OutdentChecker::hasOutdent(SM, ElemRange, WalkableParent)
        };
      }
    }
    return Aligner.getContextAndSetAlignment(CtxOverride);
  }

  SourceRange getConditionRange(const StmtCondition &Condition) {
    if (Condition.empty())
      return SourceRange();

    SourceRange Bounds = SourceRange(Condition.front().getStartLoc(),
                                     Condition.back().getEndLoc());
    if (auto Next = getTokenAfter(SM, Bounds.End)) {
      if (Next->getKind() == tok::comma)
        Bounds.widen(Next->getLoc());
    }
    return Bounds;
  }

  llvm::Optional<IndentContext>
  getIndentContextFromCaseItems(CaseStmt *CS, bool ElementExpected) {
    SourceLoc IntroducerLoc = CS->getLoc();
    ListAligner Aligner(SM, TargetLocation, IntroducerLoc, IntroducerLoc,
                        ElementExpected);
    for (auto &Elem: CS->getCaseLabelItems()) {
      // Skip the implicit 'error' pattern for empty catch CaseStmts.
      if ((!Elem.getPattern() || Elem.getPattern()->isImplicit()) &&
          Elem.getWhereLoc().isInvalid())
        continue;

      SourceRange ElemRange = Elem.getSourceRange();
      Aligner.updateAlignment(ElemRange, CS);
      if (isTargetContext(ElemRange)) {
        Aligner.setAlignmentIfNeeded(CtxOverride);
        return IndentContext {
          ElemRange.Start,
          !OutdentChecker::hasOutdent(SM, ElemRange, CS)
        };
      }
    }
    return Aligner.getContextAndSetAlignment(CtxOverride);
  }

#pragma mark Expression indent contexts

  llvm::Optional<IndentContext>
  getIndentContextFrom(Expr *E, llvm::Optional<TrailingInfo> TrailingTarget) {

    // All handled expressions may claim a trailing target.

    if (auto *TE = dyn_cast<TupleExpr>(E))
      return getIndentContextFrom(TE);

    if (auto *PE = dyn_cast<ParenExpr>(E))
      return getIndentContextFrom(PE);

    if (auto *DE = dyn_cast<DictionaryExpr>(E)) {
      SourceLoc L = DE->getLBracketLoc();
      SourceLoc R = getLocIfKind(SM, DE->getRBracketLoc(), tok::r_square);
      if (L.isInvalid() || !overlapsTarget(L, R))
        return llvm::None;

      ListAligner Aligner(SM, TargetLocation, L, L, R, true);
      for (Expr *Elem: DE->getElements()) {
        auto *TE = dyn_cast<TupleExpr>(Elem);
        Aligner.updateAlignment(TE->getSourceRange(), TE);
        if (auto Ctx = getIndentContextFromDictionaryElem(TE)) {
          Aligner.setAlignmentIfNeeded(CtxOverride);
          return Ctx;
        }
      }
      return Aligner.getContextAndSetAlignment(CtxOverride);
    }

    if (auto *AE = dyn_cast<ArrayExpr>(E)) {
      SourceLoc L = AE->getLBracketLoc();
      SourceLoc R = getLocIfKind(SM, AE->getRBracketLoc(), tok::r_square);
      if (L.isInvalid() || !overlapsTarget(L, R))
        return llvm::None;

      ListAligner Aligner(SM, TargetLocation, L, L, R, true);
      for (auto *Elem: AE->getElements()) {
        SourceRange ElemRange = Elem->getSourceRange();
        Aligner.updateAlignment(ElemRange, Elem);
        if (isTargetContext(ElemRange)) {
          Aligner.setAlignmentIfNeeded(CtxOverride);
          return IndentContext {
            ElemRange.Start,
            !OutdentChecker::hasOutdent(SM, ElemRange, Elem)
          };
        }
      }
      return Aligner.getContextAndSetAlignment(CtxOverride);
    }

    if (auto *USE = dyn_cast<UnresolvedSpecializeExpr>(E)) {
      SourceLoc L = USE->getLAngleLoc();
      SourceLoc R = getLocIfTokenTextMatches(SM, USE->getRAngleLoc(), ">");
      if (L.isInvalid() || !overlapsTarget(L, R))
        return llvm::None;

      SourceLoc ContextLoc = getContextLocForArgs(SM, USE);
      ListAligner Aligner(SM, TargetLocation, ContextLoc, L, R);
      for (auto *T : USE->getUnresolvedParams()) {
        Aligner.updateAlignment(T->getSourceRange(), T);
      }
      return Aligner.getContextAndSetAlignment(CtxOverride);
    }

    if (auto *CLE = dyn_cast<CaptureListExpr>(E))
      return getIndentContextFrom(CLE);

    if (auto *CE = dyn_cast<ClosureExpr>(E))
      return getIndentContextFrom(CE);

    return llvm::None;
  }

  llvm::Optional<IndentContext>
  getIndentContextFrom(CaptureListExpr *CL,
                       SourceLoc ContextLoc = SourceLoc()) {
    AbstractClosureExpr *CE = CL->getClosureBody();
    BraceStmt *BS = CE->getBody();
    if (!BS)
      return llvm::None;

    if (ContextLoc.isValid()) {
      NodesToSkip.insert(static_cast<Expr*>(CL));
    } else {
      NodesToSkip.insert(static_cast<Expr*>(CE));
    }

    return getIndentContextFrom(CE, ContextLoc, CL);
  }

  llvm::Optional<IndentContext>
  getIndentContextFrom(AbstractClosureExpr *ACE,
                       SourceLoc ContextLoc = SourceLoc(),
                       CaptureListExpr *ParentCapture = nullptr) {
    // Explicit capture lists should always have an explicit ClosureExpr as
    // their subexpression.
    auto CE = dyn_cast<ClosureExpr>(ACE);
    if (!CE) {
      return llvm::None;
    }
    BraceStmt *BS = CE->getBody();
    if (!BS)
      return llvm::None;
    NodesToSkip.insert(static_cast<Stmt*>(BS));

    SourceLoc L = BS->getLBraceLoc();
    SourceLoc R = getLocIfKind(SM, BS->getRBraceLoc(), tok::r_brace);

    if (ContextLoc.isValid()) {
      NodesToSkip.insert(static_cast<Expr*>(CE));
      if (isTargetContext(L, R))
        ContextLoc = CtxOverride.propagateContext(SM, ContextLoc,
                                                  IndentContext::LineStart,
                                                  L, R);
    }

    // Handle the capture list.
    SourceRange CL = CE->getBracketRange();
    if (CL.isValid()) {
      SourceLoc L = CL.Start;
      SourceLoc R = getLocIfKind(SM, CL.End, tok::r_square);
      if (isTargetContext(L, R)) {
        ContextLoc = L;
        if (!ParentCapture) // empty capture list
          return IndentContext {ContextLoc, containsTarget(L, R)};

        ListAligner Aligner(SM, TargetLocation, ContextLoc, L, R);
        for (auto &Entry: ParentCapture->getCaptureList()) {
          auto *PBD = Entry.PBD;
          NodesToSkip.insert(PBD);
          SourceRange Range = PBD->getSourceRangeIncludingAttrs();
          Aligner.updateAlignment(Range, PBD);

          if (isTargetContext(Range)) {
            Aligner.setAlignmentIfNeeded(CtxOverride);
            return IndentContext {
              Range.Start,
              !OutdentChecker::hasOutdent(SM, Range, PBD)
            };
          }
        }
        return Aligner.getContextAndSetAlignment(CtxOverride);
      }
    }

    // Handle parameter list
    if (auto Ctx = getIndentContextFrom(CE->getParameters()))
      return Ctx;

    // Handle outer braces.
    if (L.isInvalid() || !isTargetContext(L, R))
      return llvm::None;

    if (ContextLoc.isInvalid())
      ContextLoc = L;
    Expr *WalkableParent = CE;
    if (ParentCapture)
      WalkableParent = ParentCapture;

    auto InLoc = CE->getInLoc();
    if (InLoc.isValid()) {
      if (containsTarget(InLoc, R)) {
        SourceRange InToEnd = SourceRange(InLoc, BS->getEndLoc());
        return IndentContext {
          ContextLoc,
          !OutdentChecker::hasOutdent(SM, InToEnd, WalkableParent)
        };
      }
    }

    bool shouldIndent = containsTarget(L, R) &&
      !OutdentChecker::hasOutdent(SM, WalkableParent, RangeKind::Open);
    return IndentContext {ContextLoc, shouldIndent};
  }

  llvm::Optional<IndentContext>
  getIndentContextFromDictionaryElem(TupleExpr *TE) {
    SourceLoc Start = TE->getStartLoc(), End = TE->getEndLoc();
    if (!TE->getNumElements() || !isTargetContext(Start, End))
      return llvm::None;
    Expr *Key = TE->getElement(0);
    SourceLoc ColonLoc;
    if (auto Next = getTokenAfter(SM, Key->getEndLoc())) {
      if (Next && Next->getKind() == tok::colon)
        ColonLoc = Next->getLoc();
    }
    if (ColonLoc.isValid() && isTargetContext(ColonLoc, End))
      return IndentContext {
        Start,
        !OutdentChecker::hasOutdent(SM, SourceRange(ColonLoc, End), TE)
      };
    return IndentContext {Start, !OutdentChecker::hasOutdent(SM, Key)};
  }

  llvm::Optional<IndentContext>
  getIndentContextFrom(TupleExpr *TE, SourceLoc ContextLoc = SourceLoc()) {
    if (ContextLoc.isValid())
      NodesToSkip.insert(static_cast<Expr*>(TE));
    SourceLoc L = TE->getLParenLoc();
    SourceLoc R = getLocIfKind(SM, TE->getRParenLoc(),
                               {tok::r_paren, tok::r_square});
    if (L.isInvalid() || !overlapsTarget(L, R))
      return llvm::None;

    if (ContextLoc.isValid()) {
      ContextLoc = CtxOverride.propagateContext(SM, ContextLoc,
                                                IndentContext::LineStart,
                                                L, R);
    } else {
      ContextLoc = L;
    }

    ListAligner Aligner(SM, TargetLocation, ContextLoc, L, R);
    auto NumElems = TE->getNumElements();
    for (auto I : range(NumElems)) {
      SourceRange ElemRange = TE->getElementNameLoc(I);
      if (Expr *Elem = TE->getElement(I))
        widenOrSet(ElemRange, Elem->getSourceRange());
      assert(ElemRange.isValid());

      Aligner.updateAlignment(ElemRange, TE);
      if (isTargetContext(ElemRange)) {
        Aligner.setAlignmentIfNeeded(CtxOverride);
        return IndentContext {
          ElemRange.Start,
          !OutdentChecker::hasOutdent(SM, ElemRange, TE)
        };
      }
    }
    return Aligner.getContextAndSetAlignment(CtxOverride);
  }

  llvm::Optional<IndentContext>
  getIndentContextFrom(ParenExpr *PE, SourceLoc ContextLoc = SourceLoc()) {
    if (ContextLoc.isValid())
      NodesToSkip.insert(static_cast<Expr*>(PE));
    SourceLoc L = PE->getLParenLoc();
    SourceLoc R = getLocIfKind(SM, PE->getRParenLoc(),
                               {tok::r_paren, tok::r_square});
    if (L.isInvalid() || !overlapsTarget(L, R))
      return llvm::None;

    if (ContextLoc.isValid()) {
      ContextLoc = CtxOverride.propagateContext(SM, ContextLoc,
                                                IndentContext::LineStart,
                                                L, R);
    } else {
      ContextLoc = L;
    }

    ListAligner Aligner(SM, TargetLocation, ContextLoc, L, R);
    Expr *Elem = PE->getSubExpr();
    SourceRange Range = Elem->getSourceRange();
    Aligner.updateAlignment(Range, Elem);

    if (isTargetContext(Range)) {
      Aligner.setAlignmentIfNeeded(CtxOverride);
      return IndentContext {
        Range.Start,
        !OutdentChecker::hasOutdent(SM, Elem)
      };
    }
    return Aligner.getContextAndSetAlignment(CtxOverride);
  }

  llvm::Optional<IndentContext> getIndentContextFromTrailingClosure(
      ArgumentList *Args, llvm::Optional<TrailingInfo> TrailingTarget,
      SourceLoc ContextLoc) {
    if (!Args->hasAnyTrailingClosures())
      return llvm::None;

    if (auto *arg = Args->getUnaryExpr()) {
      auto *CE = findTrailingClosureFromArgument(arg);
      if (!CE)
        return llvm::None;

      auto Range = CE->getSourceRange();
      if (Range.isInvalid() || (!TrailingTarget && !overlapsTarget(Range)))
        return llvm::None;

      if (auto *CLE = dyn_cast<CaptureListExpr>(arg))
        return getIndentContextFrom(CLE, ContextLoc);

      return getIndentContextFrom(CE, ContextLoc);
    }
    auto ClosuresRange = Args->getOriginalArgs()->getTrailingSourceRange();
    if (!overlapsTarget(ClosuresRange) && !TrailingTarget)
      return llvm::None;

    SourceRange ContextToEnd(ContextLoc, ClosuresRange.End);
    ContextLoc =
        CtxOverride.propagateContext(SM, ContextLoc, IndentContext::LineStart,
                                     ClosuresRange.Start, SourceLoc());
    if (TrailingTarget)
      return llvm::None;

    auto *ParentE = Parent.getAsExpr();
    assert(ParentE && "Trailing closures can only occur in expr contexts");
    return IndentContext{
        ContextLoc, !OutdentChecker::hasOutdent(SM, ContextToEnd, ParentE)};
  }

  llvm::Optional<IndentContext>
  getIndentContextFrom(ArgumentList *Args,
                       llvm::Optional<TrailingInfo> TrailingTarget,
                       SourceLoc ContextLoc = SourceLoc()) {
    if (ContextLoc.isValid())
      NodesToSkip.insert(static_cast<ArgumentList *>(Args));
    SourceLoc L = Args->getLParenLoc();
    SourceLoc R = getLocIfKind(SM, Args->getRParenLoc(),
                               {tok::r_paren, tok::r_square});
    if (L.isInvalid() || !overlapsTarget(L, R)) {
      return getIndentContextFromTrailingClosure(Args, TrailingTarget,
                                                 ContextLoc);
    }

    if (ContextLoc.isValid()) {
      ContextLoc = CtxOverride.propagateContext(SM, ContextLoc,
                                                IndentContext::LineStart, L, R);
    } else {
      ContextLoc = L;
    }

    ListAligner Aligner(SM, TargetLocation, ContextLoc, L, R);
    for (auto Arg : Args->getOriginalArgs()->getNonTrailingArgs()) {
      SourceRange ElemRange = Arg.getLabelLoc();
      auto *Elem = Arg.getExpr();
      assert(Elem);
      widenOrSet(ElemRange, Elem->getSourceRange());
      assert(ElemRange.isValid());

      Aligner.updateAlignment(ElemRange, Args);
      if (isTargetContext(ElemRange)) {
        Aligner.setAlignmentIfNeeded(CtxOverride);
        return IndentContext{ElemRange.Start,
                             !OutdentChecker::hasOutdent(SM, ElemRange, Args)};
      }
    }
    return Aligner.getContextAndSetAlignment(CtxOverride);
  }

#pragma mark TypeRepr indent contexts

  llvm::Optional<IndentContext>
  getIndentContextFrom(TypeRepr *T,
                       llvm::Optional<TrailingInfo> TrailingTarget) {
    if (TrailingTarget)
      return llvm::None;

    if (auto *GIT = dyn_cast<GenericIdentTypeRepr>(T)) {
      SourceLoc ContextLoc = GIT->getNameLoc().getBaseNameLoc();
      SourceRange Brackets = GIT->getAngleBrackets();
      if (Brackets.isInvalid())
        return llvm::None;

      SourceLoc L = Brackets.Start;
      SourceLoc R = getLocIfTokenTextMatches(SM, Brackets.End, ">");
      ListAligner Aligner(SM, TargetLocation, ContextLoc, L, R);
      for (auto *Arg: GIT->getGenericArgs())
        Aligner.updateAlignment(Arg->getSourceRange(), GIT);

      return Aligner.getContextAndSetAlignment(CtxOverride);
    }

    if (auto *TT = dyn_cast<TupleTypeRepr>(T)) {
      SourceLoc ContextLoc = TT->getStartLoc();
      SourceRange Parens = TT->getParens();
      if (Parens.isInvalid())
        return llvm::None;

      SourceLoc L = Parens.Start;
      SourceLoc R = getLocIfKind(SM, Parens.End, tok::r_paren);
      ListAligner Aligner(SM, TargetLocation, ContextLoc, L, R);
      for (auto &Elem: TT->getElements()) {
        SourceRange ElemRange = Elem.NameLoc;
        widenOrSet(ElemRange, Elem.UnderscoreLoc);
        if (auto *T = Elem.Type)
          widenOrSet(ElemRange, T->getSourceRange());

        Aligner.updateAlignment(ElemRange, TT);
        if (Elem.ColonLoc.isValid()) {
          SourceRange FromColonToEnd = SourceRange(Elem.ColonLoc, ElemRange.End);
          if (isTargetContext(FromColonToEnd)) {
            Aligner.setAlignmentIfNeeded(CtxOverride);
            return IndentContext {
              ElemRange.Start,
              !OutdentChecker::hasOutdent(SM, FromColonToEnd, TT)
            };
          }
        }
      }
      return Aligner.getContextAndSetAlignment(CtxOverride);
    }

    if (auto *AT = dyn_cast<ArrayTypeRepr>(T)) {
      SourceLoc ContextLoc = AT->getStartLoc();
      SourceRange Brackets = AT->getBrackets();
      if (Brackets.isInvalid())
        return llvm::None;
      return IndentContext {
        ContextLoc,
        containsTarget(Brackets.Start, Brackets.End) &&
          !OutdentChecker::hasOutdent(SM, AT, RangeKind::Open)
      };
    }

    if (auto *DT = dyn_cast<DictionaryTypeRepr>(T)) {
      SourceLoc ContextLoc = DT->getStartLoc();
      SourceRange Brackets = DT->getBrackets();
      if (Brackets.isInvalid())
        return llvm::None;

      SourceLoc KeyLoc = DT->getKey()->getStartLoc();
      SourceLoc ColonLoc = DT->getColonLoc();
      if (ColonLoc.isValid()) {
        SourceRange ColonToEnd = SourceRange(ColonLoc, Brackets.End);
        if (isTargetContext(ColonToEnd))
          return IndentContext {
            KeyLoc,
            containsTarget(Brackets) &&
              !OutdentChecker::hasOutdent(SM, ColonToEnd, DT)
          };
      }
      return IndentContext {
        ContextLoc,
        containsTarget(Brackets) &&
          !OutdentChecker::hasOutdent(SM, DT, RangeKind::Open)
      };
    }

    return llvm::None;
  }

#pragma mark Pattern indent contexts

  llvm::Optional<IndentContext>
  getIndentContextFrom(Pattern *P,
                       llvm::Optional<TrailingInfo> TrailingTarget) {
    if (TrailingTarget)
      return llvm::None;

    if (auto *TP = dyn_cast<TypedPattern>(P)) {
      SourceLoc ContextLoc = TP->getStartLoc();
      auto *LHS = TP->getSubPattern();

      SourceLoc ColonLoc;
      if (auto Next = getTokenAfter(SM, LHS->getEndLoc())) {
        if (Next->getKind() == tok::colon)
          ColonLoc = Next->getLoc();
      }
      if (ColonLoc.isValid()) {
        SourceRange ColonToEnd = SourceRange(ColonLoc, TP->getEndLoc());
        if (isTargetContext(ColonToEnd))
          return IndentContext {
            ContextLoc,
            !OutdentChecker::hasOutdent(SM, ColonToEnd, TP)
          };
      }
      return IndentContext {ContextLoc, !OutdentChecker::hasOutdent(SM, TP)};
    }

    if (auto *PP = dyn_cast<ParenPattern>(P)) {
      SourceLoc ContextLoc = PP->getStartLoc();
      SourceLoc L = PP->getLParenLoc();
      SourceLoc R = getLocIfKind(SM, PP->getRParenLoc(), tok::r_paren);
      if (L.isInvalid())
        return llvm::None;
      ListAligner Aligner(SM, TargetLocation, ContextLoc, L, R);
      if (auto *Elem = PP->getSubPattern()) {
        SourceRange ElemRange = Elem->getSourceRange();
        Aligner.updateAlignment(ElemRange, Elem);

        if (isTargetContext(ElemRange)) {
          Aligner.setAlignmentIfNeeded(CtxOverride);
          return IndentContext {
            ElemRange.Start,
            !OutdentChecker::hasOutdent(SM, ElemRange, Elem)
          };
        }
      }
      return Aligner.getContextAndSetAlignment(CtxOverride);
    }

    if (auto *TP = dyn_cast<TuplePattern>(P)) {
      SourceLoc ContextLoc = TP->getStartLoc();
      SourceLoc L = TP->getLParenLoc(), R = TP->getRParenLoc();
      if (L.isInvalid())
        return llvm::None;

      ListAligner Aligner(SM, TargetLocation, ContextLoc, L, R);
      for (auto &Elem: TP->getElements()) {
        SourceRange ElemRange = Elem.getLabelLoc();
        if (auto *P = Elem.getPattern())
          widenOrSet(ElemRange, P->getSourceRange());
        Aligner.updateAlignment(ElemRange, TP);

        if (isTargetContext(ElemRange)) {
          Aligner.setAlignmentIfNeeded(CtxOverride);
          return IndentContext {
            ElemRange.Start,
            !OutdentChecker::hasOutdent(SM, ElemRange, TP)
          };
        }
      }
      return Aligner.getContextAndSetAlignment(CtxOverride);
    }

    return llvm::None;
  }
};

class CodeFormatter {
  CodeFormatOptions &FmtOptions;
public:
  CodeFormatter(CodeFormatOptions &Options)
    :FmtOptions(Options) { }

  std::pair<LineRange, std::string> indent(unsigned LineIndex,
                                           FormatContext &FC,
                                           StringRef Text) {
    if (FC.isExact()) {
      StringRef Line = swift::ide::getTextForLine(LineIndex, Text, /*Trim*/true);
      StringBuilder Builder;
      FC.padToExactColumn(Builder, FmtOptions);
      Builder.append(Line);
      return std::make_pair(LineRange(LineIndex, 1), Builder.str().str());
    }

    // Take the current indent position of the context, then add the number of
    // indents specified.
    auto LineAndColumn = FC.indentLineAndColumn();
    size_t ExpandedIndent = swift::ide::getExpandedIndentForLine(LineAndColumn.first,
                                                                 FmtOptions, Text);

    if (FC.shouldAddIndentForLine()) {
      auto Width = FmtOptions.UseTabs ? FmtOptions.TabWidth
                                      : FmtOptions.IndentWidth;
      // We don't need to add additional indentation if Width is zero.
      if (Width) {
        // Increment indent.
        ExpandedIndent += Width * FC.numIndentLevels();

        // Normalize indent to align on proper column indent width.
        ExpandedIndent -= ExpandedIndent % Width;
      }
    }

    if (FC.IsInDocCommentBlock()) {
      // Inside doc comment block, the indent is one space, e.g.
      // /**
      //  * <---Indent to align with the first star.
      //  */
      ExpandedIndent += 1;
    }

    // Reformat the specified line with the calculated indent.
    StringRef Line = swift::ide::getTextForLine(LineIndex, Text, /*Trim*/true);
    std::string IndentedLine;
    if (FmtOptions.UseTabs)
      IndentedLine.assign(ExpandedIndent / FmtOptions.TabWidth, '\t');
    else
      IndentedLine.assign(ExpandedIndent, ' ');
    IndentedLine.append(Line.str());

    // Return affected line range, which can later be more than one line.
    LineRange range = LineRange(LineIndex, 1);
    return std::make_pair(range, IndentedLine);
  }
};
} //anonymous namespace

size_t swift::ide::getOffsetOfLine(unsigned LineIndex, StringRef Text) {
  //  SourceLoc start = SourceLoc(llvm::SMLoc::getFromPointer(Text.begin()));
  // FIXME: We should have a cached line map in EditableTextBuffer, for now
  // we just do the slow naive thing here.
  size_t LineOffset = 0;
  unsigned CurrentLine = 0;
  while (LineOffset < Text.size() && ++CurrentLine < LineIndex) {
    LineOffset = Text.find_first_of("\r\n", LineOffset);
    if (LineOffset != std::string::npos) {
      ++LineOffset;
      if (LineOffset < Text.size() &&
          Text[LineOffset - 1] == '\r' && Text[LineOffset] == '\n')
        ++LineOffset;
    }

  }
  if (LineOffset == std::string::npos)
    LineOffset = 0;
  return LineOffset;
}

size_t swift::ide::getOffsetOfLine(unsigned LineIndex, StringRef Text, bool Trim) {
  size_t LineOffset = swift::ide::getOffsetOfLine(LineIndex, Text);
  if (!Trim)
    return LineOffset;
  // Skip leading whitespace.
  size_t FirstNonWSOnLine = Text.find_first_not_of(" \t\v\f", LineOffset);
  if (FirstNonWSOnLine != std::string::npos)
    LineOffset = FirstNonWSOnLine;
  return LineOffset;
}

llvm::StringRef swift::ide::getTextForLine(unsigned LineIndex, StringRef Text,
                                           bool Trim) {
  size_t LineOffset = getOffsetOfLine(LineIndex, Text, Trim);
  size_t LineEnd = Text.find_first_of("\r\n", LineOffset);
  return Text.slice(LineOffset, LineEnd);
}

size_t swift::ide::getExpandedIndentForLine(unsigned LineIndex,
                                            CodeFormatOptions Options,
                                            StringRef Text) {
  size_t LineOffset = getOffsetOfLine(LineIndex, Text);

  // Tab-expand all leading whitespace
  size_t FirstNonWSOnLine = Text.find_first_not_of(" \t\v\f", LineOffset);
  size_t Indent = 0;
  while (LineOffset < Text.size() && LineOffset < FirstNonWSOnLine) {
    if (Text[LineOffset++] == '\t')
      Indent += Options.TabWidth;
    else
      Indent += 1;
  }
  return Indent;
}

std::pair<LineRange, std::string> swift::ide::reformat(LineRange Range,
                                                       CodeFormatOptions Options,
                                                       SourceManager &SM,
                                                       SourceFile &SF) {
  // Sanitize 0-width tab
  if (Options.UseTabs && !Options.TabWidth) {
    // If IndentWidth is specified, use it as the tab width. Otherwise, use the
    // default value.
    Options.TabWidth = Options.IndentWidth ? Options.IndentWidth : 4;
  }
  auto SourceBufferID = SF.getBufferID().value();
  StringRef Text = SM.getLLVMSourceMgr()
    .getMemoryBuffer(SourceBufferID)->getBuffer();
  size_t Offset = getOffsetOfLine(Range.startLine(), Text, /*Trim*/true);
  SourceLoc Loc = SM.getLocForBufferStart(SourceBufferID)
    .getAdvancedLoc(Offset);

  FormatWalker walker(SF, SM, Options);
  FormatContext FC = walker.walkToLocation(Loc);
  CodeFormatter CF(Options);
  return CF.indent(Range.startLine(), FC, Text);
}

