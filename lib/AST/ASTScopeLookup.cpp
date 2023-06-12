//===--- ASTScopeLookup.cpp - Swift Object-Oriented AST Scope -------------===//
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
/// This file implements the lookup functionality of the ASTScopeImpl ontology.
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
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Parse/Lexer.h"
#include "llvm/Support/Compiler.h"

using namespace swift;
using namespace namelookup;
using namespace ast_scope;

void ASTScopeImpl::unqualifiedLookup(
    SourceFile *sourceFile, const SourceLoc loc, DeclConsumer consumer) {
  const auto *start =
      findStartingScopeForLookup(sourceFile, loc);
  if (start)
    start->lookup(nullptr, nullptr, consumer);
}

const ASTScopeImpl *ASTScopeImpl::findStartingScopeForLookup(
    SourceFile *sourceFile, const SourceLoc loc) {
  auto *const fileScope = sourceFile->getScope().impl;
  const auto *innermost = fileScope->findInnermostEnclosingScope(loc, nullptr);
  ASTScopeAssert(innermost->getWasExpanded(),
                 "If looking in a scope, it must have been expanded.");

  return innermost;
}

ASTScopeImpl *
ASTScopeImpl::findInnermostEnclosingScope(SourceLoc loc,
                                          NullablePtr<raw_ostream> os) {
  return findInnermostEnclosingScopeImpl(loc, os, getSourceManager(),
                                         getScopeCreator());
}

ASTScopeImpl *ASTScopeImpl::findInnermostEnclosingScopeImpl(
    SourceLoc loc, NullablePtr<raw_ostream> os, SourceManager &sourceMgr,
    ScopeCreator &scopeCreator) {
  if (!getWasExpanded())
    expandAndBeCurrent(scopeCreator);
  auto child = findChildContaining(loc, sourceMgr);
  if (!child)
    return this;
  return child.get()->findInnermostEnclosingScopeImpl(loc, os, sourceMgr,
                                                      scopeCreator);
}

/// If the \p loc is in a new buffer but \p range is not, consider the location
/// is at the start of replaced range. Otherwise, returns \p loc as is.
static SourceLoc translateLocForReplacedRange(SourceManager &sourceMgr,
                                              CharSourceRange range,
                                              SourceLoc loc) {
  for (const auto &pair : sourceMgr.getReplacedRanges()) {
    if (sourceMgr.rangeContainsTokenLoc(pair.second, loc) &&
        !sourceMgr.rangeContainsTokenLoc(pair.second, range.getStart())) {
      return pair.first.Start;
    }
  }
  return loc;
}

