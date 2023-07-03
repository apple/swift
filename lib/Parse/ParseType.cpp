//===--- ParseType.cpp - Swift Language Parser for Types ------------------===//
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
// Type Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Attr.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/SourceFile.h" // only for isMacroSignatureFile
#include "swift/AST/TypeRepr.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/IDEInspectionCallbacks.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;

TypeRepr *Parser::applyAttributeToType(TypeRepr *ty,
                                       const TypeAttributes &attrs,
                                       ParamDecl::Specifier specifier,
                                       SourceLoc specifierLoc,
                                       SourceLoc isolatedLoc,
                                       SourceLoc constLoc) {
  // Apply those attributes that do apply.
  if (!attrs.empty()) {
    ty = new (Context) AttributedTypeRepr(attrs, ty);
  }

  // Apply 'inout', 'consuming', or 'borrowing' modifiers.
  if (specifierLoc.isValid() &&
      specifier != ParamDecl::Specifier::Default) {
    ty = new (Context) OwnershipTypeRepr(ty, specifier, specifierLoc);
  }
  
  // Apply 'isolated'.
  if (isolatedLoc.isValid()) {
    ty = new (Context) IsolatedTypeRepr(ty, isolatedLoc);
  }

  if (constLoc.isValid()) {
    ty = new (Context) CompileTimeConstTypeRepr(ty, constLoc);
  }

  return ty;
}

LayoutConstraint Parser::parseLayoutConstraint(Identifier LayoutConstraintID) {
  LayoutConstraint layoutConstraint =
      getLayoutConstraint(LayoutConstraintID, Context);
  assert(layoutConstraint->isKnownLayout() &&
         "Expected layout constraint definition");

  if (!layoutConstraint->isTrivial())
    return layoutConstraint;

  SourceLoc LParenLoc;
  if (!consumeIf(tok::l_paren, LParenLoc)) {
    // It is a trivial without any size constraints.
    return LayoutConstraint::getLayoutConstraint(LayoutConstraintKind::Trivial,
                                                 Context);
  }

  int size = 0;
  int alignment = 0;

  auto ParseTrivialLayoutConstraintBody = [&] () -> bool {
    // Parse the size and alignment.
    if (Tok.is(tok::integer_literal)) {
      if (Tok.getText().getAsInteger(10, size)) {
        diagnose(Tok.getLoc(), diag::layout_size_should_be_positive);
        return true;
      }
      consumeToken();
      if (consumeIf(tok::comma)) {
        // parse alignment.
        if (Tok.is(tok::integer_literal)) {
          if (Tok.getText().getAsInteger(10, alignment)) {
            diagnose(Tok.getLoc(), diag::layout_alignment_should_be_positive);
            return true;
          }
          consumeToken();
        } else {
          diagnose(Tok.getLoc(), diag::layout_alignment_should_be_positive);
          return true;
        }
      }
    } else {
      diagnose(Tok.getLoc(), diag::layout_size_should_be_positive);
      return true;
    }
    return false;
  };

  if (ParseTrivialLayoutConstraintBody()) {
    // There was an error during parsing.
    skipUntil(tok::r_paren);
    consumeIf(tok::r_paren);
    return LayoutConstraint::getUnknownLayout();
  }

  if (!consumeIf(tok::r_paren)) {
    // Expected a closing r_paren.
    diagnose(Tok.getLoc(), diag::expected_rparen_layout_constraint);
    consumeToken();
    return LayoutConstraint::getUnknownLayout();
  }

  if (size < 0) {
    diagnose(Tok.getLoc(), diag::layout_size_should_be_positive);
    return LayoutConstraint::getUnknownLayout();
  }

  if (alignment < 0) {
    diagnose(Tok.getLoc(), diag::layout_alignment_should_be_positive);
    return LayoutConstraint::getUnknownLayout();
  }

  // Otherwise it is a trivial layout constraint with
  // provided size and alignment.
  return LayoutConstraint::getLayoutConstraint(layoutConstraint->getKind(), size,
                                               alignment, Context);
}

/// parseTypeSimple
///   type-simple:
///     type-identifier
///     type-tuple
///     type-composition-deprecated
///     'Any'
///     type-simple '.Type'
///     type-simple '.Protocol'
///     type-simple '?'
///     type-simple '!'
///     type-collection
///     type-array
///     '_'
///     'Pack' '{' (type (',' type)*)? '}'    (only in SIL files)a
ParserResult<TypeRepr> Parser::parseTypeSimple(
    Diag<> MessageID, ParseTypeReason reason) {
  ParserResult<TypeRepr> ty;


  // Prevent the use of ~ as prefix for a type. We specially parse them
  // in inheritance clauses elsewhere.
  if (Tok.isTilde()) {
    auto tildeLoc = consumeToken();
    diagnose(tildeLoc, diag::cannot_suppress_here)
        .fixItRemoveChars(tildeLoc, tildeLoc);
  }

  if (Tok.is(tok::kw_inout)
      || (canHaveParameterSpecifierContextualKeyword()
          && (Tok.getRawText().equals("__shared")
              || Tok.getRawText().equals("__owned")
              || Tok.getRawText().equals("consuming")
              || Tok.getRawText().equals("borrowing")))) {
    // Type specifier should already be parsed before here. This only happens
    // for construct like 'P1 & inout P2'.
    diagnose(Tok.getLoc(), diag::attr_only_on_parameters, Tok.getRawText());
    consumeToken();
  }

  switch (Tok.getKind()) {
  case tok::kw_Self:
  case tok::identifier:
    // In SIL files (not just when parsing SIL types), accept the
    // Pack{} syntax for spelling variadic type packs.
    if (isInSILMode() && Tok.isContextualKeyword("Pack") &&
        peekToken().is(tok::l_brace)) {
      TokReceiver->registerTokenKindChange(Tok.getLoc(),
                                           tok::contextual_keyword);
      SourceLoc keywordLoc = consumeToken(tok::identifier);
      SourceLoc lbLoc = consumeToken(tok::l_brace);
      SourceLoc rbLoc;
      SmallVector<TypeRepr *, 8> elements;
      auto status = parseList(tok::r_brace, lbLoc, rbLoc,
                              /*AllowSepAfterLast=*/false,
                              diag::expected_rbrace_pack_type_list,
                              [&] () -> ParserStatus {
        auto element = parseType(diag::expected_type);
        if (element.hasCodeCompletion())
          return makeParserCodeCompletionStatus();
        if (element.isNull())
          return makeParserError();
        elements.push_back(element.get());
        return makeParserSuccess();
      });

      ty = makeParserResult(
          status, PackTypeRepr::create(Context, keywordLoc,
                                       SourceRange(lbLoc, rbLoc), elements));
    } else {
      ty = parseTypeIdentifier();
      if (auto *ITR = cast_or_null<IdentTypeRepr>(ty.getPtrOrNull())) {
        if (Tok.is(tok::code_complete) && !Tok.isAtStartOfLine()) {
          if (CodeCompletionCallbacks) {
            CodeCompletionCallbacks->completeTypeSimpleWithoutDot(ITR);
          }

          ty.setHasCodeCompletionAndIsError();
          consumeToken(tok::code_complete);
          return ty;
        }
      }
    }
    break;
  case tok::kw_Any:
    ty = parseAnyType();
    break;
  case tok::l_paren:
    ty = parseTypeTupleBody();
    break;
  case tok::code_complete:
    if (CodeCompletionCallbacks) {
      CodeCompletionCallbacks->completeTypeSimpleBeginning();
    }
    return makeParserCodeCompletionResult<TypeRepr>(
        new (Context) ErrorTypeRepr(consumeToken(tok::code_complete)));
  case tok::l_square: {
    ty = parseTypeCollection();
    break;
  }
  case tok::kw__:
    ty = makeParserResult(new (Context) PlaceholderTypeRepr(consumeToken()));
    break;
  case tok::kw_protocol:
    if (startsWithLess(peekToken())) {
      ty = parseOldStyleProtocolComposition();
      break;
    }
    LLVM_FALLTHROUGH;
  default:
    {
      auto diag = diagnose(Tok, MessageID);
      // If the next token is closing or separating, the type was likely forgotten
      if (Tok.isAny(tok::r_paren, tok::r_brace, tok::r_square, tok::arrow,
                    tok::equal, tok::comma, tok::semi))
        diag.fixItInsert(getEndOfPreviousLoc(), " <#type#>");
    }
    if (Tok.isKeyword() && !Tok.isAtStartOfLine()) {
      ty = makeParserErrorResult(new (Context) ErrorTypeRepr(Tok.getLoc()));
      consumeToken();
      return ty;
    }
    checkForInputIncomplete();
    return nullptr;
  }

  // '.X', '.Type', '.Protocol', '?', '!', '[]'.
  while (ty.isNonNull()) {
    if (Tok.isAny(tok::period, tok::period_prefix)) {
      if (peekToken().is(tok::code_complete)) {
        consumeToken();

        if (CodeCompletionCallbacks) {
          CodeCompletionCallbacks->completeTypeSimpleWithDot(ty.get());
        }

        ty.setHasCodeCompletionAndIsError();
        consumeToken(tok::code_complete);
        break;
      }

      ty = parseTypeDotted(ty);
      continue;
    }

    if (!Tok.isAtStartOfLine()) {
      if (isOptionalToken(Tok)) {
        ty = parseTypeOptional(ty);
        continue;
      }
      if (isImplicitlyUnwrappedOptionalToken(Tok)) {
        ty = parseTypeImplicitlyUnwrappedOptional(ty);
        continue;
      }
      // Parse legacy array types for migration.
      if (Tok.is(tok::l_square) && reason != ParseTypeReason::CustomAttribute) {
        ty = parseTypeArray(ty);
        continue;
      }
    }

    if (Tok.is(tok::code_complete) && !Tok.isAtStartOfLine()) {
      if (CodeCompletionCallbacks) {
        CodeCompletionCallbacks->completeTypeSimpleWithoutDot(ty.get());
      }

      ty.setHasCodeCompletionAndIsError();
      consumeToken(tok::code_complete);
    }
    break;
  }

  return ty;
}

