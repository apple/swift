//===--- ImportMacro.cpp - Import Clang preprocessor macros ---------------===//
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
// This file implements support for translating some kinds of C preprocessor
// macros into Swift declarations.
//
//===----------------------------------------------------------------------===//

#include "ImporterImpl.h"
#include "SwiftDeclSynthesizer.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsClangImporter.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "swift/Basic/PrettyStackTrace.h"
#include "swift/Basic/Unicode.h"
#include "swift/ClangImporter/ClangModule.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/DelayedDiagnostic.h"
#include "clang/Sema/Sema.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/APSIntType.h"
#include "llvm/ADT/SmallString.h"

using namespace swift;
using namespace importer;

template <typename T = clang::Expr>
static const T *
parseNumericLiteral(ClangImporter::Implementation &impl,
                    const clang::Token &tok) {
  auto result = impl.getClangSema().ActOnNumericConstant(tok);
  if (result.isUsable())
    return dyn_cast<T>(result.get());
  return nullptr;
}

// FIXME: Duplicated from ImportDecl.cpp.
static bool isInSystemModule(DeclContext *D) {
  return cast<ClangModuleUnit>(D->getModuleScopeContext())->isSystemModule();
}

static llvm::Optional<StringRef>
getTokenSpelling(ClangImporter::Implementation &impl, const clang::Token &tok) {
  bool tokenInvalid = false;
  llvm::SmallString<32> spellingBuffer;
  StringRef tokenSpelling = impl.getClangPreprocessor().getSpelling(
      tok, spellingBuffer, &tokenInvalid);
  if (tokenInvalid)
    return llvm::None;
  return tokenSpelling;
}

static ValueDecl *
createMacroConstant(ClangImporter::Implementation &Impl,
                    const clang::MacroInfo *macro,
                    Identifier name,
                    DeclContext *dc,
                    Type type,
                    const clang::APValue &value,
                    ConstantConvertKind convertKind,
                    bool isStatic,
                    ClangNode ClangN) {
  Impl.ImportedMacroConstants[macro] = {value, type};
  return SwiftDeclSynthesizer(Impl).createConstant(
      name, dc, type, value, convertKind, isStatic, ClangN);
}

static ValueDecl *importNumericLiteral(ClangImporter::Implementation &Impl,
                                       DeclContext *DC,
                                       const clang::MacroInfo *MI,
                                       Identifier name,
                                       const clang::Token *signTok,
                                       const clang::Token &tok,
                                       ClangNode ClangN,
                                       clang::QualType castType) {
  assert(tok.getKind() == clang::tok::numeric_constant &&
         "not a numeric token");
  {
    // Temporary hack to reject literals with ud-suffix.
    // FIXME: remove this when the following radar is implemented:
    // <rdar://problem/16445608> Swift should set up a DiagnosticConsumer for
    // Clang
    llvm::Optional<StringRef> TokSpelling = getTokenSpelling(Impl, tok);
    if (!TokSpelling)
      return nullptr;
    if (TokSpelling->contains('_'))
      return nullptr;
  }

  if (const clang::Expr *parsed = parseNumericLiteral<>(Impl, tok)) {
    auto clangTy = parsed->getType();
    auto literalType = Impl.importTypeIgnoreIUO(
        clangTy, ImportTypeKind::Value,
        ImportDiagnosticAdder(Impl, MI, tok.getLocation()),
        isInSystemModule(DC), Bridgeability::None, ImportTypeAttrs());
    if (!literalType)
      return nullptr;

    Type constantType;
    if (castType.isNull()) {
      constantType = literalType;
    } else {
      constantType = Impl.importTypeIgnoreIUO(
          castType, ImportTypeKind::Value,
          ImportDiagnosticAdder(Impl, MI, MI->getDefinitionLoc()),
          isInSystemModule(DC), Bridgeability::None, ImportTypeAttrs());
      if (!constantType)
        return nullptr;
    }

    if (auto *integer = dyn_cast<clang::IntegerLiteral>(parsed)) {
      // Determine the value.
      llvm::APSInt value{integer->getValue(), clangTy->isUnsignedIntegerType()};

      // If there was a - sign, negate the value.
      // If there was a ~, flip all bits.
      if (signTok) {
        if (signTok->is(clang::tok::minus)) {
          if (!value.isMinSignedValue())
            value = -value;
        } else if (signTok->is(clang::tok::tilde)) {
          value.flipAllBits();
        }
      }

      return createMacroConstant(Impl, MI, name, DC, constantType,
                                 clang::APValue(value),
                                 ConstantConvertKind::None,
                                 /*static*/ false, ClangN);
    }

    if (auto *floating = dyn_cast<clang::FloatingLiteral>(parsed)) {
      // ~ doesn't make sense with floating-point literals.
      if (signTok && signTok->is(clang::tok::tilde))
        return nullptr;

      llvm::APFloat value = floating->getValue();

      // If there was a - sign, negate the value.
      if (signTok && signTok->is(clang::tok::minus)) {
        value.changeSign();
      }

      return createMacroConstant(Impl, MI, name, DC, constantType,
                                 clang::APValue(value),
                                 ConstantConvertKind::None,
                                 /*static*/ false, ClangN);
    }
    // TODO: Other numeric literals (complex, imaginary, etc.)
  }
  return nullptr;
}

