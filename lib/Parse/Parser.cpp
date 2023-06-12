//===--- Parser.cpp - Swift Language Parser -------------------------------===//
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
//
//  This file implements the Swift parser.
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"
#include "swift/Subsystems.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsParse.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParseRequests.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/IDEInspectionCallbacks.h"
#include "swift/Parse/ParseSILSupport.h"
#include "swift/SymbolGraphGen/SymbolGraphOptions.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/Twine.h"

static void getStringPartTokens(const swift::Token &Tok,
                                const swift::LangOptions &LangOpts,
                                const swift::SourceManager &SM, int BufID,
                                std::vector<swift::Token> &Toks);

namespace swift {
template <typename DF>
void tokenize(const LangOptions &LangOpts, const SourceManager &SM,
              unsigned BufferID, unsigned Offset, unsigned EndOffset,
              DiagnosticEngine * Diags,
              CommentRetentionMode RetainComments,
              bool TokenizeInterpolatedString, ArrayRef<Token> SplitTokens,
              DF &&DestFunc) {
  if (Offset == 0 && EndOffset == 0)
    EndOffset = SM.getRangeForBuffer(BufferID).getByteLength();

  Lexer L(LangOpts, SM, BufferID, Diags, LexerMode::Swift,
          HashbangMode::Allowed, RetainComments, Offset,
          EndOffset);

  auto TokComp = [&](const Token &A, const Token &B) {
    return SM.isBeforeInBuffer(A.getLoc(), B.getLoc());
  };

  std::set<Token, decltype(TokComp)> ResetTokens(TokComp);
  for (auto C = SplitTokens.begin(), E = SplitTokens.end(); C != E; ++C) {
    ResetTokens.insert(*C);
  }

  Token Tok;
  do {
    L.lex(Tok);

    // If the token has the same location as a reset location,
    // reset the token stream
    auto F = ResetTokens.find(Tok);
    if (F != ResetTokens.end()) {
      assert(F->isNot(tok::string_literal));

      DestFunc(*F);

      auto NewState = L.getStateForBeginningOfTokenLoc(
          F->getLoc().getAdvancedLoc(F->getLength()));
      L.restoreState(NewState);
      continue;
    }

    if (Tok.is(tok::string_literal) && TokenizeInterpolatedString) {
      std::vector<Token> StrTokens;
      getStringPartTokens(Tok, LangOpts, SM, BufferID, StrTokens);
      for (auto &StrTok : StrTokens) {
        DestFunc(StrTok);
      }
    } else {
      DestFunc(Tok);
    }

  } while (Tok.getKind() != tok::eof);
}
} // namespace swift

using namespace swift;
using ParsingFlags = SourceFile::ParsingFlags;

void SILParserStateBase::anchor() { }

void swift::performIDEInspectionSecondPass(
    SourceFile &SF, IDEInspectionCallbacksFactory &Factory) {
  return (void)evaluateOrDefault(SF.getASTContext().evaluator,
                                 IDEInspectionSecondPassRequest{&SF, &Factory},
                                 false);
}

bool IDEInspectionSecondPassRequest::evaluate(
    Evaluator &evaluator, SourceFile *SF,
    IDEInspectionCallbacksFactory *Factory) const {
  // If we didn't find the code completion token, bail.
  auto *parserState = SF->getDelayedParserState();
  if (!parserState->hasIDEInspectionDelayedDeclState())
    return true;

  // Decrement the closure discriminator index by one so a top-level closure
  // gets the same discriminator as before when being re-parsed in the second
  // pass.
  auto state = parserState->takeIDEInspectionDelayedDeclState();
  auto &Ctx = SF->getASTContext();

  auto BufferID = Ctx.SourceMgr.getIDEInspectionTargetBufferID();
  Parser TheParser(BufferID, *SF, nullptr, parserState);

  auto Callbacks = Factory->createCallbacks(TheParser);
  TheParser.setCodeCompletionCallbacks(Callbacks.CompletionCallbacks.get());
  TheParser.setDoneParsingCallback(Callbacks.DoneParsingCallback.get());

  TheParser.performIDEInspectionSecondPassImpl(*state);
  return true;
}

void Parser::performIDEInspectionSecondPassImpl(
    IDEInspectionDelayedDeclState &info) {
  // Disable updating the interface hash
  llvm::SaveAndRestore<Optional<StableHasher>> CurrentTokenHashSaver(
      CurrentTokenHash, None);

  auto BufferID = L->getBufferID();
  auto startLoc = SourceMgr.getLocForOffset(BufferID, info.StartOffset);
  SourceLoc prevLoc;
  if (info.PrevOffset != ~0U)
    prevLoc = SourceMgr.getLocForOffset(BufferID, info.PrevOffset);
  // Set the parser position to the start of the delayed decl or the body.
  restoreParserPosition(getParserPosition(startLoc, prevLoc));

  DeclContext *DC = info.ParentContext;

  // Forget about the fact that we may have already computed local
  // discriminators.
  Context.evaluator.clearCachedOutput(LocalDiscriminatorsRequest{DC});

  // Clear any ASTScopes that were expanded.
  SF.clearScope();

  switch (info.Kind) {
  case IDEInspectionDelayedDeclKind::TopLevelCodeDecl: {
    // Re-enter the top-level code decl context.
    // FIXME: this can issue discriminators out-of-order?
    auto *TLCD = cast<TopLevelCodeDecl>(DC);
    ContextChange CC(*this, TLCD);

    SourceLoc StartLoc = Tok.getLoc();
    ASTNode Result;
    parseExprOrStmt(Result);
    if (!Result.isNull()) {
      auto Brace = BraceStmt::create(Context, StartLoc, Result, Tok.getLoc());
      TLCD->setBody(Brace);
    }
    break;
  }

  case IDEInspectionDelayedDeclKind::Decl: {
    assert((DC->isTypeContext() || DC->isModuleScopeContext()) &&
           "Delayed decl must be a type member or a top-level decl");
    ContextChange CC(*this, DC);

    parseDecl(ParseDeclOptions(info.Flags),
              /*IsAtStartOfLineOrPreviousHadSemi=*/true,
              /*IfConfigsAreDeclAttrs=*/false,
              [&](Decl *D) {
                if (auto *NTD = dyn_cast<NominalTypeDecl>(DC)) {
                  NTD->addMemberPreservingSourceOrder(D);
                } else if (auto *ED = dyn_cast<ExtensionDecl>(DC)) {
                  ED->addMemberPreservingSourceOrder(D);
                } else if (auto *SF = dyn_cast<SourceFile>(DC)) {
                  SF->addTopLevelDecl(D);
                } else {
                  llvm_unreachable("invalid decl context kind");
                }
              });
    break;
  }

  case IDEInspectionDelayedDeclKind::FunctionBody: {
    auto *AFD = cast<AbstractFunctionDecl>(DC);
    (void)parseAbstractFunctionBodyImpl(AFD);
    break;
  }
  }

  assert(!State->hasIDEInspectionDelayedDeclState() &&
         "Second pass should not set any code completion info");

  DoneParsingCallback->doneParsing(DC->getParentSourceFile());

  State->restoreIDEInspectionDelayedDeclState(info);
}