ParserResult<TypeRepr> Parser::parseType() {
  return parseType(diag::expected_type);
}

ParserResult<TypeRepr> Parser::parseSILBoxType(GenericParamList *generics,
                                               const TypeAttributes &attrs) {
  auto LBraceLoc = consumeToken(tok::l_brace);
  
  SmallVector<SILBoxTypeRepr::Field, 4> Fields;
  if (!Tok.is(tok::r_brace)) {
    for (;;) {
      bool Mutable;
      if (Tok.is(tok::kw_var)) {
        Mutable = true;
      } else if (Tok.is(tok::kw_let)) {
        Mutable = false;
      } else {
        diagnose(Tok, diag::sil_box_expected_var_or_let);
        return makeParserError();
      }
      SourceLoc VarOrLetLoc = consumeToken();
      
      auto fieldTy = parseType();
      if (!fieldTy.getPtrOrNull())
        return makeParserError();
      Fields.push_back({VarOrLetLoc, Mutable, fieldTy.get()});
      
      if (!consumeIf(tok::comma))
        break;
    }
  }
  
  if (!Tok.is(tok::r_brace)) {
    diagnose(Tok, diag::sil_box_expected_r_brace);
    return makeParserError();
  }
  
  auto RBraceLoc = consumeToken(tok::r_brace);

  SourceLoc LAngleLoc, RAngleLoc;
  SmallVector<TypeRepr*, 4> Args;
  if (startsWithLess(Tok)) {
    LAngleLoc = consumeStartingLess();
    for (;;) {
      auto argTy = parseType();
      if (!argTy.getPtrOrNull())
        return makeParserError();
      Args.push_back(argTy.get());
      if (!consumeIf(tok::comma))
        break;
    }
    if (!startsWithGreater(Tok)) {
      diagnose(Tok, diag::sil_box_expected_r_angle);
      return makeParserError();
    }
    
    RAngleLoc = consumeStartingGreater();
  }
  
  auto repr = SILBoxTypeRepr::create(Context, generics,
                                     LBraceLoc, Fields, RBraceLoc,
                                     LAngleLoc, Args, RAngleLoc);
  return makeParserResult(applyAttributeToType(repr, attrs,
                                               ParamDecl::Specifier::LegacyOwned,
                                               SourceLoc(), SourceLoc(),
                                               SourceLoc()));
}


