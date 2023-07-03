//===--- ParsePattern.cpp - Swift Language Parser for Patterns ------------===//
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
// Pattern Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"

#include "swift/AST/ASTWalker.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/Module.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/StringExtras.h"
#include "swift/Parse/IDEInspectionCallbacks.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;

/// Determine the kind of a default argument given a parsed
/// expression that has not yet been type-checked.
static DefaultArgumentKind getDefaultArgKind(Expr *init) {
  if (!init)
    return DefaultArgumentKind::None;

  // Parse an as-written 'nil' expression as the special NilLiteral kind,
  // which is emitted by the caller and can participate in rethrows
  // checking.
  if (isa<NilLiteralExpr>(init))
    return DefaultArgumentKind::NilLiteral;

  auto magic = dyn_cast<MagicIdentifierLiteralExpr>(init);
  if (!magic)
    return DefaultArgumentKind::Normal;

  switch (magic->getKind()) {
#define MAGIC_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
  case MagicIdentifierLiteralExpr::NAME: return DefaultArgumentKind::NAME;
#include "swift/AST/MagicIdentifierKinds.def"
  }

  llvm_unreachable("Unhandled MagicIdentifierLiteralExpr in switch.");
}

void Parser::DefaultArgumentInfo::setFunctionContext(
    DeclContext *DC, ParameterList *paramList){
  for (auto context : ParsedContexts) {
    context->changeFunction(DC, paramList);
  }
}

static ParserStatus parseDefaultArgument(
    Parser &P, Parser::DefaultArgumentInfo *defaultArgs, unsigned argIndex,
    Expr *&init, bool &hasInheritedDefaultArg,
    Parser::ParameterContextKind paramContext) {
  assert(P.Tok.is(tok::equal) ||
       (P.Tok.isBinaryOperator() && P.Tok.getText() == "=="));
  SourceLoc equalLoc = P.consumeToken();

  if (P.SF.Kind == SourceFileKind::Interface) {
    // Swift module interfaces don't synthesize inherited initializers and
    // instead include them explicitly in subclasses. Since the
    // \c DefaultArgumentKind of these initializers is \c Inherited, this is
    // represented textually as `= super` in the interface.

    // If we're in a module interface and the default argument is exactly
    // `super` (i.e. the token after that is `,` or `)` which end a parameter)
    // report an inherited default argument to the caller and return.
    if (P.Tok.is(tok::kw_super) && P.peekToken().isAny(tok::comma, tok::r_paren)) {
      hasInheritedDefaultArg = true;
      P.consumeToken(tok::kw_super);
      defaultArgs->HasDefaultArgument = true;
      return ParserStatus();
    }
  }

  // Enter a fresh default-argument context with a meaningless parent.
  // We'll change the parent to the function later after we've created
  // that declaration.
  auto initDC = new (P.Context) DefaultArgumentInitializer(P.CurDeclContext,
                                                           argIndex);
  Parser::ParseFunctionBody initScope(P, initDC);

  ParserResult<Expr> initR = P.parseExpr(diag::expected_init_value);

  // Record the default-argument context if we're supposed to accept default
  // arguments here.
  if (defaultArgs) {
    defaultArgs->ParsedContexts.push_back(initDC);
  }

  Diag<> diagID = { DiagID() };
  switch (paramContext) {
  case Parser::ParameterContextKind::Function:
  case Parser::ParameterContextKind::Operator:
  case Parser::ParameterContextKind::Initializer:
  case Parser::ParameterContextKind::EnumElement:
  case Parser::ParameterContextKind::Subscript:
  case Parser::ParameterContextKind::Macro:
    break;
  case Parser::ParameterContextKind::Closure:
    diagID = diag::no_default_arg_closure;
    break;
  case Parser::ParameterContextKind::Curried:
    diagID = diag::no_default_arg_curried;
    break;
  }
  
  assert((diagID.ID != DiagID()) == !defaultArgs &&
         "Default arguments specified for an unexpected parameter list kind");
  
  if (diagID.ID != DiagID()) {
    auto inFlight = P.diagnose(equalLoc, diagID);
    if (initR.isNonNull())
      inFlight.fixItRemove(SourceRange(equalLoc, initR.get()->getEndLoc()));
    return ParserStatus();
  }
  
  defaultArgs->HasDefaultArgument = true;

  if (initR.hasCodeCompletion()) {
    init = initR.get();
    return makeParserCodeCompletionStatus();
  }

  if (initR.isNull())
    return makeParserError();

  init = initR.get();
  return ParserStatus();
}

/// Determine whether we are at the start of a parameter name when
/// parsing a parameter.
bool Parser::startsParameterName(bool isClosure) {
  // To have a parameter name here, we need a name.
  if (!Tok.canBeArgumentLabel())
    return false;

  // If the next token is ':', this is a name.
  const auto &nextTok = peekToken();
  if (nextTok.is(tok::colon))
    return true;

  // If the next token can be an argument label, we might have a name.
  if (nextTok.canBeArgumentLabel()) {
    // If the first name wasn't a contextual keyword, we're done.
    if (!Tok.isContextualKeyword("isolated") &&
        !Tok.isContextualKeyword("some") &&
        !Tok.isContextualKeyword("any") &&
        !Tok.isContextualKeyword("each") &&
        !Tok.isContextualKeyword("__shared") &&
        !Tok.isContextualKeyword("__owned") &&
        !Tok.isContextualKeyword("borrowing") &&
        !Tok.isContextualKeyword("consuming") &&
        !Tok.is(tok::kw_repeat))
      return true;

    // Parameter specifiers can be an argument label, but they're also
    // contextual keywords, so look ahead one more token (two total) and see
    // if we have a ':' that would
    // indicate that this is an argument label.
    return lookahead<bool>(2, [&](CancellableBacktrackingScope &) {
      if (Tok.is(tok::colon))
        return true; // isolated :

      return Tok.canBeArgumentLabel() && nextTok.is(tok::colon);
    });
  }

  if (isOptionalToken(nextTok)
      || isImplicitlyUnwrappedOptionalToken(nextTok))
    return false;

  // The identifier could be a name or it could be a type. In a closure, we
  // assume it's a name, because the type can be inferred. Elsewhere, we
  // assume it's a type.
  return isClosure;
}

