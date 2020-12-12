//===--- ASTGen.h ---------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_PARSE_ASTGEN_H
#define SWIFT_PARSE_ASTGEN_H

#include "swift/AST/ASTContext.h"
#include "swift/AST/Expr.h"
#include "swift/Syntax/SyntaxNodes.h"
#include "llvm/ADT/DenseMap.h"

namespace swift {
class ComponentIdentTypeRepr;
class TupleTypeRepr;
class Parser;

/// Generates AST nodes from Syntax nodes.
class ASTGen {
  ASTContext &Context;

  // TODO: (syntax-parse) ASTGen should not have a refernce to the parser.
  /// The parser from which the ASTGen is being invoked. Used to inform the
  /// parser about encountered code completion tokens.
  Parser &P;

  // TODO: (syntax-parse) Remove when parsing of all types has been migrated to
  // libSyntax.
  /// Types that cannot be represented by Syntax or generated by ASTGen.
  llvm::DenseMap<SourceLoc, TypeRepr *> Types;

public:
  ASTGen(ASTContext &Context, Parser &P) : Context(Context), P(P) {}

  //===--------------------------------------------------------------------===//
  // MARK: - Expressions
public:
  Expr *generate(const syntax::BooleanLiteralExprSyntax &Expr, const SourceLoc &Loc);
  Expr *generate(const syntax::FloatLiteralExprSyntax &Expr, const SourceLoc &Loc);
  Expr *generate(const syntax::IntegerLiteralExprSyntax &Expr, const SourceLoc &Loc);
  Expr *generate(const syntax::NilLiteralExprSyntax &Expr, const SourceLoc &Loc);
  Expr *generate(const syntax::PoundColumnExprSyntax &Expr, const SourceLoc &Loc);
  Expr *generate(const syntax::PoundDsohandleExprSyntax &Expr, const SourceLoc &Loc);
  Expr *generate(const syntax::PoundFileExprSyntax &Expr, const SourceLoc &Loc);
  Expr *generate(const syntax::PoundFileIDExprSyntax &Expr, const SourceLoc &Loc);
  Expr *generate(const syntax::PoundFilePathExprSyntax &Expr, const SourceLoc &Loc);
  Expr *generate(const syntax::PoundLineExprSyntax &Expr, const SourceLoc &Loc);
  Expr *generate(const syntax::PoundFunctionExprSyntax &Expr, const SourceLoc &Loc);
  Expr *generate(const syntax::UnknownExprSyntax &Expr, const SourceLoc &Loc);

private:
  Expr *
  generateMagicIdentifierLiteralExpr(const syntax::TokenSyntax &PoundToken,
                                     const SourceLoc &Loc);

  /// Map magic literal tokens such as #file to their MagicIdentifierLiteralExpr
  /// kind.
  MagicIdentifierLiteralExpr::Kind getMagicIdentifierLiteralKind(tok Kind);

  //===--------------------------------------------------------------------===//
  // MARK: - Types.
public:
  TypeRepr *generate(const syntax::TypeSyntax &Type, const SourceLoc Loc);

  TypeRepr *generate(const syntax::ArrayTypeSyntax &Type, const SourceLoc Loc);
  TypeRepr *generate(const syntax::AttributedTypeSyntax &Type,
                     const SourceLoc Loc);
  TypeRepr *generate(const syntax::CodeCompletionTypeSyntax &Type,
                     const SourceLoc Loc);
  TypeRepr *generate(const syntax::DictionaryTypeSyntax &Type,
                     const SourceLoc Loc);
  TypeRepr *generate(const syntax::MemberTypeIdentifierSyntax &Type,
                     const SourceLoc Loc);
  TypeRepr *generate(const syntax::SimpleTypeIdentifierSyntax &Type,
                     const SourceLoc Loc);
  TypeRepr *generate(const syntax::TupleTypeSyntax &Type, const SourceLoc Loc);
  TypeRepr *generate(const syntax::UnknownTypeSyntax &Type,
                     const SourceLoc Loc);

  /// Add a \c TypeRepr occurring at \c Loc whose parsing hasn't been migrated
  /// to libSyntaxParsing yet. It can later be retrieved from \c ASTGen using
  /// \c hasType and \c takeType.
  void addType(TypeRepr *Expr, const SourceLoc Loc);

  /// Check if a \c TypeRepr, whose parsing hasn't been migrated to libSyntax
  /// yet, has been added to \c Types at the given \c Loc.
  bool hasType(const SourceLoc Loc) const;

  /// Given there is a \c TypeRepr, whose parsing hasn't been migrated to
  /// libSyntax yet, at the given \c Loc.
  TypeRepr *takeType(const SourceLoc Loc);

private:
  /// Generate the \c TypeReprs specified in the \c clauseSyntax and write them
  /// to \c args. Also write the position of the left and right angle brackets
  /// to \c lAngleLoc and \c rAngleLoc.
  void
  generateGenericArgs(const syntax::GenericArgumentClauseSyntax &ClauseSyntax,
                      const SourceLoc Loc, SourceLoc &LAngleLoc,
                      SourceLoc &RAngleLoc, SmallVectorImpl<TypeRepr *> &Args);

  /// Generate a \c TupleTypeRepr for the given tuple \p Elements and parens.
  TupleTypeRepr *
  generateTuple(const syntax::TokenSyntax &LParen,
                const syntax::TupleTypeElementListSyntax &Elements,
                const syntax::TokenSyntax &RParen, const SourceLoc Loc);

  /// Generate a \c ComponentIdentTypeRepr from a \c SimpleTypeIdentifierSyntax
  /// or \c MemberTypeIdentifierSyntax. If \c TypeSyntax is a \c
  /// MemberTypeIdentifierSyntax this will *not* walk its children. Use \c
  /// gatherTypeIdentifierComponents to gather all components.
  template <typename T>
  ComponentIdentTypeRepr *generateTypeIdentifier(const T &TypeSyntax,
                                                 const SourceLoc Loc);

  /// Recursively walk the \c Component type syntax and gather all type
  /// components as \c TypeReprs in \c Components.
  void gatherTypeIdentifierComponents(
      const syntax::TypeSyntax &Component, const SourceLoc Loc,
      llvm::SmallVectorImpl<ComponentIdentTypeRepr *> &Components);

public:
  //===--------------------------------------------------------------------===//
  // MARK: Other
public:
  /// Copy a numeric literal value into AST-owned memory, stripping underscores
  /// so the semantic part of the value can be parsed by APInt/APFloat parsers.
  static StringRef copyAndStripUnderscores(StringRef Orig, ASTContext &Context);

private:
  StringRef copyAndStripUnderscores(StringRef Orig);

  /// Advance \p Loc to the first token of the \p Node.
  /// \p Loc must be the leading trivia of the first token in the tree in which
  /// \p Node resides.
  static SourceLoc advanceLocBegin(const SourceLoc &Loc,
                                   const syntax::Syntax &Node);

  /// Advance \p Loc to the last non-missing token of the \p Node or, if it
  /// doesn't contain any, the last non-missing token preceding it in the tree.
  /// \p Loc must be the leading trivia of the first token in the tree in which
  /// \p Node resides
  static SourceLoc advanceLocEnd(const SourceLoc &Loc,
                                 const syntax::Syntax &Node);
};
} // namespace swift

#endif // SWIFT_PARSE_ASTGEN_H