/// parseTypeScalar
///   type-scalar:
///     attribute-list type-composition
///     attribute-list type-function
///
///   type-function:
///     type-composition 'async'? 'throws'? '->' type-scalar
///
ParserResult<TypeRepr> Parser::parseTypeScalar(
    Diag<> MessageID, ParseTypeReason reason) {
  // Start a context for creating type syntax.
  ParserStatus status;

  // Parse attributes.
  ParamDecl::Specifier specifier;
  SourceLoc specifierLoc;
  SourceLoc isolatedLoc;
  SourceLoc constLoc;
  TypeAttributes attrs;
  status |= parseTypeAttributeList(specifier, specifierLoc, isolatedLoc, constLoc,
                                   attrs);

  // Parse generic parameters in SIL mode.
  GenericParamList *generics = nullptr;
  SourceLoc substitutedLoc;
  GenericParamList *patternGenerics = nullptr;
  if (isInSILMode()) {
    generics = maybeParseGenericParams().getPtrOrNull();
    
    if (Tok.is(tok::at_sign) && peekToken().getText() == "substituted") {
      consumeToken(tok::at_sign);
      substitutedLoc = consumeToken(tok::identifier);
      patternGenerics = maybeParseGenericParams().getPtrOrNull();
      if (!patternGenerics) {
        diagnose(Tok.getLoc(), diag::sil_function_subst_expected_generics);
      }
    }
  }
  
  // In SIL mode, parse box types { ... }.
  if (isInSILMode() && Tok.is(tok::l_brace)) {
    if (patternGenerics) {
      diagnose(Tok.getLoc(), diag::sil_function_subst_expected_function);
    }
    return parseSILBoxType(generics, attrs);
  }

  ParserResult<TypeRepr> ty = parseTypeSimpleOrComposition(MessageID, reason);
  status |= ParserStatus(ty);
  if (ty.isNull())
    return status;
  auto tyR = ty.get();

  // Parse effects specifiers.
  // Don't consume them, if there's no following '->', so we can emit a more
  // useful diagnostic when parsing a function decl.
  SourceLoc asyncLoc;
  SourceLoc throwsLoc;
  if (isAtFunctionTypeArrow()) {
    status |= parseEffectsSpecifiers(SourceLoc(),
                                     asyncLoc, /*reasync=*/nullptr,
                                     throwsLoc, /*rethrows=*/nullptr);
  }

  // Handle type-function if we have an arrow.
  if (Tok.is(tok::arrow)) {
    SourceLoc arrowLoc = consumeToken();

    // Handle async/throws in the wrong place.
    parseEffectsSpecifiers(arrowLoc,
                           asyncLoc, /*reasync=*/nullptr,
                           throwsLoc, /*rethrows=*/nullptr);

    ParserResult<TypeRepr> SecondHalf =
        parseTypeScalar(diag::expected_type_function_result,
                        ParseTypeReason::Unspecified);
    status |= SecondHalf;
    if (SecondHalf.isNull()) {
      status.setIsParseError();
      return status;
    }

    TupleTypeRepr *argsTyR = nullptr;
    if (auto *TTArgs = dyn_cast<TupleTypeRepr>(tyR)) {
      argsTyR = TTArgs;
    } else {
      bool isVoid = false;
      if (const auto Void = dyn_cast<SimpleIdentTypeRepr>(tyR)) {
        if (Void->getNameRef().isSimpleName(Context.Id_Void)) {
          isVoid = true;
        }
      }

      if (isVoid) {
        diagnose(tyR->getStartLoc(), diag::function_type_no_parens)
          .fixItReplace(tyR->getStartLoc(), "()");
        argsTyR = TupleTypeRepr::createEmpty(Context, tyR->getSourceRange());
      } else {
        diagnose(tyR->getStartLoc(), diag::function_type_no_parens)
          .highlight(tyR->getSourceRange())
          .fixItInsert(tyR->getStartLoc(), "(")
          .fixItInsertAfter(tyR->getEndLoc(), ")");
        argsTyR = TupleTypeRepr::create(Context, {tyR},
                                        tyR->getSourceRange());
      }
    }
    
    // Parse substitutions for substituted SIL types.
    MutableArrayRef<TypeRepr *> invocationSubsTypes;
    MutableArrayRef<TypeRepr *> patternSubsTypes;
    if (isInSILMode()) {
      auto parseSubstitutions =
          [&](MutableArrayRef<TypeRepr *> &subs) -> llvm::Optional<bool> {
        if (!consumeIf(tok::kw_for))
          return llvm::None;

        if (!startsWithLess(Tok)) {
          diagnose(Tok, diag::sil_function_subst_expected_l_angle);
          return false;
        }

        consumeStartingLess();

        SmallVector<TypeRepr*, 4> SubsTypesVec;
        for (;;) {
          auto argTy = parseType();
          if (!argTy.getPtrOrNull())
            return false;
          SubsTypesVec.push_back(argTy.get());
          if (!consumeIf(tok::comma))
            break;
        }
        if (!startsWithGreater(Tok)) {
          diagnose(Tok, diag::sil_function_subst_expected_r_angle);
          return false;
        }
        consumeStartingGreater();

        subs = Context.AllocateCopy(SubsTypesVec);
        return true;
      };

      // Parse pattern substitutions.  These must exist if we had pattern
      // generics above.
      if (patternGenerics) {
        auto result = parseSubstitutions(patternSubsTypes);
        if (!result || patternSubsTypes.empty()) {
          diagnose(Tok, diag::sil_function_subst_expected_subs);
          patternGenerics = nullptr;
        } else if (!*result) {
          return makeParserError();
        }
      }

      if (generics) {
        if (auto result = parseSubstitutions(invocationSubsTypes))
          if (!*result) return makeParserError();
      }

      if (Tok.is(tok::kw_for)) {
        diagnose(Tok, diag::sil_function_subs_without_generics);
        return makeParserError();
      }
    }

    tyR = new (Context) FunctionTypeRepr(generics, argsTyR, asyncLoc, throwsLoc,
                                         arrowLoc, SecondHalf.get(),
                                         patternGenerics, patternSubsTypes,
                                         invocationSubsTypes);
  } else if (auto firstGenerics = generics ? generics : patternGenerics) {
    // Only function types may be generic.
    auto brackets = firstGenerics->getSourceRange();
    diagnose(brackets.Start, diag::generic_non_function);

    // Forget any generic parameters we saw in the type.
    class EraseTypeParamWalker : public ASTWalker {
    public:
      MacroWalking getMacroWalkingBehavior() const override {
        return MacroWalking::Arguments;
      }

      PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
        if (auto ident = dyn_cast<IdentTypeRepr>(T)) {
          if (auto decl = ident->getBoundDecl()) {
            if (auto genericParam = dyn_cast<GenericTypeParamDecl>(decl))
              ident->overwriteNameRef(genericParam->createNameRef());
          }
        }
        return Action::Continue();
      }

    } walker;

    if (tyR)
      tyR->walk(walker);
  }

  return makeParserResult(
      status,
      applyAttributeToType(tyR, attrs, specifier, specifierLoc, isolatedLoc,
                           constLoc));
}

/// Build a TypeRepr for AST node for the type at the given source location in the specified file.
///
/// \param sourceLoc The source location at which to start processing a type.
/// \param endSourceLoc Will receive the source location immediately following the type.
extern "C" TypeRepr *swift_ASTGen_buildTypeRepr(
    void *sourceFile, const void *_Nullable sourceLoc,
    void *declContext, void *astContext, const void *_Nullable *endSourceLoc);

/// parseType
///   type:
///     type-scalar
///     pack-expansion-type
///
///   pack-expansion-type:
///     type-scalar '...'
///
ParserResult<TypeRepr> Parser::parseType(
    Diag<> MessageID, ParseTypeReason reason) {
  #if SWIFT_SWIFT_PARSER
  auto astGenResult = parseASTFromSyntaxTree<TypeRepr>(
      [&](void *exportedSourceFile, const void *sourceLoc) {
        const void *endLocPtr = nullptr;
        TypeRepr *typeRepr = swift_ASTGen_buildTypeRepr(
            exportedSourceFile, Tok.getLoc().getOpaquePointerValue(),
            CurDeclContext, &Context, &endLocPtr);
        return std::make_pair(typeRepr, endLocPtr);
      });
  if (astGenResult.isNonNull()) {
    // Note: there is a representational difference between the swift-syntax
    // tree and the C++ parser tree regarding variadic parameters. In the
    // swift-syntax tree, the ellipsis is part of the parameter declaration.
    // In the C++ parser tree, the ellipsis is part of the type. Account for
    // this difference by consuming the ellipsis here.
    if (Tok.isEllipsis()) {
      Tok.setKind(tok::ellipsis);
      SourceLoc ellipsisLoc = consumeToken();
      return makeParserResult(astGenResult,
          new (Context) VarargTypeRepr(astGenResult.get(), ellipsisLoc));
    }

    return astGenResult;
  }
  #endif

  // Parse pack expansion 'repeat T'
  if (Tok.is(tok::kw_repeat)) {
    SourceLoc repeatLoc = consumeToken(tok::kw_repeat);

    auto ty = parseTypeScalar(MessageID, reason);
    if (ty.isNull())
      return ty;

    return makeParserResult(ty,
        new (Context) PackExpansionTypeRepr(repeatLoc, ty.get()));
  }

  auto ty = parseTypeScalar(MessageID, reason);
  if (ty.isNull())
    return ty;

  // Parse vararg type 'T...'.
  if (Tok.isEllipsis()) {
    Tok.setKind(tok::ellipsis);
    SourceLoc ellipsisLoc = consumeToken();
    ty = makeParserResult(ty,
        new (Context) VarargTypeRepr(ty.get(), ellipsisLoc));
  }

  return ty;
}

ParserResult<TypeRepr> Parser::parseTypeWithOpaqueParams(Diag<> MessageID) {
  GenericParamList *genericParams = nullptr;
  if (Context.LangOpts.hasFeature(Feature::NamedOpaqueTypes)) {
    auto result = maybeParseGenericParams();
    genericParams = result.getPtrOrNull();
    if (result.hasCodeCompletion())
      return makeParserCodeCompletionStatus();
  }

  auto typeResult = parseType(MessageID);
  if (auto type = typeResult.getPtrOrNull()) {
    return makeParserResult(
        ParserStatus(typeResult),
        genericParams ? new (Context)
                            NamedOpaqueReturnTypeRepr(type, genericParams)
                      : type);
  } else {
    return typeResult;
  }
}