swift::Parser::BacktrackingScopeImpl::~BacktrackingScopeImpl() {
  if (Backtrack) {
    P.backtrackToPosition(PP);
    DT.abort();
  }
}

void swift::Parser::CancellableBacktrackingScope::cancelBacktrack() {
  if (!Backtrack)
    return;

  Backtrack = false;
  DT.commit();
  TempReceiver.shouldTransfer = true;
}

/// Tokenizes a string literal, taking into account string interpolation.
static void getStringPartTokens(const Token &Tok, const LangOptions &LangOpts,
                                const SourceManager &SM,
                                int BufID, std::vector<Token> &Toks) {
  assert(Tok.is(tok::string_literal));
  bool IsMultiline = Tok.isMultilineString();
  unsigned CustomDelimiterLen = Tok.getCustomDelimiterLen();
  unsigned QuoteLen = (IsMultiline ? 3 : 1) + CustomDelimiterLen;
  SmallVector<Lexer::StringSegment, 4> Segments;
  Lexer::getStringLiteralSegments(Tok, Segments, /*Diags=*/nullptr);
  for (unsigned i = 0, e = Segments.size(); i != e; ++i) {
    Lexer::StringSegment &Seg = Segments[i];
    bool isFirst = i == 0;
    bool isLast = i == e-1;
    if (Seg.Kind == Lexer::StringSegment::Literal) {
      SourceLoc Loc = Seg.Loc;
      unsigned Len = Seg.Length;
      if (isFirst) {
        // Include the quote.
        Loc = Loc.getAdvancedLoc(-QuoteLen);
        Len += QuoteLen;
      }
      if (isLast) {
        // Include the quote.
        Len += QuoteLen;
      }

      StringRef Text = SM.extractText({ Loc, Len });
      Token NewTok;
      NewTok.setToken(tok::string_literal, Text);
      NewTok.setStringLiteral(IsMultiline, CustomDelimiterLen);
      Toks.push_back(NewTok);

    } else {
      assert(Seg.Kind == Lexer::StringSegment::Expr &&
             "new enumerator was introduced ?");
      unsigned Offset = SM.getLocOffsetInBuffer(Seg.Loc, BufID);
      unsigned EndOffset = Offset + Seg.Length;

      if (isFirst) {
        // Add a token for the quote character.
        StringRef Text = SM.extractText({ Seg.Loc.getAdvancedLoc(-2), 1 });
        Token NewTok;
        NewTok.setToken(tok::string_literal, Text);
        Toks.push_back(NewTok);
      }

      std::vector<Token> NewTokens = swift::tokenize(LangOpts, SM, BufID,
                                                     Offset, EndOffset,
                                                     /*Diags=*/nullptr,
                                                     /*KeepComments=*/true);
      Toks.insert(Toks.end(), NewTokens.begin(), NewTokens.end());

      if (isLast) {
        // Add a token for the quote character.
        StringRef Text = SM.extractText({ Seg.Loc.getAdvancedLoc(Seg.Length),
                                          1 });
        Token NewTok;
        NewTok.setToken(tok::string_literal, Text);
        Toks.push_back(NewTok);
      }
    }
  }
}

std::vector<Token> swift::tokenize(const LangOptions &LangOpts,
                                   const SourceManager &SM, unsigned BufferID,
                                   unsigned Offset, unsigned EndOffset,
                                   DiagnosticEngine *Diags,
                                   bool KeepComments,
                                   bool TokenizeInterpolatedString,
                                   ArrayRef<Token> SplitTokens) {
  std::vector<Token> Tokens;

  tokenize(LangOpts, SM, BufferID, Offset, EndOffset, Diags,
           KeepComments ? CommentRetentionMode::ReturnAsTokens
                        : CommentRetentionMode::AttachToNextToken,
           TokenizeInterpolatedString,
           SplitTokens,
           [&](const Token &Tok) { Tokens.push_back(Tok); });

  assert(Tokens.back().is(tok::eof));
  Tokens.pop_back(); // Remove EOF.
  return Tokens;
}

//===----------------------------------------------------------------------===//
// Setup and Helper Methods
//===----------------------------------------------------------------------===//


static LexerMode sourceFileKindToLexerMode(SourceFileKind kind) {
  switch (kind) {
    case swift::SourceFileKind::Interface:
      return LexerMode::SwiftInterface;
    case swift::SourceFileKind::SIL:
      return LexerMode::SIL;
    case swift::SourceFileKind::Library:
    case swift::SourceFileKind::Main:
    case swift::SourceFileKind::MacroExpansion:
      return LexerMode::Swift;
  }
  llvm_unreachable("covered switch");
}

Parser::Parser(unsigned BufferID, SourceFile &SF, SILParserStateBase *SIL,
               PersistentParserState *PersistentState)
    : Parser(BufferID, SF, &SF.getASTContext().Diags, SIL, PersistentState) {}

Parser::Parser(unsigned BufferID, SourceFile &SF, DiagnosticEngine* LexerDiags,
               SILParserStateBase *SIL,
               PersistentParserState *PersistentState)
    : Parser(
          std::unique_ptr<Lexer>(new Lexer(
              SF.getASTContext().LangOpts, SF.getASTContext().SourceMgr,
              BufferID, LexerDiags,
              sourceFileKindToLexerMode(SF.Kind),
              SF.Kind == SourceFileKind::Main
                  ? HashbangMode::Allowed
                  : HashbangMode::Disallowed,
              SF.getASTContext().LangOpts.AttachCommentsToDecls
                  ? CommentRetentionMode::AttachToNextToken
                  : CommentRetentionMode::None)),
          SF, SIL, PersistentState) {}

