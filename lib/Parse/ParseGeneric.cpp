//===--- ParseGeneric.cpp - Swift Language Parser for Generics ------------===//
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
// Generic Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"
#include "swift/AST/DiagnosticsParse.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "swift/Parse/SyntaxParsingContext.h"
#include "swift/Parse/Lexer.h"
#include "swift/Syntax/SyntaxBuilders.h"
#include "swift/Syntax/SyntaxNodes.h"
using namespace swift;
using namespace swift::syntax;

/// parseGenericParameters - Parse a sequence of generic parameters, e.g.,
/// < T : Comparable, U : Container> along with an optional requires clause.
///
///   generic-params:
///     '<' generic-param (',' generic-param)* where-clause? '>'
///
///   generic-param:
///     identifier
///     identifier ':' type-identifier
///     identifier ':' type-composition
///
/// When parsing the generic parameters, this routine establishes a new scope
/// and adds those parameters to the scope.
ParserResult<GenericParamList> Parser::parseGenericParameters() {
  SyntaxParsingContext GPSContext(SyntaxContext, SyntaxKind::GenericParameterClause);
  // Parse the opening '<'.
  assert(startsWithLess(Tok) && "Generic parameter list must start with '<'");
  return parseGenericParameters(consumeStartingLess());
}

ParserStatus
Parser::parseGenericParametersBeforeWhere(SourceLoc LAngleLoc,
                        SmallVectorImpl<GenericTypeParamDecl *> &GenericParams) {
  ParserStatus Result;
  SyntaxParsingContext GPSContext(SyntaxContext, SyntaxKind::GenericParameterList);
  bool HasNextParam;
  do {
    SyntaxParsingContext GParamContext(SyntaxContext, SyntaxKind::GenericParameter);
    // Note that we're parsing a declaration.
    StructureMarkerRAII ParsingDecl(*this, Tok.getLoc(),
                                    StructureMarkerKind::Declaration);
    
    if (ParsingDecl.isFailed()) {
      return makeParserError();
    }

    // Parse attributes.
    DeclAttributes attributes;
    if (Tok.hasComment())
      attributes.add(new (Context) RawDocCommentAttr(Tok.getCommentRange()));
    parseDeclAttributeList(attributes);

    // Parse the name of the parameter.
    Identifier Name;
    SourceLoc NameLoc;
    if (parseIdentifier(Name, NameLoc, /*diagnoseDollarPrefix=*/true,
                        diag::expected_generics_parameter_name)) {
      Result.setIsParseError();
      break;
    }

    // Parse the ':' followed by a type.
    SmallVector<InheritedEntry, 1> Inherited;
    if (Tok.is(tok::colon)) {
      (void)consumeToken();
      ParserResult<TypeRepr> Ty;

      if (Tok.isAny(tok::identifier, tok::code_complete, tok::kw_protocol,
                    tok::kw_Any)) {
        Ty = parseType();
      } else if (Tok.is(tok::kw_class)) {
        diagnose(Tok, diag::unexpected_class_constraint);
        diagnose(Tok, diag::suggest_anyobject)
        .fixItReplace(Tok.getLoc(), "AnyObject");
        consumeToken();
        Result.setIsParseError();
      } else {
        diagnose(Tok, diag::expected_generics_type_restriction, Name);
        Result.setIsParseError();
      }

      if (Ty.hasCodeCompletion())
        return makeParserCodeCompletionStatus();

      if (Ty.isNonNull())
        Inherited.push_back({Ty.get()});
    }

    // We always create generic type parameters with an invalid depth.
    // Semantic analysis fills in the depth when it processes the generic
    // parameter list.
    auto Param = new (Context) GenericTypeParamDecl(CurDeclContext, Name, NameLoc,
                                            GenericTypeParamDecl::InvalidDepth,
                                                    GenericParams.size());
    if (!Inherited.empty())
      Param->setInherited(Context.AllocateCopy(Inherited));
    GenericParams.push_back(Param);

    // Attach attributes.
    Param->getAttrs() = attributes;

    // Parse the comma, if the list continues.
    HasNextParam = consumeIf(tok::comma);
  } while (HasNextParam);

  return Result;
}

ParserResult<GenericParamList>
Parser::parseGenericParameters(SourceLoc LAngleLoc) {
  // Parse the generic parameter list.
  SmallVector<GenericTypeParamDecl *, 4> GenericParams;
  auto Result = parseGenericParametersBeforeWhere(LAngleLoc, GenericParams);

  // Return early if there was code completion token.
  if (Result.hasCodeCompletion())
    return Result;
  auto Invalid = Result.isErrorOrHasCompletion();

  // Parse the optional where-clause.
  SourceLoc WhereLoc;
  SourceLoc EndLoc;
  SmallVector<RequirementRepr, 4> Requirements;
  if (Tok.is(tok::kw_where) &&
      parseGenericWhereClause(WhereLoc, EndLoc, Requirements)
        .isErrorOrHasCompletion()) {
    Invalid = true;
  }
  
  // Parse the closing '>'.
  SourceLoc RAngleLoc;
  if (startsWithGreater(Tok)) {
    RAngleLoc = consumeStartingGreater();
  } else {
    if (!Invalid) {
      diagnose(Tok, diag::expected_rangle_generics_param);
      diagnose(LAngleLoc, diag::opening_angle);
      Invalid = true;
    }

    // Skip until we hit the '>'.
    RAngleLoc = skipUntilGreaterInTypeList();
  }

  if (GenericParams.empty())
    return nullptr;

  return makeParserResult(GenericParamList::create(Context, LAngleLoc,
                                                   GenericParams, WhereLoc,
                                                   Requirements, RAngleLoc));
}

