//===--- BuilderTransform.cpp - Result-builder transformation -----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements routines associated with the result-builder
// transformation.
//
//===----------------------------------------------------------------------===//

#include "MiscDiagnostics.h"
#include "TypeCheckAvailability.h"
#include "TypeChecker.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Sema/SolutionResult.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include <iterator>
#include <map>
#include <memory>
#include <tuple>
#include <utility>

using namespace swift;
using namespace constraints;

namespace {

/// Find the first #available condition within the statement condition,
/// or return NULL if there isn't one.
const StmtConditionElement *findAvailabilityCondition(StmtCondition stmtCond) {
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
}

class BuilderTransformerBase {
protected:
  ASTContext &ctx;
  DeclContext *dc;
  ResultBuilder builder;

public:
  BuilderTransformerBase(ConstraintSystem *cs, DeclContext *dc,
                         Type builderType)
      : ctx(dc->getASTContext()), dc(dc), builder(cs, dc, builderType) {}

  virtual ~BuilderTransformerBase() {}

protected:
  virtual Expr *buildCallIfWanted(SourceLoc loc, Identifier fnName,
                                  ArrayRef<Expr *> argExprs,
                                  ArrayRef<Identifier> argLabels) = 0;

  static bool isBuildableIfChainRecursive(IfStmt *ifStmt, unsigned &numPayloads,
                                          bool &isOptional) {
    // The 'then' clause contributes a payload.
    ++numPayloads;

    // If there's an 'else' clause, it contributes payloads:
    if (auto elseStmt = ifStmt->getElseStmt()) {
      // If it's 'else if', it contributes payloads recursively.
      if (auto elseIfStmt = dyn_cast<IfStmt>(elseStmt)) {
        return isBuildableIfChainRecursive(elseIfStmt, numPayloads, isOptional);
        // Otherwise it's just the one.
      } else {
        ++numPayloads;
      }

      // If not, the chain result is at least optional.
    } else {
      isOptional = true;
    }

    return true;
  }

  static bool hasUnconditionalElse(IfStmt *ifStmt) {
    if (auto *elseStmt = ifStmt->getElseStmt()) {
      if (auto *ifStmt = dyn_cast<IfStmt>(elseStmt))
        return hasUnconditionalElse(ifStmt);
      return true;
    }

    return false;
  }

  bool isBuildableIfChain(IfStmt *ifStmt, unsigned &numPayloads,
                          bool &isOptional) {
    if (!isBuildableIfChainRecursive(ifStmt, numPayloads, isOptional))
      return false;

    // If there's a missing 'else', we need 'buildOptional' to exist.
    if (isOptional && !builder.supportsOptional())
      return false;

    // If there are multiple clauses, we need 'buildEither(first:)' and
    // 'buildEither(second:)' to both exist.
    if (numPayloads > 1) {
      if (!builder.supports(ctx.Id_buildEither, {ctx.Id_first}) ||
          !builder.supports(ctx.Id_buildEither, {ctx.Id_second}))
        return false;
    }

    return true;
  }

  /// Wrap a payload value in an expression which will produce a chain
  /// result (without `buildIf`).
  Expr *buildWrappedChainPayload(Expr *operand, unsigned payloadIndex,
                                 unsigned numPayloads, bool isOptional) {
    assert(payloadIndex < numPayloads);

    // Inject into the appropriate chain position.
    //
    // We produce a (left-biased) balanced binary tree of Eithers in order
    // to prevent requiring a linear number of injections in the worst case.
    // That is, if we have 13 clauses, we want to produce:
    //
    //                      /------------------Either------------\
    //           /-------Either-------\                     /--Either--\
    //     /--Either--\          /--Either--\          /--Either--\     \
    //   /-E-\      /-E-\      /-E-\      /-E-\      /-E-\      /-E-\    \
    // 0000 0001  0010 0011  0100 0101  0110 0111  1000 1001  1010 1011 1100
    //
    // Note that a prefix of length D of the payload index acts as a path
    // through the tree to the node at depth D.  On the rightmost path
    // through the tree (when this prefix is equal to the corresponding
    // prefix of the maximum payload index), the bits of the index mark
    // where Eithers are required.
    //
    // Since we naturally want to build from the innermost Either out, and
    // therefore work with progressively shorter prefixes, we can do it all
    // with right-shifts.
    for (auto path = payloadIndex, maxPath = numPayloads - 1; maxPath != 0;
         path >>= 1, maxPath >>= 1) {
      // Skip making Eithers on the rightmost path where they aren't required.
      // This isn't just an optimization: adding spurious Eithers could
      // leave us with unresolvable type variables if `buildEither` has
      // a signature like:
      //    static func buildEither<T,U>(first value: T) -> Either<T,U>
      // which relies on unification to work.
      if (path == maxPath && !(maxPath & 1))
        continue;

      bool isSecond = (path & 1);
      operand =
          buildCallIfWanted(operand->getStartLoc(), ctx.Id_buildEither, operand,
                            {isSecond ? ctx.Id_second : ctx.Id_first});
    }

    // Inject into Optional if required.  We'll be adding the call to
    // `buildIf` after all the recursive calls are complete.
    if (isOptional) {
      operand = buildSomeExpr(operand);
    }

    return operand;
  }

  Expr *buildSomeExpr(Expr *arg) {
    auto optionalDecl = ctx.getOptionalDecl();
    auto optionalType = optionalDecl->getDeclaredType();

    auto loc = arg->getStartLoc();
    auto optionalTypeExpr =
        TypeExpr::createImplicitHack(loc, optionalType, ctx);
    auto someRef = new (ctx) UnresolvedDotExpr(
        optionalTypeExpr, loc, DeclNameRef(ctx.getIdentifier("some")),
        DeclNameLoc(loc), /*implicit=*/true);
    auto *argList = ArgumentList::forImplicitUnlabeled(ctx, {arg});
    return CallExpr::createImplicit(ctx, someRef, argList);
  }

  Expr *buildNoneExpr(SourceLoc endLoc) {
    auto optionalDecl = ctx.getOptionalDecl();
    auto optionalType = optionalDecl->getDeclaredType();

    auto optionalTypeExpr =
        TypeExpr::createImplicitHack(endLoc, optionalType, ctx);
    return new (ctx) UnresolvedDotExpr(optionalTypeExpr, endLoc,
                                       DeclNameRef(ctx.getIdentifier("none")),
                                       DeclNameLoc(endLoc), /*implicit=*/true);
  }
};

/// Visitor to classify the contents of the given closure.
class BuilderClosureVisitor
    : private BuilderTransformerBase,
      private StmtVisitor<BuilderClosureVisitor, VarDecl *> {

  friend StmtVisitor<BuilderClosureVisitor, VarDecl *>;

  ConstraintSystem *cs;

  SkipUnhandledConstructInResultBuilder::UnhandledNode unhandledNode;

  /// Whether an error occurred during application of the builder closure,
  /// e.g., during constraint generation.
  bool hadError = false;

  /// The record of what happened when we applied the builder transform.
  AppliedBuilderTransform applied;

  /// Produce a builder call to the given named function with the given
  /// arguments.
  Expr *buildCallIfWanted(SourceLoc loc, Identifier fnName,
                          ArrayRef<Expr *> argExprs,
                          ArrayRef<Identifier> argLabels) override {
    if (!cs)
      return nullptr;

    return builder.buildCall(loc, fnName, argExprs, argLabels);
  }

  /// Capture the given expression into an implicitly-generated variable.
  VarDecl *captureExpr(Expr *expr, bool oneWay,
                       llvm::PointerUnion<Stmt *, Expr *> forEntity = nullptr) {
    if (!cs)
      return nullptr;

    Expr *origExpr = expr;

    if (oneWay) {
      // Form a one-way constraint to prevent backward propagation.
      expr = new (ctx) OneWayExpr(expr);
    }

    // Generate constraints for this expression.
    expr = cs->generateConstraints(expr, dc);
    if (!expr) {
      hadError = true;
      return nullptr;
    }

    // Create the implicit variable.
    auto var = builder.buildVar(expr->getStartLoc());

    // Record the new variable and its corresponding expression & statement.
    if (auto forStmt = forEntity.dyn_cast<Stmt *>()) {
      applied.capturedStmts.insert({forStmt, { var, { expr } }});
    } else {
      if (auto forExpr = forEntity.dyn_cast<Expr *>())
        origExpr = forExpr;

      applied.capturedExprs.insert({origExpr, {var, expr}});
    }

    cs->setType(var, cs->getType(expr));
    return var;
  }

public:
  BuilderClosureVisitor(ASTContext &ctx, ConstraintSystem *cs, DeclContext *dc,
                        Type builderType, Type bodyResultType)
      : BuilderTransformerBase(cs, dc, builderType), cs(cs) {
    applied.builderType = builder.getType();
    applied.bodyResultType = bodyResultType;
  }

  /// Apply the builder transform to the given statement.
  Optional<AppliedBuilderTransform> apply(Stmt *stmt) {
    VarDecl *bodyVar = visit(stmt);
    if (!bodyVar)
      return None;

    applied.returnExpr = builder.buildVarRef(bodyVar, stmt->getEndLoc());

    // If there is a buildFinalResult(_:), call it.
    ASTContext &ctx = cs->getASTContext();
    if (builder.supports(ctx.Id_buildFinalResult, {Identifier()})) {
      applied.returnExpr = buildCallIfWanted(
          applied.returnExpr->getLoc(), ctx.Id_buildFinalResult,
          { applied.returnExpr }, { Identifier() });
    }

    applied.returnExpr = cs->buildTypeErasedExpr(
        applied.returnExpr, dc, applied.bodyResultType, CTP_ReturnStmt);

    applied.returnExpr = cs->generateConstraints(applied.returnExpr, dc);
    if (!applied.returnExpr) {
      hadError = true;
      return None;
    }

    return std::move(applied);
  }

  /// Check whether the result builder can be applied to this statement.
  /// \returns the node that cannot be handled by this builder on failure.
  SkipUnhandledConstructInResultBuilder::UnhandledNode check(Stmt *stmt) {
    (void)visit(stmt);
    return unhandledNode;
  }

protected:
#define CONTROL_FLOW_STMT(StmtClass)                       \
  VarDecl *visit##StmtClass##Stmt(StmtClass##Stmt *stmt) { \
    if (!unhandledNode)                                    \
      unhandledNode = stmt;                                \
                                                           \
    return nullptr;                                        \
  }

  void visitPatternBindingDecl(PatternBindingDecl *patternBinding) {
    // Enforce some restrictions on local variables inside a result builder.
    for (unsigned i : range(patternBinding->getNumPatternEntries())) {
      // The pattern binding must have an initial value expression.
      if (!patternBinding->isExplicitlyInitialized(i)) {
        if (!unhandledNode)
          unhandledNode = patternBinding;
        return;
      }

      // Each variable bound by the pattern must be stored, and cannot
      // have observers.
      SmallVector<VarDecl *, 8> variables;
      patternBinding->getPattern(i)->collectVariables(variables);

      for (auto *var : variables) {
        if (!var->getImplInfo().isSimpleStored()) {
          if (!unhandledNode)
            unhandledNode = patternBinding;
          return;
        }

        // Also check for invalid attributes.
        TypeChecker::checkDeclAttributes(var);
      }
    }

    // If there is a constraint system, generate constraints for the pattern
    // binding.
    if (cs) {
      SolutionApplicationTarget target(patternBinding);
      if (cs->generateConstraints(target))
        hadError = true;
    }
  }

  void visitBraceElement(ASTNode node, SmallVectorImpl<Expr *> &expressions) {
    auto addChild = [&](VarDecl *childVar) {
      if (!childVar)
        return;

      expressions.push_back(builder.buildVarRef(childVar, childVar->getLoc()));
    };

    // Implicit returns in single-expression function bodies are treated
    // as the expression.
    if (auto returnStmt =
            dyn_cast_or_null<ReturnStmt>(node.dyn_cast<Stmt *>())) {
      assert(returnStmt->isImplicit());
      node = returnStmt->getResult();
    }

    if (auto stmt = node.dyn_cast<Stmt *>()) {
      addChild(visit(stmt));
      return;
    }

    if (auto decl = node.dyn_cast<Decl *>()) {
      // Just ignore #if; the chosen children should appear in the
      // surrounding context.  This isn't good for source tools but it
      // at least works.
      if (isa<IfConfigDecl>(decl))
        return;

      // Skip #warning/#error; we'll handle them when applying the builder.
      if (isa<PoundDiagnosticDecl>(decl))
        return;

      // Pattern bindings are okay so long as all of the entries are
      // initialized.
      if (auto patternBinding = dyn_cast<PatternBindingDecl>(decl)) {
        visitPatternBindingDecl(patternBinding);
        return;
      }

      // Ignore variable declarations, because they're always handled within
      // their enclosing pattern bindings.
      if (isa<VarDecl>(decl))
        return;

      if (!unhandledNode)
        unhandledNode = decl;

      return;
    }

    auto expr = node.get<Expr *>();
    if (auto *SVE = dyn_cast<SingleValueStmtExpr>(expr)) {
      // This should never be treated as an expression in a result builder, it
      // should have statement semantics.
      visitBraceElement(SVE->getStmt(), expressions);
      return;
    }
    if (cs && builder.supports(ctx.Id_buildExpression)) {
      expr = buildCallIfWanted(expr->getLoc(), ctx.Id_buildExpression,
                               {expr}, {Identifier()});
    }

    addChild(captureExpr(expr, /*oneWay=*/true, node.get<Expr *>()));
  }