namespace {

/// This is the token receiver that helps SourceFile to keep track of its
/// underlying corrected token stream.
class TokenRecorder: public ConsumeTokenReceiver {
  ASTContext &Ctx;
  /// The lexer that is being used to lex the source file. Used to query whether
  /// lexing has been cut off.
  Lexer &BaseLexer;
  unsigned BufferID;

  // Token list ordered by their appearance in the source file.
  std::vector<Token> Tokens;

  // Registered token kind change. These changes are registered before the
  // token is consumed, so we need to keep track of them here.
  llvm::DenseMap<const void*, tok> TokenKindChangeMap;

  std::vector<Token>::iterator lower_bound(SourceLoc Loc) {
    return token_lower_bound(Tokens, Loc);
  }

  std::vector<Token>::iterator lower_bound(Token Tok) {
    return lower_bound(Tok.getLoc());
  }

  void relexComment(CharSourceRange CommentRange,
                    llvm::SmallVectorImpl<Token> &Scratch) {
    auto &SM = Ctx.SourceMgr;
    auto EndOffset = SM.getLocOffsetInBuffer(CommentRange.getEnd(), BufferID);
    if (auto LexerCutOffOffset = BaseLexer.lexingCutOffOffset()) {
      if (*LexerCutOffOffset < EndOffset) {
        // If lexing was cut off due to a too deep nesting level, adjust the end
        // offset to not point past the cut off point.
        EndOffset = *LexerCutOffOffset;
      }
    }
    Lexer L(Ctx.LangOpts, SM, BufferID, nullptr, LexerMode::Swift,
            HashbangMode::Disallowed, CommentRetentionMode::ReturnAsTokens,
            SM.getLocOffsetInBuffer(CommentRange.getStart(), BufferID),
            EndOffset);
    while(true) {
      Token Result;
      L.lex(Result);
      if (Result.is(tok::eof))
        break;
      assert(Result.is(tok::comment));
      Scratch.push_back(Result);
    }
  }

public:
  TokenRecorder(ASTContext &ctx, Lexer &BaseLexer)
      : Ctx(ctx), BaseLexer(BaseLexer), BufferID(BaseLexer.getBufferID()) {}

  Optional<std::vector<Token>> finalize() override {
    auto &SM = Ctx.SourceMgr;

    // We should consume the comments at the end of the file that don't attach
    // to any tokens.
    SourceLoc TokEndLoc;
    if (!Tokens.empty()) {
      Token Last = Tokens.back();
      TokEndLoc = Last.getLoc().getAdvancedLoc(Last.getLength());
    } else {

      // Special case: the file contains nothing but comments.
      TokEndLoc = SM.getLocForBufferStart(BufferID);
    }
    llvm::SmallVector<Token, 4> Scratch;
    relexComment(CharSourceRange(SM, TokEndLoc,
                                 SM.getRangeForBuffer(BufferID).getEnd()),
                 Scratch);
    // Accept these orphaned comments.
    Tokens.insert(Tokens.end(), Scratch.begin(), Scratch.end());
    return std::move(Tokens);
  }

  void registerTokenKindChange(SourceLoc Loc, tok NewKind) override {
    // If a token with the same location is already in the bag, update its kind.
    auto Pos = lower_bound(Loc);
    if (Pos != Tokens.end() &&
        Pos->getLoc().getOpaquePointerValue() == Loc.getOpaquePointerValue()) {
      Pos->setKind(NewKind);
      return;
    }

    // Save the update for later.
    TokenKindChangeMap[Loc.getOpaquePointerValue()] = NewKind;
  }

  void receive(const Token &TokParam) override {
    Token Tok = TokParam;
    // We filter out all tokens without valid location
    if(Tok.getLoc().isInvalid())
      return;

    // If a token with the same location is already in the bag, skip this token.
    auto Pos = lower_bound(Tok);
    if (Pos != Tokens.end() && Pos->getLoc().getOpaquePointerValue() ==
                                   Tok.getLoc().getOpaquePointerValue()) {
      return;
    }

    // Update Token kind if a kind update was registered before.
    auto Found = TokenKindChangeMap.find(Tok.getLoc().
                                         getOpaquePointerValue());
    if (Found != TokenKindChangeMap.end()) {
      Tok.setKind(Found->getSecond());
    }

    // If the token has comment attached to it, re-lexing these comments and
    // consume them as separate tokens.
    llvm::SmallVector<Token, 4> TokensToConsume;
    if (Tok.hasComment()) {
      relexComment(Tok.getCommentRange(), TokensToConsume);
    }

    TokensToConsume.push_back(Tok);
    Tokens.insert(Pos, TokensToConsume.begin(), TokensToConsume.end());
  }
};
} // End of an anonymous namespace.

Parser::Parser(std::unique_ptr<Lexer> Lex, SourceFile &SF,
               SILParserStateBase *SIL, PersistentParserState *PersistentState)
    : SourceMgr(SF.getASTContext().SourceMgr), Diags(SF.getASTContext().Diags),
      SF(SF), L(Lex.release()), SIL(SIL), CurDeclContext(&SF),
      Context(SF.getASTContext()),
      TokReceiver(SF.shouldCollectTokens()
                      ? new TokenRecorder(SF.getASTContext(), *L)
                      : new ConsumeTokenReceiver()) {
  State = PersistentState;
  if (!State) {
    OwnedState.reset(new PersistentParserState());
    State = OwnedState.get();
  }

  // If the interface hash is enabled, set up the initial hash.
  if (SF.hasInterfaceHash())
    CurrentTokenHash.emplace(StableHasher::defaultHasher());

  // Set the token to a sentinel so that we know the lexer isn't primed yet.
  // This cannot be tok::unknown, since that is a token the lexer could produce.
  Tok.setKind(tok::NUM_TOKENS);
}

Parser::~Parser() {
  delete L;
  delete TokReceiver;
}

bool Parser::isInSILMode() const { return SF.Kind == SourceFileKind::SIL; }