NullablePtr<ASTScopeImpl>
ASTScopeImpl::findChildContaining(SourceLoc loc,
                                  SourceManager &sourceMgr) const {
  auto *moduleDecl = this->getSourceFile()->getParentModule();
  auto *locSourceFile = moduleDecl->getSourceFileContainingLocation(loc);

  // Use binary search to find the child that contains this location.
  auto *const *child = llvm::lower_bound(
      getChildren(), loc,
      [&](const ASTScopeImpl *scope, SourceLoc loc) {
        auto rangeOfScope = scope->getCharSourceRangeOfScope(sourceMgr);
        ASTScopeAssert(!sourceMgr.isBeforeInBuffer(rangeOfScope.getEnd(),
                                                   rangeOfScope.getStart()),
                       "Source range is backwards");

        // If the scope source range and the loc are in two different source
        // files, one or both of them are in a macro expansion buffer.

        // Note that `scope->getSourceFile()` returns the root of the source tree,
        // not the source file containing the location of the ASTScope.
        auto scopeStart = scope->getSourceRangeOfThisASTNode().Start;
        auto *scopeSourceFile = moduleDecl->getSourceFileContainingLocation(scopeStart);

        if (scopeSourceFile != locSourceFile) {
          // To compare a source location that is possibly inside a macro expansion
          // with a source range that is also possibly in a macro expansion (not
          // necessarily the same one as before) we need to find the LCA in the
          // source file tree of macro expansions, and compare the original source
          // ranges within that common ancestor. We can't walk all the way up to the
          // source file containing the parent scope we're searching the children of,
          // because two independent (possibly nested) macro expansions can have the
          // same original source range in that file; freestanding and peer macros
          // mean that we can have arbitrarily nested macro expansions that all add
          // declarations to the same scope, that all originate from a single macro
          // invocation in the original source file.

          // A map from enclosing source files to original source ranges of the macro
          // expansions within that file, recording the chain of macro expansions for
          // the given scope.
          llvm::SmallDenseMap<const SourceFile *, CharSourceRange>
              scopeExpansions;

          // Walk up the chain of macro expansion buffers for the scope, recording the
          // original source range of the macro expansion along the way using generated
          // source info.
          auto *scopeExpansion = scopeSourceFile;
          scopeExpansions[scopeExpansion] =
              Lexer::getCharSourceRangeFromSourceRange(
                sourceMgr, scope->getSourceRangeOfThisASTNode());
          while (auto *ancestor = scopeExpansion->getEnclosingSourceFile()) {
            auto generatedInfo =
                sourceMgr.getGeneratedSourceInfo(*scopeExpansion->getBufferID());
            scopeExpansions[ancestor] = generatedInfo->originalSourceRange;
            scopeExpansion = ancestor;
          }

          // Walk up the chain of macro expansion buffers for the source loc we're
          // searching for to find the LCA using `scopeExpansions`.
          auto *potentialLCA = locSourceFile;
          auto expansionLoc = loc;
          while (potentialLCA) {
            auto scopeExpansion = scopeExpansions.find(potentialLCA);
            if (scopeExpansion != scopeExpansions.end()) {
              // Take the original expansion range within the LCA of the loc and
              // the scope to compare.
              rangeOfScope = scopeExpansion->second;
              loc = expansionLoc;
              break;
            }

            auto generatedInfo =
                sourceMgr.getGeneratedSourceInfo(*potentialLCA->getBufferID());
            if (generatedInfo)
              expansionLoc = generatedInfo->originalSourceRange.getStart();
            potentialLCA = potentialLCA->getEnclosingSourceFile();
          }
        }

        loc = translateLocForReplacedRange(sourceMgr, rangeOfScope, loc);
        return (rangeOfScope.getEnd() == loc ||
                sourceMgr.isBeforeInBuffer(rangeOfScope.getEnd(), loc));
      });

  if (child != getChildren().end()) {
    auto rangeOfScope = (*child)->getCharSourceRangeOfScope(sourceMgr);
    loc = translateLocForReplacedRange(sourceMgr, rangeOfScope, loc);
    if (rangeOfScope.contains(loc))
      return *child;
  }

  return nullptr;
}

#pragma mark lookup methods that run once per scope

void ASTScopeImpl::lookup(const NullablePtr<const ASTScopeImpl> limit,
                          NullablePtr<const GenericParamList> lastListSearched,
                          DeclConsumer consumer) const {

#ifndef NDEBUG
  consumer.startingNextLookupStep();
#endif

  // Certain illegal nestings, e.g. protocol nestled inside a struct,
  // require that lookup stop at the outer scope.
  if (this == limit.getPtrOrNull()) {
#ifndef NDEBUG
    consumer.finishingLookup("limit return");
#endif
    return;
  }

  // Look for generics before members in violation of lexical ordering because
  // you can say "self.name" to get a name shadowed by a generic but you
  // can't do the opposite to get a generic shadowed by a name.
  const auto doneAndListSearched =
      lookInMyGenericParameters(lastListSearched, consumer);
  if (doneAndListSearched.first)
    return;

  if (lookupLocalsOrMembers(consumer))
    return;

  const auto *const lookupParent = getLookupParent().getPtrOrNull();
  if (!lookupParent) {
#ifndef NDEBUG
    consumer.finishingLookup("Finished lookup; no parent");
#endif
    return;
  }

  // If there is no limit and this scope induces one, pass that on.
  const NullablePtr<const ASTScopeImpl> limitForParent =
      limit ? limit : getLookupLimit();

  return lookupParent->lookup(limitForParent, lastListSearched,
                              consumer);
}

#pragma mark genericParams()

