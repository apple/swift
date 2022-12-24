//===--- CSSyntacticElement.cpp - Syntactic Element Constraints -----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements SyntacticElement constraint generation and solution
// application, which is used to type-check the bodies of closures. It provides
// part of the implementation of the ConstraintSystem class.
//
//===----------------------------------------------------------------------===//

#include "MiscDiagnostics.h"
#include "TypeChecker.h"
#include "TypeCheckAvailability.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/IDETypeChecking.h"

using namespace swift;
using namespace swift::constraints;

namespace {

// Produce an implicit empty tuple expression.
Expr *getVoidExpr(ASTContext &ctx, SourceLoc contextLoc = SourceLoc()) {
  auto *voidExpr = TupleExpr::createEmpty(ctx,
                                          /*LParenLoc=*/contextLoc,
                                          /*RParenLoc=*/contextLoc,
                                          /*Implicit=*/true);
  voidExpr->setType(ctx.TheEmptyTupleType);
  return voidExpr;
}

/// Find any type variable references inside of an AST node.
class TypeVariableRefFinder : public ASTWalker {
  /// A stack of all closures the walker encountered so far.
  SmallVector<DeclContext *> ClosureDCs;

  ConstraintSystem &CS;
  ASTNode Parent;

  llvm::SmallPtrSetImpl<TypeVariableType *> &ReferencedVars;

public:
  TypeVariableRefFinder(
      ConstraintSystem &cs, ASTNode parent,
      llvm::SmallPtrSetImpl<TypeVariableType *> &referencedVars)
      : CS(cs), Parent(parent), ReferencedVars(referencedVars) {
    if (auto *closure = getAsExpr<ClosureExpr>(Parent))
      ClosureDCs.push_back(closure);
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
    if (auto *closure = dyn_cast<ClosureExpr>(expr)) {
      ClosureDCs.push_back(closure);
    }

    if (auto *joinExpr = dyn_cast<TypeJoinExpr>(expr)) {
      // If this join is over a known type, let's
      // analyze it too because it can contain type
      // variables.
      if (!joinExpr->getVar())
        inferVariables(joinExpr->getType());
    }

    if (auto *DRE = dyn_cast<DeclRefExpr>(expr)) {
      auto *decl = DRE->getDecl();

      if (auto type = CS.getTypeIfAvailable(DRE->getDecl())) {
        auto &ctx = CS.getASTContext();
        // If this is not one of the closure parameters which
        // is inferrable from the body, let's replace type
        // variables with errors to avoid bringing external
        // information to the element component.
        if (type->hasTypeVariable() &&
            !(isa<ParamDecl>(decl) || decl->getName() == ctx.Id_builderSelf)) {
          // If there are type variables left in the simplified version,
          // it means that this is an invalid external declaration
          // relative to this element's context.
          if (CS.simplifyType(type)->hasTypeVariable()) {
            auto transformedTy = type.transform([&](Type type) {
              if (auto *typeVar = type->getAs<TypeVariableType>()) {
                return ErrorType::get(CS.getASTContext());
              }
              return type;
            });

            CS.setType(decl, transformedTy);
            return Action::Continue(expr);
          }
        }

        inferVariables(type);
        return Action::Continue(expr);
      }

      auto var = dyn_cast<VarDecl>(decl);
      if (!var)
        return Action::Continue(expr);

      if (auto *wrappedVar = var->getOriginalWrappedProperty()) {
        // If there is no type it means that the body of the
        // closure hasn't been resolved yet, so we can
        // just skip it and wait for \c applyPropertyWrapperToParameter
        // to assign types.
        if (wrappedVar->hasImplicitPropertyWrapper())
          return Action::Continue(expr);

        auto outermostWrapperAttr =
            wrappedVar->getOutermostAttachedPropertyWrapper();

        // If the attribute doesn't have a type it could only mean
        // that the declaration was incorrect.
        if (!CS.hasType(outermostWrapperAttr->getTypeExpr()))
          return Action::Continue(expr);

        auto wrapperType =
            CS.simplifyType(CS.getType(outermostWrapperAttr->getTypeExpr()));

        if (var->getName().hasDollarPrefix()) {
          // $<name> is the projected value var
          CS.setType(var, computeProjectedValueType(wrappedVar, wrapperType));
        } else {
          // _<name> is the wrapper var
          CS.setType(var, wrapperType);
        }

        return Action::Continue(expr);
      }

      // If there is no type recorded yet, let's check whether
      // it is a placeholder variable implicitly generated by the
      // compiler.
      if (auto *PB = var->getParentPatternBinding()) {
        if (auto placeholderTy = isPlaceholderVar(PB)) {
          auto openedTy = CS.replaceInferableTypesWithTypeVars(
              placeholderTy, CS.getConstraintLocator(expr));
          inferVariables(openedTy);
          CS.setType(var, openedTy);
        }
      }
    }

    return Action::Continue(expr);
  }

  PostWalkResult<Expr *> walkToExprPost(Expr *expr) override {
    if (auto *closure = dyn_cast<ClosureExpr>(expr)) {
      ClosureDCs.pop_back();
    }
    return Action::Continue(expr);
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *stmt) override {
    // Return statements have to reference outside result type
    // since all of them are joined by it if it's not specified
    // explicitly.
    if (isa<ReturnStmt>(stmt)) {
      if (auto *closure = getAsExpr<ClosureExpr>(Parent)) {
        // Return is only viable if it belongs to a parent closure.
        if (currentClosureDC() == closure)
          inferVariables(CS.getClosureType(closure)->getResult());
      }
    }

    return Action::Continue(stmt);
  }

private:
  DeclContext *currentClosureDC() const {
    return ClosureDCs.empty() ? nullptr : ClosureDCs.back();
  }

  void inferVariables(Type type) {
    type = type->getWithoutSpecifierType();
    // Record the type variable itself because it has to
    // be in scope even when already bound.
    if (auto *typeVar = type->getAs<TypeVariableType>()) {
      ReferencedVars.insert(typeVar);

      // It is possible that contextual type of a parameter/result
      // has been assigned to e.g. an anonymous or named argument
      // early, to facilitate closure type checking. Such a
      // type can have type variables inside e.g.
      //
      // func test<T>(_: (UnsafePointer<T>) -> Void) {}
      //
      // test { ptr in
      //  ...
      // }
      //
      // Type variable representing `ptr` in the body of
      // this closure would be bound to `UnsafePointer<$T>`
      // in this case, where `$T` is a type variable for a
      // generic parameter `T`.
      type = CS.getFixedTypeRecursive(typeVar, /*wantRValue=*/false);

      if (type->isEqual(typeVar))
        return;
    }

    // Don't walk into the opaque archetypes because they are not
    // transparent in this context - `some P` could reference a
    // type variables as substitutions which are visible only to
    // the outer context.
    if (type->is<OpaqueTypeArchetypeType>())
      return;

    if (type->hasTypeVariable()) {
      SmallPtrSet<TypeVariableType *, 4> typeVars;
      type->getTypeVariables(typeVars);

      // Some of the type variables could be non-representative, so
      // we need to recurse into `inferTypeVariables` to property
      // handle them.
      for (auto *typeVar : typeVars)
        inferVariables(typeVar);
    }
  }
};

/// Find any references to not yet resolved outer closure parameters
/// used in the body of the inner closure. This is required because
/// isolated conjunctions, just like single-expression closures, have
/// to be connected to type variables they are going to use, otherwise
/// they'll get placed in a separate solver component and would never
/// produce a solution.
class UnresolvedClosureParameterCollector : public ASTWalker {
  ConstraintSystem &CS;

  llvm::SmallSetVector<TypeVariableType *, 4> Vars;

public:
  UnresolvedClosureParameterCollector(ConstraintSystem &cs) : CS(cs) {}

  PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
    if (auto *DRE = dyn_cast<DeclRefExpr>(expr)) {
      auto *decl = DRE->getDecl();
      if (isa<ParamDecl>(decl)) {
        if (auto type = CS.getTypeIfAvailable(decl)) {
          if (auto *typeVar = type->getAs<TypeVariableType>()) {
            Vars.insert(typeVar);
          } else if (type->hasTypeVariable()) {
            // Parameter or result type could be only partially
            // resolved e.g. `{ (x: X) -> Void in ... }` where
            // `X` is a generic type.
            SmallPtrSet<TypeVariableType *, 4> typeVars;
            type->getTypeVariables(typeVars);
            Vars.insert(typeVars.begin(), typeVars.end());
          }
        }
      }
    }
    return Action::Continue(expr);
  }

  ArrayRef<TypeVariableType *> getVariables() const {
    return Vars.getArrayRef();
  }
};

// MARK: Constraint generation

/// Check whether it makes sense to convert this element into a constraint.
static bool isViableElement(ASTNode element) {
  if (auto *decl = element.dyn_cast<Decl *>()) {
    // - Ignore variable declarations, they are handled by pattern bindings;
    // - Ignore #if, the chosen children should appear in the
    // surrounding context;
    // - Skip #warning and #error, they are handled during solution
    //   application.
    if (isa<VarDecl>(decl) || isa<IfConfigDecl>(decl) ||
        isa<PoundDiagnosticDecl>(decl))
      return false;
  }

  if (auto *stmt = element.dyn_cast<Stmt *>()) {
    // Empty brace statements are now viable because they do not require
    // inference.
    if (auto *braceStmt = dyn_cast<BraceStmt>(stmt)) {
      return braceStmt->getNumElements() > 0;
    }
  }

  return true;
}

using ElementInfo = std::tuple<ASTNode, ContextualTypeInfo,
                               /*isDiscarded=*/bool, ConstraintLocator *>;