bool Parser::isDelayedParsingEnabled() const {
  // Do not delay parsing during IDE inspection's second pass.
  if (DoneParsingCallback)
    return false;

  return SF.hasDelayedBodyParsing();
}

bool Parser::shouldEvaluatePoundIfDecls() const {
  auto opts = SF.getParsingOptions();
  return !opts.contains(ParsingFlags::DisablePoundIfEvaluation);
}

bool Parser::allowTopLevelCode() const {
  return SF.isScriptMode();
}

bool Parser::isInMacroExpansion(SourceLoc loc) const {
  if (loc.isInvalid())
    return false;

  auto bufferID = SourceMgr.findBufferContainingLoc(loc);
  auto generatedSourceInfo = SourceMgr.getGeneratedSourceInfo(bufferID);
  if (!generatedSourceInfo)
    return false;

  return true;
}

const Token &Parser::peekToken() {
  return L->peekNextToken();
}

SourceLoc Parser::discardToken() {
  assert(Tok.isNot(tok::eof) && "Lexing past eof!");
  SourceLoc Loc = Tok.getLoc();
  L->lex(Tok);
  return Loc;
}

SourceLoc Parser::consumeTokenWithoutFeedingReceiver() {
  recordTokenHash(Tok);
  auto Loc = discardToken();
  PreviousLoc = Loc;
  return Loc;
}

void Parser::recordTokenHash(StringRef token) {
  assert(!token.empty());
  if (CurrentTokenHash) {
    CurrentTokenHash->combine(token);
    // Add null byte to separate tokens.
    CurrentTokenHash->combine(uint8_t{0});
  }
}

void Parser::consumeExtraToken(Token Extra) {
  TokReceiver->receive(Extra);
}

SourceLoc Parser::consumeToken() {
  TokReceiver->receive(Tok);
  return consumeTokenWithoutFeedingReceiver();
}

SourceLoc Parser::getEndOfPreviousLoc() const {
  return Lexer::getLocForEndOfToken(SourceMgr, PreviousLoc);
}

SourceLoc Parser::consumeStartingCharacterOfCurrentToken(tok Kind, size_t Len) {
  // Consumes prefix of token and returns its location.
  // (like '?', '<', '>' or '!' immediately followed by '<') 
  assert(Len >= 1);

  // Current token can be either one-character token we want to consume...
  if (Tok.getLength() == Len) {
    Tok.setKind(Kind);
    return consumeToken();
  }

  auto Loc = Tok.getLoc();

  // ... or a multi-character token with the first N characters being the one
  // that we want to consume as a separate token.
  assert(Tok.getLength() > Len);
  markSplitToken(Kind, Tok.getText().substr(0, Len));

  auto NewState = L->getStateForBeginningOfTokenLoc(Loc.getAdvancedLoc(Len));
  restoreParserPosition(ParserPosition(NewState, Loc),
                        /*enableDiagnostics=*/true);
  return PreviousLoc;
}

void Parser::markSplitToken(tok Kind, StringRef Txt) {
  SplitTokens.emplace_back();
  SplitTokens.back().setToken(Kind, Txt);
  TokReceiver->receive(SplitTokens.back());
}

SourceLoc Parser::consumeStartingLess() {
  assert(startsWithLess(Tok) && "Token does not start with '<'");
  return consumeStartingCharacterOfCurrentToken(tok::l_angle);
}

SourceLoc Parser::consumeStartingGreater() {
  assert(startsWithGreater(Tok) && "Token does not start with '>'");
  return consumeStartingCharacterOfCurrentToken(tok::r_angle);
}

bool Parser::startsWithEllipsis(Token Tok) {
  if (!Tok.isAnyOperator() && !Tok.isPunctuation())
    return false;

  return Tok.getText().startswith("...");
}

SourceLoc Parser::consumeStartingEllipsis() {
  assert(startsWithEllipsis(Tok) && "Token does not start with '...'");
  return consumeStartingCharacterOfCurrentToken(tok::ellipsis, /*length*/ 3);
}

ParserStatus Parser::skipSingle() {
  ParserStatus status;
  switch (Tok.getKind()) {
  case tok::l_paren:
    consumeToken();
    status |= skipUntil(tok::r_paren, tok::r_brace);
    consumeIf(tok::r_paren);
    break;
  case tok::l_brace:
    consumeToken();
    status |= skipUntil(tok::r_brace);
    consumeIf(tok::r_brace);
    break;
  case tok::l_square:
    consumeToken();
    status |= skipUntil(tok::r_square, tok::r_brace);
    consumeIf(tok::r_square);
    break;
  case tok::pound_if:
  case tok::pound_else:
  case tok::pound_elseif:
    consumeToken();
    // skipUntil also implicitly stops at tok::pound_endif.
    status |= skipUntil(tok::pound_else, tok::pound_elseif);
      
    if (Tok.isAny(tok::pound_else, tok::pound_elseif))
      status |= skipSingle();
    else
      consumeIf(tok::pound_endif);
    break;
      
  default:
    if (Tok.is(tok::code_complete))
      status.setHasCodeCompletionAndIsError();
    consumeToken();
    break;
  }

  return status;
}

ParserStatus Parser::skipUntil(tok T1, tok T2) {
  ParserStatus status;

  // tok::NUM_TOKENS is a sentinel that means "don't skip".
  if (T1 == tok::NUM_TOKENS && T2 == tok::NUM_TOKENS) return status;

  while (Tok.isNot(T1, T2, tok::eof, tok::pound_endif, tok::pound_else,
                   tok::pound_elseif))
    status |= skipSingle();

  return status;
}

void Parser::skipUntilAnyOperator() {
  while (Tok.isNot(tok::eof, tok::pound_endif, tok::code_complete) &&
         Tok.isNotAnyOperator())
    skipSingle();
}

