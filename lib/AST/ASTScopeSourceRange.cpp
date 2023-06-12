//===--- ASTScopeSourceRange.cpp - Swift Object-Oriented AST Scope --------===//
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
/// This file implements the source range queries for the ASTScopeImpl ontology.
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
#include "swift/Parse/Lexer.h"
#include "llvm/Support/Compiler.h"
#include <algorithm>

using namespace swift;
using namespace ast_scope;

static SourceLoc getLocAfterExtendedNominal(const ExtensionDecl *);

void ASTScopeImpl::checkSourceRangeBeforeAddingChild(ASTScopeImpl *child,
                                                     const ASTContext &ctx) const {
  // Ignore debugger bindings - they're a special mix of user code and implicit
  // wrapper code that is too difficult to check for consistency.
  if (auto d = getDeclIfAny().getPtrOrNull())
    if (auto *PBD = dyn_cast<PatternBindingDecl>(d))
      if (PBD->isDebuggerBinding())
        return;
  
  auto &sourceMgr = ctx.SourceMgr;

  auto range = getCharSourceRangeOfScope(sourceMgr);

  std::function<bool(CharSourceRange)> containedInParent;
  containedInParent = [&](CharSourceRange childCharRange) {
    // HACK: For code completion. Handle replaced range.
    for (const auto &pair : sourceMgr.getReplacedRanges()) {
      auto originalRange =
          Lexer::getCharSourceRangeFromSourceRange(sourceMgr, pair.first);
      auto newRange =
          Lexer::getCharSourceRangeFromSourceRange(sourceMgr, pair.second);
      if (range.contains(originalRange) &&
          newRange.contains(childCharRange))
        return true;
    }

    if (range.contains(childCharRange))
      return true;

    // If this is from a macro expansion, look at the where the expansion
    // occurred.
    auto childBufferID =
      sourceMgr.findBufferContainingLoc(childCharRange.getStart());
    auto generatedInfo = sourceMgr.getGeneratedSourceInfo(childBufferID);
    if (!generatedInfo)
      return false;

    CharSourceRange expansionRange = generatedInfo->originalSourceRange;
    if (expansionRange.isInvalid())
      return false;

    return containedInParent(expansionRange);
  };

  auto childCharRange = child->getCharSourceRangeOfScope(sourceMgr);

  if (!containedInParent(childCharRange)) {
    auto &out = verificationError() << "child not contained in its parent:\n";
    child->print(out);
    out << "\n***Parent node***\n";
    this->print(out);
    abort();
  }

  if (!storedChildren.empty()) {
    auto previousChild = storedChildren.back();
    auto endOfPreviousChild = previousChild->getCharSourceRangeOfScope(
        sourceMgr).getEnd();

    if (childCharRange.getStart() != endOfPreviousChild &&
        !sourceMgr.isBeforeInBuffer(endOfPreviousChild,
                                    childCharRange.getStart())) {
      auto &out = verificationError() << "child overlaps previous child:\n";
      child->print(out);
      out << "\n***Previous child\n";
      previousChild->print(out);
      out << "\n***Parent node***\n";
      this->print(out);
      abort();
    }
  }
}

#pragma mark getSourceRangeOfThisASTNode

SourceRange SpecializeAttributeScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return specializeAttr->getRange();
}

SourceRange DifferentiableAttributeScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return differentiableAttr->getRange();
}

SourceRange FunctionBodyScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return decl->getOriginalBodySourceRange();
}

SourceRange TopLevelCodeScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return SourceRange(decl->getStartLoc(), endLoc);
}

SourceRange SubscriptDeclScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return decl->getSourceRangeIncludingAttrs();
}

SourceRange MacroDeclScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return decl->getSourceRangeIncludingAttrs();
}

SourceRange MacroDefinitionScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return definition->getSourceRange();
}

SourceRange MacroExpansionDeclScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return decl->getSourceRangeIncludingAttrs();
}

SourceRange
EnumElementScope::getSourceRangeOfThisASTNode(const bool omitAssertions) const {
  return decl->getSourceRange();
}

SourceRange AbstractStmtScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return getStmt()->getSourceRange();
}

SourceRange DefaultArgumentInitializerScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return decl->getStructuralDefaultExpr()->getSourceRange();
}

