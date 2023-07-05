//===--- ParseExpr.cpp - Swift Language Parser for Expressions ------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Expression Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Attr.h"
#include "swift/AST/DiagnosticsParse.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/EditorPlaceholder.h"
#include "swift/Parse/IDEInspectionCallbacks.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/StringExtras.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

/// parseExpr
///
///   expr:
///     expr-sequence(basic | trailing-closure)
///
/// \param isExprBasic Whether we're only parsing an expr-basic.
ParserResult<Expr> Parser::parseExprImpl(Diag<> Message,
                                         bool isExprBasic) {
  // If we are parsing a refutable pattern, check to see if this is the start
  // of a let/var/is pattern.  If so, parse it as an UnresolvedPatternExpr and
  // let pattern type checking determine its final form.
  //
  // Only do this if we're parsing a pattern, to improve QoI on malformed
  // expressions followed by (e.g.) let/var decls.
  //
  if (InBindingPattern && isOnlyStartOfMatchingPattern()) {
    ParserResult<Pattern> pattern = parseMatchingPattern(/*isExprBasic*/false);
    if (pattern.hasCodeCompletion())
      return makeParserCodeCompletionResult<Expr>();
    if (pattern.isNull())
      return nullptr;
    return makeParserResult(new (Context) UnresolvedPatternExpr(pattern.get()));
  }

  return parseExprSequence(Message, isExprBasic,
                                /*forConditionalDirective*/false);
}

/// parseExprIs
///   expr-is:
///     'is' type
ParserResult<Expr> Parser::parseExprIs() {
  SourceLoc isLoc;
  {
    isLoc = consumeToken(tok::kw_is);
  }

  ParserResult<TypeRepr> type = parseType(diag::expected_type_after_is);

  if (type.hasCodeCompletion())
    return makeParserCodeCompletionResult<Expr>();
  if (type.isNull())
    return nullptr;

  return makeParserResult(IsExpr::create(Context, isLoc, type.get()));
}

/// parseExprAs
///   expr-as:
///     'as' type
///     'as?' type
///     'as!' type
ParserResult<Expr> Parser::parseExprAs() {
  SourceLoc asLoc;
  SourceLoc questionLoc;
  SourceLoc exclaimLoc;

  {
    // Parse the 'as'.
    asLoc = consumeToken(tok::kw_as);

    // Parse the postfix '?'.
    if (Tok.is(tok::question_postfix)) {
      questionLoc = consumeToken(tok::question_postfix);
    } else if (Tok.is(tok::exclaim_postfix)) {
      exclaimLoc = consumeToken(tok::exclaim_postfix);
    }
  }

  ParserResult<TypeRepr> type = parseType(diag::expected_type_after_as);

  if (type.hasCodeCompletion())
    return makeParserCodeCompletionResult<Expr>();
  if (type.isNull())
    return nullptr;

  Expr *parsed;
  if (questionLoc.isValid()) {
    parsed = ConditionalCheckedCastExpr::create(Context, asLoc, questionLoc,
                                                type.get());
  } else if (exclaimLoc.isValid()) {
    parsed = ForcedCheckedCastExpr::create(Context, asLoc, exclaimLoc,
                                           type.get());
  } else {
    parsed = CoerceExpr::create(Context, asLoc, type.get());
  }
  return makeParserResult(parsed);
}

/// parseExprArrow
///
///   expr-arrow:
///     'async'? 'throws'? '->'
ParserResult<Expr> Parser::parseExprArrow() {
  SourceLoc asyncLoc, throwsLoc, arrowLoc;
  ParserStatus status;

  status |= parseEffectsSpecifiers(SourceLoc(),
                                   asyncLoc, /*reasync=*/nullptr,
                                   throwsLoc, /*rethrows=*/nullptr);
  if (status.hasCodeCompletion() && !CodeCompletionCallbacks) {
    // Trigger delayed parsing, no need to continue.
    return status;
  }

  if (Tok.isNot(tok::arrow)) {
    assert(throwsLoc.isValid() || asyncLoc.isValid());
    diagnose(throwsLoc.isValid() ? throwsLoc : asyncLoc,
             diag::async_or_throws_in_wrong_position,
             throwsLoc.isValid() ? "throws" : "async");
    return nullptr;
  }

  arrowLoc = consumeToken(tok::arrow);

  parseEffectsSpecifiers(arrowLoc,
                         asyncLoc, /*reasync=*/nullptr,
                         throwsLoc, /*rethrows=*/nullptr);

  auto arrow = new (Context) ArrowExpr(asyncLoc, throwsLoc, arrowLoc);
  return makeParserResult(arrow);
}

/// parseExprSequence
///
///   expr-sequence(Mode):
///     expr-sequence-element(Mode) expr-binary(Mode)*
///   expr-binary(Mode):
///     operator-binary expr-sequence-element(Mode)
///     '?' expr-sequence(Mode) ':' expr-sequence-element(Mode)
///     '=' expr-unary
///     expr-is
///     expr-as
///
/// The sequencing for binary exprs is not structural, i.e., binary operators
/// are not inherently right-associative. If present, '?' and ':' tokens must
/// match.
///
/// Similarly, the parsing of 'try' as part of expr-sequence-element
/// is not structural.  'try' is not permitted at arbitrary points in
/// a sequence; in the places it's permitted, it's hoisted out to
/// apply to everything to its right.
ParserResult<Expr> Parser::parseExprSequence(Diag<> Message,
                                             bool isExprBasic,
                                             bool isForConditionalDirective) {
  SmallVector<Expr*, 8> SequencedExprs;
  SourceLoc startLoc = Tok.getLoc();
  ParserStatus SequenceStatus;

  while (true) {
    if (isForConditionalDirective && Tok.isAtStartOfLine())
      break;

    // Parse a unary expression.
    ParserResult<Expr> Primary =
      parseExprSequenceElement(Message, isExprBasic);
    SequenceStatus |= Primary;

    if (SequenceStatus.hasCodeCompletion() && CodeCompletionCallbacks)
      CodeCompletionCallbacks->setLeadingSequenceExprs(SequencedExprs);

    if (Primary.isNull()) {
      if (SequenceStatus.hasCodeCompletion()) {
        SequencedExprs.push_back(new (Context) CodeCompletionExpr(PreviousLoc));
        break;
      }
      return nullptr;
    }
    SequencedExprs.push_back(Primary.get());

    if (SequenceStatus.isError() && !SequenceStatus.hasCodeCompletion())
      break;

    if (isForConditionalDirective && Tok.isAtStartOfLine())
      break;
    
parse_operator:
    switch (Tok.getKind()) {
    case tok::oper_binary_spaced:
    case tok::oper_binary_unspaced: {
      // If this is an "&& #available()" expression (or related things that
      // show up in a stmt-condition production), then don't eat it.
      //
      // These are not general expressions, and && is an infix operator,
      // so the code is invalid.  We get better recovery if we bail out from
      // this, because then we can produce a fixit to rewrite the && into a ,
      // if we're in a stmt-condition.
      if (Tok.getText() == "&&" &&
          (peekToken().isAny(tok::pound_available, tok::pound_unavailable,
                             tok::pound__hasSymbol, tok::kw_let, tok::kw_var,
                             tok::kw_case) ||
           (Context.LangOpts.hasFeature(Feature::ReferenceBindings) &&
            peekToken().isAny(tok::kw_inout))))
        goto done;
      
      // Parse the operator.
      Expr *Operator = parseExprOperator();
      SequencedExprs.push_back(Operator);

      // The message is only valid for the first subexpr.
      Message = diag::expected_expr_after_operator;
      break;
    }
    
    case tok::question_infix: {
      // Save the '?'.
      SourceLoc questionLoc = consumeToken();
      
      // Parse the middle expression of the ternary.
      ParserResult<Expr> middle = parseExprSequence(
          diag::expected_expr_after_ternary_question, isExprBasic);
      SequenceStatus |= middle;
      ParserStatus Status = middle;
      if (middle.isNull())
        return nullptr;

      // Make sure there's a matching ':' after the middle expr.
      if (!Tok.is(tok::colon)) {
        if (middle.hasCodeCompletion()) {
          SequencedExprs.push_back(new (Context) TernaryExpr(
              questionLoc, middle.get(), PreviousLoc));
          SequencedExprs.push_back(new (Context) CodeCompletionExpr(PreviousLoc));
          goto done;
        }

        diagnose(questionLoc, diag::expected_colon_after_ternary_question);
        Status.setIsParseError();
        return makeParserResult(Status, new (Context) ErrorExpr(
            {startLoc, middle.get()->getSourceRange().End}));
      }

      SourceLoc colonLoc = consumeToken();

      auto *unresolvedTernary =
          new (Context) TernaryExpr(questionLoc, middle.get(), colonLoc);
      SequencedExprs.push_back(unresolvedTernary);
      Message = diag::expected_expr_after_ternary_colon;
      break;
    }
        
    case tok::equal: {
      // If we're parsing an expression as the body of a refutable var/let
      // pattern, then an assignment doesn't make sense.  In a "if let"
      // statement the equals is the start of the condition, so don't parse it
      // as a binary operator.
      if (InBindingPattern)
        goto done;
      SourceLoc equalsLoc = consumeToken();
      auto *assign = new (Context) AssignExpr(equalsLoc);
      SequencedExprs.push_back(assign);
      Message = diag::expected_expr_assignment;
      break;
    }
        
    case tok::kw_is: {
      // Parse a type after the 'is' token instead of an expression.
      ParserResult<Expr> is = parseExprIs();
      if (is.isNull() || is.hasCodeCompletion())
        return is;
      
      // Store the expr itself as a placeholder RHS. The real RHS is the
      // type parameter stored in the node itself.
      SequenceStatus |= is;
      SequencedExprs.push_back(is.get());
      SequencedExprs.push_back(is.get());
      
      // We already parsed the right operand as part of the 'is' production.
      // Jump directly to parsing another operator.
      goto parse_operator;
    }
        
    case tok::kw_as: {
      ParserResult<Expr> as = parseExprAs();
      if (as.isNull() || as.hasCodeCompletion())
        return as;
        
      // Store the expr itself as a placeholder RHS. The real RHS is the
      // type parameter stored in the node itself.
      SequenceStatus |= as;
      SequencedExprs.push_back(as.get());
      SequencedExprs.push_back(as.get());
      
      // We already parsed the right operand as part of the 'is' production.
      // Jump directly to parsing another operator.
      goto parse_operator;
    }

    case tok::identifier: {
      // 'async' followed by 'throws' or '->' implies that we have an arrow
      // expression.
      if (!(Tok.isContextualKeyword("async") &&
            peekToken().isAny(tok::arrow, tok::kw_throws)))
        goto done;

      LLVM_FALLTHROUGH;
    }

    case tok::arrow:
    case tok::kw_throws: {
      ParserResult<Expr> arrow = parseExprArrow();
      if (arrow.isNull() || arrow.hasCodeCompletion())
        return arrow;
      SequenceStatus |= arrow;
      SequencedExprs.push_back(arrow.get());
      break;
    }
        
    default:
      // If the next token is not a binary operator, we're done.
      goto done;
    }
  }
done:

  // For conditional directives, we stop parsing after a line break.
  if (isForConditionalDirective && (SequencedExprs.size() & 1) == 0) {
    diagnose(getEndOfPreviousLoc(),
             diag::incomplete_conditional_compilation_directive);
    return makeParserError();
  }

  // If we had semantic errors, just fail here.
  assert(!SequencedExprs.empty());

  // If we saw no operators, don't build a sequence.
  if (SequencedExprs.size() == 1)
    return makeParserResult(SequenceStatus, SequencedExprs[0]);

  return makeParserResult(SequenceStatus,
                          SequenceExpr::create(Context, SequencedExprs));
}

/// parseExprSequenceElement
///
///   expr-sequence-element(Mode):
///     'await' expr-sequence-element(Mode)
///     'try' expr-sequence-element(Mode)
///     'try' '?' expr-sequence-element(Mode)
///     'try' '!' expr-sequence-element(Mode)
///     '_move' expr-sequence-element(Mode)
///     'borrow' expr-sequence-element(Mode)
///     expr-unary(Mode)
///
/// 'try' is not actually allowed at an arbitrary position of a
/// sequence, but this isn't enforced until sequence-folding.
ParserResult<Expr> Parser::parseExprSequenceElement(Diag<> message,
                                                    bool isExprBasic) {
  // Check whether the user mistyped "async" for "await", but only in cases
  // where we are sure that "async" would be ill-formed as an identifier.
  bool isReplaceableAsync = Tok.isContextualKeyword("async") &&
    !peekToken().isAtStartOfLine() &&
    (peekToken().is(tok::identifier) || peekToken().is(tok::kw_try));
  if (Tok.isContextualKeyword("await") || isReplaceableAsync) {
    // Error on a replaceable async
    if (isReplaceableAsync) {
      diagnose(Tok.getLoc(), diag::expected_await_not_async)
        .fixItReplace(Tok.getLoc(), "await");
    }
    Tok.setKind(tok::contextual_keyword);
    SourceLoc awaitLoc = consumeToken();
    ParserResult<Expr> sub =
      parseExprSequenceElement(diag::expected_expr_after_await, isExprBasic);
    if (!sub.hasCodeCompletion() && !sub.isNull()) {
      if (auto anyTry = dyn_cast<AnyTryExpr>(sub.get())) {
        // "try" must precede "await".
        diagnose(awaitLoc, diag::await_before_try)
          .fixItRemove(awaitLoc)
          .fixItInsert(anyTry->getSubExpr()->getStartLoc(), "await ");
      }

      sub = makeParserResult(new (Context) AwaitExpr(awaitLoc, sub.get()));
    }

   return sub;
  }

  if (Tok.isContextualKeyword("consume")
      && peekToken().isAny(tok::identifier, tok::kw_self, tok::dollarident,
                           tok::code_complete)
      && !peekToken().isAtStartOfLine()) {
    Tok.setKind(tok::contextual_keyword);

    SourceLoc consumeLoc = consumeToken();
    ParserResult<Expr> sub =
        parseExprSequenceElement(diag::expected_expr_after_move, isExprBasic);
    if (!sub.isNull()) {
      sub = makeParserResult(new (Context) ConsumeExpr(consumeLoc, sub.get()));
    }
    return sub;
  }

  if (Tok.isContextualKeyword("copy") &&
      peekToken().isAny(tok::identifier, tok::kw_self, tok::dollarident,
                        tok::code_complete) &&
      !peekToken().isAtStartOfLine()) {
    Tok.setKind(tok::contextual_keyword);

    SourceLoc copyLoc = consumeToken();
    ParserResult<Expr> sub =
        parseExprSequenceElement(diag::expected_expr_after_copy, isExprBasic);
    if (!sub.isNull()) {
      sub = makeParserResult(new (Context) CopyExpr(copyLoc, sub.get()));
    }
    return sub;
  }

  if (Context.LangOpts.hasFeature(Feature::OldOwnershipOperatorSpellings)) {
    if (Tok.isContextualKeyword("_move")) {
      Tok.setKind(tok::contextual_keyword);
      SourceLoc awaitLoc = consumeToken();
      diagnose(Tok, diag::move_consume_final_spelling)
        .fixItReplace(awaitLoc, "consume");

      ParserResult<Expr> sub =
          parseExprSequenceElement(diag::expected_expr_after_move, isExprBasic);
      if (!sub.hasCodeCompletion() && !sub.isNull()) {
        sub = makeParserResult(new (Context) ConsumeExpr(awaitLoc, sub.get()));
      }
      return sub;
    }

    if (Tok.isContextualKeyword("_borrow")) {
      Tok.setKind(tok::contextual_keyword);
      SourceLoc awaitLoc = consumeToken();
      ParserResult<Expr> sub = parseExprSequenceElement(
          diag::expected_expr_after_borrow, isExprBasic);
      if (!sub.hasCodeCompletion() && !sub.isNull()) {
        sub = makeParserResult(new (Context) BorrowExpr(awaitLoc, sub.get()));
      }
      return sub;
    }
  }

  SourceLoc tryLoc;
  bool hadTry = consumeIf(tok::kw_try, tryLoc);
  llvm::Optional<Token> trySuffix;
  if (hadTry && Tok.isAny(tok::exclaim_postfix, tok::question_postfix)) {
    trySuffix = Tok;
    consumeToken();
  }

  // Try to parse '@' sign or 'inout' as a attributed typerepr.
  if (Tok.isAny(tok::at_sign, tok::kw_inout)) {
    bool isType = false;
    {
      BacktrackingScope backtrack(*this);
      isType = canParseType();
    }
    if (isType) {
      ParserResult<TypeRepr> ty = parseType();
      if (ty.isNonNull())
        return makeParserResult(
            new (Context) TypeExpr(ty.get()));
      checkForInputIncomplete();
      return nullptr;
    }
  }

  ParserResult<Expr> sub = hadTry
      ? parseExprSequenceElement(message, isExprBasic)
      : parseExprUnary(message, isExprBasic);

  if (hadTry && !sub.hasCodeCompletion() && !sub.isNull()) {
    switch (trySuffix ? trySuffix->getKind() : tok::NUM_TOKENS) {
    case tok::exclaim_postfix:
      sub = makeParserResult(
          new (Context) ForceTryExpr(tryLoc, sub.get(), trySuffix->getLoc()));
      break;
    case tok::question_postfix:
      sub = makeParserResult(
          new (Context) OptionalTryExpr(tryLoc, sub.get(),
                                        trySuffix->getLoc()));
      break;
    default:
      // If this is a simple "try expr" situation, where the expr is a closure
      // literal, and the next token is a 'catch', then the user wrote
      // try/catch instead of do/catch.  Emit a fixit hint to rewrite to the
      // correct do/catch construct.
      if (Tok.is(tok::kw_catch) && isa<ClosureExpr>(sub.get())) {
        diagnose(tryLoc, diag::docatch_not_trycatch)
          .fixItReplace(tryLoc, "do");
        
        // Eat all of the catch clauses, so we don't trip over them in error
        // recovery.
        while (Tok.is(tok::kw_catch)) {
          ParserResult<CaseStmt> clause = parseStmtCatch();
          if (clause.hasCodeCompletion() && clause.isNull())
            break;
        }

        return makeParserResult(new (Context) ErrorExpr(tryLoc));
      }
        
      sub = makeParserResult(new (Context) TryExpr(tryLoc, sub.get()));
      break;
    }
  }

  return sub;
}