ParserResult<GenericParamList> Parser::maybeParseGenericParams() {
  if (!startsWithLess(Tok))
    return nullptr;

  if (!isInSILMode())
    return parseGenericParameters();

  // In SIL mode, we can have multiple generic parameter lists, with the
  // first one being the outmost generic parameter list.
  GenericParamList *gpl = nullptr, *outer_gpl = nullptr;
  do {
    gpl = parseGenericParameters().getPtrOrNull();
    if (!gpl)
      return nullptr;

    if (outer_gpl)
      gpl->setOuterParameters(outer_gpl);
    outer_gpl = gpl;
  } while (startsWithLess(Tok));
  return makeParserResult(gpl);
}

void
Parser::diagnoseWhereClauseInGenericParamList(const GenericParamList *
                                              GenericParams) {
  if (GenericParams == nullptr || GenericParams->getWhereLoc().isInvalid())
    return;



  auto WhereRangeInsideBrackets = GenericParams->getWhereClauseSourceRange();

  // Move everything immediately following the last generic parameter
  // as written all the way to the right angle bracket (">")
  auto LastGenericParam = GenericParams->getParams().back();
  auto EndOfLastGenericParam =
      Lexer::getLocForEndOfToken(SourceMgr, LastGenericParam->getEndLoc());

  CharSourceRange RemoveWhereRange { SourceMgr,
    EndOfLastGenericParam,
    GenericParams->getRAngleLoc()
  };

  auto WhereCharRange =
    Lexer::getCharSourceRangeFromSourceRange(SourceMgr,
                                  GenericParams->getWhereClauseSourceRange());

  SmallString<64> Buffer;
  llvm::raw_svector_ostream WhereClauseText(Buffer);
  WhereClauseText << SourceMgr.extractText(Tok.is(tok::kw_where)
                                           ? WhereCharRange
                                           : RemoveWhereRange);

  // If, for some reason, there was a where clause in both locations, we're
  // adding to the list of requirements, so tack on a comma here before
  // inserting it at the head of the later where clause.
  if (Tok.is(tok::kw_where))
    WhereClauseText << ',';

  auto Diag = diagnose(WhereRangeInsideBrackets.Start,
                       diag::where_inside_brackets);

  Diag.fixItRemoveChars(RemoveWhereRange.getStart(),
                        RemoveWhereRange.getEnd());

  if (Tok.is(tok::kw_where)) {
    Diag.fixItReplace(Tok.getLoc(), WhereClauseText.str());
  } else {
    Diag.fixItInsert(Lexer::getLocForEndOfToken(SourceMgr, PreviousLoc),
                     WhereClauseText.str());
  }
}