ParserResult<TypeRepr> Parser::parseDeclResultType(Diag<> MessageID) {
  if (Tok.is(tok::code_complete)) {
    if (CodeCompletionCallbacks) {
      CodeCompletionCallbacks->completeTypeDeclResultBeginning();
    }
    consumeToken(tok::code_complete);
    return makeParserCodeCompletionStatus();
  }

  auto result = parseTypeWithOpaqueParams(MessageID);

  if (!result.isParseErrorOrHasCompletion()) {
    if (Tok.is(tok::r_square)) {
      auto diag = diagnose(Tok, diag::extra_rbracket);
      diag.fixItInsert(result.get()->getStartLoc(), getTokenText(tok::l_square));
      consumeToken();
      return makeParserErrorResult(new (Context)
                                       ErrorTypeRepr(getTypeErrorLoc()));
    }

    if (Tok.is(tok::colon)) {
      auto colonTok = consumeToken();
      auto secondType = parseType(diag::expected_dictionary_value_type);

      auto diag = diagnose(colonTok, diag::extra_colon);
      diag.fixItInsert(result.get()->getStartLoc(), getTokenText(tok::l_square));
      if (!secondType.isParseErrorOrHasCompletion()) {
        if (Tok.is(tok::r_square)) {
          consumeToken();
        } else {
          diag.fixItInsertAfter(secondType.get()->getEndLoc(), getTokenText(tok::r_square));
        }
      }
      return makeParserErrorResult(new (Context)
                                       ErrorTypeRepr(getTypeErrorLoc()));
    }
  }
  return result;
}

SourceLoc Parser::getTypeErrorLoc() const {
  // Use the same location as a missing close brace, etc.
  return getErrorOrMissingLoc();
}

ParserStatus Parser::parseGenericArguments(SmallVectorImpl<TypeRepr *> &Args,
                                           SourceLoc &LAngleLoc,
                                           SourceLoc &RAngleLoc) {
  // Parse the opening '<'.
  assert(startsWithLess(Tok) && "Generic parameter list must start with '<'");
  LAngleLoc = consumeStartingLess();

  // Allow an empty generic parameter list, since this is meaningful with
  // variadic generic types.
  if (!startsWithGreater(Tok)) {
    while (true) {
      ParserResult<TypeRepr> Ty = parseType(diag::expected_type);
      if (Ty.isNull() || Ty.hasCodeCompletion()) {
        // Skip until we hit the '>'.
        RAngleLoc = skipUntilGreaterInTypeList();
        return ParserStatus(Ty);
      }

      Args.push_back(Ty.get());
      // Parse the comma, if the list continues.
      if (!consumeIf(tok::comma))
        break;
    }
  }

  if (!startsWithGreater(Tok)) {
    checkForInputIncomplete();
    diagnose(Tok, diag::expected_rangle_generic_arg_list);
    diagnose(LAngleLoc, diag::opening_angle);

    // Skip until we hit the '>'.
    RAngleLoc = skipUntilGreaterInTypeList();
    return makeParserError();
  } else {
    RAngleLoc = consumeStartingGreater();
  }

  return makeParserSuccess();
}

ParserResult<TypeRepr> Parser::parseQualifiedDeclNameBaseType() {
  if (!canParseBaseTypeForQualifiedDeclName())
    return makeParserError();

  if (Tok.isNot(tok::identifier) && Tok.isNot(tok::kw_Self)) {
    // is this the 'Any' type
    if (Tok.is(tok::kw_Any)) {
      return parseAnyType();
    } else if (Tok.is(tok::code_complete)) {
      if (CodeCompletionCallbacks) {
        CodeCompletionCallbacks->completeTypeSimpleBeginning();
      }
      // Eat the code completion token because we handled it.
      consumeToken(tok::code_complete);
      return makeParserCodeCompletionResult<DeclRefTypeRepr>();
    }

    diagnose(Tok, diag::expected_identifier_for_type);

    // If there is a keyword at the start of a new line, we won't want to
    // skip it as a recovery but rather keep it.
    if (Tok.isKeyword() && !Tok.isAtStartOfLine())
      consumeToken();

    return nullptr;
  }

  ParserStatus Status;
  SmallVector<IdentTypeRepr *, 4> ComponentsR;
  SourceLoc EndLoc;
  while (true) {
    auto IdentResult = parseTypeIdentifier();
    if (IdentResult.isParseErrorOrHasCompletion())
      return IdentResult;

    ComponentsR.push_back(cast<IdentTypeRepr>(IdentResult.get()));

    // Treat 'Foo.<anything>' as an attempt to write a dotted type
    // unless <anything> is 'Type'.
    if ((Tok.is(tok::period) || Tok.is(tok::period_prefix))) {
      if (peekToken().is(tok::code_complete)) {
        Status.setHasCodeCompletionAndIsError();
        break;
      }
      if (peekToken().isContextualKeyword("Type") ||
          peekToken().isContextualKeyword("Protocol"))
        break;

      // Break before parsing the period before the final declaration
      // name component.
      {
        // If qualified name base type cannot be parsed from the current
        // point (i.e. the next type identifier is not followed by a '.'),
        // then the next identifier is the final declaration name component.
        BacktrackingScope backtrack(*this);
        consumeStartingCharacterOfCurrentToken(tok::period);
        if (!canParseBaseTypeForQualifiedDeclName())
          break;
      }
      // Consume the period.
      consumeToken();
      continue;
    }
    if (Tok.is(tok::code_complete) && !Tok.isAtStartOfLine())
      Status.setHasCodeCompletionAndIsError();
    break;
  }

  DeclRefTypeRepr *DeclRefTR = nullptr;
  if (!ComponentsR.empty()) {
    DeclRefTR = MemberTypeRepr::create(Context, ComponentsR);
  }

  if (Status.hasCodeCompletion()) {
    if (Tok.isNot(tok::code_complete)) {
      // We have a dot.
      consumeToken();
      if (CodeCompletionCallbacks) {
        CodeCompletionCallbacks->completeTypeSimpleWithDot(DeclRefTR);
      }
    } else {
      if (CodeCompletionCallbacks) {
        CodeCompletionCallbacks->completeTypeSimpleWithoutDot(DeclRefTR);
      }
    }
    // Eat the code completion token because we handled it.
    consumeToken(tok::code_complete);
  }

  return makeParserResult(Status, DeclRefTR);
}

ParserResult<IdentTypeRepr> Parser::parseTypeIdentifier() {
  // FIXME: We should parse e.g. 'X.var'. Almost any keyword is a valid member
  // component.
  DeclNameLoc Loc;
  DeclNameRef Name =
      parseDeclNameRef(Loc, diag::expected_identifier_in_dotted_type,
                       DeclNameFlag::AllowLowercaseAndUppercaseSelf);
  if (!Name)
    return makeParserError();

  ParserStatus Status;
  IdentTypeRepr *IdTR;

  if (startsWithLess(Tok)) {
    SourceLoc LAngle, RAngle;
    SmallVector<TypeRepr *, 8> GenericArgs;
    auto ArgsStatus = parseGenericArguments(GenericArgs, LAngle, RAngle);
    if (ArgsStatus.isErrorOrHasCompletion())
      return ArgsStatus;

    IdTR = GenericIdentTypeRepr::create(Context, Loc, Name, GenericArgs,
                                        SourceRange(LAngle, RAngle));
  } else {
    IdTR = new (Context) SimpleIdentTypeRepr(Loc, Name);
  }

  return makeParserResult(IdTR);
}