static void createConjunction(ConstraintSystem &cs,
                              ArrayRef<ElementInfo> elements,
                              ConstraintLocator *locator) {
  bool isIsolated = false;

  SmallVector<Constraint *, 4> constraints;
  SmallVector<TypeVariableType *, 2> referencedVars;

  if (locator->directlyAt<ClosureExpr>()) {
    auto *closure = castToExpr<ClosureExpr>(locator->getAnchor());
    // Conjunction associated with the body of the closure has to
    // reference a type variable representing closure type,
    // otherwise it would get disconnected from its contextual type.
    referencedVars.push_back(cs.getType(closure)->castTo<TypeVariableType>());

    // Result builder could be generic but attribute allows its use
    // in "unbound" form (i.e. `@Builder` where `Builder` is defined
    // as `struct Builder<T>`). Generic parameters of such a result
    // builder type are inferable from context, namely from `build*`
    // calls injected by the transform, and are not always resolved at
    // the time conjunction is created.
    //
    // Conjunction needs to reference all the type variables associated
    // with result builder just like parameters and result type of
    // the closure in order to stay connected to its context.
    if (auto builder = cs.getAppliedResultBuilderTransform(closure)) {
      SmallPtrSet<TypeVariableType *, 4> builderVars;
      builder->builderType->getTypeVariables(builderVars);
      referencedVars.append(builderVars.begin(), builderVars.end());
    }

    // Body of the closure is always isolated from its context, only
    // its individual elements are allowed access to type information
    // from the outside e.g. parameters/result type.
    isIsolated = true;
  }

  UnresolvedClosureParameterCollector paramCollector(cs);

  for (const auto &entry : elements) {
    ASTNode element = std::get<0>(entry);
    ContextualTypeInfo context = std::get<1>(entry);
    bool isDiscarded = std::get<2>(entry);
    ConstraintLocator *elementLoc = std::get<3>(entry);

    if (!isViableElement(element))
      continue;

    // If this conjunction going to represent a body of a closure,
    // let's collect references to not yet resolved outer
    // closure parameters.
    if (isIsolated)
      element.walk(paramCollector);

    constraints.push_back(Constraint::createSyntacticElement(
        cs, element, context, elementLoc, isDiscarded));
  }

  // It's possible that there are no viable elements in the body,
  // because e.g. whole body is an `#if` statement or it only has
  // declarations that are checked during solution application.
  // In such cases, let's avoid creating a conjunction.
  if (constraints.empty())
    return;

  for (auto *externalVar : paramCollector.getVariables())
    referencedVars.push_back(externalVar);

  cs.addUnsolvedConstraint(Constraint::createConjunction(
      cs, constraints, isIsolated, locator, referencedVars));
}

ElementInfo makeElement(ASTNode node, ConstraintLocator *locator,
                        ContextualTypeInfo context = ContextualTypeInfo(),
                        bool isDiscarded = false) {
  return std::make_tuple(node, context, isDiscarded, locator);
}

struct SyntacticElementContext
    : public llvm::PointerUnion<AbstractFunctionDecl *, AbstractClosureExpr *> {
  // Inherit the constructors from PointerUnion.
  using PointerUnion::PointerUnion;

  static SyntacticElementContext forFunctionRef(AnyFunctionRef ref) {
    if (auto *decl = ref.getAbstractFunctionDecl()) {
      return {decl};
    }

    return {ref.getAbstractClosureExpr()};
  }

  static SyntacticElementContext forClosure(ClosureExpr *closure) {
    return {closure};
  }

  static SyntacticElementContext forFunction(AbstractFunctionDecl *func) {
    return {func};
  }

  DeclContext *getAsDeclContext() const {
    if (auto *fn = this->dyn_cast<AbstractFunctionDecl *>()) {
      return fn;
    } else if (auto *closure = this->dyn_cast<AbstractClosureExpr *>()) {
      return closure;
    } else {
      llvm_unreachable("unsupported kind");
    }
  }

  NullablePtr<ClosureExpr> getAsClosureExpr() const {
    return dyn_cast_or_null<ClosureExpr>(
        this->dyn_cast<AbstractClosureExpr *>());
  }

  NullablePtr<AbstractClosureExpr> getAsAbstractClosureExpr() const {
    return this->dyn_cast<AbstractClosureExpr *>();
  }

  NullablePtr<AbstractFunctionDecl> getAsAbstractFunctionDecl() const {
    return this->dyn_cast<AbstractFunctionDecl *>();
  }

  Optional<AnyFunctionRef> getAsAnyFunctionRef() const {
    if (auto *fn = this->dyn_cast<AbstractFunctionDecl *>()) {
      return {fn};
    } else if (auto *closure = this->dyn_cast<AbstractClosureExpr *>()) {
      return {closure};
    } else {
      return None;
    }
  }

  BraceStmt *getBody() const {
    if (auto *fn = this->dyn_cast<AbstractFunctionDecl *>()) {
      return fn->getBody();
    } else if (auto *closure = this->dyn_cast<AbstractClosureExpr *>()) {
      return closure->getBody();
    } else {
      llvm_unreachable("unsupported kind");
    }
  }

  bool isSingleExpressionClosure(ConstraintSystem &cs) {
    if (auto ref = getAsAnyFunctionRef()) {
      if (cs.getAppliedResultBuilderTransform(*ref))
        return false;

      if (auto *closure = ref->getAbstractClosureExpr())
        return closure->hasSingleExpressionBody();
    }

    return false;
  }
};

