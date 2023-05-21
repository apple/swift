//===--- Parser.h - Swift Language Parser -----------------------*- C++ -*-===//
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
//  This file defines the Parser interface.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_PARSER_H
#define SWIFT_PARSER_H

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTNode.h"
#include "swift/AST/DiagnosticsParse.h"
#include "swift/AST/Expr.h"
#include "swift/AST/LayoutConstraint.h"
#include "swift/AST/ParseRequests.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Stmt.h"
#include "swift/Basic/OptionSet.h"
#include "swift/Config.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/ParserPosition.h"
#include "swift/Parse/ParserResult.h"
#include "swift/Parse/PatternBindingState.h"
#include "swift/Parse/PersistentParserState.h"
#include "swift/Parse/Token.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"

namespace llvm {
  template <typename...  PTs> class PointerUnion;
}

namespace swift {
  class IdentTypeRepr;
  class CodeCompletionCallbacks;
  class DoneParsingCallback;
  class IDEInspectionCallbacksFactory;
  class DefaultArgumentInitializer;
  class DiagnosticEngine;
  class Expr;
  class Lexer;
  class PersistentParserState;
  class RequirementRepr;
  class SILParserStateBase;
  class ScopeInfo;
  class SingleValueStmtExpr;
  class SourceManager;
  class TupleType;
  class TypeLoc;
  class UUID;
  
  struct EnumElementInfo;

  /// Different contexts in which BraceItemList are parsed.
  enum class BraceItemListKind {
    /// A statement list terminated by a closing brace. The default.
    Brace,
    /// A statement list in a case block. The list is terminated
    /// by a closing brace or a 'case' or 'default' label.
    Case,
    /// The top-level of a file, when not in parse-as-library mode (i.e. the
    /// repl or a script).
    TopLevelCode,
    /// The top-level of a file, when in parse-as-library mode.
    TopLevelLibrary,
    /// The body of the inactive clause of an #if/#else/#endif block
    InactiveConditionalBlock,
    /// The body of the active clause of an #if/#else/#endif block
    ActiveConditionalBlock,
    /// The top-level of a macro expansion "file".
    MacroExpansion,
  };

/// The receiver will be fed with consumed tokens while parsing. The main purpose
/// is to generate a corrected token stream for tooling support like syntax
/// coloring.
class ConsumeTokenReceiver {
public:
  /// This is called when a token is consumed.
  virtual void receive(const Token &Tok) {}

  /// This is called to update the kind of a token whose start location is Loc.
  virtual void registerTokenKindChange(SourceLoc Loc, tok NewKind) {};

  /// This is called when a source file is fully parsed. It returns the
  /// finalized vector of tokens, or \c None if the receiver isn't configured to
  /// record them.
  virtual Optional<std::vector<Token>> finalize() { return None; }

  virtual ~ConsumeTokenReceiver() = default;
};

/// The role of the elements in a given #if
enum class IfConfigElementsRole {
  // Parse normally.
  Normal,
  // Parse, but only for syntax. Throw away the results.
  SyntaxOnly,
  // Already skipped; no need to parse anything.
  Skipped
};

/// The main class used for parsing a source file (.swift or .sil).
///
/// Rather than instantiating a Parser yourself, use one of the parsing APIs
/// provided in Subsystems.h.
class Parser {
  Parser(const Parser&) = delete;
  void operator=(const Parser&) = delete;

  bool IsInputIncomplete = false;
  std::vector<Token> SplitTokens;

public:
  SourceManager &SourceMgr;
  DiagnosticEngine &Diags;
  SourceFile &SF;
  Lexer *L;
  SILParserStateBase *SIL; // Non-null when parsing SIL decls.
  PersistentParserState *State;
  std::unique_ptr<PersistentParserState> OwnedState;
  DeclContext *CurDeclContext;
  ASTContext &Context;
  CodeCompletionCallbacks *CodeCompletionCallbacks = nullptr;
  DoneParsingCallback *DoneParsingCallback = nullptr;
  std::vector<Located<std::vector<ParamDecl*>>> AnonClosureVars;

  /// The current token hash, or \c None if the parser isn't computing a hash
  /// for the token stream.
  Optional<StableHasher> CurrentTokenHash;

  void recordTokenHash(const Token Tok) {
    if (!Tok.getText().empty())
      recordTokenHash(Tok.getText());
  }

  void recordTokenHash(StringRef token);

  PatternBindingState InBindingPattern = PatternBindingState::NotInBinding;

  /// Whether this context has an async attribute.
  bool InPatternWithAsyncAttribute = false;

  bool InPoundLineEnvironment = false;
  bool InPoundIfEnvironment = false;
  /// ASTScopes are not created in inactive clauses and lookups to decls will fail.
  bool InInactiveClauseEnvironment = false;
  bool InSwiftKeyPath = false;

  /// Whether we should delay parsing nominal type, extension, and function
  /// bodies.
  bool isDelayedParsingEnabled() const;

  /// Whether to evaluate the conditions of #if decls, meaning that the bodies
  /// of any active clauses are hoisted such that they become sibling nodes with
  /// the #if decl.
  bool shouldEvaluatePoundIfDecls() const;

  void setCodeCompletionCallbacks(class CodeCompletionCallbacks *Callbacks) {
    CodeCompletionCallbacks = Callbacks;
  }

  void setDoneParsingCallback(class DoneParsingCallback *Callback) {
    this->DoneParsingCallback = Callback;
  }

  bool isIDEInspectionFirstPass() const {
    return SourceMgr.hasIDEInspectionTargetBuffer() && !DoneParsingCallback;
  }

  bool allowTopLevelCode() const;

  bool isInMacroExpansion(SourceLoc loc) const;

  const std::vector<Token> &getSplitTokens() const { return SplitTokens; }

  void markSplitToken(tok Kind, StringRef Txt);

  /// Returns true if the parser reached EOF with incomplete source input, due
  /// for example, a missing right brace.
  bool isInputIncomplete() const { return IsInputIncomplete; }

  void checkForInputIncomplete() {
    IsInputIncomplete = IsInputIncomplete ||
      // Check whether parser reached EOF but the real EOF, not the end of a
      // string interpolation segment.
      (Tok.is(tok::eof) && Tok.getText() != ")");
  }

  /// This is the current token being considered by the parser.
  Token Tok;

  /// The receiver to collect all consumed tokens.
  ConsumeTokenReceiver *TokReceiver;

  /// The location of the previous token.
  SourceLoc PreviousLoc;

  /// Use this to assert that the parser has advanced the lexing location, e.g.
  /// before a specific parser function has returned.
  class AssertParserMadeProgressBeforeLeavingScopeRAII {
    Parser &P;
    SourceLoc InitialLoc;
  public:
    AssertParserMadeProgressBeforeLeavingScopeRAII(Parser &parser) : P(parser) {
      InitialLoc = P.Tok.getLoc();
    }
    ~AssertParserMadeProgressBeforeLeavingScopeRAII() {
      assert(InitialLoc != P.Tok.getLoc() &&
        "parser did not make progress, this can result in infinite loop");
    }
  };

  /// A RAII object for temporarily changing CurDeclContext.
  class ContextChange {
  protected:
    Parser &P;
    DeclContext *OldContext; // null signals that this has been popped

    ContextChange(const ContextChange &) = delete;
    ContextChange &operator=(const ContextChange &) = delete;

  public:
    ContextChange(Parser &P, DeclContext *DC)
      : P(P), OldContext(P.CurDeclContext) {
      assert(DC && "pushing null context?");
      P.CurDeclContext = DC;
    }

    /// Prematurely pop the DeclContext installed by the constructor.
    /// Makes the destructor a no-op.
    void pop() {
      assert(OldContext && "already popped context!");
      popImpl();
      OldContext = nullptr;
    }

    ~ContextChange() {
      if (OldContext) popImpl();
    }

  private:
    void popImpl() {
      P.CurDeclContext = OldContext;
    }
  };

  /// A RAII object for parsing a new local context.
  class ParseFunctionBody {
  private:
    ContextChange CC;
  public:
    ParseFunctionBody(Parser &P, DeclContext *DC) : CC(P, DC) {
      assert(!isa<TopLevelCodeDecl>(DC) &&
             "top-level code should be parsed using TopLevelCodeContext!");
    }

    void pop() {
      CC.pop();
    }
  };

  /// Describes the kind of a lexical structure marker, indicating
  /// what kind of structural element we started parsing at a
  /// particular location.
  enum class StructureMarkerKind : uint8_t {
    /// The start of a declaration.
    Declaration,
    /// The start of a statement.
    Statement,
    /// An open parentheses.
    OpenParen,
    /// An open brace.
    OpenBrace,
    /// An open square bracket.
    OpenSquare,
    /// An #if conditional clause.
    IfConfig,
  };

  /// A structure marker, which identifies the location at which the
  /// parser saw an entity it is parsing.
  struct StructureMarker {
    /// The location at which the marker occurred.
    SourceLoc Loc;

    /// The kind of marker.
    StructureMarkerKind Kind;

    /// The leading whitespace for this marker, if it has already been
    /// computed.
    Optional<StringRef> LeadingWhitespace;
  };

  /// An RAII object that notes when we have seen a structure marker.
  class StructureMarkerRAII {
    Parser &P;

    /// Max nesting level
    // TODO: customizable.
    enum { MaxDepth = 256 };

    StructureMarkerRAII(Parser &parser) : P(parser) {}

  public:
    StructureMarkerRAII(Parser &parser, SourceLoc loc,
                        StructureMarkerKind kind);

    StructureMarkerRAII(Parser &parser, const Token &tok);

    ~StructureMarkerRAII() { P.StructureMarkers.pop_back(); }
  };
  friend class StructureMarkerRAII;

  /// The stack of structure markers indicating the locations of
  /// structural elements actively being parsed, including the start
  /// of declarations, statements, and opening operators of various
  /// kinds.
  ///
  /// This vector is managed by \c StructureMarkerRAII objects.
  llvm::SmallVector<StructureMarker, 16> StructureMarkers;

  /// Maps of macro name and version to availability specifications.
  typedef llvm::DenseMap<llvm::VersionTuple,
                         SmallVector<AvailabilitySpec *, 4>>
                        AvailabilityMacroVersionMap;
  typedef llvm::DenseMap<StringRef, AvailabilityMacroVersionMap>
                        AvailabilityMacroMap;

  /// Cache of the availability macros parsed from the command line arguments.
  /// Organized as two nested \c DenseMap keyed first on the macro name then
  /// the macro version. This structure allows to peek at macro names before
  /// parsing a version tuple.
  AvailabilityMacroMap AvailabilityMacros;

