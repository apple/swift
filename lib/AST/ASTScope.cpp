//===--- ASTScopeImpl.cpp - Swift Object-Oriented AST Scope ---------------===//
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
/// This file implements the common functions of the 49 ontology.
///
//===----------------------------------------------------------------------===//
#include "swift/AST/ASTScope.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/NullablePtr.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/Support/Compiler.h"
#include <algorithm>

using namespace swift;
using namespace ast_scope;

#pragma mark ASTScope

void ASTScope::unqualifiedLookup(
    SourceFile *SF, SourceLoc loc,
    namelookup::AbstractASTScopeDeclConsumer &consumer) {
  if (loc.isValid()) {
    SF = SF->getParentModule()->getSourceFileContainingLocation(loc);
  }

  if (auto *s = SF->getASTContext().Stats)
    ++s->getFrontendCounters().NumASTScopeLookups;
  ASTScopeImpl::unqualifiedLookup(SF, loc, consumer);
}

llvm::SmallVector<LabeledStmt *, 4> ASTScope::lookupLabeledStmts(
    SourceFile *sourceFile, SourceLoc loc) {
  return ASTScopeImpl::lookupLabeledStmts(sourceFile, loc);
}

std::pair<CaseStmt *, CaseStmt *> ASTScope::lookupFallthroughSourceAndDest(
    SourceFile *sourceFile, SourceLoc loc) {
  return ASTScopeImpl::lookupFallthroughSourceAndDest(sourceFile, loc);
}

void ASTScope::lookupEnclosingMacroScope(
    SourceFile *sourceFile, SourceLoc loc,
    llvm::function_ref<bool(PotentialMacro)> body) {
  return ASTScopeImpl::lookupEnclosingMacroScope(sourceFile, loc, body);
}

CatchNode ASTScope::lookupCatchNode(ModuleDecl *module, SourceLoc loc) {
  return ASTScopeImpl::lookupCatchNode(module, loc);
}

#if SWIFT_COMPILER_IS_MSVC
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

void ASTScope::dump() const { impl->dump(); }

#if SWIFT_COMPILER_IS_MSVC
#pragma warning(pop)
#endif

void ASTScope::print(llvm::raw_ostream &out) const { impl->print(out); }
void ASTScope::dumpOneScopeMapLocation(std::pair<unsigned, unsigned> lineCol) {
  impl->dumpOneScopeMapLocation(lineCol);
}

#pragma mark ASTScopeImpl


const PatternBindingEntry &AbstractPatternEntryScope::getPatternEntry() const {
  return decl->getPatternList()[patternEntryIndex];
}

Pattern *AbstractPatternEntryScope::getPattern() const {
  return getPatternEntry().getPattern();
}

NullablePtr<AbstractClosureExpr> BraceStmtScope::parentClosureIfAny() const {
  if (auto parent = getParent()) {
    if (auto closureScope = dyn_cast<ClosureParametersScope>(parent.get()))
      return closureScope->closureExpr;
  }

  return nullptr;
}

std::string ASTScopeImpl::getClassName() const {
  // GenericTypeOrExtensionScope provides a custom implementation that deals
  // with declaration names and "portions".
  if (auto generic = dyn_cast<GenericTypeOrExtensionScope>(this))
    return generic->getClassName();

  switch (getKind()) {
#define SCOPE_NODE(Name) case ScopeKind::Name: return #Name "Scope";
#include "swift/AST/ASTScopeNodes.def"
  }
}

std::string GenericTypeOrExtensionScope::getClassName() const {
  return declKindName() + portionName() + "Scope";
}

NullablePtr<Decl> ASTScopeImpl::getDeclIfAny() const {
  switch (getKind()) {
    // Declaration scope nodes extract the decl directly.
#define DECL_SCOPE_NODE(Name) \
    case ScopeKind::Name: return cast<Name##Scope>(this)->getDecl();
#define SCOPE_NODE(Name)
#include "swift/AST/ASTScopeNodes.def"

    // Everything else returns nullptr.
#define DECL_SCOPE_NODE(Name)
#define SCOPE_NODE(Name) case ScopeKind::Name:
#include "swift/AST/ASTScopeNodes.def"
      return nullptr;
  }
}

NullablePtr<Stmt> ASTScopeImpl::getStmtIfAny() const {
  switch (getKind()) {
    // Statement scope nodes extract the statement directly.
#define STMT_SCOPE_NODE(Name) \
    case ScopeKind::Name: return cast<Name##Scope>(this)->getStmt();
#define SCOPE_NODE(Name)
#include "swift/AST/ASTScopeNodes.def"

    // Everything else returns nullptr.
#define STMT_SCOPE_NODE(Name)
#define SCOPE_NODE(Name) case ScopeKind::Name:
#include "swift/AST/ASTScopeNodes.def"
      return nullptr;
  }
}

NullablePtr<Expr> ASTScopeImpl::getExprIfAny() const {
  switch (getKind()) {
    // Expression scope nodes extract the statement directly.
#define EXPR_SCOPE_NODE(Name) \
    case ScopeKind::Name: return cast<Name##Scope>(this)->getExpr();
#define SCOPE_NODE(Name)
#include "swift/AST/ASTScopeNodes.def"

    // Everything else returns nullptr.
#define EXPR_SCOPE_NODE(Name)
#define SCOPE_NODE(Name) case ScopeKind::Name:
#include "swift/AST/ASTScopeNodes.def"
      return nullptr;
  }
}

SourceManager &ASTScopeImpl::getSourceManager() const {
  return getASTContext().SourceMgr;
}

Stmt *LabeledConditionalStmtScope::getStmt() const {
  return getLabeledConditionalStmt();
}

#pragma mark getLabeledConditionalStmt
LabeledConditionalStmt *IfStmtScope::getLabeledConditionalStmt() const {
  return stmt;
}
LabeledConditionalStmt *WhileStmtScope::getLabeledConditionalStmt() const {
  return stmt;
}
LabeledConditionalStmt *GuardStmtScope::getLabeledConditionalStmt() const {
  return stmt;
}


#pragma mark getASTContext

ASTContext &ASTScopeImpl::getASTContext() const {
  if (auto d = getDeclIfAny())
    return d.get()->getASTContext();
  if (auto sfScope = dyn_cast<ASTSourceFileScope>(this))
    return sfScope->SF->getASTContext();
  return getParent().get()->getASTContext();
}

#pragma mark getSourceFile

const SourceFile *ASTScopeImpl::getSourceFile() const {
  if (auto sourceFileScope = dyn_cast<ASTSourceFileScope>(this))
    return sourceFileScope->SF;

  return getParent().get()->getSourceFile();
}

SourceRange ExtensionScope::getBraces() const { return decl->getBraces(); }

SourceRange NominalTypeScope::getBraces() const { return decl->getBraces(); }

NullablePtr<NominalTypeDecl>
ExtensionScope::getCorrespondingNominalTypeDecl() const {
  return decl->getExtendedNominal();
}

void ASTScopeImpl::preOrderDo(function_ref<void(ASTScopeImpl *)> fn) {
  fn(this);
  preOrderChildrenDo(fn);
}
void ASTScopeImpl::preOrderChildrenDo(function_ref<void(ASTScopeImpl *)> fn) {
  for (auto *child : getChildren())
    child->preOrderDo(fn);
}

void ASTScopeImpl::postOrderDo(function_ref<void(ASTScopeImpl *)> fn) {
  for (auto *child : getChildren())
    child->postOrderDo(fn);
  fn(this);
}
