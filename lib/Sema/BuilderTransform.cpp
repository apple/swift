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

class ResultBuilderTransform
    : private StmtVisitor<ResultBuilderTransform, NullablePtr<Stmt>,
                          NullablePtr<VarDecl>> {
  friend StmtVisitor<ResultBuilderTransform, NullablePtr<Stmt>,
                     NullablePtr<VarDecl>>;

  using UnsupportedElt = SkipUnhandledConstructInResultBuilder::UnhandledNode;

  ASTContext &ctx;
  DeclContext *dc;
  ResultBuilder builder;

  /// The result type of this result builder body.
  Type ResultType;

  /// The first recorded unsupported element discovered by the transformation.
  UnsupportedElt FirstUnsupported;

public:
  ResultBuilderTransform(ConstraintSystem &cs, DeclContext *dc,
                         Type builderType, Type resultTy)
      : ctx(cs.getASTContext()), dc(dc), builder(cs, dc, builderType),
        ResultType(resultTy) {}

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
          builderCall = builder.buildCall(ifStmt->getThenStmt()->getStartLoc(),
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

        buildEitherCalls.push_back(builder.buildCall(
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
        auto *builderCall = builder.buildCall(
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
          auto *builderCall = builder.buildCall(
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
          builder.buildCall(forEachStmt->getEndLoc(), ctx.Id_buildArray,
                            {builder.buildVarRef(arrayVar, endLoc)},
                            {Identifier()})));
    }

    return DoStmt::createImplicit(ctx, LabeledStmtInfo(), doBody);
  }

  UNSUPPORTED_STMT(Throw)
  UNSUPPORTED_STMT(Return)
  UNSUPPORTED_STMT(Yield)
  UNSUPPORTED_STMT(Discard)
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

private:
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
          builder.buildCall(operand->getStartLoc(), ctx.Id_buildEither, operand,
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

} // end anonymous namespace

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

    SyntacticElementTarget funcTarget(func);
    CompletionContextFinder analyzer(funcTarget, func->getDeclContext());
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
      cs.diagnoseFailureFor(SyntacticElementTarget(func));
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
  if (auto result =
          cs.applySolution(solutions.front(), SyntacticElementTarget(func))) {
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

  // We have already pre-checked the result builder body. Technically, we
  // shouldn't need to do anything here, but there was a bug here that we did
  // not apply the result builder transform if it contained an explicit return.
  // To maintain source compatibility, we still need to check for HasReturnStmt.
  // https://github.com/apple/swift/issues/64332.
  auto request =
      PreCheckResultBuilderRequest{{fn, /*SuppressDiagnostics=*/false}};
  switch (evaluateOrDefault(getASTContext().evaluator, request,
                            ResultBuilderBodyPreCheck::Error)) {
  case ResultBuilderBodyPreCheck::Okay:
    // If the pre-check was okay, apply the result-builder transform.
    break;

  case ResultBuilderBodyPreCheck::Error: {
    llvm_unreachable(
        "Running PreCheckResultBuilderRequest on a function shouldn't run "
        "preCheckExpression and thus we should never enter this case.");
    break;
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
        recordTypeVariablesAsHoles(getClosureType(closure));
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

  if (generateConstraints(fn, transformInfo.transformedBody))
    return getTypeMatchFailure(locator);

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

      HasError |= ConstraintSystem::preCheckExpression(
          E, DC, /*replaceInvalidRefsWithErrors=*/true,
          /*leaveClosureBodiesUnchecked=*/false);

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
  // Closures should already be pre-checked when we run this, so there's no need
  // to pre-check them again.
  bool skipPrecheck = owner.Fn.getAbstractClosureExpr();
  return PreCheckResultBuilderApplication(
             owner.Fn, skipPrecheck,
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
      builderType->getAnyNominal()->getLoc(),
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

ResultBuilder::ResultBuilder(ConstraintSystem &CS, DeclContext *DC,
                             Type builderType)
    : DC(DC), BuilderType(CS.simplifyType(builderType)) {
  auto &ctx = DC->getASTContext();
  // Use buildOptional(_:) if available, otherwise fall back to buildIf
  // when available.
  BuildOptionalId =
      (supports(ctx.Id_buildOptional) || !supports(ctx.Id_buildIf))
          ? ctx.Id_buildOptional
          : ctx.Id_buildIf;

  BuilderSelf = new (ctx) VarDecl(
      /*isStatic=*/false, VarDecl::Introducer::Let,
      /*nameLoc=*/SourceLoc(), ctx.Id_builderSelf, DC);
  BuilderSelf->setImplicit();
  CS.setType(BuilderSelf, MetatypeType::get(BuilderType));
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