/// parseGenericWhereClause - Parse a 'where' clause, which places additional
/// constraints on generic parameters or types based on them.
///
///   where-clause:
///     'where' requirement (',' requirement) *
///
///   requirement:
///     conformance-requirement
///     same-type-requirement
///
///   conformance-requirement:
///     type-identifier ':' type-identifier
///     type-identifier ':' type-composition
///
///   same-type-requirement:
///     type-identifier '==' type
ParserStatus Parser::parseGenericWhereClause(
               SourceLoc &WhereLoc, SourceLoc &EndLoc,
               SmallVectorImpl<RequirementRepr> &Requirements,
               bool AllowLayoutConstraints) {
  SyntaxParsingContext ClauseContext(SyntaxContext,
                                     SyntaxKind::GenericWhereClause);
  ParserStatus Status;
  // Parse the 'where'.
  WhereLoc = consumeToken(tok::kw_where);
  SyntaxParsingContext ReqListContext(SyntaxContext,
                                      SyntaxKind::GenericRequirementList);
  bool HasNextReq;
  do {
    SyntaxParsingContext ReqContext(SyntaxContext,
                                    SyntaxKind::GenericRequirement);
    Optional<SyntaxParsingContext> BodyContext;
    BodyContext.emplace(SyntaxContext);

    if (Tok.is(tok::code_complete)) {
      if (CodeCompletion)
        CodeCompletion->completeGenericRequirement();
      EndLoc = consumeToken(tok::code_complete);
      Status.setHasCodeCompletionAndIsError();
      break;
    }

    // Parse the leading type. It doesn't necessarily have to be just a type
    // identifier if we're dealing with a same-type constraint.
    ParserResult<TypeRepr> FirstType = parseType();

    if (FirstType.hasCodeCompletion()) {
      BodyContext->setTransparent();
      Status.setHasCodeCompletionAndIsError();
    }

    if (FirstType.isNull()) {
      BodyContext->setTransparent();
      Status.setIsParseError();
      break;
    }

    if (Tok.is(tok::colon)) {
      // A conformance-requirement.
      SourceLoc ColonLoc = consumeToken();
      BodyContext->setCreateSyntax(SyntaxKind::ConformanceRequirement);
      if (Tok.is(tok::identifier) &&
          getLayoutConstraint(Context.getIdentifier(Tok.getText()), Context)
              ->isKnownLayout()) {
        // Parse a layout constraint.
        Identifier LayoutName;
        auto LayoutLoc = consumeIdentifier(LayoutName,
                                           /*diagnoseDollarPrefix=*/false);
        auto LayoutInfo = parseLayoutConstraint(LayoutName);
        if (!LayoutInfo->isKnownLayout()) {
          // There was a bug in the layout constraint.
          Status.setIsParseError();
        }
        auto Layout = LayoutInfo;
        // Types in SIL mode may contain layout constraints.
        if (!AllowLayoutConstraints && !isInSILMode()) {
          diagnose(LayoutLoc,
                   diag::layout_constraints_only_inside_specialize_attr);
        } else {
          // Add the layout requirement.
          Requirements.push_back(RequirementRepr::getLayoutConstraint(
              FirstType.get(), ColonLoc,
              LayoutConstraintLoc(Layout, LayoutLoc)));
        }
      } else {
        // Parse the protocol or composition.
        ParserResult<TypeRepr> Protocol = parseType();
        Status |= Protocol;
        if (Protocol.isNull())
          Protocol = makeParserResult(new (Context) ErrorTypeRepr(PreviousLoc));

        // Add the requirement.
        Requirements.push_back(RequirementRepr::getTypeConstraint(
            FirstType.get(), ColonLoc, Protocol.get()));
      }
    } else if ((Tok.isAnyOperator() && Tok.getText() == "==") ||
               Tok.is(tok::equal)) {
      BodyContext->setCreateSyntax(SyntaxKind::SameTypeRequirement);
      // A same-type-requirement
      if (Tok.is(tok::equal)) {
        diagnose(Tok, diag::requires_single_equal)
          .fixItReplace(SourceRange(Tok.getLoc()), "==");
      }
      SourceLoc EqualLoc = consumeToken();

      // Parse the second type.
      ParserResult<TypeRepr> SecondType = parseType();
      Status |= SecondType;
      if (SecondType.isNull())
        SecondType = makeParserResult(new (Context) ErrorTypeRepr(PreviousLoc));

      // Add the requirement
      Requirements.push_back(RequirementRepr::getSameType(FirstType.get(),
                                                      EqualLoc,
                                                      SecondType.get()));
    } else if (FirstType.hasCodeCompletion()) {
      // Recover by adding dummy constraint.
      Requirements.push_back(RequirementRepr::getTypeConstraint(
          FirstType.get(), PreviousLoc, new (Context) ErrorTypeRepr(PreviousLoc)));
    } else {
      BodyContext->setTransparent();
      diagnose(Tok, diag::expected_requirement_delim);
      Status.setIsParseError();
      break;
    }
    BodyContext.reset();
    HasNextReq = consumeIf(tok::comma);
    // If there's a comma, keep parsing the list.
    // If there's a "&&", diagnose replace with a comma and keep parsing
    if (Tok.isBinaryOperator() && Tok.getText() == "&&" && !HasNextReq) {
      diagnose(Tok, diag::requires_comma)
        .fixItReplace(SourceRange(Tok.getLoc()), ",");
      consumeToken();
      HasNextReq = true;
    }
  } while (HasNextReq);

  if (!Requirements.empty())
    EndLoc = Requirements.back().getSourceRange().End;
  else if (EndLoc.isInvalid())
    EndLoc = WhereLoc;

  return Status;
}


/// Parse a free-standing where clause attached to a declaration.
ParserStatus Parser::
parseFreestandingGenericWhereClause(GenericContext *genCtx) {
  assert(Tok.is(tok::kw_where) && "Shouldn't call this without a where");

  SmallVector<RequirementRepr, 4> Requirements;
  SourceLoc WhereLoc, EndLoc;
  auto result = parseGenericWhereClause(WhereLoc, EndLoc, Requirements);

  genCtx->setTrailingWhereClause(
      TrailingWhereClause::create(Context, WhereLoc, EndLoc, Requirements));

  return result;
}

/// Parse a where clause after a protocol or associated type declaration.
ParserStatus Parser::parseProtocolOrAssociatedTypeWhereClause(
    TrailingWhereClause *&trailingWhere, bool isProtocol) {
  assert(Tok.is(tok::kw_where) && "Shouldn't call this without a where");
  SourceLoc whereLoc, endLoc;
  SmallVector<RequirementRepr, 4> requirements;
  auto whereStatus =
      parseGenericWhereClause(whereLoc, endLoc, requirements);
  if (whereStatus.isSuccess() && !whereStatus.hasCodeCompletion()) {
    trailingWhere =
        TrailingWhereClause::create(Context, whereLoc, endLoc, requirements);
  } else if (whereStatus.hasCodeCompletion()) {
    return whereStatus;
  }

  return ParserStatus();
}