  VarDecl *visitBraceStmt(BraceStmt *braceStmt) {
    SmallVector<Expr *, 4> expressions;
    for (auto node : braceStmt->getElements())
      visitBraceElement(node, expressions);

    if (!cs || hadError)
      return nullptr;

    Expr *call = nullptr;
    // If the builder supports `buildPartialBlock(first:)` and
    // `buildPartialBlock(accumulated:next:)`, use this to combine
    // subexpressions pairwise.
    if (!expressions.empty() && builder.canUseBuildPartialBlock()) {
      // NOTE: The current implementation uses one-way constraints in between
      // subexpressions. It's functionally equivalent to the following:
      //   let v0 = Builder.buildPartialBlock(first: arg_0)
      //   let v1 = Builder.buildPartialBlock(accumulated: arg_1, next: v0)
      //   ...
      //   return Builder.buildPartialBlock(accumulated: arg_n, next: ...)
      call = buildCallIfWanted(braceStmt->getStartLoc(),
                               ctx.Id_buildPartialBlock,
                               {expressions.front()},
                               /*argLabels=*/{ctx.Id_first});
      for (auto *expr : llvm::drop_begin(expressions)) {
        call = buildCallIfWanted(braceStmt->getStartLoc(),
                                 ctx.Id_buildPartialBlock,
                                 {new (ctx) OneWayExpr(call), expr},
                                 {ctx.Id_accumulated, ctx.Id_next});
      }
    }
    // If `buildBlock` does not exist at this point, it could be the case that
    // `buildPartialBlock` did not have the sufficient availability for this
    // call site.  Diagnose it.
    else if (!builder.supports(ctx.Id_buildBlock)) {
      ctx.Diags.diagnose(
          braceStmt->getStartLoc(),
          diag::result_builder_missing_available_buildpartialblock,
          builder.getType());
      return nullptr;
    }
    // Otherwise, call `buildBlock` on all subexpressions.
    else {
      // Call Builder.buildBlock(... args ...)
      call = buildCallIfWanted(braceStmt->getStartLoc(),
                               ctx.Id_buildBlock, expressions,
                               /*argLabels=*/{ });
    }

    if (!call)
      return nullptr;

    return captureExpr(call, /*oneWay=*/true, braceStmt);
  }

  VarDecl *visitDoStmt(DoStmt *doStmt) {
    auto childVar = visitBraceStmt(doStmt->getBody());
    if (!childVar)
      return nullptr;

    auto childRef = builder.buildVarRef(childVar, doStmt->getEndLoc());

    return captureExpr(childRef, /*oneWay=*/true, doStmt);
  }

  CONTROL_FLOW_STMT(Yield)
  CONTROL_FLOW_STMT(Forget)
  CONTROL_FLOW_STMT(Defer)

  VarDecl *visitIfStmt(IfStmt *ifStmt) {
    // Check whether the chain is buildable and whether it terminates
    // without an `else`.
    bool isOptional = false;
    unsigned numPayloads = 0;
    if (!isBuildableIfChain(ifStmt, numPayloads, isOptional)) {
      if (!unhandledNode)
        unhandledNode = ifStmt;
      return nullptr;
    }

    // Attempt to build the chain, propagating short-circuits, which
    // might arise either do to error or not wanting an expression.
    return buildIfChainRecursive(ifStmt, 0, numPayloads, isOptional,
                                 /*isTopLevel=*/true);
  }

  /// Recursively build an if-chain: build an expression which will have
  /// a value of the chain result type before any call to `buildIf`.
  /// The expression will perform any necessary calls to `buildEither`,
  /// and the result will have optional type if `isOptional` is true.
  VarDecl *buildIfChainRecursive(IfStmt *ifStmt, unsigned payloadIndex,
                                 unsigned numPayloads, bool isOptional,
                                 bool isTopLevel = false) {
    assert(payloadIndex < numPayloads);

    // First generate constraints for the conditions. This can introduce
    // variable bindings that will be used within the "then" branch.
    if (cs && cs->generateConstraints(ifStmt->getCond(), dc)) {
      hadError = true;
      return nullptr;
    }

    // Make sure we recursively visit both sides even if we're not
    // building expressions.

    // Build the then clause.  This will have the corresponding payload
    // type (i.e. not wrapped in any way).
    VarDecl *thenVar = visit(ifStmt->getThenStmt());

    // Build the else clause, if present.  If this is from an else-if,
    // this will be fully wrapped; otherwise it will have the corresponding
    // payload type (at index `payloadIndex + 1`).
    assert(ifStmt->getElseStmt() || isOptional);
    bool isElseIf = false;
    Optional<VarDecl *> elseChainVar;
    if (auto elseStmt = ifStmt->getElseStmt()) {
      if (auto elseIfStmt = dyn_cast<IfStmt>(elseStmt)) {
        isElseIf = true;
        elseChainVar = buildIfChainRecursive(elseIfStmt, payloadIndex + 1,
                                             numPayloads, isOptional);
      } else {
        elseChainVar = visit(elseStmt);
      }
    }

    // Short-circuit if appropriate.
    if (!cs || !thenVar || (elseChainVar && !*elseChainVar))
      return nullptr;

    Expr *thenVarRefExpr =
        builder.buildVarRef(thenVar, ifStmt->getThenStmt()->getEndLoc());

    // If there is a #available in the condition, wrap the 'then' in a call to
    // buildLimitedAvailability(_:).
    auto availabilityCond = findAvailabilityCondition(ifStmt->getCond());
    bool supportsAvailability =
        availabilityCond && builder.supports(ctx.Id_buildLimitedAvailability);
    if (supportsAvailability &&
        !availabilityCond->getAvailability()->isUnavailability()) {
      thenVarRefExpr = buildCallIfWanted(ifStmt->getThenStmt()->getEndLoc(),
                                         ctx.Id_buildLimitedAvailability,
                                         {thenVarRefExpr}, {Identifier()});
    }

    // Prepare the `then` operand by wrapping it to produce a chain result.
    Expr *thenExpr = buildWrappedChainPayload(
        thenVarRefExpr, payloadIndex, numPayloads, isOptional);

    // Prepare the `else operand:
    Expr *elseExpr;
    SourceLoc elseLoc;

    // - If there's no `else` clause, use `Optional.none`.
    if (!elseChainVar) {
      assert(isOptional);
      elseLoc = ifStmt->getEndLoc();
      elseExpr = buildNoneExpr(elseLoc);

    // - If there's an `else if`, the chain expression from that
    //   should already be producing a chain result.
    } else if (isElseIf) {
      elseExpr = builder.buildVarRef(*elseChainVar, ifStmt->getEndLoc());
      elseLoc = ifStmt->getElseLoc();

      // - Otherwise, wrap it to produce a chain result.
    } else {
      Expr *elseVarRefExpr =
          builder.buildVarRef(*elseChainVar, ifStmt->getEndLoc());

      // If there is a #unavailable in the condition, wrap the 'else' in a call
      // to buildLimitedAvailability(_:).
      if (supportsAvailability &&
          availabilityCond->getAvailability()->isUnavailability()) {
        elseVarRefExpr = buildCallIfWanted(ifStmt->getEndLoc(),
                                           ctx.Id_buildLimitedAvailability,
                                           {elseVarRefExpr}, {Identifier()});
      }

      elseExpr = buildWrappedChainPayload(elseVarRefExpr, payloadIndex + 1,
                                          numPayloads, isOptional);
      elseLoc = ifStmt->getElseLoc();
    }

    // The operand should have optional type if we had optional results,
    // so we just need to call `buildIf` now, since we're at the top level.
    if (isOptional && isTopLevel) {
      thenExpr =
          buildCallIfWanted(ifStmt->getEndLoc(), builder.getBuildOptionalId(),
                            thenExpr, /*argLabels=*/{});
      elseExpr =
          buildCallIfWanted(ifStmt->getEndLoc(), builder.getBuildOptionalId(),
                            elseExpr, /*argLabels=*/{});
    }

    thenExpr = cs->generateConstraints(thenExpr, dc);
    if (!thenExpr) {
      hadError = true;
      return nullptr;
    }

    elseExpr = cs->generateConstraints(elseExpr, dc);
    if (!elseExpr) {
      hadError = true;
      return nullptr;
    }

    Type resultType = cs->addJoinConstraint(cs->getConstraintLocator(ifStmt),
        {
          { cs->getType(thenExpr), cs->getConstraintLocator(thenExpr) },
          { cs->getType(elseExpr), cs->getConstraintLocator(elseExpr) }
        });
    if (!resultType) {
      hadError = true;
      return nullptr;
    }

    // Create a variable to capture the result of this expression.
    auto ifVar = builder.buildVar(ifStmt->getStartLoc());
    cs->setType(ifVar, resultType);
    applied.capturedStmts.insert({ifStmt, { ifVar, { thenExpr, elseExpr }}});
    return ifVar;
  }

  VarDecl *visitSwitchStmt(SwitchStmt *switchStmt) {
    // Generate constraints for the subject expression, and capture its
    // type for use in matching the various patterns.
    Expr *subjectExpr = switchStmt->getSubjectExpr();
    if (cs) {
      // Form a one-way constraint to prevent backward propagation.
      subjectExpr = new (ctx) OneWayExpr(subjectExpr);

      // FIXME: Add contextual type purpose for switch subjects?
      SolutionApplicationTarget target(subjectExpr, dc, CTP_Unused, Type(),
                                       /*isDiscarded=*/false);
      if (cs->generateConstraints(target)) {
        hadError = true;
        return nullptr;
      }

      cs->setSolutionApplicationTarget(switchStmt, target);
      subjectExpr = target.getAsExpr();
      assert(subjectExpr && "Must have a subject expression here");
    }

    // Generate constraints and capture variables for all of the cases.
    SmallVector<std::pair<CaseStmt *, VarDecl *>, 4> capturedCaseVars;
    for (auto *caseStmt : switchStmt->getCases()) {
      if (auto capturedCaseVar = visitCaseStmt(caseStmt, subjectExpr)) {
        capturedCaseVars.push_back({caseStmt, capturedCaseVar});
      }
    }

    if (!cs)
      return nullptr;

    // If there are no 'case' statements in the body let's try
    // to diagnose this situation via limited exhaustiveness check
    // before failing a builder transform, otherwise type-checker
    // might end up without any diagnostics which leads to crashes
    // in SILGen.
    if (capturedCaseVars.empty()) {
      TypeChecker::checkSwitchExhaustiveness(switchStmt, dc,
                                             /*limitChecking=*/true);
      hadError = true;
      return nullptr;
    }

    // Form the expressions that inject the result of each case into the
    // appropriate
    llvm::TinyPtrVector<Expr *> injectedCaseExprs;
    SmallVector<std::pair<Type, ConstraintLocator *>, 4> injectedCaseTerms;
    for (unsigned idx : indices(capturedCaseVars)) {
      auto caseStmt = capturedCaseVars[idx].first;
      auto caseVar = capturedCaseVars[idx].second;

      // Build the expression that injects the case variable into appropriate
      // buildEither(first:)/buildEither(second:) chain.
      Expr *caseVarRef = builder.buildVarRef(caseVar, caseStmt->getEndLoc());
      Expr *injectedCaseExpr = buildWrappedChainPayload(
          caseVarRef, idx, capturedCaseVars.size(), /*isOptional=*/false);

      // Generate constraints for this injected case result.
      injectedCaseExpr = cs->generateConstraints(injectedCaseExpr, dc);
      if (!injectedCaseExpr) {
        hadError = true;
        return nullptr;
      }

      // Record this injected case expression.
      injectedCaseExprs.push_back(injectedCaseExpr);

      // Record the type and locator for this injected case expression, to be
      // used in the "join" constraint later.
      injectedCaseTerms.push_back(
        { cs->getType(injectedCaseExpr)->getRValueType(),
          cs->getConstraintLocator(injectedCaseExpr) });
    }

    // Form the type of the switch itself.
    Type resultType = cs->addJoinConstraint(
        cs->getConstraintLocator(switchStmt), injectedCaseTerms);
    if (!resultType) {
      hadError = true;
      return nullptr;
    }

    // Create a variable to capture the result of evaluating the switch.
    auto switchVar = builder.buildVar(switchStmt->getStartLoc());
    cs->setType(switchVar, resultType);
    applied.capturedStmts.insert(
        {switchStmt, { switchVar, std::move(injectedCaseExprs) } });
    return switchVar;
  }

  VarDecl *visitCaseStmt(CaseStmt *caseStmt, Expr *subjectExpr) {
    auto *body = caseStmt->getBody();

    // Explicitly disallow `case` statements with empty bodies
    // since that helps to diagnose other issues with switch
    // statements by excluding invalid cases.
    if (auto *BS = dyn_cast<BraceStmt>(body)) {
      if (BS->getNumElements() == 0) {
        // HACK: still allow empty bodies if typechecking for code
        // completion. Code completion ignores diagnostics
        // and won't get any types if we fail.
        if (!ctx.SourceMgr.hasIDEInspectionTargetBuffer()) {
          hadError = true;
          return nullptr;
        }
      }
    }

    // If needed, generate constraints for everything in the case statement.
    if (cs) {
      auto locator = cs->getConstraintLocator(
          subjectExpr, LocatorPathElt::ContextualType(CTP_CaseStmt));
      Type subjectType = cs->getType(subjectExpr);

      if (cs->generateConstraints(caseStmt, dc, subjectType, locator)) {
        hadError = true;
        return nullptr;
      }
    }

    // Translate the body.
    return visit(caseStmt->getBody());
  }