ParserResult<TypeRepr> Parser::parseTypeDotted(ParserResult<TypeRepr> Base) {
  assert(Base.isNonNull());
  assert(Tok.isAny(tok::period, tok::period_prefix));

  SmallVector<IdentTypeRepr *, 4> MemberComponents;

  while (Tok.isAny(tok::period, tok::period_prefix)) {
    if (peekToken().is(tok::code_complete)) {
      // Code completion for "type-simple '.'" is handled in 'parseTypeSimple'.
      break;
    }

    // Consume the period.
    consumeToken();

    if (Tok.isContextualKeyword("Type") ||
        Tok.isContextualKeyword("Protocol")) {
      TypeRepr *MetaBase =
          MemberTypeRepr::create(Context, Base.get(), MemberComponents);
      if (Tok.getRawText() == "Type") {
        Base = makeParserResult(Base,
                                new (Context) MetatypeTypeRepr(
                                    MetaBase, consumeToken(tok::identifier)));
      } else {
        Base = makeParserResult(Base,
                                new (Context) ProtocolTypeRepr(
                                    MetaBase, consumeToken(tok::identifier)));
      }

      // Start anew with a metatype base.
      MemberComponents.clear();
      continue;
    }

    auto IdentResult = parseTypeIdentifier();
    if (IdentResult.isParseErrorOrHasCompletion())
      return IdentResult | ParserStatus(Base);

    MemberComponents.push_back(cast<IdentTypeRepr>(IdentResult.get()));
  }

  return makeParserResult(
      Base, MemberTypeRepr::create(Context, Base.get(), MemberComponents));
}

/// parseTypeSimpleOrComposition
///
///   type-composition:
///     'some'? type-simple
///     'any'? type-simple
///     type-composition '&' type-simple
ParserResult<TypeRepr>
Parser::parseTypeSimpleOrComposition(Diag<> MessageID, ParseTypeReason reason) {
  // Check for the contextual keyword modifiers on types.
  // These are only semantically allowed in certain contexts, but we parse it
  // generally for diagnostics and recovery.
  SourceLoc opaqueLoc;
  SourceLoc anyLoc;
  if (Tok.isContextualKeyword("some")) {
    // Treat some as a keyword.
    TokReceiver->registerTokenKindChange(Tok.getLoc(), tok::contextual_keyword);
    opaqueLoc = consumeToken();
  } else if (Tok.isContextualKeyword("any")) {
    // Treat any as a keyword.
    TokReceiver->registerTokenKindChange(Tok.getLoc(), tok::contextual_keyword);
    anyLoc = consumeToken();
  } else if (Tok.isContextualKeyword("each")) {
    // Treat 'each' as a keyword.
    TokReceiver->registerTokenKindChange(Tok.getLoc(), tok::contextual_keyword);
    SourceLoc eachLoc = consumeToken();
    ParserResult<TypeRepr> packElt = parseTypeSimple(MessageID, reason);
    if (packElt.isNull())
      return packElt;

    auto *typeRepr = new (Context) PackElementTypeRepr(eachLoc, packElt.get());
    return makeParserResult(ParserStatus(packElt), typeRepr);
  }

  auto applyOpaque = [&](TypeRepr *type) -> TypeRepr * {
    if (opaqueLoc.isValid() &&
        (anyLoc.isInvalid() || SourceMgr.isBeforeInBuffer(opaqueLoc, anyLoc))) {
      type = new (Context) OpaqueReturnTypeRepr(opaqueLoc, type);
    } else if (anyLoc.isValid()) {
      type = new (Context) ExistentialTypeRepr(anyLoc, type);
    }
    return type;
  };
  
  // Parse the first type
  ParserResult<TypeRepr> FirstType = parseTypeSimple(MessageID, reason);
  if (FirstType.isNull())
    return FirstType;
  if (!Tok.isContextualPunctuator("&")) {
    return makeParserResult(ParserStatus(FirstType),
                            applyOpaque(FirstType.get()));
  }

  SmallVector<TypeRepr *, 4> Types;
  ParserStatus Status(FirstType);
  SourceLoc FirstTypeLoc = FirstType.get()->getStartLoc();
  SourceLoc FirstAmpersandLoc = Tok.getLoc();

  auto addType = [&](TypeRepr *T) {
    if (!T) return;
    if (auto Comp = dyn_cast<CompositionTypeRepr>(T)) {
      // Accept protocol<P1, P2> & P3; explode it.
      auto TyRs = Comp->getTypes();
      if (!TyRs.empty()) // If empty, is 'Any'; ignore.
        Types.append(TyRs.begin(), TyRs.end());
      return;
    }
    Types.push_back(T);
  };

  addType(FirstType.get());
  assert(Tok.isContextualPunctuator("&"));
  do {
    consumeToken(); // consume '&'

    // Diagnose invalid `some` or `any` after an ampersand.
    if (Tok.isContextualKeyword("some") ||
        Tok.isContextualKeyword("any")) {
      auto keyword = Tok.getText();
      auto badLoc = consumeToken();
                
      // Suggest moving `some` or `any` in front of the first type unless
      // the first type is an opaque or existential type.
      if (opaqueLoc.isValid() || anyLoc.isValid()) {
        diagnose(badLoc, diag::opaque_mid_composition, keyword)
            .fixItRemove(badLoc);
      } else {
        diagnose(badLoc, diag::opaque_mid_composition, keyword)
            .fixItRemove(badLoc)
            .fixItInsert(FirstTypeLoc, keyword.str() + " ");
      }

      const bool isAnyKeyword = keyword.equals("any");

      if (isAnyKeyword) {
        if (anyLoc.isInvalid()) {
          anyLoc = badLoc;
        }
      } else if (opaqueLoc.isInvalid()) {
        opaqueLoc = badLoc;
      }
    }

    // Parse next type.
    ParserResult<TypeRepr> ty =
      parseTypeSimple(diag::expected_identifier_for_type, reason);
    if (ty.hasCodeCompletion())
      return makeParserCodeCompletionResult<TypeRepr>();
    Status |= ty;
    addType(ty.getPtrOrNull());
  } while (Tok.isContextualPunctuator("&"));

  return makeParserResult(Status, applyOpaque(CompositionTypeRepr::create(
    Context, Types, FirstTypeLoc, {FirstAmpersandLoc, PreviousLoc})));
}

ParserResult<TypeRepr> Parser::parseAnyType() {
  auto Loc = consumeToken(tok::kw_Any);
  auto TyR = CompositionTypeRepr::createEmptyComposition(Context, Loc);
  return makeParserResult(TyR);
}