/// Skip until a token that starts with '>', and consume it if found.
/// Applies heuristics that are suitable when trying to find the end of a list
/// of generic parameters, generic arguments, or list of types in a protocol
/// composition.
SourceLoc Parser::skipUntilGreaterInTypeList(bool protocolComposition) {
  SourceLoc lastLoc = PreviousLoc;
  while (true) {
    switch (Tok.getKind()) {
    case tok::eof:
    case tok::l_brace:
    case tok::r_brace:
    case tok::code_complete:
      return lastLoc;

#define KEYWORD(X) case tok::kw_##X:
#define POUND_KEYWORD(X) case tok::pound_##X:
#include "swift/AST/TokenKinds.def"
    // 'Self' can appear in types, skip it.
    if (Tok.is(tok::kw_Self))
      break;
    if (isStartOfStmt() || isStartOfSwiftDecl() || Tok.is(tok::pound_endif))
      return lastLoc;
    break;

    case tok::l_paren:
    case tok::r_paren:
    case tok::l_square:
    case tok::r_square:
      // In generic type parameter list, skip '[' ']' '(' ')', because they
      // can appear in types.
      if (protocolComposition)
        return lastLoc;
      break;

    default:
      if (Tok.isAnyOperator() && startsWithGreater(Tok))
        return consumeStartingGreater();
      
      break;
    }
    skipSingle();
    lastLoc = PreviousLoc;
  }
}

void Parser::skipUntilDeclRBrace() {
  while (Tok.isNot(tok::eof, tok::r_brace, tok::pound_endif,
                   tok::pound_else, tok::pound_elseif,
                   tok::code_complete) &&
         !isStartOfSwiftDecl(/*allowPoundIfAttributes=*/false))
    skipSingle();
}

void Parser::skipListUntilDeclRBrace(SourceLoc startLoc, tok T1, tok T2) {
  while (Tok.isNot(T1, T2, tok::eof, tok::r_brace, tok::pound_endif,
                   tok::pound_else, tok::pound_elseif)) {
    bool hasDelimiter = Tok.getLoc() == startLoc || consumeIf(tok::comma);
    bool possibleDeclStartsLine = Tok.isAtStartOfLine();
    
    if (isStartOfSwiftDecl(/*allowPoundIfAttributes=*/false)) {
      
      // Could have encountered something like `_ var:` 
      // or `let foo:` or `var:`
      if (Tok.isAny(tok::kw_var, tok::kw_let) ||
          (Context.LangOpts.hasFeature(Feature::ReferenceBindings) &&
           Tok.isAny(tok::kw_inout))) {
        if (possibleDeclStartsLine && !hasDelimiter) {
          break;
        }

        Parser::CancellableBacktrackingScope backtrack(*this);
        // Consume the let or var
        consumeToken();
        
        // If the following token is either <identifier> or :, it means that
        // this `var` or `let` should be interpreted as a label
        if ((Tok.canBeArgumentLabel() && peekToken().is(tok::colon)) ||
             peekToken().is(tok::colon)) {
          backtrack.cancelBacktrack();
          continue;
        }
      }
      break;
    }
    skipSingle();
  }
}

void Parser::skipUntilDeclRBrace(tok T1, tok T2) {
  while (Tok.isNot(T1, T2, tok::eof, tok::r_brace, tok::pound_endif,
                   tok::pound_else, tok::pound_elseif) &&
         !isStartOfSwiftDecl(/*allowPoundIfAttributes=*/false)) {
    skipSingle();
  }
}

void Parser::skipUntilConditionalBlockClose() {
  while (Tok.isNot(tok::pound_else, tok::pound_elseif, tok::pound_endif,
                   tok::eof)) {
    skipSingle();
  }
}

bool Parser::skipUntilTokenOrEndOfLine(tok T1, tok T2) {
  while (Tok.isNot(tok::eof, T1, T2) && !Tok.isAtStartOfLine())
    skipSingle();

  return Tok.isAny(T1, T2) && !Tok.isAtStartOfLine();
}

bool Parser::parseEndIfDirective(SourceLoc &Loc) {
  Loc = Tok.getLoc();
  if (parseToken(tok::pound_endif, diag::expected_close_to_if_directive)) {
    Loc = PreviousLoc;
    skipUntilConditionalBlockClose();
    return true;
  } else if (!Tok.isAtStartOfLine() && Tok.isNot(tok::eof))
    diagnose(Tok.getLoc(),
             diag::extra_tokens_conditional_compilation_directive);
  return false;
}

static Parser::StructureMarkerKind
getStructureMarkerKindForToken(const Token &tok) {
  switch (tok.getKind()) {
  case tok::l_brace:
    return Parser::StructureMarkerKind::OpenBrace;
  case tok::l_paren:
    return Parser::StructureMarkerKind::OpenParen;
  case tok::l_square:
    return Parser::StructureMarkerKind::OpenSquare;
  default:
    llvm_unreachable("Not a matched token");
  }
}

Parser::StructureMarkerRAII::StructureMarkerRAII(Parser &parser, SourceLoc loc,
                                                 StructureMarkerKind kind)
    : StructureMarkerRAII(parser) {
  parser.StructureMarkers.push_back({loc, kind, None});
  if (parser.StructureMarkers.size() > MaxDepth) {
    parser.diagnose(loc, diag::structure_overflow, MaxDepth);
    // We need to cut off parsing or we will stack-overflow.
    // But `cutOffParsing` changes the current token to eof, and we may be in
    // a place where `consumeToken()` will be expecting e.g. '[',
    // since we need that to get to the callsite, so this can cause an assert.
    parser.L->cutOffLexing();
  }
}

Parser::StructureMarkerRAII::StructureMarkerRAII(Parser &parser,
                                                 const Token &tok)
    : StructureMarkerRAII(parser, tok.getLoc(),
                          getStructureMarkerKindForToken(tok)) {}

//===----------------------------------------------------------------------===//
// Primitive Parsing
//===----------------------------------------------------------------------===//

bool Parser::parseIdentifier(Identifier &Result, SourceLoc &Loc,
                             const Diagnostic &D, bool diagnoseDollarPrefix) {
  switch (Tok.getKind()) {
  case tok::kw_self:
  case tok::kw_Self:
  case tok::identifier:
    Loc = consumeIdentifier(Result, diagnoseDollarPrefix);
    return false;
  default:
    checkForInputIncomplete();
    diagnose(Tok, D);
    return true;
  }
}

bool Parser::parseSpecificIdentifier(StringRef expected, SourceLoc &loc,
                                     const Diagnostic &D) {
  if (Tok.getText() != expected) {
    diagnose(Tok, D);
    return true;
  }
  loc = consumeToken(tok::identifier);
  return false;
}