NullablePtr<const GenericParamList> ASTScopeImpl::genericParams() const {
  return nullptr;
}
NullablePtr<const GenericParamList>
AbstractFunctionDeclScope::genericParams() const {
  return decl->getParsedGenericParams();
}
NullablePtr<const GenericParamList> SubscriptDeclScope::genericParams() const {
  return decl->getParsedGenericParams();
}
NullablePtr<const GenericParamList> GenericTypeScope::genericParams() const {
  // For Decls:
  // WAIT, WHAT?! Isn't this covered by the GenericParamScope
  // lookupLocalsOrMembers? No, that's for use of generics in the body. This is
  // for generic restrictions.

  // For Bodies:
  // Sigh... These must be here so that from body, we search generics before
  // members. But they also must be on the Decl scope for lookups starting from
  // generic parameters, where clauses, etc.
  auto *context = getGenericContext();
  if (isa<TypeAliasDecl>(context))
    return context->getParsedGenericParams();
  return context->getGenericParams();
}
NullablePtr<const GenericParamList> ExtensionScope::genericParams() const {
  return decl->getGenericParams();
}
NullablePtr<const GenericParamList> MacroDeclScope::genericParams() const {
  return decl->getParsedGenericParams();
}

bool MacroDeclScope::lookupLocalsOrMembers(
    DeclConsumer consumer) const {
  if (auto *paramList = decl->parameterList) {
    for (auto *paramDecl : *paramList)
      if (consumer.consume({paramDecl}))
        return true;
  }

  return false;
}

#pragma mark lookInMyGenericParameters

std::pair<bool, NullablePtr<const GenericParamList>>
ASTScopeImpl::lookInMyGenericParameters(
    NullablePtr<const GenericParamList> formerListSearched,
    ASTScopeImpl::DeclConsumer consumer) const {
  auto listToSearch = genericParams();
  if (listToSearch == formerListSearched)
    return std::make_pair(false, formerListSearched);

  // For extensions of nested types, must check outer parameters
  for (auto *params = listToSearch.getPtrOrNull(); params;
       params = params->getOuterParameters()) {
    if (lookInGenericParametersOf(params, consumer))
      return std::make_pair(true, listToSearch);
  }
  return std::make_pair(false, listToSearch);
}

bool ASTScopeImpl::lookInGenericParametersOf(
    const NullablePtr<const GenericParamList> paramList,
    ASTScopeImpl::DeclConsumer consumer) {
  if (!paramList)
    return false;
  SmallVector<ValueDecl *, 32> bindings;
  for (auto *param : paramList.get()->getParams())
    bindings.push_back(param);
  if (consumer.consume(bindings))
    return true;
  return false;
}

#pragma mark looking in locals or members - members

bool ASTScopeImpl::lookupLocalsOrMembers(DeclConsumer) const {
  return false; // many kinds of scopes have none
}

bool GenericTypeOrExtensionScope::lookupLocalsOrMembers(
    ASTScopeImpl::DeclConsumer consumer) const {
  return portion->lookupMembersOf(this, consumer);
}

bool Portion::lookupMembersOf(const GenericTypeOrExtensionScope *,
                              ASTScopeImpl::DeclConsumer) const {
  return false;
}

bool GenericTypeOrExtensionWhereOrBodyPortion::lookupMembersOf(
    const GenericTypeOrExtensionScope *scope,
    ASTScopeImpl::DeclConsumer consumer) const {
  if (scope->getCorrespondingNominalTypeDecl().isNull())
    return false;
  return consumer.lookInMembers(scope->getGenericContext());
}

bool GenericTypeOrExtensionWherePortion::lookupMembersOf(
    const GenericTypeOrExtensionScope *scope,
    ASTScopeImpl::DeclConsumer consumer) const {
  if (!scope->areMembersVisibleFromWhereClause())
    return false;

  return GenericTypeOrExtensionWhereOrBodyPortion::lookupMembersOf(
    scope, consumer);
}

bool GenericTypeOrExtensionScope::areMembersVisibleFromWhereClause() const {
  auto *decl = getDecl();
  return isa<ProtocolDecl>(decl) || isa<ExtensionDecl>(decl);
}

#pragma mark custom lookup parent behavior

