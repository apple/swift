//===--- BuilderTransform.cpp - Function-builder transformation -----------===//
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
// This file implements routines associated with the function-builder
// transformation.
//
//===----------------------------------------------------------------------===//

#include "ConstraintSystem.h"
#include "TypeChecker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParameterList.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include <iterator>
#include <map>
#include <memory>
#include <utility>
#include <tuple>

using namespace swift;
using namespace constraints;

namespace {

/// Constraint generation for the application of a function builder to
/// the body of a closure or function, which is also used to classify
/// the body to determine whether it can be used with a particular builder.
class BuilderConstraintGenerator
    : private StmtVisitor<BuilderConstraintGenerator, VarDecl *> {

  friend StmtVisitor<BuilderConstraintGenerator, VarDecl *>;

  ConstraintSystem *cs;
  DeclContext *dc;
  ASTContext &ctx;
  Type builderType;
  NominalTypeDecl *builder = nullptr;
  llvm::SmallDenseMap<Identifier, bool> supportedOps;

  SkipUnhandledConstructInFunctionBuilder::UnhandledNode unhandledNode;

  /// Whether an error occurred during application of the builder,
  /// e.g., during constraint generation.
  bool hadError = false;

  /// Counter used to give unique names to the variables that are
  /// created implicitly.
  unsigned varCounter = 0;

  /// The record of what happened when we applied the builder transform.
  AppliedBuilderTransform applied;

  /// Produce a builder call to the given named function with the given
  /// arguments.
  Expr *buildCallIfWanted(SourceLoc loc,
                          Identifier fnName, ArrayRef<Expr *> args,
                          ArrayRef<Identifier> argLabels) {
    if (!cs)
      return nullptr;

    // FIXME: Setting a TypeLoc on this expression is necessary in order
    // to get diagnostics if something about this builder call fails,
    // e.g. if there isn't a matching overload for `buildBlock`.
    // But we can only do this if there isn't a type variable in the type.
    TypeLoc typeLoc;
    if (!builderType->hasTypeVariable()) {
      typeLoc = TypeLoc(new (ctx) FixedTypeRepr(builderType, loc), builderType);
    }

    auto typeExpr = new (ctx) TypeExpr(typeLoc);
    cs->setType(typeExpr, MetatypeType::get(builderType));
    cs->setType(&typeExpr->getTypeLoc(), builderType);

    SmallVector<SourceLoc, 4> argLabelLocs;
    for (auto i : indices(argLabels)) {
      argLabelLocs.push_back(args[i]->getStartLoc());
    }

    typeExpr->setImplicit();
    auto memberRef = new (ctx) UnresolvedDotExpr(
        typeExpr, loc, fnName, DeclNameLoc(loc), /*implicit=*/true);
    memberRef->setFunctionRefKind(FunctionRefKind::SingleApply);
    SourceLoc openLoc = args.empty() ? loc : args.front()->getStartLoc();
    SourceLoc closeLoc = args.empty() ? loc : args.back()->getEndLoc();
    Expr *result = CallExpr::create(ctx, memberRef, openLoc, args,
                                    argLabels, argLabelLocs, closeLoc,
                                    /*trailing closure*/ nullptr,
                                    /*implicit*/true);

    return result;
  }

  /// Check whether the builder supports the given operation.
  bool builderSupports(Identifier fnName,
                       ArrayRef<Identifier> argLabels = {}) {
    auto known = supportedOps.find(fnName);
    if (known != supportedOps.end()) {
      return known->second;
    }

    bool found = false;
    for (auto decl : builder->lookupDirect(fnName)) {
      if (auto func = dyn_cast<FuncDecl>(decl)) {
        // Function must be static.
        if (!func->isStatic())
          continue;

        // Function must have the right argument labels, if provided.
        if (!argLabels.empty()) {
          auto funcLabels = func->getFullName().getArgumentNames();
          if (argLabels.size() > funcLabels.size() ||
              funcLabels.slice(0, argLabels.size()) != argLabels)
            continue;
        }

        // Okay, it's a good-enough match.
        found = true;
        break;
      }
    }

    return supportedOps[fnName] = found;
  }

  /// Build an implicit variable in this context.
  VarDecl *buildVar(SourceLoc loc) {
    // Create the implicit variable.
    Identifier name = ctx.getIdentifier(
        ("$__builder" + Twine(varCounter++)).str());
    auto var = new (ctx) VarDecl(/*isStatic=*/false, VarDecl::Introducer::Var,
                                 /*isCaptureList=*/false, loc, name, dc);
    var->setImplicit();
    return var;
  }

  /// Capture the given expression into an implicitly-generated variable.
  VarDecl *captureExpr(Expr *expr, bool oneWay, Stmt* forStmt = nullptr) {
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
    auto var = buildVar(expr->getStartLoc());

    // Record the new variable and its corresponding expression & statement.
    if (forStmt)
      applied.capturedStmts.insert({forStmt, { var, { expr } }});
    else
      applied.capturedExprs.insert({origExpr, {var, expr}});

    cs->setType(var, cs->getType(expr));
    return var;
  }

  /// Build an implicit reference to the given variable.
  DeclRefExpr *buildVarRef(VarDecl *var, SourceLoc loc) {
    return new (ctx) DeclRefExpr(var, DeclNameLoc(loc), /*Implicit=*/true);
  }

public:
  BuilderConstraintGenerator(ASTContext &ctx, ConstraintSystem *cs,
                             DeclContext *dc, Type builderType)
      : cs(cs), dc(dc), ctx(ctx), builderType(builderType) {
    assert((cs || !builderType->hasTypeVariable()) &&
           "cannot handle builder type with type variables without "
           "constraint system");
    builder = builderType->getAnyNominal();
    applied.builderType = builderType;
  }

  /// Apply the builder transform to the given statement.
  Optional<AppliedBuilderTransform> apply(Stmt *stmt) {
    VarDecl *bodyVar = visit(stmt);
    if (!bodyVar)
      return None;

    applied.returnExpr = buildVarRef(bodyVar, stmt->getEndLoc());
    applied.returnExpr = cs->generateConstraints(applied.returnExpr, dc);
    if (!applied.returnExpr) {
      hadError = true;
      return None;
    }

    return std::move(applied);
  }

  /// Check whether the function builder can be applied to this statement.
  /// \returns the node that cannot be handled by this builder on failure.
  SkipUnhandledConstructInFunctionBuilder::UnhandledNode check(Stmt *stmt) {
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

  VarDecl *visitBraceStmt(BraceStmt *braceStmt) {
    SmallVector<Expr *, 4> expressions;
    auto addChild = [&](VarDecl *childVar) {
      if (!childVar)
        return;

      expressions.push_back(buildVarRef(childVar, braceStmt->getEndLoc()));
    };

    for (const auto &node : braceStmt->getElements()) {
      if (auto stmt = node.dyn_cast<Stmt *>()) {
        addChild(visit(stmt));
        continue;
      }

      if (auto decl = node.dyn_cast<Decl *>()) {
        // Just ignore #if; the chosen children should appear in the
        // surrounding context.  This isn't good for source tools but it
        // at least works.
        if (isa<IfConfigDecl>(decl))
          continue;

        if (!unhandledNode)
          unhandledNode = decl;

        continue;
      }

      auto expr = node.get<Expr *>();
      if (cs && builderSupports(ctx.Id_buildExpression)) {
        expr = buildCallIfWanted(expr->getLoc(), ctx.Id_buildExpression,
                                 { expr }, { Identifier() });
      }

      addChild(captureExpr(expr, /*oneWay=*/true));
    }

    if (!cs)
      return nullptr;

    // Call Builder.buildBlock(... args ...)
    auto call = buildCallIfWanted(braceStmt->getStartLoc(),
                                  ctx.Id_buildBlock, expressions,
                                  /*argLabels=*/{ });
    if (!call)
      return nullptr;

    return captureExpr(call, /*oneWay=*/true, braceStmt);
  }

  VarDecl *visitReturnStmt(ReturnStmt *stmt) {
    // Allow implicit returns due to 'return' elision.
    if (!stmt->isImplicit() || !stmt->hasResult()) {
      if (!unhandledNode)
        unhandledNode = stmt;
      return nullptr;
    }

    return captureExpr(stmt->getResult(), /*oneWay=*/true, stmt);
  }

  VarDecl *visitDoStmt(DoStmt *doStmt) {
    if (!builderSupports(ctx.Id_buildDo)) {
      if (!unhandledNode)
        unhandledNode = doStmt;
      return nullptr;
    }

    auto childVar = visit(doStmt->getBody());
    if (!childVar)
      return nullptr;

    auto childRef = buildVarRef(childVar, doStmt->getEndLoc());
    auto call = buildCallIfWanted(doStmt->getStartLoc(), ctx.Id_buildDo,
                                  childRef, /*argLabels=*/{ });
    if (!call)
      return nullptr;

    return captureExpr(call, /*oneWay=*/true, doStmt);
  }

  CONTROL_FLOW_STMT(Yield)
  CONTROL_FLOW_STMT(Defer)

  static Expr *getTrivialBooleanCondition(StmtCondition condition) {
    if (condition.size() != 1)
      return nullptr;

    return condition.front().getBooleanOrNull();
  }

  static bool isBuildableIfChainRecursive(IfStmt *ifStmt,
                                          unsigned &numPayloads,
                                          bool &isOptional) {
    // The conditional must be trivial.
    if (!getTrivialBooleanCondition(ifStmt->getCond()))
      return false;

    // The 'then' clause contributes a payload.
    numPayloads++;

    // If there's an 'else' clause, it contributes payloads:
    if (auto elseStmt = ifStmt->getElseStmt()) {
      // If it's 'else if', it contributes payloads recursively.
      if (auto elseIfStmt = dyn_cast<IfStmt>(elseStmt)) {
        return isBuildableIfChainRecursive(elseIfStmt, numPayloads,
                                           isOptional);
      // Otherwise it's just the one.
      } else {
        numPayloads++;
      }

    // If not, the chain result is at least optional.
    } else {
      isOptional = true;
    }

    return true;
  }

  bool isBuildableIfChain(IfStmt *ifStmt, unsigned &numPayloads,
                          bool &isOptional) {
    if (!isBuildableIfChainRecursive(ifStmt, numPayloads, isOptional))
      return false;

    // If there's a missing 'else', we need 'buildIf' to exist.
    if (isOptional && !builderSupports(ctx.Id_buildIf))
      return false;

    // If there are multiple clauses, we need 'buildEither(first:)' and
    // 'buildEither(second:)' to both exist.
    if (numPayloads > 1) {
      if (!builderSupports(ctx.Id_buildEither, {ctx.Id_first}) ||
          !builderSupports(ctx.Id_buildEither, {ctx.Id_second}))
        return false;
    }

    return true;
  }

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

    // Prepare the `then` operand by wrapping it to produce a chain result.
    Expr *thenExpr = buildWrappedChainPayload(
        buildVarRef(thenVar, ifStmt->getThenStmt()->getEndLoc()),
        payloadIndex, numPayloads, isOptional);

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
      elseExpr = buildVarRef(*elseChainVar, ifStmt->getEndLoc());
      elseLoc = ifStmt->getElseLoc();

    // - Otherwise, wrap it to produce a chain result.
    } else {
      elseLoc = ifStmt->getElseLoc();
      elseExpr = buildWrappedChainPayload(
          buildVarRef(*elseChainVar, ifStmt->getEndLoc()),
          payloadIndex + 1, numPayloads, isOptional);
    }

    // Generate constraints for the various subexpressions.
    auto condExpr = getTrivialBooleanCondition(ifStmt->getCond());
    assert(condExpr && "Cannot get here without a trivial Boolean condition");
    condExpr = cs->generateConstraints(condExpr, dc);
    if (!condExpr) {
      hadError = true;
      return nullptr;
    }

    // Condition must convert to Bool.
    // FIXME: This should be folded into constraint generation for conditions.
    auto boolDecl = ctx.getBoolDecl();
    if (!boolDecl) {
      hadError = true;
      return nullptr;
    }
    cs->addConstraint(ConstraintKind::Conversion,
                      cs->getType(condExpr),
                      boolDecl->getDeclaredType(),
                      cs->getConstraintLocator(condExpr));

    // The operand should have optional type if we had optional results,
    // so we just need to call `buildIf` now, since we're at the top level.
    if (isOptional && isTopLevel) {
      thenExpr = buildCallIfWanted(ifStmt->getEndLoc(), ctx.Id_buildIf,
                                   thenExpr,  /*argLabels=*/{ });
      elseExpr = buildCallIfWanted(ifStmt->getEndLoc(), ctx.Id_buildIf,
                                   elseExpr,  /*argLabels=*/{ });
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

    // FIXME: Need a locator for the "if" statement.
    Type resultType = cs->addJoinConstraint(nullptr,
        {
          { cs->getType(thenExpr), cs->getConstraintLocator(thenExpr) },
          { cs->getType(elseExpr), cs->getConstraintLocator(elseExpr) }
        });
    if (!resultType) {
      hadError = true;
      return nullptr;
    }

    // Create a variable to capture the result of this expression.
    auto ifVar = buildVar(ifStmt->getStartLoc());
    cs->setType(ifVar, resultType);
    applied.capturedStmts.insert({ifStmt, { ifVar, { thenExpr, elseExpr }}});
    return ifVar;
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
    for (auto path = payloadIndex, maxPath = numPayloads - 1;
         maxPath != 0; path >>= 1, maxPath >>= 1) {
      // Skip making Eithers on the rightmost path where they aren't required.
      // This isn't just an optimization: adding spurious Eithers could
      // leave us with unresolvable type variables if `buildEither` has
      // a signature like:
      //    static func buildEither<T,U>(first value: T) -> Either<T,U>
      // which relies on unification to work.
      if (path == maxPath && !(maxPath & 1)) continue;

      bool isSecond = (path & 1);
      operand = buildCallIfWanted(operand->getStartLoc(),
                                  ctx.Id_buildEither, operand,
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
        optionalTypeExpr, loc, ctx.getIdentifier("some"),
        DeclNameLoc(loc), /*implicit=*/true);
    return CallExpr::createImplicit(ctx, someRef, arg, { });
  }

  Expr *buildNoneExpr(SourceLoc endLoc) {
    auto optionalDecl = ctx.getOptionalDecl();
    auto optionalType = optionalDecl->getDeclaredType();

    auto optionalTypeExpr =
      TypeExpr::createImplicitHack(endLoc, optionalType, ctx);
    return new (ctx) UnresolvedDotExpr(
        optionalTypeExpr, endLoc, ctx.getIdentifier("none"),
        DeclNameLoc(endLoc), /*implicit=*/true);
  }

  CONTROL_FLOW_STMT(Guard)
  CONTROL_FLOW_STMT(While)
  CONTROL_FLOW_STMT(DoCatch)
  CONTROL_FLOW_STMT(RepeatWhile)
  CONTROL_FLOW_STMT(ForEach)
  CONTROL_FLOW_STMT(Switch)
  CONTROL_FLOW_STMT(Case)
  CONTROL_FLOW_STMT(Catch)
  CONTROL_FLOW_STMT(Break)
  CONTROL_FLOW_STMT(Continue)
  CONTROL_FLOW_STMT(Fallthrough)
  CONTROL_FLOW_STMT(Fail)
  CONTROL_FLOW_STMT(Throw)
  CONTROL_FLOW_STMT(PoundAssert)

#undef CONTROL_FLOW_STMT
};

/// Describes the target into which the result of a particular statement in
/// a closure involving a function builder should be written.
struct FunctionBuilderTarget {
  enum Kind {
    /// The resulting value is returned from the closure.
    ReturnValue,
    /// The temporary variable into which the result should be assigned.
    TemporaryVar,
  } kind;

  /// Captured variable information.
  std::pair<VarDecl *, llvm::TinyPtrVector<Expr *>> captured;

  static FunctionBuilderTarget forReturn(Expr *expr) {
    return FunctionBuilderTarget{ReturnValue, {nullptr, {expr}}};
  }

  static FunctionBuilderTarget forAssign(VarDecl *temporaryVar,
                                         llvm::TinyPtrVector<Expr *> exprs) {
    return FunctionBuilderTarget{TemporaryVar, {temporaryVar, exprs}};
  }
};

/// Handles the rewrite of the body of a closure to which a function builder
/// has been applied.
class BuilderClosureRewriter
    : public StmtVisitor<BuilderClosureRewriter, Stmt *, FunctionBuilderTarget> {
  ASTContext &ctx;
  const Solution &solution;
  DeclContext *dc;
  AppliedBuilderTransform builderTransform;
  std::function<Expr *(Expr *)> rewriteExpr;

  /// Retrieve the temporary variable that will be used to capture the
  /// value of the given expression.
  AppliedBuilderTransform::RecordedExpr takeCapturedExpr(Expr *expr) {
    auto found = builderTransform.capturedExprs.find(expr);
    assert(found != builderTransform.capturedExprs.end());

    // Set the type of the temporary variable.
    auto recorded = found->second;
    if (auto temporaryVar = recorded.temporaryVar) {
      Type type = solution.simplifyType(solution.getType(temporaryVar));
      temporaryVar->setInterfaceType(type);
    }

    // Erase the captured expression, so we're sure we never do this twice.
    builderTransform.capturedExprs.erase(found);
    return recorded;
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
    temporaryVar->setInterfaceType(type);

    // Take the expressions.
    auto exprs = std::move(found->second.second);

    // Erase the statement, so we're sure we never do this twice.
    builderTransform.capturedStmts.erase(found);
    return std::make_pair(temporaryVar, std::move(exprs));
  }

private:
  /// Build the statement or expression to initialize the target.
  ASTNode initializeTarget(FunctionBuilderTarget target) {
    assert(target.captured.second.size() == 1);
    auto capturedExpr = target.captured.second.front();
    auto finalCapturedExpr = rewriteExpr(capturedExpr);
    SourceLoc implicitLoc = capturedExpr->getEndLoc();
    switch (target.kind) {
    case FunctionBuilderTarget::ReturnValue: {
      // Return the expression.
      ConstraintSystem &cs = solution.getConstraintSystem();
      finalCapturedExpr = cs.addImplicitLoadExpr(finalCapturedExpr);
      return new (ctx) ReturnStmt(implicitLoc, finalCapturedExpr);
    }

    case FunctionBuilderTarget::TemporaryVar: {
      // Assign the expression into a variable.
      auto temporaryVar = target.captured.first;
      auto declRef = new (ctx) DeclRefExpr(
          temporaryVar, DeclNameLoc(implicitLoc), /*implicit=*/true);
      declRef->setType(LValueType::get(temporaryVar->getType()));

      auto assign = new (ctx) AssignExpr(
          declRef, implicitLoc, finalCapturedExpr, /*implicit=*/true);
      assign->setType(TupleType::getEmpty(ctx));
      return assign;
    }
    }
  }

  /// Declare the given temporary variable, adding the appropriate
  /// entries to the elements of a brace stmt.
  void declareTemporaryVariable(VarDecl *temporaryVar,
                                std::vector<ASTNode> &elements) {
    if (!temporaryVar)
      return;

    // Form a new pattern binding to bind the temporary variable to the
    // transformed expression.
    auto pattern = new (ctx) NamedPattern(temporaryVar,/*implicit=*/true);
    pattern->setType(temporaryVar->getType());

    auto pbd = PatternBindingDecl::createImplicit(ctx, StaticSpellingKind::None, pattern, nullptr, dc);
    elements.push_back(temporaryVar);
    elements.push_back(pbd);
  }

public:
  BuilderClosureRewriter(const Solution &solution,
                         DeclContext *dc,
                         const AppliedBuilderTransform &builderTransform,
                         std::function<Expr *(Expr *)> rewriteExpr)
    : ctx(solution.getConstraintSystem().getASTContext()),
      solution(solution), dc(dc), builderTransform(builderTransform),
      rewriteExpr(rewriteExpr) { }

  Stmt *visitBraceStmt(BraceStmt *braceStmt, FunctionBuilderTarget target,
                       Optional<FunctionBuilderTarget> innerTarget = None) {
    std::vector<ASTNode> newElements;

    // If there is an "inner" target corresponding to this brace, declare
    // it's temporary variable if needed.
    if (innerTarget) {
      declareTemporaryVariable(innerTarget->captured.first, newElements);
    }

    for (const auto node : braceStmt->getElements()) {
      if (auto expr = node.dyn_cast<Expr *>()) {
        // Each expression turns into a 'let' that captures the value of
        // the expression.
        auto recorded = takeCapturedExpr(expr);

        // Rewrite the expression
        Expr *finalExpr = rewriteExpr(recorded.generatedExpr);

        // Form a new pattern binding to bind the temporary variable to the
        // transformed expression.
        auto pattern = new (ctx) NamedPattern(
            recorded.temporaryVar, /*implicit=*/true);
        pattern->setType(recorded.temporaryVar->getType());
        newElements.push_back(recorded.temporaryVar);

        auto pbd = PatternBindingDecl::createImplicit(ctx, StaticSpellingKind::None, pattern, finalExpr, dc);
        newElements.push_back(pbd);
        continue;
      }

      if (auto stmt = node.dyn_cast<Stmt *>()) {
        // Each statement turns into a (potential) temporary variable
        // binding followed by the statement itself.
        auto captured = takeCapturedStmt(stmt);

        declareTemporaryVariable(captured.first, newElements);

        Stmt *finalStmt = visit(
            stmt,
            FunctionBuilderTarget{FunctionBuilderTarget::TemporaryVar,
                                  std::move(captured)});
        newElements.push_back(finalStmt);
        continue;
      }

      llvm_unreachable("Cannot yet handle declarations");
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

  Stmt *visitIfStmt(IfStmt *ifStmt, FunctionBuilderTarget target) {
    // Rewrite the condition.
    // FIXME: We should handle the whole condition within the type system.
    auto cond = ifStmt->getCond();
    auto condExpr = cond.front().getBoolean();
    auto finalCondExpr = rewriteExpr(condExpr);
    cond.front().setBoolean(finalCondExpr);
    ifStmt->setCond(cond);

    assert(target.kind == FunctionBuilderTarget::TemporaryVar);
    auto temporaryVar = target.captured.first;

    // Translate the "then" branch.
    auto capturedThen = takeCapturedStmt(ifStmt->getThenStmt());
    auto newThen = visitBraceStmt(cast<BraceStmt>(ifStmt->getThenStmt()),
          FunctionBuilderTarget::forAssign(
            temporaryVar, {target.captured.second[0]}),
          FunctionBuilderTarget::forAssign(
            capturedThen.first, {capturedThen.second.front()}));
    ifStmt->setThenStmt(newThen);

    if (auto elseBraceStmt =
            dyn_cast_or_null<BraceStmt>(ifStmt->getElseStmt())) {
      // Translate the "else" branch when it's a stmt-brace.
      auto capturedElse = takeCapturedStmt(elseBraceStmt);
      Stmt *newElse = visitBraceStmt(
          elseBraceStmt,
          FunctionBuilderTarget::forAssign(
            temporaryVar, {target.captured.second[1]}),
          FunctionBuilderTarget::forAssign(
            capturedElse.first, {capturedElse.second.front()}));
      ifStmt->setElseStmt(newElse);
    } else if (auto elseIfStmt = cast_or_null<IfStmt>(ifStmt->getElseStmt())){
      // Translate the "else" branch when it's an else-if.
      auto capturedElse = takeCapturedStmt(elseIfStmt);
      std::vector<ASTNode> newElseElements;
      declareTemporaryVariable(capturedElse.first, newElseElements);
      newElseElements.push_back(
          visitIfStmt(
            elseIfStmt,
            FunctionBuilderTarget::forAssign(
              capturedElse.first, capturedElse.second)));
      newElseElements.push_back(
          initializeTarget(
            FunctionBuilderTarget::forAssign(
              temporaryVar, {target.captured.second[1]})));

      Stmt *newElse = BraceStmt::create(
          ctx, elseIfStmt->getStartLoc(), newElseElements,
          elseIfStmt->getEndLoc());
      ifStmt->setElseStmt(newElse);
    } else {
      // Form an "else" brace containing an assignment to the temporary
      // variable.
      auto init = initializeTarget(
          FunctionBuilderTarget::forAssign(
            temporaryVar, {target.captured.second[1]}));
      auto newElse = BraceStmt::create(
          ctx, ifStmt->getEndLoc(), { init }, ifStmt->getEndLoc());
      ifStmt->setElseStmt(newElse);
    }

    return ifStmt;
  }

  Stmt *visitDoStmt(DoStmt *doStmt, FunctionBuilderTarget target) {
    // Each statement turns into a (potential) temporary variable
    // binding followed by the statement itself.
    auto body = cast<BraceStmt>(doStmt->getBody());
    auto captured = takeCapturedStmt(body);

    auto newInnerBody = cast<BraceStmt>(
        visitBraceStmt(
          body,
          target,
          FunctionBuilderTarget::forAssign(
            captured.first, {captured.second.front()})));
    doStmt->setBody(newInnerBody);
    return doStmt;
  }

#define UNHANDLED_FUNCTION_BUILDER_STMT(STMT) \
  Stmt *visit##STMT##Stmt(STMT##Stmt *stmt, FunctionBuilderTarget target) { \
    llvm_unreachable("Function builders do not allow statement of kind " \
                     #STMT); \
  }

  UNHANDLED_FUNCTION_BUILDER_STMT(Return)
  UNHANDLED_FUNCTION_BUILDER_STMT(Yield)
  UNHANDLED_FUNCTION_BUILDER_STMT(Guard)
  UNHANDLED_FUNCTION_BUILDER_STMT(While)
  UNHANDLED_FUNCTION_BUILDER_STMT(Defer)
  UNHANDLED_FUNCTION_BUILDER_STMT(DoCatch)
  UNHANDLED_FUNCTION_BUILDER_STMT(RepeatWhile)
  UNHANDLED_FUNCTION_BUILDER_STMT(ForEach)
  UNHANDLED_FUNCTION_BUILDER_STMT(Switch)
  UNHANDLED_FUNCTION_BUILDER_STMT(Case)
  UNHANDLED_FUNCTION_BUILDER_STMT(Catch)
  UNHANDLED_FUNCTION_BUILDER_STMT(Break)
  UNHANDLED_FUNCTION_BUILDER_STMT(Continue)
  UNHANDLED_FUNCTION_BUILDER_STMT(Fallthrough)
  UNHANDLED_FUNCTION_BUILDER_STMT(Fail)
  UNHANDLED_FUNCTION_BUILDER_STMT(Throw)
  UNHANDLED_FUNCTION_BUILDER_STMT(PoundAssert)
#undef UNHANDLED_FUNCTION_BUILDER_STMT
};

} // end anonymous namespace

BraceStmt *swift::applyFunctionBuilderTransform(
    const Solution &solution,
    AppliedBuilderTransform applied,
    BraceStmt *body,
    DeclContext *dc,
    std::function<Expr *(Expr *)> rewriteExpr) {
  BuilderClosureRewriter rewriter(solution, dc, applied, rewriteExpr);
  auto captured = rewriter.takeCapturedStmt(body);
  return cast<BraceStmt>(
    rewriter.visitBraceStmt(
      body,
      FunctionBuilderTarget::forReturn(applied.returnExpr),
      FunctionBuilderTarget::forAssign(
        captured.first, captured.second)));
}

BraceStmt *
TypeChecker::applyFunctionBuilderBodyTransform(FuncDecl *func,
                                               Type builderType) {
  // Form a constraint system to type-check the body.
  ConstraintSystemOptions options = None;
  if (auto resultInterfaceTy = func->getResultInterfaceType()) {
    auto resultContextTy = func->mapTypeIntoContext(resultInterfaceTy);
    if (auto opaque = resultContextTy->getAs<OpaqueTypeArchetypeType>()) {
      if (opaque->getDecl()->isOpaqueReturnTypeOfFunction(func))
        options |= ConstraintSystemFlags::UnderlyingTypeForOpaqueReturnType;
    }
  }

#if false
  ConstraintSystem cs(*this, func, options);

  // Try to build a single result expression.
  // FIXME: Need a constraint system to check all of this at once. Ugh
  BuilderClosureVisitor visitor(Context, &cs, FD,
                                /*wantExpr=*/true, builderType);
  Expr *returnExpr = visitor.visit(body);
  if (!returnExpr)
    return nullptr;

  // Make sure we have a usable result type for the body.
  Type returnType = AnyFunctionRef(FD).getBodyResultType();
  if (!returnType || returnType->hasError())
    return nullptr;

  auto loc = returnExpr->getStartLoc();
  auto returnStmt =
    new (Context) ReturnStmt(loc, returnExpr, /*implicit*/ true);
  return BraceStmt::create(Context, body->getLBraceLoc(), { returnStmt },
                           body->getRBraceLoc());
#else
  return func->getBody();
#endif
}

ConstraintSystem::TypeMatchResult ConstraintSystem::matchFunctionBuilder(
    AnyFunctionRef fn, Type builderType, Type bodyResultType,
    ConstraintLocator *calleeLocator,
    ConstraintLocatorBuilder locator) {
  auto builder = builderType->getAnyNominal();
  assert(builder && "Bad function builder type");
  assert(builder->getAttrs().hasAttribute<FunctionBuilderAttr>());

  // FIXME: Right now, single-expression closures suppress the function
  // builder translation.
  if (auto closure = fn.getAbstractClosureExpr()) {
    if (closure->hasSingleExpressionBody())
      return getTypeMatchSuccess();
  }

  // Pre-check the body: pre-check any expressions in it and look
  // for return statements.
  switch (TC.preCheckFunctionBuilderBody(fn)) {
  case FunctionBuilderBodyPrecheck::Okay:
    // If the pre-check was okay, apply the function-builder transform.
    break;

  case FunctionBuilderBodyPrecheck::Error:
    // If the pre-check had an error, flag that.
    return getTypeMatchFailure(locator);

  case FunctionBuilderBodyPrecheck::HasReturnStmt:
    // If the body has a return statement, suppress the transform but
    // continue solving the constraint system.
    return getTypeMatchSuccess();
  }

  // Check the form of the body to see if we can apply the
  // function-builder translation at all.
  {
    // Check whether we can apply this specific function builder.
    BuilderConstraintGenerator visitor(getASTContext(), nullptr,
                                       fn.getAsDeclContext(),
                                       builderType);

    // If we saw a control-flow statement or declaration that the builder
    // cannot handle, we don't have a well-formed function builder application.
    if (auto unhandledNode = visitor.check(fn.getBody())) {
      // If we aren't supposed to attempt fixes, fail.
      if (!shouldAttemptFixes()) {
        return getTypeMatchFailure(locator);
      }

      // Record the first unhandled construct as a fix.
      if (recordFix(
              SkipUnhandledConstructInFunctionBuilder::create(
                *this, unhandledNode, builder,
                getConstraintLocator(locator)))) {
        return getTypeMatchFailure(locator);
      }
    }
  }

  // If the builder type has a type parameter, substitute in the type
  // variables.
  if (builderType->hasTypeParameter()) {
    assert(calleeLocator &&
           "Cannot have a generic builder type without a call site");

    // Find the opened type for this callee and substitute in the type
    // parametes.
    for (const auto &opened : OpenedTypes) {
      if (opened.first == calleeLocator) {
        OpenedTypeMap replacements(opened.second.begin(),
                                   opened.second.end());
        builderType = openType(builderType, replacements);
        break;
      }
    }
    assert(!builderType->hasTypeParameter());
  }

  BuilderConstraintGenerator visitor(
      getASTContext(), this, fn.getAsDeclContext(), builderType);

  auto applied = visitor.apply(fn.getBody());
  if (!applied)
    return getTypeMatchFailure(locator);

  Type transformedType = getType(applied->returnExpr);
  assert(transformedType && "Missing type");

  // Record the transformation.
  assert(std::find_if(
      builderTransformedFunctions.begin(),
      builderTransformedFunctions.end(),
      [&](const std::pair<AnyFunctionRef, AppliedBuilderTransform> &elt) {
        return elt.first == fn;
      }) == builderTransformedFunctions.end() &&
         "already transformed this function along this path!?!");
  builderTransformedFunctions.push_back({ fn, std::move(*applied) });

  // Bind the result type of the function to the type of the transformed
  // expression.
  addConstraint(ConstraintKind::Equal, bodyResultType, transformedType,
                locator);
  return getTypeMatchSuccess();
}

namespace {

/// Pre-check all the expressions in the function body, in preparation for
/// applying a function builder.
class PreCheckFunctionBuilderApplication : public ASTWalker {
  TypeChecker &TC;
  AnyFunctionRef Fn;
  bool HasReturnStmt = false;
  bool HasError = false;
public:
  PreCheckFunctionBuilderApplication(TypeChecker &tc, AnyFunctionRef fn)
    : TC(tc), Fn(fn) {}

  FunctionBuilderBodyPrecheck run() {
    Stmt *oldBody = Fn.getBody();

    Stmt *newBody = oldBody->walk(*this);

    // If the walk was aborted, it was because we had a problem of some kind.
    assert((newBody == nullptr) == (HasError || HasReturnStmt) &&
           "unexpected short-circuit while walking body");
    if (!newBody) {
      if (HasError)
        return FunctionBuilderBodyPrecheck::Error;

      return FunctionBuilderBodyPrecheck::HasReturnStmt;
    }

    assert(oldBody == newBody && "pre-check walk wasn't in-place?");

    return FunctionBuilderBodyPrecheck::Okay;
  }

  std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
    // Pre-check the expression.  If this fails, abort the walk immediately.
    // Otherwise, replace the expression with the result of pre-checking.
    // In either case, don't recurse into the expression.
    if (TC.preCheckExpression(E, Fn.getAsDeclContext())) {
      HasError = true;
      return std::make_pair(false, nullptr);
    }

    return std::make_pair(false, E);
  }

  std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
    // If we see a return statement, abort the walk immediately.
    if (isa<ReturnStmt>(S)) {
      HasReturnStmt = true;
      return std::make_pair(false, nullptr);
    }

    // Otherwise, recurse into the statement normally.
    return std::make_pair(true, S);
  }
};

}

FunctionBuilderBodyPrecheck
TypeChecker::preCheckFunctionBuilderBody(AnyFunctionRef fn) {
  // Check whether we've already done this analysis.
  auto it = precheckedFunctionBuilderBodies.find(fn);
  if (it != precheckedFunctionBuilderBodies.end())
    return it->second;

  auto result = PreCheckFunctionBuilderApplication(*this, fn).run();

  // Cache the result.
  precheckedFunctionBuilderBodies.insert(std::make_pair(fn, result));

  return result;
}