  /// Has \c AvailabilityMacros been computed?
  bool AvailabilityMacrosComputed = false;

public:
  Parser(unsigned BufferID, SourceFile &SF, DiagnosticEngine* LexerDiags,
         SILParserStateBase *SIL, PersistentParserState *PersistentState);
  Parser(unsigned BufferID, SourceFile &SF, SILParserStateBase *SIL,
         PersistentParserState *PersistentState = nullptr);
  Parser(std::unique_ptr<Lexer> Lex, SourceFile &SF,
         SILParserStateBase *SIL = nullptr,
         PersistentParserState *PersistentState = nullptr);
  ~Parser();

  /// Returns true if the buffer being parsed is allowed to contain SIL.
  bool isInSILMode() const;

  /// Retrieve the token receiver from the parser once it has finished parsing.
  std::unique_ptr<ConsumeTokenReceiver> takeTokenReceiver() {
    assert(Tok.is(tok::eof) && "not done parsing yet");
    auto *receiver = TokReceiver;
    TokReceiver = nullptr;
    return std::unique_ptr<ConsumeTokenReceiver>(receiver);
  }

  //===--------------------------------------------------------------------===//
  // Routines to save and restore parser state.

  ParserPosition getParserPosition() {
    return ParserPosition(L->getStateForBeginningOfToken(Tok),
                          PreviousLoc);
  }

  ParserPosition getParserPosition(SourceLoc loc, SourceLoc previousLoc) {
    return ParserPosition(L->getStateForBeginningOfTokenLoc(loc), previousLoc);
  }

  void restoreParserPosition(ParserPosition PP, bool enableDiagnostics = false) {
    L->restoreState(PP.LS, enableDiagnostics);
    L->lex(Tok);
    PreviousLoc = PP.PreviousLoc;
  }

  void backtrackToPosition(ParserPosition PP) {
    assert(PP.isValid());
    L->backtrackToState(PP.LS);
    L->lex(Tok);
    PreviousLoc = PP.PreviousLoc;
  }

  /// RAII object that, when it is destructed, restores the parser and lexer to
  /// their positions at the time the object was constructed.  Will not jump
  /// forward in the token stream.
  /// Actual uses of the backtracking scope should choose either \c
  /// BacktrackingScope, which will always backtrack or \c
  /// CancellableBacktrackingScope which can be cancelled.
  class BacktrackingScopeImpl {
  protected:
    Parser &P;
    ParserPosition PP;
    DiagnosticTransaction DT;
    bool Backtrack = true;

    /// A token receiver used by the parser in the back tracking scope. This
    /// receiver will save any consumed tokens during this back tracking scope.
    /// After the scope ends, it either transfers the saved tokens to the old receiver
    /// or discard them.
    struct DelayedTokenReceiver: ConsumeTokenReceiver {
      /// Keep track of the old token receiver in the parser so that we can recover
      /// after the backtracking sope ends.
      llvm::SaveAndRestore<ConsumeTokenReceiver*> savedConsumer;

      // Whether the tokens should be transferred to the original receiver.
      // When the back tracking scope will actually back track, this should be false;
      // otherwise true.
      bool shouldTransfer = false;
      std::vector<Token> delayedTokens;
      DelayedTokenReceiver(ConsumeTokenReceiver *&receiver):
        savedConsumer(receiver, this) {}
      void receive(const Token &tok) override { delayedTokens.push_back(tok); }
      Optional<std::vector<Token>> finalize() override {
        llvm_unreachable("Cannot finalize a DelayedTokenReceiver");
      }
      ~DelayedTokenReceiver() {
        if (!shouldTransfer)
          return;
        for (auto tok: delayedTokens) {
          savedConsumer.get()->receive(tok);
        }
      }
    } TempReceiver;

    BacktrackingScopeImpl(Parser &P)
        : P(P), PP(P.getParserPosition()), DT(P.Diags),
          TempReceiver(P.TokReceiver) { }

  public:
    ~BacktrackingScopeImpl();
    bool willBacktrack() const { return Backtrack; }
  };

  /// A backtracking scope that will always backtrack when destructed.
  class BacktrackingScope final : public BacktrackingScopeImpl {
  public:
    BacktrackingScope(Parser &P) : BacktrackingScopeImpl(P) { }
  };

  /// A backtracking scope whose backtracking can be disabled by calling
  /// \c cancelBacktrack.
  class CancellableBacktrackingScope final : public BacktrackingScopeImpl {
  public:
    CancellableBacktrackingScope(Parser &P) : BacktrackingScopeImpl(P) { }

    void cancelBacktrack();
  };

  /// RAII object that, when it is destructed, restores the parser and lexer to
  /// their positions at the time the object was constructed.
  struct ParserPositionRAII {
  private:
    Parser &P;
    ParserPosition PP;

  public:
    ParserPositionRAII(Parser &P) : P(P), PP(P.getParserPosition()) {}

    ~ParserPositionRAII() {
      P.restoreParserPosition(PP);
    }
  };

  //===--------------------------------------------------------------------===//
  // Utilities

  /// Return the next token that will be installed by \c consumeToken.
  const Token &peekToken();

  /// Consumes K tokens within a backtracking scope before calling \c f and
  /// providing it with the backtracking scope. Unless if the backtracking is
  /// explicitly cancelled, the parser's token state is restored after \c f
  /// returns.
  ///
  /// \param K The number of tokens ahead to skip. Zero is the current token.
  /// \param f The function to apply after skipping K tokens ahead.
  ///          The value returned by \c f will be returned by \c peekToken
  ///           after the parser is rolled back.
  /// \returns the value returned by \c f
  /// \note When calling, you may need to specify the \c Val type
  ///       explicitly as a type parameter.
  template <typename Val>
  Val lookahead(unsigned char K,
                llvm::function_ref<Val(CancellableBacktrackingScope &)> f) {
    CancellableBacktrackingScope backtrackScope(*this);

    for (unsigned char i = 0; i < K; ++i)
      consumeToken();

    return f(backtrackScope);
  }

  /// Discard the current token. This will avoid interface hashing or updating
  /// the previous loc. Only should be used if you've completely re-lexed
  /// a different token at that position.
  SourceLoc discardToken();

  /// Consume a token that we created on the fly to correct the original token
  /// stream from lexer.
  void consumeExtraToken(Token K);
  SourceLoc consumeTokenWithoutFeedingReceiver();
  SourceLoc consumeToken();
  SourceLoc consumeToken(tok K) {
    assert(Tok.is(K) && "Consuming wrong token kind");
    return consumeToken();
  }

  SourceLoc consumeIdentifier(Identifier &Result, bool diagnoseDollarPrefix) {
    assert(Tok.isAny(tok::identifier, tok::kw_self, tok::kw_Self));
    assert(Result.empty());
    Result = Context.getIdentifier(Tok.getText());

    if (Tok.getText()[0] == '$')
      diagnoseDollarIdentifier(Tok, diagnoseDollarPrefix);

    return consumeToken();
  }

  SourceLoc consumeArgumentLabel(Identifier &Result,
                                 bool diagnoseDollarPrefix) {
    assert(Tok.canBeArgumentLabel());
    assert(Result.empty());
    if (!Tok.is(tok::kw__)) {
      Tok.setKind(tok::identifier);
      Result = Context.getIdentifier(Tok.getText());

      if (Tok.getText()[0] == '$')
        diagnoseDollarIdentifier(Tok, diagnoseDollarPrefix);
    }
    return consumeToken();
  }

  /// When we have a token that is an identifier starting with '$',
  /// diagnose it if not permitted in this mode.
  /// \param diagnoseDollarPrefix Whether to diagnose dollar-prefixed
  /// identifiers in addition to a standalone '$'.
  void diagnoseDollarIdentifier(const Token &tok,
                                bool diagnoseDollarPrefix) {
    assert(tok.getText()[0] == '$');

    // If '$' is not guarded by backticks, offer
    // to replace it with '`$`'.
    if (Tok.getRawText() == "$") {
      diagnose(Tok.getLoc(), diag::standalone_dollar_identifier)
          .fixItReplace(Tok.getLoc(), "`$`");
      return;
    }

    if (!diagnoseDollarPrefix)
      return;

    if (tok.getText().size() == 1 || Context.LangOpts.EnableDollarIdentifiers ||
        isInSILMode() || L->isSwiftInterface() ||
        isInMacroExpansion(tok.getLoc()))
      return;

    diagnose(tok.getLoc(), diag::dollar_identifier_decl,
             Context.getIdentifier(tok.getText()));
  }

  /// Retrieve the location just past the end of the previous
  /// source location.
  SourceLoc getEndOfPreviousLoc() const;

  /// If the current token is the specified kind, consume it and
  /// return true.  Otherwise, return false without consuming it.
  bool consumeIf(tok K) {
    if (Tok.isNot(K)) return false;
    consumeToken(K);
    return true;
  }

  /// If the current token is the specified kind, consume it and
  /// return true.  Otherwise, return false without consuming it.
  bool consumeIf(tok K, SourceLoc &consumedLoc) {
    if (Tok.isNot(K)) return false;
    consumedLoc = consumeToken(K);
    return true;
  }

  bool consumeIfNotAtStartOfLine(tok K) {
    if (Tok.isAtStartOfLine()) return false;
    return consumeIf(K);
  }

  bool isContextualYieldKeyword() {
    return (Tok.isContextualKeyword("yield") &&
            isa<AccessorDecl>(CurDeclContext) &&
            cast<AccessorDecl>(CurDeclContext)->isCoroutine());
  }

  /// `discard self` is the only valid phrase, but we peek ahead for just any
  /// identifier after `discard` to determine if it's the statement. This helps
  /// us avoid interpreting `discard(self)` as the statement and not a call.
  /// We also want to be mindful of statements like `discard ++ something` where
  /// folks have defined a custom operator returning void.
  ///
  /// Later, type checking will verify that you're discarding the right thing
  /// so that when people make a mistake, thinking they can `discard x` we give
  /// a nice diagnostic.
  bool isContextualDiscardKeyword() {
    // must be `discard` ...
    if (!(Tok.isContextualKeyword("_forget") // NOTE: support for deprecated _forget
        || Tok.isContextualKeyword("discard")))
      return false;

    // followed by either an identifier, `self`, or `Self`.
    return !peekToken().isAtStartOfLine()
      && peekToken().isAny(tok::identifier, tok::kw_self, tok::kw_Self);
  }
  
