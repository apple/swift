//===--- TypeCheckCodeCompletion.cpp - Type Checking for Code Completion --===//
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
// This file implements various entry points for use by lib/IDE/.
//
//===----------------------------------------------------------------------===//

#include "CodeSynthesis.h"
#include "MiscDiagnostics.h"
#include "TypeCheckObjC.h"
#include "TypeCheckType.h"
#include "TypeChecker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Attr.h"
#include "swift/AST/DiagnosticSuppression.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/Statistic.h"
#include "swift/Parse/IDEInspectionCallbacks.h"
#include "swift/Parse/Lexer.h"
#include "swift/Sema/CompletionContextFinder.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/Twine.h"
#include <algorithm>

using namespace swift;
using namespace constraints;

static Type
getTypeOfExpressionWithoutApplying(Expr *&expr, DeclContext *dc,
                                   ConcreteDeclRef &referencedDecl) {
  if (isa<AbstractClosureExpr>(dc)) {
    // If the expression is embedded in a closure, the constraint system tries
    // to retrieve that closure's type, which will fail since we won't have
    // generated any type variables for it. Thus, fallback type checking isn't
    // available in this case.
    return Type();
  }
  auto &Context = dc->getASTContext();

  FrontendStatsTracer StatsTracer(Context.Stats,
                                  "typecheck-expr-no-apply", expr);
  PrettyStackTraceExpr stackTrace(Context, "type-checking", expr);
  referencedDecl = nullptr;

  ConstraintSystemOptions options;
  options |= ConstraintSystemFlags::SuppressDiagnostics;

  // Construct a constraint system from this expression.
  ConstraintSystem cs(dc, options);

  // Attempt to solve the constraint system.
  const Type originalType = expr->getType();
  const bool needClearType = originalType && originalType->hasError();
  const auto recoverOriginalType = [&] () {
    if (needClearType)
      expr->setType(originalType);
  };

  // If the previous checking gives the expr error type, clear the result and
  // re-check.
  if (needClearType)
    expr->setType(Type());
  SyntacticElementTarget target(expr, dc, CTP_Unused, Type(),
                                /*isDiscarded=*/false);

  SmallVector<Solution, 2> viable;
  cs.solveForCodeCompletion(target, viable);

  if (viable.empty()) {
    recoverOriginalType();
    return Type();
  }

  // Get the expression's simplified type.
  expr = target.getAsExpr();
  auto &solution = viable.front();
  auto &solutionCS = solution.getConstraintSystem();
  Type exprType = solution.simplifyType(solutionCS.getType(expr));

  assert(exprType && !exprType->hasTypeVariable() &&
         "free type variable with FreeTypeVariableBinding::GenericParameters?");
  assert(exprType && !exprType->hasPlaceholder() &&
         "type placeholder with FreeTypeVariableBinding::GenericParameters?");

  if (exprType->hasError()) {
    recoverOriginalType();
    return Type();
  }

  // Dig the declaration out of the solution.
  auto semanticExpr = expr->getSemanticsProvidingExpr();
  auto topLocator = cs.getConstraintLocator(semanticExpr);
  referencedDecl = solution.resolveLocatorToDecl(topLocator);

  if (!referencedDecl.getDecl()) {
    // Do another check in case we have a curried call from binding a function
    // reference to a variable, for example:
    //
    //   class C {
    //     func instanceFunc(p1: Int, p2: Int) {}
    //   }
    //   func t(c: C) {
    //     C.instanceFunc(c)#^COMPLETE^#
    //   }
    //
    // We need to get the referenced function so we can complete the argument
    // labels. (Note that the requirement to have labels in the curried call
    // seems inconsistent with the removal of labels from function types.
    // If this changes the following code could be removed).
    if (auto *CE = dyn_cast<CallExpr>(semanticExpr)) {
      if (auto *UDE = dyn_cast<UnresolvedDotExpr>(CE->getFn())) {
        if (isa<TypeExpr>(UDE->getBase())) {
          auto udeLocator = cs.getConstraintLocator(UDE);
          auto udeRefDecl = solution.resolveLocatorToDecl(udeLocator);
          if (auto *FD = dyn_cast_or_null<FuncDecl>(udeRefDecl.getDecl())) {
            if (FD->isInstanceMember())
              referencedDecl = udeRefDecl;
          }
        }
      }
    }
  }

  // Recover the original type if needed.
  recoverOriginalType();
  return exprType;
}