  VarDecl *visitForEachStmt(ForEachStmt *forEachStmt) {
    // for...in statements are handled via buildArray(_:); bail out if the
    // builder does not support it.
    if (!builder.supports(ctx.Id_buildArray)) {
      if (!unhandledNode)
        unhandledNode = forEachStmt;
      return nullptr;
    }

    // Generate constraints for the loop header. This also wires up the
    // types for the patterns.
    auto target = SolutionApplicationTarget::forForEachStmt(
        forEachStmt, dc, /*bindPatternVarsOneWay=*/true);
    if (cs) {
      if (cs->generateConstraints(target)) {
        hadError = true;
        return nullptr;
      }

      cs->setSolutionApplicationTarget(forEachStmt, target);
    }

    // Visit the loop body itself.
    VarDecl *bodyVar = visit(forEachStmt->getBody());
    if (!bodyVar)
      return nullptr;

    // If there's no constraint system, there is nothing left to visit.
    if (!cs)
      return nullptr;

    // Form a variable of array type that will capture the result of each
    // iteration of the loop. We need a fresh type variable to remove the
    // lvalue-ness of the array variable.
    SourceLoc loc = forEachStmt->getForLoc();
    VarDecl *arrayVar = builder.buildVar(loc);
    Type arrayElementType = cs->createTypeVariable(
        cs->getConstraintLocator(forEachStmt), 0);
    cs->addConstraint(ConstraintKind::Equal, cs->getType(bodyVar),
                      arrayElementType,
                      cs->getConstraintLocator(
                          forEachStmt, ConstraintLocator::SequenceElementType));
    Type arrayType = ArraySliceType::get(arrayElementType);
    cs->setType(arrayVar, arrayType);

    // Form an initialization of the array to an empty array literal.
    Expr *arrayInitExpr = ArrayExpr::create(ctx, loc, { }, { }, loc);
    cs->setContextualType(
        arrayInitExpr, TypeLoc::withoutLoc(arrayType), CTP_CannotFail);
    arrayInitExpr = cs->generateConstraints(arrayInitExpr, dc);
    if (!arrayInitExpr) {
      hadError = true;
      return nullptr;
    }
    cs->addConstraint(
        ConstraintKind::Equal, cs->getType(arrayInitExpr), arrayType,
        cs->getConstraintLocator(
            arrayInitExpr, LocatorPathElt::ContextualType(CTP_Initialization)));

    // Form a call to Array.append(_:) to add the result of executing each
    // iteration of the loop body to the array formed above.
    SourceLoc endLoc = forEachStmt->getEndLoc();
    auto arrayVarRef = builder.buildVarRef(arrayVar, endLoc);
    auto arrayAppendRef = new (ctx) UnresolvedDotExpr(
        arrayVarRef, endLoc, DeclNameRef(ctx.getIdentifier("append")),
        DeclNameLoc(endLoc), /*implicit=*/true);
    arrayAppendRef->setFunctionRefKind(FunctionRefKind::SingleApply);

    auto bodyVarRef = builder.buildVarRef(bodyVar, endLoc);
    auto *argList = ArgumentList::createImplicit(
        ctx, endLoc, {Argument::unlabeled(bodyVarRef)}, endLoc);
    Expr *arrayAppendCall =
        CallExpr::createImplicit(ctx, arrayAppendRef, argList);
    arrayAppendCall = cs->generateConstraints(arrayAppendCall, dc);
    if (!arrayAppendCall) {
      hadError = true;
      return nullptr;
    }

    // Form the final call to buildArray(arrayVar) to allow the function
    // builder to reshape the array into whatever it wants as the result of
    // the for-each loop.
    auto finalArrayVarRef = builder.buildVarRef(arrayVar, endLoc);
    auto buildArrayCall = buildCallIfWanted(
        endLoc, ctx.Id_buildArray, { finalArrayVarRef }, { Identifier() });
    assert(buildArrayCall);
    buildArrayCall = cs->generateConstraints(buildArrayCall, dc);
    if (!buildArrayCall) {
      hadError = true;
      return nullptr;
    }

    // Form a final variable for the for-each expression itself, which will
    // be initialized with the call to the result builder's buildArray(_:).
    auto finalForEachVar = builder.buildVar(loc);
    cs->setType(finalForEachVar, cs->getType(buildArrayCall));
    applied.capturedStmts.insert(
      {forEachStmt, {
          finalForEachVar,
          { arrayVarRef, arrayInitExpr, arrayAppendCall, buildArrayCall }}});

    return finalForEachVar;
  }

  /// Visit a throw statement, which never produces a result.
  VarDecl *visitThrowStmt(ThrowStmt *throwStmt) {
    if (!ctx.getErrorDecl()) {
      hadError = true;
    }

    if (cs) {
      SolutionApplicationTarget target(throwStmt->getSubExpr(), dc,
                                       CTP_ThrowStmt,
                                       ctx.getErrorExistentialType(),
                                       /*isDiscarded=*/false);
      if (cs->generateConstraints(target))
        hadError = true;

      cs->setSolutionApplicationTarget(throwStmt, target);
    }

    return nullptr;
  }

  CONTROL_FLOW_STMT(Guard)
  CONTROL_FLOW_STMT(While)
  CONTROL_FLOW_STMT(DoCatch)
  CONTROL_FLOW_STMT(RepeatWhile)
  CONTROL_FLOW_STMT(Case)
  CONTROL_FLOW_STMT(Break)
  CONTROL_FLOW_STMT(Continue)
  CONTROL_FLOW_STMT(Fallthrough)
  CONTROL_FLOW_STMT(Fail)
  CONTROL_FLOW_STMT(PoundAssert)
  CONTROL_FLOW_STMT(Return)

#undef CONTROL_FLOW_STMT
};

