//===--- ASTScopeCreation.cpp - Swift Object-Oriented AST Scope -----------===//
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
/// This file implements the creation methods of the ASTScopeImpl ontology.
///
//===----------------------------------------------------------------------===//
#include "swift/AST/ASTScope.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Parse/Lexer.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/Support/Compiler.h"
#include <algorithm>
#include <unordered_set>

using namespace swift;
using namespace ast_scope;

namespace swift {
namespace ast_scope {

#pragma mark ScopeCreator

class ScopeCreator final : public ASTAllocated<ScopeCreator> {
  friend class ASTSourceFileScope;
  /// For allocating scopes.
  ASTContext &ctx;

public:
  ASTSourceFileScope *const sourceFileScope;
  ASTContext &getASTContext() const { return ctx; }

  ScopeCreator(SourceFile *SF)
      : ctx(SF->getASTContext()),
        sourceFileScope(new (ctx) ASTSourceFileScope(SF, this)) {}

  ScopeCreator(const ScopeCreator &) = delete;  // ensure no copies
  ScopeCreator(const ScopeCreator &&) = delete; // ensure no moves

public:
  /// For each of searching, call this unless the insertion point is needed
  void addToScopeTree(ASTNode n, ASTScopeImpl *parent) {
    (void)addToScopeTreeAndReturnInsertionPoint(n, parent, None);
  }
  /// Return new insertion point.
  /// For ease of searching, don't call unless insertion point is needed
  ///
  /// \param endLoc The end location for any "scopes until the end" that
  /// we introduce here, such as PatternEntryDeclScope and GuardStmtScope
  ASTScopeImpl *
  addToScopeTreeAndReturnInsertionPoint(ASTNode, ASTScopeImpl *parent,
                                        Optional<SourceLoc> endLoc);

  template <typename Scope, typename... Args>
  ASTScopeImpl *constructExpandAndInsert(ASTScopeImpl *parent, Args... args) {
    auto *child = new (ctx) Scope(args...);
    parent->addChild(child, ctx);

    if (auto *ip = child->insertionPointForDeferredExpansion().getPtrOrNull())
      return ip;

    ASTScopeImpl *insertionPoint = child->expandAndBeCurrent(*this);
    return insertionPoint;
  }

public:
  template <typename Scope, typename PortionClass, typename... Args>
  ASTScopeImpl *constructWithPortionExpandAndInsert(ASTScopeImpl *parent,
                                                    Args... args) {
    const Portion *portion = new (ctx) PortionClass();
    return constructExpandAndInsert<Scope>(parent, portion, args...);
  }

  void addExprToScopeTree(Expr *expr, ASTScopeImpl *parent) {
    // Use the ASTWalker to find buried captures and closures
    ASTScopeAssert(expr,
                 "If looking for closures, must have an expression to search.");

    /// AST walker that finds nested scopes in expressions. This handles both
    /// closures and if/switch expressions.
    class NestedExprScopeFinder : public ASTWalker {
      ScopeCreator &scopeCreator;
      ASTScopeImpl *parent;

    public:
      NestedExprScopeFinder(ScopeCreator &scopeCreator, ASTScopeImpl *parent)
          : scopeCreator(scopeCreator), parent(parent) {}

      PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
        if (auto *closure = dyn_cast<ClosureExpr>(E)) {
          scopeCreator
              .constructExpandAndInsert<ClosureParametersScope>(
                  parent, closure);
          return Action::SkipChildren(E);
        }
        if (auto *capture = dyn_cast<CaptureListExpr>(E)) {
          scopeCreator
              .constructExpandAndInsert<CaptureListScope>(
                  parent, capture);
          return Action::SkipChildren(E);
        }

        // If we have a single value statement expression, we need to add any
        // scopes in the underlying statement.
        if (auto *SVE = dyn_cast<SingleValueStmtExpr>(E)) {
          scopeCreator.addToScopeTree(SVE->getStmt(), parent);
          return Action::SkipChildren(E);
        }
        return Action::Continue(E);
      }
      PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
        if (isa<BraceStmt>(S)) { // closures hidden in here
          return Action::Continue(S);
        }
        return Action::SkipChildren(S);
      }
      PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override {
        return Action::SkipChildren(P);
      }
      PreWalkAction walkToDeclPre(Decl *D) override {
        return Action::SkipChildren();
      }
      PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
        return Action::SkipChildren();
      }
      PreWalkAction walkToParameterListPre(ParameterList *PL) override {
        return Action::SkipChildren();
      }

      MacroWalking getMacroWalkingBehavior() const override {
        return MacroWalking::ArgumentsAndExpansion;
      }
    };

    expr->walk(NestedExprScopeFinder(*this, parent));
  }

public:
  /// Create the matryoshka nested generic param scopes (if any)
  /// that are subscopes of the receiver. Return
  /// the furthest descendant.
  /// Last GenericParamsScope includes the where clause
  ASTScopeImpl *addNestedGenericParamScopesToTree(Decl *parameterizedDecl,
                                                  GenericParamList *generics,
                                                  ASTScopeImpl *parent) {
    if (!generics)
      return parent;
    auto *s = parent;
    for (unsigned i : indices(generics->getParams()))
      s = constructExpandAndInsert<GenericParamScope>(
              s, parameterizedDecl, generics, i);
    return s;
  }

  void
  addChildrenForParsedAccessors(AbstractStorageDecl *asd,
                                ASTScopeImpl *parent);

  void addChildrenForKnownAttributes(ValueDecl *decl,
                                     ASTScopeImpl *parent);

  /// Add PatternEntryDeclScopes for each pattern binding entry.
  ///
  /// Returns the new insertion point.
  ///
  /// \param endLoc Must be valid iff the pattern binding is in a local
  /// scope, in which case this is the last source location where the
  /// pattern bindings are going to be visible.
  ASTScopeImpl *
  addPatternBindingToScopeTree(PatternBindingDecl *patternBinding,
                               ASTScopeImpl *parent,
                               Optional<SourceLoc> endLoc);

  SWIFT_DEBUG_DUMP { print(llvm::errs()); }

  void print(raw_ostream &out) const {
    out << "(swift::ASTSourceFileScope*) " << sourceFileScope << "\n";
  }
};
} // ast_scope
} // namespace swift

#pragma mark Scope tree creation and extension

ASTScope::ASTScope(SourceFile *SF) : impl(createScopeTree(SF)) {}

void ASTScope::buildFullyExpandedTree() { impl->buildFullyExpandedTree(); }

