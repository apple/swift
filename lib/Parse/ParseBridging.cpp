//===--- ParseBridging.cpp ------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/ParseBridging.h"
#include "swift/Basic/Assertions.h"
#include "swift/Bridging/ASTGen.h"
#include "swift/Parse/Parser.h"

using namespace swift;

// MARK: Functions called from SwiftParser.

BridgedExpr BridgedLegacyParser_parseExpr(BridgedLegacyParser p,
                                          BridgedSourceLoc loc,
                                          BridgedDeclContext DC,
                                          bool isExprBasic) {
  auto &P = p.unbridged();
  auto PP =
      P.getParserPosition(loc.unbridged(), /*PreviousLoc=*/loc.unbridged());
  P.CurDeclContext = DC.unbridged();
  P.restoreParserPosition(PP);
  // Since `SequenceExpr` is implemented in ASTGen, it only delegates sequence
  // elements parsing.
  ParserResult<Expr> result =
      P.parseExprSequenceElement(diag::expected_expr, isExprBasic);
  return result.getPtrOrNull();
}

BridgedDecl BridgedLegacyParser_parseDecl(BridgedLegacyParser p,
                                          BridgedSourceLoc loc,
                                          BridgedDeclContext DC) {
  auto &P = p.unbridged();
  auto PP =
      P.getParserPosition(loc.unbridged(), /*PreviousLoc=*/loc.unbridged());
  P.CurDeclContext = DC.unbridged();
  P.restoreParserPosition(PP);

  // FIXME: IsAtStartOfLineOrPreviousHadSemi should be passed in from ASTGen.
  // IfConfigsAreDeclAttrs: true because ASTGen thinks the current location is
  // a start of a decl.
  ParserResult<Decl> result = P.parseDecl(
      /*IsAtStartOfLineOrPreviousHadSemi=*/true,
      /*IfConfigsAreDeclAttrs=*/true, [&](Decl *decl) {},
      /*fromASTGen=*/true);
  return result.getPtrOrNull();
}

BridgedStmt BridgedLegacyParser_parseStmt(BridgedLegacyParser p,
                                          BridgedSourceLoc loc,
                                          BridgedDeclContext DC) {
  auto &P = p.unbridged();
  auto PP =
      P.getParserPosition(loc.unbridged(), /*PreviousLoc=*/loc.unbridged());
  P.CurDeclContext = DC.unbridged();
  P.restoreParserPosition(PP);

  ParserResult<Stmt> result = P.parseStmt(/*fromASTGen=*/true);
  return result.getPtrOrNull();
}

BridgedTypeRepr BridgedLegacyParser_parseType(BridgedLegacyParser p,
                                              BridgedSourceLoc loc,
                                              BridgedDeclContext DC) {
  auto &P = p.unbridged();
  auto PP =
      P.getParserPosition(loc.unbridged(), /*PreviousLoc=*/loc.unbridged());
  P.CurDeclContext = DC.unbridged();
  P.restoreParserPosition(PP);

  // FIXME: Calculate 'ParseTypeReason' in ASTGen.
  ParserResult<TypeRepr> result =
      P.parseType(diag::expected_type, Parser::ParseTypeReason::Unspecified,
                  /*fromASTGen=*/true);
  return result.getPtrOrNull();
}

// MARK: Functions called from C++ parser.

/// Callback function used for creating a C++ AST from the syntax node at the
/// given source location.
///
/// The arguments to this callback are the source file to pass into ASTGen (the
/// exported source file) and the source location pointer to pass into ASTGen
/// (to find the syntax node).
///
/// The callback returns the new AST node and the ending location of the syntax
/// node. If the AST node is NULL, something went wrong.
template <typename T>
using ASTFromSyntaxTreeCallback = T *(void *sourceFile,
                                      BridgedSourceLoc sourceLoc,
                                      BridgedSourceLoc &endLoc);

#if SWIFT_BUILD_SWIFT_SYNTAX
/// Parse by constructing a C++ AST node from the Swift syntax tree via ASTGen.
template <typename T>
static ParserResult<T>
parseASTFromSyntaxTree(swift::Parser &P,
                       llvm::function_ref<ASTFromSyntaxTreeCallback<T>> body) {
  assert(P.IsForASTGen);
  auto exportedSourceFile = P.SF.getExportedSourceFile();
  if (!exportedSourceFile)
    return nullptr;

  // Perform the translation.
  BridgedSourceLoc endLoc;
  T *astNode = body(exportedSourceFile, P.Tok.getLoc(), endLoc);

  if (!astNode) {
    assert(false && "Could not build AST node from syntax tree");
    return nullptr;
  }

  // Reset the lexer to the ending location.
  auto endOffset =
      P.SourceMgr.getLocOffsetInBuffer(endLoc.unbridged(), P.L->getBufferID());
  P.L->resetToOffset(endOffset);
  P.consumeTokenWithoutFeedingReceiver();

  return makeParserResult(astNode);
}
#endif

ParserResult<TypeRepr> Parser::parseTypeReprFromSyntaxTree() {
#if SWIFT_BUILD_SWIFT_SYNTAX
  return parseASTFromSyntaxTree<TypeRepr>(*this, [&](void *exportedSourceFile,
                                                     BridgedSourceLoc sourceLoc,
                                                     BridgedSourceLoc &endLoc) {
    return swift_ASTGen_buildTypeRepr(&Diags, exportedSourceFile, sourceLoc,
                                      CurDeclContext, Context, *this, &endLoc);
  });
#else
  llvm_unreachable("ASTGen is not supported");
#endif
}

ParserResult<Decl> Parser::parseDeclFromSyntaxTree() {
#if SWIFT_BUILD_SWIFT_SYNTAX
  return parseASTFromSyntaxTree<Decl>(*this, [&](void *exportedSourceFile,
                                                 BridgedSourceLoc sourceLoc,
                                                 BridgedSourceLoc &endLoc) {
    return swift_ASTGen_buildDecl(&Diags, exportedSourceFile, sourceLoc,
                                  CurDeclContext, Context, *this, &endLoc);
  });
#else
  llvm_unreachable("ASTGen is not supported");
#endif
}

ParserResult<Expr> Parser::parseExprFromSyntaxTree() {
#if SWIFT_BUILD_SWIFT_SYNTAX
  return parseASTFromSyntaxTree<Expr>(*this, [&](void *exportedSourceFile,
                                                 BridgedSourceLoc sourceLoc,
                                                 BridgedSourceLoc &endLoc) {
    return swift_ASTGen_buildExpr(&Diags, exportedSourceFile, sourceLoc,
                                  CurDeclContext, Context, *this, &endLoc);
  });
#else
  llvm_unreachable("ASTGen is not supported");
#endif
}

ParserResult<Stmt> Parser::parseStmtFromSyntaxTree() {
#if SWIFT_BUILD_SWIFT_SYNTAX
  return parseASTFromSyntaxTree<Stmt>(*this, [&](void *exportedSourceFile,
                                                 BridgedSourceLoc sourceLoc,
                                                 BridgedSourceLoc &endLoc) {
    return swift_ASTGen_buildStmt(&Diags, exportedSourceFile, sourceLoc,
                                  CurDeclContext, Context, *this, &endLoc);
  });
#else
  llvm_unreachable("ASTGen is not supported");
#endif
}