class ResultBuilderTransform
    : private BuilderTransformerBase,
      private StmtVisitor<ResultBuilderTransform, NullablePtr<Stmt>,
                          NullablePtr<VarDecl>> {
  friend StmtVisitor<ResultBuilderTransform, NullablePtr<Stmt>,
                     NullablePtr<VarDecl>>;

  using UnsupportedElt = SkipUnhandledConstructInResultBuilder::UnhandledNode;

  /// The result type of this result builder body.
  Type ResultType;

  /// The first recorded unsupported element discovered by the transformation.
  UnsupportedElt FirstUnsupported;

public:
  ResultBuilderTransform(ConstraintSystem &cs, DeclContext *dc,
                         Type builderType, Type resultTy)
      : BuilderTransformerBase(&cs, dc, builderType), ResultType(resultTy) {}

  UnsupportedElt getUnsupportedElement() const { return FirstUnsupported; }

  BraceStmt *apply(BraceStmt *braceStmt) {
    auto newBody = visitBraceStmt(braceStmt, /*bodyVar=*/nullptr);
    if (!newBody)
      return nullptr;
    return castToStmt<BraceStmt>(newBody.get());
  }

  VarDecl *getBuilderSelf() const { return builder.getBuilderSelf(); }

protected:
  NullablePtr<Stmt> failTransform(UnsupportedElt unsupported) {
    recordUnsupported(unsupported);
    return nullptr;
  }

  Expr *buildCallIfWanted(SourceLoc loc, Identifier fnName,
                          ArrayRef<Expr *> argExprs,
                          ArrayRef<Identifier> argLabels) override {
    return builder.buildCall(loc, fnName, argExprs, argLabels);
  }

  VarDecl *recordVar(PatternBindingDecl *PB,
                     SmallVectorImpl<ASTNode> &container) {
    container.push_back(PB);
    container.push_back(PB->getSingleVar());
    return PB->getSingleVar();
  }

  VarDecl *captureExpr(Expr *expr, SmallVectorImpl<ASTNode> &container) {
    auto *var = builder.buildVar(expr->getStartLoc());
    Pattern *pattern = NamedPattern::createImplicit(ctx, var);
    auto *PB = PatternBindingDecl::createImplicit(
        ctx, StaticSpellingKind::None, pattern, expr, dc, var->getStartLoc());
    return recordVar(PB, container);
  }

  VarDecl *buildPlaceholderVar(SourceLoc loc,
                               SmallVectorImpl<ASTNode> &container,
                               Type type = Type(), Expr *initExpr = nullptr) {
    auto *var = builder.buildVar(loc);
    Pattern *placeholder = TypedPattern::createImplicit(
        ctx, NamedPattern::createImplicit(ctx, var),
        type ? type : PlaceholderType::get(ctx, var));
    auto *PB = PatternBindingDecl::createImplicit(
        ctx, StaticSpellingKind::None, placeholder, /*init=*/initExpr, dc,
        var->getStartLoc());
    return recordVar(PB, container);
  }

  AssignExpr *buildAssignment(VarDecl *dst, VarDecl *src) {
    auto *dstRef = builder.buildVarRef(dst, /*Loc=*/SourceLoc());
    auto *srcRef = builder.buildVarRef(src, /*Loc=*/SourceLoc());
    return new (ctx) AssignExpr(dstRef, /*EqualLoc=*/SourceLoc(), srcRef,
                                /*Implicit=*/true);
  }

  AssignExpr *buildAssignment(VarDecl *dst, Expr *srcExpr) {
    auto *dstRef = builder.buildVarRef(dst, /*Loc=*/SourceLoc());
    return new (ctx) AssignExpr(dstRef, /*EqualLoc=*/SourceLoc(), srcExpr,
                                /*implicit=*/true);
  }

  void recordUnsupported(UnsupportedElt node) {
    if (!FirstUnsupported)
      FirstUnsupported = node;
  }

#define UNSUPPORTED_STMT(StmtClass)                                            \
  NullablePtr<Stmt> visit##StmtClass##Stmt(StmtClass##Stmt *stmt,              \
                                           NullablePtr<VarDecl> var) {         \
    return failTransform(stmt);                                                \
  }

  /// Visit the element of a brace statement, returning \c None if the element
  /// was transformed successfully, or an unsupported element if the element
  /// cannot be handled.
  Optional<UnsupportedElt>
  transformBraceElement(ASTNode element, SmallVectorImpl<ASTNode> &newBody,
                        SmallVectorImpl<Expr *> &buildBlockArguments) {
    if (auto *returnStmt = getAsStmt<ReturnStmt>(element)) {
      assert(returnStmt->isImplicit());
      element = returnStmt->getResult();
    }

    if (auto *decl = element.dyn_cast<Decl *>()) {
      switch (decl->getKind()) {
        // Just ignore #if; the chosen children should appear in
        // the surrounding context.  This isn't good for source
        // tools but it at least works.
      case DeclKind::IfConfig:
        // Skip #warning/#error; we'll handle them when applying
        // the builder.
      case DeclKind::PoundDiagnostic:
      case DeclKind::PatternBinding:
      case DeclKind::Var:
      case DeclKind::Param:
        newBody.push_back(element);
        return None;

      default:
        return UnsupportedElt(decl);
      }
      llvm_unreachable("Unhandled case in switch!");
    }

    if (auto *stmt = element.dyn_cast<Stmt *>()) {
      // Throw is allowed as is.
      if (auto *throwStmt = dyn_cast<ThrowStmt>(stmt)) {
        newBody.push_back(throwStmt);
        return None;
      }

      // Allocate variable with a placeholder type
      auto *resultVar = buildPlaceholderVar(stmt->getStartLoc(), newBody);

      auto result = visit(stmt, resultVar);
      if (!result)
        return UnsupportedElt(stmt);

      newBody.push_back(result.get());
      buildBlockArguments.push_back(
          builder.buildVarRef(resultVar, stmt->getStartLoc()));
      return None;
    }

    auto *expr = element.get<Expr *>();
    if (auto *SVE = dyn_cast<SingleValueStmtExpr>(expr)) {
      // This should never be treated as an expression in a result builder, it
      // should have statement semantics.
      return transformBraceElement(SVE->getStmt(), newBody,
                                   buildBlockArguments);
    }
    if (builder.supports(ctx.Id_buildExpression)) {
      expr = builder.buildCall(expr->getLoc(), ctx.Id_buildExpression, {expr},
                               {Identifier()});
    }

    if (isa<CodeCompletionExpr>(expr)) {
      // Insert the CodeCompletionExpr directly into the buildBlock call. That
      // way, we can extract the contextual type of the code completion token
      // to rank code completion items that match the type expected by
      // buildBlock higher.
      buildBlockArguments.push_back(expr);
    } else {
      auto *capture = captureExpr(expr, newBody);
      // A reference to the synthesized variable is passed as an argument
      // to buildBlock.
      buildBlockArguments.push_back(
          builder.buildVarRef(capture, element.getStartLoc()));
    }

    return None;
  }

  std::pair<NullablePtr<Expr>, Optional<UnsupportedElt>>
  transform(BraceStmt *braceStmt, SmallVectorImpl<ASTNode> &newBody) {
    SmallVector<Expr *, 4> buildBlockArguments;

    auto failTransform = [&](UnsupportedElt unsupported) {
      return std::make_pair(nullptr, unsupported);
    };

    for (auto element : braceStmt->getElements()) {
      if (auto unsupported =
              transformBraceElement(element, newBody, buildBlockArguments)) {
        return failTransform(*unsupported);
      }
    }

    // Synthesize `buildBlock` or `buildPartial` based on captured arguments.
    {
      // If the builder supports `buildPartialBlock(first:)` and
      // `buildPartialBlock(accumulated:next:)`, use this to combine
      // sub-expressions pairwise.
      if (!buildBlockArguments.empty() && builder.canUseBuildPartialBlock()) {
        //   let v0 = Builder.buildPartialBlock(first: arg_0)
        //   let v1 = Builder.buildPartialBlock(accumulated: v0, next: arg_1)
        //   ...
        //   let vN = Builder.buildPartialBlock(accumulated: vN-1, next: argN)
        auto *buildPartialFirst = builder.buildCall(
            braceStmt->getStartLoc(), ctx.Id_buildPartialBlock,
            {buildBlockArguments.front()},
            /*argLabels=*/{ctx.Id_first});

        auto *buildBlockVar = captureExpr(buildPartialFirst, newBody);

        for (auto *argExpr : llvm::drop_begin(buildBlockArguments)) {
          auto *accumPartialBlock = builder.buildCall(
              braceStmt->getStartLoc(), ctx.Id_buildPartialBlock,
              {builder.buildVarRef(buildBlockVar, argExpr->getStartLoc()),
               argExpr},
              {ctx.Id_accumulated, ctx.Id_next});
          buildBlockVar = captureExpr(accumPartialBlock, newBody);
        }

        return std::make_pair(
            builder.buildVarRef(buildBlockVar, braceStmt->getStartLoc()), None);
      }
      // If `buildBlock` does not exist at this point, it could be the case that
      // `buildPartialBlock` did not have the sufficient availability for this
      // call site.  Diagnose it.
      else if (!builder.supports(ctx.Id_buildBlock)) {
        ctx.Diags.diagnose(
            braceStmt->getStartLoc(),
            diag::result_builder_missing_available_buildpartialblock,
            builder.getType());
        return failTransform(braceStmt);
      }

      // Otherwise, call `buildBlock` on all subexpressions.
      // Call Builder.buildBlock(... args ...)
      auto *buildBlock = builder.buildCall(
          braceStmt->getStartLoc(), ctx.Id_buildBlock, buildBlockArguments,
          /*argLabels=*/{});
      return std::make_pair(buildBlock, None);
    }
  }

  std::pair<bool, UnsupportedElt>
  transform(BraceStmt *braceStmt, NullablePtr<VarDecl> bodyVar,
            SmallVectorImpl<ASTNode> &elements) {
    // Arguments passed to a synthesized `build{Partial}Block`.
    SmallVector<Expr *, 4> buildBlockArguments;

    auto failure = [&](UnsupportedElt element) {
      return std::make_pair(true, element);
    };

    NullablePtr<Expr> buildBlockVarRef;
    Optional<UnsupportedElt> unsupported;

    std::tie(buildBlockVarRef, unsupported) = transform(braceStmt, elements);
    if (unsupported)
      return failure(*unsupported);

    // If this is not a top-level brace statement, we need to form an
    // assignment from the `build{Partial}Block` call result variable
    // to the provided one.
    //
    // Use start loc for the return statement so any contextual issues
    // are attached to the beginning of the brace instead of its end.
    auto resultLoc = braceStmt->getStartLoc();
    if (bodyVar) {
      elements.push_back(
          new (ctx) AssignExpr(builder.buildVarRef(bodyVar.get(), resultLoc),
                               /*EqualLoc=*/SourceLoc(), buildBlockVarRef.get(),
                               /*Implicit=*/true));
    } else {
      Expr *buildBlockResult = buildBlockVarRef.get();
      // Otherwise, it's a top-level brace and we need to synthesize
      // a call to `buildFialBlock` if supported.
      if (builder.supports(ctx.Id_buildFinalResult, {Identifier()})) {
        buildBlockResult =
            builder.buildCall(resultLoc, ctx.Id_buildFinalResult,
                              {buildBlockResult}, {Identifier()});
      }

      elements.push_back(new (ctx) ReturnStmt(resultLoc, buildBlockResult,
                                              /*Implicit=*/true));
    }

    return std::make_pair(false, UnsupportedElt());
  }

  BraceStmt *cloneBraceWith(BraceStmt *braceStmt,
                            SmallVectorImpl<ASTNode> &elements) {
    auto lBrace = braceStmt ? braceStmt->getLBraceLoc() : SourceLoc();
    auto rBrace = braceStmt ? braceStmt->getRBraceLoc() : SourceLoc();
    bool implicit = braceStmt ? braceStmt->isImplicit() : true;

    return BraceStmt::create(ctx, lBrace, elements, rBrace, implicit);
  }

  NullablePtr<Stmt> visitBraceStmt(BraceStmt *braceStmt,
                                   NullablePtr<VarDecl> bodyVar) {
    SmallVector<ASTNode, 4> elements;

    bool failed;
    UnsupportedElt unsupported;

    std::tie(failed, unsupported) = transform(braceStmt, bodyVar, elements);

    if (failed)
      return failTransform(unsupported);

    return cloneBraceWith(braceStmt, elements);
  }

  NullablePtr<Stmt> visitDoStmt(DoStmt *doStmt, NullablePtr<VarDecl> doVar) {
    auto body = visitBraceStmt(doStmt->getBody(), doVar);
    if (!body)
      return nullptr;

    return new (ctx) DoStmt(doStmt->getLabelInfo(), doStmt->getDoLoc(),
                            cast<BraceStmt>(body.get()), doStmt->isImplicit());
  }

  NullablePtr<Stmt> visitIfStmt(IfStmt *ifStmt, NullablePtr<VarDecl> ifVar) {
    // Check whether the chain is buildable and whether it terminates
    // without an `else`.
    bool isOptional = false;
    unsigned numPayloads = 0;
    if (!isBuildableIfChain(ifStmt, numPayloads, isOptional))
      return failTransform(ifStmt);

    SmallVector<std::pair<Expr *, Stmt *>, 4> branchVarRefs;

    auto transformed = transformIf(ifStmt, branchVarRefs);
    if (!transformed)
      return failTransform(ifStmt);

    // Let's wrap `if` statement into a `do` and inject `type-join`
    // operation with appropriate combination of `buildEither` that
    // would get re-distributed after inference.
    SmallVector<ASTNode, 4> doBody;
    {
      ifStmt = transformed.get();

      // `if` goes first.
      doBody.push_back(ifStmt);

      assert(numPayloads == branchVarRefs.size());

      SmallVector<Expr *, 4> buildEitherCalls;
      for (unsigned i = 0; i != numPayloads; i++) {
        Expr *branchVarRef;
        Stmt *anchor;

        std::tie(branchVarRef, anchor) = branchVarRefs[i];

        auto *builderCall =
            buildWrappedChainPayload(branchVarRef, i, numPayloads, isOptional);

        // The operand should have optional type if we had optional results,
        // so we just need to call `buildIf` now, since we're at the top level.
        if (isOptional) {
          builderCall = buildCallIfWanted(ifStmt->getThenStmt()->getStartLoc(),
                                          builder.getBuildOptionalId(),
                                          builderCall, /*argLabels=*/{});
        }

        buildEitherCalls.push_back(builderCall);
      }

      // If there is no `else` branch we need to build one.
      // It consists a `buildOptional` call that uses `nil` as an argument.
      //
      // ```
      // {
      //   $__builderResult = buildOptional(nil)
      // }
      // ```
      //
      // Type of `nil` is going to be inferred from `$__builderResult`.
      if (!hasUnconditionalElse(ifStmt)) {
        assert(isOptional);

        auto *nil =
            new (ctx) NilLiteralExpr(ifStmt->getEndLoc(), /*implicit=*/true);

        buildEitherCalls.push_back(buildCallIfWanted(
            /*loc=*/ifStmt->getEndLoc(), builder.getBuildOptionalId(), nil,
            /*argLabels=*/{}));
      }

      auto *ifVarRef = builder.buildVarRef(ifVar.get(), ifStmt->getStartLoc());
      doBody.push_back(TypeJoinExpr::create(ctx, ifVarRef, buildEitherCalls));
    }

    return DoStmt::createImplicit(ctx, LabeledStmtInfo(), doBody);
  }

  NullablePtr<IfStmt>
  transformIf(IfStmt *ifStmt,
              SmallVectorImpl<std::pair<Expr *, Stmt *>> &branchVarRefs) {
    Optional<UnsupportedElt> unsupported;

    // If there is a #available in the condition, wrap the 'then' or 'else'
    // in a call to buildLimitedAvailability(_:).
    auto availabilityCond = findAvailabilityCondition(ifStmt->getCond());
    bool supportsAvailability =
        availabilityCond && builder.supports(ctx.Id_buildLimitedAvailability);

    NullablePtr<Expr> thenVarRef;
    NullablePtr<Stmt> thenBranch;
    {
      SmallVector<ASTNode, 4> thenBody;

      auto *ifBraceStmt = cast<BraceStmt>(ifStmt->getThenStmt());

      std::tie(thenVarRef, unsupported) = transform(ifBraceStmt, thenBody);
      if (unsupported) {
        recordUnsupported(*unsupported);
        return nullptr;
      }

      if (supportsAvailability &&
          !availabilityCond->getAvailability()->isUnavailability()) {
        auto *builderCall = buildCallIfWanted(
            ifBraceStmt->getStartLoc(), ctx.Id_buildLimitedAvailability,
            {thenVarRef.get()}, {Identifier()});

        thenVarRef = builder.buildVarRef(captureExpr(builderCall, thenBody),
                                         ifBraceStmt->getStartLoc());
      }

      thenBranch = cloneBraceWith(ifBraceStmt, thenBody);
      branchVarRefs.push_back({thenVarRef.get(), thenBranch.get()});
    }

    NullablePtr<Stmt> elseBranch;

    if (auto *elseStmt = ifStmt->getElseStmt()) {
      NullablePtr<Expr> elseVarRef;

      if (auto *innerIfStmt = getAsStmt<IfStmt>(elseStmt)) {
        elseBranch = transformIf(innerIfStmt, branchVarRefs);
        if (!elseBranch) {
          recordUnsupported(innerIfStmt);
          return nullptr;
        }
      } else {
        auto *elseBraceStmt = cast<BraceStmt>(elseStmt);
        SmallVector<ASTNode> elseBody;

        std::tie(elseVarRef, unsupported) = transform(elseBraceStmt, elseBody);
        if (unsupported) {
          recordUnsupported(*unsupported);
          return nullptr;
        }

        // If there is a #unavailable in the condition, wrap the 'else' in a
        // call to buildLimitedAvailability(_:).
        if (supportsAvailability &&
            availabilityCond->getAvailability()->isUnavailability()) {
          auto *builderCall = buildCallIfWanted(
              elseBraceStmt->getStartLoc(), ctx.Id_buildLimitedAvailability,
              {elseVarRef.get()}, {Identifier()});

          elseVarRef = builder.buildVarRef(captureExpr(builderCall, elseBody),
                                           elseBraceStmt->getStartLoc());
        }

        elseBranch = cloneBraceWith(elseBraceStmt, elseBody);
        branchVarRefs.push_back({elseVarRef.get(), elseBranch.get()});
      }
    }

    return new (ctx)
        IfStmt(ifStmt->getLabelInfo(), ifStmt->getIfLoc(), ifStmt->getCond(),
               thenBranch.get(), ifStmt->getElseLoc(),
               elseBranch.getPtrOrNull(), ifStmt->isImplicit());
  }

  NullablePtr<Stmt> visitSwitchStmt(SwitchStmt *switchStmt,
                                    NullablePtr<VarDecl> switchVar) {
    // For a do statement wrapping this switch that contains all of the
    // `buildEither` calls that would get injected back into `case` bodies
    // after solving is done.
    //
    // This is necessary because `buildEither requires type information from
    // both sides to be available, so all case statements have to be
    // type-checked first.
    SmallVector<ASTNode, 4> doBody;

    SmallVector<ASTNode, 4> cases;
    SmallVector<Expr *, 4> caseVarRefs;

    for (auto *caseStmt : switchStmt->getCases()) {
      auto transformed = transformCase(caseStmt);
      if (!transformed)
        return failTransform(caseStmt);

      cases.push_back(transformed->second);
      caseVarRefs.push_back(transformed->first);
    }

    // If there are no 'case' statements in the body let's try
    // to diagnose this situation via limited exhaustiveness check
    // before failing a builder transform, otherwise type-checker
    // might end up without any diagnostics which leads to crashes
    // in SILGen.
    if (caseVarRefs.empty()) {
      TypeChecker::checkSwitchExhaustiveness(switchStmt, dc,
                                             /*limitChecking=*/true);
      return failTransform(switchStmt);
    }

    auto *transformedSwitch = SwitchStmt::create(
        switchStmt->getLabelInfo(), switchStmt->getSwitchLoc(),
        switchStmt->getSubjectExpr(), switchStmt->getLBraceLoc(), cases,
        switchStmt->getRBraceLoc(), switchStmt->getEndLoc(), ctx);

    doBody.push_back(transformedSwitch);

    SmallVector<Expr *, 4> injectedExprs;
    for (auto idx : indices(caseVarRefs)) {
      auto *caseVarRef = caseVarRefs[idx];

      // Build the expression that injects the case variable into appropriate
      // buildEither(first:)/buildEither(second:) chain.
      Expr *injectedCaseExpr = buildWrappedChainPayload(
          caseVarRef, idx, caseVarRefs.size(), /*isOptional=*/false);

      injectedExprs.push_back(injectedCaseExpr);
    }

    auto *switchVarRef =
        builder.buildVarRef(switchVar.get(), switchStmt->getEndLoc());
    doBody.push_back(TypeJoinExpr::create(ctx, switchVarRef, injectedExprs));

    return DoStmt::createImplicit(ctx, LabeledStmtInfo(), doBody);
  }

  Optional<std::pair<Expr *, CaseStmt *>> transformCase(CaseStmt *caseStmt) {
    auto *body = caseStmt->getBody();

    // Explicitly disallow `case` statements with empty bodies
    // since that helps to diagnose other issues with switch
    // statements by excluding invalid cases.
    if (auto *BS = dyn_cast<BraceStmt>(body)) {
      if (BS->getNumElements() == 0) {
        // HACK: still allow empty bodies if typechecking for code
        // completion. Code completion ignores diagnostics
        // and won't get any types if we fail.
        if (!ctx.SourceMgr.hasIDEInspectionTargetBuffer())
          return None;
      }
    }

    NullablePtr<Expr> caseVarRef;
    Optional<UnsupportedElt> unsupported;
    SmallVector<ASTNode, 4> newBody;

    std::tie(caseVarRef, unsupported) = transform(body, newBody);

    if (unsupported) {
      recordUnsupported(*unsupported);
      return None;
    }

    auto *newCase = CaseStmt::create(
        ctx, caseStmt->getParentKind(), caseStmt->getLoc(),
        caseStmt->getCaseLabelItems(),
        caseStmt->hasUnknownAttr() ? caseStmt->getStartLoc() : SourceLoc(),
        caseStmt->getItemTerminatorLoc(), cloneBraceWith(body, newBody),
        caseStmt->getCaseBodyVariablesOrEmptyArray(), caseStmt->isImplicit(),
        caseStmt->getFallthroughStmt());

    return std::make_pair(caseVarRef.get(), newCase);
  }

  /// do {
  ///   var $__forEach = []
  ///   for ... in ... {
  ///     ...
  ///     $__builderVar = buildBlock(...)
  ///     $__forEach.append($__builderVar)
  ///   }
  ///   buildArray($__forEach)
  /// }
  NullablePtr<Stmt> visitForEachStmt(ForEachStmt *forEachStmt,
                                     NullablePtr<VarDecl> forEachVar) {
    // for...in statements are handled via buildArray(_:); bail out if the
    // builder does not support it.
    if (!builder.supports(ctx.Id_buildArray))
      return failTransform(forEachStmt);

    // For-each statements require the Sequence protocol. If we don't have
    // it (which generally means the standard library isn't loaded), fall
    // out of the result-builder path entirely to let normal type checking
    // take care of this.
    auto sequenceProto = TypeChecker::getProtocol(
        dc->getASTContext(), forEachStmt->getForLoc(),
        forEachStmt->getAwaitLoc().isValid() ? KnownProtocolKind::AsyncSequence
                                             : KnownProtocolKind::Sequence);
    if (!sequenceProto)
      return failTransform(forEachStmt);

    SmallVector<ASTNode, 4> doBody;
    SourceLoc endLoc = forEachStmt->getEndLoc();

    // Build a variable that is going to hold array of results produced
    // by each iteration of the loop.
    //
    // Not that it's not going to be initialized here, that would happen
    // only when a solution is found.
    VarDecl *arrayVar = buildPlaceholderVar(
        forEachStmt->getEndLoc(), doBody,
        ArraySliceType::get(PlaceholderType::get(ctx, forEachVar.get())),
        ArrayExpr::create(ctx, /*LBrace=*/endLoc, /*Elements=*/{},
                          /*Commas=*/{}, /*RBrace=*/endLoc));

    NullablePtr<Expr> bodyVarRef;
    Optional<UnsupportedElt> unsupported;

    SmallVector<ASTNode, 4> newBody;
    {
      std::tie(bodyVarRef, unsupported) =
          transform(forEachStmt->getBody(), newBody);
      if (unsupported)
        return failTransform(*unsupported);

      // Form a call to Array.append(_:) to add the result of executing each
      // iteration of the loop body to the array formed above.
      {
        auto arrayVarRef = builder.buildVarRef(arrayVar, endLoc);
        auto arrayAppendRef = new (ctx) UnresolvedDotExpr(
            arrayVarRef, endLoc, DeclNameRef(ctx.getIdentifier("append")),
            DeclNameLoc(endLoc), /*implicit=*/true);
        arrayAppendRef->setFunctionRefKind(FunctionRefKind::SingleApply);

        auto *argList = ArgumentList::createImplicit(
            ctx, endLoc, {Argument::unlabeled(bodyVarRef.get())}, endLoc);

        newBody.push_back(
            CallExpr::createImplicit(ctx, arrayAppendRef, argList));
      }
    }

    auto *newForEach = new (ctx)
        ForEachStmt(forEachStmt->getLabelInfo(), forEachStmt->getForLoc(),
                    forEachStmt->getTryLoc(), forEachStmt->getAwaitLoc(),
                    forEachStmt->getPattern(), forEachStmt->getInLoc(),
                    forEachStmt->getParsedSequence(),
                    forEachStmt->getWhereLoc(), forEachStmt->getWhere(),
                    cloneBraceWith(forEachStmt->getBody(), newBody),
                    forEachStmt->isImplicit());

    // For a body of new `do` statement that holds updated `for-in` loop
    // and epilog that consists of a call to `buildArray` that forms the
    // final result.
    {
      // Modified `for { ... }`
      doBody.push_back(newForEach);

      // $__forEach = buildArray($__arrayVar)
      doBody.push_back(buildAssignment(
          forEachVar.get(),
          buildCallIfWanted(forEachStmt->getEndLoc(), ctx.Id_buildArray,
                            {builder.buildVarRef(arrayVar, endLoc)},
                            {Identifier()})));
    }

    return DoStmt::createImplicit(ctx, LabeledStmtInfo(), doBody);
  }

  UNSUPPORTED_STMT(Throw)
  UNSUPPORTED_STMT(Return)
  UNSUPPORTED_STMT(Yield)
  UNSUPPORTED_STMT(Forget)
  UNSUPPORTED_STMT(Defer)
  UNSUPPORTED_STMT(Guard)
  UNSUPPORTED_STMT(While)
  UNSUPPORTED_STMT(DoCatch)
  UNSUPPORTED_STMT(RepeatWhile)
  UNSUPPORTED_STMT(Break)
  UNSUPPORTED_STMT(Continue)
  UNSUPPORTED_STMT(Fallthrough)
  UNSUPPORTED_STMT(Fail)
  UNSUPPORTED_STMT(PoundAssert)
  UNSUPPORTED_STMT(Case)