void ASTScope::
    buildEnoughOfTreeForTopLevelExpressionsButDontRequestGenericsOrExtendedNominals() {
  impl->buildEnoughOfTreeForTopLevelExpressionsButDontRequestGenericsOrExtendedNominals();
}

void ASTScope::expandFunctionBody(AbstractFunctionDecl *AFD) {
  // There is no source file associated with C++ decl contexts, so there will
  // be no parent source file if AFD is a C++ function.
  if (auto *const SF = AFD->getParentSourceFile())
    SF->getScope().expandFunctionBodyImpl(AFD);
}

void ASTScope::expandFunctionBodyImpl(AbstractFunctionDecl *AFD) {
  impl->expandFunctionBody(AFD);
}

ASTSourceFileScope *ASTScope::createScopeTree(SourceFile *SF) {
  ScopeCreator *scopeCreator = new (SF->getASTContext()) ScopeCreator(SF);
  return scopeCreator->sourceFileScope;
}

void ASTSourceFileScope::buildFullyExpandedTree() {
  if (!getWasExpanded())
    expandAndBeCurrent(*scopeCreator);
  preOrderChildrenDo([&](ASTScopeImpl *s) {
    if (!s->getWasExpanded())
      s->expandAndBeCurrent(*scopeCreator);
  });
}

void ASTSourceFileScope::
    buildEnoughOfTreeForTopLevelExpressionsButDontRequestGenericsOrExtendedNominals() {
  if (!getWasExpanded())
    expandAndBeCurrent(*scopeCreator);
}

void ASTSourceFileScope::expandFunctionBody(AbstractFunctionDecl *AFD) {
  if (!AFD)
    return;
  auto sr = AFD->getOriginalBodySourceRange();
  if (sr.isInvalid())
    return;
  ASTScopeImpl *bodyScope = findInnermostEnclosingScope(sr.Start, nullptr);
  if (!bodyScope->getWasExpanded())
    bodyScope->expandAndBeCurrent(*scopeCreator);
}

ASTSourceFileScope::ASTSourceFileScope(SourceFile *SF,
                                       ScopeCreator *scopeCreator)
    : SF(SF), scopeCreator(scopeCreator) {
  if (auto enclosingSF = SF->getEnclosingSourceFile()) {
    SourceLoc parentLoc;
    auto macroRole = SF->getFulfilledMacroRole();
    auto expansion = SF->getMacroExpansion();

    // Determine the parent source location based on the macro role.
    switch (*macroRole) {
    case MacroRole::Expression:
    case MacroRole::Declaration:
    case MacroRole::CodeItem:
    case MacroRole::Accessor:
    case MacroRole::MemberAttribute:
    case MacroRole::Conformance:
      parentLoc = expansion.getStartLoc();
      break;
    case MacroRole::Peer: {
      ASTContext &ctx = SF->getASTContext();
      SourceManager &sourceMgr = ctx.SourceMgr;
      auto generatedSourceInfo =
          *sourceMgr.getGeneratedSourceInfo(*SF->getBufferID());

      ASTNode node = ASTNode::getFromOpaqueValue(generatedSourceInfo.astNode);
      parentLoc = Lexer::getLocForEndOfToken(sourceMgr, node.getEndLoc());
      break;
    }
    case MacroRole::Member: {
      // For synthesized member macros, take the end loc of the
      // enclosing declaration (before the closing brace), because
      // the macro expansion is inside this scope.
      auto *decl = expansion.getAsDeclContext()->getAsDecl();
      parentLoc = decl->getEndLoc();
      break;
    }
    }

    if (auto parentScope = findStartingScopeForLookup(enclosingSF, parentLoc)) {
      parentAndWasExpanded.setPointer(const_cast<ASTScopeImpl *>(parentScope));
    }
  }
}

#pragma mark NodeAdder

namespace swift {
namespace ast_scope {

class NodeAdder
    : public ASTVisitor<NodeAdder, ASTScopeImpl *,
                        ASTScopeImpl *, ASTScopeImpl *,
                        void, void, void, ASTScopeImpl *, ScopeCreator &> {
  Optional<SourceLoc> endLoc;

public:
  explicit NodeAdder(Optional<SourceLoc> endLoc) : endLoc(endLoc) {}

#pragma mark ASTNodes that do not create scopes

#define VISIT_AND_IGNORE(What)                                                 \
  ASTScopeImpl *visit##What(What *w, ASTScopeImpl *p,                          \
                            ScopeCreator &) {                                  \
    return p;                                                                  \
  }

  VISIT_AND_IGNORE(ImportDecl)
  VISIT_AND_IGNORE(EnumCaseDecl)
  VISIT_AND_IGNORE(PrecedenceGroupDecl)
  VISIT_AND_IGNORE(InfixOperatorDecl)
  VISIT_AND_IGNORE(PrefixOperatorDecl)
  VISIT_AND_IGNORE(PostfixOperatorDecl)
  VISIT_AND_IGNORE(GenericTypeParamDecl)
  VISIT_AND_IGNORE(AssociatedTypeDecl)
  VISIT_AND_IGNORE(ModuleDecl)
  VISIT_AND_IGNORE(ParamDecl)
  VISIT_AND_IGNORE(PoundDiagnosticDecl)
  VISIT_AND_IGNORE(MissingDecl)
  VISIT_AND_IGNORE(MissingMemberDecl)

  // Only members of the active clause are in scope, and those
  // are visited separately.
  VISIT_AND_IGNORE(IfConfigDecl)

  // This declaration is handled from the PatternBindingDecl
  VISIT_AND_IGNORE(VarDecl)

  // These contain nothing to scope.
  VISIT_AND_IGNORE(BreakStmt)
  VISIT_AND_IGNORE(ContinueStmt)
  VISIT_AND_IGNORE(FallthroughStmt)
  VISIT_AND_IGNORE(FailStmt)

#undef VISIT_AND_IGNORE

#pragma mark simple creation ignoring deferred nodes

#define VISIT_AND_CREATE(What, ScopeClass)                                     \
  ASTScopeImpl *visit##What(What *w, ASTScopeImpl *p,                          \
                            ScopeCreator &scopeCreator) {                      \
    return scopeCreator.constructExpandAndInsert<ScopeClass>(p, w);            \
  }