static bool isStringToken(const clang::Token &tok) {
  return tok.is(clang::tok::string_literal) ||
         tok.is(clang::tok::utf8_string_literal);
}

// Describes the kind of string literal we're importing.
enum class MappedStringLiteralKind {
  CString,  // "string"
  NSString, // @"string"
  CFString  // CFSTR("string")
};

static ValueDecl *importStringLiteral(ClangImporter::Implementation &Impl,
                                      DeclContext *DC,
                                      const clang::MacroInfo *MI,
                                      Identifier name,
                                      const clang::Token &tok,
                                      MappedStringLiteralKind kind,
                                      ClangNode ClangN) {
  assert(isStringToken(tok));

  clang::ActionResult<clang::Expr*> result =
    Impl.getClangSema().ActOnStringLiteral(tok);
  if (!result.isUsable())
    return nullptr;

  auto parsed = dyn_cast<clang::StringLiteral>(result.get());
  if (!parsed)
    return nullptr;

  Type importTy = Impl.getNamedSwiftType(Impl.getStdlibModule(), "String");
  if (!importTy)
    return nullptr;

  StringRef text = parsed->getString();
  if (!unicode::isWellFormedUTF8(text))
    return nullptr;

  return SwiftDeclSynthesizer(Impl).createConstant(name, DC, importTy, text,
                                                   ConstantConvertKind::None,
                                                   /*static*/ false, ClangN);
}

static ValueDecl *importLiteral(ClangImporter::Implementation &Impl,
                                DeclContext *DC,
                                const clang::MacroInfo *MI,
                                Identifier name,
                                const clang::Token &tok,
                                ClangNode ClangN,
                                clang::QualType castType) {
  switch (tok.getKind()) {
  case clang::tok::numeric_constant: {
    ValueDecl *importedNumericLiteral = importNumericLiteral(
        Impl, DC, MI, name, /*signTok*/ nullptr, tok, ClangN, castType);
    if (!importedNumericLiteral) {
      Impl.addImportDiagnostic(
          &tok, Diagnostic(diag::macro_not_imported_invalid_numeric_literal),
          tok.getLocation());
      Impl.addImportDiagnostic(MI,
                               Diagnostic(diag::macro_not_imported, name.str()),
                               MI->getDefinitionLoc());
    }
    return importedNumericLiteral;
  }
  case clang::tok::string_literal:
  case clang::tok::utf8_string_literal: {
    ValueDecl *importedStringLiteral = importStringLiteral(
        Impl, DC, MI, name, tok, MappedStringLiteralKind::CString, ClangN);
    if (!importedStringLiteral) {
      Impl.addImportDiagnostic(
          &tok, Diagnostic(diag::macro_not_imported_invalid_string_literal),
          tok.getLocation());
      Impl.addImportDiagnostic(MI,
                               Diagnostic(diag::macro_not_imported, name.str()),
                               MI->getDefinitionLoc());
    }
    return importedStringLiteral;
  }

  // TODO: char literals.
  default:
    Impl.addImportDiagnostic(
        &tok, Diagnostic(diag::macro_not_imported_unsupported_literal),
        tok.getLocation());
    Impl.addImportDiagnostic(MI,
                             Diagnostic(diag::macro_not_imported, name.str()),
                             MI->getDefinitionLoc());
    return nullptr;
  }
}