#undef UNSUPPORTED_STMT
};

/// Describes the target into which the result of a particular statement in
/// a closure involving a result builder should be written.
struct ResultBuilderTarget {
  enum Kind {
    /// The resulting value is returned from the closure.
    ReturnValue,
    /// The temporary variable into which the result should be assigned.
    TemporaryVar,
    /// An expression to evaluate at the end of the block, allowing the update
    /// of some state from an outer scope.
    Expression,
  } kind;

  /// Captured variable information.
  std::pair<VarDecl *, llvm::TinyPtrVector<Expr *>> captured;

  static ResultBuilderTarget forReturn(Expr *expr) {
    return ResultBuilderTarget{ReturnValue, {nullptr, {expr}}};
  }

  static ResultBuilderTarget forAssign(VarDecl *temporaryVar,
                                         llvm::TinyPtrVector<Expr *> exprs) {
    return ResultBuilderTarget{TemporaryVar, {temporaryVar, exprs}};
  }

  static ResultBuilderTarget forExpression(Expr *expr) {
    return ResultBuilderTarget{Expression, { nullptr, { expr }}};
  }
};

/// Handles the rewrite of the body of a closure to which a result builder
/// has been applied.
class BuilderClosureRewriter
    : public StmtVisitor<BuilderClosureRewriter, NullablePtr<Stmt>,
                         ResultBuilderTarget> {
  ASTContext &ctx;
  const Solution &solution;
  DeclContext *dc;
  AppliedBuilderTransform builderTransform;
  std::function<
      Optional<SolutionApplicationTarget> (SolutionApplicationTarget)>
        rewriteTarget;

  /// Retrieve the temporary variable that will be used to capture the
  /// value of the given expression.
  AppliedBuilderTransform::RecordedExpr takeCapturedExpr(Expr *expr) {
    auto found = builderTransform.capturedExprs.find(expr);
    assert(found != builderTransform.capturedExprs.end());

    // Set the type of the temporary variable.
    auto recorded = found->second;
    if (auto temporaryVar = recorded.temporaryVar) {
      Type type = solution.simplifyType(solution.getType(temporaryVar));
      temporaryVar->setInterfaceType(type->mapTypeOutOfContext());
    }

    // Erase the captured expression, so we're sure we never do this twice.
    builderTransform.capturedExprs.erase(found);
    return recorded;
  }

  /// Rewrite an expression without any particularly special context.
  Expr *rewriteExpr(Expr *expr) {
    auto result = rewriteTarget(
      SolutionApplicationTarget(expr, dc, CTP_Unused, Type(),
                                /*isDiscarded=*/false));
    if (result)
      return result->getAsExpr();

    return nullptr;
  }

public:
  /// Retrieve information about a captured statement.
  std::pair<VarDecl *, llvm::TinyPtrVector<Expr *>>
  takeCapturedStmt(Stmt *stmt) {
    auto found = builderTransform.capturedStmts.find(stmt);
    assert(found != builderTransform.capturedStmts.end());

    // Set the type of the temporary variable.
    auto temporaryVar = found->second.first;
    Type type = solution.simplifyType(solution.getType(temporaryVar));
    temporaryVar->setInterfaceType(type->mapTypeOutOfContext());

    // Take the expressions.
    auto exprs = std::move(found->second.second);

    // Erase the statement, so we're sure we never do this twice.
    builderTransform.capturedStmts.erase(found);
    return std::make_pair(temporaryVar, std::move(exprs));
  }

private:
  /// Build the statement or expression to initialize the target.
  ASTNode initializeTarget(ResultBuilderTarget target) {
    assert(target.captured.second.size() == 1);
    auto capturedExpr = target.captured.second.front();
    SourceLoc implicitLoc = capturedExpr->getEndLoc();
    switch (target.kind) {
    case ResultBuilderTarget::ReturnValue: {
      // Return the expression.
      Type bodyResultInterfaceType =
          solution.simplifyType(builderTransform.bodyResultType);

      SolutionApplicationTarget returnTarget(capturedExpr, dc, CTP_ReturnStmt,
                                             bodyResultInterfaceType,
                                             /*isDiscarded=*/false);
      Expr *resultExpr = nullptr;
      if (auto resultTarget = rewriteTarget(returnTarget))
        resultExpr = resultTarget->getAsExpr();

      return new (ctx) ReturnStmt(implicitLoc, resultExpr);
    }

    case ResultBuilderTarget::TemporaryVar: {
      // Assign the expression into a variable.
      auto temporaryVar = target.captured.first;
      auto declRef = new (ctx) DeclRefExpr(
          temporaryVar, DeclNameLoc(implicitLoc), /*implicit=*/true);
      declRef->setType(LValueType::get(temporaryVar->getType()));

      // Load the right-hand side if needed.
      auto finalCapturedExpr = rewriteExpr(capturedExpr);
      if (finalCapturedExpr->getType()->hasLValueType()) {
        finalCapturedExpr =
            TypeChecker::addImplicitLoadExpr(ctx, finalCapturedExpr);
      }

      auto assign = new (ctx) AssignExpr(
          declRef, implicitLoc, finalCapturedExpr, /*implicit=*/true);
      assign->setType(TupleType::getEmpty(ctx));
      return assign;
    }

    case ResultBuilderTarget::Expression:
      // Execute the expression.
      return rewriteExpr(capturedExpr);
    }
    llvm_unreachable("invalid result builder target");
  }

  /// Declare the given temporary variable, adding the appropriate
  /// entries to the elements of a brace stmt.
  void declareTemporaryVariable(VarDecl *temporaryVar,
                                std::vector<ASTNode> &elements,
                                Expr *initExpr = nullptr) {
    if (!temporaryVar)
      return;

    // Form a new pattern binding to bind the temporary variable to the
    // transformed expression.
    auto pattern = NamedPattern::createImplicit(ctx, temporaryVar);
    pattern->setType(temporaryVar->getType());

    auto pbd = PatternBindingDecl::create(
        ctx, SourceLoc(), StaticSpellingKind::None, temporaryVar->getLoc(),
        pattern, SourceLoc(), initExpr, dc);
    if (temporaryVar->isImplicit())
      pbd->setImplicit();
    elements.push_back(temporaryVar);
    elements.push_back(pbd);
  }

public:
  BuilderClosureRewriter(
      const Solution &solution,
      DeclContext *dc,
      const AppliedBuilderTransform &builderTransform,
      std::function<
          Optional<SolutionApplicationTarget> (SolutionApplicationTarget)>
            rewriteTarget
    ) : ctx(solution.getConstraintSystem().getASTContext()),
        solution(solution), dc(dc), builderTransform(builderTransform),
        rewriteTarget(rewriteTarget) { }

  /// Visit the element of a brace statement, returning \c false if the element
  /// was rewritten successfully, or \c true if there was an error.
  bool visitBraceElement(ASTNode node, std::vector<ASTNode> &newElements) {
    // Implicit returns in single-expression function bodies are treated
    // as the expression.
    if (auto returnStmt =
            dyn_cast_or_null<ReturnStmt>(node.dyn_cast<Stmt *>())) {
      assert(returnStmt->isImplicit());
      node = returnStmt->getResult();
    }

    if (auto expr = node.dyn_cast<Expr *>()) {
      if (auto *SVE = dyn_cast<SingleValueStmtExpr>(expr)) {
        // This should never be treated as an expression in a result builder, it
        // should have statement semantics.
        return visitBraceElement(SVE->getStmt(), newElements);
      }
      // Skip error expressions.
      if (isa<ErrorExpr>(expr))
        return false;

      // Each expression turns into a 'let' that captures the value of
      // the expression.
      auto recorded = takeCapturedExpr(expr);

      // Rewrite the expression
      Expr *finalExpr = rewriteExpr(recorded.generatedExpr);

      // Form a new pattern binding to bind the temporary variable to the
      // transformed expression.
      declareTemporaryVariable(recorded.temporaryVar, newElements, finalExpr);
      return false;
    }

    if (auto stmt = node.dyn_cast<Stmt *>()) {
      // "throw" statements produce no value. Transform them directly.
      if (auto throwStmt = dyn_cast<ThrowStmt>(stmt)) {
        if (auto newStmt = visitThrowStmt(throwStmt)) {
          newElements.push_back(newStmt.get());
        }
        return false;
      }

      // Each statement turns into a (potential) temporary variable
      // binding followed by the statement itself.
      auto captured = takeCapturedStmt(stmt);

      declareTemporaryVariable(captured.first, newElements);

      auto finalStmt =
          visit(stmt, ResultBuilderTarget{ResultBuilderTarget::TemporaryVar,
                                          std::move(captured)});

      // Re-write of statements that envolve type-checking
      // could fail, such a failure terminates the walk.
      if (!finalStmt)
        return true;

      newElements.push_back(finalStmt.get());
      return false;
    }

    auto decl = node.get<Decl *>();

    // Skip #if declarations.
    if (isa<IfConfigDecl>(decl)) {
      newElements.push_back(decl);
      return false;
    }

    // Diagnose #warning / #error during application.
    if (auto poundDiag = dyn_cast<PoundDiagnosticDecl>(decl)) {
      TypeChecker::typeCheckDecl(poundDiag);
      newElements.push_back(decl);
      return false;
    }

    // Skip variable declarations; they're always part of a pattern
    // binding.
    if (isa<VarDecl>(decl)) {
      TypeChecker::typeCheckDecl(decl);
      newElements.push_back(decl);
      return false;
    }

    // Handle pattern bindings.
    if (auto patternBinding = dyn_cast<PatternBindingDecl>(decl)) {
      auto resultTarget =
          rewriteTarget(SolutionApplicationTarget{patternBinding});
      assert(resultTarget.has_value() &&
             "Could not rewrite pattern binding entries!");
      TypeChecker::typeCheckDecl(resultTarget->getAsPatternBinding());
      newElements.push_back(resultTarget->getAsPatternBinding());
      return false;
    }

    llvm_unreachable("Cannot yet handle declarations");
  }

  NullablePtr<Stmt>
  visitBraceStmt(BraceStmt *braceStmt, ResultBuilderTarget target,
                 Optional<ResultBuilderTarget> innerTarget = None) {
    std::vector<ASTNode> newElements;

    // If there is an "inner" target corresponding to this brace, declare
    // it's temporary variable if needed.
    if (innerTarget)
      declareTemporaryVariable(innerTarget->captured.first, newElements);

    for (auto node : braceStmt->getElements()) {
      if (visitBraceElement(node, newElements))
        return nullptr;
    }

    // If there is an "inner" target corresponding to this brace, initialize
    // it.
    if (innerTarget) {
      newElements.push_back(initializeTarget(*innerTarget));
    }

    // Capture the result of the buildBlock() call in the manner requested
    // by the caller.
    newElements.push_back(initializeTarget(target));

    return BraceStmt::create(ctx, braceStmt->getLBraceLoc(), newElements,
                             braceStmt->getRBraceLoc());
  }

  NullablePtr<Stmt> visitIfStmt(IfStmt *ifStmt, ResultBuilderTarget target) {
    // Rewrite the condition.
    if (auto condition = rewriteTarget(
            SolutionApplicationTarget(ifStmt->getCond(), dc)))
      ifStmt->setCond(*condition->getAsStmtCondition());

    assert(target.kind == ResultBuilderTarget::TemporaryVar);
    auto temporaryVar = target.captured.first;

    // Translate the "then" branch.
    auto capturedThen = takeCapturedStmt(ifStmt->getThenStmt());
    auto newThen = visitBraceStmt(cast<BraceStmt>(ifStmt->getThenStmt()),
          ResultBuilderTarget::forAssign(
            temporaryVar, {target.captured.second[0]}),
          ResultBuilderTarget::forAssign(
            capturedThen.first, {capturedThen.second.front()}));
    if (!newThen)
      return nullptr;

    ifStmt->setThenStmt(newThen.get());

    // Look for a #available condition. If there is one, we need to check
    // that the resulting type of the "then" doesn't refer to any types that
    // are unavailable in the enclosing context.
    //
    // Note that this is for staging in support for buildLimitedAvailability();
    // the diagnostic is currently a warning, so that existing code that
    // compiles today will continue to compile. Once result builder types
    // have had the chance to adopt buildLimitedAvailability(), we'll upgrade
    // this warning to an error.
    if (auto availabilityCond = findAvailabilityCondition(ifStmt->getCond())) {
      SourceLoc loc = availabilityCond->getStartLoc();
      Type bodyType;
      if (availabilityCond->getAvailability()->isUnavailability()) {
        // For #unavailable, we need to check the "else".
        Type elseBodyType = solution.simplifyType(
          solution.getType(target.captured.second[1]));
        bodyType = elseBodyType;
      } else {
        Type thenBodyType = solution.simplifyType(
          solution.getType(target.captured.second[0]));
        bodyType = thenBodyType;
      }
      bodyType.findIf([&](Type type) {
        auto nominal = type->getAnyNominal();
        if (!nominal)
          return false;

        ExportContext where = ExportContext::forFunctionBody(dc, loc);
        if (auto reason = TypeChecker::checkDeclarationAvailability(
                              nominal, where)) {
          ctx.Diags.diagnose(
              loc, diag::result_builder_missing_limited_availability,
              builderTransform.builderType);

          // Add a note to the result builder with a stub for
          // buildLimitedAvailability().
          if (auto builder = builderTransform.builderType->getAnyNominal()) {
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

                builder->diagnose(
                    diag::result_builder_missing_build_limited_availability,
                    builderTransform.builderType)
                  .fixItInsert(buildInsertionLoc, fixItString);
              }
            }
          }

          return true;
        }

        return false;
      });
    }

    if (auto elseBraceStmt =
            dyn_cast_or_null<BraceStmt>(ifStmt->getElseStmt())) {
      // Translate the "else" branch when it's a stmt-brace.
      auto capturedElse = takeCapturedStmt(elseBraceStmt);
      auto newElse = visitBraceStmt(
          elseBraceStmt,
          ResultBuilderTarget::forAssign(
            temporaryVar, {target.captured.second[1]}),
          ResultBuilderTarget::forAssign(
            capturedElse.first, {capturedElse.second.front()}));
      if (!newElse)
        return nullptr;

      ifStmt->setElseStmt(newElse.get());
    } else if (auto elseIfStmt = cast_or_null<IfStmt>(ifStmt->getElseStmt())){
      // Translate the "else" branch when it's an else-if.
      auto capturedElse = takeCapturedStmt(elseIfStmt);
      std::vector<ASTNode> newElseElements;
      declareTemporaryVariable(capturedElse.first, newElseElements);
      auto newElseElt =
          visitIfStmt(elseIfStmt, ResultBuilderTarget::forAssign(
                                      capturedElse.first, capturedElse.second));
      if (!newElseElt)
        return nullptr;

      newElseElements.push_back(newElseElt.get());
      newElseElements.push_back(
          initializeTarget(
            ResultBuilderTarget::forAssign(
              temporaryVar, {target.captured.second[1]})));

      Stmt *newElse = BraceStmt::create(
          ctx, elseIfStmt->getStartLoc(), newElseElements,
          elseIfStmt->getEndLoc());
      ifStmt->setElseStmt(newElse);
    } else {
      // Form an "else" brace containing an assignment to the temporary
      // variable.
      auto init = initializeTarget(
          ResultBuilderTarget::forAssign(
            temporaryVar, {target.captured.second[1]}));
      auto newElse = BraceStmt::create(
          ctx, ifStmt->getEndLoc(), { init }, ifStmt->getEndLoc());
      ifStmt->setElseStmt(newElse);
    }

    return ifStmt;
  }

  NullablePtr<Stmt> visitDoStmt(DoStmt *doStmt, ResultBuilderTarget target) {
    // Each statement turns into a (potential) temporary variable
    // binding followed by the statement itself.
    auto body = cast<BraceStmt>(doStmt->getBody());
    auto captured = takeCapturedStmt(body);

    auto newInnerBody =
        visitBraceStmt(body, target,
                       ResultBuilderTarget::forAssign(
                           captured.first, {captured.second.front()}));
    if (!newInnerBody)
      return nullptr;

    doStmt->setBody(cast<BraceStmt>(newInnerBody.get()));
    return doStmt;
  }

  NullablePtr<Stmt> visitSwitchStmt(SwitchStmt *switchStmt,
                                    ResultBuilderTarget target) {
    // Translate the subject expression.
    ConstraintSystem &cs = solution.getConstraintSystem();
    auto subjectTarget =
        rewriteTarget(*cs.getSolutionApplicationTarget(switchStmt));
    if (!subjectTarget)
      return nullptr;

    switchStmt->setSubjectExpr(subjectTarget->getAsExpr());

    // Handle any declaration nodes within the case list first; we'll
    // handle the cases in a second pass.
    for (auto child : switchStmt->getRawCases()) {
      if (auto decl = child.dyn_cast<Decl *>()) {
        TypeChecker::typeCheckDecl(decl);
      }
    }

    // Translate all of the cases.
    bool limitExhaustivityChecks = false;
    assert(target.kind == ResultBuilderTarget::TemporaryVar);
    auto temporaryVar = target.captured.first;
    unsigned caseIndex = 0;
    for (auto caseStmt : switchStmt->getCases()) {
      if (!visitCaseStmt(
            caseStmt,
            ResultBuilderTarget::forAssign(
              temporaryVar, {target.captured.second[caseIndex]})))
        return nullptr;

      // Check restrictions on '@unknown'.
      if (caseStmt->hasUnknownAttr()) {
        checkUnknownAttrRestrictions(
            cs.getASTContext(), caseStmt, limitExhaustivityChecks);
      }

      ++caseIndex;
    }

    TypeChecker::checkSwitchExhaustiveness(
        switchStmt, dc, limitExhaustivityChecks);

    return switchStmt;
  }

  NullablePtr<Stmt> visitCaseStmt(CaseStmt *caseStmt,
                                  ResultBuilderTarget target) {
    // Translate the patterns and guard expressions for each case label item.
    for (auto &caseLabelItem : caseStmt->getMutableCaseLabelItems()) {
      SolutionApplicationTarget caseLabelTarget(&caseLabelItem, dc);
      if (!rewriteTarget(caseLabelTarget))
        return nullptr;
    }

    // Setup the types of our case body var decls.
    for (auto *expected : caseStmt->getCaseBodyVariablesOrEmptyArray()) {
      assert(expected->hasName());
      auto prev = expected->getParentVarDecl();
      auto type = solution.resolveInterfaceType(
          solution.getType(prev)->mapTypeOutOfContext());
      expected->setInterfaceType(type);
    }

    // Transform the body of the case.
    auto body = cast<BraceStmt>(caseStmt->getBody());
    auto captured = takeCapturedStmt(body);
    auto newInnerBody =
        visitBraceStmt(body, target,
                       ResultBuilderTarget::forAssign(
                           captured.first, {captured.second.front()}));
    if (!newInnerBody)
      return nullptr;

    caseStmt->setBody(cast<BraceStmt>(newInnerBody.get()));
    return caseStmt;
  }

  NullablePtr<Stmt> visitForEachStmt(ForEachStmt *forEachStmt,
                                     ResultBuilderTarget target) {
    // Translate the for-each loop header.
    ConstraintSystem &cs = solution.getConstraintSystem();
    auto forEachTarget =
        rewriteTarget(*cs.getSolutionApplicationTarget(forEachStmt));
    if (!forEachTarget)
      return nullptr;

    const auto &captured = target.captured;
    auto finalForEachVar = captured.first;
    auto arrayVarRef = captured.second[0];
    auto arrayVar = cast<VarDecl>(cast<DeclRefExpr>(arrayVarRef)->getDecl());
    auto arrayInitExpr = captured.second[1];
    auto arrayAppendCall = captured.second[2];
    auto buildArrayCall = captured.second[3];

    // Collect the three steps to initialize the array variable to an
    // empty array, execute the loop to collect the results of each iteration,
    // then form the buildArray() call to the write the result.
    std::vector<ASTNode> outerBodySteps;

    // Step 1: Declare and initialize the array variable.
    arrayVar->setInterfaceType(solution.simplifyType(cs.getType(arrayVar)));
    arrayInitExpr = rewriteExpr(arrayInitExpr);
    declareTemporaryVariable(arrayVar, outerBodySteps, arrayInitExpr);

    // Step 2. Transform the body of the for-each statement. Each iteration
    // will append the result of executing the loop body to the array.
    auto body = forEachStmt->getBody();
    auto capturedBody = takeCapturedStmt(body);
    auto newBody = visitBraceStmt(
        body, ResultBuilderTarget::forExpression(arrayAppendCall),
        ResultBuilderTarget::forAssign(capturedBody.first,
                                       {capturedBody.second.front()}));
    if (!newBody)
      return nullptr;

    forEachStmt->setBody(cast<BraceStmt>(newBody.get()));
    outerBodySteps.push_back(forEachStmt);

    // Step 3. Perform the buildArray() call to turn the array of results
    // collected from the iterations into a single value under the control of
    // the result builder.
    outerBodySteps.push_back(
        initializeTarget(
          ResultBuilderTarget::forAssign(finalForEachVar, {buildArrayCall})));

    // Form a brace statement to put together the three main steps for the
    // for-each loop translation outlined above.
    return BraceStmt::create(ctx, forEachStmt->getStartLoc(), outerBodySteps,
                             newBody.get()->getEndLoc());
  }

  NullablePtr<Stmt> visitThrowStmt(ThrowStmt *throwStmt) {
    // Rewrite the error.
    auto target = *solution.getConstraintSystem()
        .getSolutionApplicationTarget(throwStmt);
    if (auto result = rewriteTarget(target))
      throwStmt->setSubExpr(result->getAsExpr());
    else
      return nullptr;

    return throwStmt;
  }

  NullablePtr<Stmt> visitThrowStmt(ThrowStmt *throwStmt,
                                   ResultBuilderTarget target) {
    llvm_unreachable("Throw statements produce no value");
  }