/// parseOldStyleProtocolComposition
///   type-composition-deprecated:
///     'protocol' '<' '>'
///     'protocol' '<' type-composition-list-deprecated '>'
///
///   type-composition-list-deprecated:
///     type-identifier
///     type-composition-list-deprecated ',' type-identifier
ParserResult<TypeRepr> Parser::parseOldStyleProtocolComposition() {
  assert(Tok.is(tok::kw_protocol) && startsWithLess(peekToken()));

  SourceLoc ProtocolLoc = consumeToken();
  SourceLoc LAngleLoc = consumeStartingLess();

  // Parse the type-composition-list.
  ParserStatus Status;
  SmallVector<TypeRepr *, 4> Components;
  bool IsEmpty = startsWithGreater(Tok);
  if (!IsEmpty) {
    do {
      // Parse the type.
      ParserResult<TypeRepr> TR =
          parseTypeSimple(diag::expected_type, ParseTypeReason::Unspecified);
      Status |= TR;

      if (TR.isNonNull())
        Components.push_back(TR.get());
    } while (consumeIf(tok::comma));
  }

  // Check for the terminating '>'.
  SourceLoc RAngleLoc = PreviousLoc;
  if (startsWithGreater(Tok)) {
    RAngleLoc = consumeStartingGreater();
  } else {
    if (Status.isSuccess() && !Status.hasCodeCompletion()) {
      diagnose(Tok, diag::expected_rangle_protocol);
      diagnose(LAngleLoc, diag::opening_angle);
      Status.setIsParseError();
    }
    
    // Skip until we hit the '>'.
    RAngleLoc = skipUntilGreaterInTypeList(/*protocolComposition=*/true);
  }

  auto composition = CompositionTypeRepr::create(
    Context, Components, ProtocolLoc, {LAngleLoc, RAngleLoc});

  if (Status.isSuccess() && !Status.hasCodeCompletion()) {
    // Only if we have complete protocol<...> construct, diagnose deprecated.
    SmallString<32> replacement;
    if (Components.empty()) {
      replacement = "Any";
    } else {
      auto extractText = [&](TypeRepr *Ty) -> StringRef {
        auto SourceRange = Ty->getSourceRange();
        return SourceMgr.extractText(
          Lexer::getCharSourceRangeFromSourceRange(SourceMgr, SourceRange));
      };
      auto Begin = Components.begin();
      replacement += extractText(*Begin);
      while (++Begin != Components.end()) {
        replacement += " & ";
        replacement += extractText(*Begin);
      }
    }

    if (Components.size() > 1) {
      // Need parenthesis if the next token looks like postfix TypeRepr.
      // i.e. '?', '!', '.Type', '.Protocol'
      bool needParen = false;
      needParen |= !Tok.isAtStartOfLine() &&
          (isOptionalToken(Tok) || isImplicitlyUnwrappedOptionalToken(Tok));
      needParen |= Tok.isAny(tok::period, tok::period_prefix);
      if (needParen) {
        replacement.insert(replacement.begin(), '(');
        replacement += ")";
      }
    }

    // Copy split token after '>' to the replacement string.
    // FIXME: lexer should smartly separate '>' and trailing contents like '?'.
    StringRef TrailingContent = L->getTokenAt(RAngleLoc).getRange().str().
      substr(1);
    if (!TrailingContent.empty()) {
      replacement += TrailingContent;
    }

    // Replace 'protocol<T1, T2>' with 'T1 & T2'
    diagnose(ProtocolLoc,
      IsEmpty               ? diag::deprecated_any_composition :
      Components.size() > 1 ? diag::deprecated_protocol_composition :
                              diag::deprecated_protocol_composition_single)
      .highlight(composition->getSourceRange())
      .fixItReplace(composition->getSourceRange(), replacement);
  }

  return makeParserResult(Status, composition);
}

/// FIXME: This is an egregious hack.
static bool isMacroSignatureFile(SourceFile &sf) {
  return sf.getFilename().startswith("Macro signature of");
}

/// parseTypeTupleBody
///   type-tuple:
///     '(' type-tuple-body? ')'
///   type-tuple-body:
///     type-tuple-element (',' type-tuple-element)*
///   type-tuple-element:
///     identifier? identifier ':' type
///     type
ParserResult<TypeRepr> Parser::parseTypeTupleBody() {
  Parser::StructureMarkerRAII ParsingTypeTuple(*this, Tok);

  SourceLoc RPLoc, LPLoc = consumeToken(tok::l_paren);
  SmallVector<TupleTypeReprElement, 8> ElementsR;

  ParserStatus Status = parseList(tok::r_paren, LPLoc, RPLoc,
                                  /*AllowSepAfterLast=*/false,
                                  diag::expected_rparen_tuple_type_list,
                                  [&] () -> ParserStatus {
    TupleTypeReprElement element;

    // 'inout' here can be a obsoleted use of the marker in an argument list,
    // consume it in backtracking context so we can determine it's really a
    // deprecated use of it.
    llvm::Optional<CancellableBacktrackingScope> Backtracking;
    SourceLoc ObsoletedInOutLoc;
    if (Tok.is(tok::kw_inout)) {
      Backtracking.emplace(*this);
      ObsoletedInOutLoc = consumeToken(tok::kw_inout);
    }

    // If the tuple element starts with a potential argument label followed by a
    // ':' or another potential argument label, then the identifier is an
    // element tag, and it is followed by a type annotation.
    if (startsParameterName(false)) {
      // Consume a name.
      element.NameLoc = consumeArgumentLabel(element.Name,
                                             /*diagnoseDollarPrefix=*/true);

      // If there is a second name, consume it as well.
      if (Tok.canBeArgumentLabel())
        element.SecondNameLoc = consumeArgumentLabel(element.SecondName,
                                                     /*diagnoseDollarPrefix=*/true);

      // Consume the ':'.
      if (consumeIf(tok::colon, element.ColonLoc)) {
        // If we succeed, then we successfully parsed a label.
        if (Backtracking)
          Backtracking->cancelBacktrack();
      // Otherwise, if we can't backtrack to parse this as a type,
      // this is a syntax error.
      } else {
        if (!Backtracking) {
          diagnose(Tok, diag::expected_parameter_colon);
        }
        element.NameLoc = SourceLoc();
        element.SecondNameLoc = SourceLoc();
      }

    } else if (Backtracking) {
      // If we don't have labels, 'inout' is not a obsoleted use.
      ObsoletedInOutLoc = SourceLoc();
    }
    Backtracking.reset();

    // Parse the type annotation.
    auto type = parseType(diag::expected_type);
    if (type.hasCodeCompletion())
      return makeParserCodeCompletionStatus();
    if (type.isNull())
      return makeParserError();
    element.Type = type.get();

    // Complain obsoleted 'inout' etc. position; (inout name: Ty)
    if (ObsoletedInOutLoc.isValid()) {
      if (isa<SpecifierTypeRepr>(element.Type)) {
        // If the parsed type is already a inout type et al, just remove it.
        diagnose(Tok, diag::parameter_specifier_repeated)
            .fixItRemove(ObsoletedInOutLoc);
      } else {
        diagnose(ObsoletedInOutLoc,
                 diag::parameter_specifier_as_attr_disallowed, "inout")
            .fixItRemove(ObsoletedInOutLoc)
            .fixItInsert(element.Type->getStartLoc(), "inout ");
        // Build inout type. Note that we bury the inout locator within the
        // named locator. This is weird but required by Sema apparently.
        element.Type =
            new (Context) OwnershipTypeRepr(element.Type,
                                            ParamSpecifier::InOut,
                                            ObsoletedInOutLoc);
      }
    }

    // Parse '= expr' here so we can complain about it directly, rather
    // than dying when we see it.
    if (Tok.is(tok::equal)) {
      SourceLoc equalLoc = consumeToken(tok::equal);
      auto init = parseExpr(diag::expected_init_value);
      auto inFlight = diagnose(equalLoc, diag::tuple_type_init);
      if (init.isNonNull())
        inFlight.fixItRemove(SourceRange(equalLoc, init.get()->getEndLoc()));
    }

    // Record the ',' location.
    if (Tok.is(tok::comma))
      element.TrailingCommaLoc = Tok.getLoc();

    ElementsR.push_back(element);
    return makeParserSuccess();
  });

  bool isFunctionType =
      Tok.isAny(tok::arrow, tok::kw_throws, tok::kw_rethrows) ||
      Tok.isContextualKeyword("async");

  // If there were any labels, figure out which labels should go into the type
  // representation.
  for (auto &element : ElementsR) {
    // True tuples have labels.
    if (!isFunctionType) {
      // If there were two names, complain.
      if (element.NameLoc.isValid() && element.SecondNameLoc.isValid()) {
        auto diag = diagnose(element.NameLoc, diag::tuple_type_multiple_labels);
        if (element.Name.empty()) {
          diag.fixItRemoveChars(element.NameLoc,
                                element.Type->getStartLoc());
        } else {
          diag.fixItRemove(
            SourceRange(Lexer::getLocForEndOfToken(SourceMgr, element.NameLoc),
                        element.SecondNameLoc));
        }
      }
      continue;
    }

    // If there was a first name, complain; arguments in function types are
    // always unlabeled.
    if (element.NameLoc.isValid() && !element.Name.empty() &&
        /*FIXME: Gross hack*/!isMacroSignatureFile(SF)) {
      auto diag = diagnose(element.NameLoc, diag::function_type_argument_label,
                           element.Name);
      if (element.SecondNameLoc.isInvalid())
        diag.fixItInsert(element.NameLoc, "_ ");
      else if (element.SecondName.empty())
        diag.fixItRemoveChars(element.NameLoc,
                              element.Type->getStartLoc());
      else
        diag.fixItReplace(SourceRange(element.NameLoc), "_");
    }

    if (element.SecondNameLoc.isValid()) {
      // Form the named parameter type representation.
      element.UnderscoreLoc = element.NameLoc;
      element.Name = element.SecondName;
      element.NameLoc = element.SecondNameLoc;
    }
  }

  return makeParserResult(Status,
                          TupleTypeRepr::create(Context, ElementsR,
                                                SourceRange(LPLoc, RPLoc)));
}