static ValueDecl *importNil(ClangImporter::Implementation &Impl,
                            DeclContext *DC, Identifier name,
                            ClangNode clangN) {
  // We use a dummy type since we don't have a convenient type for 'nil'.  Any
  // use of this will be an error anyway.
  auto type = TupleType::getEmpty(Impl.SwiftContext);
  return Impl.createUnavailableDecl(name, DC, type,
                                    "use 'nil' instead of this imported macro",
                                    /*isStatic=*/false, clangN);
}

static bool isSignToken(const clang::Token &tok) {
  return tok.is(clang::tok::plus) || tok.is(clang::tok::minus) ||
         tok.is(clang::tok::tilde);
}

static llvm::Optional<clang::QualType>
builtinTypeForToken(const clang::Token &tok, const clang::ASTContext &context) {
  switch (tok.getKind()) {
  case clang::tok::kw_short:
    return clang::QualType(context.ShortTy);
  case clang::tok::kw_long:
    return clang::QualType(context.LongTy);
  case clang::tok::kw___int64:
    return clang::QualType(context.LongLongTy);
  case clang::tok::kw___int128:
    return clang::QualType(context.Int128Ty);
  case clang::tok::kw_signed:
    return clang::QualType(context.IntTy);
  case clang::tok::kw_unsigned:
    return clang::QualType(context.UnsignedIntTy);
  case clang::tok::kw_void:
    return clang::QualType(context.VoidTy);
  case clang::tok::kw_char:
    return clang::QualType(context.CharTy);
  case clang::tok::kw_int:
    return clang::QualType(context.IntTy);
  case clang::tok::kw_float:
    return clang::QualType(context.FloatTy);
  case clang::tok::kw_double:
    return clang::QualType(context.DoubleTy);
  case clang::tok::kw_wchar_t:
    return clang::QualType(context.WCharTy);
  case clang::tok::kw_bool:
    return clang::QualType(context.BoolTy);
  case clang::tok::kw_char16_t:
    return clang::QualType(context.Char16Ty);
  case clang::tok::kw_char32_t:
    return clang::QualType(context.Char32Ty);
  default:
    return llvm::None;
  }
}