  VISIT_AND_CREATE(SubscriptDecl, SubscriptDeclScope)
  VISIT_AND_CREATE(IfStmt, IfStmtScope)
  VISIT_AND_CREATE(WhileStmt, WhileStmtScope)
  VISIT_AND_CREATE(RepeatWhileStmt, RepeatWhileScope)
  VISIT_AND_CREATE(DoStmt, DoStmtScope)
  VISIT_AND_CREATE(DoCatchStmt, DoCatchStmtScope)
  VISIT_AND_CREATE(SwitchStmt, SwitchStmtScope)
  VISIT_AND_CREATE(ForEachStmt, ForEachStmtScope)
  VISIT_AND_CREATE(CaseStmt, CaseStmtScope)
  VISIT_AND_CREATE(AbstractFunctionDecl, AbstractFunctionDeclScope)
  VISIT_AND_CREATE(MacroDecl, MacroDeclScope)
  VISIT_AND_CREATE(MacroExpansionDecl, MacroExpansionDeclScope)

#undef VISIT_AND_CREATE

#pragma mark 2D simple creation (ignoring deferred nodes)

#define VISIT_AND_CREATE_WHOLE_PORTION(What, WhatScope)                        \
  ASTScopeImpl *visit##What(What *w, ASTScopeImpl *p,                          \
                            ScopeCreator &scopeCreator) {                      \
    return scopeCreator.constructWithPortionExpandAndInsert<                   \
        WhatScope, GenericTypeOrExtensionWholePortion>(p, w);                  \
  }

  VISIT_AND_CREATE_WHOLE_PORTION(ExtensionDecl, ExtensionScope)
  VISIT_AND_CREATE_WHOLE_PORTION(StructDecl, NominalTypeScope)
  VISIT_AND_CREATE_WHOLE_PORTION(ClassDecl, NominalTypeScope)
  VISIT_AND_CREATE_WHOLE_PORTION(ProtocolDecl, NominalTypeScope)
  VISIT_AND_CREATE_WHOLE_PORTION(EnumDecl, NominalTypeScope)
  VISIT_AND_CREATE_WHOLE_PORTION(TypeAliasDecl, TypeAliasScope)
  VISIT_AND_CREATE_WHOLE_PORTION(OpaqueTypeDecl, OpaqueTypeScope)
#undef VISIT_AND_CREATE_WHOLE_PORTION

  ASTScopeImpl *visitBuiltinTupleDecl(BuiltinTupleDecl *btd, ASTScopeImpl *p,
                                      ScopeCreator &scopeCreator) {
    llvm_unreachable("BuiltinTupleDecl should never appear in a source file");
  }

  // This declaration is handled from
  // addChildrenForParsedAccessors
  ASTScopeImpl *visitAccessorDecl(AccessorDecl *ad, ASTScopeImpl *p,
                                  ScopeCreator &scopeCreator) {
    return visitAbstractFunctionDecl(ad, p, scopeCreator);
  }

#pragma mark simple creation with deferred nodes

  // Each of the following creates a new scope, so that nodes which were parsed
  // after them need to be placed in scopes BELOW them in the tree. So pass down
  // the deferred nodes.
  ASTScopeImpl *visitGuardStmt(GuardStmt *e, ASTScopeImpl *p,
                               ScopeCreator &scopeCreator) {
    ASTScopeAssert(endLoc.has_value(), "GuardStmt outside of a BraceStmt?");
    return scopeCreator.constructExpandAndInsert<GuardStmtScope>(
      p, e, *endLoc);
  }
  ASTScopeImpl *visitTopLevelCodeDecl(TopLevelCodeDecl *d,
                                      ASTScopeImpl *p,
                                      ScopeCreator &scopeCreator) {
    ASTScopeAssert(endLoc.has_value(), "TopLevelCodeDecl in wrong place?");
    return scopeCreator.constructExpandAndInsert<TopLevelCodeScope>(
        p, d, *endLoc);
  }

#pragma mark special-case creation

  ASTScopeImpl *visitSourceFile(SourceFile *, ASTScopeImpl *, ScopeCreator &) {
    ASTScope_unreachable("SourceFiles are orphans.");
  }

  ASTScopeImpl *visitYieldStmt(YieldStmt *ys, ASTScopeImpl *p,
                               ScopeCreator &scopeCreator) {
    for (Expr *e : ys->getYields())
      visitExpr(e, p, scopeCreator);
    return p;
  }

  ASTScopeImpl *visitDeferStmt(DeferStmt *ds, ASTScopeImpl *p,
                               ScopeCreator &scopeCreator) {
    visitFuncDecl(ds->getTempDecl(), p, scopeCreator);
    return p;
  }

  ASTScopeImpl *visitBraceStmt(BraceStmt *bs, ASTScopeImpl *p,
                               ScopeCreator &scopeCreator) {
    if (bs->empty())
      return p;

    SmallVector<ValueDecl *, 2> localFuncsAndTypes;
    SmallVector<VarDecl *, 2> localVars;

    // All types and functions are visible anywhere within a brace statement
    // scope. When ordering matters (i.e. var decl) we will have split the brace
    // statement into nested scopes.
    for (auto braceElement : bs->getElements()) {
      if (auto localBinding = braceElement.dyn_cast<Decl *>()) {
        if (auto *vd = dyn_cast<ValueDecl>(localBinding)) {
          if (isa<FuncDecl>(vd)  || isa<TypeDecl>(vd)) {
            localFuncsAndTypes.push_back(vd);
          } else if (auto *var = dyn_cast<VarDecl>(localBinding)) {
            localVars.push_back(var);
          }
        }
      }
    }

    SourceLoc endLocForBraceStmt = bs->getEndLoc();
    if (endLoc.has_value())
      endLocForBraceStmt = *endLoc;

    ASTContext &ctx = scopeCreator.getASTContext();

    return
        scopeCreator.constructExpandAndInsert<BraceStmtScope>(
            p, bs,
            ctx.AllocateCopy(localFuncsAndTypes),
            ctx.AllocateCopy(localVars),
            endLocForBraceStmt);
  }

  ASTScopeImpl *
  visitPatternBindingDecl(PatternBindingDecl *patternBinding,
                          ASTScopeImpl *parentScope,
                          ScopeCreator &scopeCreator) {
    return scopeCreator.addPatternBindingToScopeTree(
        patternBinding, parentScope, endLoc);
  }

  ASTScopeImpl *visitEnumElementDecl(EnumElementDecl *eed,
                                     ASTScopeImpl *p,
                                     ScopeCreator &scopeCreator) {
    scopeCreator.constructExpandAndInsert<EnumElementScope>(p, eed);
    return p;
  }

  ASTScopeImpl *visitReturnStmt(ReturnStmt *rs, ASTScopeImpl *p,
                                ScopeCreator &scopeCreator) {
    if (rs->hasResult())
      visitExpr(rs->getResult(), p, scopeCreator);
    return p;
  }

  ASTScopeImpl *visitThrowStmt(ThrowStmt *ts, ASTScopeImpl *p,
                               ScopeCreator &scopeCreator) {
    visitExpr(ts->getSubExpr(), p, scopeCreator);
    return p;
  }

  ASTScopeImpl *visitDiscardStmt(DiscardStmt *ds, ASTScopeImpl *p,
                                 ScopeCreator &scopeCreator) {
    visitExpr(ds->getSubExpr(), p, scopeCreator);
    return p;
  }

  ASTScopeImpl *visitPoundAssertStmt(PoundAssertStmt *pas,
                                     ASTScopeImpl *p,
                                     ScopeCreator &scopeCreator) {
    visitExpr(pas->getCondition(), p, scopeCreator);
    return p;
  }

  ASTScopeImpl *visitExpr(Expr *expr, ASTScopeImpl *p,
                          ScopeCreator &scopeCreator) {
    if (!expr)
      return p;

    scopeCreator.addExprToScopeTree(expr, p);
    return p;
  }
};
} // namespace ast_scope
} // namespace swift