static bool hasTypeForCompletion(Solution &solution,
                                 CompletionContextFinder &contextAnalyzer) {
  if (contextAnalyzer.hasCompletionExpr()) {
    return solution.hasType(contextAnalyzer.getCompletionExpr());
  } else {
    return solution.hasType(
        contextAnalyzer.getKeyPathContainingCompletionComponent(),
        contextAnalyzer.getKeyPathCompletionComponentIndex());
  }
}

void TypeChecker::filterSolutionsForCodeCompletion(
    SmallVectorImpl<Solution> &solutions,
    CompletionContextFinder &contextAnalyzer) {
  // Ignore solutions that didn't end up involving the completion (e.g. due to
  // a fix to skip over/ignore it).
  llvm::erase_if(solutions, [&](Solution &S) {
    if (hasTypeForCompletion(S, contextAnalyzer))
      return false;
    // FIXME: Technically this should never happen, but it currently does in
    // result builder contexts. Re-evaluate if we can assert here when we have
    // multi-statement closure checking for result builders.
    return true;
  });

  if (solutions.size() <= 1)
    return;

  Score minScore = std::min_element(solutions.begin(), solutions.end(),
                                    [](const Solution &a, const Solution &b) {
    return a.getFixedScore() < b.getFixedScore();
  })->getFixedScore();

  llvm::erase_if(solutions, [&](const Solution &S) {
    return S.getFixedScore().Data[SK_Fix] > minScore.Data[SK_Fix];
  });
}

bool TypeChecker::typeCheckForCodeCompletion(
    SyntacticElementTarget &target, bool needsPrecheck,
    llvm::function_ref<void(const Solution &)> callback) {
  auto *DC = target.getDeclContext();
  auto &Context = DC->getASTContext();
  // First of all, let's check whether given target expression
  // does indeed have the code completion location in it.
  {
    auto range = target.getSourceRange();
    if (range.isInvalid() ||
        !containsIDEInspectionTarget(range, Context.SourceMgr))
      return false;
  }

  CompletionContextFinder contextAnalyzer(target, DC);

  // If there was no completion expr (e.g. if the code completion location was
  // among tokens that were skipped over during parser error recovery) bail.
  if (!contextAnalyzer.hasCompletion())
    return false;

  if (needsPrecheck) {
    // First, pre-check the expression, validating any types that occur in the
    // expression and folding sequence expressions.
    auto failedPreCheck =
        ConstraintSystem::preCheckTarget(target,
                                         /*replaceInvalidRefsWithErrors=*/true);

    if (failedPreCheck)
      return false;
  }

  enum class CompletionResult { Ok, NotApplicable, Fallback };

  auto solveForCodeCompletion =
      [&](SyntacticElementTarget &target) -> CompletionResult {
    ConstraintSystemOptions options;
    options |= ConstraintSystemFlags::AllowFixes;
    options |= ConstraintSystemFlags::SuppressDiagnostics;
    options |= ConstraintSystemFlags::ForCodeCompletion;
    
    ConstraintSystem cs(DC, options);

    llvm::SmallVector<Solution, 4> solutions;

    // If solve failed to generate constraints or with some other
    // issue, we need to fallback to type-checking a sub-expression.
    cs.setTargetFor(target.getAsExpr(), target);
    if (!cs.solveForCodeCompletion(target, solutions))
      return CompletionResult::Fallback;

    // Similarly, if the type-check didn't produce any solutions, fall back
    // to type-checking a sub-expression in isolation.
    if (solutions.empty())
      return CompletionResult::Fallback;

    // FIXME: instead of filtering, expose the score and viability to clients.
    // Remove solutions that skipped over/ignored the code completion point
    // or that require fixes and have a score that is worse than the best.
    filterSolutionsForCodeCompletion(solutions, contextAnalyzer);

    llvm::for_each(solutions, callback);
    return CompletionResult::Ok;
  };

  switch (solveForCodeCompletion(target)) {
  case CompletionResult::Ok:
    return true;

  case CompletionResult::NotApplicable:
    return false;

  case CompletionResult::Fallback:
    break;
  }

  // Determine the best subexpression to use based on the collected context
  // of the code completion expression.
  auto fallback = contextAnalyzer.getFallbackCompletionExpr();
  if (!fallback) {
    return true;
  }
  if (isa<AbstractClosureExpr>(fallback->DC)) {
    // If the expression is embedded in a closure, the constraint system tries
    // to retrieve that closure's type, which will fail since we won't have
    // generated any type variables for it. Thus, fallback type checking isn't
    // available in this case.
    return true;
  }
  if (auto *expr = target.getAsExpr()) {
    assert(fallback->E != expr);
    (void)expr;
  }
  SyntacticElementTarget completionTarget(fallback->E, fallback->DC,
                                          CTP_Unused,
                                          /*contextualType=*/Type(),
                                          /*isDiscarded=*/true);
  typeCheckForCodeCompletion(completionTarget, fallback->SeparatePrecheck,
                             callback);
  return true;
}