  /// Read tokens until we get to one of the specified tokens, then
  /// return without consuming it.  Because we cannot guarantee that the token
  /// will ever occur, this skips to some likely good stopping point.
  ParserStatus skipUntil(tok T1, tok T2 = tok::NUM_TOKENS);
  void skipUntilAnyOperator();

  /// Skip until a token that starts with '>', and consume it if found.
  /// Applies heuristics that are suitable when trying to find the end of a list
  /// of generic parameters, generic arguments, or list of types in a protocol
  /// composition.
  SourceLoc skipUntilGreaterInTypeList(bool protocolComposition = false);

  /// skipUntilDeclStmtRBrace - Skip to the next decl or '}'.
  void skipUntilDeclRBrace();

  template <typename ...T>
  void skipUntilDeclStmtRBrace(T... K) {
    while (Tok.isNot(K..., tok::eof, tok::r_brace, tok::pound_endif,
                     tok::pound_else, tok::pound_elseif,
                     tok::code_complete) &&
           !isStartOfStmt() &&
           !isStartOfSwiftDecl(/*allowPoundIfAttributes=*/true)) {
      skipSingle();
    }
  }

  void skipUntilDeclRBrace(tok T1, tok T2);
  
  void skipListUntilDeclRBrace(SourceLoc startLoc, tok T1, tok T2);
  
  /// Skip a single token, but match parentheses, braces, and square brackets.
  ///
  /// Note: this does \em not match angle brackets ("<" and ">")! These are
  /// matched in the source when they refer to a generic type,
  /// but not when used as comparison operators.
  ///
  /// Returns a parser status that can capture whether a code completion token
  /// was returned.
  ParserStatus skipSingle();

  /// Skip until the next '#else', '#endif' or until eof.
  void skipUntilConditionalBlockClose();

  /// Skip until either finding \c T1 or reaching the end of the line.
  ///
  /// This uses \c skipSingle and so matches parens etc. After calling, one or
  /// more of the following will be true: Tok.is(T1), Tok.isStartOfLine(),
  /// Tok.is(tok::eof). The "or more" case is the first two: if the next line
  /// starts with T1.
  ///
  /// \returns true if there is an instance of \c T1 on the current line (this
  /// avoids the foot-gun of not considering T1 starting the next line for a
  /// plain Tok.is(T1) check).
  bool skipUntilTokenOrEndOfLine(tok T1, tok T2 = tok::NUM_TOKENS);

  /// Skip over SIL decls until we encounter the start of a Swift decl or eof.
  void skipSILUntilSwiftDecl();

  /// Skip over any attribute.
  void skipAnyAttribute();

  /// Parse an #endif.
  bool parseEndIfDirective(SourceLoc &Loc);

  /// Given that the current token is a string literal,
  /// - if it is not interpolated, returns the contents;
  /// - otherwise, diagnoses and returns None.
  ///
  /// \param Loc where to diagnose.
  /// \param DiagText name for the string literal in the diagnostic.
  Optional<StringRef>
  getStringLiteralIfNotInterpolated(SourceLoc Loc, StringRef DiagText);

  /// Returns true to indicate that experimental concurrency syntax should be
  /// parsed if the parser is generating only a syntax tree or if the user has
  /// passed the `-enable-experimental-concurrency` flag to the frontend.
  bool shouldParseExperimentalConcurrency() const {
    return Context.LangOpts.EnableExperimentalConcurrency;
  }

public:
  InFlightDiagnostic diagnose(SourceLoc Loc, Diagnostic Diag) {
    if (Diags.isDiagnosticPointsToFirstBadToken(Diag.getID()) &&
        Loc == Tok.getLoc() && Tok.isAtStartOfLine())
      Loc = getEndOfPreviousLoc();
    return Diags.diagnose(Loc, Diag);
  }

  InFlightDiagnostic diagnose(Token Tok, Diagnostic Diag) {
    return diagnose(Tok.getLoc(), Diag);
  }

  template<typename ...DiagArgTypes, typename ...ArgTypes>
  InFlightDiagnostic diagnose(SourceLoc Loc, Diag<DiagArgTypes...> DiagID,
                              ArgTypes &&...Args) {
    return diagnose(Loc, Diagnostic(DiagID, std::forward<ArgTypes>(Args)...));
  }

  template<typename ...DiagArgTypes, typename ...ArgTypes>
  InFlightDiagnostic diagnose(Token Tok, Diag<DiagArgTypes...> DiagID,
                              ArgTypes &&...Args) {
    return diagnose(Tok.getLoc(),
                    Diagnostic(DiagID, std::forward<ArgTypes>(Args)...));
  }
    
  /// Add a fix-it to remove the space in consecutive identifiers.
  /// Add a camel-cased option if it is different than the first option.
  void diagnoseConsecutiveIDs(StringRef First, SourceLoc FirstLoc,
                              StringRef DeclKindName);

  bool startsWithSymbol(Token Tok, char symbol) {
    return (Tok.isAnyOperator() || Tok.isPunctuation()) &&
           Tok.getText()[0] == symbol;
  }
  /// Check whether the current token starts with '<'.
  bool startsWithLess(Token Tok) { return startsWithSymbol(Tok, '<'); }

  /// Check whether the current token starts with '>'.
  bool startsWithGreater(Token Tok) { return startsWithSymbol(Tok, '>'); }

  /// Check whether the current token starts with '...'.
  bool startsWithEllipsis(Token Tok);

  /// Check whether the current token starts with a multi-line string delimiter.
  bool startsWithMultilineStringDelimiter(Token Tok) {
    return Tok.getText().ltrim('#').startswith("\"\"\"");
  }

  /// Returns true if token is an identifier with the given value.
  bool isIdentifier(Token Tok, StringRef value) {
    return Tok.is(tok::identifier) && Tok.getText() == value;
  }

  /// Consume the starting '<' of the current token, which may either
  /// be a complete '<' token or some kind of operator token starting with '<',
  /// e.g., '<>'.
  SourceLoc consumeStartingLess();

  /// Consume the starting '>' of the current token, which may either
  /// be a complete '>' token or some kind of operator token starting with '>',
  /// e.g., '>>'.
  SourceLoc consumeStartingGreater();

  /// Consume the starting '...' of the current token, which may either be a
  /// complete '...' token or some kind of operator token starting with '...',
  /// e.g '...>'.
  SourceLoc consumeStartingEllipsis();

  /// Consume the starting character of the current token, and split the
  /// remainder of the token into a new token (or tokens).
  SourceLoc
  consumeStartingCharacterOfCurrentToken(tok Kind = tok::oper_binary_unspaced,
                                         size_t Len = 1);

  //===--------------------------------------------------------------------===//
  // Primitive Parsing

  /// Consume an identifier (but not an operator) if present and return
  /// its name in \p Result.  Otherwise, emit an error.
  ///
  /// \returns false on success, true on error.
  bool parseIdentifier(Identifier &Result, SourceLoc &Loc, const Diagnostic &D,
                       bool diagnoseDollarPrefix);

  /// Consume an identifier with a specific expected name.  This is useful for
  /// contextually sensitive keywords that must always be present.
  bool parseSpecificIdentifier(StringRef expected, SourceLoc &Loc,
                               const Diagnostic &D);

  template<typename ...DiagArgTypes, typename ...ArgTypes>
  bool parseIdentifier(Identifier &Result, SourceLoc &L,
                       bool diagnoseDollarPrefix, Diag<DiagArgTypes...> ID,
                       ArgTypes... Args) {
    return parseIdentifier(Result, L, Diagnostic(ID, Args...),
                           diagnoseDollarPrefix);
  }
  
  template<typename ...DiagArgTypes, typename ...ArgTypes>
  bool parseSpecificIdentifier(StringRef expected,
                               Diag<DiagArgTypes...> ID, ArgTypes... Args) {
    SourceLoc L;
    return parseSpecificIdentifier(expected, L, Diagnostic(ID, Args...));
  }

  /// Consume an identifier or operator if present and return its name
  /// in \p Result.  Otherwise, emit an error and return true.
  bool parseAnyIdentifier(Identifier &Result, SourceLoc &Loc,
                          const Diagnostic &D, bool diagnoseDollarPrefix);

  template<typename ...DiagArgTypes, typename ...ArgTypes>
  bool parseAnyIdentifier(Identifier &Result, bool diagnoseDollarPrefix,
                          Diag<DiagArgTypes...> ID, ArgTypes... Args) {
    SourceLoc L;
    return parseAnyIdentifier(Result, L, Diagnostic(ID, Args...),
                              diagnoseDollarPrefix);
  }

  /// \brief Parse an unsigned integer and returns it in \p Result. On failure
  /// emit the specified error diagnostic, and a note at the specified note
  /// location.
  bool parseUnsignedInteger(unsigned &Result, SourceLoc &Loc,
                            const Diagnostic &D);

  /// The parser expects that \p K is next token in the input.  If so,
  /// it is consumed and false is returned.
  ///
  /// If the input is malformed, this emits the specified error diagnostic.
  bool parseToken(tok K, SourceLoc &TokLoc, const Diagnostic &D);
  
  template<typename ...DiagArgTypes, typename ...ArgTypes>
  bool parseToken(tok K, Diag<DiagArgTypes...> ID, ArgTypes... Args) {
    SourceLoc L;
    return parseToken(K, L, Diagnostic(ID, Args...));
  }
  template<typename ...DiagArgTypes, typename ...ArgTypes>
  bool parseToken(tok K, SourceLoc &L,
                  Diag<DiagArgTypes...> ID, ArgTypes... Args) {
    return parseToken(K, L, Diagnostic(ID, Args...));
  }
  
  /// Parse the specified expected token and return its location on success.  On failure, emit the specified
  /// error diagnostic,  a note at the specified note location, and return the location of the previous token.
  bool parseMatchingToken(tok K, SourceLoc &TokLoc, Diagnostic ErrorDiag,
                          SourceLoc OtherLoc);

  /// Returns the proper location for a missing right brace, parenthesis, etc.
  SourceLoc getLocForMissingMatchingToken() const;

  /// When encountering an error or a missing matching token (e.g. '}'), return
  /// the location to use for it. This value should be at the last token in
  /// the ASTNode being parsed so that it nests within any enclosing nodes, and,
  /// for ASTScope lookups, it does not precede any identifiers to be looked up.
  /// However, the latter case does not hold when  parsing an interpolated
  /// string literal because there may be identifiers to be looked up in the
  /// literal and their locations will not precede the location of a missing
  /// close brace.
  SourceLoc getErrorOrMissingLoc() const;