/// Statement visitor that generates constraints for a given closure body.
class SyntacticElementConstraintGenerator
    : public StmtVisitor<SyntacticElementConstraintGenerator, void> {
  friend StmtVisitor<SyntacticElementConstraintGenerator, void>;

  ConstraintSystem &cs;
  SyntacticElementContext context;
  ConstraintLocator *locator;

public:
  /// Whether an error was encountered while generating constraints.
  bool hadError = false;

  SyntacticElementConstraintGenerator(ConstraintSystem &cs,
                                      SyntacticElementContext context,
                                      ConstraintLocator *locator)
      : cs(cs), context(context), locator(locator) {}

  void visitPattern(Pattern *pattern, ContextualTypeInfo context) {
    auto parentElement =
        locator->getLastElementAs<LocatorPathElt::SyntacticElement>();

    if (!parentElement) {
      hadError = true;
      return;
    }

    if (auto *stmt = parentElement->getElement().dyn_cast<Stmt *>()) {
      if (isa<ForEachStmt>(stmt)) {
        visitForEachPattern(pattern, cast<ForEachStmt>(stmt));
        return;
      }

      if (isa<CaseStmt>(stmt)) {
        visitCaseItemPattern(pattern, context);
        return;
      }
    }

    llvm_unreachable("Unsupported pattern");
  }

  void visitCaseItem(CaseLabelItem *caseItem, ContextualTypeInfo contextInfo) {
    assert(contextInfo.purpose == CTP_CaseStmt);

    // Resolve the pattern.
    auto *pattern = caseItem->getPattern();
    if (!caseItem->isPatternResolved()) {
      pattern = TypeChecker::resolvePattern(pattern, context.getAsDeclContext(),
                                            /*isStmtCondition=*/false);
      if (!pattern) {
        hadError = true;
        return;
      }

      caseItem->setPattern(pattern, /*resolved=*/true);
    }

    // Let's generate constraints for pattern + where clause.
    // The assumption is that this shouldn't be too complex
    // to handle, but if it turns out to be false, this could
    // always be converted into a conjunction.

    // Generate constraints for pattern.
    visitPattern(pattern, contextInfo);

    auto *guardExpr = caseItem->getGuardExpr();

    // Generate constraints for `where` clause (if any).
    if (guardExpr) {
      guardExpr = cs.generateConstraints(guardExpr, context.getAsDeclContext());
      if (!guardExpr) {
        hadError = true;
        return;
      }
    }

    // Save information about case item so it could be referenced during
    // solution application.
    cs.setCaseLabelItemInfo(caseItem, {pattern, guardExpr});
  }

private:
  /// This method handles both pattern and the sequence expression
  /// associated with `for-in` loop because types in this situation
  /// flow in both directions:
  ///
  /// - From pattern to sequence, informing its element type e.g.
  ///   `for i: Int8 in 0 ..< 8`
  ///
  /// - From sequence to pattern, when pattern has no type information.
  void visitForEachPattern(Pattern *pattern, ForEachStmt *forEachStmt) {
    auto target = SolutionApplicationTarget::forForEachStmt(
        forEachStmt, context.getAsDeclContext(),
        /*bindTypeVarsOneWay=*/false);

    if (cs.generateConstraints(target, FreeTypeVariableBinding::Disallow)) {
      hadError = true;
      return;
    }

    // After successful constraint generation, let's record
    // solution application target with all relevant information.
    cs.setSolutionApplicationTarget(forEachStmt, target);
  }

  void visitCaseItemPattern(Pattern *pattern, ContextualTypeInfo context) {
    Type patternType = cs.generateConstraints(
        pattern, locator, /*bindPatternVarsOneWay=*/false,
        /*patternBinding=*/nullptr, /*patternIndex=*/0);

    if (!patternType) {
      hadError = true;
      return;
    }

    // Convert the contextual type to the pattern, which establishes the
    // bindings.
    cs.addConstraint(ConstraintKind::Conversion, context.getType(), patternType,
                     locator);

    // For any pattern variable that has a parent variable (i.e., another
    // pattern variable with the same name in the same case), require that
    // the types be equivalent.
    pattern->forEachNode([&](Pattern *pattern) {
      auto namedPattern = dyn_cast<NamedPattern>(pattern);
      if (!namedPattern)
        return;

      auto var = namedPattern->getDecl();
      if (auto parentVar = var->getParentVarDecl()) {
        cs.addConstraint(
            ConstraintKind::Equal, cs.getType(parentVar), cs.getType(var),
            cs.getConstraintLocator(
                locator, LocatorPathElt::PatternMatch(namedPattern)));
      }
    });
  }

  void visitPatternBinding(PatternBindingDecl *patternBinding,
                           SmallVectorImpl<ElementInfo> &patterns) {
    auto *baseLoc = cs.getConstraintLocator(
        locator, LocatorPathElt::SyntacticElement(patternBinding));

    for (unsigned index : range(patternBinding->getNumPatternEntries())) {
      if (patternBinding->isInitializerChecked(index))
        continue;

      auto *pattern = TypeChecker::resolvePattern(
          patternBinding->getPattern(index), patternBinding->getDeclContext(),
          /*isStmtCondition=*/true);

      if (!pattern) {
        hadError = true;
        return;
      }

      // Reset binding to point to the resolved pattern. This is required
      // before calling `forPatternBindingDecl`.
      patternBinding->setPattern(index, pattern,
                                 patternBinding->getInitContext(index));

      patterns.push_back(makeElement(
          patternBinding,
          cs.getConstraintLocator(
              baseLoc, LocatorPathElt::PatternBindingElement(index))));
    }
  }

  Optional<SolutionApplicationTarget>
  getTargetForPattern(PatternBindingDecl *patternBinding, unsigned index,
                      Type patternType) {
    auto hasPropertyWrapper = [&](Pattern *pattern) -> bool {
      if (auto *singleVar = pattern->getSingleVar())
        return singleVar->hasAttachedPropertyWrapper();
      return false;
    };

    auto *pattern = patternBinding->getPattern(index);
    auto *init = patternBinding->getInit(index);

    if (!init && patternBinding->isDefaultInitializable(index) &&
        pattern->hasStorage()) {
      init = TypeChecker::buildDefaultInitializer(patternType);
    }

    // A property wrapper initializer (either user-defined
    // or a synthesized one) has to be pre-checked before use.
    //
    // This is not a problem in top-level code because pattern
    // bindings go through `typeCheckExpression` which does
    // pre-check automatically and result builders do not allow
    // declaring local wrapped variables (yet).
    if (hasPropertyWrapper(pattern)) {
      auto target = SolutionApplicationTarget::forInitialization(
          init, patternBinding->getDeclContext(), patternType, patternBinding,
          index,
          /*bindPatternVarsOneWay=*/false);

      if (ConstraintSystem::preCheckTarget(
              target, /*replaceInvalidRefsWithErrors=*/true,
              /*LeaveCLosureBodyUnchecked=*/false))
        return None;

      return target;
    }

    if (init) {
      return SolutionApplicationTarget::forInitialization(
          init, patternBinding->getDeclContext(), patternType, patternBinding,
          index,
          /*bindPatternVarsOneWay=*/false);
    }

    return SolutionApplicationTarget::forUninitializedVar(patternBinding, index,
                                                          patternType);
  }

  void visitPatternBindingElement(PatternBindingDecl *patternBinding) {
    assert(locator->isLastElement<LocatorPathElt::PatternBindingElement>());

    auto index =
        locator->castLastElementTo<LocatorPathElt::PatternBindingElement>()
            .getIndex();

    if (patternBinding->isInitializerChecked(index))
      return;

    auto contextualPattern =
        ContextualPattern::forPatternBindingDecl(patternBinding, index);
    Type patternType = TypeChecker::typeCheckPattern(contextualPattern);

    // Fail early if pattern couldn't be type-checked.
    if (!patternType || patternType->hasError()) {
      hadError = true;
      return;
    }

    auto target = getTargetForPattern(patternBinding, index, patternType);
    if (!target) {
      hadError = true;
      return;
    }

    // Keep track of this binding entry.
    cs.setSolutionApplicationTarget({patternBinding, index}, *target);

    if (isPlaceholderVar(patternBinding))
      return;

    if (cs.generateConstraints(*target, FreeTypeVariableBinding::Disallow)) {
      hadError = true;
      return;
    }
  }

  void visitDecl(Decl *decl) {
    if (!context.isSingleExpressionClosure(cs)) {
      if (auto patternBinding = dyn_cast<PatternBindingDecl>(decl)) {
        if (locator->isLastElement<LocatorPathElt::PatternBindingElement>())
          visitPatternBindingElement(patternBinding);
        else
          llvm_unreachable("cannot visit pattern binding directly");
        return;
      }
    }

    // Just ignore #if; the chosen children should appear in the
    // surrounding context.  This isn't good for source tools but it
    // at least works.
    if (isa<IfConfigDecl>(decl))
      return;

    // Skip #warning/#error; we'll handle them when applying the closure.
    if (isa<PoundDiagnosticDecl>(decl))
      return;

    // Ignore variable declarations, because they're always handled within
    // their enclosing pattern bindings.
    if (isa<VarDecl>(decl))
      return;

    // Other declarations will be handled at application time.
  }

  void visitBreakStmt(BreakStmt *breakStmt) {
  }

  void visitContinueStmt(ContinueStmt *continueStmt) {
  }

  void visitDeferStmt(DeferStmt *deferStmt) {
  }

  void visitFallthroughStmt(FallthroughStmt *fallthroughStmt) {
  }

  void visitStmtCondition(LabeledConditionalStmt *S,
                          SmallVectorImpl<ElementInfo> &elements,
                          ConstraintLocator *locator) {
    auto *condLocator =
        cs.getConstraintLocator(locator, ConstraintLocator::Condition);
    for (auto &condition : S->getCond())
      elements.push_back(makeElement(&condition, condLocator));
  }

  void visitIfStmt(IfStmt *ifStmt) {
    SmallVector<ElementInfo, 4> elements;

    // Condition
    visitStmtCondition(ifStmt, elements, locator);

    // Then Branch
    {
      auto *thenLoc = cs.getConstraintLocator(
        locator, LocatorPathElt::TernaryBranch(/*then=*/true));
      elements.push_back(makeElement(ifStmt->getThenStmt(), thenLoc));
    }

    // Else Branch (if any).
    if (auto *elseStmt = ifStmt->getElseStmt()) {
      auto *elseLoc = cs.getConstraintLocator(
          locator, LocatorPathElt::TernaryBranch(/*then=*/false));
      elements.push_back(makeElement(ifStmt->getElseStmt(), elseLoc));
    }

    createConjunction(cs, elements, locator);
  }

  void visitGuardStmt(GuardStmt *guardStmt) {
    SmallVector<ElementInfo, 4> elements;

    visitStmtCondition(guardStmt, elements, locator);
    elements.push_back(makeElement(guardStmt->getBody(), locator));

    createConjunction(cs, elements, locator);
  }

  void visitWhileStmt(WhileStmt *whileStmt) {
    SmallVector<ElementInfo, 4> elements;

    visitStmtCondition(whileStmt, elements, locator);
    elements.push_back(makeElement(whileStmt->getBody(), locator));

    createConjunction(cs, elements, locator);
  }

  void visitDoStmt(DoStmt *doStmt) {
    visitBraceStmt(doStmt->getBody());
  }

  void visitRepeatWhileStmt(RepeatWhileStmt *repeatWhileStmt) {
    createConjunction(cs,
                      {makeElement(repeatWhileStmt->getCond(),
                                   cs.getConstraintLocator(
                                       locator, ConstraintLocator::Condition),
                                   getContextForCondition()),
                       makeElement(repeatWhileStmt->getBody(), locator)},
                      locator);
  }

  void visitPoundAssertStmt(PoundAssertStmt *poundAssertStmt) {
    createConjunction(cs,
                      {makeElement(poundAssertStmt->getCondition(),
                                   cs.getConstraintLocator(
                                       locator, ConstraintLocator::Condition),
                                   getContextForCondition())},
                      locator);
  }

  void visitThrowStmt(ThrowStmt *throwStmt) {
    if (!cs.getASTContext().getErrorDecl()) {
      hadError = true;
      return;
    }

    auto errType = cs.getASTContext().getErrorExistentialType();
    auto *errorExpr = throwStmt->getSubExpr();

    createConjunction(
        cs,
        {makeElement(
            errorExpr,
            cs.getConstraintLocator(
                locator, LocatorPathElt::SyntacticElement(errorExpr)),
            {errType, CTP_ThrowStmt})},
        locator);
  }

  void visitForEachStmt(ForEachStmt *forEachStmt) {
    auto *stmtLoc = cs.getConstraintLocator(locator);

    SmallVector<ElementInfo, 4> elements;

    // For-each pattern.
    //
    // Note that we don't record a sequence or where clause here,
    // they would be handled together with pattern because pattern can
    // inform a type of sequence element e.g. `for i: Int8 in 0 ..< 8`
    elements.push_back(makeElement(forEachStmt->getPattern(), stmtLoc));
    // Body of the `for-in` loop.
    elements.push_back(makeElement(forEachStmt->getBody(), stmtLoc));

    createConjunction(cs, elements, locator);
  }

  void visitSwitchStmt(SwitchStmt *switchStmt) {
    auto *switchLoc = cs.getConstraintLocator(
        locator, LocatorPathElt::SyntacticElement(switchStmt));

    SmallVector<ElementInfo, 4> elements;
    {
      auto *subjectExpr = switchStmt->getSubjectExpr();
      {
        elements.push_back(makeElement(subjectExpr, switchLoc));

        SolutionApplicationTarget target(subjectExpr,
                                         context.getAsDeclContext(), CTP_Unused,
                                         Type(), /*isDiscarded=*/false);

        cs.setSolutionApplicationTarget(switchStmt, target);
      }

      for (auto rawCase : switchStmt->getRawCases())
        elements.push_back(makeElement(rawCase, switchLoc));
    }

    createConjunction(cs, elements, switchLoc);
  }

  void visitDoCatchStmt(DoCatchStmt *doStmt) {
    auto *doLoc = cs.getConstraintLocator(
        locator, LocatorPathElt::SyntacticElement(doStmt));

    SmallVector<ElementInfo, 4> elements;

    // First, let's record a body of `do` statement.
    elements.push_back(makeElement(doStmt->getBody(), doLoc));

    // After that has been type-checked, let's switch to
    // individual `catch` statements.
    for (auto *catchStmt : doStmt->getCatches())
      elements.push_back(makeElement(catchStmt, doLoc));

    createConjunction(cs, elements, doLoc);
  }

  void visitCaseStmt(CaseStmt *caseStmt) {
    Type contextualTy;

    {
      auto parent =
          locator->castLastElementTo<LocatorPathElt::SyntacticElement>()
              .getElement();

      if (parent.isStmt(StmtKind::Switch)) {
        auto *switchStmt = cast<SwitchStmt>(parent.get<Stmt *>());
        contextualTy = cs.getType(switchStmt->getSubjectExpr());
      } else if (parent.isStmt(StmtKind::DoCatch)) {
        contextualTy = cs.getASTContext().getErrorExistentialType();
      } else {
        hadError = true;
        return;
      }
    }

    auto *caseLoc = cs.getConstraintLocator(
        locator, LocatorPathElt::SyntacticElement(caseStmt));

    SmallVector<ElementInfo, 4> elements;
    for (auto &caseLabelItem : caseStmt->getMutableCaseLabelItems()) {
      elements.push_back(
          makeElement(&caseLabelItem, caseLoc, {contextualTy, CTP_CaseStmt}));
    }

    elements.push_back(makeElement(caseStmt->getBody(), caseLoc));

    createConjunction(cs, elements, caseLoc);
  }

  void visitBraceStmt(BraceStmt *braceStmt) {
    auto &ctx = cs.getASTContext();

    if (context.isSingleExpressionClosure(cs)) {
      for (auto node : braceStmt->getElements()) {
        if (auto expr = node.dyn_cast<Expr *>()) {
          auto generatedExpr = cs.generateConstraints(
            expr, context.getAsDeclContext(), /*isInputExpression=*/false);
          if (!generatedExpr) {
            hadError = true;
          }
        } else if (auto stmt = node.dyn_cast<Stmt *>()) {
          visit(stmt);
        } else {
          visitDecl(node.get<Decl *>());
        }
      }
      return;
    }

    // If this brace statement represents a body of an empty or
    // multi-statement closure.
    if (locator->directlyAt<ClosureExpr>()) {
      auto *closure = context.getAsClosureExpr().get();
      // If this closure has an empty body and no explicit result type
      // let's bind result type to `Void` since that's the only type empty
      // body can produce. Otherwise, if (multi-statement) closure doesn't
      // have an explicit result (no `return` statements) let's default it to
      // `Void`.
      //
      // Note that result builder bodies always have a `return` statement
      // at the end, so they don't need to be defaulted.
      if (!cs.getAppliedResultBuilderTransform({closure}) &&
          !hasExplicitResult(closure)) {
        auto constraintKind =
            (closure->hasEmptyBody() && !closure->hasExplicitResultType())
                ? ConstraintKind::Bind
                : ConstraintKind::Defaultable;

        cs.addConstraint(
            constraintKind, cs.getClosureType(closure)->getResult(),
            ctx.TheEmptyTupleType,
            cs.getConstraintLocator(closure, ConstraintLocator::ClosureResult));
      }

      // Let's not walk into the body if empty or multi-statement closure
      // doesn't participate in inference.
      if (!cs.participatesInInference(closure))
        return;
    }

    if (isChildOf(StmtKind::Case)) {
      auto *caseStmt = cast<CaseStmt>(
          locator->castLastElementTo<LocatorPathElt::SyntacticElement>()
              .asStmt());

      if (recordInferredSwitchCasePatternVars(caseStmt)) {
        hadError = true;
      }
    }

    SmallVector<ElementInfo, 4> elements;
    for (auto element : braceStmt->getElements()) {
      bool isDiscarded =
          element.is<Expr *>() &&
          (!ctx.LangOpts.Playground && !ctx.LangOpts.DebuggerSupport);

      if (auto *decl = element.dyn_cast<Decl *>()) {
        if (auto *PDB = dyn_cast<PatternBindingDecl>(decl)) {
          visitPatternBinding(PDB, elements);
          continue;
        }
      }

      elements.push_back(
          makeElement(element,
                      cs.getConstraintLocator(
                          locator, LocatorPathElt::SyntacticElement(element)),
                      /*contextualInfo=*/{}, isDiscarded));
    }

    createConjunction(cs, elements, locator);
  }

  void visitReturnStmt(ReturnStmt *returnStmt) {
    // Single-expression closures are effectively a `return` statement,
    // so let's give them a special locator as to indicate that.
    // Return statements might not have a result if we have a closure whose
    // implicit returned value is coerced to Void.
    if (context.isSingleExpressionClosure(cs) && returnStmt->hasResult()) {
      auto *expr = returnStmt->getResult();
      assert(expr && "single expression closure without expression?");

      expr = cs.generateConstraints(expr, context.getAsDeclContext(),
                                    /*isInputExpression=*/false);
      if (!expr) {
        hadError = true;
        return;
      }

      auto contextualResultInfo = getContextualResultInfo();
      cs.addConstraint(ConstraintKind::Conversion, cs.getType(expr),
                       contextualResultInfo.getType(),
                       cs.getConstraintLocator(
                           context.getAsAbstractClosureExpr().get(),
                           LocatorPathElt::ClosureBody(
                               /*hasReturn=*/!returnStmt->isImplicit())));
      return;
    }

    Expr *resultExpr;

    if (returnStmt->hasResult()) {
      resultExpr = returnStmt->getResult();
      assert(resultExpr && "non-empty result without expression?");
    } else {
      // If this is simplify `return`, let's create an empty tuple
      // which is also useful if contextual turns out to be e.g. `Void?`.
      // Also, attach return stmt source location so if there is a contextual
      // mismatch we can produce a diagnostic in a valid source location.
      resultExpr = getVoidExpr(cs.getASTContext(), returnStmt->getEndLoc());
    }

    auto contextualResultInfo = getContextualResultInfo();
    SolutionApplicationTarget target(resultExpr, context.getAsDeclContext(),
                                     contextualResultInfo.purpose,
                                     contextualResultInfo.getType(),
                                     /*isDiscarded=*/false);

    if (cs.generateConstraints(target, FreeTypeVariableBinding::Disallow)) {
      hadError = true;
      return;
    }

    cs.setContextualType(target.getAsExpr(),
                         TypeLoc::withoutLoc(contextualResultInfo.getType()),
                         contextualResultInfo.purpose);
    cs.setSolutionApplicationTarget(returnStmt, target);
  }

  ContextualTypeInfo getContextualResultInfo() const {
    auto funcRef = context.getAsAnyFunctionRef();
    if (!funcRef)
      return {Type(), CTP_Unused};

    if (auto transform = cs.getAppliedResultBuilderTransform(*funcRef))
      return {transform->bodyResultType, CTP_ReturnStmt};

    if (auto *closure =
            getAsExpr<ClosureExpr>(funcRef->getAbstractClosureExpr()))
      return {cs.getClosureType(closure)->getResult(), CTP_ClosureResult};

    return {funcRef->getBodyResultType(), CTP_ReturnStmt};
  }

#define UNSUPPORTED_STMT(STMT) void visit##STMT##Stmt(STMT##Stmt *) { \
      llvm_unreachable("Unsupported statement kind " #STMT);          \
  }
  UNSUPPORTED_STMT(Yield)
  UNSUPPORTED_STMT(Fail)
#undef UNSUPPORTED_STMT

private:
  ContextualTypeInfo getContextForCondition() const {
    auto boolDecl = cs.getASTContext().getBoolDecl();
    assert(boolDecl && "Bool is missing");
    return {boolDecl->getDeclaredInterfaceType(), CTP_Condition};
  }

  bool isChildOf(StmtKind kind) {
    if (locator->getPath().empty())
      return false;

    auto parentElt =
        locator->getLastElementAs<LocatorPathElt::SyntacticElement>();
    return parentElt ? parentElt->getElement().isStmt(kind) : false;
  }

  bool recordInferredSwitchCasePatternVars(CaseStmt *caseStmt) {
    llvm::SmallDenseMap<Identifier, SmallVector<VarDecl *, 2>, 4> patternVars;

    auto recordVar = [&](VarDecl *var) {
      if (!var->hasName())
        return;
      patternVars[var->getName()].push_back(var);
    };

    for (auto &caseItem : caseStmt->getMutableCaseLabelItems()) {
      assert(caseItem.isPatternResolved());

      auto *pattern = caseItem.getPattern();
      pattern->forEachVariable([&](VarDecl *var) { recordVar(var); });
    }

    for (auto bodyVar : caseStmt->getCaseBodyVariablesOrEmptyArray()) {
      if (!bodyVar->hasName())
        continue;

      const auto &variants = patternVars[bodyVar->getName()];

      auto getType = [&](VarDecl *var) {
        auto type = cs.simplifyType(cs.getType(var));
        assert(!type->hasTypeVariable());
        return type;
      };

      switch (variants.size()) {
      case 0:
        break;

      case 1:
        // If there is only one choice here, let's use it directly.
        cs.setType(bodyVar, getType(variants.front()));
        break;

      default: {
        // If there are multiple choices it could only mean multiple
        // patterns e.g. `.a(let x), .b(let x), ...:`. Let's join them.
        Type joinType = getType(variants.front());

        SmallVector<VarDecl *, 2> conflicts;
        for (auto *var : llvm::drop_begin(variants)) {
          auto varType = getType(var);
          // Type mismatch between different patterns.
          if (!joinType->isEqual(varType))
            conflicts.push_back(var);
        }

        if (!conflicts.empty()) {
          if (!cs.shouldAttemptFixes())
            return true;

          // dfdf
          auto *locator = cs.getConstraintLocator(bodyVar);
          if (cs.recordFix(RenameConflictingPatternVariables::create(
                  cs, joinType, conflicts, locator)))
            return true;
        }

        cs.setType(bodyVar, joinType);
      }
      }
    }

    return false;
  }
};
}