// These definitions are way down here so it can call into
// NodeAdder
ASTScopeImpl *
ScopeCreator::addToScopeTreeAndReturnInsertionPoint(ASTNode n,
                                                    ASTScopeImpl *parent,
                                                    Optional<SourceLoc> endLoc) {
  if (!n)
    return parent;

  // HACK: LLDB creates implicit pattern bindings that... contain user
  // expressions. We need to actually honor lookups through those bindings
  // in case they contain closures that bind additional variables in further
  // scopes.
  if (auto *d = n.dyn_cast<Decl *>())
    if (d->isImplicit())
      if (!isa<PatternBindingDecl>(d)
          || !cast<PatternBindingDecl>(d)->isDebuggerBinding())
        return parent;

  NodeAdder adder(endLoc);
  if (auto *p = n.dyn_cast<Decl *>())
    return adder.visit(p, parent, *this);
  if (auto *p = n.dyn_cast<Expr *>())
    return adder.visit(p, parent, *this);
  auto *p = n.get<Stmt *>();
  return adder.visit(p, parent, *this);
}

void ScopeCreator::addChildrenForParsedAccessors(
    AbstractStorageDecl *asd, ASTScopeImpl *parent) {
  asd->visitParsedAccessors([&](AccessorDecl *ad) {
    assert(asd == ad->getStorage());
    this->addToScopeTree(ad, parent);
  });
}

void ScopeCreator::addChildrenForKnownAttributes(ValueDecl *decl,
                                                 ASTScopeImpl *parent) {
  SmallVector<DeclAttribute *, 2> relevantAttrs;

  for (auto *attr : decl->getAttrs()) {
    if (attr->isImplicit())
      continue;

    if (isa<DifferentiableAttr>(attr))
      relevantAttrs.push_back(attr);

    if (isa<SpecializeAttr>(attr))
      relevantAttrs.push_back(attr);

    if (isa<CustomAttr>(attr))
      relevantAttrs.push_back(attr);
  }

  // Decl::getAttrs() is a linked list with head insertion, so the
  // attributes are in reverse source order.
  std::reverse(relevantAttrs.begin(), relevantAttrs.end());

  for (auto *attr : relevantAttrs) {
    if (auto *diffAttr = dyn_cast<DifferentiableAttr>(attr)) {
      constructExpandAndInsert<DifferentiableAttributeScope>(
          parent, diffAttr, decl);
    } else if (auto *specAttr = dyn_cast<SpecializeAttr>(attr)) {
      if (auto *afd = dyn_cast<AbstractFunctionDecl>(decl)) {
        constructExpandAndInsert<SpecializeAttributeScope>(
            parent, specAttr, afd);
      }
    } else if (auto *customAttr = dyn_cast<CustomAttr>(attr)) {
      if (auto *vd = dyn_cast<VarDecl>(decl)) {
        constructExpandAndInsert<AttachedPropertyWrapperScope>(
            parent, customAttr, vd);
      }
    }
  }
}

ASTScopeImpl *
ScopeCreator::addPatternBindingToScopeTree(PatternBindingDecl *patternBinding,
                                           ASTScopeImpl *parentScope,
                                           Optional<SourceLoc> endLoc) {
  if (auto *var = patternBinding->getSingleVar())
    addChildrenForKnownAttributes(var, parentScope);

  bool isLocalBinding = false;
  for (auto i : range(patternBinding->getNumPatternEntries())) {
    if (auto *varDecl = patternBinding->getAnchoringVarDecl(i)) {
      isLocalBinding = varDecl->getDeclContext()->isLocalContext();
      break;
    }
  }

  auto *insertionPoint = parentScope;
  for (auto i : range(patternBinding->getNumPatternEntries())) {
    Optional<SourceLoc> endLocForBinding = None;
    if (isLocalBinding) {
      endLocForBinding = endLoc;
      ASTScopeAssert(endLoc.has_value() && endLoc->isValid(),
                     "PatternBindingDecl in local context outside of BraceStmt?");
    }

    insertionPoint =
        constructExpandAndInsert<PatternEntryDeclScope>(
            insertionPoint, patternBinding, i,
            isLocalBinding, endLocForBinding);

    ASTScopeAssert(isLocalBinding || insertionPoint == parentScope,
                   "Bindings at the top-level or members of types should "
                   "not change the insertion point");
  }

  return insertionPoint;
}

#pragma mark creation helpers

void ASTScopeImpl::addChild(ASTScopeImpl *child, ASTContext &ctx) {
  ASTScopeAssert(!child->getParent(), "child should not already have parent");
  child->parentAndWasExpanded.setPointer(this);

#ifndef NDEBUG
  // checkSourceRangeBeforeAddingChild(child, ctx);
#endif

  // If this is the first time we've added children, notify the ASTContext
  // that there's a SmallVector that needs to be cleaned up.
  if (storedChildren.empty())
    ctx.addDestructorCleanup(storedChildren);

  storedChildren.push_back(child);
}

#pragma mark implementations of expansion

