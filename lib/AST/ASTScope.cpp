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
  return !getParent() ? nullptr : getParent().get()->getClosureIfClosureScope();
}

NullablePtr<AbstractClosureExpr> ASTScopeImpl::getClosureIfClosureScope() const {
  return nullptr;
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
  return getParent().get()->getASTContext();
}

#pragma mark getClassName

std::string GenericTypeOrExtensionScope::getClassName() const {
  return declKindName() + portionName() + "Scope";
}

#define DEFINE_GET_CLASS_NAME(Name)                                            \
  std::string Name::getClassName() const { return #Name; }

DEFINE_GET_CLASS_NAME(ASTSourceFileScope)
DEFINE_GET_CLASS_NAME(GenericParamScope)
DEFINE_GET_CLASS_NAME(AbstractFunctionDeclScope)
DEFINE_GET_CLASS_NAME(ParameterListScope)
DEFINE_GET_CLASS_NAME(FunctionBodyScope)
DEFINE_GET_CLASS_NAME(DefaultArgumentInitializerScope)
DEFINE_GET_CLASS_NAME(CustomAttributeScope)
DEFINE_GET_CLASS_NAME(PatternEntryDeclScope)
DEFINE_GET_CLASS_NAME(PatternEntryInitializerScope)
DEFINE_GET_CLASS_NAME(ConditionalClausePatternUseScope)
DEFINE_GET_CLASS_NAME(ConditionalClauseInitializerScope)
DEFINE_GET_CLASS_NAME(CaptureListScope)
DEFINE_GET_CLASS_NAME(ClosureParametersScope)
DEFINE_GET_CLASS_NAME(TopLevelCodeScope)
DEFINE_GET_CLASS_NAME(SpecializeAttributeScope)
DEFINE_GET_CLASS_NAME(DifferentiableAttributeScope)
DEFINE_GET_CLASS_NAME(SubscriptDeclScope)
DEFINE_GET_CLASS_NAME(EnumElementScope)
DEFINE_GET_CLASS_NAME(MacroDeclScope)
DEFINE_GET_CLASS_NAME(MacroDefinitionScope)
DEFINE_GET_CLASS_NAME(MacroExpansionDeclScope)
DEFINE_GET_CLASS_NAME(IfStmtScope)
DEFINE_GET_CLASS_NAME(WhileStmtScope)
DEFINE_GET_CLASS_NAME(GuardStmtScope)
DEFINE_GET_CLASS_NAME(GuardStmtBodyScope)
DEFINE_GET_CLASS_NAME(RepeatWhileScope)
DEFINE_GET_CLASS_NAME(DoStmtScope)
DEFINE_GET_CLASS_NAME(DoCatchStmtScope)
DEFINE_GET_CLASS_NAME(SwitchStmtScope)
DEFINE_GET_CLASS_NAME(ForEachStmtScope)
DEFINE_GET_CLASS_NAME(ForEachPatternScope)
DEFINE_GET_CLASS_NAME(CaseStmtScope)
DEFINE_GET_CLASS_NAME(CaseLabelItemScope)
DEFINE_GET_CLASS_NAME(CaseStmtBodyScope)
DEFINE_GET_CLASS_NAME(BraceStmtScope)

#undef DEFINE_GET_CLASS_NAME

#pragma mark getSourceFile

const SourceFile *ASTScopeImpl::getSourceFile() const {
  return getParent().get()->getSourceFile();
}

const SourceFile *ASTSourceFileScope::getSourceFile() const { return SF; }

ASTContext &ASTSourceFileScope::getASTContext() const {
  return SF->getASTContext();
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