ParserStatus
Parser::parseParameterClause(SourceLoc &leftParenLoc,
                             SmallVectorImpl<ParsedParameter> &params,
                             SourceLoc &rightParenLoc,
                             DefaultArgumentInfo *defaultArgs,
                             ParameterContextKind paramContext) {
  assert(params.empty() && leftParenLoc.isInvalid() &&
         rightParenLoc.isInvalid() && "Must start with empty state");

  // Consume the starting '(';
  leftParenLoc = consumeToken(tok::l_paren);

  // Trivial case: empty parameter list.
  if (Tok.is(tok::r_paren)) {
    rightParenLoc = consumeToken(tok::r_paren);

    // Per SE-0155, enum elements may not have empty parameter lists.
    if (paramContext == ParameterContextKind::EnumElement) {
      decltype(diag::enum_element_empty_arglist) diagnostic;
      if (Context.isSwiftVersionAtLeast(5)) {
        diagnostic = diag::enum_element_empty_arglist;
      } else {
        diagnostic = diag::enum_element_empty_arglist_swift4;
      }

      diagnose(leftParenLoc, diagnostic)
        .highlight({leftParenLoc, rightParenLoc});
      diagnose(leftParenLoc, diag::enum_element_empty_arglist_delete)
        .fixItRemoveChars(leftParenLoc,
                          Lexer::getLocForEndOfToken(SourceMgr, rightParenLoc));
      diagnose(leftParenLoc, diag::enum_element_empty_arglist_add_void)
        .fixItInsertAfter(leftParenLoc, "Void");
    }
    return ParserStatus();
  }

  // Parse the parameter list.
  bool isClosure = paramContext == ParameterContextKind::Closure;
  return parseList(tok::r_paren, leftParenLoc, rightParenLoc,
                      /*AllowSepAfterLast=*/false,
                      diag::expected_rparen_parameter,
                      [&]() -> ParserStatus {
    ParsedParameter param;
    ParserStatus status;
    SourceLoc StartLoc = Tok.getLoc();

    unsigned defaultArgIndex = defaultArgs ? defaultArgs->NextIndex++ : 0;

    // Attributes.
    if (paramContext != ParameterContextKind::EnumElement) {
      auto AttrStatus = parseDeclAttributeList(param.Attrs);
      if (AttrStatus.hasCodeCompletion()) {
        if (this->CodeCompletionCallbacks)
          this->CodeCompletionCallbacks->setAttrTargetDeclKind(DeclKind::Param);
        status.setHasCodeCompletionAndIsError();
      }
    }

    {
      // ('inout' | '__shared' | '__owned' | isolated)?
      bool hasSpecifier = false;
      while (Tok.is(tok::kw_inout)
             || (canHaveParameterSpecifierContextualKeyword()
                 && (Tok.isContextualKeyword("__shared")
                     || Tok.isContextualKeyword("__owned")
                     || Tok.isContextualKeyword("borrowing")
                     || Tok.isContextualKeyword("consuming")
                     || Tok.isContextualKeyword("isolated")
                     || Tok.isContextualKeyword("_const")))) {
        // is this token the identifier of an argument label? `inout` is a
        // reserved keyword but the other modifiers are not.
        if (!Tok.is(tok::kw_inout)) {
          bool partOfArgumentLabel = lookahead<bool>(1, [&](CancellableBacktrackingScope &) {
            if (Tok.is(tok::colon))
              return true;  // isolated :

            return Tok.canBeArgumentLabel() && peekToken().is(tok::colon);
          });

          if (partOfArgumentLabel)
            break;
        }
        
        if (Tok.isContextualKeyword("isolated")) {
          // did we already find an 'isolated' type modifier?
          if (param.IsolatedLoc.isValid()) {
            diagnose(Tok, diag::parameter_specifier_repeated)
              .fixItRemove(Tok.getLoc());
            consumeToken();
            continue;
          }

          // consume 'isolated' as type modifier
          param.IsolatedLoc = consumeToken();
          continue;
        }

        if (Tok.isContextualKeyword("_const")) {
          param.CompileConstLoc = consumeToken();
          continue;
        }

        if (!hasSpecifier) {
          // These cases are handled later when mapping to ParamDecls for
          // better fixits.
          if (Tok.is(tok::kw_inout)) {
            param.SpecifierKind = ParamDecl::Specifier::InOut;
            param.SpecifierLoc = consumeToken();
          } else if (Tok.isContextualKeyword("borrowing")) {
            param.SpecifierKind = ParamDecl::Specifier::Borrowing;
            param.SpecifierLoc = consumeToken();
          } else if (Tok.isContextualKeyword("consuming")) {
            param.SpecifierKind = ParamDecl::Specifier::Consuming;
            param.SpecifierLoc = consumeToken();
          } else if (Tok.isContextualKeyword("__shared")) {
            param.SpecifierKind = ParamDecl::Specifier::LegacyShared;
            param.SpecifierLoc = consumeToken();
          } else if (Tok.isContextualKeyword("__owned")) {
            param.SpecifierKind = ParamDecl::Specifier::LegacyOwned;
            param.SpecifierLoc = consumeToken();
          }
          hasSpecifier = true;
        } else {
          // Redundant specifiers are fairly common, recognize, reject, and
          // recover from this gracefully.
          diagnose(Tok, diag::parameter_specifier_repeated)
            .fixItRemove(Tok.getLoc());
          consumeToken();
        }
      }
    }
    
    // If let or var is being used as an argument label, allow it but
    // generate a warning.
    if (!isClosure &&
        (Tok.isAny(tok::kw_let, tok::kw_var) ||
         (Context.LangOpts.hasFeature(Feature::ReferenceBindings) &&
          Tok.isAny(tok::kw_inout)))) {
      diagnose(Tok, diag::parameter_let_var_as_attr, Tok.getText())
        .fixItReplace(Tok.getLoc(), "`" + Tok.getText().str() + "`");
    }

    if (startsParameterName(isClosure)) {
      // identifier-or-none for the first name
      param.FirstNameLoc = consumeArgumentLabel(param.FirstName,
                                                /*diagnoseDollarPrefix=*/!isClosure);

      // identifier-or-none? for the second name
      if (Tok.canBeArgumentLabel())
        param.SecondNameLoc = consumeArgumentLabel(param.SecondName,
                                                   /*diagnoseDollarPrefix=*/true);

      // Operators, closures, and enum elements cannot have API names.
      if ((paramContext == ParameterContextKind::Operator ||
           paramContext == ParameterContextKind::Closure ||
           paramContext == ParameterContextKind::EnumElement) &&
          !param.FirstName.empty() &&
          param.SecondNameLoc.isValid()) {
        enum KeywordArgumentDiagnosticContextKind {
          Operator    = 0,
          Closure     = 1,
          EnumElement = 2,
        } diagContextKind;

        switch (paramContext) {
        case ParameterContextKind::Operator:
          diagContextKind = Operator;
          break;
        case ParameterContextKind::Closure:
          diagContextKind = Closure;
          break;
        case ParameterContextKind::EnumElement:
          diagContextKind = EnumElement;
          break;
        default:
          llvm_unreachable("Unhandled parameter context kind!");
        }
        diagnose(param.FirstNameLoc, diag::parameter_operator_keyword_argument,
                 unsigned(diagContextKind))
          .fixItRemoveChars(param.FirstNameLoc, param.SecondNameLoc);
        param.FirstName = param.SecondName;
        param.FirstNameLoc = param.SecondNameLoc;
        param.SecondName = Identifier();
        param.SecondNameLoc = SourceLoc();
      }

      // (':' type)?
      if (consumeIf(tok::colon)) {

        auto type = parseType(diag::expected_parameter_type);
        status |= type;
        param.Type = type.getPtrOrNull();

        // If we didn't parse a type, then we already diagnosed that the type
        // was invalid.  Remember that.
        if (type.isNull() && !type.hasCodeCompletion())
          param.isInvalid = true;
      } else if (paramContext != Parser::ParameterContextKind::Closure) {
        diagnose(Tok, diag::expected_parameter_colon);
        param.isInvalid = true;
      }
    } else {
      // Otherwise, we have invalid code.  Check to see if this looks like a
      // type.  If so, diagnose it as a common error.
      bool isBareType = false;
      {
        BacktrackingScope backtrack(*this);
        isBareType = canParseType() && Tok.isAny(tok::comma, tok::r_paren,
                                                 tok::equal);
      }

      if (isBareType && paramContext == ParameterContextKind::EnumElement) {
        auto type = parseType(diag::expected_parameter_type);
        status |= type;
        param.Type = type.getPtrOrNull();
        param.FirstName = Identifier();
        param.FirstNameLoc = SourceLoc();
        param.SecondName = Identifier();
        param.SecondNameLoc = SourceLoc();
      } else if (isBareType && !Tok.is(tok::code_complete)) {
        // Otherwise, if this is a bare type, then the user forgot to name the
        // parameter, e.g. "func foo(Int) {}"
        // Don't enter this case if the element could only be parsed as a bare
        // type because a code completion token is positioned here. In this case
        // the user is about to type the parameter label and we shouldn't
        // suggest types.
        SourceLoc typeStartLoc = Tok.getLoc();
        auto type = parseType(diag::expected_parameter_type);
        status |= type;
        param.Type = type.getPtrOrNull();

        // If this is a closure declaration, what is going
        // on is most likely argument destructuring, we are going
        // to diagnose that after all of the parameters are parsed.
        if (param.Type) {
          // Mark current parameter type as invalid so it is possible
          // to diagnose it as destructuring of the closure parameter list.
          param.isPotentiallyDestructured = true;
          if (!isClosure) {
            // Unnamed parameters must be written as "_: Type".
            diagnose(typeStartLoc, diag::parameter_unnamed)
                .fixItInsert(typeStartLoc, "_: ");
          } else {
            // Unnamed parameters were accidentally possibly accepted after
            // SE-110 depending on the kind of declaration.  We now need to
            // warn about the misuse of this syntax and offer to
            // fix it.
            // An exception to this rule is when the type is declared with type sugar
            // Reference: https://github.com/apple/swift/issues/54133
            if (isa<OptionalTypeRepr>(param.Type)
                || isa<ImplicitlyUnwrappedOptionalTypeRepr>(param.Type)) {
                diagnose(typeStartLoc, diag::parameter_unnamed)
                    .fixItInsert(typeStartLoc, "_: ");
            } else {
                diagnose(typeStartLoc, diag::parameter_unnamed_warn)
                    .fixItInsert(typeStartLoc, "_: ");
            }
          }
        }
      } else {
        // Otherwise, we're not sure what is going on, but this doesn't smell
        // like a parameter.
        diagnose(Tok, diag::expected_parameter_name);
        param.isInvalid = true;
        param.FirstNameLoc = Tok.getLoc();
        TokReceiver->registerTokenKindChange(param.FirstNameLoc,
                                             tok::identifier);
        status.setIsParseError();
      }
    }

    // If this parameter had an ellipsis, check it has a TypeRepr.
    if (Tok.isEllipsis()) {
      if (param.Type == nullptr && !param.isInvalid) {
        diagnose(Tok, diag::untyped_pattern_ellipsis);
        consumeToken();
      }
    }

    // ('=' expr) or ('==' expr)?
    bool isEqualBinaryOperator =
        Tok.isBinaryOperator() && Tok.getText() == "==";
    if (Tok.is(tok::equal) || isEqualBinaryOperator) {
      SourceLoc EqualLoc = Tok.getLoc();

      if (isEqualBinaryOperator) {
        diagnose(Tok, diag::expected_assignment_instead_of_comparison_operator)
            .fixItReplace(EqualLoc, "=");
      }

      status |= parseDefaultArgument(
          *this, defaultArgs, defaultArgIndex, param.DefaultArg,
          param.hasInheritedDefaultArg, paramContext);
    }

    // If we haven't made progress, don't add the parameter.
    if (Tok.getLoc() == StartLoc) {
      // If we took a default argument index for this parameter, but didn't add
      // one, then give it back.
      if (defaultArgs) --defaultArgs->NextIndex;
      return status;
    }

    params.push_back(param);
    return status;
  });
}