/// parseExprUnary
///
///   expr-unary(Mode):
///     expr-postfix(Mode)
///     operator-prefix expr-unary(Mode)
///     '&' expr-unary(Mode)
///
ParserResult<Expr> Parser::parseExprUnary(Diag<> Message, bool isExprBasic) {
  UnresolvedDeclRefExpr *Operator;

  // First check to see if we have the start of a regex literal `/.../`.
  tryLexRegexLiteral(/*forUnappliedOperator*/ false);

  // 'repeat' as an expression prefix is a pack expansion expression.
  if (Tok.is(tok::kw_repeat)) {
    SourceLoc repeatLoc = consumeToken();
    auto patternExpr = parseExpr(Message);
    if (patternExpr.isNull())
      return patternExpr;

    auto *expansion =
        PackExpansionExpr::create(Context, repeatLoc, patternExpr.get(),
                                  /*genericEnv*/ nullptr);
    return makeParserResult(expansion);
  }

  // Try parse an 'if' or 'switch' as an expression. Note we do this here in
  // parseExprUnary as we don't allow postfix syntax to hang off such
  // expressions to avoid ambiguities such as postfix '.member', which can
  // currently be parsed as a static dot member for a result builder.
  if (Tok.isAny(tok::kw_if, tok::kw_switch)) {
    auto Result = parseStmt();
    Expr *E = nullptr;
    if (Result.isNonNull()) {
      E = SingleValueStmtExpr::createWithWrappedBranches(
          Context, Result.get(), CurDeclContext, /*mustBeExpr*/ true);
    }
    return makeParserResult(ParserStatus(Result), E);
  }

  switch (Tok.getKind()) {
  default:
    // If the next token is not an operator, just parse this as expr-postfix.
    return parseExprPostfix(Message, isExprBasic);

  case tok::amp_prefix: {
    SourceLoc Loc = consumeToken(tok::amp_prefix);

    ParserResult<Expr> SubExpr = parseExprUnary(Message, isExprBasic);
    if (SubExpr.hasCodeCompletion())
      return makeParserCodeCompletionResult<Expr>(SubExpr.getPtrOrNull());
    if (SubExpr.isNull())
      return nullptr;
    return makeParserResult(
        new (Context) InOutExpr(Loc, SubExpr.get(), Type()));
  }

  case tok::backslash:
    return parseExprKeyPath();

  case tok::oper_postfix: {
    // Postfix operators cannot start a subexpression, but can happen
    // syntactically because the operator may just follow whatever precedes this
    // expression (and that may not always be an expression).
    diagnose(Tok, diag::invalid_postfix_operator);
    Tok.setKind(tok::oper_prefix);
    Operator = parseExprOperator();
    break;
  }
  case tok::oper_prefix: {
    Operator = parseExprOperator();
    break;
  }
  case tok::oper_binary_spaced:
  case tok::oper_binary_unspaced: {
    // For recovery purposes, accept an oper_binary here.
    SourceLoc OperEndLoc = Tok.getLoc().getAdvancedLoc(Tok.getLength());
    Tok.setKind(tok::oper_prefix);
    Operator = parseExprOperator();

    if (OperEndLoc == Tok.getLoc())
      diagnose(PreviousLoc, diag::expected_expr_after_unary_operator);
    else
      diagnose(PreviousLoc, diag::expected_prefix_operator)
          .fixItRemoveChars(OperEndLoc, Tok.getLoc());
    break;
  }
  }

  ParserResult<Expr> SubExpr = parseExprUnary(Message, isExprBasic);
  ParserStatus Status = SubExpr;
  if (SubExpr.isNull())
    return Status;

  // Check if we have a unary '-' with number literal sub-expression, for
  // example, "-42" or "-1.25".
  if (auto *LE = dyn_cast<NumberLiteralExpr>(SubExpr.get())) {
    if (Operator->hasName() && Operator->getName().getBaseName() == "-") {
      LE->setNegative(Operator->getLoc());
      return makeParserResult(Status, LE);
    }
  }

  auto *opCall = PrefixUnaryExpr::create(Context, Operator, SubExpr.get());
  return makeParserResult(Status, opCall);
}

/// expr-keypath-swift:
///   \ type? . initial-key-path-component key-path-components
///
/// key-path-components:
//    key-path-component*
///   <empty>
///
/// key-path-component:
///   .identifier
///   ?
///   !
///   [ expression ]
///
/// initial-key-path-component:
///   identifier
///   ?
///   !
///   [ expression ]
ParserResult<Expr> Parser::parseExprKeyPath() {
  // Consume '\'.
  SourceLoc backslashLoc = consumeToken(tok::backslash);
  llvm::SaveAndRestore<bool> S(InSwiftKeyPath, true);

  // FIXME: diagnostics
  ParserResult<Expr> rootResult, pathResult;
  ParserStatus parseStatus;

  if (!startsWithSymbol(Tok, '.')) {
    rootResult = parseExprPostfix(diag::expr_keypath_expected_expr,
                                  /*isBasic=*/true);
    parseStatus = rootResult;

    if (rootResult.isParseErrorOrHasCompletion())
      return rootResult;
  }

  bool hasLeadingDot = startsWithSymbol(Tok, '.');
  if (hasLeadingDot) {
    auto dotLoc = Tok.getLoc();
    // For uniformity, \.foo is parsed as if it were MAGIC.foo, so we need to
    // make sure the . is there, but parsing the ? in \.? as .? doesn't make
    // sense. This is all made more complicated by .?. being considered an
    // operator token. Since keypath allows '.!' '.?' and '.[', consume '.'
    // the token is a operator starts with '.', or the following token is '['.
    if ((Tok.isAnyOperator() && Tok.getLength() != 1) ||
        peekToken().is(tok::l_square)) {
      consumeStartingCharacterOfCurrentToken(tok::period);
    }

    auto inner = makeParserResult(new (Context) KeyPathDotExpr(dotLoc));
    bool unusedHasBindOptional = false;

    // Inside a keypath's path, the period always behaves normally: the key path
    // behavior is only the separation between type and path.
    pathResult = parseExprPostfixSuffix(inner, /*isExprBasic=*/true,
                                        /*periodHasKeyPathBehavior=*/false,
                                        unusedHasBindOptional);
    parseStatus |= pathResult;
  }

  if (rootResult.isNull() && pathResult.isNull())
    return nullptr;

  // Handle code completion.
  if ((Tok.is(tok::code_complete) && !Tok.isAtStartOfLine()) ||
      (Tok.is(tok::period) && peekToken().isAny(tok::code_complete))) {
    SourceLoc DotLoc;
    consumeIf(tok::period, DotLoc);

    // Add the code completion expression to the path result.
    CodeCompletionExpr *CC = new (Context)
        CodeCompletionExpr(pathResult.getPtrOrNull(), Tok.getLoc());
    auto *keypath = KeyPathExpr::createParsed(
        Context, backslashLoc, rootResult.getPtrOrNull(), CC, hasLeadingDot);
    if (this->CodeCompletionCallbacks)
      this->CodeCompletionCallbacks->completeExprKeyPath(keypath, DotLoc);
    consumeToken(tok::code_complete);
    return makeParserCodeCompletionResult(keypath);
  }

  auto *keypath = KeyPathExpr::createParsed(
      Context, backslashLoc, rootResult.getPtrOrNull(),
      pathResult.getPtrOrNull(), hasLeadingDot);
  return makeParserResult(parseStatus, keypath);
}

///   expr-keypath-objc:
///     '#keyPath' '(' unqualified-name ('.' unqualified-name) * ')'
///
ParserResult<Expr> Parser::parseExprKeyPathObjC() {
  // Consume '#keyPath'.
  SourceLoc keywordLoc = consumeToken(tok::pound_keyPath);

  // Parse the leading '('.
  if (!Tok.is(tok::l_paren)) {
    diagnose(Tok, diag::expr_keypath_expected_lparen);
    return makeParserError();
  }
  SourceLoc lParenLoc = consumeToken(tok::l_paren);

  SmallVector<KeyPathExpr::Component, 4> components;
  /// Handler for code completion.
  auto handleCodeCompletion = [&](SourceLoc DotLoc) -> ParserResult<Expr> {
    KeyPathExpr *expr = nullptr;
    if (!components.empty()) {
      expr = KeyPathExpr::createParsedPoundKeyPath(
          Context, keywordLoc, lParenLoc, components, Tok.getLoc());
    }

    if (this->CodeCompletionCallbacks)
      this->CodeCompletionCallbacks->completeExprKeyPath(expr, DotLoc);

    // Eat the code completion token because we handled it.
    consumeToken(tok::code_complete);
    return makeParserCodeCompletionResult(expr);
  };

  // Parse the sequence of unqualified-names.
  ParserStatus status;
  SourceLoc LastDotLoc;
  DeclNameOptions flags = DeclNameFlag::AllowCompoundNames |
                          DeclNameFlag::AllowLowercaseAndUppercaseSelf;
  while (true) {
    // Handle code completion.
    if (Tok.is(tok::code_complete))
      return handleCodeCompletion(LastDotLoc);

    // Parse the next name.
    DeclNameLoc nameLoc;
    DeclNameRef name = parseDeclNameRef(nameLoc,
        diag::expr_keypath_expected_property_or_type, flags);
    if (!name) {
      status.setIsParseError();
      break;
    }

    // Record the name we parsed.
    auto component = KeyPathExpr::Component::forUnresolvedProperty(name,
                                                      nameLoc.getBaseNameLoc());
    components.push_back(component);

    // After the first component, we can start parsing keywords.
    flags |= DeclNameFlag::AllowKeywords;

    // Handle code completion.
    if (Tok.is(tok::code_complete))
      return handleCodeCompletion(SourceLoc());

    // Parse the next period to continue the path.
    if (consumeIf(tok::period, LastDotLoc))
      continue;

    break;
  }

  // Parse the closing ')'.
  SourceLoc rParenLoc;
  if (status.isErrorOrHasCompletion()) {
    skipUntilDeclStmtRBrace(tok::r_paren);
    if (Tok.is(tok::r_paren))
      rParenLoc = consumeToken();
    else
      rParenLoc = PreviousLoc;
  } else {
    parseMatchingToken(tok::r_paren, rParenLoc,
                       diag::expr_keypath_expected_rparen, lParenLoc);
  }

  // If we cannot build a useful expression, just return an error
  // expression.
  if (components.empty() || status.isErrorOrHasCompletion()) {
    return makeParserResult<Expr>(
             new (Context) ErrorExpr(SourceRange(keywordLoc, rParenLoc)));
  }

  // We're done: create the key-path expression.
  return makeParserResult<Expr>(KeyPathExpr::createParsedPoundKeyPath(
      Context, keywordLoc, lParenLoc, components, rParenLoc));
}

/// parseExprSelector
///
///   expr-selector:
///     '#selector' '(' expr ')'
///     '#selector' '(' 'getter' ':' expr ')'
///     '#selector' '(' 'setter' ':' expr ')'
///
ParserResult<Expr> Parser::parseExprSelector() {
  // Consume '#selector'.
  SourceLoc keywordLoc = consumeToken(tok::pound_selector);

  // Parse the leading '('.
  if (!Tok.is(tok::l_paren)) {
    diagnose(Tok, diag::expr_selector_expected_lparen);
    return makeParserError();
  }
  SourceLoc lParenLoc = consumeToken(tok::l_paren);
  SourceLoc modifierLoc;

  // Parse possible 'getter:' or 'setter:' modifiers, and determine
  // the kind of selector we're working with.
  ObjCSelectorExpr::ObjCSelectorKind selectorKind;
  if (peekToken().is(tok::colon) &&
      (Tok.isContextualKeyword("getter") ||
       Tok.isContextualKeyword("setter"))) {
    // Parse the modifier.
    if (Tok.isContextualKeyword("getter"))
      selectorKind = ObjCSelectorExpr::Getter;
    else
      selectorKind = ObjCSelectorExpr::Setter;

    Tok.setKind(tok::contextual_keyword);
    modifierLoc = consumeToken();
    (void)consumeToken(tok::colon);
  } else {
    selectorKind = ObjCSelectorExpr::Method;
  }

  ObjCSelectorContext selectorContext;
  switch (selectorKind) {
  case ObjCSelectorExpr::Getter:
    selectorContext = ObjCSelectorContext::GetterSelector;
    break;
  case ObjCSelectorExpr::Setter:
    selectorContext = ObjCSelectorContext::SetterSelector;
    break;
  case ObjCSelectorExpr::Method:
    selectorContext = ObjCSelectorContext::MethodSelector;
  }

  // Parse the subexpression.
  CodeCompletionCallbacks::InObjCSelectorExprRAII InObjCSelectorExpr(
      CodeCompletionCallbacks, selectorContext);
  ParserResult<Expr> subExpr =
    parseExpr(selectorKind == ObjCSelectorExpr::Method
                ? diag::expr_selector_expected_method_expr
                : diag::expr_selector_expected_property_expr);

  // Parse the closing ')'.
  SourceLoc rParenLoc;
  if (subExpr.isParseErrorOrHasCompletion()) {
    skipUntilDeclStmtRBrace(tok::r_paren);
    if (Tok.is(tok::r_paren))
      rParenLoc = consumeToken();
    else
      rParenLoc = PreviousLoc;
  } else {
    parseMatchingToken(tok::r_paren, rParenLoc,
                       diag::expr_selector_expected_rparen, lParenLoc);
  }

  // If the subexpression was in error, just propagate the error.
  if (subExpr.isParseErrorOrHasCompletion() && !subExpr.hasCodeCompletion())
    return makeParserResult<Expr>(
      new (Context) ErrorExpr(SourceRange(keywordLoc, rParenLoc)));

  return makeParserResult<Expr>(
    new (Context) ObjCSelectorExpr(selectorKind, keywordLoc, lParenLoc,
                                   modifierLoc, subExpr.get(), rParenLoc));
}

static DeclRefKind getDeclRefKindForOperator(tok kind) {
  switch (kind) {
  case tok::oper_binary_spaced:
  case tok::oper_binary_unspaced:  return DeclRefKind::BinaryOperator;
  case tok::oper_postfix: return DeclRefKind::PostfixOperator;
  case tok::oper_prefix:  return DeclRefKind::PrefixOperator;
  default: llvm_unreachable("bad operator token kind");
  }
}

/// parseExprOperator - Parse an operator reference expression.  These
/// are not "proper" expressions; they can only appear in binary/unary
/// operators.
UnresolvedDeclRefExpr *Parser::parseExprOperator() {
  assert(Tok.isAnyOperator());
  DeclRefKind refKind = getDeclRefKindForOperator(Tok.getKind());
  SourceLoc loc = Tok.getLoc();
  DeclNameRef name(Context.getIdentifier(Tok.getText()));
  consumeToken();
  // Bypass local lookup.
  return new (Context) UnresolvedDeclRefExpr(name, refKind, DeclNameLoc(loc));
}

void Parser::tryLexRegexLiteral(bool forUnappliedOperator) {
  if (!Context.LangOpts.hasFeature(Feature::BareSlashRegexLiterals) ||
      !Context.LangOpts.EnableExperimentalStringProcessing)
    return;

  // Check to see if we have a regex literal `/.../`, optionally with a prefix
  // operator e.g `!/.../`.
  // NOTE: If you change this logic you must also change the logic in
  // isPotentialUnskippableBareSlashRegexLiteral.
  bool mustBeRegex = false;
  switch (Tok.getKind()) {
  case tok::oper_prefix:
    // Prefix operators may contain `/` characters, so this may not be a regex,
    // and as such need to make sure we have a closing `/`.
    break;
  case tok::oper_binary_spaced:
  case tok::oper_binary_unspaced:
    // When re-lexing for a unary expression, binary operators are always
    // invalid, so we can be confident in always lexing a regex literal.
    mustBeRegex = !forUnappliedOperator;
    break;
  default:
    // We only re-lex regex literals for operator tokens.
    return;
  }

  // Check to see if we have an operator containing '/'.
  auto slashIdx = Tok.getText().find("/");
  if (slashIdx == StringRef::npos)
    return;

  CancellableBacktrackingScope backtrack(*this);
  {
    llvm::Optional<Lexer::ForwardSlashRegexRAII> regexScope;
    regexScope.emplace(*L, mustBeRegex);

    // Try re-lex as a `/.../` regex literal, this will split an operator if
    // necessary.
    L->restoreState(getParserPosition().LS, /*enableDiagnostics*/ true);

    // If we didn't split a prefix operator, reset the regex lexing scope.
    // Otherwise, we want to keep it in place for the next token.
    auto didSplit = L->peekNextToken().getLength() == slashIdx;
    if (!didSplit)
      regexScope.reset();

    // Discard the current token, which will be replaced by the re-lexed
    // token, which will either be a regex literal token, a prefix operator,
    // or the original unchanged token.
    discardToken();

    // If we split a prefix operator from the regex literal, and are not sure
    // whether this should be a regex, backtrack if we didn't end up lexing a
    // regex literal.
    if (didSplit && !mustBeRegex &&
        !L->peekNextToken().is(tok::regex_literal)) {
      return;
    }

    // Otherwise, accept the result.
    backtrack.cancelBacktrack();
  }
}