bool ConstraintSystem::generateConstraints(AnyFunctionRef fn, BraceStmt *body) {
  NullablePtr<ConstraintLocator> locator;

  if (auto *func = fn.getAbstractFunctionDecl()) {
    locator = getConstraintLocator(func);
  } else {
    locator = getConstraintLocator(fn.getAbstractClosureExpr());
  }

  SyntacticElementConstraintGenerator generator(
      *this, SyntacticElementContext::forFunctionRef(fn), locator.get());

  generator.visit(body);

  return generator.hadError;
}

bool ConstraintSystem::isInResultBuilderContext(ClosureExpr *closure) const {
  if (!closure->hasSingleExpressionBody()) {
    auto *DC = closure->getParent();
    do {
      // Result builder is applied to a function/getter body.
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(DC)) {
        if (resultBuilderTransformed.count(AFD))
          return true;
      }

      if (auto *parentClosure = dyn_cast<ClosureExpr>(DC)) {
        if (resultBuilderTransformed.count(parentClosure))
          return true;
      }
    } while ((DC = DC->getParent()));
  }
  return false;
}

bool isConditionOfStmt(ConstraintLocatorBuilder locator) {
  auto last = locator.last();
  if (!(last && last->is<LocatorPathElt::Condition>()))
    return false;

  SmallVector<LocatorPathElt, 4> path;
  (void)locator.getLocatorParts(path);

  path.pop_back();

  if (path.empty())
    return false;

  if (auto closureElt = path.back().getAs<LocatorPathElt::SyntacticElement>())
    return closureElt->getElement().dyn_cast<Stmt *>();

  return false;
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifySyntacticElementConstraint(
    ASTNode element, ContextualTypeInfo contextInfo, bool isDiscarded,
    TypeMatchOptions flags, ConstraintLocatorBuilder locator) {
  auto anchor = locator.getAnchor();

  Optional<SyntacticElementContext> context;
  if (auto *closure = getAsExpr<ClosureExpr>(anchor)) {
    context = SyntacticElementContext::forClosure(closure);
  } else if (auto *fn = getAsDecl<AbstractFunctionDecl>(anchor)) {
    context = SyntacticElementContext::forFunction(fn);
  } else {
    return SolutionKind::Error;
  }

  SyntacticElementConstraintGenerator generator(*this, *context,
                                                getConstraintLocator(locator));

  if (auto *expr = element.dyn_cast<Expr *>()) {
    SolutionApplicationTarget target(expr, context->getAsDeclContext(),
                                     contextInfo.purpose, contextInfo.getType(),
                                     isDiscarded);

    if (generateConstraints(target, FreeTypeVariableBinding::Disallow))
      return SolutionKind::Error;

    setSolutionApplicationTarget(expr, target);
    return SolutionKind::Solved;
  } else if (auto *stmt = element.dyn_cast<Stmt *>()) {
    generator.visit(stmt);
  } else if (auto *cond = element.dyn_cast<StmtConditionElement *>()) {
    if (generateConstraints({*cond}, context->getAsDeclContext()))
      return SolutionKind::Error;
  } else if (auto *pattern = element.dyn_cast<Pattern *>()) {
    generator.visitPattern(pattern, contextInfo);
  } else if (auto *caseItem = element.dyn_cast<CaseLabelItem *>()) {
    generator.visitCaseItem(caseItem, contextInfo);
  } else {
    generator.visit(element.get<Decl *>());
  }

  return generator.hadError ? SolutionKind::Error : SolutionKind::Solved;
}

// MARK: Solution application

namespace {

/// Statement visitor that applies constraints for a given closure body.
class SyntacticElementSolutionApplication
    : public StmtVisitor<SyntacticElementSolutionApplication, ASTNode> {
  friend StmtVisitor<SyntacticElementSolutionApplication, ASTNode>;
  friend class ResultBuilderRewriter;

protected:
  Solution &solution;
  SyntacticElementContext context;
  Type resultType;
  RewriteTargetFn rewriteTarget;

  /// All `func`s declared in the body of the closure.
  SmallVector<FuncDecl *, 4> LocalFuncs;

public:
  /// Whether an error was encountered while generating constraints.
  bool hadError = false;

  SyntacticElementSolutionApplication(Solution &solution,
                                      SyntacticElementContext context,
                                      Type resultType,
                                      RewriteTargetFn rewriteTarget)
      : solution(solution), context(context), resultType(resultType),
        rewriteTarget(rewriteTarget) {}

  virtual ~SyntacticElementSolutionApplication() {}

private:
  ASTNode visit(Stmt *S) {
    auto rewritten = ASTVisitor::visit(S);
    if (!rewritten)
      return {};

    if (auto *stmt = getAsStmt(rewritten))
      performStmtDiagnostics(stmt, context.getAsDeclContext());

    return rewritten;
  }

  void visitDecl(Decl *decl) {
    if (isa<IfConfigDecl>(decl))
      return;

    // Generate constraints for pattern binding declarations.
    if (auto patternBinding = dyn_cast<PatternBindingDecl>(decl)) {
      SolutionApplicationTarget target(patternBinding);

      // If this is a placeholder varaible with an initializer, let's set
      // the inferred type, and ask `typeCheckDecl` to type-check initializer.
      if (isPlaceholderVar(patternBinding) && patternBinding->getInit(0)) {
        auto *pattern = patternBinding->getPattern(0);
        pattern->setType(
            solution.getResolvedType(patternBinding->getSingleVar()));

        TypeChecker::typeCheckDecl(decl);
        return;
      }

      if (!rewriteTarget(target)) {
        hadError = true;
        return;
      }

      // Allow `typeCheckDecl` to be called after solution is applied
      // to a pattern binding. That would materialize required
      // information e.g. accessors and do access/availability checks.
    }

    // Local functions cannot be type-checked in-order because they can
    // capture variables declared after them. Let's save them to be
    // processed after the solution has been applied to the body.
    if (auto *func = dyn_cast<FuncDecl>(decl)) {
      LocalFuncs.push_back(func);
      return;
    }

    TypeChecker::typeCheckDecl(decl);
  }

  ASTNode visitBreakStmt(BreakStmt *breakStmt) {
    // Force the target to be computed in case it produces diagnostics.
    (void)breakStmt->getTarget();
    return breakStmt;
  }

  ASTNode visitContinueStmt(ContinueStmt *continueStmt) {
    // Force the target to be computed in case it produces diagnostics.
    (void)continueStmt->getTarget();
    return continueStmt;
  }

  ASTNode visitFallthroughStmt(FallthroughStmt *fallthroughStmt) {
    if (checkFallthroughStmt(context.getAsDeclContext(), fallthroughStmt))
      hadError = true;
    return fallthroughStmt;
  }

  ASTNode visitDeferStmt(DeferStmt *deferStmt) {
    TypeChecker::typeCheckDecl(deferStmt->getTempDecl());

    Expr *theCall = deferStmt->getCallExpr();
    TypeChecker::typeCheckExpression(theCall, context.getAsDeclContext());
    deferStmt->setCallExpr(theCall);

    return deferStmt;
  }

  ASTNode visitIfStmt(IfStmt *ifStmt) {
    // Rewrite the condition.
    if (auto condition = rewriteTarget(SolutionApplicationTarget(
            ifStmt->getCond(), context.getAsDeclContext())))
      ifStmt->setCond(*condition->getAsStmtCondition());
    else
      hadError = true;

    ifStmt->setThenStmt(visit(ifStmt->getThenStmt()).get<Stmt *>());

    if (auto elseStmt = ifStmt->getElseStmt()) {
      ifStmt->setElseStmt(visit(elseStmt).get<Stmt *>());
    }

    return ifStmt;
  }

  ASTNode visitGuardStmt(GuardStmt *guardStmt) {
    if (auto condition = rewriteTarget(SolutionApplicationTarget(
            guardStmt->getCond(), context.getAsDeclContext())))
      guardStmt->setCond(*condition->getAsStmtCondition());
    else
      hadError = true;

    auto *body = visit(guardStmt->getBody()).get<Stmt *>();
    guardStmt->setBody(cast<BraceStmt>(body));
    return guardStmt;
  }

  ASTNode visitWhileStmt(WhileStmt *whileStmt) {
    if (auto condition = rewriteTarget(SolutionApplicationTarget(
            whileStmt->getCond(), context.getAsDeclContext())))
      whileStmt->setCond(*condition->getAsStmtCondition());
    else
      hadError = true;

    auto *body = visit(whileStmt->getBody()).get<Stmt *>();
    whileStmt->setBody(cast<BraceStmt>(body));
    return whileStmt;
  }

  virtual ASTNode visitDoStmt(DoStmt *doStmt) {
    auto body = visit(doStmt->getBody()).get<Stmt *>();
    doStmt->setBody(cast<BraceStmt>(body));
    return doStmt;
  }

  ASTNode visitRepeatWhileStmt(RepeatWhileStmt *repeatWhileStmt) {
    auto body = visit(repeatWhileStmt->getBody()).get<Stmt *>();
    repeatWhileStmt->setBody(cast<BraceStmt>(body));

    // Rewrite the condition.
    auto &cs = solution.getConstraintSystem();
    auto target = *cs.getSolutionApplicationTarget(repeatWhileStmt->getCond());
    if (auto condition = rewriteTarget(target))
      repeatWhileStmt->setCond(condition->getAsExpr());
    else
      hadError = true;

    return repeatWhileStmt;
  }

  ASTNode visitPoundAssertStmt(PoundAssertStmt *poundAssertStmt) {
    // FIXME: This should be done through \c solution instead of
    //        constraint system.
    auto &cs = solution.getConstraintSystem();
    // Rewrite the condition.
    auto target =
        *cs.getSolutionApplicationTarget(poundAssertStmt->getCondition());

    if (auto result = rewriteTarget(target))
      poundAssertStmt->setCondition(result->getAsExpr());
    else
      hadError = true;

    return poundAssertStmt;
  }

  ASTNode visitThrowStmt(ThrowStmt *throwStmt) {
    auto &cs = solution.getConstraintSystem();

    // Rewrite the error.
    auto target = *cs.getSolutionApplicationTarget(throwStmt->getSubExpr());
    if (auto result = rewriteTarget(target))
      throwStmt->setSubExpr(result->getAsExpr());
    else
      hadError = true;

    return throwStmt;
  }

  ASTNode visitForEachStmt(ForEachStmt *forEachStmt) {
    ConstraintSystem &cs = solution.getConstraintSystem();

    auto forEachTarget =
        rewriteTarget(*cs.getSolutionApplicationTarget(forEachStmt));

    if (!forEachTarget)
      hadError = true;

    auto body = visit(forEachStmt->getBody()).get<Stmt *>();
    forEachStmt->setBody(cast<BraceStmt>(body));

    // Check to see if the sequence expr is throwing (in async context),
    // if so require the stmt to have a `try`.
    hadError |= diagnoseUnhandledThrowsInAsyncContext(
        context.getAsDeclContext(), forEachStmt);

    return forEachStmt;
  }

  ASTNode visitSwitchStmt(SwitchStmt *switchStmt) {
    ConstraintSystem &cs = solution.getConstraintSystem();

    // Rewrite the switch subject.
    auto subjectTarget =
        rewriteTarget(*cs.getSolutionApplicationTarget(switchStmt));
    if (subjectTarget) {
      switchStmt->setSubjectExpr(subjectTarget->getAsExpr());
    } else {
      hadError = true;
    }

    // Visit the raw cases.
    bool limitExhaustivityChecks = false;
    for (auto rawCase : switchStmt->getRawCases()) {
      if (auto decl = rawCase.dyn_cast<Decl *>()) {
        visitDecl(decl);
        continue;
      }

      auto caseStmt = cast<CaseStmt>(rawCase.get<Stmt *>());
      // Body of the `case` statement can contain a `fallthrough`
      // statement that requires both source and destination
      // `case` preambles to be type-checked, so bodies of `case`
      // statements should be visited after preambles.
      visitCaseStmtPreamble(caseStmt);
    }

    for (auto *caseStmt : switchStmt->getCases()) {
      visitCaseStmtBody(caseStmt);

      // Check restrictions on '@unknown'.
      if (caseStmt->hasUnknownAttr()) {
        checkUnknownAttrRestrictions(cs.getASTContext(), caseStmt,
                                     limitExhaustivityChecks);
      }
    }

    TypeChecker::checkSwitchExhaustiveness(
        switchStmt, context.getAsDeclContext(), limitExhaustivityChecks);

    return switchStmt;
  }

  ASTNode visitDoCatchStmt(DoCatchStmt *doStmt) {
    // Translate the body.
    auto newBody = visit(doStmt->getBody());
    doStmt->setBody(newBody.get<Stmt *>());

    // Visit the catch blocks.
    for (auto catchStmt : doStmt->getCatches())
      visitCaseStmt(catchStmt);

    return doStmt;
  }

  void visitCaseStmtPreamble(CaseStmt *caseStmt) {
    // Translate the patterns and guard expressions for each case label item.
    for (auto &caseItem : caseStmt->getMutableCaseLabelItems()) {
      SolutionApplicationTarget caseTarget(&caseItem,
                                           context.getAsDeclContext());
      if (!rewriteTarget(caseTarget)) {
        hadError = true;
      }
    }

    bindSwitchCasePatternVars(context.getAsDeclContext(), caseStmt);

    for (auto *expected : caseStmt->getCaseBodyVariablesOrEmptyArray()) {
      assert(expected->hasName());
      auto prev = expected->getParentVarDecl();
      auto type = solution.resolveInterfaceType(
          solution.getType(prev)->mapTypeOutOfContext());
      expected->setInterfaceType(type);
    }
  }

  void visitCaseStmtBody(CaseStmt *caseStmt) {
    auto *newBody = visit(caseStmt->getBody()).get<Stmt *>();
    caseStmt->setBody(cast<BraceStmt>(newBody));
  }

  ASTNode visitCaseStmt(CaseStmt *caseStmt) {
    visitCaseStmtPreamble(caseStmt);
    visitCaseStmtBody(caseStmt);
    return caseStmt;
  }

  ASTNode visitBraceElement(ASTNode node) {
    auto &cs = solution.getConstraintSystem();
    if (auto *expr = node.dyn_cast<Expr *>()) {
      // Rewrite the expression.
      auto target = *cs.getSolutionApplicationTarget(expr);
      if (auto rewrittenTarget = rewriteTarget(target)) {
        node = rewrittenTarget->getAsExpr();

        if (target.isDiscardedExpr())
          TypeChecker::checkIgnoredExpr(castToExpr(node));
      } else {
        hadError = true;
      }
    } else if (auto stmt = node.dyn_cast<Stmt *>()) {
      node = visit(stmt);
    } else {
      visitDecl(node.get<Decl *>());
    }
    return node;
  }

  ASTNode visitBraceStmt(BraceStmt *braceStmt) {
    auto &cs = solution.getConstraintSystem();

    // Diagnose defer statement being last one in block.
    if (!braceStmt->empty()) {
      if (auto stmt = braceStmt->getLastElement().dyn_cast<Stmt *>()) {
        if (auto deferStmt = dyn_cast<DeferStmt>(stmt)) {
          auto &diags = cs.getASTContext().Diags;
          diags
              .diagnose(deferStmt->getStartLoc(), diag::defer_stmt_at_block_end)
              .fixItReplace(deferStmt->getStartLoc(), "do");
        }
      }
    }

    for (auto &node : braceStmt->getElements())
      node = visitBraceElement(node);

    // Source compatibility workaround.
    //
    // func test<T>(_: () -> T?) {
    //   ...
    // }
    //
    // A multi-statement closure passed to `test` that has an optional
    // `Void` result type inferred from the body allows:
    //   - empty `return`(s);
    //   - to skip `return nil` or `return ()` at the end.
    //
    // Implicit `return ()` has to be inserted as the last element
    // of the body if there is none. This wasn't needed before SE-0326
    // because result type was (incorrectly) inferred as `Void` due to
    // the body being skipped.
    auto closure = context.getAsAbstractClosureExpr();
    if (closure && !closure.get()->hasSingleExpressionBody() &&
        closure.get()->getBody() == braceStmt) {
      if (resultType->getOptionalObjectType() &&
          resultType->lookThroughAllOptionalTypes()->isVoid() &&
          !braceStmt->getLastElement().isStmt(StmtKind::Return)) {
        return addImplicitVoidReturn(braceStmt);
      }
    }

    return braceStmt;
  }

  ASTNode addImplicitVoidReturn(BraceStmt *braceStmt) {
    auto &cs = solution.getConstraintSystem();
    auto &ctx = cs.getASTContext();

    auto *resultExpr = getVoidExpr(ctx);
    cs.cacheExprTypes(resultExpr);

    auto *returnStmt = new (ctx) ReturnStmt(SourceLoc(), resultExpr,
                                            /*implicit=*/true);

    // For a target for newly created result and apply a solution
    // to it, to make sure that optional injection happens required
    // number of times.
    {
      SolutionApplicationTarget target(resultExpr, context.getAsDeclContext(),
                                       CTP_ReturnStmt, resultType,
                                       /*isDiscarded=*/false);
      cs.setSolutionApplicationTarget(returnStmt, target);

      visitReturnStmt(returnStmt);
    }

    // Re-create brace statement with an additional `return` at the end.

    SmallVector<ASTNode, 4> elements;
    elements.append(braceStmt->getElements().begin(),
                    braceStmt->getElements().end());
    elements.push_back(returnStmt);

    return BraceStmt::create(ctx, braceStmt->getLBraceLoc(), elements,
                             braceStmt->getRBraceLoc());
  }

  ASTNode visitReturnStmt(ReturnStmt *returnStmt) {
    auto &cs = solution.getConstraintSystem();

    if (!returnStmt->hasResult()) {
      // If contextual is not optional, there is nothing to do here.
      if (resultType->isVoid())
        return returnStmt;

      // It's possible to infer e.g. `Void?` for cases where
      // `return` doesn't have an expression. If contextual
      // type is `Void` wrapped into N optional types, let's
      // add an implicit `()` expression and let it be injected
      // into optional required number of times.

      assert(resultType->getOptionalObjectType() &&
             resultType->lookThroughAllOptionalTypes()->isVoid());

      auto target = *cs.getSolutionApplicationTarget(returnStmt);
      returnStmt->setResult(target.getAsExpr());
    }

    auto *resultExpr = returnStmt->getResult();

    enum {
      convertToResult,
      coerceToVoid,
      coerceFromNever,
    } mode;

    auto resultExprType =
        solution.simplifyType(solution.getType(resultExpr))->getRValueType();
    // A closure with a non-void return expression can coerce to a closure
    // that returns Void.
    if (resultType->isVoid() && !resultExprType->isVoid()) {
      mode = coerceToVoid;

      // A single-expression closure with a Never expression type
      // coerces to any other function type.
    } else if (context.isSingleExpressionClosure(cs) &&
               resultExprType->isUninhabited()) {
      mode = coerceFromNever;

      // Normal rule is to coerce to the return expression to the closure type.
    } else {
      mode = convertToResult;
    }

    Optional<SolutionApplicationTarget> resultTarget;
    if (auto target = cs.getSolutionApplicationTarget(returnStmt)) {
      resultTarget = *target;
    } else {
      // Single-expression closures have to handle returns in a special
      // way so the target has to be created for them during solution
      // application based on the resolved type.
      assert(context.isSingleExpressionClosure(cs));
      resultTarget = SolutionApplicationTarget(
          resultExpr, context.getAsDeclContext(),
          mode == convertToResult ? CTP_ClosureResult : CTP_Unused,
          mode == convertToResult ? resultType : Type(),
          /*isDiscarded=*/false);
    }

    if (auto newResultTarget = rewriteTarget(*resultTarget)) {
      resultExpr = newResultTarget->getAsExpr();
    }

    switch (mode) {
    case convertToResult:
      // Record the coerced expression.
      returnStmt->setResult(resultExpr);
      return returnStmt;

    case coerceToVoid: {
      // Evaluate the expression, then produce a return statement that
      // returns nothing.
      TypeChecker::checkIgnoredExpr(resultExpr);
      auto &ctx = solution.getConstraintSystem().getASTContext();
      auto newReturnStmt =
          new (ctx) ReturnStmt(
            returnStmt->getStartLoc(), nullptr, /*implicit=*/true);
      ASTNode elements[2] = { resultExpr, newReturnStmt };
      return BraceStmt::create(ctx, returnStmt->getStartLoc(),
                               elements, returnStmt->getEndLoc(),
                               /*implicit*/ true);
    }

    case coerceFromNever:
      // Replace the return statement with its expression, so that the
      // expression is evaluated directly. This only works because coercion
      // from never is limited to single-expression closures.
      return resultExpr;
    }

    return returnStmt;
  }

#define UNSUPPORTED_STMT(STMT) ASTNode visit##STMT##Stmt(STMT##Stmt *) { \
      llvm_unreachable("Unsupported statement kind " #STMT);          \
  }
  UNSUPPORTED_STMT(Yield)
  UNSUPPORTED_STMT(Fail)
#undef UNSUPPORTED_STMT

public:
  /// Apply solution to the closure and return updated body.
  ASTNode apply() {
    auto body = visit(context.getBody());

    // Since local functions can capture variables that are declared
    // after them, let's type-check them after all of the pattern
    // bindings have been resolved by applying solution to the body.
    for (auto *func : LocalFuncs)
      TypeChecker::typeCheckDecl(func);

    return body;
  }
};

class ResultBuilderRewriter : public SyntacticElementSolutionApplication {
  const AppliedBuilderTransform &Transform;

public:
  ResultBuilderRewriter(Solution &solution, AnyFunctionRef context,
                        const AppliedBuilderTransform &transform,
                        RewriteTargetFn rewriteTarget)
      : SyntacticElementSolutionApplication(
            solution, SyntacticElementContext::forFunctionRef(context),
            transform.bodyResultType, rewriteTarget),
        Transform(transform) {}

  bool apply() {
    auto body = visit(context.getBody());

    if (!body || hadError)
      return true;

    auto funcRef = context.getAsAnyFunctionRef();
    assert(funcRef);

    funcRef->setTypecheckedBody(castToStmt<BraceStmt>(body),
                                /*hasSingleExpression=*/false);

    if (auto *closure =
            getAsExpr<ClosureExpr>(funcRef->getAbstractClosureExpr()))
      solution.setExprTypes(closure);

    return false;
  }

private:
  ASTNode visitDoStmt(DoStmt *doStmt) override {
    if (auto transformed = transformDo(doStmt))
      return visit(transformed.get());

    auto newBody = visit(doStmt->getBody());
    if (!newBody)
      return nullptr;

    doStmt->setBody(castToStmt<BraceStmt>(newBody));
    return doStmt;
  }

  NullablePtr<Stmt> transformDo(DoStmt *doStmt) {
    if (!doStmt->isImplicit())
      return nullptr;

    // Implicit `do` wraps a statement and it's `type_join` expression.
    auto *body = doStmt->getBody();

    // If there are more than two elements, this `do` doesn't need to
    // get be transformed.
    if (body->getNumElements() != 2)
      return nullptr;

    auto *stmt = castToStmt(body->getFirstElement());
    auto *join = castToExpr<TypeJoinExpr>(body->getLastElement());

    switch (stmt->getKind()) {
    case StmtKind::If:
      return transformIf(castToStmt<IfStmt>(stmt), join, /*index=*/0);

    case StmtKind::Switch:
      return transformSwitch(castToStmt<SwitchStmt>(stmt), join);

    default:
      llvm_unreachable("only 'if' and 'switch' statements are transformed");
    }
  }

  NullablePtr<Stmt> transformSwitch(SwitchStmt *switchStmt,
                                    TypeJoinExpr *join) {
    unsigned caseIndex = 0;
    for (auto *caseStmt : switchStmt->getCases()) {
      auto newBody = transformBody(caseStmt->getBody(), join, caseIndex++);
      if (!newBody)
        return nullptr;

      caseStmt->setBody(newBody.get());
    }

    return switchStmt;
  }

  NullablePtr<Stmt> transformIf(IfStmt *ifStmt, TypeJoinExpr *join,
                                unsigned index) {
    // FIXME: Turn this into a condition once warning is an error.
    (void)diagnoseMissingBuildWithAvailability(ifStmt);

    auto *joinVar = join->getVar();

    // First, let's add assignment to the end of `then` branch
    {
      auto *thenBody = castToStmt<BraceStmt>(ifStmt->getThenStmt());
      auto newBody = transformBody(thenBody, join, index);
      if (!newBody)
        return nullptr;

      ifStmt->setThenStmt(newBody.get());
    }

    if (auto *elseStmt = ifStmt->getElseStmt()) {
      if (auto *innerIfStmt = getAsStmt<IfStmt>(elseStmt)) {
        auto transformedIf = transformIf(innerIfStmt, join, index + 1);
        if (!transformedIf)
          return nullptr;

        ifStmt->setElseStmt(transformedIf.get());
      } else {
        auto newBody =
            transformBody(castToStmt<BraceStmt>(elseStmt), join, index + 1);
        if (!newBody)
          return nullptr;

        ifStmt->setElseStmt(newBody.get());
      }
    } else {
      auto &ctx = getASTContext();
      SmallVector<ASTNode, 2> elseBranch;

      elseBranch.push_back(
          createAssignment(joinVar, join->getElement(index + 1)));

      ifStmt->setElseStmt(BraceStmt::create(ctx, ifStmt->getEndLoc(),
                                            elseBranch, ifStmt->getEndLoc(),
                                            /*implicit=*/true));
    }

    return ifStmt;
  }

  NullablePtr<BraceStmt> transformBody(BraceStmt *body, TypeJoinExpr *join,
                                       unsigned index) {
    for (auto &element : body->getElements()) {
      if (auto *doStmt = getAsStmt<DoStmt>(element)) {
        if (auto transformed = transformDo(doStmt))
          element = transformed.get();
      }
    }

    return addBuilderAssignment(body, join->getVar(), join->getElement(index));
  }

  // Add `$__bulderN = build{Optional, Either}(...)` at the end of a block body.
  BraceStmt *addBuilderAssignment(BraceStmt *body, DeclRefExpr *joinVar,
                                  Expr *builderCall) {
    SmallVector<ASTNode, 4> newBody;
    llvm::copy(body->getElements(), std::back_inserter(newBody));

    newBody.push_back(createAssignment(joinVar, builderCall));

    return BraceStmt::create(getASTContext(), body->getLBraceLoc(), newBody,
                             body->getRBraceLoc(), body->isImplicit());
  }

  AssignExpr *createAssignment(DeclRefExpr *destRef, Expr *source) {
    auto &ctx = getASTContext();
    auto &CS = solution.getConstraintSystem();

    auto *assignment = new (ctx) AssignExpr(destRef, /*EqualLoc=*/SourceLoc(),
                                            source, /*Implicit=*/true);

    {
      // Assignment expression is always `Void`.
      CS.setType(assignment, ctx.TheEmptyTupleType);

      CS.setSolutionApplicationTarget(
          {assignment}, {assignment, context.getAsDeclContext(), CTP_Unused,
                         /*contextualType=*/Type(), /*isDiscarded=*/false});
    }

    return assignment;
  }

  ASTContext &getASTContext() const {
    return context.getAsDeclContext()->getASTContext();
  }

private:
  /// Look for a #available condition. If there is one, we need to check
  /// that the resulting type of the "then" doesn't refer to any types that
  /// are unavailable in the enclosing context.
  ///
  /// Note that this is for staging in support for buildLimitedAvailability();
  /// the diagnostic is currently a warning, so that existing code that
  /// compiles today will continue to compile. Once result builder types
  /// have had the chance to adopt buildLimitedAvailability(), we'll upgrade
  /// this warning to an error.
  LLVM_NODISCARD
  bool diagnoseMissingBuildWithAvailability(IfStmt *ifStmt) {
    auto findAvailabilityCondition =
        [](StmtCondition stmtCond) -> const StmtConditionElement * {
      for (const auto &cond : stmtCond) {
        switch (cond.getKind()) {
        case StmtConditionElement::CK_Boolean:
        case StmtConditionElement::CK_PatternBinding:
        case StmtConditionElement::CK_HasSymbol:
          continue;

        case StmtConditionElement::CK_Availability:
          return &cond;
          break;
        }
      }

      return nullptr;
    };

    auto availabilityCond = findAvailabilityCondition(ifStmt->getCond());
    if (!availabilityCond)
      return false;

    SourceLoc loc = availabilityCond->getStartLoc();
    Type bodyType;
    if (availabilityCond->getAvailability()->isUnavailability()) {
      BraceStmt *elseBody = nullptr;
      // For #unavailable, we need to check the "else".
      if (auto *innerIf = getAsStmt<IfStmt>(ifStmt->getElseStmt())) {
        elseBody = castToStmt<BraceStmt>(innerIf->getThenStmt());
      } else {
        elseBody = castToStmt<BraceStmt>(ifStmt->getElseStmt());
      }

      Type elseBodyType =
        solution.simplifyType(solution.getType(elseBody->getLastElement()));
      bodyType = elseBodyType;
    } else {
      auto *thenBody = castToStmt<BraceStmt>(ifStmt->getThenStmt());
      Type thenBodyType =
          solution.simplifyType(solution.getType(thenBody->getLastElement()));
      bodyType = thenBodyType;
    }

    auto builderType = solution.simplifyType(Transform.builderType);

    return bodyType.findIf([&](Type type) {
      auto nominal = type->getAnyNominal();
      if (!nominal)
        return false;

      ExportContext where =
          ExportContext::forFunctionBody(context.getAsDeclContext(), loc);
      if (auto reason =
              TypeChecker::checkDeclarationAvailability(nominal, where)) {
        auto &ctx = getASTContext();
        ctx.Diags.diagnose(loc,
                           diag::result_builder_missing_limited_availability,
                           builderType);

        // Add a note to the result builder with a stub for
        // buildLimitedAvailability().
        if (auto builder = builderType->getAnyNominal()) {
          SourceLoc buildInsertionLoc;
          std::string stubIndent;
          Type componentType;
          std::tie(buildInsertionLoc, stubIndent, componentType) =
              determineResultBuilderBuildFixItInfo(builder);
          if (buildInsertionLoc.isValid()) {
            std::string fixItString;
            {
              llvm::raw_string_ostream out(fixItString);
              printResultBuilderBuildFunction(
                  builder, componentType,
                  ResultBuilderBuildFunction::BuildLimitedAvailability,
                  stubIndent, out);

              builder
                  ->diagnose(
                      diag::result_builder_missing_build_limited_availability,
                      builderType)
                  .fixItInsert(buildInsertionLoc, fixItString);
            }
          }
        }

        return true;
      }

      return false;
    });
  }
};
} // namespace

SolutionApplicationToFunctionResult ConstraintSystem::applySolution(
    Solution &solution, AnyFunctionRef fn,
    DeclContext *&currentDC,
    RewriteTargetFn rewriteTarget) {
  auto &cs = solution.getConstraintSystem();
  auto *closure = getAsExpr<ClosureExpr>(fn.getAbstractClosureExpr());
  FunctionType *closureFnType = nullptr;
  if (closure) {
    // Update the closure's type.
    auto closureType = solution.simplifyType(cs.getType(closure));
    cs.setType(closure, closureType);

    // Coerce the parameter types.
    closureFnType = closureType->castTo<FunctionType>();
    auto *params = closure->getParameters();
    TypeChecker::coerceParameterListToType(params, closureFnType);

    // Find any isolated parameters in this closure and mark them as isolated.
    for (auto param : solution.isolatedParams) {
      if (param->getDeclContext() == closure)
        param->setIsolated(true);
    }

    if (llvm::is_contained(solution.preconcurrencyClosures, closure))
      closure->setIsolatedByPreconcurrency();

    // Coerce the result type, if it was written explicitly.
    if (closure->hasExplicitResultType()) {
      closure->setExplicitResultType(closureFnType->getResult());
    }
  }

  // Enter the context of the function before performing any additional
  // transformations.
  llvm::SaveAndRestore<DeclContext *> savedDC(currentDC, fn.getAsDeclContext());

  // Apply the result builder transform, if there is one.
  if (auto transform = solution.getAppliedBuilderTransform(fn)) {
    NullablePtr<BraceStmt> newBody;

    if (auto transformedBody = transform->transformedBody) {
      fn.setParsedBody(const_cast<BraceStmt *>(transformedBody.get()),
                       /*singleExpression=*/false);

      ResultBuilderRewriter rewriter(solution, fn, *transform, rewriteTarget);

      return rewriter.apply() ? SolutionApplicationToFunctionResult::Failure
                              : SolutionApplicationToFunctionResult::Success;
    }

    // Apply the result builder to the closure. We want to be in the
    // context of the closure for subsequent transforms.
    newBody = applyResultBuilderTransform(
        solution, *transform, fn.getBody(), fn.getAsDeclContext(),
        [&](SolutionApplicationTarget target) {
          auto resultTarget = rewriteTarget(target);
          if (resultTarget) {
            if (auto expr = resultTarget->getAsExpr())
              solution.setExprTypes(expr);
          }

          return resultTarget;
        });

    if (!newBody)
      return SolutionApplicationToFunctionResult::Failure;

    fn.setTypecheckedBody(newBody.get(), /*isSingleExpression=*/false);
    if (closure) {
      solution.setExprTypes(closure);
    }

    return SolutionApplicationToFunctionResult::Success;

  }
  assert(closure && "Can only get here with a closure at the moment");

  // If this closure is checked as part of the enclosing expression, handle
  // that now.
  //
  // Multi-statement closures are handled separately because they need to
  // wait until all of the `ExtInfo` flags are propagated from the context
  // e.g. parameter could be no-escape if closure is applied to a call.
  if (closure->hasSingleExpressionBody()) {
    bool hadError =
        applySolutionToBody(solution, closure, currentDC, rewriteTarget);
    return hadError ? SolutionApplicationToFunctionResult::Failure
                    : SolutionApplicationToFunctionResult::Success;
  }

  // Otherwise, we need to delay type checking of the closure until later.
  solution.setExprTypes(closure);
  closure->setBodyState(ClosureExpr::BodyState::ReadyForTypeChecking);
  return SolutionApplicationToFunctionResult::Delay;
}

bool ConstraintSystem::applySolutionToBody(Solution &solution,
                                           AnyFunctionRef fn,
                                           DeclContext *&currentDC,
                                           RewriteTargetFn rewriteTarget) {
  // Enter the context of the function before performing any additional
  // transformations.
  llvm::SaveAndRestore<DeclContext *> savedDC(currentDC, fn.getAsDeclContext());

  Type resultTy;

  if (auto transform = solution.getAppliedBuilderTransform(fn)) {
    resultTy = solution.simplifyType(transform->bodyResultType);
  } else if (auto *closure =
                 getAsExpr<ClosureExpr>(fn.getAbstractClosureExpr())) {
    resultTy =
        solution.getResolvedType(closure)->castTo<FunctionType>()->getResult();
  } else {
    resultTy = fn.getBodyResultType();
  }

  SyntacticElementSolutionApplication application(
      solution, SyntacticElementContext::forFunctionRef(fn), resultTy,
      rewriteTarget);

  auto body = application.apply();

  if (!body || application.hadError)
    return true;

  fn.setTypecheckedBody(castToStmt<BraceStmt>(body),
                        fn.hasSingleExpressionBody());
  return false;
}

void ConjunctionElement::findReferencedVariables(
    ConstraintSystem &cs, SmallPtrSetImpl<TypeVariableType *> &typeVars) const {
  auto referencedVars = Element->getTypeVariables();
  typeVars.insert(referencedVars.begin(), referencedVars.end());

  if (Element->getKind() != ConstraintKind::SyntacticElement)
    return;

  ASTNode element = Element->getSyntacticElement();
  auto *locator = Element->getLocator();

  TypeVariableRefFinder refFinder(cs, locator->getAnchor(), typeVars);

  // If this is a pattern of `for-in` statement, let's walk into `for-in`
  // sequence expression because both elements are type-checked together.
  //
  // Correct expressions wouldn't have any type variables in sequence but
  // they could appear due to circular references or other incorrect syntax.
  if (element.is<Pattern *>()) {
    if (auto parent =
            locator->getLastElementAs<LocatorPathElt::SyntacticElement>()) {
      if (auto *forEach = getAsStmt<ForEachStmt>(parent->getElement())) {
        if (auto *sequence = forEach->getParsedSequence())
          sequence->walk(refFinder);
        return;
      }
    }
  }

  if (auto *patternBinding =
          dyn_cast_or_null<PatternBindingDecl>(element.dyn_cast<Decl *>())) {
    // Let's not walk into placeholder variable initializers, since they
    // are type-checked separately right now.
    if (isPlaceholderVar(patternBinding))
      return;

    if (auto patternBindingElt =
            locator
                ->getLastElementAs<LocatorPathElt::PatternBindingElement>()) {
      if (auto *init = patternBinding->getInit(patternBindingElt->getIndex()))
        init->walk(refFinder);
      return;
    }
  }

  if (element.is<Decl *>() || element.is<StmtConditionElement *>() ||
      element.is<Expr *>() || element.isStmt(StmtKind::Return))
    element.walk(refFinder);
}

Type constraints::isPlaceholderVar(PatternBindingDecl *PB) {
  auto *var = PB->getSingleVar();
  if (!var)
    return Type();

  if (!var->getName().hasDollarPrefix())
    return Type();

  auto *pattern = PB->getPattern(0);
  if (auto *typedPattern = dyn_cast<TypedPattern>(pattern)) {
    auto type = typedPattern->getType();
    if (type && type->hasPlaceholder())
      return type;
  }

  return Type();
}