  enum class ParseListItemResult {
    /// There are more list items to parse.
    Continue,
    /// The list ended inside a string literal interpolation context.
    FinishedInStringInterpolation,
    /// The list ended for another reason.
    Finished,
  };

  /// Parses a single item from a comma separated list and updates `Status`.
  ParseListItemResult
  parseListItem(ParserStatus &Status, tok RightK, SourceLoc LeftLoc,
                SourceLoc &RightLoc, bool AllowSepAfterLast,
                llvm::function_ref<ParserStatus()> callback);

  /// Parse a comma separated list of some elements.
  ParserStatus parseList(tok RightK, SourceLoc LeftLoc, SourceLoc &RightLoc,
                         bool AllowSepAfterLast, Diag<> ErrorDiag,
                         llvm::function_ref<ParserStatus()> callback);

  void consumeTopLevelDecl(ParserPosition BeginParserPosition,
                           TopLevelCodeDecl *TLCD);

  ParserStatus parseBraceItems(SmallVectorImpl<ASTNode> &Decls,
                               BraceItemListKind Kind,
                               BraceItemListKind ConditionalBlockKind,
                               bool &IsFollowingGuard);
  ParserStatus parseBraceItems(SmallVectorImpl<ASTNode> &Decls,
                               BraceItemListKind Kind =
                                   BraceItemListKind::Brace,
                               BraceItemListKind ConditionalBlockKind =
                                   BraceItemListKind::Brace) {
    bool IsFollowingGuard = false;
    return parseBraceItems(Decls, Kind, ConditionalBlockKind,
                           IsFollowingGuard);
  }
  ParserResult<BraceStmt> parseBraceItemList(Diag<> ID);
  
  //===--------------------------------------------------------------------===//
  // Decl Parsing

  /// Returns true if parser is at the start of a Swift decl or decl-import.
  bool isStartOfSwiftDecl(bool allowPoundIfAttributes = true,
                          bool hadAttrsOrModifiers = false);

  /// Returns true if the parser is at the start of a SIL decl.
  bool isStartOfSILDecl();

  /// Returns true if the parser is at a freestanding macro expansion.
  bool isStartOfFreestandingMacroExpansion();

  /// Parse the top-level Swift items into the provided vector.
  ///
  /// Each item will be a declaration, statement, or expression.
  void parseTopLevelItems(SmallVectorImpl<ASTNode> &items);

  /// Parse the source file via the Swift Parser using the ASTGen library.
  void parseSourceFileViaASTGen(SmallVectorImpl<ASTNode> &items,
                                Optional<DiagnosticTransaction> &transaction,
                                bool suppressDiagnostics = false);

  /// Parse the top-level SIL decls into the SIL module.
  /// \returns \c true if there was a parsing error.
  bool parseTopLevelSIL();

  bool isStartOfGetSetAccessor();

  /// Flags that control the parsing of declarations.
  enum ParseDeclFlags {
    PD_Default              = 0,
    PD_AllowTopLevel        = 1 << 1,
    PD_HasContainerType     = 1 << 2,
    PD_DisallowInit         = 1 << 3,
    PD_AllowEnumElement     = 1 << 4,
    PD_InProtocol           = 1 << 5,
    PD_InClass              = 1 << 6,
    PD_InExtension          = 1 << 7,
    PD_InStruct             = 1 << 8,
    PD_InEnum               = 1 << 9,
  };

  /// Options that control the parsing of declarations.
  using ParseDeclOptions = OptionSet<ParseDeclFlags>;

  void consumeDecl(ParserPosition BeginParserPosition, ParseDeclOptions Flags,
                   bool IsTopLevel);

  ParserResult<Decl> parseDecl(ParseDeclOptions Flags,
                               bool IsAtStartOfLineOrPreviousHadSemi,
                               bool IfConfigsAreDeclAttrs,
                               llvm::function_ref<void(Decl*)> Handler);

  std::pair<std::vector<Decl *>, Optional<Fingerprint>>
  parseDeclListDelayed(IterableDeclContext *IDC);

  bool parseMemberDeclList(SourceLoc &LBLoc, SourceLoc &RBLoc,
                           Diag<> LBraceDiag, Diag<> RBraceDiag,
                           IterableDeclContext *IDC);

  bool canDelayMemberDeclParsing(bool &HasOperatorDeclarations,
                                 bool &HasNestedClassDeclarations);

  bool canDelayFunctionBodyParsing(bool &HasNestedTypeDeclarations);

  bool delayParsingDeclList(SourceLoc LBLoc, SourceLoc &RBLoc,
                            IterableDeclContext *IDC);

  ParserResult<TypeDecl> parseDeclTypeAlias(ParseDeclOptions Flags,
                                            DeclAttributes &Attributes);

  ParserResult<TypeDecl> parseDeclAssociatedType(ParseDeclOptions Flags,
                                                 DeclAttributes &Attributes);

  /// Parse a #if ... #endif directive.
  /// Delegate callback function to parse elements in the blocks and record
  /// them however they wish. The parsing function will be provided with the
  /// location of the clause token (`#if`, `#else`, etc.), the condition,
  /// whether this is the active clause, and the role of the elements.
  template<typename Result>
  Result parseIfConfigRaw(
      llvm::function_ref<void(SourceLoc clauseLoc, Expr *condition,
                              bool isActive, IfConfigElementsRole role)>
          parseElements,
      llvm::function_ref<Result(SourceLoc endLoc, bool hadMissingEnd)> finish);

  /// Parse a #if ... #endif directive.
  /// Delegate callback function to parse elements in the blocks.
  ParserResult<IfConfigDecl> parseIfConfig(
    llvm::function_ref<void(SmallVectorImpl<ASTNode> &, bool)> parseElements);

  /// Parse an #if ... #endif containing only attributes.
  ParserStatus parseIfConfigDeclAttributes(
    DeclAttributes &attributes, bool ifConfigsAreDeclAttrs,
    PatternBindingInitializer *initContext);

  /// Parse a #error or #warning diagnostic.
  ParserResult<PoundDiagnosticDecl> parseDeclPoundDiagnostic();

  /// Parse a #line/#sourceLocation directive.
  /// 'isLine = true' indicates parsing #line instead of #sourcelocation
  ParserStatus parseLineDirective(bool isLine = false);

  void recordLocalType(TypeDecl *TD);

  /// Skip an `#if` configuration block containing only attributes.
  ///
  /// \returns true if the skipping was successful, false otherwise.
  bool skipIfConfigOfAttributes(bool &sawAnyAttributes);

  /// Determine whether the `#if` at which the parser occurs only contains
  /// attributes (in all branches), in which case it is treated as part of
  /// an attribute list.
  bool ifConfigContainsOnlyAttributes();

  /// Parse the optional attributes before a declaration.
  ParserStatus parseDeclAttributeList(DeclAttributes &Attributes,
                                      bool IfConfigsAreDeclAttrs = false);

  /// Parse the optional attributes before a declaration.
  ///
  /// This is the inner loop, which can be called recursively.
  ParserStatus parseDeclAttributeList(DeclAttributes &Attributes,
                                      bool IfConfigsAreDeclAttrs,
                                      PatternBindingInitializer *initContext);

  /// Parse the optional modifiers before a declaration.
  ParserStatus parseDeclModifierList(DeclAttributes &Attributes,
                                     SourceLoc &StaticLoc,
                                     StaticSpellingKind &StaticSpelling,
                                     bool isFromClangAttribute = false);

  /// Parse an availability attribute of the form
  /// @available(*, introduced: 1.0, deprecated: 3.1).
  /// \return \p nullptr if the platform name is invalid
  ParserResult<AvailableAttr>
  parseExtendedAvailabilitySpecList(SourceLoc AtLoc, SourceLoc AttrLoc,
                                    StringRef AttrName);

  /// Parse a string literal whose contents can be interpreted as a UUID.
  ///
  /// \returns false on success, true on error.
  bool parseUUIDString(UUID &uuid, Diag<> diag);

  /// Parse the Objective-C selector inside @objc
  void parseObjCSelector(SmallVector<Identifier, 4> &Names,
                         SmallVector<SourceLoc, 4> &NameLocs,
                         bool &IsNullarySelector);

  /// Parse the @_specialize attribute.
  /// \p closingBrace is the expected closing brace, which can be either ) or ]
  /// \p Attr is where to store the parsed attribute
  bool parseSpecializeAttribute(
      swift::tok ClosingBrace, SourceLoc AtLoc, SourceLoc Loc,
      SpecializeAttr *&Attr, AvailabilityContext *SILAvailability,
      llvm::function_ref<bool(Parser &)> parseSILTargetName =
          [](Parser &) { return false; },
      llvm::function_ref<bool(Parser &)> parseSILSIPModule =
          [](Parser &) { return false; });

  /// Parse the arguments inside the @_specialize attribute
  bool parseSpecializeAttributeArguments(
      swift::tok ClosingBrace, bool &DiscardAttribute, Optional<bool> &Exported,
      Optional<SpecializeAttr::SpecializationKind> &Kind,
      TrailingWhereClause *&TrailingWhereClause, DeclNameRef &targetFunction,
      AvailabilityContext *SILAvailability,
      SmallVectorImpl<Identifier> &spiGroups,
      SmallVectorImpl<AvailableAttr *> &availableAttrs,
      size_t &typeErasedParamsCount,
      llvm::function_ref<bool(Parser &)> parseSILTargetName,
      llvm::function_ref<bool(Parser &)> parseSILSIPModule);

  /// Parse the @_implements attribute.
  /// \p Attr is where to store the parsed attribute
  ParserResult<ImplementsAttr> parseImplementsAttribute(SourceLoc AtLoc,
                                                        SourceLoc Loc);

  /// Parse the @differentiable attribute.
  ParserResult<DifferentiableAttr> parseDifferentiableAttribute(SourceLoc AtLoc,
                                                                SourceLoc Loc);

  /// Parse the arguments inside the @differentiable attribute.
  bool parseDifferentiableAttributeArguments(
      DifferentiabilityKind &diffKind,
      SmallVectorImpl<ParsedAutoDiffParameter> &params,
      TrailingWhereClause *&whereClause);

  /// Parse a differentiability parameters clause, i.e. the 'wrt:' clause in
  /// `@differentiable`, `@derivative`, and `@transpose` attributes.
  ///
  /// If `allowNamedParameters` is false, allow only index parameters and
  /// 'self'. Used for `@transpose` attributes.
  bool parseDifferentiabilityParametersClause(
      SmallVectorImpl<ParsedAutoDiffParameter> &parameters, StringRef attrName,
      bool allowNamedParameters = true);