/// parseExprSuper
///
///   expr-super:
///     expr-super-member
///     expr-super-init
///     expr-super-subscript
///   expr-super-member:
///     'super' '.' identifier
///   expr-super-init:
///     'super' '.' 'init'
///   expr-super-subscript:
///     'super' '[' expr ']'
ParserResult<Expr> Parser::parseExprSuper() {
  // Parse the 'super' reference.
  SourceLoc superLoc = consumeToken(tok::kw_super);

  // 'super.' must be followed by a member ref, explicit initializer ref, or
  // subscript call.
  if (!Tok.isAny(tok::period, tok::period_prefix, tok::code_complete) &&
      !Tok.isFollowingLSquare()) {
    if (!consumeIf(tok::unknown))
      diagnose(Tok, diag::expected_dot_or_subscript_after_super);
    return nullptr;
  }

  return makeParserResult(new (Context) SuperRefExpr(/*selfDecl=*/nullptr,
                                                     superLoc,
                                                     /*Implicit=*/false));
}

/// Copy a numeric literal value into AST-owned memory, stripping underscores
/// so the semantic part of the value can be parsed by APInt/APFloat parsers.
StringRef Parser::copyAndStripUnderscores(StringRef orig) {
  char *start = static_cast<char*>(Context.Allocate(orig.size(), 1));
  char *p = start;

  if (p) {
    for (char c : orig) {
      if (c != '_') {
        *p++ = c;
      }
    }
  }
  
  return StringRef(start, p - start);
}

StringRef Parser::stripUnderscoresIfNeeded(StringRef text,
                                           SmallVectorImpl<char> &buffer) {
  if (text.contains('_')) {
    buffer.clear();
    llvm::copy_if(text, std::back_inserter(buffer),
                  [](char ch) { return ch != '_'; });
    return StringRef(buffer.data(), buffer.size());
  }
  return text;
}

/// Disambiguate the parse after '{' token that is in a place that might be
/// the start of a trailing closure, or start the variable accessor block.
///
/// Check to see if the '{' is followed by a 'didSet' or a 'willSet' label,
/// possibly preceded by attributes.  If so, we disambiguate the parse as the
/// start of a get-set block in a variable definition (not as a trailing
/// closure).
bool Parser::isStartOfGetSetAccessor() {
  assert(Tok.is(tok::l_brace) && "not checking a brace?");

  // The only case this can happen is if the accessor label is immediately after
  // a brace (possibly preceded by attributes).  "get" is implicit, so it can't
  // be checked for.  Conveniently however, get/set properties are not allowed
  // to have initializers unless they have `init` accessor, so we don't have an
  // ambiguity, we just have to check for observing accessors and init accessor.
  //
  // If we have a 'didSet' or a 'willSet' label, disambiguate immediately as
  // an accessor block.
  Token NextToken = peekToken();
  if (NextToken.isContextualKeyword("didSet") ||
      NextToken.isContextualKeyword("willSet") || NextToken.is(tok::kw_init))
    return true;

  // If we don't have attributes, then it cannot be an accessor block.
  if (NextToken.isNot(tok::at_sign))
    return false;

  Parser::BacktrackingScope Backtrack(*this);

  // Eat the "{".
  consumeToken(tok::l_brace);

  // Eat attributes, if present.
  while (consumeIf(tok::at_sign)) {
    if (!consumeIf(tok::identifier))
      return false;
    // Eat paren after attribute name; e.g. @foo(x)
    if (Tok.is(tok::l_paren))
      skipSingle();
  }

  // Check if we have 'didSet'/'willSet' or 'init' after attributes.
  return Tok.isContextualKeyword("didSet") ||
         Tok.isContextualKeyword("willSet") || Tok.is(tok::kw_init);
}

/// Recover invalid uses of trailing closures in a situation
/// where the parser requires an expr-basic (which does not allow them).  We
/// handle this by doing some lookahead in common situations. And later, Sema
/// will emit a diagnostic with a fixit to add wrapping parens.
static bool isValidTrailingClosure(bool isExprBasic, Parser &P){
  assert(P.Tok.is(tok::l_brace) && "Couldn't be a trailing closure");
  
  // If this is the start of a get/set accessor, then it isn't a trailing
  // closure.
  if (P.isStartOfGetSetAccessor())
    return false;

  // If this is the start of a switch body, this isn't a trailing closure.
  if (P.peekToken().is(tok::kw_case))
    return false;

  // If this is a normal expression (not an expr-basic) then trailing closures
  // are allowed, so this is obviously one.
  // TODO: We could handle try to disambiguate cases like:
  //   let x = foo
  //   {...}()
  // by looking ahead for the ()'s, but this has been replaced by do{}, so this
  // probably isn't worthwhile.
  //
  if (!isExprBasic)
    return true;
  
  // If this is an expr-basic, then a trailing closure is not allowed.  However,
  // it is very common for someone to write something like:
  //
  //    for _ in numbers.filter {$0 > 4} {
  //
  // and we want to recover from this very well.   We need to perform arbitrary
  // look-ahead to disambiguate this case, so we only do this in the case where
  // the token after the { is on the same line as the {.
  if (P.peekToken().isAtStartOfLine())
    return false;

  // Determine if the {} goes with the expression by eating it, and looking
  // to see if it is immediately followed by a token which indicates we should
  // consider it part of the preceding expression
  Parser::BacktrackingScope backtrack(P);
  P.consumeToken(tok::l_brace);
  P.skipUntil(tok::r_brace);
  SourceLoc endLoc;
  if (!P.consumeIf(tok::r_brace, endLoc))
    return false;

  switch (P.Tok.getKind()) {
  case tok::l_brace:
  case tok::kw_where:
  case tok::comma:
    return true;
  case tok::l_square:
  case tok::l_paren:
  case tok::period:
  case tok::period_prefix:
  case tok::kw_is:
  case tok::kw_as:
  case tok::question_postfix:
  case tok::question_infix:
  case tok::exclaim_postfix:
  case tok::colon:
  case tok::equal:
  case tok::oper_postfix:
  case tok::oper_binary_spaced:
  case tok::oper_binary_unspaced:
    return !P.Tok.isAtStartOfLine();
  default:
    return false;
  }
}



/// Map magic literal tokens such as #file to their
/// MagicIdentifierLiteralExpr kind.
static MagicIdentifierLiteralExpr::Kind
getMagicIdentifierLiteralKind(tok Kind, const LangOptions &Opts) {
  switch (Kind) {
  case tok::pound_file:
    // TODO(https://github.com/apple/swift/issues/55639): Enable by default at the next source break.
    return Opts.hasFeature(Feature::ConciseMagicFile)
         ? MagicIdentifierLiteralExpr::FileIDSpelledAsFile
         : MagicIdentifierLiteralExpr::FilePathSpelledAsFile;
#define MAGIC_IDENTIFIER_TOKEN(NAME, TOKEN) \
  case tok::TOKEN: \
    return MagicIdentifierLiteralExpr::Kind::NAME;
#include "swift/AST/MagicIdentifierKinds.def"
  default:
    llvm_unreachable("not a magic literal");
  }
}

ParserResult<Expr>
Parser::parseExprPostfixSuffix(ParserResult<Expr> Result, bool isExprBasic,
                               bool periodHasKeyPathBehavior,
                               bool &hasBindOptional) {
  hasBindOptional = false;

  // Handle suffix expressions.
  while (1) {
    // FIXME: Better recovery.
    if (Result.isNull())
      return Result;

    if (InPoundIfEnvironment && Tok.isAtStartOfLine())
      return Result;

    if (Result.hasCodeCompletion() &&
        SourceMgr.getIDEInspectionTargetLoc() == PreviousLoc) {
      // Don't parse suffixes if the expression ended with code completion
      // token. Because, for example, given:
      //   [.foo(), .bar()]
      // If user want to insert another element in between:
      //   [.foo(), <HERE> .bar()]
      // '.bar()' is probably not a part of the inserting element. Moreover,
      // having suffixes doesn't help type inference in any way.

      return Result;
    }

    // Check for a .foo suffix.
    SourceLoc TokLoc = Tok.getLoc();
    if (Tok.is(tok::period) || Tok.is(tok::period_prefix)) {
      // A key path is special, because it allows .[, unlike anywhere else. The
      // period itself should be left in the token stream. (.? and .! end up
      // being operators, and so aren't handled here.)
      if (periodHasKeyPathBehavior && peekToken().is(tok::l_square)) {
        break;
      }
      // Completion for keyPath expression is handled in parseExprKeyPath.
      if (InSwiftKeyPath && peekToken().is(tok::code_complete))
        break;

      Tok.setKind(tok::period);
      consumeToken();

      // Handle "x.42" - a tuple index.
      if (Tok.is(tok::integer_literal)) {
        DeclNameRef name(Context.getIdentifier(Tok.getText()));
        SourceLoc nameLoc = consumeToken(tok::integer_literal);

        // Don't allow '.<integer literal>' following a numeric literal
        // expression (unless in #if env, for 1.2.3.4 version numbers)
        if (!InPoundIfEnvironment && Result.isNonNull() &&
            isa<NumberLiteralExpr>(Result.get())) {
          diagnose(nameLoc, diag::numeric_literal_numeric_member)
              .highlight(Result.get()->getSourceRange());
          continue;
        }

        Result = makeParserResult(
            Result, new (Context) UnresolvedDotExpr(Result.get(), TokLoc, name,
                                                    DeclNameLoc(nameLoc),
                                                    /*Implicit=*/false));
        continue;
      }

      // Handle "x.self" expr.
      if (Tok.is(tok::kw_self)) {
        Result = makeParserResult(
            Result,
            new (Context) DotSelfExpr(Result.get(), TokLoc, consumeToken()));
        continue;
      }

      // Handle "x.<tab>" for code completion.
      if (Tok.is(tok::code_complete)) {
        assert(!InSwiftKeyPath);
        auto CCExpr = new (Context) CodeCompletionExpr(Result.get(),
                                                       Tok.getLoc());
        if (CodeCompletionCallbacks) {
          CodeCompletionCallbacks->completeDotExpr(CCExpr, /*DotLoc=*/TokLoc);
        }
        consumeToken(tok::code_complete);

        // Parse and discard remaining suffixes.
        // e.g.
        //   closureReceiver {
        //     baseExpr.<complete> { $0 }
        //   }
        // In this case, we want to consume the trailing closure because
        // otherwise it will get parsed as a get-set clause on a variable
        // declared by `baseExpr.<complete>` which is clearly wrong.
        bool hasBindOptional = false;
        parseExprPostfixSuffix(makeParserResult(CCExpr), isExprBasic,
                               periodHasKeyPathBehavior, hasBindOptional);

        return makeParserCodeCompletionResult(CCExpr);
      }

      DeclNameLoc NameLoc;
      Diag<> D = isa<SuperRefExpr>(Result.get())
                     ? diag::expected_identifier_after_super_dot_expr
                     : diag::expected_member_name;
      auto Name = parseDeclNameRef(
          NameLoc, D,
          DeclNameFlag::AllowKeywords | DeclNameFlag::AllowCompoundNames |
              DeclNameFlag::AllowLowercaseAndUppercaseSelf);
      if (!Name) {
        SourceRange ErrorRange = Result.get()->getSourceRange();
        ErrorRange.widen(TokLoc);
        return makeParserErrorResult(new (Context) ErrorExpr(ErrorRange, Type(), Result.get()));
      }
      Result = makeParserResult(Result, new (Context) UnresolvedDotExpr(
                                            Result.get(), TokLoc, Name, NameLoc,
                                            /*Implicit=*/false));

      if (canParseAsGenericArgumentList()) {
        SmallVector<TypeRepr *, 8> args;
        SourceLoc LAngleLoc, RAngleLoc;
        auto argStat = parseGenericArguments(args, LAngleLoc, RAngleLoc);
        Result = makeParserResult(Result | argStat, Result.getPtrOrNull());
        if (argStat.isErrorOrHasCompletion())
          diagnose(LAngleLoc, diag::while_parsing_as_left_angle_bracket);

        Result = makeParserResult(
            Result, UnresolvedSpecializeExpr::create(
                        Context, Result.get(), LAngleLoc, args, RAngleLoc));
      }

      continue;
    }

    // If there is an expr-call-suffix, parse it and form a call.
    if (Tok.isFollowingLParen()) {
      Result = parseExprCallSuffix(Result, isExprBasic);
      continue;
    }

    // Check for a [expr] suffix.
    // Note that this cannot be the start of a new line.
    if (Tok.isFollowingLSquare()) {
      auto args = parseArgumentList(tok::l_square, tok::r_square,  isExprBasic);
      auto *subscript = SubscriptExpr::create(Context, Result.get(),
                                              args.get());
      Result = makeParserResult(ParserStatus(args) | Result, subscript);
      continue;
    }

    // Check for a trailing closure, if allowed.
    if (Tok.is(tok::l_brace) && isValidTrailingClosure(isExprBasic, *this)) {
      // FIXME: if Result has a trailing closure, break out.

      // Stop after literal expressions, which may never have trailing closures.
      const auto *callee = Result.get();
      if (isa<LiteralExpr>(callee) || isa<CollectionExpr>(callee) ||
          isa<TupleExpr>(callee))
        break;

      SmallVector<Argument, 2> trailingClosures;
      auto trailingResult =
          parseTrailingClosures(isExprBasic, callee->getSourceRange(),
                                trailingClosures);
      if (trailingClosures.empty())
        return nullptr;

      // Trailing closure implicitly forms a call.
      auto *argList = ArgumentList::createParsed(Context, SourceLoc(),
                                                 trailingClosures, SourceLoc(),
                                                 /*trailingClosureIdx*/ 0);
      Result = makeParserResult(
          ParserStatus(Result) | trailingResult,
          CallExpr::create(Context, Result.get(), argList, /*implicit*/ false));

      // We only allow a single trailing closure on a call.  This could be
      // generalized in the future, but needs further design.
      if (Tok.is(tok::l_brace))
        break;
      continue;
    }

    // Check for a ? suffix.
    if (consumeIf(tok::question_postfix)) {
      Result = makeParserResult(Result, new (Context) BindOptionalExpr(
                                            Result.get(), TokLoc, /*depth*/ 0));
      hasBindOptional = true;
      continue;
    }

    // Check for a ! suffix.
    if (consumeIf(tok::exclaim_postfix)) {
      Result = makeParserResult(
          Result, new (Context) ForceValueExpr(Result.get(), TokLoc));
      continue;
    }

    // Check for a postfix-operator suffix.
    if (Tok.is(tok::oper_postfix)) {
      // KeyPaths are more restricted in what can go after a ., and so we treat
      // them specially.
      if (periodHasKeyPathBehavior && startsWithSymbol(Tok, '.'))
        break;

      Expr *oper = parseExprOperator();

      Result = makeParserResult(
          Result, PostfixUnaryExpr::create(Context, oper, Result.get()));
      continue;
    }

    if (Tok.is(tok::pound_if)) {

      // Helper function to see if we can parse member reference like suffixes
      // inside '#if'.
      auto isAtStartOfPostfixExprSuffix = [&]() {
        if (!Tok.isAny(tok::period, tok::period_prefix)) {
          return false;
        }
        if (!peekToken().isAny(tok::identifier, tok::kw_Self, tok::kw_self,
                               tok::integer_literal, tok::code_complete) &&
            !peekToken().isKeyword()) {
          return false;
        }
        return true;
      };

      // Check if the first '#if' body starts with '.' <identifier>, and parse
      // it as a "postfix ifconfig expression".
      bool isPostfixIfConfigExpr = false;
      {
        llvm::SaveAndRestore<llvm::Optional<StableHasher>> H(CurrentTokenHash,
                                                             llvm::None);
        Parser::BacktrackingScope Backtrack(*this);
        // Skip to the first body. We may need to skip multiple '#if' directives
        // since we support nested '#if's. e.g.
        //   baseExpr
        //   #if CONDITION_1
        //     #if CONDITION_2
        //       .someMember
        do {
          consumeToken(tok::pound_if);
          skipUntilTokenOrEndOfLine(tok::NUM_TOKENS);
        } while (Tok.is(tok::pound_if));
        isPostfixIfConfigExpr = isAtStartOfPostfixExprSuffix();
      }
      if (!isPostfixIfConfigExpr)
        break;

      if (!Tok.isAtStartOfLine()) {
        diagnose(Tok, diag::statement_same_line_without_newline)
          .fixItInsert(getEndOfPreviousLoc(), "\n");
      }

      llvm::SmallPtrSet<Expr *, 4> exprsWithBindOptional;
      auto ICD =
          parseIfConfig([&](SmallVectorImpl<ASTNode> &elements, bool isActive) {
            // Although we know the '#if' body starts with period,
            // '#elseif'/'#else' bodies might start with invalid tokens.
            if (isAtStartOfPostfixExprSuffix() || Tok.is(tok::pound_if)) {
              bool exprHasBindOptional = false;
              auto expr = parseExprPostfixSuffix(Result, isExprBasic,
                                                 periodHasKeyPathBehavior,
                                                 exprHasBindOptional);
              if (exprHasBindOptional)
                exprsWithBindOptional.insert(expr.get());
              elements.push_back(expr.get());
            }

            // Don't allow any character other than the postfix expression.
            if (!Tok.isAny(tok::pound_elseif, tok::pound_else, tok::pound_endif,
                           tok::eof)) {
              diagnose(Tok, diag::expr_postfix_ifconfig_unexpectedtoken);
              skipUntilConditionalBlockClose();
            }
          });
      if (ICD.isNull())
        break;

      auto activeElements = ICD.get()->getActiveClauseElements();
      if (activeElements.empty())
        // There's no active clause, or it was empty. Keep the current result.
        continue;

      // Extract the parsed expression as the result.
      assert(activeElements.size() == 1 && activeElements[0].is<Expr *>());
      auto expr = activeElements[0].get<Expr *>();
      ParserStatus status(ICD);
      auto charRange = Lexer::getCharSourceRangeFromSourceRange(
          SourceMgr, expr->getSourceRange());
      if (SourceMgr.rangeContainsIDEInspectionTarget(charRange) &&
          L->isCodeCompletion())
        status.setHasCodeCompletion();
      hasBindOptional |= exprsWithBindOptional.contains(expr);
      Result = makeParserResult(status, expr);
      continue;
    }

    if (Tok.is(tok::code_complete)) {
      if (InSwiftKeyPath)
        return Result;

      if (Tok.isAtStartOfLine()) {
        // Postfix expression is located on a different line than the code
        // completion token, and thus they are not related.
        return Result;
      }

      if (CodeCompletionCallbacks && Result.isNonNull()) {
        bool hasSpace = Tok.getLoc() != getEndOfPreviousLoc();
        CodeCompletionCallbacks->completePostfixExpr(Result.get(), hasSpace);
      }
      // Eat the code completion token because we handled it.
      consumeToken(tok::code_complete);
      Result.setHasCodeCompletionAndIsError();
      return Result;
    }

    // If we end up with an unknown token on this line, return an ErrorExpr
    // covering the range of the token.
    if (!Tok.isAtStartOfLine() && Tok.is(tok::unknown)) {
      SourceLoc UnknownLoc = consumeToken();
      SourceRange ErrorRange = Result.get()->getSourceRange();
      ErrorRange.widen(UnknownLoc);
      Result = makeParserResult(Result, new (Context) ErrorExpr(ErrorRange,
                                                                Type(),
                                                                Result.get()));
      continue;
    }

    // Otherwise, we don't know what this token is, it must end the expression.
    break;
  }

  return Result;
}