static TypeRepr *
validateParameterWithOwnership(Parser &parser,
                               Parser::ParsedParameter &paramInfo,
                               ParamSpecifier specifier,
                               bool parsingEnumElt) {
  auto type = paramInfo.Type;
  auto loc = paramInfo.SpecifierLoc;
  // If we're validating an enum element, 'inout' is not allowed
  // at all - Sema will catch this for us.  In all other contexts, we
  // assume the user put 'inout' in the wrong place and offer a fixit.
  if (parsingEnumElt) {
    return new (parser.Context) OwnershipTypeRepr(type, specifier, loc);
  }
  
  if (isa<SpecifierTypeRepr>(type)) {
    parser.diagnose(loc, diag::parameter_specifier_repeated).fixItRemove(loc);
  } else {
    auto specifierName = ParamDecl::getSpecifierSpelling(specifier);
    llvm::SmallString<128> replacement(specifierName);
    replacement += " ";
    parser
        .diagnose(loc, diag::parameter_specifier_as_attr_disallowed,
                  specifierName)
        .fixItRemove(loc)
        .fixItInsert(type->getStartLoc(), replacement);
    type = new (parser.Context) OwnershipTypeRepr(type, specifier, loc);
  }

  return type;
}

/// Map parsed parameters to a ParameterList.
static ParameterList *
mapParsedParameters(Parser &parser,
                    SourceLoc leftParenLoc,
                    MutableArrayRef<Parser::ParsedParameter> params,
                    SourceLoc rightParenLoc,
                    SmallVectorImpl<Identifier> *argNames,
                    Parser::ParameterContextKind paramContext) {
  auto &ctx = parser.Context;

  // Local function to create a pattern for a single parameter.
  auto createParam = [&](Parser::ParsedParameter &paramInfo,
                         Identifier argName, SourceLoc argNameLoc,
                         Identifier paramName, SourceLoc paramNameLoc)
  -> ParamDecl * {
    auto param = new (ctx) ParamDecl(paramInfo.SpecifierLoc,
                                     argNameLoc, argName,
                                     paramNameLoc, paramName,
                                     parser.CurDeclContext);
    param->getAttrs() = paramInfo.Attrs;

    bool parsingEnumElt
      = (paramContext == Parser::ParameterContextKind::EnumElement);
    // If we're not parsing an enum case, lack of a SourceLoc for both
    // names indicates the parameter is synthetic.
    if (!parsingEnumElt && argNameLoc.isInvalid() && paramNameLoc.isInvalid())
      param->setImplicit();
    
    // If we diagnosed this parameter as a parse error, propagate to the decl.
    if (paramInfo.isInvalid)
      param->setInvalid();

    // If we need to diagnose this parameter as a destructuring, propagate that
    // to the decl.
    // FIXME: This is a terrible way to catch this.
    if (paramInfo.isPotentiallyDestructured)
      param->setDestructured(true);

    // If a type was provided, create the type for the parameter.
    if (auto type = paramInfo.Type) {
      // If 'inout' was specified, turn the type into an in-out type.
      if (paramInfo.SpecifierKind != ParamDecl::Specifier::Default) {
        type = validateParameterWithOwnership(parser, paramInfo,
                                              paramInfo.SpecifierKind,
                                              parsingEnumElt);
      }

      if (paramInfo.IsolatedLoc.isValid()) {
        type = new (parser.Context) IsolatedTypeRepr(
            type, paramInfo.IsolatedLoc);
        param->setIsolated();
      }

      if (paramInfo.CompileConstLoc.isValid()) {
        type = new (parser.Context) CompileTimeConstTypeRepr(
            type, paramInfo.CompileConstLoc);
        param->setCompileTimeConst();
      }

      param->setTypeRepr(type);

      // Dig through the type to find any attributes or modifiers that are
      // associated with the type but should also be reflected on the
      // declaration.
      {
        auto unwrappedType = type;
        while (true) {
          if (auto *ATR = dyn_cast<AttributedTypeRepr>(unwrappedType)) {
            auto &attrs = ATR->getAttrs();
            // At this point we actually don't know if that's valid to mark
            // this parameter declaration as `autoclosure` because type has
            // not been resolved yet - it should either be a function type
            // or typealias with underlying function type.
            param->setAutoClosure(attrs.has(TypeAttrKind::TAK_autoclosure));

            unwrappedType = ATR->getTypeRepr();
            continue;
          }

          if (auto *STR = dyn_cast<SpecifierTypeRepr>(unwrappedType)) {
            if (isa<IsolatedTypeRepr>(STR))
              param->setIsolated(true);
            else if (isa<CompileTimeConstTypeRepr>(STR))
              param->setCompileTimeConst(true);

            unwrappedType = STR->getBase();
            continue;
          }

          break;
        }
      }
    } else if (paramInfo.SpecifierLoc.isValid()) {
      llvm::SmallString<16> specifier;
      {
        llvm::raw_svector_ostream ss(specifier);
        
        ss << '\'' << ParamDecl::getSpecifierSpelling(paramInfo.SpecifierKind)
           << '\'';
      }
      parser.diagnose(paramInfo.SpecifierLoc, diag::specifier_must_have_type,
                      specifier);
      paramInfo.SpecifierLoc = SourceLoc();
      paramInfo.SpecifierKind = ParamDecl::Specifier::Default;

      param->setSpecifier(ParamSpecifier::Default);
    } else {
      param->setSpecifier(ParamSpecifier::Default);
    }

    return param;
  };

  // Collect the elements of the tuple patterns for argument and body
  // parameters.
  SmallVector<ParamDecl*, 4> elements;

  for (auto &param : params) {
    // Whether the provided name is API by default depends on the parameter
    // context.
    bool isKeywordArgumentByDefault;
    switch (paramContext) {
    case Parser::ParameterContextKind::Closure:
    case Parser::ParameterContextKind::Subscript:
    case Parser::ParameterContextKind::Operator:
      isKeywordArgumentByDefault = false;
      break;
    case Parser::ParameterContextKind::EnumElement:
    case Parser::ParameterContextKind::Curried:
    case Parser::ParameterContextKind::Initializer:
    case Parser::ParameterContextKind::Function:
    case Parser::ParameterContextKind::Macro:
      isKeywordArgumentByDefault = true;
      break;
    }

    // Create the pattern.
    ParamDecl *result = nullptr;
    Identifier argName;
    Identifier paramName;
    if (param.SecondNameLoc.isValid()) {
      argName = param.FirstName;
      paramName = param.SecondName;

      // Both names were provided, so pass them in directly.
      result = createParam(param, argName, param.FirstNameLoc,
                           paramName, param.SecondNameLoc);

      // If the first and second names are equivalent and non-empty, and we
      // would have an argument label by default, complain.
      if (isKeywordArgumentByDefault && param.FirstName == param.SecondName
          && !param.FirstName.empty()) {
        parser.diagnose(param.FirstNameLoc,
                        diag::parameter_extraneous_double_up,
                        param.FirstName)
          .fixItRemoveChars(param.FirstNameLoc, param.SecondNameLoc);
      }
    } else {
      if (isKeywordArgumentByDefault)
        argName = param.FirstName;
      paramName = param.FirstName;

      result = createParam(param, argName, SourceLoc(),
                           param.FirstName, param.FirstNameLoc);
    }

    assert (((!param.DefaultArg &&
              !param.hasInheritedDefaultArg) ||
             paramContext == Parser::ParameterContextKind::Function ||
             paramContext == Parser::ParameterContextKind::Operator ||
             paramContext == Parser::ParameterContextKind::Initializer ||
             paramContext == Parser::ParameterContextKind::EnumElement ||
             paramContext == Parser::ParameterContextKind::Subscript ||
             paramContext == Parser::ParameterContextKind::Macro) &&
            "Default arguments are only permitted on the first param clause");

    if (param.DefaultArg) {
      DefaultArgumentKind kind = getDefaultArgKind(param.DefaultArg);
      result->setDefaultArgumentKind(kind);
      result->setDefaultExpr(param.DefaultArg, /*isTypeChecked*/ false);
    } else if (param.hasInheritedDefaultArg) {
      result->setDefaultArgumentKind(DefaultArgumentKind::Inherited);
    }

    elements.push_back(result);

    if (argNames)
      argNames->push_back(argName);
  }

  return ParameterList::create(ctx, leftParenLoc, elements, rightParenLoc);
}