/// parseAnyIdentifier - Consume an identifier or operator if present and return
/// its name in Result.  Otherwise, emit an error and return true.
bool Parser::parseAnyIdentifier(Identifier &Result, SourceLoc &Loc,
                                const Diagnostic &D,
                                bool diagnoseDollarPrefix) {
  if (Tok.is(tok::identifier)) {
    Loc = consumeIdentifier(Result, diagnoseDollarPrefix);
    return false;
  }

  if (Tok.isAnyOperator()) {
    Result = Context.getIdentifier(Tok.getText());
    Loc = Tok.getLoc();
    consumeToken();
    return false;
  }

  // When we know we're supposed to get an identifier or operator, map the
  // postfix '!' to an operator name.
  if (Tok.is(tok::exclaim_postfix)) {
    Result = Context.getIdentifier(Tok.getText());
    Loc = Tok.getLoc();
    consumeToken(tok::exclaim_postfix);
    return false;
  }

  checkForInputIncomplete();

  if (Tok.isKeyword()) {
    diagnose(Tok, diag::keyword_cant_be_identifier, Tok.getText());
    diagnose(Tok, diag::backticks_to_escape)
      .fixItReplace(Tok.getLoc(), "`" + Tok.getText().str() + "`");
  } else {
    diagnose(Tok, D);
  }

  return true;
}

/// parseToken - The parser expects that 'K' is next in the input.  If so, it is
/// consumed and false is returned.
///
/// If the input is malformed, this emits the specified error diagnostic.
bool Parser::parseToken(tok K, SourceLoc &TokLoc, const Diagnostic &D) {
  if (Tok.is(K)) {
    TokLoc = consumeToken(K);
    return false;
  }

  checkForInputIncomplete();
  diagnose(Tok, D);
  return true;
}

bool Parser::parseMatchingToken(tok K, SourceLoc &TokLoc, Diagnostic ErrorDiag,
                                SourceLoc OtherLoc) {
  Diag<> OtherNote;
  switch (K) {
  case tok::r_paren:  OtherNote = diag::opening_paren;    break;
  case tok::r_square: OtherNote = diag::opening_bracket;  break;
  case tok::r_brace:  OtherNote = diag::opening_brace;    break;
  default:            llvm_unreachable("unknown matching token!"); break;
  }
  if (parseToken(K, TokLoc, ErrorDiag)) {
    diagnose(OtherLoc, OtherNote);

    TokLoc = getLocForMissingMatchingToken();
    return true;
  }

  return false;
}

bool Parser::parseUnsignedInteger(unsigned &Result, SourceLoc &Loc,
                                  const Diagnostic &D) {
  auto IntTok = Tok;
  if (parseToken(tok::integer_literal, Loc, D))
    return true;
  if (IntTok.getText().getAsInteger(0, Result)) {
    diagnose(IntTok.getLoc(), D);
    return true;
  }
  return false;
}

SourceLoc Parser::getLocForMissingMatchingToken() const {
  // At present, use the same location whether it's an error or whether
  // the matching token is missing.
  // Both cases supply a location for something the user didn't type.
  return getErrorOrMissingLoc();
}

SourceLoc Parser::getErrorOrMissingLoc() const {
  // The next token might start a new enclosing construct,
  // and SourceLoc's are always at the start of a token (for example, for
  // fixits, so use the previous token's SourceLoc and allow a subnode to end
  // right at the same place as its supernode.

  // The tricky case is when the previous token is an InterpolatedStringLiteral.
  // Then, there will be names in scope whose SourceLoc is *after* the
  // the location of a missing close brace.
  // ASTScope tree creation will have to cope.

  return PreviousLoc;
}

static bool tokIsStringInterpolationEOF(Token &Tok, tok RightK) {
  return Tok.is(tok::eof) && Tok.getText() == ")" && RightK == tok::r_paren;
}

Parser::ParseListItemResult
Parser::parseListItem(ParserStatus &Status, tok RightK, SourceLoc LeftLoc,
                      SourceLoc &RightLoc, bool AllowSepAfterLast,
                      llvm::function_ref<ParserStatus()> callback) {
  while (Tok.is(tok::comma)) {
    diagnose(Tok, diag::unexpected_separator, ",").fixItRemove(Tok.getLoc());
    consumeToken();
  }
  SourceLoc StartLoc = Tok.getLoc();

  Status |= callback();
  if (Tok.is(RightK))
    return ParseListItemResult::Finished;

  // If the lexer stopped with an EOF token whose spelling is ")", then this
  // is actually the tuple that is a string literal interpolation context.
  // Just accept the ")" and build the tuple as we usually do.
  if (tokIsStringInterpolationEOF(Tok, RightK)) {
    RightLoc = Tok.getLoc();
    return ParseListItemResult::FinishedInStringInterpolation;
  }
  // If we haven't made progress, or seeing any error, skip ahead.
  if (Tok.getLoc() == StartLoc || Status.isErrorOrHasCompletion()) {
    assert(Status.isErrorOrHasCompletion() && "no progress without error");
    skipListUntilDeclRBrace(LeftLoc, RightK, tok::comma);
    if (Tok.is(RightK) || Tok.isNot(tok::comma))
      return ParseListItemResult::Finished;
  }
  if (consumeIf(tok::comma)) {
    if (Tok.isNot(RightK))
      return ParseListItemResult::Continue;
    if (!AllowSepAfterLast) {
      diagnose(Tok, diag::unexpected_separator, ",").fixItRemove(PreviousLoc);
    }
    return ParseListItemResult::Finished;
  }
  // If we're in a comma-separated list, the next token is at the
  // beginning of a new line and can never start an element, break.
  if (Tok.isAtStartOfLine() &&
      (Tok.is(tok::r_brace) || isStartOfSwiftDecl() || isStartOfStmt())) {
    return ParseListItemResult::Finished;
  }
  // If we found EOF or such, bailout.
  if (Tok.isAny(tok::eof, tok::pound_endif)) {
    IsInputIncomplete = true;
    return ParseListItemResult::Finished;
  }

  diagnose(Tok, diag::expected_separator, ",")
      .fixItInsertAfter(PreviousLoc, ",");
  Status.setIsParseError();
  return ParseListItemResult::Continue;
}