ASTScopeImpl *ASTScopeImpl::expandAndBeCurrent(ScopeCreator &scopeCreator) {
  ASTScopeAssert(!getWasExpanded(),
                 "Cannot expand the same scope twice");

  // Set the flag before we actually expand, to detect re-entrant calls
  // via the above assertion.
  setWasExpanded();

  if (auto *s = scopeCreator.getASTContext().Stats)
    ++s->getFrontendCounters().NumASTScopeExpansions;

  auto *insertionPoint = expandSpecifically(scopeCreator);
  ASTScopeAssert(!insertionPointForDeferredExpansion() ||
                     insertionPointForDeferredExpansion().get() ==
                         insertionPoint,
                 "In order for lookups into lazily-expanded scopes to be "
                 "accurate before expansion, the insertion point before "
                 "expansion must be the same as after expansion.");

  return insertionPoint;
}

  // Do this whole bit so it's easy to see which type of scope is which

#define CREATES_NEW_INSERTION_POINT(Scope)                                     \
  ASTScopeImpl *Scope::expandSpecifically(ScopeCreator &scopeCreator) {        \
    return expandAScopeThatCreatesANewInsertionPoint(scopeCreator)             \
        .insertionPoint;                                                       \
  }

#define NO_NEW_INSERTION_POINT(Scope)                                          \
  ASTScopeImpl *Scope::expandSpecifically(ScopeCreator &scopeCreator) {        \
    expandAScopeThatDoesNotCreateANewInsertionPoint(scopeCreator);             \
    return getParent().get();                                                  \
  }

// Return this in particular for GenericParamScope so body is scoped under it
#define NO_EXPANSION(Scope)                                                    \
  ASTScopeImpl *Scope::expandSpecifically(ScopeCreator &) { return this; }

CREATES_NEW_INSERTION_POINT(ASTSourceFileScope)
CREATES_NEW_INSERTION_POINT(GuardStmtScope)
CREATES_NEW_INSERTION_POINT(PatternEntryDeclScope)
CREATES_NEW_INSERTION_POINT(GenericTypeOrExtensionScope)
CREATES_NEW_INSERTION_POINT(BraceStmtScope)
CREATES_NEW_INSERTION_POINT(TopLevelCodeScope)
CREATES_NEW_INSERTION_POINT(ConditionalClausePatternUseScope)

NO_NEW_INSERTION_POINT(FunctionBodyScope)
NO_NEW_INSERTION_POINT(AbstractFunctionDeclScope)
NO_NEW_INSERTION_POINT(AttachedPropertyWrapperScope)
NO_NEW_INSERTION_POINT(EnumElementScope)
NO_NEW_INSERTION_POINT(GuardStmtBodyScope)
NO_NEW_INSERTION_POINT(ParameterListScope)
NO_NEW_INSERTION_POINT(PatternEntryInitializerScope)

NO_NEW_INSERTION_POINT(CaptureListScope)
NO_NEW_INSERTION_POINT(CaseStmtScope)
NO_NEW_INSERTION_POINT(CaseLabelItemScope)
NO_NEW_INSERTION_POINT(CaseStmtBodyScope)
NO_NEW_INSERTION_POINT(ConditionalClauseInitializerScope)
NO_NEW_INSERTION_POINT(ClosureParametersScope)
NO_NEW_INSERTION_POINT(DefaultArgumentInitializerScope)
NO_NEW_INSERTION_POINT(DoStmtScope)
NO_NEW_INSERTION_POINT(DoCatchStmtScope)
NO_NEW_INSERTION_POINT(ForEachPatternScope)
NO_NEW_INSERTION_POINT(ForEachStmtScope)
NO_NEW_INSERTION_POINT(IfStmtScope)
NO_NEW_INSERTION_POINT(RepeatWhileScope)
NO_NEW_INSERTION_POINT(SubscriptDeclScope)
NO_NEW_INSERTION_POINT(MacroDeclScope)
NO_NEW_INSERTION_POINT(MacroDefinitionScope)
NO_NEW_INSERTION_POINT(MacroExpansionDeclScope)
NO_NEW_INSERTION_POINT(SwitchStmtScope)
NO_NEW_INSERTION_POINT(WhileStmtScope)

NO_EXPANSION(GenericParamScope)
NO_EXPANSION(SpecializeAttributeScope)
NO_EXPANSION(DifferentiableAttributeScope)

#undef CREATES_NEW_INSERTION_POINT
#undef NO_NEW_INSERTION_POINT

AnnotatedInsertionPoint
ASTSourceFileScope::expandAScopeThatCreatesANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  ASTScopeAssert(SF, "Must already have a SourceFile.");

  SourceLoc endLoc = getSourceRangeOfThisASTNode().End;

  ASTScopeImpl *insertionPoint = this;
  for (auto node : SF->getTopLevelItems()) {
    insertionPoint = scopeCreator.addToScopeTreeAndReturnInsertionPoint(
      node, insertionPoint, endLoc);
  }

  return {insertionPoint, "Next time decls are added they go here."};
}

void
ParameterListScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  // Each initializer for a function parameter is its own, sibling, scope.
  // Unlike generic parameters or pattern initializers, it cannot refer to a
  // previous parameter.
  for (ParamDecl *pd : params->getArray()) {
    if (pd->hasDefaultExpr())
      scopeCreator
          .constructExpandAndInsert<DefaultArgumentInitializerScope>(
              this, pd);
  }
}