/// Parse a single parameter-clause.
ParserResult<ParameterList>
Parser::parseSingleParameterClause(ParameterContextKind paramContext,
                                   SmallVectorImpl<Identifier> *namePieces,
                                   DefaultArgumentInfo *defaultArgs) {
  if (!Tok.is(tok::l_paren)) {
    // If we don't have the leading '(', complain.
    Diag<> diagID;
    switch (paramContext) {
    case ParameterContextKind::Function:
    case ParameterContextKind::Operator:
      diagID = diag::func_decl_without_paren;
      break;
    case ParameterContextKind::Subscript:
      diagID = diag::expected_lparen_subscript;
      break;
    case ParameterContextKind::Initializer:
      diagID = diag::expected_lparen_initializer;
      break;
    case ParameterContextKind::Macro:
      diagID = diag::expected_lparen_macro;
      break;
    case ParameterContextKind::EnumElement:
    case ParameterContextKind::Closure:
    case ParameterContextKind::Curried:
      llvm_unreachable("should never be here");
    }

    {
      auto diag = diagnose(Tok, diagID);
      if (Tok.isAny(tok::l_brace, tok::arrow, tok::kw_throws, tok::kw_rethrows))
        diag.fixItInsertAfter(PreviousLoc, "()");
    }

    // Create an empty parameter list to recover.
    return makeParserErrorResult(
        ParameterList::createEmpty(Context, PreviousLoc, PreviousLoc));
  }

  ParserStatus status;
  SmallVector<ParsedParameter, 4> params;
  SourceLoc leftParenLoc, rightParenLoc;
  
  // Parse the parameter clause.
  status |= parseParameterClause(leftParenLoc, params, rightParenLoc,
                                 defaultArgs, paramContext);

  // Turn the parameter clause into argument and body patterns.
  auto paramList = mapParsedParameters(*this, leftParenLoc, params,
                                       rightParenLoc, namePieces, paramContext);

  return makeParserResult(status, paramList);
}