/// parseExprPostfix
///
///   expr-dot:
///     expr-postfix '.' 'type'
///     expr-postfix '.' (identifier|keyword) generic-args? expr-call-suffix?
///     expr-postfix '.' integer_literal
///
///   expr-subscript:
///     expr-postfix '[' expr ']'
///
///   expr-call:
///     expr-postfix expr-paren
///
///   expr-force-value:
///     expr-postfix '!'
///
///   expr-trailing-closure:
///     expr-postfix(trailing-closure) expr-closure
///
///   expr-postfix(Mode):
///     expr-postfix(Mode) operator-postfix
///
///   expr-postfix(basic):
///     expr-primary
///     expr-dot
///     expr-metatype
///     expr-init
///     expr-subscript
///     expr-call
///     expr-force-value
///
///   expr-postfix(trailing-closure):
///     expr-postfix(basic)
///     expr-trailing-closure
///
ParserResult<Expr> Parser::parseExprPostfix(Diag<> ID, bool isExprBasic) {
  auto Result = parseExprPrimary(ID, isExprBasic);
  // If we couldn't parse any expr, don't attempt to parse suffixes.
  if (Result.isNull())
    return Result;

  bool hasBindOptional = false;
  Result = parseExprPostfixSuffix(Result, isExprBasic,
                                  /*periodHasKeyPathBehavior=*/InSwiftKeyPath,
                                  hasBindOptional);
  if (Result.isParseErrorOrHasCompletion() || Result.hasCodeCompletion())
    return Result;

  // If we had a ? suffix expression, bind the entire postfix chain
  // within an OptionalEvaluationExpr.
  if (hasBindOptional) {
    Result = makeParserResult(new (Context) OptionalEvaluationExpr(Result.get()));
  }

  return Result;
}

/// parseExprPrimary
///
///   expr-literal:
///     integer_literal
///     floating_literal
///     string_literal
///     nil
///     true
///     false
///     #file
///     #line
///     #column
///     #function
///     #dsohandle
///
///   expr-delayed-identifier:
///     '.' identifier
///
///   expr-discard:
///     '_'
///
///   expr-primary:
///     expr-literal
///     expr-identifier expr-call-suffix?
///     expr-closure
///     expr-anon-closure-argument
///     expr-delayed-identifier
///     expr-paren
///     expr-super
///     expr-discard
///     expr-selector
///
ParserResult<Expr> Parser::parseExprPrimary(Diag<> ID, bool isExprBasic) {
  switch (Tok.getKind()) {
  case tok::integer_literal: {
    StringRef Text = copyAndStripUnderscores(Tok.getText());
    SourceLoc Loc = consumeToken(tok::integer_literal);
    return makeParserResult(new (Context)
                                IntegerLiteralExpr(Text, Loc,
                                                   /*Implicit=*/false));
  }
  case tok::floating_literal: {
    StringRef Text = copyAndStripUnderscores(Tok.getText());
    SourceLoc Loc = consumeToken(tok::floating_literal);
    return makeParserResult(new (Context) FloatLiteralExpr(Text, Loc,
                                                           /*Implicit=*/false));
  }
  case tok::at_sign:
    // Objective-C programmers habitually type @"foo", so recover gracefully
    // with a fixit.  If this isn't @"foo", just handle it like an unknown
    // input.
    if (peekToken().isNot(tok::string_literal))
      goto UnknownCharacter;
    
    diagnose(Tok.getLoc(), diag::string_literal_no_atsign)
      .fixItRemove(Tok.getLoc());
    consumeToken(tok::at_sign);
    LLVM_FALLTHROUGH;
      
  case tok::string_literal:  // "foo"
    return parseExprStringLiteral();

  case tok::regex_literal:
    return parseExprRegexLiteral();

  case tok::kw_nil:
    return makeParserResult(new (Context)
                                NilLiteralExpr(consumeToken(tok::kw_nil)));

  case tok::kw_true:
  case tok::kw_false: {
    bool isTrue = Tok.is(tok::kw_true);
    return makeParserResult(new (Context)
                                BooleanLiteralExpr(isTrue, consumeToken()));
  }

  // Cases for non-deprecated magic identifier tokens
  case tok::pound_file:
#define MAGIC_IDENTIFIER_TOKEN(NAME, TOKEN) case tok::TOKEN:
#include "swift/AST/MagicIdentifierKinds.def"
  {
    auto Kind = getMagicIdentifierLiteralKind(Tok.getKind(), Context.LangOpts);
    SourceLoc Loc = consumeToken();
    return makeParserResult(new (Context) MagicIdentifierLiteralExpr(
        Kind, Loc, /*implicit=*/false));
  }

  case tok::identifier:  // foo
  case tok::kw_self: {   // self
    auto canParseBindingInPattern = [&]() {
      if (InBindingPattern != PatternBindingState::ImplicitlyImmutable &&
          !InBindingPattern.getIntroducer().has_value()) {
        return false;
      }
      // If we have "case let x.", "case let x(", or "case let x[", we parse 'x'
      // as a normal name, not a binding, because it is the start of an enum
      // pattern, call, or subscript.
      if (peekToken().isAny(tok::period, tok::period_prefix, tok::l_paren,
                            tok::l_square)) {
        return false;
      }
      // If we have a generic argument list, this is something like
      // "case let E<Int>.e(y)", and 'E' should be parsed as a normal name, not
      // a binding.
      if (peekToken().isAnyOperator() && peekToken().getText().equals("<")) {
        BacktrackingScope S(*this);
        consumeToken();
        return !canParseAsGenericArgumentList();
      }
      return true;
    }();
    // If we are parsing a refutable pattern and are inside a let/var pattern,
    // the identifiers change to be value bindings instead of decl references.
    // Parse and return this as an UnresolvedPatternExpr around a binding.  This
    // will be resolved (or rejected) by sema when the overall refutable pattern
    // it transformed from an expression into a pattern.
    if (canParseBindingInPattern) {
      Identifier name;
      SourceLoc loc = consumeIdentifier(name, /*diagnoseDollarPrefix=*/false);
      // If we have an inout/let/var, set that as our introducer. otherwise
      // default to Let.
      auto introducer =
          InBindingPattern.getIntroducer().value_or(VarDecl::Introducer::Let);
      auto pattern = createBindingFromPattern(loc, name, introducer);
      return makeParserResult(new (Context) UnresolvedPatternExpr(pattern));
    }

    // 'any' followed by another identifier is an existential type.
    if (Tok.isContextualKeyword("any") &&
        peekToken().is(tok::identifier) &&
        !peekToken().isAtStartOfLine()) {
      ParserResult<TypeRepr> ty = parseType();
      auto *typeExpr = new (Context) TypeExpr(ty.get());
      return makeParserResult(typeExpr);
    }

    // 'each' followed by another identifier is a pack element expr.
    if (Tok.isContextualKeyword("each") &&
        peekToken().is(tok::identifier) &&
        !peekToken().isAtStartOfLine()) {
      SourceLoc loc = consumeToken();
      ParserResult<Expr> ref = parseExpr(ID);
      if (ref.isNull())
        return ref;

      auto *packRef = PackElementExpr::create(Context, loc, ref.get());
      return makeParserResult(packRef);
    }

    LLVM_FALLTHROUGH;
  }
  case tok::kw_Self:     // Self
    return parseExprIdentifier();

  case tok::kw_Any: { // Any
    auto TyR = parseAnyType();
    return makeParserResult(new (Context) TypeExpr(TyR.get()));
  }

  case tok::dollarident: // $1
    return makeParserResult(parseExprAnonClosureArg());

  case tok::kw__: // _
    return makeParserResult(
      new (Context) DiscardAssignmentExpr(consumeToken(), /*Implicit=*/false));

  case tok::pound_selector: // expr-selector
    return parseExprSelector();

  case tok::pound_keyPath:
    return parseExprKeyPathObjC();

  case tok::l_brace:     // expr-closure
    return parseExprClosure();

  case tok::period:              //=.foo
  case tok::period_prefix: {     // .foo
    Tok.setKind(tok::period_prefix);
    SourceLoc DotLoc = consumeToken();
    
    // Special case ".<integer_literal>" like ".4".  This isn't valid, but the
    // developer almost certainly meant to use "0.4".  Diagnose this, and
    // recover as if they wrote that.
    if (Tok.is(tok::integer_literal) && !Tok.isAtStartOfLine()) {
      diagnose(DotLoc, diag::invalid_float_literal_missing_leading_zero,
               Tok.getText())
        .fixItInsert(DotLoc, "0")
        .highlight({DotLoc, Tok.getLoc()});
      char *Ptr = (char*)Context.Allocate(Tok.getLength()+2, 1);
      memcpy(Ptr, "0.", 2);
      memcpy(Ptr+2, Tok.getText().data(), Tok.getLength());
      auto FltText = StringRef(Ptr, Tok.getLength()+2);
      FltText = copyAndStripUnderscores(FltText);
      
      consumeToken(tok::integer_literal);
      return makeParserResult(new (Context)
                                  FloatLiteralExpr(FltText, DotLoc,
                                                   /*Implicit=*/false));
    }
    
    DeclNameRef Name;
    DeclNameLoc NameLoc;

    if (Tok.is(tok::code_complete)) {
      auto CCE = new (Context) CodeCompletionExpr(Tok.getLoc());
      auto Result = makeParserResult(CCE);
      Result.setHasCodeCompletionAndIsError();
      if (CodeCompletionCallbacks) {
        CodeCompletionCallbacks->completeUnresolvedMember(CCE, DotLoc);
      }
      consumeToken();
      return Result;
    }

    Name = parseDeclNameRef(NameLoc, diag::expected_identifier_after_dot_expr,
                            DeclNameFlag::AllowKeywords |
                                DeclNameFlag::AllowCompoundNames |
                                DeclNameFlag::AllowLowercaseAndUppercaseSelf);
    if (!Name)
      return makeParserErrorResult(new (Context) ErrorExpr(DotLoc));

    return makeParserResult(new (Context) UnresolvedMemberExpr(
        DotLoc, NameLoc, Name, /*implicit=*/false));
  }
      
  case tok::kw_super: // 'super'
    return parseExprSuper();

  case tok::l_paren:
    // Build a tuple expression syntax node.
    // AST differentiates paren and tuple expression where the former allows
    // only one element without label. However, libSyntax tree doesn't have this
    // differentiation. A tuple expression node in libSyntax can have a single
    // element without label.
    return parseTupleOrParenExpr(tok::l_paren, tok::r_paren);

  case tok::l_square:
    return parseExprCollection();

  case tok::pound_available:
  case tok::pound_unavailable: {
    // For better error recovery, parse but reject availability in an expr
    // context.
    diagnose(Tok.getLoc(), diag::special_condition_outside_if_stmt_guard,
             Tok.getText());
    auto res = parseStmtConditionPoundAvailable();
    if (res.hasCodeCompletion())
      return makeParserCodeCompletionStatus();
    if (res.isParseErrorOrHasCompletion() || res.isNull())
      return nullptr;
    return makeParserResult(new (Context)
                                ErrorExpr(res.get()->getSourceRange()));
  }

  case tok::pound__hasSymbol: {
    // For better error recovery, parse but reject #_hasSymbol in an expr
    // context.
    diagnose(Tok.getLoc(), diag::special_condition_outside_if_stmt_guard,
             Tok.getText());
    auto res = parseStmtConditionPoundHasSymbol();
    if (res.hasCodeCompletion())
      return makeParserCodeCompletionStatus();
    if (res.isParseErrorOrHasCompletion() || res.isNull())
      return nullptr;
    return makeParserResult(new (Context)
                                ErrorExpr(res.get()->getSourceRange()));
  }

#define POUND_OBJECT_LITERAL(Name, Desc, Proto)                                \
  case tok::pound_##Name:                                                      \
    return parseExprObjectLiteral(ObjectLiteralExpr::Name, isExprBasic);
#include "swift/AST/TokenKinds.def"

  case tok::code_complete: {
    auto Result =
        makeParserResult(new (Context) CodeCompletionExpr(Tok.getLoc()));
    Result.setHasCodeCompletionAndIsError();
    if (CodeCompletionCallbacks &&
        // We cannot code complete anything after var/let.
        (!InBindingPattern ||
         InBindingPattern == PatternBindingState::InMatchingPattern)) {
      if (InPoundIfEnvironment) {
        CodeCompletionCallbacks->completePlatformCondition();
      } else {
        CodeCompletionCallbacks->completePostfixExprBeginning(
            cast<CodeCompletionExpr>(Result.get()));
      }
    }
    consumeToken(tok::code_complete);
    if (canParseAsGenericArgumentList()) {
      SmallVector<TypeRepr*, 8> args;
      SourceLoc LAngleLoc, RAngleLoc;
      parseGenericArguments(args, LAngleLoc, RAngleLoc);
    }
    return Result;
  }

  case tok::pound:
    if (!isStartOfFreestandingMacroExpansion()) {
      consumeToken(tok::pound);
      diagnose(Tok.getLoc(),
               diag::macro_expansion_expr_expected_macro_identifier);
      return makeParserError();
    }
    if (peekToken().is(tok::code_complete) &&
        Tok.getLoc().getAdvancedLoc(1) == peekToken().getLoc()) {
      return parseExprPoundCodeCompletion(/*ParentKind*/ llvm::None);
    }

    return parseExprMacroExpansion(isExprBasic);

  // Eat an invalid token in an expression context.  Error tokens are diagnosed
  // by the lexer, so there is no reason to emit another diagnostic.
  case tok::unknown:
    if (startsWithMultilineStringDelimiter(Tok)) {
      // This was due to unterminated multi-line string.
      IsInputIncomplete = true;
    }
    consumeToken(tok::unknown);
    return nullptr;

  default:
  UnknownCharacter:
    checkForInputIncomplete();
    // FIXME: offer a fixit: 'Self' -> 'self'
    diagnose(Tok, ID);
    return nullptr;
  }
}

static StringLiteralExpr *
createStringLiteralExprFromSegment(ASTContext &Ctx,
                                   const Lexer *L,
                                   Lexer::StringSegment &Segment,
                                   SourceLoc TokenLoc) {
  assert(Segment.Kind == Lexer::StringSegment::Literal);
  // FIXME: Consider lazily encoding the string when needed.
  llvm::SmallString<256> Buf;
  StringRef EncodedStr = L->getEncodedStringSegment(Segment, Buf);
  if (!Buf.empty()) {
    assert(EncodedStr.begin() == Buf.begin() &&
           "Returned string is not from buffer?");
    EncodedStr = Ctx.AllocateCopy(EncodedStr);
  }
  return new (Ctx) StringLiteralExpr(EncodedStr, TokenLoc);
}