ParserStatus
Parser::parseList(tok RightK, SourceLoc LeftLoc, SourceLoc &RightLoc,
                  bool AllowSepAfterLast, Diag<> ErrorDiag,
                  llvm::function_ref<ParserStatus()> callback) {
  if (Tok.is(RightK)) {
    RightLoc = consumeToken(RightK);
    return makeParserSuccess();
  }
  if (tokIsStringInterpolationEOF(Tok, RightK)) {
    RightLoc = Tok.getLoc();
    return makeParserSuccess();
  }

  ParserStatus Status;
  ParseListItemResult Result;
  do {
    Result = parseListItem(Status, RightK, LeftLoc, RightLoc, AllowSepAfterLast,
                           callback);
  } while (Result == ParseListItemResult::Continue);

  if (Result == ParseListItemResult::FinishedInStringInterpolation) {
    return Status;
  }

  if (Status.isErrorOrHasCompletion()) {
    // If we've already got errors, don't emit missing RightK diagnostics.
    if (Tok.is(RightK)) {
      RightLoc = consumeToken();
      // Don't propagate the error because we have recovered.
      if (!Status.hasCodeCompletion())
        Status = makeParserSuccess();
    } else {
      RightLoc = getLocForMissingMatchingToken();
    }
  } else if (parseMatchingToken(RightK, RightLoc, ErrorDiag, LeftLoc)) {
    Status.setIsParseError();
  }

  return Status;
}

Optional<StringRef>
Parser::getStringLiteralIfNotInterpolated(SourceLoc Loc,
                                          StringRef DiagText) {
  assert(Tok.is(tok::string_literal));

  // FIXME: Support extended escaping string literal.
  if (Tok.getCustomDelimiterLen()) {
    diagnose(Loc, diag::forbidden_extended_escaping_string, DiagText);
    return None;
  }

  SmallVector<Lexer::StringSegment, 1> Segments;
  L->getStringLiteralSegments(Tok, Segments);
  if (Segments.size() != 1 ||
      Segments.front().Kind == Lexer::StringSegment::Expr) {
    diagnose(Loc, diag::forbidden_interpolated_string, DiagText);
    return None;
  }

  return SourceMgr.extractText(CharSourceRange(Segments.front().Loc,
                                               Segments.front().Length));
}

struct ParserUnit::Implementation {
  LangOptions LangOpts;
  TypeCheckerOptions TypeCheckerOpts;
  SILOptions SILOpts;
  SearchPathOptions SearchPathOpts;
  ClangImporterOptions clangImporterOpts;
  symbolgraphgen::SymbolGraphOptions symbolGraphOpts;
  DiagnosticEngine Diags;
  ASTContext &Ctx;
  SourceFile *SF;
  std::unique_ptr<Parser> TheParser;

  Implementation(SourceManager &SM, SourceFileKind SFKind, unsigned BufferID,
                 const LangOptions &Opts, const TypeCheckerOptions &TyOpts,
                 const SILOptions &silOpts, StringRef ModuleName)
      : LangOpts(Opts),
        TypeCheckerOpts(TyOpts), SILOpts(silOpts), Diags(SM),
        Ctx(*ASTContext::get(LangOpts, TypeCheckerOpts, SILOpts, SearchPathOpts,
                             clangImporterOpts, symbolGraphOpts, SM, Diags)) {
    registerParseRequestFunctions(Ctx.evaluator);

    auto parsingOpts = SourceFile::getDefaultParsingOptions(LangOpts);
    parsingOpts |= ParsingFlags::DisableDelayedBodies;
    parsingOpts |= ParsingFlags::DisablePoundIfEvaluation;

    auto *M = ModuleDecl::create(Ctx.getIdentifier(ModuleName), Ctx);
    SF = new (Ctx) SourceFile(*M, SFKind, BufferID, parsingOpts);
    M->addAuxiliaryFile(*SF);
  }

  ~Implementation() {
    TheParser.reset();
    delete &Ctx;
  }
};

ParserUnit::ParserUnit(SourceManager &SM, SourceFileKind SFKind,
                       unsigned BufferID)
    : ParserUnit(SM, SFKind, BufferID, LangOptions(), TypeCheckerOptions(),
                 SILOptions(), "input") {}

ParserUnit::ParserUnit(SourceManager &SM, SourceFileKind SFKind,
                       unsigned BufferID, const LangOptions &LangOpts,
                       const TypeCheckerOptions &TypeCheckOpts,
                       const SILOptions &SILOpts, StringRef ModuleName)
    : Impl(*new Implementation(SM, SFKind, BufferID, LangOpts, TypeCheckOpts,
                               SILOpts, ModuleName)) {
  Impl.TheParser.reset(new Parser(BufferID, *Impl.SF, /*SIL=*/nullptr,
                                  /*PersistentState=*/nullptr));
}

ParserUnit::ParserUnit(SourceManager &SM, SourceFileKind SFKind,
                       unsigned BufferID, unsigned Offset, unsigned EndOffset)
    : Impl(*new Implementation(SM, SFKind, BufferID, LangOptions(),
                               TypeCheckerOptions(), SILOptions(), "input")) {

  std::unique_ptr<Lexer> Lex;
  Lex.reset(new Lexer(Impl.LangOpts, SM,
                      BufferID, &Impl.Diags,
                      LexerMode::Swift,
                      HashbangMode::Allowed,
                      CommentRetentionMode::None,
                      Offset, EndOffset));
  Impl.TheParser.reset(new Parser(std::move(Lex), *Impl.SF, /*SIL=*/nullptr,
                                  /*PersistentState=*/nullptr));
}

ParserUnit::~ParserUnit() {
  delete &Impl;
}

void ParserUnit::parse() {
  auto &P = getParser();
  auto &ctx = P.Context;

  SmallVector<ASTNode, 128> items;
  P.parseTopLevelItems(items);

  Optional<ArrayRef<Token>> tokensRef;
  if (auto tokens = P.takeTokenReceiver()->finalize())
    tokensRef = ctx.AllocateCopy(*tokens);

  auto result = SourceFileParsingResult{ctx.AllocateCopy(items), tokensRef,
                                        P.CurrentTokenHash};
  ctx.evaluator.cacheOutput(ParseSourceFileRequest{&P.SF}, std::move(result));
}

Parser &ParserUnit::getParser() {
  return *Impl.TheParser;
}

DiagnosticEngine &ParserUnit::getDiagnosticEngine() {
  return Impl.Diags;
}