#define UNHANDLED_RESULT_BUILDER_STMT(STMT) \
  NullablePtr<Stmt> \
  visit##STMT##Stmt(STMT##Stmt *stmt, ResultBuilderTarget target) { \
    llvm_unreachable("Function builders do not allow statement of kind " \
                     #STMT); \
  }

  UNHANDLED_RESULT_BUILDER_STMT(Return)
  UNHANDLED_RESULT_BUILDER_STMT(Yield)
  UNHANDLED_RESULT_BUILDER_STMT(Forget)
  UNHANDLED_RESULT_BUILDER_STMT(Guard)
  UNHANDLED_RESULT_BUILDER_STMT(While)
  UNHANDLED_RESULT_BUILDER_STMT(Defer)
  UNHANDLED_RESULT_BUILDER_STMT(DoCatch)
  UNHANDLED_RESULT_BUILDER_STMT(RepeatWhile)
  UNHANDLED_RESULT_BUILDER_STMT(Break)
  UNHANDLED_RESULT_BUILDER_STMT(Continue)
  UNHANDLED_RESULT_BUILDER_STMT(Fallthrough)
  UNHANDLED_RESULT_BUILDER_STMT(Fail)
  UNHANDLED_RESULT_BUILDER_STMT(PoundAssert)
#undef UNHANDLED_RESULT_BUILDER_STMT
};

} // end anonymous namespace

BraceStmt *swift::applyResultBuilderTransform(
    const Solution &solution,
    AppliedBuilderTransform applied,
    BraceStmt *body,
    DeclContext *dc,
    std::function<
        Optional<SolutionApplicationTarget> (SolutionApplicationTarget)>
          rewriteTarget) {
  BuilderClosureRewriter rewriter(solution, dc, applied, rewriteTarget);
  auto captured = rewriter.takeCapturedStmt(body);
  auto result = rewriter.visitBraceStmt(
      body, ResultBuilderTarget::forReturn(applied.returnExpr),
      ResultBuilderTarget::forAssign(captured.first, captured.second));
  if (!result)
    return nullptr;
  return cast<BraceStmt>(result.get());
}