static llvm::Optional<std::pair<llvm::APSInt, Type>>
getIntegerConstantForMacroToken(ClangImporter::Implementation &impl,
                                const clang::MacroInfo *macro, DeclContext *DC,
                                const clang::Token &token) {

  // Integer literal.
  if (token.is(clang::tok::numeric_constant)) {
    if (auto literal = parseNumericLiteral<clang::IntegerLiteral>(impl,token)) {
      auto value = llvm::APSInt { literal->getValue(),
                                  literal->getType()->isUnsignedIntegerType() };
      auto type = impl.importTypeIgnoreIUO(
          literal->getType(), ImportTypeKind::Value,
          ImportDiagnosticAdder(impl, macro, token.getLocation()),
          isInSystemModule(DC), Bridgeability::None, ImportTypeAttrs());
      return {{ value, type }};
    }

  // Macro identifier.
  // TODO: for some reason when in C++ mode, "hasMacroDefinition" is often
  // false: rdar://110071334
  } else if (token.is(clang::tok::identifier) &&
             token.getIdentifierInfo()->hasMacroDefinition()) {

    auto rawID = token.getIdentifierInfo();
    auto definition = impl.getClangPreprocessor().getMacroDefinition(rawID);
    if (!definition)
      return llvm::None;

    ClangNode macroNode;
    const clang::MacroInfo *macroInfo;
    if (definition.getModuleMacros().empty()) {
      macroInfo = definition.getMacroInfo();
      macroNode = macroInfo;
    } else {
      // Follow MacroDefinition::getMacroInfo in preferring the last ModuleMacro
      // rather than the first.
      const clang::ModuleMacro *moduleMacro =
          definition.getModuleMacros().back();
      macroInfo = moduleMacro->getMacroInfo();
      macroNode = moduleMacro;
    }
    auto importedID = impl.getNameImporter().importMacroName(rawID, macroInfo);
    (void)impl.importMacro(importedID, macroNode);

    auto searcher = impl.ImportedMacroConstants.find(macroInfo);
    if (searcher == impl.ImportedMacroConstants.end()) {
      return llvm::None;
    }
    auto importedConstant = searcher->second;
    if (!importedConstant.first.isInt()) {
      return llvm::None;
    }
    return {{ importedConstant.first.getInt(), importedConstant.second }};
  }

  return llvm::None;
}

