//===--- ArgumentList.cpp - Function and subscript argument lists -*- C++ -*==//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the logic for the Argument and ArgumentList classes.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ArgumentList.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ParameterList.h"

using namespace swift;

Type swift::__Expr_getType(Expr *E) { return E->getType(); }

SourceRange Argument::getSourceRange() const {
  auto labelLoc = getLabelLoc();
  if (labelLoc.isInvalid())
    return getExpr()->getSourceRange();

  auto exprEndLoc = getExpr()->getEndLoc();
  if (exprEndLoc.isInvalid())
    return labelLoc;

  return SourceRange(labelLoc, exprEndLoc);
}

Argument Argument::implicitInOut(ASTContext &ctx, Expr *expr) {
  assert(!isa<InOutExpr>(expr) && "Cannot nest InOutExpr");

  // Eventually this will set an 'inout' bit on Argument, but for now,
  // synthesize in the InOutExpr.
  Type objectTy;
  if (auto subTy = expr->getType())
    objectTy = subTy->castTo<LValueType>()->getObjectType();

  return Argument::unlabeled(
      new (ctx) InOutExpr(SourceLoc(), expr, objectTy, /*isImplicit*/ true));
}

bool Argument::isInOut() const {
  return ArgExpr->isSemanticallyInOutExpr();
}

bool Argument::isConst() const {
  return ArgExpr->isSemanticallyConstExpr();
}

ArgumentList *ArgumentList::create(
    ASTContext &ctx, SourceLoc lParenLoc, ArrayRef<Argument> args,
    SourceLoc rParenLoc, llvm::Optional<unsigned> firstTrailingClosureIndex,
    bool isImplicit, ArgumentList *originalArgs, AllocationArena arena) {
  SmallVector<Expr *, 4> exprs;
  SmallVector<Identifier, 4> labels;
  SmallVector<SourceLoc, 4> labelLocs;

  bool hasLabels = false;
  bool hasLabelLocs = false;
  for (auto &arg : args) {
    exprs.push_back(arg.getExpr());

    hasLabels |= !arg.getLabel().empty();
    labels.push_back(arg.getLabel());

    hasLabelLocs |= arg.getLabelLoc().isValid();
    labelLocs.push_back(arg.getLabelLoc());
  }
  if (!hasLabels)
    labels.clear();
  if (!hasLabelLocs)
    labelLocs.clear();

  auto numBytes =
      totalSizeToAlloc<Expr *, Identifier, SourceLoc, ArgumentList *>(
          exprs.size(), labels.size(), labelLocs.size(), originalArgs ? 1 : 0);
  auto *mem = ctx.Allocate(numBytes, alignof(ArgumentList), arena);
  auto *argList = new (mem)
      ArgumentList(lParenLoc, rParenLoc, args.size(), firstTrailingClosureIndex,
                   originalArgs, isImplicit, hasLabels, hasLabelLocs);

  std::uninitialized_copy(exprs.begin(), exprs.end(),
                          argList->getExprsBuffer().begin());
  if (hasLabels) {
    std::uninitialized_copy(labels.begin(), labels.end(),
                            argList->getLabelsBuffer().begin());
  }
  if (hasLabelLocs) {
    std::uninitialized_copy(labelLocs.begin(), labelLocs.end(),
                            argList->getLabelLocsBuffer().begin());
  }
  if (originalArgs) {
    *argList->getTrailingObjects<ArgumentList *>() = originalArgs;
  }
  return argList;
}

ArgumentList *
ArgumentList::createParsed(ASTContext &ctx, SourceLoc lParenLoc,
                           ArrayRef<Argument> args, SourceLoc rParenLoc,
                           llvm::Optional<unsigned> firstTrailingClosureIndex) {
  return create(ctx, lParenLoc, args, rParenLoc, firstTrailingClosureIndex,
                /*implicit*/ false);
}

ArgumentList *ArgumentList::createTypeChecked(ASTContext &ctx,
                                              ArgumentList *originalArgs,
                                              ArrayRef<Argument> newArgs) {
  return create(ctx, originalArgs->getLParenLoc(), newArgs,
                originalArgs->getRParenLoc(), /*trailingClosureIdx*/ llvm::None,
                originalArgs->isImplicit(), originalArgs);
}

ArgumentList *
ArgumentList::createImplicit(ASTContext &ctx, SourceLoc lParenLoc,
                             ArrayRef<Argument> args, SourceLoc rParenLoc,
                             llvm::Optional<unsigned> firstTrailingClosureIndex,
                             AllocationArena arena) {
  return create(ctx, lParenLoc, args, rParenLoc, firstTrailingClosureIndex,
                /*implicit*/ true,
                /*originalArgs*/ nullptr, arena);
}

ArgumentList *
ArgumentList::createImplicit(ASTContext &ctx, ArrayRef<Argument> args,
                             llvm::Optional<unsigned> firstTrailingClosureIndex,
                             AllocationArena arena) {
  return createImplicit(ctx, SourceLoc(), args, SourceLoc(),
                        firstTrailingClosureIndex, arena);
}

ArgumentList *ArgumentList::forImplicitSingle(ASTContext &ctx, Identifier label,
                                              Expr *arg) {
  return createImplicit(ctx, {Argument(SourceLoc(), label, arg)});
}

ArgumentList *ArgumentList::forImplicitUnlabeled(ASTContext &ctx,
                                                 ArrayRef<Expr *> argExprs) {
  SmallVector<Argument, 4> args;
  for (auto *argExpr : argExprs)
    args.push_back(Argument::unlabeled(argExpr));
  return createImplicit(ctx, args);
}