const LangOptions &ParserUnit::getLangOptions() const {
  return Impl.LangOpts;
}

SourceFile &ParserUnit::getSourceFile() {
  return *Impl.SF;
}

ParsedDeclName swift::parseDeclName(StringRef name) {
  if (name.empty()) return ParsedDeclName();

  // Local function to handle the parsing of the base name + context.
  //
  // Returns true if an error occurred, without recording the base name.
  ParsedDeclName result;
  auto parseBaseName = [&](StringRef text) -> bool {
    // Split the text into context name and base name.
    StringRef contextName, baseName;
    std::tie(contextName, baseName) = text.rsplit('.');
    if (baseName.empty()) {
      baseName = contextName;
      contextName = StringRef();
    } else if (contextName.empty()) {
      return true;
    }

    auto isValidIdentifier = [](StringRef text) -> bool {
      return Lexer::isIdentifier(text) && text != "_";
    };

    // Make sure we have an identifier for the base name.
    if (!isValidIdentifier(baseName))
      return true;

    // If we have a context, make sure it is an identifier, or a series of
    // dot-separated identifiers.
    // FIXME: What about generic parameters?
    if (!contextName.empty()) {
      StringRef first;
      StringRef rest = contextName;
      do {
        std::tie(first, rest) = rest.split('.');
        if (!isValidIdentifier(first))
          return true;
      } while (!rest.empty());
    }

    // Record the results.
    result.ContextName = contextName;
    result.BaseName = baseName;
    return false;
  };

  // If this is not a function name, just parse the base name and
  // we're done.
  if (name.back() != ')') {
    if (Lexer::isOperator(name))
      result.BaseName = name;
    else if (parseBaseName(name))
      return ParsedDeclName();
    return result;
  }

  // We have a function name.
  result.IsFunctionName = true;

  // Split the base name from the parameters.
  StringRef baseName, parameters;
  std::tie(baseName, parameters) = name.split('(');
  if (parameters.empty()) return ParsedDeclName();

  // If the base name is prefixed by "getter:" or "setter:", it's an
  // accessor.
  if (baseName.startswith("getter:")) {
    result.IsGetter = true;
    result.IsFunctionName = false;
    baseName = baseName.substr(7);
  } else if (baseName.startswith("setter:")) {
    result.IsSetter = true;
    result.IsFunctionName = false;
    baseName = baseName.substr(7);
  }

  // If the base name is prefixed by "subscript", it's an subscript.
  if (baseName == "subscript") {
    result.IsSubscript = true;
  }

  // Parse the base name.
  if (parseBaseName(baseName)) return ParsedDeclName();

  parameters = parameters.drop_back(); // ')'
  if (parameters.empty()) return result;

  if (parameters.back() != ':')
    return ParsedDeclName();

  bool isMember = !result.ContextName.empty();
  do {
    StringRef NextParam;
    std::tie(NextParam, parameters) = parameters.split(':');

    if (!Lexer::isIdentifier(NextParam))
      return ParsedDeclName();
    if (NextParam == "_") {
      result.ArgumentLabels.push_back("");
    } else if (isMember && NextParam == "self") {
      // For a member, "self" indicates the self parameter. There can
      // only be one such parameter.
      if (result.SelfIndex) return ParsedDeclName();
      result.SelfIndex = result.ArgumentLabels.size();
    } else {
      result.ArgumentLabels.push_back(NextParam);
    }
  } while (!parameters.empty());

  return result;
}

DeclName ParsedDeclName::formDeclName(ASTContext &ctx, bool isSubscript,
                                      bool isCxxClassTemplateSpec) const {
  return formDeclNameRef(ctx, isSubscript, isCxxClassTemplateSpec).getFullName();
}

DeclNameRef ParsedDeclName::formDeclNameRef(ASTContext &ctx,
                                            bool isSubscript,
                                            bool isCxxClassTemplateSpec) const {
  return swift::formDeclNameRef(ctx, BaseName, ArgumentLabels, IsFunctionName,
                                /*IsInitializer=*/true, isSubscript,
                                isCxxClassTemplateSpec);
}

DeclName swift::formDeclName(ASTContext &ctx,
                             StringRef baseName,
                             ArrayRef<StringRef> argumentLabels,
                             bool isFunctionName,
                             bool isInitializer,
                             bool isSubscript,
                             bool isCxxClassTemplateSpec) {
  return formDeclNameRef(ctx, baseName, argumentLabels, isFunctionName,
                         isInitializer, isSubscript,
                         isCxxClassTemplateSpec).getFullName();
}

DeclNameRef swift::formDeclNameRef(ASTContext &ctx,
                                   StringRef baseName,
                                   ArrayRef<StringRef> argumentLabels,
                                   bool isFunctionName,
                                   bool isInitializer,
                                   bool isSubscript,
                                   bool isCxxClassTemplateSpec) {
  // We cannot import when the base name is not an identifier.
  if (baseName.empty())
    return DeclNameRef();

  if (!Lexer::isIdentifier(baseName) && !Lexer::isOperator(baseName) &&
      !isCxxClassTemplateSpec)
    return DeclNameRef();

  // Get the identifier for the base name. Special-case `init`.
  DeclBaseName baseNameId;
  if (isInitializer && baseName == "init")
    baseNameId = DeclBaseName::createConstructor();
  else if (isSubscript && baseName == "subscript")
    baseNameId = DeclBaseName::createSubscript();
  else
    baseNameId = ctx.getIdentifier(baseName);

  // For non-functions, just use the base name.
  if (!isFunctionName && !baseNameId.isSubscript())
    return DeclNameRef(baseNameId);

  // For functions, we need to form a complete name.

  // Convert the argument names.
  SmallVector<Identifier, 4> argumentLabelIds;
  for (auto argName : argumentLabels) {
    if (argumentLabels.empty() || !Lexer::isIdentifier(argName)) {
      argumentLabelIds.push_back(Identifier());
      continue;
    }

    argumentLabelIds.push_back(ctx.getIdentifier(argName));
  }

  // Build the result.
  return DeclNameRef({ ctx, baseNameId, argumentLabelIds });
}

void PrettyStackTraceParser::print(llvm::raw_ostream &out) const {
  out << "With parser at source location: ";
  P.Tok.getLoc().print(out, P.Context.SourceMgr);
  out << '\n';
}