ParserStatus Parser::
parseStringSegments(SmallVectorImpl<Lexer::StringSegment> &Segments,
                    Token EntireTok,
                    VarDecl *InterpolationVar,
                    /* remaining parameters are outputs: */
                    SmallVectorImpl<ASTNode> &Stmts,
                    unsigned &LiteralCapacity,
                    unsigned &InterpolationCount) {
  SourceLoc Loc = EntireTok.getLoc();
  ParserStatus Status;
  bool First = true;

  DeclNameRef appendLiteral(
      { Context, Context.Id_appendLiteral, { Identifier() } });
  DeclNameRef appendInterpolation(Context.Id_appendInterpolation);

  for (auto Segment : Segments) {
    auto InterpolationVarRef =
      new (Context) DeclRefExpr(InterpolationVar,
                                DeclNameLoc(Segment.Loc), /*implicit=*/true);

    switch (Segment.Kind) {
    case Lexer::StringSegment::Literal: {

      // The end location of the entire string literal.
      SourceLoc EndLoc = EntireTok.getLoc().getAdvancedLoc(EntireTok.getLength());

      auto TokenLoc = First ? Loc : Segment.Loc;
      auto Literal = createStringLiteralExprFromSegment(Context, L, Segment,
                                                        TokenLoc);

      LiteralCapacity += Literal->getValue().size();

      auto AppendLiteralRef =
        new (Context) UnresolvedDotExpr(InterpolationVarRef,
                                        /*dotloc=*/SourceLoc(),
                                        appendLiteral,
                                        /*nameloc=*/DeclNameLoc(), 
                                        /*Implicit=*/true);
      auto *ArgList = ArgumentList::forImplicitUnlabeled(Context, {Literal});
      auto AppendLiteralCall =
          CallExpr::createImplicit(Context, AppendLiteralRef, ArgList);
      Stmts.push_back(AppendLiteralCall);

      // Since the string is already parsed, Tok already points to the first
      // token after the whole string, but PreviousLoc is not exactly correct.
      PreviousLoc = TokenLoc;
      SourceLoc TokEnd = Segment.IsLastSegment ? EndLoc : Segment.getEndLoc();
      unsigned CommentLength = 0;

      // First segment shall inherit the attached comments.
      if (First && EntireTok.hasComment()) {
        CommentLength = SourceMgr.getByteDistance(EntireTok.getCommentRange().
          getStart(), TokenLoc);
      }
      consumeExtraToken(Token(tok::string_literal,
                              CharSourceRange(SourceMgr, TokenLoc, TokEnd).str(),
                              CommentLength));

      // Make an unknown token to encapsulate the entire string segment and add
      // such token to the context.
      Token content(tok::string_segment,
                    CharSourceRange(Segment.Loc, Segment.Length).str());
      break;
    }
        
    case Lexer::StringSegment::Expr: {
      unsigned DelimiterLen = EntireTok.getCustomDelimiterLen();
      bool HasCustomDelimiter = DelimiterLen > 0;

      // Backslash is part of an expression segment.
      SourceLoc BackSlashLoc = Segment.Loc.getAdvancedLoc(-1 - DelimiterLen);
      Token BackSlash(tok::backslash, CharSourceRange(BackSlashLoc, 1).str());

      // Custom delimiter may be a part of an expression segment.
      if (HasCustomDelimiter) {
        SourceLoc DelimiterLoc = Segment.Loc.getAdvancedLoc(-DelimiterLen);
        Token Delimiter(tok::raw_string_delimiter,
                        CharSourceRange(DelimiterLoc, DelimiterLen).str());
      }

      // Create a temporary lexer that lexes from the body of the string.
      LexerState BeginState =
          L->getStateForBeginningOfTokenLoc(Segment.Loc);
      // We need to set the EOF at r_paren, to prevent the Lexer from eagerly
      // trying to lex the token beyond it. Parser::parseList() does a special
      // check for a tok::EOF that is spelled with a ')'.
      // FIXME: This seems like a hack, there must be a better way..
      LexerState EndState = BeginState.advance(Segment.Length-1);
      Lexer LocalLex(*L, BeginState, EndState);

      // Temporarily swap out the parser's current lexer with our new one.
      llvm::SaveAndRestore<Lexer *> T(L, &LocalLex);

      // Prime the new lexer with a '(' as the first token.
      // We might be at tok::eof now, so ensure that consumeToken() does not
      // assert about lexing past eof.
      Tok.setKind(tok::unknown);
      consumeTokenWithoutFeedingReceiver();
      assert(Tok.is(tok::l_paren));
      TokReceiver->registerTokenKindChange(Tok.getLoc(),
                                           tok::string_interpolation_anchor);

      auto callee = new (Context)
          UnresolvedDotExpr(InterpolationVarRef,
                            /*dotloc=*/BackSlashLoc, appendInterpolation,
                            /*nameloc=*/DeclNameLoc(Segment.Loc),
                            /*Implicit=*/true);
      auto S = parseExprCallSuffix(makeParserResult(callee), true);

      // If we stopped parsing the expression before the expression segment is
      // over, eat the remaining tokens into a token list
      if (Segment.getEndLoc() !=
          L->getLocForEndOfToken(SourceMgr, Tok.getLoc())) {
        do {
          consumeToken();
        } while (Segment.getEndLoc() !=
                 L->getLocForEndOfToken(SourceMgr, Tok.getLoc()));
      }

      Expr *call = S.getPtrOrNull();
      if (!call)
        call = new (Context) ErrorExpr(SourceRange(Segment.Loc,
                                                   Segment.getEndLoc()));

      InterpolationCount += 1;
      Stmts.push_back(call);
      Status |= S;

      if (!Tok.is(tok::eof)) {
        diagnose(Tok, diag::string_interpolation_extra);
      } else if (Tok.getText() == ")") {
        Tok.setKind(tok::string_interpolation_anchor);
        consumeToken();
      }
      break;
    }
    }
    First = false;
  }
  return Status;
}

///   expr-literal:
///     string_literal
ParserResult<Expr> Parser::parseExprStringLiteral() {
  SmallVector<Lexer::StringSegment, 1> Segments;
  L->getStringLiteralSegments(Tok, Segments);

  Token EntireTok = Tok;

  // The start location of the entire string literal.
  SourceLoc Loc = Tok.getLoc();

  StringRef OpenDelimiterStr, OpenQuoteStr, CloseQuoteStr, CloseDelimiterStr;
  unsigned DelimiterLength = Tok.getCustomDelimiterLen();
  unsigned QuoteLength;
  tok QuoteKind;
  std::tie(QuoteLength, QuoteKind) =
    Tok.isMultilineString() ? std::make_tuple(3, tok::multiline_string_quote)
                            : std::make_tuple(1, Tok.getText().startswith("\'") ?
                                          tok::single_quote: tok::string_quote);
  unsigned CloseQuoteBegin = Tok.getLength() - DelimiterLength - QuoteLength;

  OpenDelimiterStr = Tok.getRawText().take_front(DelimiterLength);
  OpenQuoteStr = Tok.getRawText().substr(DelimiterLength, QuoteLength);
  CloseQuoteStr = Tok.getRawText().substr(CloseQuoteBegin, QuoteLength);
  CloseDelimiterStr = Tok.getRawText().take_back(DelimiterLength);

  // Make unknown tokens to represent the open and close quote.
  Token OpenQuote(QuoteKind, OpenQuoteStr);
  Token CloseQuote(QuoteKind, CloseQuoteStr);

  // The simple case: just a single literal segment.
  if (Segments.size() == 1 &&
      Segments.front().Kind == Lexer::StringSegment::Literal) {
    consumeExtraToken(Tok);
    consumeTokenWithoutFeedingReceiver();

    return makeParserResult(
        createStringLiteralExprFromSegment(Context, L, Segments.front(), Loc));
  }

  // We don't expose the entire interpolated string as one token. Instead, we
  // should expose the tokens in each segment.
  consumeTokenWithoutFeedingReceiver();
  // We are going to mess with Tok to do reparsing for interpolated literals,
  // don't lose our 'next' token.
  llvm::SaveAndRestore<Token> SavedTok(Tok);
  // For errors, we need the real PreviousLoc, i.e. the start of the
  // whole InterpolatedStringLiteral.
  llvm::SaveAndRestore<SourceLoc> SavedPreviousLoc(PreviousLoc);

  unsigned LiteralCapacity = 0;
  unsigned InterpolationCount = 0;
  TapExpr * AppendingExpr;

  ParserStatus Status;
  {
    SmallVector<ASTNode, 4> Stmts;

    // Make the variable which will contain our temporary value.
    auto InterpolationVar =
      new (Context) VarDecl(/*IsStatic=*/false, VarDecl::Introducer::Var,
                            /*NameLoc=*/SourceLoc(),
                            Context.Id_dollarInterpolation, CurDeclContext);
    InterpolationVar->setImplicit(true);
    InterpolationVar->setUserAccessible(false);
    
    Stmts.push_back(InterpolationVar);

    // Collect all string segments.
    Status = parseStringSegments(Segments, EntireTok, InterpolationVar, 
                                 Stmts, LiteralCapacity, InterpolationCount);

    auto Body = BraceStmt::create(Context, Loc, Stmts, /*endLoc=*/Loc,
                                  /*implicit=*/true);
    AppendingExpr = new (Context) TapExpr(nullptr, Body);
  }

  if (AppendingExpr->getBody()->getNumElements() == 1) {
    Status.setIsParseError();
    return makeParserResult(Status, new (Context) ErrorExpr(Loc));
  }

  return makeParserResult(Status, new (Context) InterpolatedStringLiteralExpr(
                                      Loc, Loc.getAdvancedLoc(CloseQuoteBegin),
                                      LiteralCapacity, InterpolationCount,
                                      AppendingExpr));
}

void Parser::parseOptionalArgumentLabel(Identifier &name, SourceLoc &loc) {
  // Check to see if there is an argument label.
  if (Tok.canBeArgumentLabel() && peekToken().is(tok::colon)) {
    auto text = Tok.getText();

    // If this was an escaped identifier that need not have been escaped, say
    // so. Only _ needs escaping, because we take foo(_: 3) to be equivalent
    // to foo(3), to be more uniform with _ in function declaration as well as
    // the syntax for referring to the function pointer (foo(_:)),
    auto escaped = Tok.isEscapedIdentifier();
    auto underscore = Tok.is(tok::kw__) || (escaped && text == "_");
    if (escaped && !underscore && canBeArgumentLabel(text)) {
      SourceLoc start = Tok.getLoc();
      SourceLoc end = start.getAdvancedLoc(Tok.getLength());
      diagnose(Tok, diag::escaped_parameter_name, text)
          .fixItRemoveChars(start, start.getAdvancedLoc(1))
          .fixItRemoveChars(end.getAdvancedLoc(-1), end);
    }

    loc = consumeArgumentLabel(name, /*diagnoseDollarPrefix=*/false);
    consumeToken(tok::colon);
  }
}

static bool tryParseArgLabelList(Parser &P, Parser::DeclNameOptions flags,
                                 SourceLoc &lparenLoc,
                                 SmallVectorImpl<Identifier> &argumentLabels,
                                 SmallVectorImpl<SourceLoc> &argumentLabelLocs,
                                 SourceLoc &rparenLoc) {
  if (!flags.contains(Parser::DeclNameFlag::AllowCompoundNames))
    return false;

  // Is the current token a left paren?
  if (!P.Tok.isFollowingLParen())
    return false;

  // Okay, let's look ahead and see if the next token is something that could
  // be in an arg label list...
  const Token &next = P.peekToken();

  // A close parenthesis, if empty lists are allowed.
  bool nextIsRParen =
      flags.contains(Parser::DeclNameFlag::AllowZeroArgCompoundNames) &&
      next.is(tok::r_paren);
  // An argument label.
  bool nextIsArgLabel = next.canBeArgumentLabel() || next.is(tok::colon);
  // An editor placeholder.
  bool nextIsPlaceholder = Identifier::isEditorPlaceholder(next.getText());

  if (!(nextIsRParen || nextIsArgLabel || nextIsPlaceholder))
    return false;

  // Try to parse a compound name.
  Parser::CancellableBacktrackingScope backtrack(P);

  lparenLoc = P.consumeToken(tok::l_paren);
  while (P.Tok.isNot(tok::r_paren)) {
    // If we see a ':', the user forgot the '_';
    if (P.Tok.is(tok::colon)) {
      P.diagnose(P.Tok, diag::empty_arg_label_underscore)
          .fixItInsert(P.Tok.getLoc(), "_");
      argumentLabels.push_back(Identifier());
      argumentLabelLocs.push_back(P.consumeToken(tok::colon));
    }

    Identifier argName;
    SourceLoc argLoc;
    P.parseOptionalArgumentLabel(argName, argLoc);
    if (argLoc.isValid()) {
      argumentLabels.push_back(argName);
      argumentLabelLocs.push_back(argLoc);
      continue;
    }

    // This is not a compound name.
    return false;
  }

  // We have a compound name. Cancel backtracking and build that name.
  backtrack.cancelBacktrack();

  rparenLoc = P.consumeToken(tok::r_paren);

  assert(argumentLabels.size() == argumentLabelLocs.size());

  return true;
}

DeclNameRef Parser::parseDeclNameRef(DeclNameLoc &loc,
                                     const Diagnostic &diag,
                                     DeclNameOptions flags) {
  // Consume the base name.
  DeclBaseName baseName;
  SourceLoc baseNameLoc;
  if (Tok.is(tok::identifier) ||
      (flags.contains(DeclNameFlag::AllowLowercaseAndUppercaseSelf) &&
       Tok.isAny(tok::kw_Self, tok::kw_self))) {
    Identifier baseNameId;
    baseNameLoc = consumeIdentifier(baseNameId, /*diagnoseDollarPrefix=*/false);
    baseName = baseNameId;
  } else if (flags.contains(DeclNameFlag::AllowOperators) &&
             Tok.isAnyOperator()) {
    baseName = Context.getIdentifier(Tok.getText());
    baseNameLoc = consumeToken();
  } else if (flags.contains(DeclNameFlag::AllowKeywords) && Tok.isKeyword()) {
    bool specialDeinitAndSubscript =
        flags.contains(DeclNameFlag::AllowKeywordsUsingSpecialNames);

    // Syntax highlighting should treat this token as an identifier and
    // not as a keyword.
    if (Tok.is(tok::kw_init))
      baseName = DeclBaseName::createConstructor();
    else if (specialDeinitAndSubscript && Tok.is(tok::kw_deinit))
      baseName = DeclBaseName::createDestructor();
    else if (specialDeinitAndSubscript && Tok.is(tok::kw_subscript))
      baseName = DeclBaseName::createSubscript();
    else
      baseName = Context.getIdentifier(Tok.getText());
    Tok.setKind(tok::identifier);
    baseNameLoc = consumeToken();
  } else {
    checkForInputIncomplete();
    diagnose(Tok, diag);
    return DeclNameRef();
  }

  // Parse an argument list, if the flags allow it and it's present.
  SmallVector<Identifier, 2> argumentLabels;
  SmallVector<SourceLoc, 2> argumentLabelLocs;
  SourceLoc lparenLoc;
  SourceLoc rparenLoc;

  bool hadArgList = tryParseArgLabelList(*this, flags, lparenLoc,
                                         argumentLabels, argumentLabelLocs,
                                         rparenLoc);

  if (argumentLabelLocs.empty() || !hadArgList)
    loc = DeclNameLoc(baseNameLoc);
  else
    loc = DeclNameLoc(Context, baseNameLoc, lparenLoc, argumentLabelLocs,
                      rparenLoc);

  if (!hadArgList)
    return DeclNameRef(baseName);

  return DeclNameRef({ Context, baseName, argumentLabels });
}

ParserStatus Parser::parseFreestandingMacroExpansion(
    SourceLoc &poundLoc, DeclNameLoc &macroNameLoc, DeclNameRef &macroNameRef,
    SourceLoc &leftAngleLoc, SmallVectorImpl<TypeRepr *> &genericArgs,
    SourceLoc &rightAngleLoc, ArgumentList *&argList, bool isExprBasic,
    const Diagnostic &diag) {
  SourceLoc poundEndLoc = Tok.getRange().getEnd();
  poundLoc = consumeToken(tok::pound);

  if (Tok.isAtStartOfLine()) {
    // Diagnose and bail out if there's no macro name on the same line.
    diagnose(poundEndLoc, diag);
    return makeParserError();
  }

  bool hasWhitespaceBeforeName = poundEndLoc != Tok.getLoc();

  macroNameRef = parseDeclNameRef(macroNameLoc, diag, DeclNameFlag::AllowKeywords);
  if (!macroNameRef)
    return makeParserError();

  // Diagnose whitespace between '#' and the macro name, and continue parsing.
  if (hasWhitespaceBeforeName) {
    diagnose(poundEndLoc, diag::extra_whitespace_macro_expansion_identifier)
        .fixItRemoveChars(poundEndLoc, macroNameLoc.getStartLoc());
  }

  ParserStatus status;
  if (canParseAsGenericArgumentList()) {
    auto genericArgsStatus =
        parseGenericArguments(genericArgs, leftAngleLoc, rightAngleLoc);
    status |= genericArgsStatus;
    if (genericArgsStatus.isErrorOrHasCompletion())
      diagnose(leftAngleLoc, diag::while_parsing_as_left_angle_bracket);
  }

  llvm::SaveAndRestore<bool> inMacroArgument(InFreestandingMacroArgument, true);

  if (Tok.isFollowingLParen()) {
    auto result = parseArgumentList(tok::l_paren, tok::r_paren, isExprBasic,
                                    /*allowTrailingClosure*/ true);
    status |= result;
    argList = result.getPtrOrNull();
  } else if (Tok.is(tok::l_brace)) {
    SmallVector<Argument, 2> trailingClosures;
    auto closuresStatus = parseTrailingClosures(
        isExprBasic, macroNameLoc.getSourceRange(), trailingClosures);
    status |= closuresStatus;

    if (!trailingClosures.empty()) {
      argList = ArgumentList::createParsed(Context, SourceLoc(),
                                           trailingClosures, SourceLoc(),
                                           /*trailingClosureIdx*/ 0);
    }
  }

  return status;
}

///   expr-identifier:
///     unqualified-decl-name generic-args?
ParserResult<Expr> Parser::parseExprIdentifier() {
  ParserStatus status;
  assert(Tok.isAny(tok::identifier, tok::kw_self, tok::kw_Self));
  Token IdentTok = Tok;

  // Parse the unqualified-decl-name.
  DeclNameLoc loc;
  DeclNameRef name =
      parseDeclNameRef(loc, diag::expected_expr,
                       DeclNameFlag::AllowCompoundNames |
                           DeclNameFlag::AllowLowercaseAndUppercaseSelf);

  SmallVector<TypeRepr*, 8> args;
  SourceLoc LAngleLoc, RAngleLoc;
  bool hasGenericArgumentList = false;
  
  ///   The generic-args case is ambiguous with an expression involving '<'
  ///   and '>' operators. The operator expression is favored unless a generic
  ///   argument list can be successfully parsed, and the closing bracket is
  ///   followed by one of these tokens:
  ///     lparen_following rparen lsquare_following rsquare lbrace rbrace
  ///     period_following comma semicolon
  ///
  if (canParseAsGenericArgumentList()) {
    auto argStatus = parseGenericArguments(args, LAngleLoc, RAngleLoc);
    status |= argStatus;
    if (argStatus.isErrorOrHasCompletion())
      diagnose(LAngleLoc, diag::while_parsing_as_left_angle_bracket);
    
    hasGenericArgumentList = true;
  }
  
  if (name.getBaseName().isEditorPlaceholder()) {
    return makeParserResult(
        status, parseExprEditorPlaceholder(IdentTok, name.getBaseIdentifier()));
  }

  auto refKind = DeclRefKind::Ordinary;
  Expr *E = new (Context) UnresolvedDeclRefExpr(name, refKind, loc);
  
  if (hasGenericArgumentList) {
    E = UnresolvedSpecializeExpr::create(Context, E, LAngleLoc, args,
                                         RAngleLoc);
  }
  return makeParserResult(status, E);
}