SourceRange PatternEntryDeclScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  SourceRange range = getPatternEntry().getSourceRange();
  if (endLoc.has_value())
    range.End = *endLoc;
  return range;
}

SourceRange PatternEntryInitializerScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  // TODO: Don't remove the initializer in the rest of the compiler:
  // Search for "When the initializer is removed we don't actually clear the
  // pointer" because we do!
  return initAsWrittenWhenCreated->getSourceRange();
}

SourceRange GenericParamScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  // We want to ensure the extended type is not part of the generic
  // parameter scope.
  if (auto const *const ext = dyn_cast<ExtensionDecl>(holder)) {
    return SourceRange(getLocAfterExtendedNominal(ext), ext->getEndLoc());
  }

  // For a variable, the generic parameter is visible throughout the pattern
  // binding entry.
  if (auto var = dyn_cast<VarDecl>(holder)) {
    if (auto patternBinding = var->getParentPatternBinding()) {
      unsigned index = patternBinding->getPatternEntryIndexForVarDecl(var);
      return patternBinding->getPatternList()[index].getSourceRange();
    }
  }

  // For all other declarations, generic parameters are visible everywhere.
  return holder->getSourceRange();
}

SourceRange ASTSourceFileScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  if (auto bufferID = SF->getBufferID()) {
    auto charRange = getSourceManager().getRangeForBuffer(*bufferID);
    return SourceRange(charRange.getStart(), charRange.getEnd());
  }

  if (SF->getTopLevelItems().empty())
    return SourceRange();

  // Use the source ranges of the declarations in the file.
  return SourceRange(SF->getTopLevelItems().front().getStartLoc(),
                     SF->getTopLevelItems().back().getEndLoc());
}

SourceRange GenericTypeOrExtensionScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return portion->getChildlessSourceRangeOf(this, omitAssertions);
}

SourceRange GenericTypeOrExtensionWholePortion::getChildlessSourceRangeOf(
    const GenericTypeOrExtensionScope *scope, const bool omitAssertions) const {
  auto *d = scope->getDecl();
  auto r = d->getSourceRangeIncludingAttrs();
  if (r.Start.isValid()) {
    ASTScopeAssert(r.End.isValid(), "Start valid imples end valid.");
    return scope->moveStartPastExtendedNominal(r);
  }
  return d->getSourceRange();
}

SourceRange
ExtensionScope::moveStartPastExtendedNominal(const SourceRange sr) const {
  const auto afterExtendedNominal = getLocAfterExtendedNominal(decl);
  // Illegal code can have an endLoc that is before the end of the
  // ExtendedNominal
  if (getSourceManager().isBeforeInBuffer(sr.End, afterExtendedNominal)) {
    // Must not have the source range returned include the extended nominal
    return SourceRange(sr.Start, sr.Start);
  }
  return SourceRange(afterExtendedNominal, sr.End);
}

SourceRange
GenericTypeScope::moveStartPastExtendedNominal(const SourceRange sr) const {
  // There is no extended nominal
  return sr;
}

SourceRange GenericTypeOrExtensionWherePortion::getChildlessSourceRangeOf(
    const GenericTypeOrExtensionScope *scope, const bool omitAssertions) const {
  return scope->getGenericContext()->getTrailingWhereClause()->getSourceRange();
}

SourceRange IterableTypeBodyPortion::getChildlessSourceRangeOf(
    const GenericTypeOrExtensionScope *scope, const bool omitAssertions) const {
  auto *d = scope->getDecl();
  if (auto *nt = dyn_cast<NominalTypeDecl>(d))
    return nt->getBraces();
  if (auto *e = dyn_cast<ExtensionDecl>(d))
    return e->getBraces();
  if (omitAssertions)
    return SourceRange();
  ASTScope_unreachable("No body!");
}

SourceRange AbstractFunctionDeclScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  // For a get/put accessor	 all of the parameters are implicit, so start
  // them at the start location of the accessor.
  auto r = decl->getSourceRangeIncludingAttrs();
  if (r.Start.isValid()) {
    ASTScopeAssert(r.End.isValid(), "Start valid imples end valid.");
    return r;
  }
  return decl->getOriginalBodySourceRange();
}

SourceRange ParameterListScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return params->getSourceRange();
}