NullablePtr<const ASTScopeImpl>
PatternEntryInitializerScope::getLookupParent() const {
  auto parent = getParent().get();

  // Skip generic parameter scopes, which occur here due to named opaque
  // result types.
  // FIXME: Proper isa/dyn_cast support would be better than a string
  // comparison here.
  while (parent->getClassName() == "GenericParamScope")
    parent = parent->getLookupParent().get();

  ASTScopeAssert(parent->getClassName() == "PatternEntryDeclScope",
                 "PatternEntryInitializerScope in unexpected place");

  // Lookups from inside a pattern binding initializer skip the parent
  // scope that introduces bindings bound by the pattern, since we
  // want this to work:
  //
  // func f(x: Int) {
  //   let x = x
  //   print(x)
  // }
  return parent->getLookupParent();
}

NullablePtr<const ASTScopeImpl>
ConditionalClauseInitializerScope::getLookupParent() const {
  auto parent = getParent().get();
  ASTScopeAssert(parent->getClassName() == "ConditionalClausePatternUseScope",
                 "ConditionalClauseInitializerScope in unexpected place");

  // Lookups from inside a conditional clause initializer skip the parent
  // scope that introduces bindings bound by the pattern, since we
  // want this to work:
  //
  // func f(x: Int?) {
  //   guard let x = x else { return }
  //   print(x)
  // }
  return parent->getLookupParent();
}

#pragma mark looking in locals or members - locals

bool GenericParamScope::lookupLocalsOrMembers(DeclConsumer consumer) const {
  auto *param = paramList->getParams()[index];
  return consumer.consume({param});
}

bool PatternEntryDeclScope::lookupLocalsOrMembers(DeclConsumer consumer) const {
  if (!isLocalBinding)
    return false;
  return lookupLocalBindingsInPattern(getPattern(), consumer);
}

bool ForEachPatternScope::lookupLocalsOrMembers(DeclConsumer consumer) const {
  return lookupLocalBindingsInPattern(stmt->getPattern(), consumer);
}

bool CaseLabelItemScope::lookupLocalsOrMembers(DeclConsumer consumer) const {
  return lookupLocalBindingsInPattern(item.getPattern(), consumer);
}

bool CaseStmtBodyScope::lookupLocalsOrMembers(DeclConsumer consumer) const {
  for (auto *var : stmt->getCaseBodyVariablesOrEmptyArray())
    if (consumer.consume({var}))
        return true;

  return false;
}

bool FunctionBodyScope::lookupLocalsOrMembers(
    DeclConsumer consumer) const {
  if (auto *paramList = decl->getParameters()) {
    for (auto *paramDecl : *paramList)
      if (consumer.consume({paramDecl}))
        return true;
  }

  if (decl->getDeclContext()->isTypeContext()) {
    return consumer.consume({decl->getImplicitSelfDecl()});
  }

  // Consider \c var t: T { (did/will/)get/set { ... t }}
  // Lookup needs to find t, but if the var is inside of a type the baseDC needs
  // to be set. It all works fine, except: if the var is not inside of a type,
  // then t needs to be found as a local binding:
  if (auto *accessor = dyn_cast<AccessorDecl>(decl)) {
    if (auto *storage = accessor->getStorage())
      if (consumer.consume({storage}))
        return true;
  }

  return false;
}

bool SpecializeAttributeScope::lookupLocalsOrMembers(
    DeclConsumer consumer) const {
  if (auto *params = whatWasSpecialized->getGenericParams())
    for (auto *param : params->getParams())
      if (consumer.consume({param}))
        return true;
  return false;
}

bool DifferentiableAttributeScope::lookupLocalsOrMembers(
    DeclConsumer consumer) const {
  auto visitAbstractFunctionDecl = [&](AbstractFunctionDecl *afd) {
    if (auto *params = afd->getGenericParams())
      for (auto *param : params->getParams())
        if (consumer.consume({param}))
          return true;
    return false;
  };
  if (auto *afd = dyn_cast<AbstractFunctionDecl>(attributedDeclaration)) {
    return visitAbstractFunctionDecl(afd);
  } else if (auto *asd = dyn_cast<AbstractStorageDecl>(attributedDeclaration)) {
    if (auto *accessor = asd->getParsedAccessor(AccessorKind::Get))
      if (visitAbstractFunctionDecl(accessor))
        return true;
  }
  return false;
}