ArgumentList *ArgumentList::forImplicitCallTo(DeclNameRef fnNameRef,
                                              ArrayRef<Expr *> argExprs,
                                              ASTContext &ctx) {
  auto labels = fnNameRef.getArgumentNames();
  assert(labels.size() == argExprs.size());

  SmallVector<Argument, 8> args;
  for (auto idx : indices(argExprs))
    args.emplace_back(SourceLoc(), labels[idx], argExprs[idx]);

  return createImplicit(ctx, args);
}

ArgumentList *ArgumentList::forImplicitCallTo(ParameterList *params,
                                              ArrayRef<Expr *> argExprs,
                                              ASTContext &ctx) {
  assert(params->size() == argExprs.size());
  SmallVector<Argument, 8> args;
  for (auto idx : indices(argExprs)) {
    auto *param = params->get(idx);
    assert(param->isInOut() == argExprs[idx]->isSemanticallyInOutExpr());
    args.emplace_back(SourceLoc(), param->getArgumentName(), argExprs[idx]);
  }
  return createImplicit(ctx, args);
}

SourceLoc ArgumentList::getLoc() const {
  // If we have an unlabeled unary arg, return the start loc of the expr. This
  // preserves the behavior of when such argument lists were represented by
  // ParenExprs.
  if (auto *unary = getUnlabeledUnaryExpr())
    return unary->getStartLoc();
  return getStartLoc();
}

SourceRange ArgumentList::getSourceRange() const {
  auto start = LParenLoc;
  if (start.isInvalid()) {
    // Scan forward for the first valid source loc.
    for (auto arg : *this) {
      start = arg.getStartLoc();
      if (start.isValid())
        break;
    }
  }
  auto end = RParenLoc;
  if (hasAnyTrailingClosures() || RParenLoc.isInvalid()) {
    // Scan backward for the first valid source loc. We use getOriginalArgs to
    // filter out default arguments and get accurate trailing closure info.
    for (auto arg : llvm::reverse(*getOriginalArgs())) {
      end = arg.getEndLoc();
      if (end.isValid())
        break;
    }
  }
  if (start.isInvalid() || end.isInvalid())
    return SourceRange();

  return SourceRange(start, end);
}

ArrayRef<Identifier>
ArgumentList::getArgumentLabels(SmallVectorImpl<Identifier> &scratch) const {
  assert(scratch.empty());
  if (HasLabels)
    return getLabelsBuffer();
  scratch.append(size(), Identifier());
  return scratch;
}

llvm::Optional<unsigned>
ArgumentList::findArgumentExpr(Expr *expr, bool allowSemantic) const {
  if (allowSemantic)
    expr = expr->getSemanticsProvidingExpr();
  for (auto idx : indices(*this)) {
    auto *argExpr = getExpr(idx);
    if (allowSemantic)
      argExpr = argExpr->getSemanticsProvidingExpr();

    if (expr == argExpr)
      return idx;
  }
  return llvm::None;
}

Expr *ArgumentList::packIntoImplicitTupleOrParen(
    ASTContext &ctx, llvm::function_ref<Type(Expr *)> getType) const {
  assert(!hasAnyInOutArgs() && "Cannot construct bare tuple/paren with inout");

  // Make sure to preserve the source location info here and below as it may be
  // needed for e.g serialization of its textual representation.
  if (auto *unary = getUnlabeledUnaryExpr()) {
    auto *paren = new (ctx) ParenExpr(getLParenLoc(), unary, getRParenLoc());
    if (auto ty = getType(unary))
      paren->setType(ParenType::get(ctx, ty));
    paren->setImplicit();
    return paren;
  }

  SmallVector<Expr *, 2> argExprs;
  SmallVector<Identifier, 2> argLabels;
  SmallVector<SourceLoc, 2> argLabelLocs;
  SmallVector<TupleTypeElt, 2> tupleEltTypes;

  for (auto arg : *this) {
    auto *argExpr = arg.getExpr();
    argExprs.push_back(argExpr);
    argLabels.push_back(arg.getLabel());
    argLabelLocs.push_back(arg.getLabelLoc());
    if (auto ty = getType(argExpr))
      tupleEltTypes.emplace_back(ty, arg.getLabel());
  }
  assert(tupleEltTypes.empty() || tupleEltTypes.size() == argExprs.size());

  auto *tuple =
      TupleExpr::create(ctx, getLParenLoc(), argExprs, argLabels, argLabelLocs,
                        getRParenLoc(), /*implicit*/ true);
  if (empty() || !tupleEltTypes.empty())
    tuple->setType(TupleType::get(tupleEltTypes, ctx));

  return tuple;
}

bool ArgumentList::matches(ArrayRef<AnyFunctionType::Param> params,
                           llvm::function_ref<Type(Expr *)> getType) const {
  if (size() != params.size())
    return false;

  for (auto i : indices(*this)) {
    auto arg = get(i);
    auto &param = params[i];

    if (arg.getLabel() != param.getLabel())
      return false;

    if (arg.isInOut() != param.isInOut())
      return false;

    auto argTy = getType(arg.getExpr());
    assert(argTy && "Expected type for argument");
    auto paramTy = param.getParameterType();
    assert(paramTy && "Expected a type for param");

    if (arg.isInOut())
      argTy = argTy->getInOutObjectType();

    if (!argTy->isEqual(paramTy))
      return false;
  }
  return true;
}