SourceRange ForEachPatternScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  // The scope of the pattern extends from the 'where' expression (if present)
  // until the end of the body.
  if (stmt->getWhere())
    return SourceRange(stmt->getWhere()->getStartLoc(),
                       stmt->getBody()->getEndLoc());

  // Otherwise, scope of the pattern covers the body.
  return stmt->getBody()->getSourceRange();
}

SourceRange
CaseLabelItemScope::getSourceRangeOfThisASTNode(const bool omitAssertions) const {
  return item.getGuardExpr()->getSourceRange();
}

SourceRange
CaseStmtBodyScope::getSourceRangeOfThisASTNode(const bool omitAssertions) const {
  return stmt->getBody()->getSourceRange();
}

SourceRange
BraceStmtScope::getSourceRangeOfThisASTNode(const bool omitAssertions) const {
  // The brace statements that represent closures start their scope at the
  // 'in' keyword, when present.
  if (auto anyClosure = parentClosureIfAny()) {
    if (auto closure = dyn_cast<ClosureExpr>(parentClosureIfAny().get())) {
      if (closure->getInLoc().isValid()) {
        return SourceRange(closure->getInLoc(), endLoc);
      }
    }
  }
  return SourceRange(stmt->getStartLoc(), endLoc);
}

SourceRange ConditionalClauseInitializerScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return initializer->getSourceRange();
}

SourceRange ConditionalClausePatternUseScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return SourceRange(sec.getInitializer()->getStartLoc(), endLoc);
}

SourceRange
CaptureListScope::getSourceRangeOfThisASTNode(const bool omitAssertions) const {
  if (auto autoClosure = dyn_cast<AutoClosureExpr>(expr->getClosureBody())) {
    return autoClosure->getSourceRange();
  }
  auto closureExpr = cast<ClosureExpr>(expr->getClosureBody());
  if (!omitAssertions)
    ASTScopeAssert(closureExpr->getInLoc().isValid(),
                   "We don't create these if no in loc");
  return SourceRange(closureExpr->getInLoc(), closureExpr->getEndLoc());
}

SourceRange ClosureParametersScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  if (auto autoClosure = dyn_cast<AutoClosureExpr>(closureExpr)) {
    return autoClosure->getSourceRange();
  }
  auto explicitClosureExpr = cast<ClosureExpr>(closureExpr);
  if (explicitClosureExpr->getInLoc().isValid()) {
    return SourceRange(explicitClosureExpr->getInLoc(),
                       explicitClosureExpr->getEndLoc());
  }

  return explicitClosureExpr->getSourceRange();
}

SourceRange CustomAttributeScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return attr->getRange();
}

SourceRange GuardStmtScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return SourceRange(getStmt()->getStartLoc(), endLoc);
}

SourceRange GuardStmtBodyScope::getSourceRangeOfThisASTNode(
    const bool omitAssertions) const {
  return body->getSourceRange();
}

#pragma mark source range caching

CharSourceRange
ASTScopeImpl::getCharSourceRangeOfScope(SourceManager &SM,
                                        bool omitAssertions) const {
  if (!isCharSourceRangeCached()) {
    auto range = getSourceRangeOfThisASTNode(omitAssertions);
    ASTScopeAssert(range.isValid(), "scope has invalid source range");
    ASTScopeAssert(SM.isBeforeInBuffer(range.Start, range.End) ||
                   range.Start == range.End,
                   "scope source range ends before start");

    cachedCharSourceRange =
      Lexer::getCharSourceRangeFromSourceRange(SM, range);
  }

  return *cachedCharSourceRange;
}

bool ASTScopeImpl::isCharSourceRangeCached() const {
  return cachedCharSourceRange.has_value();
}

SourceLoc getLocAfterExtendedNominal(const ExtensionDecl *const ext) {
  const auto *const etr = ext->getExtendedTypeRepr();
  if (!etr)
    return ext->getStartLoc();
  const auto &SM = ext->getASTContext().SourceMgr;
  return Lexer::getCharSourceRangeFromSourceRange(SM, etr->getSourceRange())
      .getEnd();
}

SourceLoc ast_scope::extractNearestSourceLoc(
    std::tuple<ASTScopeImpl *, ScopeCreator *> scopeAndCreator) {
  const ASTScopeImpl *scope = std::get<0>(scopeAndCreator);
  return scope->getSourceRangeOfThisASTNode().Start;
}