static std::optional<Type>
getTypeOfCompletionContextExpr(DeclContext *DC, CompletionTypeCheckKind kind,
                               Expr *&parsedExpr,
                               ConcreteDeclRef &referencedDecl) {
  if (constraints::ConstraintSystem::preCheckExpression(
          parsedExpr, DC,
          /*replaceInvalidRefsWithErrors=*/true))
    return std::nullopt;

  switch (kind) {
  case CompletionTypeCheckKind::Normal:
    // Handle below.
    break;

  case CompletionTypeCheckKind::KeyPath:
    referencedDecl = nullptr;
    if (auto keyPath = dyn_cast<KeyPathExpr>(parsedExpr)) {
      auto components = keyPath->getComponents();
      if (!components.empty()) {
        auto &last = components.back();
        if (last.isResolved()) {
          if (last.getKind() == KeyPathExpr::Component::Kind::Property)
            referencedDecl = last.getDeclRef();
          Type lookupTy = last.getComponentType();
          ASTContext &Ctx = DC->getASTContext();
          if (auto bridgedClass = Ctx.getBridgedToObjC(DC, lookupTy))
            return bridgedClass;
          return lookupTy;
        }
      }
    }

    return std::nullopt;
  }

  Type originalType = parsedExpr->getType();
  if (auto T =
          getTypeOfExpressionWithoutApplying(parsedExpr, DC, referencedDecl))
    return T;

  // Try to recover if we've made any progress.
  if (parsedExpr &&
      !isa<ErrorExpr>(parsedExpr) &&
      parsedExpr->getType() &&
      !parsedExpr->getType()->hasError() &&
      (originalType.isNull() ||
       !parsedExpr->getType()->isEqual(originalType))) {
    return parsedExpr->getType();
  }

  return std::nullopt;
}

/// Return the type of an expression parsed during code completion, or
/// a null \c Type on error.
std::optional<Type> swift::getTypeOfCompletionContextExpr(
    ASTContext &Ctx, DeclContext *DC, CompletionTypeCheckKind kind,
    Expr *&parsedExpr, ConcreteDeclRef &referencedDecl) {
  DiagnosticSuppression suppression(Ctx.Diags);

  // Try to solve for the actual type of the expression.
  return ::getTypeOfCompletionContextExpr(DC, kind, parsedExpr,
                                          referencedDecl);
}

LookupResult
swift::lookupSemanticMember(DeclContext *DC, Type ty, DeclName name) {
  return TypeChecker::lookupMember(DC, ty, DeclNameRef(name), SourceLoc(),
                                   std::nullopt);
}