  /// Parse the @derivative attribute.
  ParserResult<DerivativeAttr> parseDerivativeAttribute(SourceLoc AtLoc,
                                                        SourceLoc Loc);

  /// Parse the @transpose attribute.
  ParserResult<TransposeAttr> parseTransposeAttribute(SourceLoc AtLoc,
                                                      SourceLoc Loc);

  /// Parse the @backDeployed attribute.
  bool parseBackDeployedAttribute(DeclAttributes &Attributes,
                                  StringRef AttrName, SourceLoc AtLoc,
                                  SourceLoc Loc);

  /// Parse the @_documentation attribute.
  ParserResult<DocumentationAttr> parseDocumentationAttribute(SourceLoc AtLoc,
                                                              SourceLoc Loc);

  /// Parse a single argument from a @_documentation attribute.
  bool parseDocumentationAttributeArgument(Optional<StringRef> &Metadata,
                                           Optional<AccessLevel> &Visibility);

  /// Parse the @attached or @freestanding attribute that specifies a macro
  /// role.
  ParserResult<MacroRoleAttr> parseMacroRoleAttribute(
      MacroSyntax syntax, SourceLoc AtLoc, SourceLoc Loc
  );

  /// Parse a specific attribute.
  ParserStatus parseDeclAttribute(DeclAttributes &Attributes, SourceLoc AtLoc,
                                  PatternBindingInitializer *&initContext,
                                  bool isFromClangAttribute = false);

  bool isCustomAttributeArgument();
  bool canParseCustomAttribute();

  /// Parse a custom attribute after the initial '@'.
  ///
  /// \param atLoc The location of the already-parsed '@'.
  ///
  /// \param initContext A reference to the initializer context used
  /// for the set of custom attributes. This should start as nullptr, and
  /// will get filled in by this function. The same variable should be provided
  /// for every custom attribute within the same attribute list.
  ParserResult<CustomAttr> parseCustomAttribute(
      SourceLoc atLoc, PatternBindingInitializer *&initContext);

  ParserStatus parseNewDeclAttribute(DeclAttributes &Attributes,
                                     SourceLoc AtLoc, DeclAttrKind DK,
                                     bool isFromClangAttribute = false);

  /// Parse a version tuple of the form x[.y[.z]]. Returns true if there was
  /// an error parsing.
  bool parseVersionTuple(llvm::VersionTuple &Version, SourceRange &Range,
                         const Diagnostic &D);

  bool canHaveParameterSpecifierContextualKeyword() {
    // The parameter specifiers like `isolated`, `consuming`, `borrowing` are
    // also valid identifiers and could be the name of a type. Check whether
    // the following token is something that can introduce a type. Thankfully
    // none of these tokens overlap with the set of tokens that can follow an
    // identifier in a type production.
    return Tok.is(tok::identifier)
      && peekToken().isAny(tok::at_sign,
                           tok::kw_inout,
                           tok::l_paren,
                           tok::identifier,
                           tok::l_square,
                           tok::kw_Any,
                           tok::kw_Self,
                           tok::kw__,
                           tok::kw_var,
                           tok::kw_let);
  }

  ParserStatus parseTypeAttributeList(ParamDecl::Specifier &Specifier,
                                      SourceLoc &SpecifierLoc,
                                      SourceLoc &IsolatedLoc,
                                      SourceLoc &ConstLoc,
                                      TypeAttributes &Attributes) {
    if (Tok.isAny(tok::at_sign, tok::kw_inout) ||
        (canHaveParameterSpecifierContextualKeyword() &&
         (Tok.getRawText().equals("__shared") ||
          Tok.getRawText().equals("__owned") ||
          Tok.getRawText().equals("consuming") ||
          Tok.getRawText().equals("borrowing") ||
          Tok.isContextualKeyword("isolated") ||
          Tok.isContextualKeyword("_const"))))
      return parseTypeAttributeListPresent(
          Specifier, SpecifierLoc, IsolatedLoc, ConstLoc, Attributes);
    return makeParserSuccess();
  }

  ParserStatus parseTypeAttributeListPresent(ParamDecl::Specifier &Specifier,
                                             SourceLoc &SpecifierLoc,
                                             SourceLoc &IsolatedLoc,
                                             SourceLoc &ConstLoc,
                                             TypeAttributes &Attributes);

  bool parseConventionAttributeInternal(bool justChecking,
                                        TypeAttributes::Convention &convention);

  ParserStatus parseTypeAttribute(TypeAttributes &Attributes, SourceLoc AtLoc,
                                  PatternBindingInitializer *&initContext,
                                  bool justChecking = false);

  ParserResult<ImportDecl> parseDeclImport(ParseDeclOptions Flags,
                                           DeclAttributes &Attributes);

  /// Parse an inheritance clause into a vector of InheritedEntry's.
  ///
  /// \param allowClassRequirement whether to permit parsing of 'class'
  /// \param allowAnyObject whether to permit parsing of 'AnyObject'
  /// \param parseTildeCopyable if non-null, permits parsing of `~Copyable`
  ///   and writes out a valid source location if it was parsed. If null, then a
  ///   parsing error will be emitted upon the appearance of `~` in the clause.
  ParserStatus parseInheritance(SmallVectorImpl<InheritedEntry> &Inherited,
                                bool allowClassRequirement,
                                bool allowAnyObject,
                                SourceLoc *parseTildeCopyable = nullptr);
  ParserStatus parseDeclItem(bool &PreviousHadSemi,
                             ParseDeclOptions Options,
                             llvm::function_ref<void(Decl*)> handler);
  std::pair<std::vector<Decl *>, Optional<Fingerprint>>
  parseDeclList(SourceLoc LBLoc, SourceLoc &RBLoc, Diag<> ErrorDiag,
                ParseDeclOptions Options, IterableDeclContext *IDC,
                bool &hadError);
  ParserResult<ExtensionDecl> parseDeclExtension(ParseDeclOptions Flags,
                                                 DeclAttributes &Attributes);
  ParserResult<EnumDecl> parseDeclEnum(ParseDeclOptions Flags,
                                       DeclAttributes &Attributes);
  ParserResult<EnumCaseDecl>
  parseDeclEnumCase(ParseDeclOptions Flags, DeclAttributes &Attributes,
                    SmallVectorImpl<Decl *> &decls);
  ParserResult<StructDecl>
  parseDeclStruct(ParseDeclOptions Flags, DeclAttributes &Attributes);
  ParserResult<ClassDecl>
  parseDeclClass(ParseDeclOptions Flags, DeclAttributes &Attributes);
  ParserResult<PatternBindingDecl>
  parseDeclVar(ParseDeclOptions Flags, DeclAttributes &Attributes,
               SmallVectorImpl<Decl *> &Decls,
               SourceLoc StaticLoc,
               StaticSpellingKind StaticSpelling,
               SourceLoc TryLoc,
               bool HasLetOrVarKeyword = true);

  struct ParsedAccessors;

  bool parseAccessorAfterIntroducer(
      SourceLoc Loc, AccessorKind Kind, ParsedAccessors &accessors,
      bool &hasEffectfulGet, ParameterList *Indices, bool &parsingLimitedSyntax,
      DeclAttributes &Attributes, ParseDeclOptions Flags,
      AbstractStorageDecl *storage, SourceLoc StaticLoc, ParserStatus &Status
  );

  ParserStatus parseGetSet(ParseDeclOptions Flags, ParameterList *Indices,
                           TypeRepr *ResultType, ParsedAccessors &accessors,
                           AbstractStorageDecl *storage, SourceLoc StaticLoc);
  ParserResult<VarDecl> parseDeclVarGetSet(PatternBindingEntry &entry,
                                           ParseDeclOptions Flags,
                                           SourceLoc StaticLoc,
                                           StaticSpellingKind StaticSpelling,
                                           SourceLoc VarLoc,
                                           bool hasInitializer,
                                           const DeclAttributes &Attributes,
                                           SmallVectorImpl<Decl *> &Decls);
  ParserStatus parseGetEffectSpecifier(ParsedAccessors &accessors,
                                       SourceLoc &asyncLoc,
                                       SourceLoc &throwsLoc,
                                       bool &hasEffectfulGet,
                                       AccessorKind currentKind,
                                       SourceLoc const& currentLoc);

  /// Parse accessors provided as a separate list, for use in macro
  /// expansions.
  void parseTopLevelAccessors(
      AbstractStorageDecl *storage, SmallVectorImpl<ASTNode> &items
  );

  /// Parse the result of attribute macro expansion, which is a floating
  /// attribute list.
  ///
  /// Parsing a floating attribute list will produce a `MissingDecl` with
  /// the attribute list attached.
  void parseExpandedAttributeList(SmallVectorImpl<ASTNode> &items);

  /// Parse the result of member macro expansion, which is a floating
  /// member list.
  void parseExpandedMemberList(SmallVectorImpl<ASTNode> &items);

  ParserResult<FuncDecl> parseDeclFunc(SourceLoc StaticLoc,
                                       StaticSpellingKind StaticSpelling,
                                       ParseDeclOptions Flags,
                                       DeclAttributes &Attributes,
                                       bool HasFuncKeyword = true);
  BodyAndFingerprint
  parseAbstractFunctionBodyImpl(AbstractFunctionDecl *AFD);
  void parseAbstractFunctionBody(AbstractFunctionDecl *AFD);
  BodyAndFingerprint
  parseAbstractFunctionBodyDelayed(AbstractFunctionDecl *AFD);

  ParserResult<MacroDecl> parseDeclMacro(DeclAttributes &Attributes);

  ParserStatus parsePrimaryAssociatedTypes(
      SmallVectorImpl<PrimaryAssociatedTypeName> &AssocTypeNames);
  ParserStatus parsePrimaryAssociatedTypeList(
      SmallVectorImpl<PrimaryAssociatedTypeName> &AssocTypeNames);
  ParserResult<ProtocolDecl> parseDeclProtocol(ParseDeclOptions Flags,
                                               DeclAttributes &Attributes);

  ParserResult<SubscriptDecl>
  parseDeclSubscript(SourceLoc StaticLoc, StaticSpellingKind StaticSpelling,
                     ParseDeclOptions Flags, DeclAttributes &Attributes,
                     SmallVectorImpl<Decl *> &Decls);