Expr *Parser::parseExprEditorPlaceholder(Token PlaceholderTok,
                                         Identifier PlaceholderId) {
  assert(PlaceholderTok.is(tok::identifier));
  assert(PlaceholderId.isEditorPlaceholder());

  auto parseTypeForPlaceholder = [&]() -> std::pair<TypeRepr *, TypeRepr *> {
    llvm::Optional<EditorPlaceholderData> DataOpt =
        swift::parseEditorPlaceholder(PlaceholderTok.getText());
    if (!DataOpt)
      return {nullptr, nullptr};
    StringRef TypeStr = DataOpt->Type;
    if (TypeStr.empty())
      return {nullptr, nullptr};

    // Ensure that we restore the parser state at exit.
    ParserPositionRAII PPR(*this);

    auto parseTypeString = [&](StringRef TyStr) -> TypeRepr* {
      unsigned Offset = TyStr.data() - PlaceholderTok.getText().data();
      SourceLoc TypeStartLoc = PlaceholderTok.getLoc().getAdvancedLoc(Offset);
      SourceLoc TypeEndLoc = TypeStartLoc.getAdvancedLoc(TyStr.size());

      LexerState StartState = L->getStateForBeginningOfTokenLoc(TypeStartLoc);
      LexerState EndState = L->getStateForBeginningOfTokenLoc(TypeEndLoc);

      // Create a lexer for the type sub-string.
      Lexer LocalLex(*L, StartState, EndState);

      // Temporarily swap out the parser's current lexer with our new one.
      llvm::SaveAndRestore<Lexer *> T(L, &LocalLex);

      // Don't feed to syntax token recorder.
      ConsumeTokenReceiver DisabledRec;
      llvm::SaveAndRestore<ConsumeTokenReceiver *> R(TokReceiver, &DisabledRec);

      Tok.setKind(tok::unknown); // we might be at tok::eof now.
      consumeTokenWithoutFeedingReceiver();
      return parseType().getPtrOrNull();
    };

    TypeRepr *PlaceholderTyR = parseTypeString(TypeStr);
    TypeRepr *ExpansionTyR = nullptr;
    if (DataOpt->TypeForExpansion == TypeStr) {
      ExpansionTyR = PlaceholderTyR;
    } else {
      ExpansionTyR = parseTypeString(DataOpt->TypeForExpansion);
    }
    return {PlaceholderTyR, ExpansionTyR};
  };

  TypeRepr *PlaceholderTyR = nullptr;
  TypeRepr *ExpansionTyR = nullptr;
  std::tie(PlaceholderTyR, ExpansionTyR) = parseTypeForPlaceholder();
  return new (Context) EditorPlaceholderExpr(
      PlaceholderId, PlaceholderTok.getLoc(), PlaceholderTyR, ExpansionTyR);
}

// Extract names of the tuple elements and preserve the structure
// of the tuple (with any nested tuples inside) to be able to use
// it in the fix-it without any type information provided by user.
static void printTupleNames(const TypeRepr *typeRepr, llvm::raw_ostream &OS) {
  if (!typeRepr)
    return;
  
  auto tupleRepr = dyn_cast<TupleTypeRepr>(typeRepr);
  if (!tupleRepr)
    return;
  
  OS << "(";
  unsigned elementIndex = 0;
  llvm::SmallVector<TypeRepr *, 10> elementTypes;
  tupleRepr->getElementTypes(elementTypes);
  interleave(elementTypes,
             [&](const TypeRepr *element) {
               if (isa<TupleTypeRepr>(element)) {
                 printTupleNames(element, OS);
               } else {
                 auto name = tupleRepr->getElementName(elementIndex);
                 // If there is no label from the element
                 // it means that it's malformed and we can
                 // use the type instead.
                 if (name.empty())
                   element->print(OS);
                 else
                   OS << name;
               }
               
               ++elementIndex;
             },
             [&] { OS << ", "; });
  OS << ")";
}

ParserStatus Parser::parseClosureSignatureIfPresent(
    DeclAttributes &attributes,
    SourceRange &bracketRange,
    SmallVectorImpl<CaptureListEntry> &captureList,
    VarDecl *&capturedSelfDecl,
    ParameterList *&params,
    SourceLoc &asyncLoc, SourceLoc &throwsLoc,
    SourceLoc &arrowLoc,
    TypeExpr *&explicitResultType, SourceLoc &inLoc) {
  // Clear out result parameters.
  bracketRange = SourceRange();
  attributes = DeclAttributes();
  capturedSelfDecl = nullptr;
  params = nullptr;
  throwsLoc = SourceLoc();
  arrowLoc = SourceLoc();
  explicitResultType = nullptr;
  inLoc = SourceLoc();

  // Consume 'async', 'throws', and 'rethrows', but in any order.
  auto consumeEffectsSpecifiers = [&] {
    while (isEffectsSpecifier(Tok) ||
           (Tok.is(tok::code_complete) && !Tok.isAtStartOfLine()))
      consumeToken();
  };

  // If we have a leading token that may be part of the closure signature, do a
  // speculative parse to validate it and look for 'in'.
  if (Tok.isAny(
          tok::at_sign, tok::l_paren, tok::l_square, tok::identifier,
          tok::kw__, tok::code_complete)) {
    BacktrackingScope backtrack(*this);

    // Consume attributes.
    while (Tok.is(tok::at_sign)) {
      skipAnyAttribute();
    }

    // Skip by a closure capture list if present.
    if (consumeIf(tok::l_square)) {
      skipUntil(tok::r_square);
      if (!consumeIf(tok::r_square))
        return makeParserSuccess();
    }

    // Parse pattern-tuple func-signature-result? 'in'.
    if (consumeIf(tok::l_paren)) {      // Consume the ')'.

      // While we don't have '->' or ')', eat balanced tokens.
      while (!Tok.is(tok::r_paren) && !Tok.is(tok::eof))
        skipSingle();

      // Consume the ')', if it's there.
      if (consumeIf(tok::r_paren)) {
        consumeEffectsSpecifiers();

        // Parse the func-signature-result, if present.
        if (consumeIf(tok::arrow)) {
          if (!canParseType())
            return makeParserSuccess();

          consumeEffectsSpecifiers();
        }
      }

      // Okay, we have a closure signature.
    } else if (Tok.isIdentifierOrUnderscore() || Tok.is(tok::code_complete)) {
      // Parse identifier (',' identifier)*
      consumeToken();
      while (consumeIf(tok::comma)) {
        if (Tok.isIdentifierOrUnderscore() || Tok.is(tok::code_complete)) {
          consumeToken();
          continue;
        }

        return makeParserSuccess();
      }

      consumeEffectsSpecifiers();

      // Parse the func-signature-result, if present.
      if (consumeIf(tok::arrow)) {
        if (!canParseType())
          return makeParserSuccess();

        consumeEffectsSpecifiers();
      }
    }
    
    // Parse the 'in' at the end.
    if (Tok.isNot(tok::kw_in))
      return makeParserSuccess();

    // Okay, we have a closure signature.
  } else {
    // No closure signature.
    return makeParserSuccess();
  }
  ParserStatus status;
  (void)parseDeclAttributeList(attributes);

  if (Tok.is(tok::l_square) && peekToken().is(tok::r_square)) {
    SourceLoc lBracketLoc = consumeToken(tok::l_square);
    SourceLoc rBracketLoc = consumeToken(tok::r_square);
    bracketRange = SourceRange(lBracketLoc, rBracketLoc);
  } else if (Tok.is(tok::l_square) && !peekToken().is(tok::r_square)) {
    SourceLoc lBracketLoc = consumeToken(tok::l_square);
    // At this point, we know we have a closure signature. Parse the capture list
    // and parameters.
    bool HasNext;
    do {
      SWIFT_DEFER { HasNext = consumeIf(tok::comma); };
      // Check for the strength specifier: "weak", "unowned", or
      // "unowned(safe/unsafe)".
      SourceLoc ownershipLocStart, ownershipLocEnd;
      auto ownershipKind = ReferenceOwnership::Strong;
      if (Tok.isContextualKeyword("weak")){
        ownershipLocStart = ownershipLocEnd = consumeToken(tok::identifier);
        ownershipKind = ReferenceOwnership::Weak;
      } else if (Tok.isContextualKeyword("unowned")) {
        ownershipLocStart = ownershipLocEnd = consumeToken(tok::identifier);
        ownershipKind = ReferenceOwnership::Unowned;

        // Skip over "safe" and "unsafe" if present.
        if (consumeIf(tok::l_paren)) {
          if (Tok.getText() == "safe")
            ownershipKind =
                ReferenceOwnership::Unowned; // FIXME: No "safe" variant.
          else if (Tok.getText() == "unsafe")
            ownershipKind = ReferenceOwnership::Unmanaged;
          else
            diagnose(Tok, diag::attr_unowned_invalid_specifier);
          consumeIf(tok::identifier, ownershipLocEnd);
          if (!consumeIf(tok::r_paren, ownershipLocEnd))
            diagnose(Tok, diag::attr_unowned_expected_rparen);
        }
      } else if (Tok.isAny(tok::identifier, tok::kw_self, tok::code_complete) &&
                 peekToken().isAny(tok::equal, tok::comma, tok::r_square,
                                   tok::period)) {
        // "x = 42", "x," and "x]" are all strong captures of x.
      } else {
        diagnose(Tok, diag::expected_capture_specifier);
        skipUntil(tok::comma, tok::r_square);
        continue;
      }

      if (Tok.isNot(tok::identifier, tok::kw_self, tok::code_complete)) {
        diagnose(Tok, diag::expected_capture_specifier_name);
        skipUntil(tok::comma, tok::r_square);
        continue;
      }

      // The thing being capture specified is an identifier, or as an identifier
      // followed by an expression.
      Expr *initializer;
      Identifier name;
      SourceLoc nameLoc = Tok.getLoc();
      SourceLoc equalLoc;
      if (peekToken().isNot(tok::equal)) {
        // If this is the simple case, then the identifier is both the name and
        // the expression to capture.
        if (!Tok.is(tok::code_complete)) {
          name = Context.getIdentifier(Tok.getText());
          auto initializerResult = parseExprIdentifier();
          status |= initializerResult;
          initializer = initializerResult.get();
        } else {
          auto CCE = new (Context) CodeCompletionExpr(Tok.getLoc());
          if (CodeCompletionCallbacks) {
            CodeCompletionCallbacks->completePostfixExprBeginning(CCE);
          }
          name = Identifier();
          initializer = CCE;
          consumeToken();
          status.setHasCodeCompletion();
        }

        // It is a common error to try to capture a nested field instead of just
        // a local name, reject it with a specific error message.
        if (Tok.isAny(tok::period, tok::exclaim_postfix,tok::question_postfix)){
          auto diag = diagnose(Tok, diag::cannot_capture_fields);
          while (peekToken().isNot(tok::comma, tok::r_square, tok::eof,
                                   tok::kw_in, tok::r_brace,
                                   tok::pound_endif, tok::pound_else,
                                   tok::pound_elseif))
            consumeToken();
          if (Tok.isKeyword() || Tok.isContextualDeclKeyword()) {
            StringRef name = Tok.getText();
            diag.fixItInsert(nameLoc, ("`" + name + "` = ").str());
          } else if (Tok.is(tok::identifier)) {
            StringRef name = Tok.getRawText();
            diag.fixItInsert(nameLoc, (name + " = ").str());
          }
          skipSingle(); // Advance to the comma or r_square
          continue;
        }

      } else {
        // Otherwise, the name is a new declaration.
        if (!Tok.is(tok::code_complete)) {
          consumeIdentifier(name, /*diagnoseDollarPrefix=*/true);
        } else {
          // Ignore completion token because it's a new declaration.
          name = Identifier();
          consumeToken(tok::code_complete);
        }
        equalLoc = consumeToken(tok::equal);

        auto ExprResult = parseExpr(diag::expected_init_capture_specifier);
        if (ExprResult.isNull())
          continue;
        initializer = ExprResult.get();
      }

      // Create the VarDecl and the PatternBindingDecl for the captured
      // expression.  This uses the parent declcontext (not the closure) since
      // the initializer expression is evaluated before the closure is formed.
      auto introducer = (ownershipKind != ReferenceOwnership::Weak
                         ? VarDecl::Introducer::Let
                         : VarDecl::Introducer::Var);
      auto *VD = new (Context) VarDecl(/*isStatic*/false, introducer,
                                       nameLoc, name, CurDeclContext);
        
      // If we captured something under the name "self", remember that.
      if (name == Context.Id_self)
        capturedSelfDecl = VD;

      // Attributes.
      if (ownershipKind != ReferenceOwnership::Strong)
        VD->getAttrs().add(new (Context) ReferenceOwnershipAttr(
          SourceRange(ownershipLocStart, ownershipLocEnd), ownershipKind));

      auto pattern = NamedPattern::createImplicit(Context, VD);

      auto *PBD = PatternBindingDecl::create(
          Context, /*StaticLoc*/ SourceLoc(), StaticSpellingKind::None,
          /*VarLoc*/ nameLoc, pattern, /*EqualLoc*/ equalLoc, initializer,
          CurDeclContext);

      auto CLE = CaptureListEntry(PBD);
      if (CLE.isSimpleSelfCapture())
        VD->setIsSelfParamCapture();

      captureList.push_back(CLE);
    } while (HasNext);

    // The capture list needs to be closed off with a ']'.
    SourceLoc rBracketLoc = Tok.getLoc();
    if (!consumeIf(tok::r_square)) {
      diagnose(Tok, diag::expected_capture_list_end_rsquare);
      skipUntil(tok::r_square);
      if (Tok.is(tok::r_square))
        rBracketLoc = consumeToken(tok::r_square);
    }
    bracketRange = SourceRange(lBracketLoc, rBracketLoc);
  }
  
  bool invalid = false;
  if (Tok.isNot(tok::kw_in)) {
    if (Tok.is(tok::l_paren)) {
      // Parse the closure arguments.
      auto pattern = parseSingleParameterClause(ParameterContextKind::Closure);
      if (pattern.isNonNull())
        params = pattern.get();
      else
        status.setIsParseError();
    } else {
      // Parse identifier (',' identifier)*
      SmallVector<ParamDecl*, 4> elements;
      bool HasNext;
      do {
        if (Tok.isNot(tok::identifier, tok::kw__, tok::code_complete)) {
          diagnose(Tok, diag::expected_closure_parameter_name);
          status.setIsParseError();
          break;
        }

        Identifier name;
        SourceLoc nameLoc;
        if (Tok.is(tok::identifier)) {
          nameLoc = consumeIdentifier(name, /*diagnoseDollarPrefix=*/false);
        } else {
          assert(Tok.isAny(tok::kw__ , tok::code_complete));
          // Consume and ignore code_completion token so that completion don't
          // suggest anything for the parameter name declaration.
          nameLoc = consumeToken();
        }
        auto var = new (Context)
            ParamDecl(SourceLoc(), SourceLoc(),
                      Identifier(), nameLoc, name, nullptr);
        var->setSpecifier(ParamSpecifier::Default);
        elements.push_back(var);

        // Consume a comma to continue.
        HasNext = consumeIf(tok::comma);
      } while (HasNext);

      params = ParameterList::create(Context, elements);
    }

    status |= parseEffectsSpecifiers(SourceLoc(),
                                     asyncLoc, /*reasync*/nullptr,
                                     throwsLoc, /*rethrows*/nullptr);

    // Parse the optional explicit return type.
    if (Tok.is(tok::arrow)) {
      // Consume the '->'.
      arrowLoc = consumeToken();

      // Parse the type.
      auto *explicitResultTypeRepr =
          parseType(diag::expected_closure_result_type).getPtrOrNull();
      if (!explicitResultTypeRepr) {
        // If we couldn't parse the result type, clear out the arrow location.
        arrowLoc = SourceLoc();
        status.setIsParseError();
      } else {
        explicitResultType = new (Context) TypeExpr(explicitResultTypeRepr);

        // Check for 'throws' and 'rethrows' after the type and correct it.
        parseEffectsSpecifiers(arrowLoc,
                               asyncLoc, /*reasync*/nullptr,
                               throwsLoc, /*rethrows*/nullptr);
      }
    }
  }

  // Parse the 'in'.
  if (Tok.is(tok::kw_in)) {
    inLoc = consumeToken();
  } else {
    // Scan forward to see if we can find the 'in'. This re-synchronizes the
    // parser so we can at least parse the body correctly.
    SourceLoc startLoc = Tok.getLoc();
    ParserPosition pos = getParserPosition();
    while (Tok.isNot(tok::eof) && !Tok.is(tok::kw_in) &&
           Tok.isNot(tok::r_brace)) {
      skipSingle();
    }

    if (Tok.is(tok::kw_in)) {
      // We found the 'in'. If this is the first error, complain about the
      // junk tokens in-between but re-sync at the 'in'.
      if (!invalid) {
        diagnose(startLoc, diag::unexpected_tokens_before_closure_in);
      }
      inLoc = consumeToken();
    } else {
      // We didn't find an 'in', backtrack to where we started. If this is the
      // first error, complain about the missing 'in'.
      backtrackToPosition(pos);
      if (!invalid) {
        diagnose(Tok, diag::expected_closure_in)
          .fixItInsert(Tok.getLoc(), "in ");
      }
      inLoc = Tok.getLoc();
    }
  }

  if (!params)
    return status;

  // If this was a closure declaration (maybe even trailing)
  // tuple parameter destructuring is one of the common
  // problems, and is misleading to users, so it's imperative
  // to detect any tuple splat or destructuring as early as
  // possible and give a proper fix-it. See SE-0110 for more details.
  auto isTupleDestructuring = [](ParamDecl *param) -> bool {
    auto *typeRepr = param->getTypeRepr();
    if (!(typeRepr && param->isDestructured()))
      return false;
    return !param->hasName() && isa<TupleTypeRepr>(typeRepr);
  };

  for (unsigned i = 0, e = params->size(); i != e; ++i) {
    auto *param = params->get(i);
    if (!isTupleDestructuring(param)) {
      param->setDestructured(false);
      continue;
    }

    auto argName = "arg" + std::to_string(i);

    SmallString<64> fixIt;
    llvm::raw_svector_ostream OS(fixIt);
    auto isMultiLine = Tok.isAtStartOfLine();
    StringRef indent = Lexer::getIndentationForLine(SourceMgr, Tok.getLoc());
    if (isMultiLine)
      OS << '\n' << indent;

    OS << "let ";
    printTupleNames(param->getTypeRepr(), OS);
    OS << " = " << argName << (isMultiLine ? "\n" + indent : "; ");

    diagnose(param->getStartLoc(), diag::anon_closure_tuple_param_destructuring)
        .fixItReplace(param->getSourceRange(), argName)
        .fixItInsert(Tok.getLoc(), OS.str());

    status.setIsParseError();
  }

  return status;
}