/// Parse function arguments.
///
/// Emits a special diagnostic if there are multiple parameter lists,
/// but otherwise is identical to parseSingleParameterClause().
ParserStatus
Parser::parseFunctionArguments(SmallVectorImpl<Identifier> &NamePieces,
                               ParameterList *&BodyParams,
                               ParameterContextKind paramContext,
                               DefaultArgumentInfo &DefaultArgs) {
  // Parse parameter-clauses.
  ParserStatus status;

  auto FirstParameterClause
    = parseSingleParameterClause(paramContext, &NamePieces, &DefaultArgs);
  status |= FirstParameterClause;
  BodyParams = FirstParameterClause.get();

  bool MultipleParameterLists = false;
  while (Tok.is(tok::l_paren)) {
    MultipleParameterLists = true;
    auto CurriedParameterClause
      = parseSingleParameterClause(ParameterContextKind::Curried);
    status |= CurriedParameterClause;
  }

  // If the decl uses currying syntax, complain that syntax has gone away.
  if (MultipleParameterLists) {
    diagnose(BodyParams->getStartLoc(),
             diag::parameter_curry_syntax_removed);
  }

  return status;
}

/// Parse a function definition signature.
///   func-signature:
///     func-arguments ('async'|'reasync')? func-throws? func-signature-result?
///   func-signature-result:
///     '->' type
///
/// Note that this leaves retType as null if unspecified.
ParserStatus
Parser::parseFunctionSignature(DeclBaseName SimpleName,
                               DeclName &FullName,
                               ParameterList *&bodyParams,
                               DefaultArgumentInfo &defaultArgs,
                               SourceLoc &asyncLoc,
                               bool &reasync,
                               SourceLoc &throwsLoc,
                               bool &rethrows,
                               TypeRepr *&retType) {
  SmallVector<Identifier, 4> NamePieces;
  ParserStatus Status;

  ParameterContextKind paramContext = SimpleName.isOperator()
    ? ParameterContextKind::Operator
    : (SimpleName == DeclBaseName::createConstructor()
         ? ParameterContextKind::Initializer
         : ParameterContextKind::Function);
  Status |= parseFunctionArguments(NamePieces, bodyParams, paramContext,
                                   defaultArgs);
  FullName = DeclName(Context, SimpleName, NamePieces);

  // Check for the 'async' and 'throws' keywords.
  reasync = false;
  rethrows = false;
  Status |= parseEffectsSpecifiers(SourceLoc(),
                                   asyncLoc, &reasync,
                                   throwsLoc, &rethrows);

  // If there's a trailing arrow, parse the rest as the result type.
  SourceLoc arrowLoc;
  if (Tok.isAny(tok::arrow, tok::colon)) {
    if (!consumeIf(tok::arrow, arrowLoc)) {
      // FixIt ':' to '->'.
      diagnose(Tok, diag::func_decl_expected_arrow)
          .fixItReplace(Tok.getLoc(), " -> ");
      arrowLoc = consumeToken(tok::colon);
    }

    // Check for effect specifiers after the arrow, but before the return type,
    // and correct it.
    parseEffectsSpecifiers(arrowLoc, asyncLoc, &reasync, throwsLoc, &rethrows);

    ParserResult<TypeRepr> ResultType =
        parseDeclResultType(diag::expected_type_function_result);
    retType = ResultType.getPtrOrNull();
    Status |= ResultType;
    if (Status.isErrorOrHasCompletion())
      return Status;

    // Check for effect specifiers after the type and correct it.
    parseEffectsSpecifiers(arrowLoc, asyncLoc, &reasync, throwsLoc, &rethrows);
  } else {
    // Otherwise, we leave retType null.
    retType = nullptr;
  }

  return Status;
}

bool Parser::isEffectsSpecifier(const Token &T) {
  // NOTE: If this returns 'true', that token must be handled in
  //       'parseEffectsSpecifiers()'.

  if (T.isContextualKeyword("async") ||
      (T.isContextualKeyword("await") && !T.isAtStartOfLine()) ||
      T.isContextualKeyword("reasync"))
    return true;

  if (T.isAny(tok::kw_throws, tok::kw_rethrows) ||
      (T.isAny(tok::kw_throw, tok::kw_try) && !T.isAtStartOfLine()))
    return true;

  return false;
}