static ValueDecl *importMacro(ClangImporter::Implementation &impl,
                              llvm::SmallSet<StringRef, 4> &visitedMacros,
                              DeclContext *DC, Identifier name,
                              const clang::MacroInfo *macro, ClangNode ClangN,
                              clang::QualType castType) {
  if (name.empty()) return nullptr;

  assert(visitedMacros.count(name.str()) &&
         "Add the name of the macro to visitedMacros before calling this "
         "function.");

  if (macro->isFunctionLike()) {
    impl.addImportDiagnostic(
        macro, Diagnostic(diag::macro_not_imported_function_like, name.str()),
        macro->getDefinitionLoc());
    return nullptr;
  }

  auto numTokens = macro->getNumTokens();
  auto tokenI = macro->tokens_begin(), tokenE = macro->tokens_end();

  // Drop one layer of parentheses.
  if (numTokens > 2 &&
      tokenI[0].is(clang::tok::l_paren) &&
      tokenE[-1].is(clang::tok::r_paren)) {
    ++tokenI;
    --tokenE;
    numTokens -= 2;
  }

  // Handle tokens starting with a type cast
  bool castTypeIsId = false;
  if (numTokens > 3 &&
      tokenI[0].is(clang::tok::l_paren) &&
      (tokenI[1].is(clang::tok::identifier) ||
        impl.getClangSema().isSimpleTypeSpecifier(tokenI[1].getKind())) &&
      tokenI[2].is(clang::tok::r_paren)) {
    if (!castType.isNull()) {
      // this is a nested cast
      // TODO(https://github.com/apple/swift/issues/57735): Diagnose nested cast.
      return nullptr;
    }

    if (tokenI[1].is(clang::tok::identifier)) {
      auto identifierInfo = tokenI[1].getIdentifierInfo();
      if (identifierInfo->isStr("id")) {
        castTypeIsId = true;
      }
      auto identifierName = identifierInfo->getName();
      auto &identifier = impl.getClangASTContext().Idents.get(identifierName);

      clang::sema::DelayedDiagnosticPool diagPool{
          impl.getClangSema().DelayedDiagnostics.getCurrentPool()};
      auto diagState = impl.getClangSema().DelayedDiagnostics.push(diagPool);
      auto parsedType = impl.getClangSema().getTypeName(identifier,
                                                        clang::SourceLocation(),
                                                        impl.getClangSema().TUScope);
      impl.getClangSema().DelayedDiagnostics.popWithoutEmitting(diagState);

      if (parsedType && diagPool.empty()) {
        castType = parsedType.get();
      } else {
        // TODO(https://github.com/apple/swift/issues/57735): Add diagnosis.
        return nullptr;
      }
      if (!castType->isBuiltinType() && !castTypeIsId) {
        // TODO(https://github.com/apple/swift/issues/57735): Add diagnosis.
        return nullptr;
      }
    } else {
      auto builtinType = builtinTypeForToken(tokenI[1],
                                             impl.getClangASTContext());
      if (builtinType) {
        castType = builtinType.value();
      } else {
        // TODO(https://github.com/apple/swift/issues/57735): Add diagnosis.
        return nullptr;
      }
    }
    tokenI += 3;
    numTokens -= 3;
  }

  // FIXME: Ask Clang to try to parse and evaluate the expansion as a constant
  // expression instead of doing these special-case pattern matches.
  switch (numTokens) {
  case 1: {
    // Check for a single-token expansion of the form <literal>.
    // TODO: or <identifier>.
    const clang::Token &tok = *tokenI;

    if (castTypeIsId && tok.is(clang::tok::numeric_constant)) {
      auto *integerLiteral =
        parseNumericLiteral<clang::IntegerLiteral>(impl, tok);
      if (integerLiteral && integerLiteral->getValue() == 0)
        return importNil(impl, DC, name, ClangN);
    }

    // If it's a literal token, we might be able to translate the literal.
    if (tok.isLiteral()) {
      return importLiteral(impl, DC, macro, name, tok, ClangN, castType);
    }

    if (tok.is(clang::tok::identifier)) {
      auto clangID = tok.getIdentifierInfo();

      if (clangID->isOutOfDate())
        // Update the identifier with macro definitions subsequently loaded from
        // a module/AST file. We're supposed to use
        // Preprocessor::HandleIdentifier() to do that, but that method does too
        // much to call it here. Instead, we call getLeafModuleMacros() for its
        // side effect of calling updateOutOfDateIdentifier().
        // FIXME: clang should give us a better way to do this.
        (void)impl.getClangPreprocessor().getLeafModuleMacros(clangID);

      // If it's an identifier that is itself a macro, look into that macro.
      if (clangID->hasMacroDefinition()) {
        auto isNilMacro =
          llvm::StringSwitch<bool>(clangID->getName())
#define NIL_MACRO(NAME) .Case(#NAME, true)
#include "MacroTable.def"
          .Default(false);
        if (isNilMacro)
          return importNil(impl, DC, name, ClangN);

        auto macroID = impl.getClangPreprocessor().getMacroInfo(clangID);
        if (macroID && macroID != macro) {
          // If we've already visited this macro, then bail to prevent an
          // infinite loop. Otherwise, record that we're going to visit it.
          if (!visitedMacros.insert(clangID->getName()).second)
            return nullptr;

          // FIXME: This was clearly intended to pass the cast type down, but
          // doing so would be a behavior change.
          return importMacro(impl, visitedMacros, DC, name, macroID, ClangN,
                             /*castType*/ {});
        }
      }

      // FIXME: If the identifier refers to a declaration, alias it?
    }

    // TODO(https://github.com/apple/swift/issues/57735): Seems rare to have a single token that is neither a literal nor an identifier, but add diagnosis.
    return nullptr;
  }
  case 2: {
    // Check for a two-token expansion of the form +<number> or -<number>.
    // These are technically subtly wrong without parentheses because they
    // allow things like:
    //   #define EOF -1
    //   int pred(int x) { return x EOF; }
    // but are pervasive in C headers anyway.
    clang::Token const &first = tokenI[0];
    clang::Token const &second = tokenI[1];

    if (isSignToken(first) && second.is(clang::tok::numeric_constant)) {
      ValueDecl *importedNumericLiteral = importNumericLiteral(
          impl, DC, macro, name, &first, second, ClangN, castType);
      if (!importedNumericLiteral) {
        impl.addImportDiagnostic(
            macro, Diagnostic(diag::macro_not_imported, name.str()),
            macro->getDefinitionLoc());
        impl.addImportDiagnostic(
            &second,
            Diagnostic(diag::macro_not_imported_invalid_numeric_literal),
            second.getLocation());
      }
      return importedNumericLiteral;
    }

    // We also allow @"string".
    if (first.is(clang::tok::at) && isStringToken(second)) {
      ValueDecl *importedStringLiteral =
          importStringLiteral(impl, DC, macro, name, second,
                              MappedStringLiteralKind::NSString, ClangN);
      if (!importedStringLiteral) {
        impl.addImportDiagnostic(
            macro, Diagnostic(diag::macro_not_imported, name.str()),
            macro->getDefinitionLoc());
        impl.addImportDiagnostic(
            &second,
            Diagnostic(diag::macro_not_imported_invalid_string_literal),
            second.getLocation());
      }
      return importedStringLiteral;
    }

    break;
  }
  case 3: {
    // Check for infix operations between two integer constants.
    // Import the result as another integer constant:
    //   #define INT3 (INT1 <op> INT2)
    // Doesn't allow inner parentheses.

    // Parse INT1.
    llvm::APSInt firstValue;
    Type firstSwiftType = nullptr;
    if (auto firstInt = getIntegerConstantForMacroToken(impl, macro, DC,
                                                        tokenI[0])) {
      firstValue     = firstInt->first;
      firstSwiftType = firstInt->second;
    } else {
      impl.addImportDiagnostic(
          macro,
          Diagnostic(diag::macro_not_imported_unsupported_structure,
                     name.str()),
          macro->getDefinitionLoc());
      return nullptr;
    }

    // Parse INT2.
    llvm::APSInt secondValue;
    Type secondSwiftType = nullptr;
    if (auto secondInt = getIntegerConstantForMacroToken(impl, macro, DC,
                                                         tokenI[2])) {
      secondValue     = secondInt->first;
      secondSwiftType = secondInt->second;
    } else {
      impl.addImportDiagnostic(
          macro,
          Diagnostic(diag::macro_not_imported_unsupported_structure,
                     name.str()),
          macro->getDefinitionLoc());
      return nullptr;
    }

    llvm::APSInt resultValue;
    Type resultSwiftType = nullptr;

    // Resolve width and signedness differences and find the type of the result.
    auto firstIntSpec  = clang::ento::APSIntType(firstValue);
    auto secondIntSpec = clang::ento::APSIntType(secondValue);
    if (firstIntSpec == std::max(firstIntSpec, secondIntSpec)) {
      firstIntSpec.apply(secondValue);
      resultSwiftType = firstSwiftType;
    } else {
      secondIntSpec.apply(firstValue);
      resultSwiftType = secondSwiftType;
    }

    // Addition.
    if (tokenI[1].is(clang::tok::plus)) {
      resultValue = firstValue + secondValue;

    // Subtraction.
    } else if (tokenI[1].is(clang::tok::minus)) {
      resultValue = firstValue - secondValue;

    // Multiplication.
    } else if (tokenI[1].is(clang::tok::star)) {
      resultValue = firstValue * secondValue;

    // Division.
    } else if (tokenI[1].is(clang::tok::slash)) {
      if (secondValue == 0) { return nullptr; }
      resultValue = firstValue / secondValue;

    // Left-shift.
    } else if (tokenI[1].is(clang::tok::lessless)) {
      // Shift by a negative number is UB in C. Don't import.
      if (secondValue.isNegative()) { return nullptr; }
      resultValue = llvm::APSInt { firstValue.shl(secondValue),
                                   firstValue.isUnsigned() };

    // Right-shift.
    } else if (tokenI[1].is(clang::tok::greatergreater)) {
      // Shift by a negative number is UB in C. Don't import.
      if (secondValue.isNegative()) { return nullptr; }
      if (firstValue.isUnsigned()) {
        resultValue = llvm::APSInt { firstValue.lshr(secondValue),
                                     /*isUnsigned*/ true };
      } else {
        resultValue = llvm::APSInt { firstValue.ashr(secondValue),
                                     /*isUnsigned*/ false };
      }

    // Bitwise OR.
    } else if (tokenI[1].is(clang::tok::pipe)) {
      firstValue.setIsUnsigned(true);
      secondValue.setIsUnsigned(true);
      resultValue = llvm::APSInt { firstValue | secondValue,
                                   /*isUnsigned*/ true };

    // Bitwise AND.
    } else if (tokenI[1].is(clang::tok::amp)) {
      firstValue.setIsUnsigned(true);
      secondValue.setIsUnsigned(true);
      resultValue = llvm::APSInt { firstValue & secondValue,
                                   /*isUnsigned*/ true };

    // XOR.
    } else if (tokenI[1].is(clang::tok::caret)) {
      firstValue.setIsUnsigned(true);
      secondValue.setIsUnsigned(true);
      resultValue = llvm::APSInt { firstValue ^ secondValue,
                                   /*isUnsigned*/ true };

    // Logical OR.
    } else if (tokenI[1].is(clang::tok::pipepipe)) {
      bool result  = firstValue.getBoolValue() || secondValue.getBoolValue();
      resultValue  = llvm::APSInt::get(result);
      resultSwiftType = impl.SwiftContext.getBoolType();

    // Logical AND.
    } else if (tokenI[1].is(clang::tok::ampamp)) {
      bool result  = firstValue.getBoolValue() && secondValue.getBoolValue();
      resultValue  = llvm::APSInt::get(result);
      resultSwiftType = impl.SwiftContext.getBoolType();

    // Equality.
    } else if (tokenI[1].is(clang::tok::equalequal)) {
      resultValue     = llvm::APSInt::get(firstValue == secondValue);
      resultSwiftType = impl.SwiftContext.getBoolType();

    // Less than.
    } else if (tokenI[1].is(clang::tok::less)) {
      resultValue     = llvm::APSInt::get(firstValue < secondValue);
      resultSwiftType = impl.SwiftContext.getBoolType();

    // Less than or equal.
    } else if (tokenI[1].is(clang::tok::lessequal)) {
      resultValue     = llvm::APSInt::get(firstValue <= secondValue);
      resultSwiftType = impl.SwiftContext.getBoolType();

    // Greater than.
    } else if (tokenI[1].is(clang::tok::greater)) {
      resultValue     = llvm::APSInt::get(firstValue > secondValue);
      resultSwiftType = impl.SwiftContext.getBoolType();

    // Greater than or equal.
    } else if (tokenI[1].is(clang::tok::greaterequal)) {
      resultValue     = llvm::APSInt::get(firstValue >= secondValue);
      resultSwiftType = impl.SwiftContext.getBoolType();

    // Unhandled operators.
    } else {
      if (llvm::Optional<StringRef> operatorSpelling =
              getTokenSpelling(impl, tokenI[1])) {
        impl.addImportDiagnostic(
            &tokenI[1],
            Diagnostic(diag::macro_not_imported_unsupported_named_operator,
                       *operatorSpelling),
            tokenI[1].getLocation());
      } else {
        impl.addImportDiagnostic(
            &tokenI[1],
            Diagnostic(diag::macro_not_imported_unsupported_operator),
            tokenI[1].getLocation());
      }
      impl.addImportDiagnostic(macro,
                               Diagnostic(diag::macro_not_imported, name.str()),
                               macro->getDefinitionLoc());
      return nullptr;
    }

    return createMacroConstant(impl, macro, name, DC, resultSwiftType,
                               clang::APValue(resultValue),
                               ConstantConvertKind::None,
                               /*isStatic=*/false, ClangN);
  }
  case 4: {
    // Check for a CFString literal of the form CFSTR("string").
    if (tokenI[0].is(clang::tok::identifier) &&
        tokenI[0].getIdentifierInfo()->isStr("CFSTR") &&
        tokenI[1].is(clang::tok::l_paren) &&
        isStringToken(tokenI[2]) &&
        tokenI[3].is(clang::tok::r_paren)) {
      return importStringLiteral(impl, DC, macro, name, tokenI[2],
                                 MappedStringLiteralKind::CFString, ClangN);
    }
    // FIXME: Handle BIT_MASK(pos) helper macros which expand to a constant?
    break;
  }
  case 5:
    // Check for the literal series of tokens (void*)0. (We've already stripped
    // one layer of parentheses.)
    if (tokenI[0].is(clang::tok::l_paren) &&
        tokenI[1].is(clang::tok::kw_void) &&
        tokenI[2].is(clang::tok::star) &&
        tokenI[3].is(clang::tok::r_paren) &&
        tokenI[4].is(clang::tok::numeric_constant)) {
      auto *integerLiteral =
        parseNumericLiteral<clang::IntegerLiteral>(impl, tokenI[4]);
      if (!integerLiteral || integerLiteral->getValue() != 0)
        break;
      return importNil(impl, DC, name, ClangN);
    }
    break;
  default:
    break;
  }

  impl.addImportDiagnostic(
      macro,
      Diagnostic(diag::macro_not_imported_unsupported_structure, name.str()),
      macro->getDefinitionLoc());
  return nullptr;
}