Optional<BraceStmt *> TypeChecker::applyResultBuilderBodyTransform(
    FuncDecl *func, Type builderType,
    bool ClosuresInResultBuilderDontParticipateInInference) {
  // Pre-check the body: pre-check any expressions in it and look
  // for return statements.
  //
  // If we encountered an error or there was an explicit result type,
  // bail out and report that to the caller.
  auto &ctx = func->getASTContext();
  auto request =
      PreCheckResultBuilderRequest{{AnyFunctionRef(func),
                                      /*SuppressDiagnostics=*/false}};
  switch (evaluateOrDefault(ctx.evaluator, request,
                            ResultBuilderBodyPreCheck::Error)) {
  case ResultBuilderBodyPreCheck::Okay:
    // If the pre-check was okay, apply the result-builder transform.
    break;

  case ResultBuilderBodyPreCheck::Error:
    return nullptr;

  case ResultBuilderBodyPreCheck::HasReturnStmt: {
    // One or more explicit 'return' statements were encountered, which
    // disables the result builder transform. Warn when we do this.
    auto returnStmts = findReturnStatements(func);
    assert(!returnStmts.empty());

    ctx.Diags.diagnose(
        returnStmts.front()->getReturnLoc(),
        diag::result_builder_disabled_by_return_warn, builderType);

    // Note that one can remove the result builder attribute.
    auto attr = func->getAttachedResultBuilder();
    if (!attr) {
      if (auto accessor = dyn_cast<AccessorDecl>(func)) {
        attr = accessor->getStorage()->getAttachedResultBuilder();
      }
    }

    if (attr) {
      diagnoseAndRemoveAttr(func, attr, diag::result_builder_remove_attr);
    }

    // Note that one can remove all of the return statements.
    {
      auto diag = ctx.Diags.diagnose(
          returnStmts.front()->getReturnLoc(),
          diag::result_builder_remove_returns);
      for (auto returnStmt : returnStmts) {
        diag.fixItRemove(returnStmt->getReturnLoc());
      }
    }

    return None;
  }
  }

  ConstraintSystemOptions options = ConstraintSystemFlags::AllowFixes;
  if (ClosuresInResultBuilderDontParticipateInInference) {
    options |= ConstraintSystemFlags::
        ClosuresInResultBuildersDontParticipateInInference;
  }
  auto resultInterfaceTy = func->getResultInterfaceType();
  auto resultContextType = func->mapTypeIntoContext(resultInterfaceTy);

  // Determine whether we're inferring the underlying type for the opaque
  // result type of this function.
  ConstraintKind resultConstraintKind = ConstraintKind::Conversion;
  if (auto opaque = resultContextType->getAs<OpaqueTypeArchetypeType>()) {
    if (opaque->getDecl()->isOpaqueReturnTypeOfFunction(func)) {
      resultConstraintKind = ConstraintKind::Equal;
    }
  }

  // Build a constraint system in which we can check the body of the function.
  ConstraintSystem cs(func, options);

  if (cs.isDebugMode()) {
    auto &log = llvm::errs();

    log << "--- Applying result builder to function ---\n";
    func->dump(log);
    log << '\n';
  }

  // Map type parameters into context. We don't want type
  // parameters to appear in the result builder type, because
  // the result builder type will only be used inside the body
  // of this decl; it's not part of the interface type.
  builderType = func->mapTypeIntoContext(builderType);

  if (auto result = cs.matchResultBuilder(
          func, builderType, resultContextType, resultConstraintKind,
          /*contextualType=*/Type(),
          cs.getConstraintLocator(func->getBody()))) {
    if (result->isFailure())
      return nullptr;
  }

  // Solve the constraint system.
  if (cs.getASTContext().CompletionCallback) {
    SmallVector<Solution, 4> solutions;
    cs.solveForCodeCompletion(solutions);

    CompletionContextFinder analyzer(func, func->getDeclContext());
    if (analyzer.hasCompletion()) {
      filterSolutionsForCodeCompletion(solutions, analyzer);
      for (const auto &solution : solutions) {
        cs.getASTContext().CompletionCallback->sawSolution(solution);
      }
    }
    return nullptr;
  }

  SmallVector<Solution, 4> solutions;
  bool solvingFailed = cs.solve(solutions);

  auto reportSolutionsToSolutionCallback = [&](const SolutionResult &result) {
    if (!cs.getASTContext().SolutionCallback) {
      return;
    }
    switch (result.getKind()) {
    case SolutionResult::Success:
      cs.getASTContext().SolutionCallback->sawSolution(result.getSolution());
      break;
    case SolutionResult::Ambiguous:
      for (auto &solution : result.getAmbiguousSolutions()) {
        cs.getASTContext().SolutionCallback->sawSolution(solution);
      }
      break;
    default:
      break;
    }
  };

  if (solvingFailed || solutions.size() != 1) {
    // Try to fix the system or provide a decent diagnostic.
    auto salvagedResult = cs.salvage();
    switch (salvagedResult.getKind()) {
    case SolutionResult::Kind::Success:
      solutions.clear();
      solutions.push_back(std::move(salvagedResult).takeSolution());
      break;

    case SolutionResult::Kind::Error:
    case SolutionResult::Kind::Ambiguous:
      reportSolutionsToSolutionCallback(salvagedResult);
      return nullptr;

    case SolutionResult::Kind::UndiagnosedError:
      reportSolutionsToSolutionCallback(salvagedResult);
      cs.diagnoseFailureFor(SolutionApplicationTarget(func));
      salvagedResult.markAsDiagnosed();
      return nullptr;

    case SolutionResult::Kind::TooComplex:
      reportSolutionsToSolutionCallback(salvagedResult);
      func->diagnose(diag::expression_too_complex)
        .highlight(func->getBodySourceRange());
      salvagedResult.markAsDiagnosed();
      return nullptr;
    }

    // The system was salvaged; continue on as if nothing happened.
  }

  if (cs.isDebugMode()) {
    auto indent = cs.solverState ? cs.solverState->getCurrentIndent() : 0;
    auto &log = llvm::errs().indent(indent);
    log << "--- Applying Solution ---\n";
    solutions.front().dump(log, indent);
    log << '\n';
  }

  if (cs.getASTContext().SolutionCallback) {
    for (auto &solution : solutions) {
      cs.getASTContext().SolutionCallback->sawSolution(solution);
    }
    return nullptr;
  }

  // FIXME: Shouldn't need to do this.
  cs.applySolution(solutions.front());

  // Apply the solution to the function body.
  if (auto result = cs.applySolution(
          solutions.front(),
          SolutionApplicationTarget(func))) {
    performSyntacticDiagnosticsForTarget(*result, /*isExprStmt*/ false);
    auto *body = result->getFunctionBody();

    if (cs.isDebugMode()) {
      auto indent = cs.solverState ? cs.solverState->getCurrentIndent() : 0;
      auto &log = llvm::errs().indent(indent);
      log << "--- Type-checked function body ---\n";
      body->dump(log);
      log << '\n';
    }

    return body;
  }

  return nullptr;
}

Optional<ConstraintSystem::TypeMatchResult>
ConstraintSystem::matchResultBuilder(AnyFunctionRef fn, Type builderType,
                                     Type bodyResultType,
                                     ConstraintKind bodyResultConstraintKind,
                                     Type contextualType,
                                     ConstraintLocatorBuilder locator) {
  builderType = simplifyType(builderType);
  auto builder = builderType->getAnyNominal();
  assert(builder && "Bad result builder type");
  assert(builder->getAttrs().hasAttribute<ResultBuilderAttr>());
  assert(!builderType->hasTypeParameter());

  if (InvalidResultBuilderBodies.count(fn)) {
    (void)recordFix(IgnoreInvalidResultBuilderBody::create(
        *this, getConstraintLocator(fn.getAbstractClosureExpr())));
    return getTypeMatchSuccess();
  }

  // Pre-check the body: pre-check any expressions in it and look
  // for return statements.
  auto request =
      PreCheckResultBuilderRequest{{fn, /*SuppressDiagnostics=*/false}};
  switch (evaluateOrDefault(getASTContext().evaluator, request,
                            ResultBuilderBodyPreCheck::Error)) {
  case ResultBuilderBodyPreCheck::Okay:
    // If the pre-check was okay, apply the result-builder transform.
    break;

  case ResultBuilderBodyPreCheck::Error: {
    InvalidResultBuilderBodies.insert(fn);

    if (!shouldAttemptFixes())
      return getTypeMatchFailure(locator);

    if (recordFix(IgnoreInvalidResultBuilderBody::create(
            *this, getConstraintLocator(fn.getAbstractClosureExpr()))))
      return getTypeMatchFailure(locator);

    return getTypeMatchSuccess();
  }

  case ResultBuilderBodyPreCheck::HasReturnStmt:
    // Diagnostic mode means that solver couldn't reach any viable
    // solution, so let's diagnose presence of a `return` statement
    // in the closure body.
    if (shouldAttemptFixes()) {
      if (recordFix(IgnoreResultBuilderWithReturnStmts::create(
              *this, builderType,
              getConstraintLocator(fn.getAbstractClosureExpr()))))
        return getTypeMatchFailure(locator);

      return getTypeMatchSuccess();
    }

    // If the body has a return statement, suppress the transform but
    // continue solving the constraint system.
    return None;
  }

  auto disableASTTransform = [&](NominalTypeDecl *builder) { return false; };

  if (!disableASTTransform(builder)) {
    auto transformedBody = getBuilderTransformedBody(fn, builder);
    // If this builder transform has not yet been applied to this function,
    // let's do it and cache the result.
    if (!transformedBody) {
      ResultBuilderTransform transform(*this, fn.getAsDeclContext(),
                                       builderType, bodyResultType);
      auto *body = transform.apply(fn.getBody());

      if (auto unsupported = transform.getUnsupportedElement()) {
        assert(!body);

        // If we aren't supposed to attempt fixes, fail.
        if (!shouldAttemptFixes()) {
          return getTypeMatchFailure(locator);
        }

        // If we're solving for code completion and the body contains the code
        // completion location, skipping it won't get us to a useful solution so
        // just bail.
        if (isForCodeCompletion() &&
            containsIDEInspectionTarget(fn.getBody())) {
          return getTypeMatchFailure(locator);
        }

        // Record the first unhandled construct as a fix.
        if (recordFix(
                SkipUnhandledConstructInResultBuilder::create(
                    *this, unsupported, builder, getConstraintLocator(locator)),
                /*impact=*/100)) {
          return getTypeMatchFailure(locator);
        }

        if (auto *closure =
                getAsExpr<ClosureExpr>(fn.getAbstractClosureExpr())) {
          auto closureTy = getClosureType(closure);
          simplifyType(closureTy).visit([&](Type componentTy) {
            if (auto *typeVar = componentTy->getAs<TypeVariableType>()) {
              assignFixedType(typeVar,
                              PlaceholderType::get(getASTContext(), typeVar));
            }
          });
        }

        return getTypeMatchSuccess();
      }

      transformedBody = std::make_pair(transform.getBuilderSelf(), body);
      // Record the transformation so it could be re-used if needed.
      setBuilderTransformedBody(fn, builder, transformedBody->first,
                                transformedBody->second);
    }

    // Set the type of `$__builderSelf` variable before constraint generation.
    setType(transformedBody->first, MetatypeType::get(builderType));

    if (isDebugMode()) {
      auto &log = llvm::errs();
      auto indent = solverState ? solverState->getCurrentIndent() : 0;
      log.indent(indent) << "------- Transformed Body -------\n";
      transformedBody->second->dump(log, &getASTContext(), indent);
      log << '\n';
    }

    AppliedBuilderTransform transformInfo;

    transformInfo.builderType = builderType;
    transformInfo.bodyResultType = bodyResultType;
    transformInfo.contextualType = contextualType;
    transformInfo.transformedBody = transformedBody->second;

    // Record the transformation.
    assert(
        std::find_if(
            resultBuilderTransformed.begin(), resultBuilderTransformed.end(),
            [&](const std::pair<AnyFunctionRef, AppliedBuilderTransform> &elt) {
              return elt.first == fn;
            }) == resultBuilderTransformed.end() &&
        "already transformed this body along this path!?!");
    resultBuilderTransformed.insert(
        std::make_pair(fn, std::move(transformInfo)));

    if (generateConstraints(fn, transformInfo.transformedBody.get()))
      return getTypeMatchFailure(locator);

    return getTypeMatchSuccess();
  }

  // Check the form of this body to see if we can apply the
  // result-builder translation at all.
  auto dc = fn.getAsDeclContext();
  {
    // Check whether we can apply this specific result builder.
    BuilderClosureVisitor visitor(getASTContext(), nullptr, dc, builderType,
                                  bodyResultType);

    // If we saw a control-flow statement or declaration that the builder
    // cannot handle, we don't have a well-formed result builder application.
    if (auto unhandledNode = visitor.check(fn.getBody())) {
      // If we aren't supposed to attempt fixes, fail.
      if (!shouldAttemptFixes()) {
        return getTypeMatchFailure(locator);
      }

      // If we're solving for code completion and the body contains the code
      // completion location, skipping it won't get us to a useful solution so
      // just bail.
      if (isForCodeCompletion() && containsIDEInspectionTarget(fn.getBody())) {
        return getTypeMatchFailure(locator);
      }

      // Record the first unhandled construct as a fix.
      if (recordFix(
              SkipUnhandledConstructInResultBuilder::create(
                *this, unhandledNode, builder,
                getConstraintLocator(locator)))) {
        return getTypeMatchFailure(locator);
      }
    }
  }

  BuilderClosureVisitor visitor(getASTContext(), this, dc, builderType,
                                bodyResultType);

  Optional<AppliedBuilderTransform> applied = None;
  {
    DiagnosticTransaction transaction(dc->getASTContext().Diags);

    applied = visitor.apply(fn.getBody());
    if (!applied)
      return getTypeMatchFailure(locator);

    if (transaction.hasErrors()) {
      InvalidResultBuilderBodies.insert(fn);

      if (recordFix(IgnoreInvalidResultBuilderBody::create(
              *this, getConstraintLocator(fn.getAbstractClosureExpr()))))
        return getTypeMatchFailure(locator);

      return getTypeMatchSuccess();
    }
  }

  Type transformedType = getType(applied->returnExpr);
  assert(transformedType && "Missing type");

  // Record the transformation.
  assert(std::find_if(
      resultBuilderTransformed.begin(),
      resultBuilderTransformed.end(),
      [&](const std::pair<AnyFunctionRef, AppliedBuilderTransform> &elt) {
        return elt.first == fn;
      }) == resultBuilderTransformed.end() &&
         "already transformed this body along this path!?!");
  resultBuilderTransformed.insert(std::make_pair(fn, std::move(*applied)));

  // If builder is applied to the closure expression then
  // `closure body` to `closure result` matching should
  // use special locator.
  if (auto *closure = fn.getAbstractClosureExpr()) {
    locator = getConstraintLocator(closure, ConstraintLocator::ClosureResult);
  } else {
    locator = getConstraintLocator(fn.getAbstractFunctionDecl(),
                                   ConstraintLocator::ResultBuilderBodyResult);
  }

  // Bind the body result type to the type of the transformed expression.
  addConstraint(bodyResultConstraintKind, transformedType,
                openOpaqueType(bodyResultType, CTP_ReturnStmt, locator),
                locator);
  return getTypeMatchSuccess();
}

namespace {

/// Pre-check all the expressions in the body.
class PreCheckResultBuilderApplication : public ASTWalker {
  AnyFunctionRef Fn;
  bool SkipPrecheck = false;
  bool SuppressDiagnostics = false;
  std::vector<ReturnStmt *> ReturnStmts;
  bool HasError = false;

  bool hasReturnStmt() const { return !ReturnStmts.empty(); }

public:
  PreCheckResultBuilderApplication(AnyFunctionRef fn, bool skipPrecheck,
                                     bool suppressDiagnostics)
      : Fn(fn), SkipPrecheck(skipPrecheck),
        SuppressDiagnostics(suppressDiagnostics) {}

  const std::vector<ReturnStmt *> getReturnStmts() const { return ReturnStmts; }

  ResultBuilderBodyPreCheck run() {
    Stmt *oldBody = Fn.getBody();

    Stmt *newBody = oldBody->walk(*this);

    // If the walk was aborted, it was because we had a problem of some kind.
    assert((newBody == nullptr) == HasError &&
           "unexpected short-circuit while walking body");
    if (HasError)
      return ResultBuilderBodyPreCheck::Error;

    assert(oldBody == newBody && "pre-check walk wasn't in-place?");

    if (hasReturnStmt())
      return ResultBuilderBodyPreCheck::HasReturnStmt;

    return ResultBuilderBodyPreCheck::Okay;
  }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Arguments;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (SkipPrecheck)
      return Action::SkipChildren(E);