ParserStatus Parser::parseEffectsSpecifiers(SourceLoc existingArrowLoc,
                                            SourceLoc &asyncLoc,
                                            bool *reasync,
                                            SourceLoc &throwsLoc,
                                            bool *rethrows) {
  ParserStatus status;

  while (true) {
    // 'async'
    bool isReasync = (shouldParseExperimentalConcurrency() &&
                      Tok.isContextualKeyword("reasync"));
    if (Tok.isContextualKeyword("async") ||
        isReasync) {
      if (asyncLoc.isValid()) {
        diagnose(Tok, diag::duplicate_effects_specifier, Tok.getText())
            .highlight(asyncLoc)
            .fixItRemove(Tok.getLoc());
      } else if (!reasync && isReasync) {
        // Replace 'reasync' with 'async' unless it's allowed.
        diagnose(Tok, diag::reasync_function_type)
            .fixItReplace(Tok.getLoc(), "async");
      } else if (existingArrowLoc.isValid()) {
        SourceLoc insertLoc = existingArrowLoc;
        if (throwsLoc.isValid() &&
            SourceMgr.isBeforeInBuffer(throwsLoc, insertLoc))
          insertLoc = throwsLoc;
        diagnose(Tok, diag::async_or_throws_in_wrong_position,
                 (reasync && isReasync) ? "reasync" : "async")
            .fixItRemove(Tok.getLoc())
            .fixItInsert(insertLoc,
                         (reasync && isReasync) ? "reasync " : "async ");
      } else if (throwsLoc.isValid()) {
        // 'async' cannot be after 'throws'.
        assert(existingArrowLoc.isInvalid());
        diagnose(Tok, diag::async_after_throws,
                 reasync && isReasync,
                 rethrows && *rethrows)
            .fixItRemove(Tok.getLoc())
            .fixItInsert(throwsLoc, isReasync ? "reasync " : "async ");
      }
      if (asyncLoc.isInvalid()) {
        Tok.setKind(tok::contextual_keyword);
        if (reasync)
          *reasync = isReasync;
        asyncLoc = Tok.getLoc();
      }
      consumeToken();
      continue;
    }
    // diagnose 'await'
    if (Tok.isContextualKeyword("await") && !Tok.isAtStartOfLine()) {
      diagnose(Tok, diag::await_in_function_type)
        .fixItReplace(Tok.getLoc(), "async");
      consumeToken();
      continue;
    }

    // 'throws'/'rethrows', or diagnose 'throw'/'try'.
    if (Tok.isAny(tok::kw_throws, tok::kw_rethrows) ||
        (Tok.isAny(tok::kw_throw, tok::kw_try) && !Tok.isAtStartOfLine())) {
      bool isRethrows = Tok.is(tok::kw_rethrows);

      if (throwsLoc.isValid()) {
        diagnose(Tok, diag::duplicate_effects_specifier, Tok.getText())
            .highlight(throwsLoc)
            .fixItRemove(Tok.getLoc());
      } else if (Tok.isAny(tok::kw_throw, tok::kw_try)) {
        // Replace 'throw' or 'try' with 'throws'.
        diagnose(Tok, diag::throw_in_function_type)
            .fixItReplace(Tok.getLoc(), "throws");
      } else if (!rethrows && isRethrows) {
        // Replace 'rethrows' with 'throws' unless it's allowed.
        diagnose(Tok, diag::rethrowing_function_type)
            .fixItReplace(Tok.getLoc(), "throws");
      } else if (existingArrowLoc.isValid()) {
        diagnose(Tok, diag::async_or_throws_in_wrong_position, Tok.getText())
            .fixItRemove(Tok.getLoc())
            .fixItInsert(existingArrowLoc, (Tok.getText() + " ").str());
      }

      if (throwsLoc.isInvalid()) {
        if (rethrows)
          *rethrows = isRethrows;
        throwsLoc = Tok.getLoc();
      }
      consumeToken();
      continue;
    }

    // Code completion.
    if (Tok.is(tok::code_complete) && !Tok.isAtStartOfLine() &&
        !existingArrowLoc.isValid()) {
      if (CodeCompletionCallbacks) {
        CodeCompletionCallbacks->completeEffectsSpecifier(asyncLoc.isValid(),
                                                          throwsLoc.isValid());
      }
      consumeToken(tok::code_complete);
      status.setHasCodeCompletionAndIsError();
      continue;
    }

    break;
  }
  return status;
}

/// Parse a pattern with an optional type annotation.
///
///  typed-pattern ::= pattern (':' type)?
///
ParserResult<Pattern> Parser::parseTypedPattern() {
  auto result = parsePattern();
  
  // Now parse an optional type annotation.
  if (Tok.is(tok::colon)) {
    SourceLoc colonLoc = consumeToken(tok::colon);
    
    if (result.isNull()) {
      // Recover by creating AnyPattern.
      auto *AP = new (Context) AnyPattern(colonLoc);
      if (colonLoc.isInvalid())
        AP->setImplicit();
      result = makeParserErrorResult(AP);
    }

    ParserResult<TypeRepr> Ty = parseDeclResultType(diag::expected_type);
    if (Ty.hasCodeCompletion())
      return makeParserCodeCompletionResult<Pattern>();
    if (!Ty.isNull()) {
      // Attempt to diagnose initializer calls incorrectly written
      // as typed patterns, such as "var x: [Int]()".
      // Disable this tentative parse when in IDE inspection mode, otherwise
      // code-completion may enter the delayed-decl state twice.
      if (Tok.isFollowingLParen() &&
          !SourceMgr.hasIDEInspectionTargetBuffer()) {
        CancellableBacktrackingScope backtrack(*this);

        SmallVector<ExprListElt, 2> elts;
        auto argListResult = parseArgumentList(tok::l_paren, tok::r_paren,
                                               /*isExprBasic*/ false);
        if (!argListResult.isParseErrorOrHasCompletion()) {
          backtrack.cancelBacktrack();
          
          // Suggest replacing ':' with '='
          auto *args = argListResult.get();
          diagnose(args->getLParenLoc(), diag::initializer_as_typed_pattern)
            .highlight({Ty.get()->getStartLoc(), args->getRParenLoc()})
            .fixItReplace(colonLoc, " = ");
          result.setIsParseError();
        }
      }
    } else {
      Ty = makeParserResult(new (Context) ErrorTypeRepr(PreviousLoc));
    }
    
    result = makeParserResult(result,
                            new (Context) TypedPattern(result.get(), Ty.get()));
  }
  
  return result;
}