ParserResult<Expr> Parser::parseExprClosure() {
  assert(Tok.is(tok::l_brace) && "Not at a left brace?");
  ParserStatus Status;
  // We may be parsing this closure expr in a matching pattern context.  If so,
  // reset our state to not be in a pattern for any recursive pattern parses.
  llvm::SaveAndRestore<decltype(InBindingPattern)> T(
      InBindingPattern, PatternBindingState::NotInBinding);

  // Reset async attribute in parser context.
  llvm::SaveAndRestore<bool> AsyncAttr(InPatternWithAsyncAttribute, false);

  // Parse the opening left brace.
  SourceLoc leftBrace = consumeToken();

  // Parse the closure-signature, if present.
  DeclAttributes attributes;
  SourceRange bracketRange;
  SmallVector<CaptureListEntry, 2> captureList;
  VarDecl *capturedSelfDecl;
  ParameterList *params = nullptr;
  SourceLoc asyncLoc;
  SourceLoc throwsLoc;
  SourceLoc arrowLoc;
  TypeExpr *explicitResultType;
  SourceLoc inLoc;
  Status |= parseClosureSignatureIfPresent(
      attributes, bracketRange, captureList, capturedSelfDecl, params, asyncLoc,
      throwsLoc, arrowLoc, explicitResultType, inLoc);

  // Create the closure expression and enter its context.
  auto *closure = new (Context) ClosureExpr(
      attributes, bracketRange, capturedSelfDecl, params, asyncLoc, throwsLoc,
      arrowLoc, inLoc, explicitResultType, CurDeclContext);
  ParseFunctionBody cc(*this, closure);

  // Handle parameters.
  if (!params) {
    // There are no parameters; allow anonymous closure variables.
    // FIXME: We could do this all the time, and then provide Fix-Its
    // to map $i -> the appropriately-named argument. This might help
    // users who are refactoring code by adding names.
    AnonClosureVars.push_back({{}, leftBrace});
  }

  // Parse the body.
  SmallVector<ASTNode, 4> bodyElements;
  Status |= parseBraceItems(bodyElements, BraceItemListKind::Brace);

  if (SourceMgr.rangeContainsIDEInspectionTarget(
          Lexer::getCharSourceRangeFromSourceRange(SourceMgr,
                                                   {leftBrace, PreviousLoc}))) {
    // Ignore 'IDEInspectionDelayedDeclState' inside closures.
    // Completions inside functions body inside closures at top level should
    // be considered top-level completions.
    if (State->hasIDEInspectionDelayedDeclState()) {
      (void)State->takeIDEInspectionDelayedDeclState();
    }
    if (L->isCodeCompletion()) {
      Status.setHasCodeCompletionAndIsError();
    }
  }

  // Parse the closing '}'.
  SourceLoc rightBrace;
  bool missingRBrace = parseMatchingToken(tok::r_brace, rightBrace,
                                          diag::expected_closure_rbrace,
                                          leftBrace);
  if (missingRBrace) {
    Status.setIsParseError();
  } else {
    // We recovered so don't propagate any error status (but still preserve
    // HasCodeCompletion).
    Status.clearIsError();
  }

  // If we didn't have any parameters, create a parameter list from the
  // anonymous closure arguments.
  if (!params) {
    // Create a parameter pattern containing the anonymous variables.
    auto &anonVars = AnonClosureVars.back().Item;
    SmallVector<ParamDecl*, 4> elements;
    for (auto anonVar : anonVars)
      elements.push_back(anonVar);
    
    params = ParameterList::create(Context, leftBrace, elements, leftBrace);

    // Pop out of the anonymous closure variables scope.
    AnonClosureVars.pop_back();

    // Attach the parameters to the closure.
    closure->setParameterList(params);
    closure->setHasAnonymousClosureVars();
  }

  auto *BS = BraceStmt::create(Context, leftBrace, bodyElements, rightBrace);

  // If the body consists of a single expression, turn it into a return
  // statement.
  bool hasSingleExpressionBody = false;
  if (!missingRBrace) {
    if (auto Element = BS->getSingleActiveElement()) {
      if (Element.is<Stmt *>()) {
        if (auto returnStmt = dyn_cast<ReturnStmt>(Element.get<Stmt *>())) {
          hasSingleExpressionBody = true;
          if (!returnStmt->hasResult()) {
            auto returnExpr = TupleExpr::createEmpty(Context,
                                                     SourceLoc(),
                                                     SourceLoc(),
                                                     /*implicit*/true);
            returnStmt->setResult(returnExpr);
          }
        }
      } else if (Element.is<Expr *>()) {
        // Create the wrapping return.
        hasSingleExpressionBody = true;
        auto returnExpr = Element.get<Expr*>();
        BS->setLastElement(new (Context) ReturnStmt(SourceLoc(), returnExpr));
      }
    }
  }

  // Set the body of the closure.
  closure->setBody(BS, hasSingleExpressionBody);

  // If the closure includes a capture list, create an AST node for it as well.
  Expr *result = closure;
  if (!captureList.empty())
    result = CaptureListExpr::create(Context, captureList, closure);

  return makeParserResult(Status, result);
}

///   expr-anon-closure-argument:
///     dollarident
Expr *Parser::parseExprAnonClosureArg() {
  StringRef Name = Tok.getText();
  SourceLoc Loc = consumeToken(tok::dollarident);
  assert(Name[0] == '$' && "Not a dollarident");

  // We know from the lexer that this is all-numeric.

  unsigned ArgNo = 0;
  if (Name.substr(1).getAsInteger(10, ArgNo)) {
    diagnose(Loc.getAdvancedLoc(1), diag::dollar_numeric_too_large);
    return new (Context) ErrorExpr(Loc);
  }

  // If this is a closure expression that did not have any named parameters,
  // generate the anonymous variables we need.
  auto closure = dyn_cast_or_null<ClosureExpr>(
      dyn_cast<AbstractClosureExpr>(CurDeclContext));
  if (!closure) {
    if (Context.LangOpts.DebuggerSupport) {
      auto refKind = DeclRefKind::Ordinary;
      auto identifier = Context.getIdentifier(Name);
      return new (Context) UnresolvedDeclRefExpr(DeclNameRef(identifier),
                                                 refKind, DeclNameLoc(Loc));
    }
    diagnose(Loc, diag::anon_closure_arg_not_in_closure);
    return new (Context) ErrorExpr(Loc);
  }
  // When the closure already has explicit parameters, offer their names as
  // replacements.
  if (auto *params = closure->getParameters()) {
    if (ArgNo < params->size() && params->get(ArgNo)->hasName()) {
      auto paramName = params->get(ArgNo)->getNameStr();
      diagnose(Loc, diag::anon_closure_arg_in_closure_with_args_typo, paramName)
        .fixItReplace(Loc, paramName);
      return new (Context) DeclRefExpr(params->get(ArgNo), DeclNameLoc(Loc),
                                       /*Implicit=*/false);
    } else {
      diagnose(Loc, diag::anon_closure_arg_in_closure_with_args);
      return new (Context) ErrorExpr(Loc);
    }
  }

  auto leftBraceLoc = AnonClosureVars.back().Loc;
  auto &decls = AnonClosureVars.back().Item;
  while (ArgNo >= decls.size()) {
    unsigned nextIdx = decls.size();
    SmallVector<char, 4> StrBuf;
    StringRef varName = ("$" + Twine(nextIdx)).toStringRef(StrBuf);
    Identifier ident = Context.getIdentifier(varName);
    SourceLoc varLoc = leftBraceLoc;
    auto *var = new (Context)
        ParamDecl(SourceLoc(), SourceLoc(),
                  Identifier(), varLoc, ident, closure);
    var->setSpecifier(ParamSpecifier::Default);
    var->setImplicit();
    decls.push_back(var);
  }

  return new (Context) DeclRefExpr(decls[ArgNo], DeclNameLoc(Loc),
                                   /*Implicit=*/false);
}

/// parseTupleOrParenExpr - Parse a tuple or paren expression.
///
///   expr-paren:
///     lparen-any ')'
///     lparen-any expr-paren-element (',' expr-paren-element)* ')'
///
///   expr-paren-element:
///     (identifier ':')? expr
///
ParserResult<Expr> Parser::parseTupleOrParenExpr(tok leftTok, tok rightTok) {
  SmallVector<ExprListElt, 8> elts;
  SourceLoc leftLoc, rightLoc;
  auto status =
      parseExprList(leftTok, rightTok, /*isArgumentList*/ false, leftLoc, elts,
                    rightLoc);

  // A tuple with a single, unlabeled element is just parentheses.
  if (elts.size() == 1 && !isa<PackExpansionExpr>(elts[0].E) &&
      elts[0].Label.empty()) {
    return makeParserResult(
        status, new (Context) ParenExpr(leftLoc, elts[0].E, rightLoc));
  }

  SmallVector<Expr *, 8> exprs;
  SmallVector<Identifier, 8> labels;
  SmallVector<SourceLoc, 8> labelLocs;
  for (auto &elt : elts) {
    exprs.push_back(elt.E);
    labels.push_back(elt.Label);
    labelLocs.push_back(elt.LabelLoc);
  }
  return makeParserResult(status, TupleExpr::create(Context, leftLoc, exprs,
                                                    labels, labelLocs, rightLoc,
                                                    /*implicit*/ false));
}

/// parseArgumentList - Parse an argument list.
///
///   arg-list:
///     lparen-any ')'
///     lparen-any argument (',' argument)* ')'
///
///   argument:
///     (identifier ':')? '&'? expr
///
ParserResult<ArgumentList>
Parser::parseArgumentList(tok leftTok, tok rightTok, bool isExprBasic,
                          bool allowTrailingClosure) {
  SourceLoc leftLoc, rightLoc;
  SmallVector<ExprListElt, 8> elts;

  // FIXME: Introduce new SyntaxKind for ArgumentList (rdar://81786229)
  auto status =
      parseExprList(leftTok, rightTok, /*isArgumentList*/ true, leftLoc, elts,
                    rightLoc);

  SmallVector<Argument, 8> args;
  for (auto &elt : elts)
    args.emplace_back(elt.LabelLoc, elt.Label, elt.E);

  auto numNonTrailing = args.size();
  llvm::Optional<unsigned> trailingClosureIndex;

  // If we can parse trailing closures, do so.
  if (allowTrailingClosure && Tok.is(tok::l_brace) &&
      isValidTrailingClosure(isExprBasic, *this)) {
    status |= parseTrailingClosures(isExprBasic, SourceRange(leftLoc, rightLoc),
                                    args);
    if (args.size() > numNonTrailing)
      trailingClosureIndex = numNonTrailing;
  }
  auto *argList = ArgumentList::createParsed(Context, leftLoc, args, rightLoc,
                                             trailingClosureIndex);
  return makeParserResult(status, argList);
}

/// parseExprList - Parse a list of expressions.
///
///   expr-list:
///     lparen-any ')'
///     lparen-any expr-list-element (',' expr-list-element)* ')'
///
///   expr-list-element:
///     (identifier ':')? '&'? expr
///
ParserStatus Parser::parseExprList(tok leftTok, tok rightTok,
                                   bool isArgumentList, SourceLoc &leftLoc,
                                   SmallVectorImpl<ExprListElt> &elts,
                                   SourceLoc &rightLoc) {
  StructureMarkerRAII ParsingExprList(*this, Tok);
  
  leftLoc = consumeToken(leftTok);
  return parseList(rightTok, leftLoc, rightLoc, /*AllowSepAfterLast=*/false,
                   rightTok == tok::r_paren ? diag::expected_rparen_expr_list
                                            : diag::expected_rsquare_expr_list,
                   [&] () -> ParserStatus {
    Identifier FieldName;
    SourceLoc FieldNameLoc;
    parseOptionalArgumentLabel(FieldName, FieldNameLoc);

    // See if we have an operator decl ref '(<op>)'. The operator token in
    // this case lexes as a binary operator because it neither leads nor
    // follows a proper subexpression.
    auto isUnappliedOperator = [&]() {
      return Tok.isBinaryOperator() && peekToken().isAny(rightTok, tok::comma);
    };

    if (isUnappliedOperator()) {
      // Check to see if we have the start of a regex literal `/.../`. We need
      // to do this for an unapplied operator reference, as e.g `(/, /)` might
      // be a regex literal.
      tryLexRegexLiteral(/*forUnappliedOperator*/ true);
    }

    ParserStatus Status;
    Expr *SubExpr = nullptr;
    if (isUnappliedOperator()) {
      DeclNameLoc Loc;
      auto OperName =
          parseDeclNameRef(Loc, diag::expected_operator_ref,
                           DeclNameFlag::AllowOperators |
                               DeclNameFlag::AllowLowercaseAndUppercaseSelf);
      if (!OperName) {
        return makeParserError();
      }
      // Bypass local lookup. Use an 'Ordinary' reference kind so that the
      // reference may resolve to any unary or binary operator based on
      // context.
      SubExpr = new(Context) UnresolvedDeclRefExpr(OperName,
                                                   DeclRefKind::Ordinary, Loc);
    } else if (isArgumentList && Tok.is(tok::code_complete)) {
      // Handle call arguments specially because it may need argument labels.
      auto CCExpr = new (Context) CodeCompletionExpr(Tok.getLoc());
      if (this->CodeCompletionCallbacks)
        this->CodeCompletionCallbacks->completeCallArg(CCExpr,
                                                       PreviousLoc == leftLoc);
      consumeIf(tok::code_complete);
      SubExpr = CCExpr;
      Status.setHasCodeCompletionAndIsError();
    } else {
      auto ParsedSubExpr = parseExpr(diag::expected_expr_in_expr_list);
      SubExpr = ParsedSubExpr.getPtrOrNull();
      Status = ParsedSubExpr;
    }

    // If we got a subexpression, add it.
    if (SubExpr)
      elts.push_back({FieldNameLoc, FieldName, SubExpr});

    return Status;
  });
}

static bool isStartOfLabelledTrailingClosure(Parser &P) {
  // Fast path: the next two tokens must be a label and a colon.
  // But 'default:' is ambiguous with switch cases and we disallow it
  // (unless escaped) even outside of switches.
  if (!P.Tok.canBeArgumentLabel() ||
      P.Tok.is(tok::kw_default) ||
      !P.peekToken().is(tok::colon))
    return false;

  // Do some tentative parsing to distinguish `label: { ... }` and
  // `label: switch x { ... }`.
  Parser::BacktrackingScope backtrack(P);
  P.consumeToken();
  if (P.peekToken().is(tok::l_brace))
    return true;
  // Parse editor placeholder as trailing closure so SourceKit can expand it to
  // closure literal.
  if (P.peekToken().is(tok::identifier) &&
      Identifier::isEditorPlaceholder(P.peekToken().getText()))
    return true;
  // Consider `label: <complete>` that the user is trying to write a closure.
  if (P.peekToken().is(tok::code_complete) && !P.peekToken().isAtStartOfLine())
    return true;
  return false;
}

