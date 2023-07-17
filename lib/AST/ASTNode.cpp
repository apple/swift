//===--- ASTNode.cpp - Swift Language ASTs --------------------------------===//
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
// This file implements the ASTNode, which is a union of Stmt, Expr, and Decl.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTNode.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Parse/Token.h"

using namespace swift;

SourceRange ASTNode::getSourceRange() const {
  if (const auto *E = this->dyn_cast<Expr*>())
    return E->getSourceRange();
  if (const auto *S = this->dyn_cast<Stmt*>())
    return S->getSourceRange();
  if (const auto *D = this->dyn_cast<Decl*>())
    return D->getSourceRange();
  if (const auto *P = this->dyn_cast<Pattern*>())
    return P->getSourceRange();
  if (const auto *T = this->dyn_cast<TypeRepr *>())
    return T->getSourceRange();
  if (const auto *C = this->dyn_cast<StmtConditionElement *>())
    return C->getSourceRange();
  if (const auto *I = this->dyn_cast<CaseLabelItem *>()) {
    return I->getSourceRange();
  }
  assert(!isNull() && "Null ASTNode doesn't have a source range");
  llvm_unreachable("unsupported AST node");
}

/// Return the location of the start of the statement.
SourceLoc ASTNode::getStartLoc() const {
  return getSourceRange().Start;
}

/// Return the location of the end of the statement.
SourceLoc ASTNode::getEndLoc() const {
  return getSourceRange().End;
}

DeclContext *ASTNode::getAsDeclContext() const {
  if (auto *E = this->dyn_cast<Expr*>()) {
    if (isa<AbstractClosureExpr>(E))
      return static_cast<AbstractClosureExpr*>(E);
  } else if (is<Stmt*>()) {
    return nullptr;
  } else if (auto *D = this->dyn_cast<Decl*>()) {
    if (isa<DeclContext>(D))
      return cast<DeclContext>(D);
  } else if (getOpaqueValue())
    llvm_unreachable("unsupported AST node");
  return nullptr;
}

bool ASTNode::isImplicit() const {
  if (const auto *E = this->dyn_cast<Expr*>())
    return E->isImplicit();
  if (const auto *S = this->dyn_cast<Stmt*>())
    return S->isImplicit();
  if (const auto *D = this->dyn_cast<Decl*>())
    return D->isImplicit();
  if (const auto *P = this->dyn_cast<Pattern*>())
    return P->isImplicit();
  if (const auto *T = this->dyn_cast<TypeRepr*>())
    return false;
  if (const auto *C = this->dyn_cast<StmtConditionElement *>())
    return false;
  if (const auto *I = this->dyn_cast<CaseLabelItem *>())
    return false;
  llvm_unreachable("unsupported AST node");
}

void ASTNode::walk(ASTWalker &Walker) {
  if (auto *E = this->dyn_cast<Expr*>())
    E->walk(Walker);
  else if (auto *S = this->dyn_cast<Stmt*>())
    S->walk(Walker);
  else if (auto *D = this->dyn_cast<Decl*>())
    D->walk(Walker);
  else if (auto *P = this->dyn_cast<Pattern*>())
    P->walk(Walker);
  else if (auto *T = this->dyn_cast<TypeRepr*>())
    T->walk(Walker);
  else if (auto *C = this->dyn_cast<StmtConditionElement *>())
    C->walk(Walker);
  else if (auto *I = this->dyn_cast<CaseLabelItem *>()) {
    if (auto *P = I->getPattern())
      P->walk(Walker);

    if (auto *G = I->getGuardExpr())
      G->walk(Walker);
  } else
    llvm_unreachable("unsupported AST node");
}

void ASTNode::dump(raw_ostream &OS, unsigned Indent) const {
  if (isNull())
    OS << "(null)";
  else if (auto S = dyn_cast<Stmt*>())
    S->dump(OS, /*context=*/nullptr, Indent);
  else if (auto E = dyn_cast<Expr*>())
    E->dump(OS, Indent);
  else if (auto D = dyn_cast<Decl*>())
    D->dump(OS, Indent);
  else if (auto P = dyn_cast<Pattern*>())
    P->dump(OS, Indent);
  else if (auto T = dyn_cast<TypeRepr*>())
    T->print(OS);
  else if (auto *C = dyn_cast<StmtConditionElement *>())
    OS.indent(Indent) << "(statement condition)";
  else if (auto *I = dyn_cast<CaseLabelItem *>()) {
    OS.indent(Indent) << "(case label item)";
  } else
    llvm_unreachable("unsupported AST node");
}

void ASTNode::dump() const {
  dump(llvm::errs());
}

StringRef swift::getTokenText(tok kind) {
  switch(kind) {
#define KEYWORD(KW) case tok::kw_##KW: return #KW;
#define POUND_KEYWORD(KW) case tok::pound_##KW: return "#"#KW;
#define PUNCTUATOR(PUN, TEXT) case tok::PUN: return TEXT;
#include "swift/AST/TokenKinds.def"
  default: return StringRef();
  }
}

#define FUNC(T)                                                               \
bool ASTNode::is##T(T##Kind Kind) const {                                     \
  if (!is<T*>())                                                              \
    return false;                                                             \
  return get<T*>()->getKind() == Kind;                                        \
}
FUNC(Stmt)
FUNC(Expr)
FUNC(Decl)
FUNC(Pattern)
#undef FUNC