  ParserResult<ConstructorDecl>
  parseDeclInit(ParseDeclOptions Flags, DeclAttributes &Attributes);
  ParserResult<DestructorDecl>
  parseDeclDeinit(ParseDeclOptions Flags, DeclAttributes &Attributes);

  ParserResult<OperatorDecl> parseDeclOperator(ParseDeclOptions Flags,
                                               DeclAttributes &Attributes);
  ParserResult<OperatorDecl> parseDeclOperatorImpl(SourceLoc OperatorLoc,
                                                   Identifier Name,
                                                   SourceLoc NameLoc,
                                                   DeclAttributes &Attrs);

  ParserResult<PrecedenceGroupDecl>
  parseDeclPrecedenceGroup(ParseDeclOptions flags, DeclAttributes &attributes);

  ParserResult<MacroExpansionDecl>
  parseDeclMacroExpansion(ParseDeclOptions flags, DeclAttributes &attributes);

  ParserResult<TypeRepr> parseDeclResultType(Diag<> MessageID);

  /// Get the location for a type error.
  SourceLoc getTypeErrorLoc() const;

  //===--------------------------------------------------------------------===//
  // Type Parsing

  enum class ParseTypeReason {
    /// Any type parsing context.
    Unspecified,

    /// Whether the type is for a closure attribute.
    CustomAttribute,
  };

  ParserResult<TypeRepr> parseTypeScalar(
      Diag<> MessageID,
      ParseTypeReason reason);

  ParserResult<TypeRepr> parseType();
  ParserResult<TypeRepr> parseType(
      Diag<> MessageID,
      ParseTypeReason reason = ParseTypeReason::Unspecified);

  /// Parse a type optionally prefixed by a list of named opaque parameters. If
  /// no params present, return 'type'. Otherwise, return 'type-named-opaque'.
  ///
  ///   type-named-opaque:
  ///     generic-params type
  ParserResult<TypeRepr> parseTypeWithOpaqueParams(Diag<> MessageID);

  ParserResult<TypeRepr>
    parseTypeSimpleOrComposition(Diag<> MessageID, ParseTypeReason reason);

  ParserResult<TypeRepr> parseTypeSimple(
      Diag<> MessageID, ParseTypeReason reason);

  /// Parse layout constraint.
  LayoutConstraint parseLayoutConstraint(Identifier LayoutConstraintID);

  ParserStatus parseGenericArguments(SmallVectorImpl<TypeRepr *> &Args,
                                     SourceLoc &LAngleLoc,
                                     SourceLoc &RAngleLoc);

  /// Parses and returns the base type for a qualified declaration name,
  /// positioning the parser at the '.' before the final declaration name. This
  /// position is important for parsing final declaration names like '.init' via
  /// `parseUnqualifiedDeclName`. For example, 'Foo.Bar.f' parses as 'Foo.Bar'
  /// and the parser is positioned at '.f'. If there is no base type qualifier
  /// (e.g. when parsing just 'f'), returns an empty parser error.
  ///
  /// \verbatim
  ///   qualified-decl-name-base-type:
  ///     identifier generic-args? ('.' identifier generic-args?)*
  /// \endverbatim
  ParserResult<TypeRepr> parseQualifiedDeclNameBaseType();

  /// Parse an identifier type, e.g 'Foo' or 'Bar<Int>'.
  ///
  /// \verbatim
  /// type-identifier: identifier generic-args?
  /// \endverbatim
  ParserResult<IdentTypeRepr> parseTypeIdentifier();

  /// Parse a dotted type, e.g. 'Foo<X>.Y.Z', 'P.Type', '[X].Y'.
  ParserResult<TypeRepr> parseTypeDotted(ParserResult<TypeRepr> Base);

  ParserResult<TypeRepr> parseOldStyleProtocolComposition();
  ParserResult<TypeRepr> parseAnyType();
  ParserResult<TypeRepr> parseSILBoxType(GenericParamList *generics,
                                         const TypeAttributes &attrs);
  
  ParserResult<TypeRepr> parseTypeTupleBody();
  ParserResult<TypeRepr> parseTypeArray(ParserResult<TypeRepr> Base);

  /// Parse a collection type.
  ///   type-simple:
  ///     '[' type ']'
  ///     '[' type ':' type ']'
  ParserResult<TypeRepr> parseTypeCollection();

  ParserResult<TypeRepr> parseTypeOptional(ParserResult<TypeRepr> Base);

  ParserResult<TypeRepr>
  parseTypeImplicitlyUnwrappedOptional(ParserResult<TypeRepr> Base);

  bool isOptionalToken(const Token &T) const;
  SourceLoc consumeOptionalToken();
  
  bool isImplicitlyUnwrappedOptionalToken(const Token &T) const;
  SourceLoc consumeImplicitlyUnwrappedOptionalToken();

  TypeRepr *applyAttributeToType(TypeRepr *Ty, const TypeAttributes &Attr,
                                 ParamDecl::Specifier Specifier,
                                 SourceLoc SpecifierLoc,
                                 SourceLoc IsolatedLoc,
                                 SourceLoc ConstLoc);

  //===--------------------------------------------------------------------===//
  // Pattern Parsing

  /// A structure for collecting information about the default
  /// arguments of a context.
  struct DefaultArgumentInfo {
    llvm::SmallVector<DefaultArgumentInitializer *, 4> ParsedContexts;
    unsigned NextIndex : 31;
    
    /// Track whether or not one of the parameters in a signature's argument
    /// list accepts a default argument.
    unsigned HasDefaultArgument : 1;

    /// Claim the next argument index.  It's important to do this for
    /// all the arguments, not just those that have default arguments.
    unsigned claimNextIndex() { return NextIndex++; }

    /// Set the parsed context of all default argument initializers to
    /// the given function, enum case or subscript.
    void setFunctionContext(DeclContext *DC, ParameterList *paramList);
    
    DefaultArgumentInfo() {
      NextIndex = 0;
      HasDefaultArgument = false;
    }
  };

  /// Describes a parsed parameter.
  struct ParsedParameter {
    /// Any declaration attributes attached to the parameter.
    DeclAttributes Attrs;

    /// The location of the 'inout' keyword, if present.
    SourceLoc SpecifierLoc;
    
    /// The parsed specifier kind, if present.
    ParamDecl::Specifier SpecifierKind = ParamDecl::Specifier::Default;

    /// The location of the first name.
    ///
    /// \c FirstName is the name.
    SourceLoc FirstNameLoc;

    /// The location of the second name, if present.
    ///
    /// \p SecondName is the name.
    SourceLoc SecondNameLoc;

    /// The first name.
    Identifier FirstName;

    /// The second name, the presence of which is indicated by \c SecondNameLoc.
    Identifier SecondName;

    /// The location of the 'isolated' keyword, if present.
    SourceLoc IsolatedLoc;

    /// The location of the '_const' keyword, if present.
    SourceLoc CompileConstLoc;

    /// The type following the ':'.
    TypeRepr *Type = nullptr;

    /// The default argument for this parameter.
    Expr *DefaultArg = nullptr;

    /// True if this parameter inherits a default argument via '= super'
    bool hasInheritedDefaultArg = false;
    
    /// True if we emitted a parse error about this parameter.
    bool isInvalid = false;

    /// True if this parameter is potentially destructuring a tuple argument.
    bool isPotentiallyDestructured = false;
  };

  /// Describes the context in which the given parameter is being parsed.
  enum class ParameterContextKind {
    /// An operator.
    Operator,
    /// A function.
    Function,
    /// An initializer.
    Initializer,
    /// A closure.
    Closure,
    /// A subscript.
    Subscript,
    /// A curried argument clause.
    Curried,
    /// An enum element.
    EnumElement,
    /// A macro.
    Macro,
  };

  /// Whether we are at the start of a parameter name when parsing a parameter.
  bool startsParameterName(bool isClosure);

  /// Parse a parameter-clause.
  ///
  /// \verbatim
  ///   parameter-clause:
  ///     '(' ')'
  ///     '(' parameter (',' parameter)* '...'? )'
  ///
  ///   parameter:
  ///     'inout'? ('let' | 'var')? '`'? identifier-or-none identifier-or-none?
  ///         (':' type)? ('...' | '=' expr)?
  ///
  ///   identifier-or-none:
  ///     identifier
  ///     '_'
  /// \endverbatim
  ParserStatus parseParameterClause(SourceLoc &leftParenLoc,
                                    SmallVectorImpl<ParsedParameter> &params,
                                    SourceLoc &rightParenLoc,
                                    DefaultArgumentInfo *defaultArgs,
                                    ParameterContextKind paramContext);

  ParserResult<ParameterList> parseSingleParameterClause(
                          ParameterContextKind paramContext,
                          SmallVectorImpl<Identifier> *namePieces = nullptr,
                          DefaultArgumentInfo *defaultArgs = nullptr);

  ParserStatus parseFunctionArguments(SmallVectorImpl<Identifier> &NamePieces,
                                      ParameterList *&BodyParams,
                                      ParameterContextKind paramContext,
                                      DefaultArgumentInfo &defaultArgs);
  ParserStatus parseFunctionSignature(DeclBaseName functionName,
                                      DeclName &fullName,
                                      ParameterList *&bodyParams,
                                      DefaultArgumentInfo &defaultArgs,
                                      SourceLoc &asyncLoc,
                                      bool &reasync,
                                      SourceLoc &throws,
                                      bool &rethrows,
                                      TypeRepr *&retType);

  /// Parse 'async' and 'throws', if present, putting the locations of the
  /// keywords into the \c SourceLoc parameters.
  ///
  /// \param existingArrowLoc The location of an existing '->', if there is
  /// one. Parsing 'async' or 'throws' after the `->` is an error we
  /// correct for.
  ///
  /// \param reasync If non-NULL, will also parse the 'reasync' keyword in
  /// lieu of 'async'.
  ///
  /// \param rethrows If non-NULL, will also parse the 'rethrows' keyword in
  /// lieu of 'throws'.
  ParserStatus parseEffectsSpecifiers(SourceLoc existingArrowLoc,
                                      SourceLoc &asyncLoc, bool *reasync,
                                      SourceLoc &throwsLoc, bool *rethrows);

  /// Returns 'true' if \p T is considered effects specifier.
  bool isEffectsSpecifier(const Token &T);

  //===--------------------------------------------------------------------===//
  // Pattern Parsing

  ParserResult<Pattern> parseTypedPattern();
  ParserResult<Pattern> parsePattern();
  
