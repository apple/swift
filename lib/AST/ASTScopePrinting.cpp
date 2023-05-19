//===--- ASTScopePrinting.cpp - Swift Object-Oriented AST Scope -----------===//
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
///
/// This file implements the printing functions of the ASTScopeImpl ontology.
///
//===----------------------------------------------------------------------===//
#include "swift/AST/ASTScope.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/Support/Compiler.h"
#include <algorithm>

using namespace swift;
using namespace ast_scope;

#pragma mark dumping

void ASTScopeImpl::dump() const { print(llvm::errs(), 0, false); }

void ASTScopeImpl::dumpOneScopeMapLocation(
    std::pair<unsigned, unsigned> lineColumn) {
  auto bufferID = getSourceFile()->getBufferID();
  if (!bufferID) {
    llvm::errs() << "***No buffer, dumping all scopes***";
    print(llvm::errs());
    return;
  }
  SourceLoc loc = getSourceManager().getLocForLineCol(
      *bufferID, lineColumn.first, lineColumn.second);
  if (loc.isInvalid())
    return;

  llvm::errs() << "***Scope at " << lineColumn.first << ":" << lineColumn.second
               << "***\n";
  auto *locScope = findInnermostEnclosingScope(loc, &llvm::errs());
  locScope->print(llvm::errs(), 0, false, false);

  namelookup::ASTScopeDeclGatherer gatherer;
  // Print the local bindings introduced by this scope.
  locScope->lookupLocalsOrMembers(gatherer);
  if (!gatherer.getDecls().empty()) {
    llvm::errs() << "Local bindings: ";
    llvm::interleave(
        gatherer.getDecls().begin(), gatherer.getDecls().end(),
        [&](ValueDecl *value) { llvm::errs() << value->getName(); },
        [&]() { llvm::errs() << " "; });
    llvm::errs() << "\n";
  }
}

llvm::raw_ostream &ASTScopeImpl::verificationError() const {
  return llvm::errs() << "ASTScopeImpl verification error in source file '"
                      << getSourceFile()->getFilename() << "': ";
}

#pragma mark printing

void ASTScopeImpl::print(llvm::raw_ostream &out, unsigned level, bool lastChild,
                         bool printChildren) const {
  // Indent for levels 2+.
  if (level > 1)
    out.indent((level - 1) * 2);

  // Print child marker and leading '-' for levels 1+.
  if (level > 0)
    out << (lastChild ? '`' : '|') << '-';

  out << getClassName();
  if (auto *a = addressForPrinting().getPtrOrNull())
    out << " " << a;
  out << ", ";
  if (auto *d = getDeclIfAny().getPtrOrNull()) {
    if (d->isImplicit())
      out << "implicit ";
  }
  printRange(out);
  out << " ";
  printSpecifics(out);
  out << "\n";

  if (printChildren) {
    for (unsigned i : indices(getChildren())) {
      getChildren()[i]->print(out, level + 1,
                              /*lastChild=*/i == getChildren().size() - 1);
    }
  }
}

static void printSourceRange(llvm::raw_ostream &out, const SourceRange range,
                             SourceManager &SM) {
  if (range.isInvalid()) {
    out << "[invalid source range]";
    return;
  }

  auto startLineAndCol = SM.getPresumedLineAndColumnForLoc(range.Start);
  auto endLineAndCol = SM.getPresumedLineAndColumnForLoc(range.End);

  out << "[" << startLineAndCol.first << ":" << startLineAndCol.second << " - "
      << endLineAndCol.first << ":" << endLineAndCol.second << "]";
}

void ASTScopeImpl::printRange(llvm::raw_ostream &out) const {
  SourceRange range = getSourceRangeOfThisASTNode(/*omitAssertions=*/true);
  printSourceRange(out, range, getSourceManager());
}

#pragma mark printSpecifics


void ASTSourceFileScope::printSpecifics(
    llvm::raw_ostream &out) const {
  out << "'" << SF->getFilename() << "'";
}

NullablePtr<const void> ASTScopeImpl::addressForPrinting() const {
  if (auto *p = getDeclIfAny().getPtrOrNull())
    return p;
  if (auto *p = getStmtIfAny().getPtrOrNull())
    return p;
  if (auto *p = getExprIfAny().getPtrOrNull())
    return p;
  return nullptr;
}

void GenericTypeOrExtensionScope::printSpecifics(llvm::raw_ostream &out) const {
  if (shouldHaveABody() && !doesDeclHaveABody())
    out << "<no body>";
  else if (auto *n = getCorrespondingNominalTypeDecl().getPtrOrNull())
    out << "'" << n->getName() << "'";
  else
    out << "<no extended nominal?!>";
}

void GenericParamScope::printSpecifics(llvm::raw_ostream &out) const {
  out << "param " << index;
  auto *genericTypeParamDecl = paramList->getParams()[index];
  out << " '";
  genericTypeParamDecl->print(out);
  out << "'";
}

void AbstractFunctionDeclScope::printSpecifics(llvm::raw_ostream &out) const {
  out << "'" << decl->getName() << "'";
}

void AbstractPatternEntryScope::printSpecifics(llvm::raw_ostream &out) const {
  out << "entry " << patternEntryIndex;
  getPattern()->forEachVariable([&](VarDecl *vd) {
    out << " '" << vd->getName() << "'";
  });
}

void SubscriptDeclScope::printSpecifics(llvm::raw_ostream &out) const {
  decl->dumpRef(out);
}

void MacroDeclScope::printSpecifics(llvm::raw_ostream &out) const {
  decl->dumpRef(out);
}

void MacroExpansionDeclScope::printSpecifics(llvm::raw_ostream &out) const {
  out << decl->getMacroName();
}

void ConditionalClausePatternUseScope::printSpecifics(
    llvm::raw_ostream &out) const {
  sec.getPattern()->print(out);
}

bool GenericTypeOrExtensionScope::doesDeclHaveABody() const { return false; }

bool IterableTypeScope::doesDeclHaveABody() const {
  return getBraces().Start != getBraces().End;
}

void ast_scope::simple_display(llvm::raw_ostream &out,
                               const ASTScopeImpl *scope) {
  // Cannot call scope->print(out) because printing an ASTFunctionBodyScope
  // gets the source range which can cause a request to parse it.
  // That in turn causes the request dependency printing code to blow up
  // as the AnyRequest ends up with a null.
  out << scope->getClassName() << "\n";
}