/// Parse a pattern.
///   pattern ::= identifier
///   pattern ::= '_'
///   pattern ::= pattern-tuple
///   pattern ::= 'var' pattern
///   pattern ::= 'let' pattern
///
ParserResult<Pattern> Parser::parsePattern() {
  auto introducer =
      InBindingPattern.getIntroducer().value_or(VarDecl::Introducer::Let);
  switch (Tok.getKind()) {
  case tok::l_paren:
    return parsePatternTuple();

  case tok::kw__: {
    // Normally, '_' is invalid in type context for patterns, but they show up
    // in interface files as the name for type members that are non-public.
    // Treat them as an implicitly synthesized NamedPattern with a nameless
    // VarDecl inside.
    if (CurDeclContext->isTypeContext() &&
        SF.Kind == SourceFileKind::Interface) {
      auto VD = new (Context) VarDecl(
        /*IsStatic*/false, introducer,
        consumeToken(tok::kw__), Identifier(), CurDeclContext);
      return makeParserResult(NamedPattern::createImplicit(Context, VD));
    }

    const auto isAsyncLet =
        InPatternWithAsyncAttribute && introducer == VarDecl::Introducer::Let;
    return makeParserResult(
        new (Context) AnyPattern(consumeToken(tok::kw__), isAsyncLet));
  }
  case tok::identifier: {
    Identifier name;
    SourceLoc loc = consumeIdentifier(name, /*diagnoseDollarPrefix=*/true);
    if (Tok.isIdentifierOrUnderscore() && !Tok.isContextualDeclKeyword() &&
        !Tok.isAtStartOfLine())
      diagnoseConsecutiveIDs(name.str(), loc,
                             introducer == VarDecl::Introducer::Let
                             ? "constant" : "variable");

    return makeParserResult(createBindingFromPattern(loc, name, introducer));
  }
    
  case tok::code_complete:
    if (!CurDeclContext->isTypeContext()) {
      // This cannot be an overridden property, so just eat the token. We cannot
      // code complete anything here -- we expect an identifier.
      consumeToken(tok::code_complete);
    }
    return makeParserCodeCompletionStatus();
  case tok::kw_inout:
    // If we don't have the reference binding feature, break if we have
    // inout. Otherwise, go below.
    if (!Context.LangOpts.hasFeature(Feature::ReferenceBindings))
      break;
    LLVM_FALLTHROUGH;
  case tok::kw_var:
  case tok::kw_let: {
    auto newBindingState = PatternBindingState(Tok);
    SourceLoc varLoc = consumeToken();

    // 'var', 'let', 'inout' patterns shouldn't nest.
    if (InBindingPattern.getIntroducer().has_value()) {
      auto diag = diag::var_pattern_in_var;
      unsigned index = *newBindingState.getSelectIndexForIntroducer();
      if (Context.LangOpts.hasFeature(Feature::ReferenceBindings)) {
        diag = diag::var_pattern_in_var_inout;
      }
      diagnose(varLoc, diag, index);
    }

    // 'let' isn't valid inside an implicitly immutable context, but var is.
    if (newBindingState.isLet() &&
        InBindingPattern == PatternBindingState::ImplicitlyImmutable)
      diagnose(varLoc, diag::let_pattern_in_immutable_context);

    // In our recursive parse, remember that we're in a let/var/inout
    // pattern. We default to var if we don't have an immediate pattern bidning
    // state.
    llvm::SaveAndRestore<decltype(InBindingPattern)> T(
        InBindingPattern, newBindingState.getPatternBindingStateForIntroducer(
                              VarDecl::Introducer::Var));

    // Reset async attribute in parser context.
    llvm::SaveAndRestore<bool> AsyncAttr(InPatternWithAsyncAttribute, false);

    ParserResult<Pattern> subPattern = parsePattern();
    if (subPattern.hasCodeCompletion())
      return makeParserCodeCompletionResult<Pattern>();
    if (subPattern.isNull())
      return nullptr;
    return makeParserResult(new (Context) BindingPattern(
        varLoc,
        newBindingState.getIntroducer().value_or(VarDecl::Introducer::Var),
        subPattern.get()));
  }
      
  default:
    break;
  }

  // Handle the default case.
  if (Tok.isKeyword() &&
      (peekToken().is(tok::colon) || peekToken().is(tok::equal))) {
    diagnose(Tok, diag::keyword_cant_be_identifier, Tok.getText());
    diagnose(Tok, diag::backticks_to_escape)
      .fixItReplace(Tok.getLoc(), "`" + Tok.getText().str() + "`");
    SourceLoc Loc = Tok.getLoc();
    consumeToken();
    return makeParserErrorResult(new (Context) AnyPattern(Loc));
  }
  diagnose(Tok, diag::expected_pattern);
  return nullptr;
}

Pattern *Parser::createBindingFromPattern(SourceLoc loc, Identifier name,
                                          VarDecl::Introducer introducer) {
  auto var = new (Context) VarDecl(/*IsStatic*/false, introducer,
                                   loc, name, CurDeclContext);
  return new (Context) NamedPattern(var);
}

/// Parse an element of a tuple pattern.
///
///   pattern-tuple-element:
///     (identifier ':')? pattern
std::pair<ParserStatus, llvm::Optional<TuplePatternElt>>
Parser::parsePatternTupleElement() {
  // If this element has a label, parse it.
  Identifier Label;
  SourceLoc LabelLoc;

  // If the tuple element has a label, parse it.
  if (Tok.is(tok::identifier) && peekToken().is(tok::colon)) {
    LabelLoc = consumeIdentifier(Label, /*diagnoseDollarPrefix=*/true);
    consumeToken(tok::colon);
  }

  // Parse the pattern.
  ParserResult<Pattern>  pattern = parsePattern();
  if (pattern.hasCodeCompletion())
    return std::make_pair(makeParserCodeCompletionStatus(), llvm::None);
  if (pattern.isNull())
    return std::make_pair(makeParserError(), llvm::None);

  auto Elt = TuplePatternElt(Label, LabelLoc, pattern.get());
  return std::make_pair(makeParserSuccess(), Elt);
}

/// Parse a tuple pattern.
///
///   pattern-tuple:
///     '(' pattern-tuple-body? ')'
///   pattern-tuple-body:
///     pattern-tuple-element (',' pattern-tuple-body)*
ParserResult<Pattern> Parser::parsePatternTuple() {
  StructureMarkerRAII ParsingPatternTuple(*this, Tok);
  SourceLoc LPLoc = consumeToken(tok::l_paren);
  SourceLoc RPLoc;

  // Parse all the elements.
  SmallVector<TuplePatternElt, 8> elts;
  ParserStatus ListStatus =
    parseList(tok::r_paren, LPLoc, RPLoc,
              /*AllowSepAfterLast=*/false,
              diag::expected_rparen_tuple_pattern_list,
              [&] () -> ParserStatus {
    // Parse the pattern tuple element.
    ParserStatus EltStatus;
    llvm::Optional<TuplePatternElt> elt;
    std::tie(EltStatus, elt) = parsePatternTupleElement();
    if (EltStatus.hasCodeCompletion())
      return makeParserCodeCompletionStatus();
    if (!elt)
      return makeParserError();

    // Add this element to the list.
    elts.push_back(*elt);
    return makeParserSuccess();
  });

  return makeParserResult(
           ListStatus,
           TuplePattern::createSimple(Context, LPLoc, elts, RPLoc));
}