ParserStatus
Parser::parseTrailingClosures(bool isExprBasic, SourceRange calleeRange,
                              SmallVectorImpl<Argument> &closures) {
  SourceLoc braceLoc = Tok.getLoc();

  // Record the line numbers for the diagnostics below.
  // Note that *do not* move this to after 'parseExprClosure()' it slows down
  // 'getLineNumber()' call because of cache in SourceMgr.
  auto origLine = SourceMgr.getLineAndColumnInBuffer(calleeRange.End).first;
  auto braceLine = SourceMgr.getLineAndColumnInBuffer(braceLoc).first;

  ParserStatus result;

  // Parse the closure.
  ParserResult<Expr> closure = parseExprClosure();
  if (closure.isNull())
    return makeParserError();

  result |= closure;

  closures.push_back(Argument::unlabeled(closure.get()));

  // Warn if the trailing closure is separated from its callee by more than
  // one line. A single-line separation is acceptable for a trailing closure
  // call, and will be diagnosed later only if the call fails to typecheck.
  if (braceLine > origLine + 1) {
    diagnose(braceLoc, diag::trailing_closure_after_newlines);
    diagnose(calleeRange.Start, diag::trailing_closure_callee_here);

    auto *CE = dyn_cast<ClosureExpr>(closures[0].getExpr());
    if (CE && CE->hasAnonymousClosureVars() &&
        CE->getParameters()->size() == 0) {
      diagnose(braceLoc, diag::brace_stmt_suggest_do)
        .fixItInsert(braceLoc, "do ");
    }
  }

  // Parse labeled trailing closures.
  while (true) {
    if (!isStartOfLabelledTrailingClosure(*this)) {
      if (!Tok.is(tok::code_complete))
        break;

      // FIXME: Additional trailing closure completion on newline positions.
      //   let foo = SomeThing {
      //     ...
      //   }
      //   <HERE>
      // This was previously enabled, but it failed to suggest 'foo' because
      // the token was considered a part of the initializer.
      if (Tok.isAtStartOfLine())
        break;

      // If the current completion mode doesn't support trailing closure
      // completion, leave the token here and let "postfix completion" to
      // handle it.
      if (CodeCompletionCallbacks &&
          !CodeCompletionCallbacks->canPerformCompleteLabeledTrailingClosure())
        break;

      // foo() {} <token>
      auto CCExpr = new (Context) CodeCompletionExpr(Tok.getLoc());
      if (CodeCompletionCallbacks) {
        CodeCompletionCallbacks->completeLabeledTrailingClosure(
            CCExpr, Tok.isAtStartOfLine());
      }
      consumeToken(tok::code_complete);
      result.setHasCodeCompletionAndIsError();
      closures.emplace_back(SourceLoc(), Identifier(), CCExpr);
      continue;
    }

    Identifier label;
    auto labelLoc = consumeArgumentLabel(label, /*diagnoseDollarPrefix=*/false);
    consumeToken(tok::colon);
    ParserResult<Expr> closure;
    if (Tok.is(tok::l_brace)) {
      closure = parseExprClosure();
    } else if (Tok.is(tok::identifier)) {
      // Parse editor placeholder as a closure literal.
      assert(Identifier::isEditorPlaceholder(Tok.getText()));
      Identifier name = Context.getIdentifier(Tok.getText());
      closure = makeParserResult(parseExprEditorPlaceholder(Tok, name));
      consumeToken(tok::identifier);
    } else if (Tok.is(tok::code_complete)) {
      assert(!Tok.isAtStartOfLine() &&
             "isStartOfLabelledTrailingClosure() should return false");
      // Swallow code completion token after the label.
      // FIXME: Closure literal completion.
      consumeToken(tok::code_complete);
      return makeParserCodeCompletionStatus();
    }
    if (closure.isNull())
      return makeParserError();

    result |= closure;
    closures.emplace_back(labelLoc, label, closure.get());

    // Don't diagnose whitespace gaps before labelled closures.
  }
  return result;
}

/// Parse an object literal expression.
///
/// expr-literal:
///   '#' identifier expr-paren
ParserResult<Expr>
Parser::parseExprObjectLiteral(ObjectLiteralExpr::LiteralKind LitKind,
                               bool isExprBasic) {
  SourceLoc PoundLoc = consumeToken();
  // Parse a tuple of args
  if (!Tok.is(tok::l_paren)) {
    diagnose(Tok, diag::expected_arg_list_in_object_literal);
    return makeParserError();
  }

  // Parse the argument list.
  auto argList = parseArgumentList(tok::l_paren, tok::r_paren, isExprBasic);
  if (argList.hasCodeCompletion())
    return makeParserCodeCompletionResult<Expr>();
  if (argList.isParseErrorOrHasCompletion())
    return makeParserError();

  return makeParserResult(ObjectLiteralExpr::create(Context, PoundLoc, LitKind,
                                                    argList.get(),
                                                    /*implicit*/ false));
}

/// Handle code completion after pound in expression position.
///
/// In case it's in a stmt condition position, specify \p ParentKind to
/// decide the position accepts #available(...) condition.
///
/// expr-pound-codecompletion:
///   '#' code-completion-token
ParserResult<Expr>
Parser::parseExprPoundCodeCompletion(llvm::Optional<StmtKind> ParentKind) {
  assert(Tok.is(tok::pound) && peekToken().is(tok::code_complete) &&
         Tok.getLoc().getAdvancedLoc(1) == peekToken().getLoc());
  consumeToken(); // '#' token.
  auto CodeCompletionPos = consumeToken();
  auto Expr = new (Context) CodeCompletionExpr(CodeCompletionPos);
  if (CodeCompletionCallbacks) {
    CodeCompletionCallbacks->completeAfterPoundExpr(Expr, ParentKind);
  }
  return makeParserCodeCompletionResult(Expr);
}

/// Parse an expression call suffix.
///
/// expr-call-suffix:
///   expr-paren
///   expr-closure (except in expr-basic)
ParserResult<Expr>
Parser::parseExprCallSuffix(ParserResult<Expr> fn, bool isExprBasic) {
  assert(Tok.isFollowingLParen() && "Not a call suffix?");

  // If there is a code completion token right after the '(', do a special case
  // callback.
  if (peekToken().is(tok::code_complete) && CodeCompletionCallbacks) {
    auto lParenLoc = consumeToken(tok::l_paren);
    auto CCE = new (Context) CodeCompletionExpr(Tok.getLoc());
    auto rParenLoc = Tok.getLoc();
    auto *argList = ArgumentList::createParsed(
        Context, lParenLoc, {Argument::unlabeled(CCE)}, rParenLoc,
        /*trailingClosureIdx*/ llvm::None);
    auto Result = makeParserResult(
        fn, CallExpr::create(Context, fn.get(), argList, /*implicit*/ false));
    CodeCompletionCallbacks->completePostfixExprParen(fn.get(), CCE);
    // Eat the code completion token because we handled it.
    consumeToken(tok::code_complete);
    Result.setHasCodeCompletionAndIsError();
    return Result;
  }

  // Parse the argument list.
  auto argList = parseArgumentList(tok::l_paren, tok::r_paren, isExprBasic);

  // Form the call.
  return makeParserResult(ParserStatus(argList) | fn,
                          CallExpr::create(Context, fn.get(), argList.get(),
                                           /*implicit=*/false));
}

ParserResult<Expr> Parser::parseExprMacroExpansion(bool isExprBasic) {
  SourceLoc poundLoc;
  DeclNameLoc macroNameLoc;
  DeclNameRef macroNameRef;
  SourceLoc leftAngleLoc, rightAngleLoc;
  SmallVector<TypeRepr *, 4> genericArgs;
  ArgumentList *argList = nullptr;
  ParserStatus status = parseFreestandingMacroExpansion(
      poundLoc, macroNameLoc, macroNameRef, leftAngleLoc, genericArgs,
      rightAngleLoc, argList, isExprBasic,
      diag::macro_expansion_expr_expected_macro_identifier);
  if (!macroNameRef)
    return status;

  return makeParserResult(
      status,
      MacroExpansionExpr::create(
          CurDeclContext, poundLoc, macroNameRef, macroNameLoc, leftAngleLoc,
          Context.AllocateCopy(genericArgs), rightAngleLoc, argList,
          CurDeclContext->isTypeContext() ? MacroRole::Declaration
                                          : getFreestandingMacroRoles()));
}

/// parseExprCollection - Parse a collection literal expression.
///
///   expr-collection:
///     expr-array
///     expr-dictionary
///   expr-array:
///     '[' expr (',' expr)* ','? ']'
///     '[' ']'
///   expr-dictionary:
///     '[' expr ':' expr (',' expr ':' expr)* ','? ']'
///     '[' ':' ']'
ParserResult<Expr> Parser::parseExprCollection() {
  SourceLoc LSquareLoc = consumeToken(tok::l_square);
  SourceLoc RSquareLoc;

  Parser::StructureMarkerRAII ParsingCollection(
                                *this, LSquareLoc,
                                StructureMarkerKind::OpenSquare);

  // [] is always an array.
  if (Tok.is(tok::r_square)) {
    RSquareLoc = consumeToken(tok::r_square);
    return makeParserResult(
                    ArrayExpr::create(Context, LSquareLoc, {}, {}, RSquareLoc));
  }

  // [:] is always an empty dictionary.
  if (Tok.is(tok::colon) && peekToken().is(tok::r_square)) {
    consumeToken(tok::colon);
    RSquareLoc = consumeToken(tok::r_square);
    return makeParserResult(
               DictionaryExpr::create(Context, LSquareLoc, {}, {}, RSquareLoc));
  }

  ParserStatus Status;
  llvm::Optional<bool> isDictionary;
  SmallVector<Expr *, 8> ElementExprs;
  SmallVector<SourceLoc, 8> CommaLocs;

  {
    while (true) {
      auto Element = parseExprCollectionElement(isDictionary);
      Status |= Element;
      if (Element.isNonNull())
        ElementExprs.push_back(Element.get());

      // Skip to ']' or ',' in case of error.
      // NOTE: This checks 'Status' instead of 'Element' to silence excessive
      // diagnostics.
      if (Status.isErrorOrHasCompletion()) {
        skipUntilDeclRBrace(tok::r_square, tok::comma);
        if (Tok.isNot(tok::comma))
          break;
      }

      // Parse the ',' if exists.
      if (Tok.is(tok::comma)) {
        CommaLocs.push_back(consumeToken());
        if (!Tok.is(tok::r_square))
          continue;
      }

      // Close square.
      if (Tok.is(tok::r_square))
        break;

      // If we found EOF or such, bailout.
      if (Tok.is(tok::eof)) {
        IsInputIncomplete = true;
        break;
      }

      // If The next token is at the beginning of a new line and can never start
      // an element, break.
      if (Tok.isAtStartOfLine() && (Tok.isAny(tok::r_brace, tok::pound_endif) ||
                                    isStartOfSwiftDecl() || isStartOfStmt()))
        break;

      diagnose(Tok, diag::expected_separator, ",")
          .fixItInsertAfter(PreviousLoc, ",");
      Status.setIsParseError();
    }
  }

  if (Status.isErrorOrHasCompletion()) {
    // If we've already got errors, don't emit missing RightK diagnostics.
    RSquareLoc = Tok.is(tok::r_square) ? consumeToken()
                                       : getLocForMissingMatchingToken();
  } else if (parseMatchingToken(tok::r_square, RSquareLoc,
                                diag::expected_rsquare_array_expr,
                                LSquareLoc)) {
    Status.setIsParseError();
  }

  // Don't bother to create expression if any expressions aren't parsed.
  if (ElementExprs.empty())
    return Status;

  Expr *expr;
  if (*isDictionary)
    expr = DictionaryExpr::create(Context, LSquareLoc, ElementExprs, CommaLocs,
                                  RSquareLoc);
  else
    expr = ArrayExpr::create(Context, LSquareLoc, ElementExprs, CommaLocs,
                             RSquareLoc);

  return makeParserResult(Status, expr);
}

/// parseExprCollectionElement - Parse an element for collection expr.
///
/// If \p isDictionary is \c llvm::None, it's set to \c true if the element is
/// for dictionary literal, or \c false otherwise.
ParserResult<Expr>
Parser::parseExprCollectionElement(llvm::Optional<bool> &isDictionary) {
  auto Element = parseExpr(isDictionary.has_value() && *isDictionary
                               ? diag::expected_key_in_dictionary_literal
                               : diag::expected_expr_in_collection_literal);

  if (!isDictionary.has_value())
    isDictionary = Tok.is(tok::colon);

  if (!*isDictionary) {
    validateCollectionElement(Element);
    return Element;
  }

  if (Element.isNull())
    return Element;

  // Parse the ':'.
  ParserResult<Expr> Value;
  if (consumeIf(tok::colon)) {
    // Parse the value.
    Value = parseExpr(diag::expected_value_in_dictionary_literal);
    if (Value.isNull()) {
      if (!Element.hasCodeCompletion()) {
        Value = makeParserResult(Value, new (Context) ErrorExpr(PreviousLoc));
      } else {
        Value = makeParserResult(Value,
                                 new (Context) CodeCompletionExpr(PreviousLoc));
      }
    }
  } else {
    diagnose(Tok, diag::expected_colon_in_dictionary_literal);
    Value = makeParserResult(makeParserError(),
                             new (Context) ErrorExpr(SourceRange()));
  }

  // Make a tuple of Key Value pair.
  return makeParserResult(
      ParserStatus(Element) | ParserStatus(Value),
      TupleExpr::createImplicit(Context, {Element.get(), Value.get()}, {}));
}

/// validateCollectionElement - Check if a given collection element is valid.
///
/// At the moment, this checks whether a given collection element is a subscript
/// expression and whether we're subscripting into an array. If we are, then it
/// we emit a diagnostic in case it was not something that the user was
/// expecting.
///
/// For example: `let array [ [0, 1] [42] ]`
void Parser::validateCollectionElement(ParserResult<Expr> element) {
  if (element.isNull())
    return;

  auto elementExpr = element.get();
  if (!isa<SubscriptExpr>(elementExpr))
    return;

  auto subscriptExpr = cast<SubscriptExpr>(elementExpr);
  if (!isa<ArrayExpr>(subscriptExpr->getBase()))
    return;

  auto arrayExpr = cast<ArrayExpr>(subscriptExpr->getBase());

  auto startLocOfSubscript = subscriptExpr->getArgs()->getStartLoc();
  auto endLocOfArray = arrayExpr->getEndLoc();
  auto locForEndOfTokenArray = L->getLocForEndOfToken(SourceMgr, endLocOfArray);

  if (locForEndOfTokenArray != startLocOfSubscript) {
    auto subscriptLoc = subscriptExpr->getLoc();
    diagnose(subscriptLoc, diag::subscript_array_element)
        .highlight(subscriptExpr->getSourceRange());
    diagnose(subscriptLoc, diag::subscript_array_element_fix_it_add_comma)
        .fixItInsertAfter(endLocOfArray, ",");
    diagnose(subscriptLoc, diag::subscript_array_element_fix_it_remove_space)
        .fixItRemoveChars(locForEndOfTokenArray, startLocOfSubscript);
  }
}



/// Parse availability query specification.
///
///  availability-spec:
///     '*'
///     language-version-constraint-spec
///     package-description-constraint-spec
///     platform-version-constraint-spec
ParserResult<AvailabilitySpec> Parser::parseAvailabilitySpec() {
  if (Tok.isBinaryOperator() && Tok.getText() == "*") {
    SourceLoc StarLoc = Tok.getLoc();
    consumeToken();

    return makeParserResult(new (Context) OtherPlatformAvailabilitySpec(StarLoc));
  }
  if (Tok.isIdentifierOrUnderscore() &&
       (Tok.getText() == "swift" || Tok.getText() == "_PackageDescription"))
      return parsePlatformAgnosticVersionConstraintSpec();

  return parsePlatformVersionConstraintSpec();
}

/// Parse platform-agnostic version constraint specification.
///
///  language-version-constraint-spec:
///     "swift" version-tuple
///  package-description-version-constraint-spec:
///     "_PackageDescription" version-tuple
ParserResult<PlatformAgnosticVersionConstraintAvailabilitySpec>
Parser::parsePlatformAgnosticVersionConstraintSpec() {
  SourceLoc PlatformAgnosticNameLoc;
  llvm::VersionTuple Version;
  llvm::Optional<AvailabilitySpecKind> Kind;
  SourceRange VersionRange;

  if (Tok.isIdentifierOrUnderscore()) {
    if (Tok.getText() == "swift")
      Kind = AvailabilitySpecKind::LanguageVersionConstraint;
    else if (Tok.getText() == "_PackageDescription")
      Kind = AvailabilitySpecKind::PackageDescriptionVersionConstraint;
  }

  if (!Kind.has_value())
    return nullptr;

  PlatformAgnosticNameLoc = Tok.getLoc();
  consumeToken();
  if (parseVersionTuple(Version, VersionRange,
                        diag::avail_query_expected_version_number)) {
    return nullptr;
  }
  return makeParserResult(new (Context)
                          PlatformAgnosticVersionConstraintAvailabilitySpec(
                            Kind.value(), PlatformAgnosticNameLoc, Version, VersionRange));
}

/// Parse platform-version constraint specification.
///
///  platform-version-constraint-spec:
///     identifier version-comparison version-tuple
ParserResult<PlatformVersionConstraintAvailabilitySpec>
Parser::parsePlatformVersionConstraintSpec() {
  Identifier PlatformIdentifier;
  SourceLoc PlatformLoc;
  if (Tok.is(tok::code_complete)) {
    consumeToken();
    if (CodeCompletionCallbacks) {
      CodeCompletionCallbacks->completePoundAvailablePlatform();
    }
    return makeParserCodeCompletionStatus();
  }

  if (parseIdentifier(PlatformIdentifier, PlatformLoc,
                      /*diagnoseDollarPrefix=*/false,
                      diag::avail_query_expected_platform_name)) {
    return nullptr;
  }

  if (Tok.isBinaryOperator() && Tok.getText() == ">=") {
    diagnose(Tok, diag::avail_query_version_comparison_not_needed)
        .fixItRemove(Tok.getLoc());
    consumeToken();
  }

  llvm::VersionTuple Version;
  SourceRange VersionRange;

  if (parseVersionTuple(Version, VersionRange,
                        diag::avail_query_expected_version_number)) {
    return nullptr;
  }

  llvm::Optional<PlatformKind> Platform =
      platformFromString(PlatformIdentifier.str());

  if (!Platform.has_value() || Platform.value() == PlatformKind::none) {
    if (auto CorrectedPlatform =
            closestCorrectedPlatformString(PlatformIdentifier.str())) {
      diagnose(PlatformLoc, diag::avail_query_suggest_platform_name,
               PlatformIdentifier, *CorrectedPlatform)
          .fixItReplace(PlatformLoc, *CorrectedPlatform);
    } else {
      diagnose(PlatformLoc, diag::avail_query_unrecognized_platform_name,
               PlatformIdentifier);
    }
    Platform = PlatformKind::none;
  }

  // Register the platform name as a keyword token.
  TokReceiver->registerTokenKindChange(PlatformLoc, tok::contextual_keyword);

  // Keep the original version around for run-time checks to support
  // macOS Big Sur betas that report 10.16 at
  // run time.
  llvm::VersionTuple RuntimeVersion = Version;
  Version = canonicalizePlatformVersion(*Platform, Version);
  return makeParserResult(new (Context) PlatformVersionConstraintAvailabilitySpec(
      Platform.value(), PlatformLoc, Version, RuntimeVersion, VersionRange));
}