    // Pre-check the expression.  If this fails, abort the walk immediately.
    // Otherwise, replace the expression with the result of pre-checking.
    // In either case, don't recurse into the expression.
    {
      auto *DC = Fn.getAsDeclContext();
      auto &diagEngine = DC->getASTContext().Diags;

      // Suppress any diagnostics which could be produced by this expression.
      DiagnosticTransaction transaction(diagEngine);

      auto preCheckOptions = ConstraintSystem::PreCheckOptions();
      preCheckOptions |=
          ConstraintSystem::PreCheckFlags::ReplaceInvalidRefsWithErrors;
      HasError |= ConstraintSystem::preCheckExpression(E, DC, preCheckOptions);

      HasError |= transaction.hasErrors();

      if (!HasError)
        HasError |= containsErrorExpr(E);

      if (SuppressDiagnostics)
        transaction.abort();

      if (HasError)
        return Action::Stop();

      return Action::SkipChildren(E);
    }
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    // If we see a return statement, note it..
    if (auto returnStmt = dyn_cast<ReturnStmt>(S)) {
      if (!returnStmt->isImplicit()) {
        ReturnStmts.push_back(returnStmt);
        return Action::SkipChildren(S);
      }
    }

    // Otherwise, recurse into the statement normally.
    return Action::Continue(S);
  }

  /// Check whether given expression (including single-statement
  /// closures) contains `ErrorExpr` as one of its sub-expressions.
  bool containsErrorExpr(Expr *expr) {
    bool hasError = false;

    expr->forEachChildExpr([&](Expr *expr) -> Expr * {
      hasError |= isa<ErrorExpr>(expr);
      if (hasError)
        return nullptr;

      if (auto *closure = dyn_cast<ClosureExpr>(expr)) {
        if (closure->hasSingleExpressionBody()) {
          hasError |= containsErrorExpr(closure->getSingleExpressionBody());
          return hasError ? nullptr : expr;
        }
      }

      return expr;
    });

    return hasError;
  }

  /// Ignore patterns.
  PreWalkResult<Pattern *> walkToPatternPre(Pattern *pat) override {
    return Action::SkipChildren(pat);
  }
};

}

ResultBuilderBodyPreCheck PreCheckResultBuilderRequest::evaluate(
    Evaluator &evaluator, PreCheckResultBuilderDescriptor owner) const {
  return PreCheckResultBuilderApplication(
             owner.Fn, /*skipPrecheck=*/false,
             /*suppressDiagnostics=*/owner.SuppressDiagnostics)
      .run();
}

std::vector<ReturnStmt *> TypeChecker::findReturnStatements(AnyFunctionRef fn) {
  PreCheckResultBuilderApplication precheck(fn, /*skipPreCheck=*/true,
                                              /*SuppressDiagnostics=*/true);
  (void)precheck.run();
  return precheck.getReturnStmts();
}

ResultBuilderOpSupport TypeChecker::checkBuilderOpSupport(
    Type builderType, DeclContext *dc, Identifier fnName,
    ArrayRef<Identifier> argLabels, SmallVectorImpl<ValueDecl *> *allResults) {

  auto isUnavailable = [&](Decl *D) -> bool {
    if (AvailableAttr::isUnavailable(D))
      return true;

    auto loc = extractNearestSourceLoc(dc);
    auto context = ExportContext::forFunctionBody(dc, loc);
    return TypeChecker::checkDeclarationAvailability(D, context).has_value();
  };

  bool foundMatch = false;
  bool foundUnavailable = false;

  SmallVector<ValueDecl *, 4> foundDecls;
  dc->lookupQualified(
      builderType, DeclNameRef(fnName),
      NL_QualifiedDefault | NL_ProtocolMembers, foundDecls);
  for (auto decl : foundDecls) {
    if (auto func = dyn_cast<FuncDecl>(decl)) {
      // Function must be static.
      if (!func->isStatic())
        continue;

      // Function must have the right argument labels, if provided.
      if (!argLabels.empty()) {
        auto funcLabels = func->getName().getArgumentNames();
        if (argLabels.size() > funcLabels.size() ||
            funcLabels.slice(0, argLabels.size()) != argLabels)
          continue;
      }

      // Check if the the candidate has a suitable availability for the
      // calling context.
      if (isUnavailable(func)) {
        foundUnavailable = true;
        continue;
      }
      foundMatch = true;
      break;
    }
  }

  if (allResults)
    allResults->append(foundDecls.begin(), foundDecls.end());

  if (!foundMatch) {
    return foundUnavailable ? ResultBuilderOpSupport::Unavailable
                            : ResultBuilderOpSupport::Unsupported;
  }
  // If the builder type itself isn't available, don't consider any builder
  // method available.
  if (auto *D = builderType->getAnyNominal()) {
    if (isUnavailable(D))
      return ResultBuilderOpSupport::Unavailable;
  }
  return ResultBuilderOpSupport::Supported;
}

bool TypeChecker::typeSupportsBuilderOp(
    Type builderType, DeclContext *dc, Identifier fnName,
    ArrayRef<Identifier> argLabels, SmallVectorImpl<ValueDecl *> *allResults) {
  return checkBuilderOpSupport(builderType, dc, fnName, argLabels, allResults)
      .isSupported(/*requireAvailable*/ false);
}

Type swift::inferResultBuilderComponentType(NominalTypeDecl *builder) {
  Type componentType;

  SmallVector<ValueDecl *, 4> potentialMatches;
  ASTContext &ctx = builder->getASTContext();
  bool supportsBuildBlock = TypeChecker::typeSupportsBuilderOp(
      builder->getDeclaredInterfaceType(), builder, ctx.Id_buildBlock,
      /*argLabels=*/{}, &potentialMatches);
  if (supportsBuildBlock) {
    for (auto decl : potentialMatches) {
      auto func = dyn_cast<FuncDecl>(decl);
      if (!func || !func->isStatic())
        continue;

      // If we haven't seen a component type before, gather it.
      if (!componentType) {
        componentType = func->getResultInterfaceType();
        continue;
      }

      // If there are inconsistent component types, bail out.
      if (!componentType->isEqual(func->getResultInterfaceType())) {
        componentType = Type();
        break;
      }
    }
  }

  return componentType;
}

std::tuple<SourceLoc, std::string, Type>
swift::determineResultBuilderBuildFixItInfo(NominalTypeDecl *builder) {
  SourceLoc buildInsertionLoc = builder->getBraces().Start;
  std::string stubIndent;
  Type componentType;

  if (buildInsertionLoc.isInvalid())
    return std::make_tuple(buildInsertionLoc, stubIndent, componentType);

  ASTContext &ctx = builder->getASTContext();
  buildInsertionLoc = Lexer::getLocForEndOfToken(
      ctx.SourceMgr, buildInsertionLoc);

  StringRef extraIndent;
  StringRef currentIndent = Lexer::getIndentationForLine(
      ctx.SourceMgr, buildInsertionLoc, &extraIndent);
  stubIndent = (currentIndent + extraIndent).str();

  componentType = inferResultBuilderComponentType(builder);
  return std::make_tuple(buildInsertionLoc, stubIndent, componentType);
}

void swift::printResultBuilderBuildFunction(
      NominalTypeDecl *builder, Type componentType,
      ResultBuilderBuildFunction function,
      Optional<std::string> stubIndent, llvm::raw_ostream &out) {
  // Render the component type into a string.
  std::string componentTypeString;
  if (componentType)
    componentTypeString = componentType.getString();
  else
    componentTypeString = "<#Component#>";

  // Render the code.
  std::string stubIndentStr = stubIndent.value_or(std::string());
  ExtraIndentStreamPrinter printer(out, stubIndentStr);

  // If we're supposed to provide a full stub, add a newline and the introducer
  // keywords.
  if (stubIndent) {
    printer.printNewline();

    if (builder->getFormalAccess() >= AccessLevel::Public)
      printer << "public ";

    printer << "static func ";
  }

  bool printedResult = false;
  switch (function) {
  case ResultBuilderBuildFunction::BuildBlock:
    printer << "buildBlock(_ components: " << componentTypeString << "...)";
    break;

  case ResultBuilderBuildFunction::BuildExpression:
    printer << "buildExpression(_ expression: <#Expression#>)";
    break;

  case ResultBuilderBuildFunction::BuildOptional:
    printer << "buildOptional(_ component: " << componentTypeString << "?)";
    break;

  case ResultBuilderBuildFunction::BuildEitherFirst:
    printer << "buildEither(first component: " << componentTypeString << ")";
    break;

  case ResultBuilderBuildFunction::BuildEitherSecond:
    printer << "buildEither(second component: " << componentTypeString << ")";
    break;

  case ResultBuilderBuildFunction::BuildArray:
    printer << "buildArray(_ components: [" << componentTypeString << "])";
    break;

  case ResultBuilderBuildFunction::BuildLimitedAvailability:
    printer << "buildLimitedAvailability(_ component: " << componentTypeString
            << ")";
    break;

  case ResultBuilderBuildFunction::BuildFinalResult:
    printer << "buildFinalResult(_ component: " << componentTypeString
            << ") -> <#Result#>";
    printedResult = true;
    break;
  case ResultBuilderBuildFunction::BuildPartialBlockFirst:
    printer << "buildPartialBlock(first: " << componentTypeString << ")";
    break;
  case ResultBuilderBuildFunction::BuildPartialBlockAccumulated:
    printer << "buildPartialBlock(accumulated: " << componentTypeString
            << ", next: " << componentTypeString << ")";
    break;
  }

  if (!printedResult)
    printer << " -> " << componentTypeString;

  if (stubIndent) {
    printer << " {";
    printer.printNewline();
    printer << "  <#code#>";
    printer.printNewline();
    printer << "}";
  }
}

ResultBuilder::ResultBuilder(ConstraintSystem *CS, DeclContext *DC,
                             Type builderType)
    : DC(DC), BuilderType(CS ? CS->simplifyType(builderType) : builderType) {
  auto &ctx = DC->getASTContext();
  // Use buildOptional(_:) if available, otherwise fall back to buildIf
  // when available.
  BuildOptionalId =
      (supports(ctx.Id_buildOptional) || !supports(ctx.Id_buildIf))
          ? ctx.Id_buildOptional
          : ctx.Id_buildIf;

  if (CS) {
    BuilderSelf = new (ctx) VarDecl(
        /*isStatic=*/false, VarDecl::Introducer::Let,
        /*nameLoc=*/SourceLoc(), ctx.Id_builderSelf, DC);
    BuilderSelf->setImplicit();
    CS->setType(BuilderSelf, MetatypeType::get(BuilderType));
  }
}

bool ResultBuilder::supportsBuildPartialBlock(bool checkAvailability) {
  auto &ctx = DC->getASTContext();
  return supports(ctx.Id_buildPartialBlock, {ctx.Id_first},
                  checkAvailability) &&
         supports(ctx.Id_buildPartialBlock, {ctx.Id_accumulated, ctx.Id_next},
                  checkAvailability);
}

bool ResultBuilder::canUseBuildPartialBlock() {
  // If buildPartialBlock doesn't exist at all, we can't use it.
  if (!supportsBuildPartialBlock(/*checkAvailability*/ false))
    return false;

  // If buildPartialBlock exists and is available, use it.
  if (supportsBuildPartialBlock(/*checkAvailability*/ true))
    return true;

  // We have buildPartialBlock, but it is unavailable. We can however still
  // use it if buildBlock is also unavailable.
  auto &ctx = DC->getASTContext();
  return supports(ctx.Id_buildBlock) &&
         !supports(ctx.Id_buildBlock, /*labels*/ {},
                   /*checkAvailability*/ true);
}

bool ResultBuilder::supports(Identifier fnBaseName,
                             ArrayRef<Identifier> argLabels,
                             bool checkAvailability) {
  DeclName name(DC->getASTContext(), fnBaseName, argLabels);
  auto known = SupportedOps.find(name);
  if (known != SupportedOps.end())
    return known->second.isSupported(checkAvailability);

  auto support = TypeChecker::checkBuilderOpSupport(
      BuilderType, DC, fnBaseName, argLabels, /*allResults*/ {});
  SupportedOps.insert({name, support});
  return support.isSupported(checkAvailability);
}

Expr *ResultBuilder::buildCall(SourceLoc loc, Identifier fnName,
                               ArrayRef<Expr *> argExprs,
                               ArrayRef<Identifier> argLabels) const {
  assert(BuilderSelf);

  auto &ctx = DC->getASTContext();

  SmallVector<Argument, 4> args;
  for (auto i : indices(argExprs)) {
    auto *expr = argExprs[i];
    auto label = argLabels.empty() ? Identifier() : argLabels[i];
    auto labelLoc = argLabels.empty() ? SourceLoc() : expr->getStartLoc();
    args.emplace_back(labelLoc, label, expr);
  }

  auto *baseExpr = new (ctx) DeclRefExpr({BuilderSelf}, DeclNameLoc(loc),
                                         /*isImplicit=*/true);

  auto memberRef = new (ctx)
      UnresolvedDotExpr(baseExpr, loc, DeclNameRef(fnName), DeclNameLoc(loc),
                        /*implicit=*/true);
  memberRef->setFunctionRefKind(FunctionRefKind::SingleApply);

  auto openLoc = args.empty() ? loc : argExprs.front()->getStartLoc();
  auto closeLoc = args.empty() ? loc : argExprs.back()->getEndLoc();

  auto *argList = ArgumentList::createImplicit(ctx, openLoc, args, closeLoc);
  return CallExpr::createImplicit(ctx, memberRef, argList);
}

VarDecl *ResultBuilder::buildVar(SourceLoc loc) {
  auto &ctx = DC->getASTContext();
  // Create the implicit variable.
  Identifier name =
      ctx.getIdentifier(("$__builder" + Twine(VarCounter++)).str());
  auto var = new (ctx)
      VarDecl(/*isStatic=*/false, VarDecl::Introducer::Var, loc, name, DC);
  var->setImplicit();
  return var;
}

DeclRefExpr *ResultBuilder::buildVarRef(VarDecl *var, SourceLoc loc) {
  return new (DC->getASTContext())
      DeclRefExpr(var, DeclNameLoc(loc), /*Implicit=*/true);
}