bool BraceStmtScope::lookupLocalsOrMembers(DeclConsumer consumer) const {
  if (consumer.consume(localFuncsAndTypes))
    return true;

  if (consumer.consumePossiblyNotInScope(localVars))
    return true;

  if (consumer.finishLookupInBraceStmt(stmt))
    return true;

  return false;
}

bool PatternEntryInitializerScope::lookupLocalsOrMembers(
    DeclConsumer consumer) const {
  // 'self' is available within the pattern initializer of a 'lazy' variable.
  auto *initContext = dyn_cast_or_null<PatternBindingInitializer>(
      decl->getInitContext(0));
  if (initContext) {
    if (auto *selfParam = initContext->getImplicitSelfDecl()) {
      return consumer.consume({selfParam});
    }
  }
  return false;
}

bool CaptureListScope::lookupLocalsOrMembers(DeclConsumer consumer) const {
  for (auto &e : expr->getCaptureList()) {
    if (consumer.consume({e.getVar()}))
      return true;
  }
  return false;
}

bool ClosureParametersScope::lookupLocalsOrMembers(
    DeclConsumer consumer) const {
  for (auto param : *closureExpr->getParameters())
    if (consumer.consume({param}))
      return true;
  return false;
}

bool ConditionalClausePatternUseScope::lookupLocalsOrMembers(
    DeclConsumer consumer) const {
  return lookupLocalBindingsInPattern(sec.getPattern(), consumer);
}

bool ASTScopeImpl::lookupLocalBindingsInPattern(const Pattern *p,
                                                DeclConsumer consumer) {
  if (!p)
    return false;
  bool isDone = false;
  p->forEachVariable([&](VarDecl *var) {
    if (!isDone)
      isDone = consumer.consume({var});
  });
  return isDone;
}

#pragma mark getLookupLimit

NullablePtr<const ASTScopeImpl> ASTScopeImpl::getLookupLimit() const {
  return nullptr;
}

NullablePtr<const ASTScopeImpl>
GenericTypeOrExtensionScope::getLookupLimit() const {
  return portion->getLookupLimitFor(this);
}

NullablePtr<const ASTScopeImpl>
Portion::getLookupLimitFor(const GenericTypeOrExtensionScope *) const {
  return nullptr;
}
NullablePtr<const ASTScopeImpl>
GenericTypeOrExtensionWholePortion::getLookupLimitFor(
    const GenericTypeOrExtensionScope *scope) const {
  return scope->getLookupLimitForDecl();
}

NullablePtr<const ASTScopeImpl>
GenericTypeOrExtensionScope::getLookupLimitForDecl() const {
  return nullptr;
}

NullablePtr<const ASTScopeImpl>
NominalTypeScope::getLookupLimitForDecl() const {
  if (isa<ProtocolDecl>(decl)) {
    // ProtocolDecl can only be legally nested in a SourceFile,
    // so any other kind of Decl is illegal
    return parentIfNotChildOfTopScope();
  }
  // AFAICT, a struct, decl, or enum can be nested inside anything
  // but a ProtocolDecl.
  return ancestorWithDeclSatisfying(
      [&](const Decl *const d) { return isa<ProtocolDecl>(d); });
}

NullablePtr<const ASTScopeImpl> ExtensionScope::getLookupLimitForDecl() const {
  // Extensions can only be legally nested in a SourceFile,
  // so any other kind of Decl is illegal
  return parentIfNotChildOfTopScope();
}

NullablePtr<const ASTScopeImpl> ASTScopeImpl::ancestorWithDeclSatisfying(
    function_ref<bool(const Decl *)> predicate) const {
  for (NullablePtr<const ASTScopeImpl> s = getParent(); s;
       s = s.get()->getParent()) {
    if (Decl *d = s.get()->getDeclIfAny().getPtrOrNull()) {
      if (predicate(d))
        return s;
    }
  }
  return nullptr;
}

#pragma mark isLabeledStmtLookupTerminator implementations
bool ASTScopeImpl::isLabeledStmtLookupTerminator() const {
  return true;
}

bool GuardStmtBodyScope::isLabeledStmtLookupTerminator() const {
  return false;
}

bool ConditionalClausePatternUseScope::isLabeledStmtLookupTerminator() const {
  return false;
}

bool AbstractStmtScope::isLabeledStmtLookupTerminator() const {
  return false;
}