/// parseTypeArray - Parse the type-array production, given that we
/// are looking at the initial l_square.  Note that this index
/// clause is actually the outermost (first-indexed) clause.
///
///   type-array:
///     type-simple
///     type-array '[' ']'
///     type-array '[' expr ']'
///
ParserResult<TypeRepr> Parser::parseTypeArray(ParserResult<TypeRepr> Base) {
  assert(Tok.isFollowingLSquare());
  Parser::StructureMarkerRAII ParsingArrayBound(*this, Tok);
  SourceLoc lsquareLoc = consumeToken();

  // Handle a postfix [] production, a common typo for a C-like array.

  // If we have something that might be an array size expression, parse it as
  // such, for better error recovery.
  if (Tok.isNot(tok::r_square)) {
    auto sizeEx = parseExprBasic(diag::expected_expr);
    if (sizeEx.hasCodeCompletion())
      return makeParserCodeCompletionStatus();
  }
  
  SourceLoc rsquareLoc;
  if (parseMatchingToken(tok::r_square, rsquareLoc,
                         diag::expected_rbracket_array_type, lsquareLoc)) {
    Base.setIsParseError();
    return Base;
  }

  auto baseTyR = Base.get();

  // If we parsed something valid, diagnose it with a fixit to rewrite it to
  // Swift syntax.
  diagnose(lsquareLoc, diag::new_array_syntax)
    .fixItInsert(baseTyR->getStartLoc(), "[")
    .fixItRemove(lsquareLoc);
  
  // Build a normal array slice type for recovery.
  ArrayTypeRepr *ATR = new (Context) ArrayTypeRepr(
      baseTyR, SourceRange(baseTyR->getStartLoc(), rsquareLoc));
  return makeParserResult(ParserStatus(Base), ATR);
}

ParserResult<TypeRepr> Parser::parseTypeCollection() {
  ParserStatus Status;
  // Parse the leading '['.
  assert(Tok.is(tok::l_square));
  Parser::StructureMarkerRAII parsingCollection(*this, Tok);
  SourceLoc lsquareLoc = consumeToken();

  // Parse the element type.
  ParserResult<TypeRepr> firstTy = parseType(diag::expected_element_type);
  Status |= firstTy;

  // If there is a ':', this is a dictionary type.
  SourceLoc colonLoc;
  ParserResult<TypeRepr> secondTy;
  if (Tok.is(tok::colon)) {
    colonLoc = consumeToken();

    // Parse the second type.
    secondTy = parseType(diag::expected_dictionary_value_type);
    Status |= secondTy;
  }

  // Parse the closing ']'.
  SourceLoc rsquareLoc;
  if (parseMatchingToken(tok::r_square, rsquareLoc,
                         colonLoc.isValid()
                             ? diag::expected_rbracket_dictionary_type
                             : diag::expected_rbracket_array_type,
                         lsquareLoc))
    Status.setIsParseError();

  if (Status.hasCodeCompletion())
    return Status;

  // If we couldn't parse anything for one of the types, propagate the error.
  if (Status.isErrorOrHasCompletion())
    return makeParserError();

  TypeRepr *TyR;

  SourceRange brackets(lsquareLoc, rsquareLoc);
  if (colonLoc.isValid()) {
    // Form the dictionary type.
    TyR = new (Context)
        DictionaryTypeRepr(firstTy.get(), secondTy.get(), colonLoc, brackets);
  } else {
    // Form the array type.
    TyR = new (Context) ArrayTypeRepr(firstTy.get(), brackets);
  }
    
  return makeParserResult(Status, TyR);
}

bool Parser::isOptionalToken(const Token &T) const {
  // A postfix '?' by itself is obviously optional.
  if (T.is(tok::question_postfix))
    return true;
  // A postfix or bound infix operator token that begins with '?' can be
  // optional too.
  if (T.is(tok::oper_postfix) || T.is(tok::oper_binary_unspaced)) {
    // We'll munch off the '?', so long as it is left-bound with
    // the type (i.e., parsed as a postfix or unspaced binary operator).
    return T.getText().startswith("?");
  }

  return false;
}

bool Parser::isImplicitlyUnwrappedOptionalToken(const Token &T) const {
  // A postfix '!' by itself, or a '!' in SIL mode, is obviously implicitly
  // unwrapped optional.
  if (T.is(tok::exclaim_postfix) || T.is(tok::sil_exclamation))
    return true;
  // A postfix or bound infix operator token that begins with '!' can be
  // implicitly unwrapped optional too.
  if (T.is(tok::oper_postfix) || T.is(tok::oper_binary_unspaced)) {
    // We'll munch off the '!', so long as it is left-bound with
    // the type (i.e., parsed as a postfix or unspaced binary operator).
    return T.getText().startswith("!");
  }

  return false;
}

SourceLoc Parser::consumeOptionalToken() {
  assert(isOptionalToken(Tok) && "not a '?' token?!");
  return consumeStartingCharacterOfCurrentToken(tok::question_postfix);
}

SourceLoc Parser::consumeImplicitlyUnwrappedOptionalToken() {
  assert(isImplicitlyUnwrappedOptionalToken(Tok) && "not a '!' token?!");
  // If the text of the token is just '!', grab the next token.
  return consumeStartingCharacterOfCurrentToken(tok::exclaim_postfix);
}

/// Parse a single optional suffix, given that we are looking at the
/// question mark.
ParserResult<TypeRepr>
Parser::parseTypeOptional(ParserResult<TypeRepr> base) {
  SourceLoc questionLoc = consumeOptionalToken();
  auto TyR = new (Context) OptionalTypeRepr(base.get(), questionLoc);
  return makeParserResult(ParserStatus(base), TyR);
}

/// Parse a single implicitly unwrapped optional suffix, given that we
/// are looking at the exclamation mark.
ParserResult<TypeRepr>
Parser::parseTypeImplicitlyUnwrappedOptional(ParserResult<TypeRepr> base) {
  SourceLoc exclamationLoc = consumeImplicitlyUnwrappedOptionalToken();
  auto TyR =
      new (Context) ImplicitlyUnwrappedOptionalTypeRepr(base.get(), exclamationLoc);
  return makeParserResult(ParserStatus(base), TyR);
}

//===----------------------------------------------------------------------===//
// Speculative type list parsing
//===----------------------------------------------------------------------===//