ValueDecl *ClangImporter::Implementation::importMacro(Identifier name,
                                                      ClangNode macroNode) {
  const clang::MacroInfo *macro = macroNode.getAsMacro();
  if (!macro)
    return nullptr;

  PrettyStackTraceStringAction stackRAII{"importing macro", name.str()};

  // Look for macros imported with the same name.
  auto known = ImportedMacros.find(name);
  if (known == ImportedMacros.end()) {
    // Push in a placeholder to break circularity.
    ImportedMacros[name].push_back({macro, nullptr});
  } else {
    // Check whether this macro has already been imported.
    for (const auto &entry : known->second) {
      if (entry.first == macro)
        return entry.second;
    }

    // Otherwise, check whether this macro is identical to a macro that has
    // already been imported.
    auto &clangPP = getClangPreprocessor();
    for (const auto &entry : known->second) {
      // If the macro is equal to an existing macro, map down to the same
      // declaration.
      if (macro->isIdenticalTo(*entry.first, clangPP, true)) {
        ValueDecl *result = entry.second;
        known->second.push_back({macro, result});
        return result;
      }
    }

    // If not, push in a placeholder to break circularity.
    known->second.push_back({macro, nullptr});
  }

  startedImportingEntity();

  // We haven't tried to import this macro yet. Do so now, and cache the
  // result.

  DeclContext *DC;
  if (const clang::Module *module = getClangOwningModule(macroNode)) {
    // Get the parent module because currently we don't model Clang submodules
    // in Swift.
    DC = getWrapperForModule(module->getTopLevelModule());
  } else {
    DC = ImportedHeaderUnit;
  }

  llvm::SmallSet<StringRef, 4> visitedMacros;
  visitedMacros.insert(name.str());
  auto valueDecl =
      ::importMacro(*this, visitedMacros, DC, name, macro, macroNode,
                    /*castType*/ {});

  // Update the entry for the value we just imported.
  // It's /probably/ the last entry in ImportedMacros[name], but there's an
  // outside chance more macros with the same name have been imported
  // re-entrantly since this method started.
  if (valueDecl) {
    auto entryIter = llvm::find_if(llvm::reverse(ImportedMacros[name]),
        [macro](std::pair<const clang::MacroInfo *, ValueDecl *> entry) {
      return entry.first == macro;
    });
    assert(entryIter != llvm::reverse(ImportedMacros[name]).end() &&
           "placeholder not found");
    entryIter->second = valueDecl;
  }

  return valueDecl;
}