bool ForEachPatternScope::isLabeledStmtLookupTerminator() const {
  return false;
}

bool CaseStmtBodyScope::isLabeledStmtLookupTerminator() const {
  return false;
}

bool PatternEntryDeclScope::isLabeledStmtLookupTerminator() const {
  return false;
}

bool PatternEntryInitializerScope::isLabeledStmtLookupTerminator() const {
  // This is needed for SingleValueStmtExprs, which may be used in bindings,
  // and have nested statements.
  return false;
}

llvm::SmallVector<LabeledStmt *, 4>
ASTScopeImpl::lookupLabeledStmts(SourceFile *sourceFile, SourceLoc loc) {
  // Find the innermost scope from which to start our search.
  auto *const fileScope = sourceFile->getScope().impl;
  const auto *innermost = fileScope->findInnermostEnclosingScope(loc, nullptr);
  ASTScopeAssert(innermost->getWasExpanded(),
                 "If looking in a scope, it must have been expanded.");

  llvm::SmallVector<LabeledStmt *, 4> labeledStmts;
  for (auto scope = innermost; scope && !scope->isLabeledStmtLookupTerminator();
       scope = scope->getParent().getPtrOrNull()) {
    // If we have a labeled statement, record it.
    auto stmt = scope->getStmtIfAny();
    if (!stmt) continue;

    auto labeledStmt = dyn_cast<LabeledStmt>(stmt.get());
    if (!labeledStmt) continue;

    // Skip guard statements; they aren't actually targets for break or
    // continue.
    if (isa<GuardStmt>(labeledStmt)) continue;

    labeledStmts.push_back(labeledStmt);
  }

  return labeledStmts;
}

std::pair<CaseStmt *, CaseStmt *> ASTScopeImpl::lookupFallthroughSourceAndDest(
    SourceFile *sourceFile, SourceLoc loc) {
  // Find the innermost scope from which to start our search.
  auto *const fileScope = sourceFile->getScope().impl;
  const auto *innermost = fileScope->findInnermostEnclosingScope(loc, nullptr);
  ASTScopeAssert(innermost->getWasExpanded(),
                 "If looking in a scope, it must have been expanded.");

  // Look for the enclosing case statement of a 'switch'.
  for (auto scope = innermost; scope && !scope->isLabeledStmtLookupTerminator();
       scope = scope->getParent().getPtrOrNull()) {
    // If we have a case statement, record it.
    auto stmt = scope->getStmtIfAny();
    if (!stmt) continue;

    // If we've found the first case statement of a switch, record it as the
    // fallthrough source. do-catch statements don't support fallthrough.
    if (auto caseStmt = dyn_cast<CaseStmt>(stmt.get())) {
      if (caseStmt->getParentKind() == CaseParentKind::Switch)
        return { caseStmt, caseStmt->findNextCaseStmt() };

      continue;
    }
  }

  return { nullptr, nullptr };
}

void ASTScopeImpl::lookupEnclosingMacroScope(
    SourceFile *sourceFile, SourceLoc loc,
    llvm::function_ref<bool(ASTScope::PotentialMacro)> consume) {
  if (!sourceFile || sourceFile->Kind == SourceFileKind::Interface)
    return;

  if (loc.isInvalid())
    return;

  auto *fileScope = sourceFile->getScope().impl;
  auto *scope = fileScope->findInnermostEnclosingScope(loc, nullptr);
  do {
    auto *freestanding = scope->getFreestandingMacro().getPtrOrNull();
    if (freestanding && consume(freestanding))
      return;

    auto *attr = scope->getDeclAttributeIfAny().getPtrOrNull();
    auto *potentialAttached = dyn_cast_or_null<CustomAttr>(attr);
    if (potentialAttached && consume(potentialAttached))
      return;

    // If we've reached a source file scope, we can't be inside of
    // a macro argument. Either this is a top-level source file, or
    // it's macro expansion buffer. We have to check for this because
    // macro expansion buffers for freestanding macros are children of
    // MacroExpansionDeclScope, and child scopes of freestanding macros
    // are otherwise inside the macro argument.
    if (scope->getClassName() == "ASTSourceFileScope")
      return;

  } while ((scope = scope->getParent().getPtrOrNull()));
}