/// Parse an optional type annotation on a pattern.
///
///  pattern-type-annotation ::= (':' type)?
///
ParserResult<Pattern> Parser::
parseOptionalPatternTypeAnnotation(ParserResult<Pattern> result) {
  if (!Tok.is(tok::colon))
    return result;

  // Parse an optional type annotation.
  consumeToken(tok::colon);

  if (result.isNull())
    return result;

  Pattern *P = result.get();
  ParserStatus status;
  if (result.hasCodeCompletion())
    status.setHasCodeCompletionAndIsError();

  ParserResult<TypeRepr> Ty = parseType();
  if (Ty.hasCodeCompletion()) {
    result.setHasCodeCompletionAndIsError();
    return result;
  }

  TypeRepr *repr = Ty.getPtrOrNull();
  if (!repr)
    repr = new (Context) ErrorTypeRepr(PreviousLoc);

  return makeParserResult(status, new (Context) TypedPattern(P, repr));
}



/// matching-pattern ::= 'is' type
/// matching-pattern ::= matching-pattern-var
/// matching-pattern ::= expr
///
ParserResult<Pattern> Parser::parseMatchingPattern(bool isExprBasic) {
  // TODO: Since we expect a pattern in this position, we should optimistically
  // parse pattern nodes for productions shared by pattern and expression
  // grammar. For short-term ease of initial implementation, we always go
  // through the expr parser for ambiguous productions.

  // Parse productions that can only be patterns.
  if (Tok.isAny(tok::kw_var, tok::kw_let) ||
      (Context.LangOpts.hasFeature(Feature::ReferenceBindings) &&
       Tok.isAny(tok::kw_inout))) {
    assert(Tok.isAny(tok::kw_let, tok::kw_var, tok::kw_inout) && "expects var or let");
    auto newPatternBindingState = PatternBindingState(Tok);
    SourceLoc varLoc = consumeToken();

    return parseMatchingPatternAsBinding(newPatternBindingState, varLoc,
                                         isExprBasic);
  }

  // matching-pattern ::= 'is' type
  if (Tok.is(tok::kw_is)) {
    SourceLoc isLoc = consumeToken(tok::kw_is);
    ParserResult<TypeRepr> castType = parseType();
    if (castType.isNull() || castType.hasCodeCompletion())
      return nullptr;
    auto *CastTE = new (Context) TypeExpr(castType.get());
    return makeParserResult(new (Context) IsPattern(
        isLoc, CastTE, nullptr, CheckedCastKind::Unresolved));
  }

  // matching-pattern ::= expr
  // Fall back to expression parsing for ambiguous forms. Name lookup will
  // disambiguate.
  ParserResult<Expr> subExpr =
    parseExprImpl(diag::expected_pattern, isExprBasic);
  ParserStatus status = subExpr;
  if (subExpr.isNull())
    return status;

  if (isa<CodeCompletionExpr>(subExpr.get()) && Tok.isFollowingLParen()) {
    // We are in the case like the following of parsing a pattern with the code
    // completion token as base and associated value matches:
    // #^COMPLETE^#(let a)
    // We will have not consumed the `(let a)` in `parseExprPostfixSuffix`
    // because usually suffixes don't influence the code completion's type and
    // the suffix might be unrelated. But the trailing `(let a)` that is left
    // prevents us from forming a valid pattern.
    // Consume and discard the `(let a)`, which just leaves us with the base
    // of the pattern.
    (void)parseExprCallSuffix(subExpr, isExprBasic);
  }

  // The most common case here is to parse something that was a lexically
  // obvious pattern, which will come back wrapped in an immediate
  // UnresolvedPatternExpr.  Transform this now to simplify later code.
  if (auto *UPE = dyn_cast<UnresolvedPatternExpr>(subExpr.get()))
    return makeParserResult(status, UPE->getSubPattern());

  auto *EP = ExprPattern::createParsed(Context, subExpr.get(), CurDeclContext);
  return makeParserResult(status, EP);
}

ParserResult<Pattern>
Parser::parseMatchingPatternAsBinding(PatternBindingState newState,
                                      SourceLoc varLoc, bool isExprBasic) {
  // 'var', 'let', 'inout' patterns shouldn't nest.
  if (InBindingPattern.getIntroducer().has_value()) {
    auto diag = diag::var_pattern_in_var;
    if (Context.LangOpts.hasFeature(Feature::ReferenceBindings))
      diag = diag::var_pattern_in_var_inout;
    diagnose(varLoc, diag,
             *newState.getSelectIndexForIntroducer());
  }

  // 'let' isn't valid inside an implicitly immutable context, but var is.
  if (newState.isLet() &&
      InBindingPattern == PatternBindingState::ImplicitlyImmutable)
    diagnose(varLoc, diag::let_pattern_in_immutable_context);

  // In our recursive parse, remember that we're in a var/let pattern.
  llvm::SaveAndRestore<decltype(InBindingPattern)> T(
      InBindingPattern,
      newState.getPatternBindingStateForIntroducer(VarDecl::Introducer::Var));

  // Reset async attribute in parser context.
  llvm::SaveAndRestore<bool> AsyncAttr(InPatternWithAsyncAttribute, false);

  ParserResult<Pattern> subPattern = parseMatchingPattern(isExprBasic);
  if (subPattern.isNull())
    return nullptr;
  auto *varP = new (Context) BindingPattern(
      varLoc, newState.getIntroducer().value_or(VarDecl::Introducer::Var),
      subPattern.get());
  return makeParserResult(ParserStatus(subPattern), varP);
}

bool Parser::isOnlyStartOfMatchingPattern() {
  return Tok.isAny(tok::kw_var, tok::kw_let, tok::kw_is) ||
         (Context.LangOpts.hasFeature(Feature::ReferenceBindings) &&
          Tok.isAny(tok::kw_inout));
}


static bool canParsePatternTuple(Parser &P);

///   pattern ::= identifier
///   pattern ::= '_'
///   pattern ::= pattern-tuple
///   pattern ::= 'var' pattern
///   pattern ::= 'let' pattern
static bool canParsePattern(Parser &P) {
  switch (P.Tok.getKind()) {
  case tok::identifier:
  case tok::kw__:
    P.consumeToken();
    return true;
  case tok::kw_inout:
    if (!P.Context.LangOpts.hasFeature(Feature::ReferenceBindings))
      return false;
    LLVM_FALLTHROUGH;
  case tok::kw_let:
  case tok::kw_var:
    P.consumeToken();
    return canParsePattern(P);
  case tok::l_paren:
    return canParsePatternTuple(P);

  default:
    return false;
  }
}


static bool canParsePatternTuple(Parser &P) {
  if (!P.consumeIf(tok::l_paren)) return false;

  if (P.Tok.isNot(tok::r_paren)) {
    do {
      if (!canParsePattern(P)) return false;
    } while (P.consumeIf(tok::comma));
  }

  return P.consumeIf(tok::r_paren);
}

///  typed-pattern ::= pattern (':' type)?
///
bool Parser::canParseTypedPattern() {
  if (!canParsePattern(*this)) return false;
  
  if (consumeIf(tok::colon))
    return canParseType();
  return true;
}