  /// Parse a tuple pattern element.
  ///
  /// \code
  ///   pattern-tuple-element:
  ///     pattern ('=' expr)?
  /// \endcode
  ///
  /// \returns The tuple pattern element, if successful.
  std::pair<ParserStatus, Optional<TuplePatternElt>>
  parsePatternTupleElement();
  ParserResult<Pattern> parsePatternTuple();
  
  ParserResult<Pattern>
  parseOptionalPatternTypeAnnotation(ParserResult<Pattern> P);
  ParserResult<Pattern> parseMatchingPattern(bool isExprBasic);
  ParserResult<Pattern>
  parseMatchingPatternAsBinding(PatternBindingState newState, SourceLoc VarLoc,
                                bool isExprBasic);

  Pattern *createBindingFromPattern(SourceLoc loc, Identifier name,
                                    VarDecl::Introducer introducer);
  

  /// Determine whether this token can only start a matching pattern
  /// production and not an expression.
  bool isOnlyStartOfMatchingPattern();

  //===--------------------------------------------------------------------===//
  // Speculative type list parsing
  //===--------------------------------------------------------------------===//
  
  /// Returns true if we can parse a generic argument list at the current
  /// location in expression context. This parses types without generating
  /// AST nodes from the '<' at the current location up to a matching '>'. If
  /// the type list parse succeeds, and the closing '>' is followed by one
  /// of the following tokens:
  ///   lparen_following rparen lsquare_following rsquare lbrace rbrace
  ///   period_following comma semicolon
  /// then this function returns true, and the expression will parse as a
  /// generic parameter list. If the parse fails, or the closing '>' is not
  /// followed by one of the above tokens, then this function returns false,
  /// and the expression will parse with the '<' as an operator.
  bool canParseAsGenericArgumentList();

  bool canParseType();

  /// Returns true if a simple type identifier can be parsed.
  ///
  /// \verbatim
  ///   simple-type-identifier: identifier generic-argument-list?
  /// \endverbatim
  bool canParseTypeIdentifier();
  bool canParseOldStyleProtocolComposition();
  bool canParseTypeTupleBody();
  bool canParseTypeAttribute();
  bool canParseGenericArguments();

  bool canParseTypedPattern();

  /// Returns true if a qualified declaration name base type can be parsed.
  bool canParseBaseTypeForQualifiedDeclName();

  /// Returns true if the current token is '->' or effects specifiers followed
  /// by '->'.
  ///
  /// e.g.
  ///  throws ->       // true
  ///  async throws -> // true
  ///  throws {        // false
  bool isAtFunctionTypeArrow();

  //===--------------------------------------------------------------------===//
  // Expression Parsing
  ParserResult<Expr> parseExpr(Diag<> ID) {
    return parseExprImpl(ID, /*isExprBasic=*/false);
  }
  ParserResult<Expr> parseExprBasic(Diag<> ID) {
    return parseExprImpl(ID, /*isExprBasic=*/true);
  }
  ParserResult<Expr> parseExprImpl(Diag<> ID, bool isExprBasic);
  ParserResult<Expr> parseExprIs();
  ParserResult<Expr> parseExprAs();
  ParserResult<Expr> parseExprArrow();
  ParserResult<Expr> parseExprSequence(Diag<> ID,
                                       bool isExprBasic,
                                       bool isForConditionalDirective = false);
  ParserResult<Expr> parseExprSequenceElement(Diag<> ID,
                                              bool isExprBasic);
  ParserResult<Expr> parseExprPostfixSuffix(ParserResult<Expr> inner,
                                            bool isExprBasic,
                                            bool periodHasKeyPathBehavior,
                                            bool &hasBindOptional);
  ParserResult<Expr> parseExprPostfix(Diag<> ID, bool isExprBasic);
  ParserResult<Expr> parseExprPrimary(Diag<> ID, bool isExprBasic);
  ParserResult<Expr> parseExprUnary(Diag<> ID, bool isExprBasic);
  ParserResult<Expr> parseExprKeyPathObjC();
  ParserResult<Expr> parseExprKeyPath();
  ParserResult<Expr> parseExprSelector();
  ParserResult<Expr> parseExprSuper();
  ParserResult<Expr> parseExprStringLiteral();
  ParserResult<Expr> parseExprRegexLiteral();

  StringRef copyAndStripUnderscores(StringRef text);
  StringRef stripUnderscoresIfNeeded(StringRef text,
                                     SmallVectorImpl<char> &buffer);

  ParserStatus parseStringSegments(SmallVectorImpl<Lexer::StringSegment> &Segments,
                                   Token EntireTok,
                                   VarDecl *InterpolationVar,
                                   SmallVectorImpl<ASTNode> &Stmts,
                                   unsigned &LiteralCapacity,
                                   unsigned &InterpolationCount);

  /// Parse an argument label `identifier ':'`, if it exists.
  ///
  /// \param name The parsed name of the label (empty if it doesn't exist, or is
  /// _)
  /// \param loc The location of the label (empty if it doesn't exist)
  void parseOptionalArgumentLabel(Identifier &name, SourceLoc &loc);

  enum class DeclNameFlag : uint8_t {
    /// If passed, operator basenames are allowed.
    AllowOperators = 1 << 0,

    /// If passed, names that coincide with keywords are allowed. Used after a
    /// dot to enable things like '.init' and '.default'.
    AllowKeywords = 1 << 1,

    /// If passed, 'deinit' and 'subscript' should be parsed as special names,
    /// not ordinary identifiers.
    AllowKeywordsUsingSpecialNames = AllowKeywords | 1 << 2,

    /// If passed, compound names with argument lists are allowed, unless they
    /// have empty argument lists.
    AllowCompoundNames = 1 << 4,

    /// If passed, compound names with empty argument lists are allowed.
    AllowZeroArgCompoundNames = AllowCompoundNames | 1 << 5,

    /// If passed, \c self and \c Self are allowed as basenames. In a lot of
    /// cases this doesn't actually make sense but we need to accept them for
    /// backwards compatibility.
    AllowLowercaseAndUppercaseSelf = 1 << 6,
  };
  using DeclNameOptions = OptionSet<DeclNameFlag>;

  friend DeclNameOptions operator|(DeclNameFlag flag1, DeclNameFlag flag2) {
    return DeclNameOptions(flag1) | flag2;
  }

  /// Without \c DeclNameFlag::AllowCompoundNames, parse an
  /// unqualified-decl-base-name.
  ///
  ///   unqualified-decl-base-name: identifier
  ///
  /// With \c DeclNameFlag::AllowCompoundNames, parse an unqualified-base-name.
  ///
  ///   unqualified-decl-name:
  ///     unqualified-decl-base-name
  ///     unqualified-decl-base-name '(' ((identifier | '_') ':') + ')'
  DeclNameRef parseDeclNameRef(DeclNameLoc &loc, const Diagnostic &diag,
                               DeclNameOptions flags);

  /// Parse macro expansion.
  ///
  ///   macro-expansion:
  ///     '#' identifier generic-arguments? expr-paren? trailing-closure?
  ParserStatus parseFreestandingMacroExpansion(
      SourceLoc &poundLoc, DeclNameLoc &macroNameLoc, DeclNameRef &macroNameRef,
      SourceLoc &leftAngleLoc, SmallVectorImpl<TypeRepr *> &genericArgs,
      SourceLoc &rightAngleLoc, ArgumentList *&argList, bool isExprBasic,
      const Diagnostic &diag);

  ParserResult<Expr> parseExprIdentifier();
  Expr *parseExprEditorPlaceholder(Token PlaceholderTok,
                                   Identifier PlaceholderId);

  /// Parse a closure expression after the opening brace.
  ///
  /// \verbatim
  ///   expr-closure:
  ///     '{' closure-signature? brace-item-list* '}'
  ///
  ///   closure-signature:
  ///     '|' closure-signature-arguments? '|' closure-signature-result?
  ///
  ///   closure-signature-arguments:
  ///     pattern-tuple-element (',' pattern-tuple-element)*
  ///
  ///   closure-signature-result:
  ///     '->' type
  /// \endverbatim
  ParserResult<Expr> parseExprClosure();

  /// Parse the closure signature, if present.
  ///
  /// \verbatim
  ///   closure-signature:
  ///     parameter-clause func-signature-result? 'in'
  ///     identifier (',' identifier)* func-signature-result? 'in'
  /// \endverbatim
  ///
  /// \param bracketRange The range of the brackets enclosing a capture list, if
  /// present. Needed to offer fix-its for inserting 'self' into a capture list.
  /// \param captureList The entries in the capture list.
  /// \param params The parsed parameter list, or null if none was provided.
  /// \param arrowLoc The location of the arrow, if present.
  /// \param explicitResultType The explicit result type, if specified.
  /// \param inLoc The location of the 'in' keyword, if present.
  ///
  /// \returns ParserStatus error if an error occurred. Success if no signature
  /// is present or successfully parsed.
  ParserStatus parseClosureSignatureIfPresent(
          DeclAttributes &attributes,
          SourceRange &bracketRange,
          SmallVectorImpl<CaptureListEntry> &captureList,
          VarDecl *&capturedSelfParamDecl,
          ParameterList *&params,
          SourceLoc &asyncLoc,
          SourceLoc &throwsLoc,
          SourceLoc &arrowLoc,
          TypeExpr *&explicitResultType,
          SourceLoc &inLoc);

  Expr *parseExprAnonClosureArg();

  /// An element of an expression list, which may become e.g a tuple element or
  /// argument list argument.
  struct ExprListElt {
    SourceLoc LabelLoc;
    Identifier Label;
    Expr *E;
  };

  /// Parse a tuple or paren expr.
  ParserResult<Expr> parseTupleOrParenExpr(tok leftTok, tok rightTok);

  /// Parse an argument list.
  ParserResult<ArgumentList>
  parseArgumentList(tok leftTok, tok rightTok, bool isExprBasic,
                    bool allowTrailingClosure = true);

  /// Parse one or more trailing closures after an argument list.
  ParserStatus parseTrailingClosures(bool isExprBasic, SourceRange calleeRange,
                                     SmallVectorImpl<Argument> &closures);

  /// Parse an expression list.
  ParserStatus parseExprList(tok leftTok, tok rightTok, bool isArgumentList,
                             SourceLoc &leftLoc,
                             SmallVectorImpl<ExprListElt> &elts,
                             SourceLoc &rightLoc);