static bool isGenericTypeDisambiguatingToken(Parser &P) {
  auto &tok = P.Tok;
  switch (tok.getKind()) {
  default:
    return false;
  case tok::r_paren:
  case tok::r_square:
  case tok::l_brace:
  case tok::r_brace:
  case tok::period:
  case tok::period_prefix:
  case tok::comma:
  case tok::semi:
  case tok::eof:
  case tok::code_complete:
  case tok::exclaim_postfix:
  case tok::question_postfix:
  case tok::colon:
    return true;

  case tok::oper_binary_spaced:
    if (tok.getText() == "&")
      return true;

    LLVM_FALLTHROUGH;
  case tok::oper_binary_unspaced:
  case tok::oper_postfix:
    // These might be '?' or '!' type modifiers.
    return P.isOptionalToken(tok) || P.isImplicitlyUnwrappedOptionalToken(tok);

  case tok::l_paren:
  case tok::l_square:
    // These only apply to the generic type if they don't start a new line.
    return !tok.isAtStartOfLine();
  }
}

bool Parser::canParseAsGenericArgumentList() {
  if (!Tok.isAnyOperator() || !Tok.getText().equals("<"))
    return false;

  BacktrackingScope backtrack(*this);

  if (canParseGenericArguments())
    return isGenericTypeDisambiguatingToken(*this);

  return false;
}

bool Parser::canParseGenericArguments() {
  // Parse the opening '<'.
  if (!startsWithLess(Tok))
    return false;
  consumeStartingLess();

  if (startsWithGreater(Tok)) {
    consumeStartingGreater();
    return true;
  }

  do {
    if (!canParseType())
      return false;
    // Parse the comma, if the list continues.
  } while (consumeIf(tok::comma));

  if (!startsWithGreater(Tok)) {
    return false;
  } else {
    consumeStartingGreater();
    return true;
  }
}

bool Parser::canParseType() {
  // 'repeat' starts a pack expansion type.
  consumeIf(tok::kw_repeat);

  // Accept 'inout' at for better recovery.
  consumeIf(tok::kw_inout);

  if (Tok.isContextualKeyword("some")) {
    consumeToken();
  } else if (Tok.isContextualKeyword("any")) {
    consumeToken();
  } else if (Tok.isContextualKeyword("each")) {
    consumeToken();
  }

  switch (Tok.getKind()) {
  case tok::kw_Self:
  case tok::kw_Any:
  case tok::identifier:
  case tok::code_complete:
    if (!canParseTypeIdentifier())
      return false;
    break;
  case tok::kw_protocol:
    return canParseOldStyleProtocolComposition();
  case tok::l_paren: {
    consumeToken();
    if (!canParseTypeTupleBody())
      return false;
    break;
  }
  case tok::at_sign: {
    consumeToken();
    if (!canParseTypeAttribute())
      return false;
    return canParseType();
  }
  case tok::l_square:
    consumeToken();
    if (!canParseType())
      return false;
    if (consumeIf(tok::colon)) {
      if (!canParseType())
        return false;
    }
    if (!consumeIf(tok::r_square))
      return false;
    break;
  case tok::kw__:
    consumeToken();
    break;


  default:
    return false;
  }

  // A member type, '.Type', '.Protocol', '?', and '!' still leave us with
  // type-simple.
  while (true) {
    if (Tok.isAny(tok::period_prefix, tok::period)) {
      consumeToken();

      if (Tok.isContextualKeyword("Type") ||
          Tok.isContextualKeyword("Protocol")) {
        consumeToken();
        continue;
      }

      if (canParseTypeIdentifier())
        continue;

      return false;
    }
    if (isOptionalToken(Tok)) {
      consumeOptionalToken();
      continue;
    }
    if (isImplicitlyUnwrappedOptionalToken(Tok)) {
      consumeImplicitlyUnwrappedOptionalToken();
      continue;
    }
    break;
  }

  while (Tok.isContextualPunctuator("&")) {
    consumeToken();
    // FIXME: Should be 'canParseTypeSimple', but we don't have one.
    if (!canParseType())
      return false;
  }

  if (isAtFunctionTypeArrow()) {
    // Handle type-function if we have an '->' with optional
    // 'async' and/or 'throws'.
    while (isEffectsSpecifier(Tok))
      consumeToken();

    if (!consumeIf(tok::arrow))
      return false;
    
    if (!canParseType())
      return false;
    
    return true;
  }

  // Parse pack expansion 'T...'.
  if (Tok.isEllipsis()) {
    Tok.setKind(tok::ellipsis);
    consumeToken();
  }

  return true;
}

bool Parser::canParseTypeIdentifier() {
  // Parse an identifier.
  //
  // FIXME: We should expect e.g. 'X.var'. Almost any keyword is a valid member component.
  if (!Tok.isAny(tok::identifier, tok::kw_Self, tok::kw_Any, tok::code_complete))
    return false;
  consumeToken();

  // Parse an optional generic argument list.
  if (startsWithLess(Tok) && !canParseGenericArguments())
    return false;

  return true;
}

bool Parser::canParseBaseTypeForQualifiedDeclName() {
  BacktrackingScope backtrack(*this);

  // Parse a simple type identifier.
  if (!canParseTypeIdentifier())
    return false;

  // Qualified name base types must be followed by a period.
  // If the next token starts with a period, return true.
  return startsWithSymbol(Tok, '.');
}

bool Parser::canParseOldStyleProtocolComposition() {
  consumeToken(tok::kw_protocol);
  
  // Check for the starting '<'.
  if (!startsWithLess(Tok)) {
    return false;
  }
  consumeStartingLess();
  
  // Check for empty protocol composition.
  if (startsWithGreater(Tok)) {
    consumeStartingGreater();
    return true;
  }
  
  // Parse the type-composition-list.
  do {
    if (!canParseType()) {
      return false;
    }
  } while (consumeIf(tok::comma));
  
  // Check for the terminating '>'.
  if (!startsWithGreater(Tok)) {
    return false;
  }
  consumeStartingGreater();
  
  return true;
}

bool Parser::canParseTypeTupleBody() {
  if (Tok.isNot(tok::r_paren) && Tok.isNot(tok::r_brace) &&
      Tok.isNotEllipsis() &&
      // In types, we do not allow for an inout binding to be declared in a
      // tuple type.
      (Tok.is(tok::kw_inout) || !isStartOfSwiftDecl())) {
    do {
      // The contextual inout marker is part of argument lists.
      consumeIf(tok::kw_inout);

      // If the tuple element starts with "ident :", then it is followed
      // by a type annotation.
      if (startsParameterName(/*isClosure=*/false)) {
        consumeToken();
        if (Tok.canBeArgumentLabel()) {
          consumeToken();
          if (!Tok.is(tok::colon)) return false;
        }
        consumeToken(tok::colon);

        // Parse a type.
        if (!canParseType())
          return false;

        // Parse default values. This aren't actually allowed, but we recover
        // better if we skip over them.
        if (consumeIf(tok::equal)) {
          while (Tok.isNot(tok::eof) && Tok.isNot(tok::r_paren) &&
                 Tok.isNot(tok::r_brace) && Tok.isNotEllipsis() &&
                 Tok.isNot(tok::comma) && !isStartOfSwiftDecl()) {
            skipSingle();
          }
        }

        continue;
      }
      
      // Otherwise, this has to be a type.
      if (!canParseType())
        return false;

    } while (consumeIf(tok::comma));
  }
  
  return consumeIf(tok::r_paren);
}

bool Parser::isAtFunctionTypeArrow() {
  if (Tok.is(tok::arrow))
    return true;

  if (isEffectsSpecifier(Tok)) {
    if (peekToken().is(tok::arrow))
      return true;
    if (isEffectsSpecifier(peekToken())) {
      BacktrackingScope backtrack(*this);
      consumeToken();
      consumeToken();
      return isAtFunctionTypeArrow();
    }
    // Don't look for '->' in code completion. The user may write it later.
    if (peekToken().is(tok::code_complete) && !peekToken().isAtStartOfLine())
      return true;

    return false;
  }

  // Don't look for '->' in code completion. The user may write it later.
  if (Tok.is(tok::code_complete) && !Tok.isAtStartOfLine())
    return true;

  return false;
}