AnnotatedInsertionPoint
PatternEntryDeclScope::expandAScopeThatCreatesANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  // Initializers come before VarDecls, e.g. PCMacro/didSet.swift 19
  auto patternEntry = getPatternEntry();

  // If the pattern type is for a named opaque result type, introduce the
  // generic type parameters based on the first variable we find.
  ASTScopeImpl *leaf = this;
  auto pattern = patternEntry.getPattern();
  if (auto typedPattern = dyn_cast<TypedPattern>(pattern)) {
    if (auto namedOpaque =
            dyn_cast_or_null<NamedOpaqueReturnTypeRepr>(
              typedPattern->getTypeRepr())) {
      bool addedOpaqueResultTypeScope = false;
      pattern->forEachVariable([&](VarDecl *var) {
        if (addedOpaqueResultTypeScope)
          return;

        leaf = scopeCreator.addNestedGenericParamScopesToTree(
            var, namedOpaque->getGenericParams(), leaf);
        addedOpaqueResultTypeScope = true;
      });
    }
  }

  // Create a child for the initializer, if present.
  // Cannot trust the source range given in the ASTScopeImpl for the end of the
  // initializer (because of InterpolatedLiteralStrings and EditorPlaceHolders),
  // so compute it ourselves.
  // Even if this predicate fails, there may be an initContext but
  // we cannot make a scope for it, since no source range.
  if (patternEntry.getOriginalInit()) {
    ASTScopeAssert(
        patternEntry.getOriginalInit()->getSourceRange().isValid(),
        "pattern initializer has invalid source range");
    ASTScopeAssert(
        !getSourceManager().isBeforeInBuffer(
            patternEntry.getOriginalInit()->getStartLoc(), decl->getStartLoc()),
        "Original inits are always after the '='");
    scopeCreator
        .constructExpandAndInsert<PatternEntryInitializerScope>(
            leaf, decl, patternEntryIndex);
  }

  // If this pattern binding entry was created by the debugger, it will always
  // have a synthesized init that is created from user code. We special-case
  // lookups into these scopes to look through the debugger's chicanery to the
  // underlying user-defined scopes, if any.
  if (patternEntry.isFromDebugger() && patternEntry.getInit()) {
    ASTScopeAssert(
        patternEntry.getInit()->getSourceRange().isValid(),
        "pattern initializer has invalid source range");
    ASTScopeAssert(
        !getSourceManager().isBeforeInBuffer(
            patternEntry.getInit()->getStartLoc(), decl->getStartLoc()),
        "inits are always after the '='");
    scopeCreator
        .constructExpandAndInsert<PatternEntryInitializerScope>(
            leaf, decl, patternEntryIndex);
  }

  // Add accessors for the variables in this pattern.
  pattern->forEachVariable([&](VarDecl *var) {
    scopeCreator.addChildrenForParsedAccessors(var, leaf);
  });

  // In local context, the PatternEntryDeclScope becomes the insertion point, so
  // that all any bindings introduced by the pattern are in scope for subsequent
  // lookups.
  if (isLocalBinding)
    return {this, "All code that follows is inside this scope"};

  return {getParent().get(), "Global and type members do not introduce scopes"};
}

void
PatternEntryInitializerScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  // Create a child for the initializer expression.
  scopeCreator.addToScopeTree(ASTNode(initAsWrittenWhenCreated), this);
}


AnnotatedInsertionPoint
ConditionalClausePatternUseScope::expandAScopeThatCreatesANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  auto *initializer = sec.getInitializer();
  if (!isa<ErrorExpr>(initializer)) {
    scopeCreator
      .constructExpandAndInsert<ConditionalClauseInitializerScope>(
        this, initializer);
    }

  return {this,
          "Succeeding code must be in scope of conditional clause pattern bindings"};
}

void
ConditionalClauseInitializerScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  scopeCreator.addToScopeTree(ASTNode(initializer), this);
}

void
GuardStmtBodyScope::expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &
                                                                    scopeCreator) {
  scopeCreator.addToScopeTree(ASTNode(body), this);
}

AnnotatedInsertionPoint
GuardStmtScope::expandAScopeThatCreatesANewInsertionPoint(ScopeCreator &
                                                          scopeCreator) {
  ASTScopeImpl *conditionLookupParent =
      createNestedConditionalClauseScopes(scopeCreator, endLoc);

  // Add a child for the 'guard' body, which always exits.
  // The lookup parent is whole guard stmt scope, NOT the cond scopes
  auto *body = stmt->getBody();
  if (!body->empty()) {
    scopeCreator
        .constructExpandAndInsert<GuardStmtBodyScope>(
            conditionLookupParent, this, stmt->getBody());
  }

  return {conditionLookupParent,
          "Succeeding code must be in scope of guard variables"};
}

AnnotatedInsertionPoint
GenericTypeOrExtensionScope::expandAScopeThatCreatesANewInsertionPoint(
    ScopeCreator & scopeCreator) {
  return {portion->expandScope(this, scopeCreator),
          "<X: Foo, Y: X> is legal, so nest these"};
}

AnnotatedInsertionPoint
BraceStmtScope::expandAScopeThatCreatesANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  ASTScopeImpl *insertionPoint = this;
  for (auto nd : stmt->getElements()) {
    insertionPoint = scopeCreator.addToScopeTreeAndReturnInsertionPoint(
        nd, insertionPoint, endLoc);
  }

  return {
      insertionPoint,
      "For top-level code decls, need the scope under, say a guard statement."};
}

AnnotatedInsertionPoint
TopLevelCodeScope::expandAScopeThatCreatesANewInsertionPoint(ScopeCreator &
                                                             scopeCreator) {

  auto *body =
      scopeCreator
          .addToScopeTreeAndReturnInsertionPoint(decl->getBody(), this, endLoc);

  return {body, "So next top level code scope and put its decls in its body "
                "under a guard statement scope (etc) from the last top level "
                "code scope"};
}

#pragma mark expandAScopeThatDoesNotCreateANewInsertionPoint

// Create child scopes for every declaration in a body.

namespace {
  /// Retrieve the opaque generic parameter list if present, otherwise the normal generic parameter list.
  template<typename T>
  GenericParamList *getPotentiallyOpaqueGenericParams(T *decl) {
    if (auto opaqueRepr = decl->getResultTypeRepr()) {
      if (auto namedOpaque = dyn_cast<NamedOpaqueReturnTypeRepr>(opaqueRepr)) {
        return namedOpaque->getGenericParams();
      }
    }

    return decl->getParsedGenericParams();
  }
}

void AbstractFunctionDeclScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  scopeCreator.addChildrenForKnownAttributes(decl, this);

  // Create scopes for generic and ordinary parameters.
  // For a subscript declaration, the generic and ordinary parameters are in an
  // ancestor scope, so don't make them here.
  ASTScopeImpl *leaf = this;

  if (!isa<AccessorDecl>(decl)) {
    leaf = scopeCreator.addNestedGenericParamScopesToTree(
        decl, getPotentiallyOpaqueGenericParams(decl), leaf);

    auto *params = decl->getParameters();
    if (params->size() > 0) {
      scopeCreator.constructExpandAndInsert<ParameterListScope>(
          leaf, params, nullptr);
    }
  }

  // Create scope for the body.
  // We create body scopes when there is no body for source kit to complete
  // erroneous code in bodies.
  if (decl->getBodySourceRange().isValid()) {
    scopeCreator.constructExpandAndInsert<FunctionBodyScope>(leaf, decl);
  }
}

void EnumElementScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  if (auto *pl = decl->getParameterList())
    scopeCreator.constructExpandAndInsert<ParameterListScope>(this, pl, nullptr);
  // The invariant that the raw value expression can never introduce a new scope
  // is checked in Parse.  However, this guarantee is not future-proof.  Compute
  // and add the raw value expression anyways just to be defensive.
  //
  // FIXME: Re-enable this.  It currently crashes for malformed enum cases.
  // scopeCreator.addToScopeTree(decl->getStructuralRawValueExpr(), this);
}

void FunctionBodyScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  expandBody(scopeCreator);
}

void IfStmtScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  auto *thenStmt = stmt->getThenStmt();
  auto *elseStmt = stmt->getElseStmt();

  SourceLoc endLoc = thenStmt->getEndLoc();
  ASTScopeImpl *insertionPoint =
      createNestedConditionalClauseScopes(scopeCreator, endLoc);

  // The 'then' branch
  scopeCreator.addToScopeTree(thenStmt, insertionPoint);

  // Result builders can add an 'else' block consisting entirely of
  // implicit expressions. In this case, the end location of the
  // 'then' block is equal to the start location of the 'else'
  // block, and the 'else' block source range is empty.
  if (elseStmt &&
      thenStmt->getEndLoc() == elseStmt->getStartLoc() &&
      elseStmt->getStartLoc() == elseStmt->getEndLoc())
    return;

  // Add the 'else' branch, if needed.
  scopeCreator.addToScopeTree(elseStmt, this);
}

void WhileStmtScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  SourceLoc endLoc = stmt->getBody()->getEndLoc();
  ASTScopeImpl *insertionPoint =
      createNestedConditionalClauseScopes(scopeCreator, endLoc);
  scopeCreator.addToScopeTree(stmt->getBody(), insertionPoint);
}

void RepeatWhileScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  scopeCreator.addToScopeTree(stmt->getBody(), this);
  scopeCreator.addToScopeTree(stmt->getCond(), this);
}

void DoStmtScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  scopeCreator.addToScopeTree(stmt->getBody(), this);
}

void DoCatchStmtScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  scopeCreator.addToScopeTree(stmt->getBody(), this);

  for (auto catchClause : stmt->getCatches())
    scopeCreator.addToScopeTree(catchClause, this);
}

void SwitchStmtScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  scopeCreator.addToScopeTree(stmt->getSubjectExpr(), this);

  for (auto caseStmt : stmt->getCases()) {
    ASTScopeAssert(
        caseStmt->getSourceRange().isValid(),
        "pattern initializer has invalid source range");
    scopeCreator.constructExpandAndInsert<CaseStmtScope>(this, caseStmt);
  }
}

void ForEachStmtScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  scopeCreator.addToScopeTree(stmt->getParsedSequence(), this);

  // Add a child describing the scope of the pattern.
  // In error cases such as:
  //    let v: C { for b : Int -> S((array: P { }
  // the body is implicit and it would overlap the source range of the expr
  // above.
  if (!stmt->getBody()->isImplicit()) {
    ASTScopeAssert(
        stmt->getBody()->getSourceRange().isValid(),
        "pattern initializer has invalid source range");
    scopeCreator.constructExpandAndInsert<ForEachPatternScope>(this, stmt);
  }
}

void ForEachPatternScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  scopeCreator.addToScopeTree(stmt->getWhere(), this);
  scopeCreator.addToScopeTree(stmt->getBody(), this);
}

void CaseStmtScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  for (auto &item : stmt->getCaseLabelItems()) {
    if (item.getGuardExpr()) {
      scopeCreator.constructExpandAndInsert<CaseLabelItemScope>(this, item);
    }
  }

  if (!stmt->getBody()->empty()) {
    scopeCreator.constructExpandAndInsert<CaseStmtBodyScope>(this, stmt);
  }
}

void CaseLabelItemScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  scopeCreator.addToScopeTree(item.getGuardExpr(), this);
}

void CaseStmtBodyScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  scopeCreator.addToScopeTree(stmt->getBody(), this);
}

void SubscriptDeclScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  scopeCreator.addChildrenForKnownAttributes(decl, this);
  auto *leaf = scopeCreator.addNestedGenericParamScopesToTree(
      decl, getPotentiallyOpaqueGenericParams(decl), this);
  scopeCreator.constructExpandAndInsert<ParameterListScope>(
      leaf, decl->getIndices(), decl->getAccessor(AccessorKind::Get));
  scopeCreator.addChildrenForParsedAccessors(decl, leaf);
}

void MacroDeclScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  scopeCreator.addChildrenForKnownAttributes(decl, this);
  auto *leaf = scopeCreator.addNestedGenericParamScopesToTree(
      decl, getPotentiallyOpaqueGenericParams(decl), this);
  if (decl->parameterList) {
    leaf = scopeCreator.constructExpandAndInsert<ParameterListScope>(
        leaf, decl->parameterList, nullptr);
  }
  if (auto def = decl->definition) {
    scopeCreator
      .constructExpandAndInsert<MacroDefinitionScope>(leaf, def);
  }
}

void
MacroDefinitionScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  scopeCreator.addToScopeTree(ASTNode(definition), this);
}

void MacroExpansionDeclScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  // FIXME: If we get attributes on macro expansions, visit them here.
  if (auto argList = decl->getArgs()) {
    for (const auto &arg : *argList) {
      scopeCreator.addExprToScopeTree(arg.getExpr(), this);
    }
  }
}

void CaptureListScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  auto *closureExpr = expr->getClosureBody();
  scopeCreator
      .constructExpandAndInsert<ClosureParametersScope>(this, closureExpr);
}

void ClosureParametersScope::expandAScopeThatDoesNotCreateANewInsertionPoint(
    ScopeCreator &scopeCreator) {
  scopeCreator.addToScopeTree(closureExpr->getBody(), this);
}

void DefaultArgumentInitializerScope::
    expandAScopeThatDoesNotCreateANewInsertionPoint(
        ScopeCreator &scopeCreator) {
  auto *initExpr = decl->getStructuralDefaultExpr();
  ASTScopeAssert(initExpr,
                 "Default argument initializer must have an initializer.");
  scopeCreator.addToScopeTree(initExpr, this);
}

void AttachedPropertyWrapperScope::
    expandAScopeThatDoesNotCreateANewInsertionPoint(
        ScopeCreator &scopeCreator) {
  if (auto *args = attr->getArgs()) {
    for (auto arg : *args)
      scopeCreator.addToScopeTree(arg.getExpr(), this);
  }
}

#pragma mark expandScope