  /// Parse an object literal.
  ///
  /// \param LK The literal kind as determined by the first token.
  ParserResult<Expr> parseExprObjectLiteral(ObjectLiteralExpr::LiteralKind LK,
                                            bool isExprBasic);
  ParserResult<Expr> parseExprCallSuffix(ParserResult<Expr> fn,
                                         bool isExprBasic);
  ParserResult<Expr> parseExprMacroExpansion(bool isExprBasic);
  ParserResult<Expr> parseExprCollection();
  ParserResult<Expr> parseExprCollectionElement(Optional<bool> &isDictionary);
  ParserResult<Expr>
  parseExprPoundCodeCompletion(Optional<StmtKind> ParentKind);

  UnresolvedDeclRefExpr *parseExprOperator();

  /// Try re-lex a '/' operator character as a regex literal. This should be
  /// called when parsing in an expression position to ensure a regex literal is
  /// correctly parsed.
  void tryLexRegexLiteral(bool forUnappliedOperator);

  void validateCollectionElement(ParserResult<Expr> element);

  //===--------------------------------------------------------------------===//
  // Statement Parsing

  bool isStartOfStmt();
  bool isTerminatorForBraceItemListKind(BraceItemListKind Kind,
                                        ArrayRef<ASTNode> ParsedDecls);
  ParserResult<Stmt> parseStmt();
  ParserStatus parseExprOrStmt(ASTNode &Result);
  ParserResult<Stmt> parseStmtBreak();
  ParserResult<Stmt> parseStmtContinue();
  ParserResult<Stmt> parseStmtReturn(SourceLoc tryLoc);
  ParserResult<Stmt> parseStmtYield(SourceLoc tryLoc);
  ParserResult<Stmt> parseStmtThrow(SourceLoc tryLoc);
  ParserResult<Stmt> parseStmtDiscard();
  ParserResult<Stmt> parseStmtDefer();
  ParserStatus
  parseStmtConditionElement(SmallVectorImpl<StmtConditionElement> &result,
                            Diag<> DefaultID, StmtKind ParentKind,
                            StringRef &BindingKindStr);
  ParserStatus parseStmtCondition(StmtCondition &Result, Diag<> ID,
                                  StmtKind ParentKind);
  ParserResult<PoundAvailableInfo> parseStmtConditionPoundAvailable();
  ParserResult<PoundHasSymbolInfo> parseStmtConditionPoundHasSymbol();
  ParserResult<Stmt> parseStmtIf(LabeledStmtInfo LabelInfo,
                                 bool IfWasImplicitlyInserted = false);
  ParserResult<Stmt> parseStmtGuard();
  ParserResult<Stmt> parseStmtWhile(LabeledStmtInfo LabelInfo);
  ParserResult<Stmt> parseStmtRepeat(LabeledStmtInfo LabelInfo);
  ParserResult<Stmt> parseStmtDo(LabeledStmtInfo LabelInfo,
                                 bool shouldSkipDoTokenConsume = false);
  ParserResult<CaseStmt> parseStmtCatch();
  ParserResult<Stmt> parseStmtForEach(LabeledStmtInfo LabelInfo);
  ParserResult<Stmt> parseStmtSwitch(LabeledStmtInfo LabelInfo);
  ParserStatus parseStmtCases(SmallVectorImpl<ASTNode> &cases, bool IsActive);
  ParserResult<CaseStmt> parseStmtCase(bool IsActive);
  ParserResult<Stmt> parseStmtPoundAssert();

  //===--------------------------------------------------------------------===//
  // Generics Parsing

  ParserResult<GenericParamList> parseGenericParameters();
  ParserResult<GenericParamList> parseGenericParameters(SourceLoc LAngleLoc);
  ParserStatus parseGenericParametersBeforeWhere(SourceLoc LAngleLoc,
                        SmallVectorImpl<GenericTypeParamDecl *> &GenericParams);
  ParserResult<GenericParamList> maybeParseGenericParams();
  void
  diagnoseWhereClauseInGenericParamList(const GenericParamList *GenericParams);

  ParserStatus
  parseFreestandingGenericWhereClause(GenericContext *genCtx);

  ParserStatus parseGenericWhereClause(
      SourceLoc &WhereLoc, SourceLoc &EndLoc,
      SmallVectorImpl<RequirementRepr> &Requirements,
      bool AllowLayoutConstraints = false);

  ParserStatus
  parseProtocolOrAssociatedTypeWhereClause(TrailingWhereClause *&trailingWhere,
                                           bool isProtocol);

  //===--------------------------------------------------------------------===//
  // Availability Specification Parsing

  /// The source of an availability spec list.
  enum class AvailabilitySpecSource: uint8_t {
    /// A spec from '@available(<spec>, ...)' or '#available(<spec>, ...)'.
    Available,
    /// A spec from '#unavailable(<spec>, ...)'.
    Unavailable,
    /// A spec from a '-define-availability "Name:<spec>, ..."' frontend arg.
    Macro,
  };

  /// Parse a comma-separated list of availability specifications. Try to
  /// expand availability macros when /p Source is not a command line macro.
  ParserStatus
  parseAvailabilitySpecList(SmallVectorImpl<AvailabilitySpec *> &Specs,
                            AvailabilitySpecSource Source);

  /// Does the current matches an argument macro name? Parsing compiler
  /// arguments as required without consuming tokens from the source file
  /// parser.
  bool peekAvailabilityMacroName();

  /// Try to parse a reference to an availability macro and append its result
  /// to \p Specs. If the current token doesn't match a macro name, return
  /// a success without appending anything to \c Specs.
  ParserStatus
  parseAvailabilityMacro(SmallVectorImpl<AvailabilitySpec *> &Specs);

  /// Parse the availability macros definitions passed as arguments.
  void parseAllAvailabilityMacroArguments();

  /// Result of parsing an availability macro definition.
  struct AvailabilityMacroDefinition {
    StringRef Name;
    llvm::VersionTuple Version;
    SmallVector<AvailabilitySpec *, 4> Specs;
  };

  /// Parse an availability macro definition from a command line argument.
  /// This function should be called on a Parser set up on the command line
  /// argument code.
  ParserStatus
  parseAvailabilityMacroDefinition(AvailabilityMacroDefinition &Result);

  ParserResult<AvailabilitySpec> parseAvailabilitySpec();
  ParserResult<PlatformVersionConstraintAvailabilitySpec>
  parsePlatformVersionConstraintSpec();
  ParserResult<PlatformAgnosticVersionConstraintAvailabilitySpec>
  parsePlatformAgnosticVersionConstraintSpec();
  bool
  parseAvailability(bool parseAsPartOfSpecializeAttr, StringRef AttrName,
                    bool &DiscardAttribute, SourceRange &attrRange,
                    SourceLoc AtLoc, SourceLoc Loc,
                    llvm::function_ref<void(AvailableAttr *)> addAttribute);

  using PlatformAndVersion = std::pair<PlatformKind, llvm::VersionTuple>;

  /// Parse a platform and version tuple (e.g. "macOS 12.0") and append it to the
  /// given vector. Wildcards ('*') parse successfully but are ignored. Assumes
  /// that the tuples are part of a comma separated list ending with a trailing
  /// ')'.
  ParserStatus parsePlatformVersionInList(StringRef AttrName,
      llvm::SmallVector<PlatformAndVersion, 4> &PlatformAndVersions);

  //===--------------------------------------------------------------------===//
  // Code completion second pass.

  void performIDEInspectionSecondPassImpl(
      IDEInspectionDelayedDeclState &info);
};

/// Describes a parsed declaration name.
struct ParsedDeclName {
  /// The name of the context of which the corresponding entity should
  /// become a member.
  StringRef ContextName;

  /// The base name of the declaration.
  StringRef BaseName;

  /// The argument labels for a function declaration.
  SmallVector<StringRef, 4> ArgumentLabels;

  /// Whether this is a function name (vs. a value name).
  bool IsFunctionName = false;

  /// Whether this is a getter for the named property.
  bool IsGetter = false;

  /// Whether this is a setter for the named property.
  bool IsSetter = false;

  bool IsSubscript = false;

  /// For a declaration name that makes the declaration into an
  /// instance member, the index of the "Self" parameter.
  Optional<unsigned> SelfIndex;

  /// Determine whether this is a valid name.
  explicit operator bool() const { return !BaseName.empty(); }

  /// Whether this declaration name turns the declaration into a
  /// member of some named context.
  bool isMember() const { return !ContextName.empty(); }

  /// Whether the result is translated into an instance member.
  bool isInstanceMember() const {
    return isMember() && static_cast<bool>(SelfIndex);
  }

  /// Whether the result is translated into a static/class member.
  bool isClassMember() const {
    return isMember() && !static_cast<bool>(SelfIndex);
  }

  /// Whether this is a property accessor.
  bool isPropertyAccessor() const { return IsGetter || IsSetter; }

  /// Whether this is an operator.
  bool isOperator() const {
    return Lexer::isOperator(BaseName);
  }

  /// Form a declaration name from this parsed declaration name.
  DeclName formDeclName(ASTContext &ctx, bool isSubscript = false,
                        bool isCxxClassTemplateSpec = false) const;

  /// Form a declaration name from this parsed declaration name.
  DeclNameRef formDeclNameRef(ASTContext &ctx, bool isSubscript = false,
                              bool isCxxClassTemplateSpec = false) const;
};

/// To assist debugging parser crashes, tell us the location of the
/// current token.
class PrettyStackTraceParser : public llvm::PrettyStackTraceEntry {
  Parser &P;
public:
  explicit PrettyStackTraceParser(Parser &P) : P(P) {}
  void print(llvm::raw_ostream &out) const override;
};

/// Parse a stringified Swift declaration name,
/// e.g. "Foo.translateBy(self:x:y:)".
ParsedDeclName parseDeclName(StringRef name) LLVM_READONLY;

/// Form a Swift declaration name from its constituent parts.
DeclName formDeclName(ASTContext &ctx,
                      StringRef baseName,
                      ArrayRef<StringRef> argumentLabels,
                      bool isFunctionName,
                      bool isInitializer,
                      bool isSubscript = false,
                      bool isCxxClassTemplateSpec = false);

/// Form a Swift declaration name reference from its constituent parts.
DeclNameRef formDeclNameRef(ASTContext &ctx,
                            StringRef baseName,
                            ArrayRef<StringRef> argumentLabels,
                            bool isFunctionName,
                            bool isInitializer,
                            bool isSubscript = false,
                            bool isCxxClassTemplateSpec = false);

/// Whether a given token can be the start of a decl.
bool isKeywordPossibleDeclStart(const LangOptions &options, const Token &Tok);

} // end namespace swift

#endif