ASTScopeImpl *GenericTypeOrExtensionWholePortion::expandScope(
    GenericTypeOrExtensionScope *scope, ScopeCreator &scopeCreator) const {
  // Get now in case recursion emancipates scope
  auto *const ip = scope->getParent().get();
  
  auto *context = scope->getGenericContext();
  auto *genericParams = (isa<TypeAliasDecl>(context)
                         ? context->getParsedGenericParams()
                         : context->getGenericParams());
  auto *deepestScope = scopeCreator.addNestedGenericParamScopesToTree(
      scope->getDecl(), genericParams, scope);
  if (context->getTrailingWhereClause())
    scope->createTrailingWhereClauseScope(deepestScope, scopeCreator);

  // Prevent circular request bugs caused by illegal input and
  // doing lookups that getExtendedNominal in the midst of getExtendedNominal.
  if (scope->shouldHaveABody() && !scope->doesDeclHaveABody())
    return ip;

  scope->createBodyScope(deepestScope, scopeCreator);
  return ip;
}

ASTScopeImpl *
IterableTypeBodyPortion::expandScope(GenericTypeOrExtensionScope *scope,
                                     ScopeCreator &scopeCreator) const {
  // Get it now in case of recursion and this one gets emancipated
  auto *const ip = scope->getParent().get();
  scope->expandBody(scopeCreator);
  return ip;
}

ASTScopeImpl *GenericTypeOrExtensionWherePortion::expandScope(
    GenericTypeOrExtensionScope *scope, ScopeCreator &) const {
  return scope->getParent().get();
}

#pragma mark createBodyScope

void ExtensionScope::createBodyScope(ASTScopeImpl *leaf,
                                     ScopeCreator &scopeCreator) {
  scopeCreator.constructWithPortionExpandAndInsert<ExtensionScope,
                                                   IterableTypeBodyPortion>(
      leaf, decl);
}
void NominalTypeScope::createBodyScope(ASTScopeImpl *leaf,
                                       ScopeCreator &scopeCreator) {
  scopeCreator.constructWithPortionExpandAndInsert<NominalTypeScope,
                                                   IterableTypeBodyPortion>(
      leaf, decl);
}

#pragma mark createTrailingWhereClauseScope

ASTScopeImpl *GenericTypeOrExtensionScope::createTrailingWhereClauseScope(
    ASTScopeImpl *parent, ScopeCreator &scopeCreator) {
  return parent;
}

ASTScopeImpl *
ExtensionScope::createTrailingWhereClauseScope(ASTScopeImpl *parent,
                                               ScopeCreator &scopeCreator) {
  return scopeCreator.constructWithPortionExpandAndInsert<
      ExtensionScope, GenericTypeOrExtensionWherePortion>(parent, decl);
}
ASTScopeImpl *
NominalTypeScope::createTrailingWhereClauseScope(ASTScopeImpl *parent,
                                                 ScopeCreator &scopeCreator) {
  return scopeCreator.constructWithPortionExpandAndInsert<
      NominalTypeScope, GenericTypeOrExtensionWherePortion>(parent, decl);
}
ASTScopeImpl *
TypeAliasScope::createTrailingWhereClauseScope(ASTScopeImpl *parent,
                                               ScopeCreator &scopeCreator) {
  return scopeCreator.constructWithPortionExpandAndInsert<
      TypeAliasScope, GenericTypeOrExtensionWherePortion>(parent, decl);
}

#pragma mark misc

ASTScopeImpl *LabeledConditionalStmtScope::createNestedConditionalClauseScopes(
    ScopeCreator &scopeCreator, SourceLoc endLoc) {
  auto *stmt = getLabeledConditionalStmt();
  ASTScopeImpl *insertionPoint = this;
  for (auto &sec : stmt->getCond()) {
    switch (sec.getKind()) {
    case StmtConditionElement::CK_Availability:
    case StmtConditionElement::CK_HasSymbol:
      break;
    case StmtConditionElement::CK_Boolean:
      scopeCreator.addToScopeTree(sec.getBoolean(), insertionPoint);
      break;
    case StmtConditionElement::CK_PatternBinding:
      insertionPoint =
          scopeCreator.constructExpandAndInsert<
              ConditionalClausePatternUseScope>(
                  insertionPoint, sec, endLoc);
      break;
    }
  }
  return insertionPoint;
}

AbstractPatternEntryScope::AbstractPatternEntryScope(
    PatternBindingDecl *declBeingScoped, unsigned entryIndex)
    : decl(declBeingScoped), patternEntryIndex(entryIndex) {
  ASTScopeAssert(entryIndex < declBeingScoped->getPatternList().size(),
                 "out of bounds");
}

#pragma mark - expandBody

void FunctionBodyScope::expandBody(ScopeCreator &scopeCreator) {
  scopeCreator.addToScopeTree(decl->getBody(), this);
}

void GenericTypeOrExtensionScope::expandBody(ScopeCreator &) {}

void IterableTypeScope::expandBody(ScopeCreator &scopeCreator) {
  for (auto *d : getIterableDeclContext().get()->getMembers())
    scopeCreator.addToScopeTree(ASTNode(d), this);
}

#pragma mark getScopeCreator
ScopeCreator &ASTScopeImpl::getScopeCreator() {
  return getParent().get()->getScopeCreator();
}

ScopeCreator &ASTSourceFileScope::getScopeCreator() { return *scopeCreator; }

#pragma mark currency
NullablePtr<ASTScopeImpl> ASTScopeImpl::insertionPointForDeferredExpansion() {
  return nullptr;
}

NullablePtr<ASTScopeImpl>
FunctionBodyScope::insertionPointForDeferredExpansion() {
  return getParent().get();
}

NullablePtr<ASTScopeImpl>
IterableTypeScope::insertionPointForDeferredExpansion() {
  return portion->insertionPointForDeferredExpansion(this);
}

NullablePtr<ASTScopeImpl>
GenericTypeOrExtensionWholePortion::insertionPointForDeferredExpansion(
    IterableTypeScope *s) const {
  return s->getParent().get();
}
NullablePtr<ASTScopeImpl>
GenericTypeOrExtensionWherePortion::insertionPointForDeferredExpansion(
    IterableTypeScope *) const {
  return nullptr;
}
NullablePtr<ASTScopeImpl>
IterableTypeBodyPortion::insertionPointForDeferredExpansion(
    IterableTypeScope *s) const {
  return s->getParent().get();
}

#pragma mark verification

void ast_scope::simple_display(llvm::raw_ostream &out,
                               const ScopeCreator *scopeCreator) {
  scopeCreator->print(out);
}
