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

Argument::Argument(SourceLoc labelLoc, Identifier label, Expr *expr)
    : LabelLoc(labelLoc), Label(label), ArgExpr(expr) {}

SourceRange Argument::getSourceRange() const {
  auto exprEndLoc = getExpr()->getEndLoc();
  if (getLabelLoc().isValid() && exprEndLoc.isValid())
    return SourceRange(getLabelLoc(), exprEndLoc);

  return getExpr()->getSourceRange();
}

bool Argument::isInOut() const {
  return ArgExpr->isSemanticallyInOutExpr();
}

ArgumentList *ArgumentList::create(ASTContext &ctx, SourceLoc lParenLoc,
                                   ArrayRef<Argument> args, SourceLoc rParenLoc,
                                   Optional<unsigned> firstTrailingClosureAt,
                                   bool isImplicit,
                                   Optional<AllocationArena> arena) {
  auto numArgs = args.size();
  auto bytesToAllocate = totalSizeToAlloc<Argument>(numArgs);
  auto *mem = ctx.Allocate(bytesToAllocate, alignof(Argument),
                           arena.getValueOr(AllocationArena::Permanent));

  auto *argList = ::new (mem) ArgumentList(lParenLoc, rParenLoc, numArgs,
                                           firstTrailingClosureAt, isImplicit);
  std::uninitialized_copy(args.begin(), args.end(),
                          argList->getArray().begin());
  return argList;
}

ArgumentList *
ArgumentList::createImplicit(ASTContext &ctx, SourceLoc lParenLoc,
                             ArrayRef<Argument> args, SourceLoc rParenLoc,
                             Optional<AllocationArena> arena) {
  return create(ctx, lParenLoc, args, rParenLoc, /*trailingClosure*/ None,
                /*implicit*/ true, arena);
}

ArgumentList *ArgumentList::createImplicit(ASTContext &ctx,
                                           ArrayRef<Argument> args,
                                           Optional<AllocationArena> arena) {
  return createImplicit(ctx, SourceLoc(), args, SourceLoc());
}

ArgumentList *ArgumentList::forImplicitSingle(ASTContext &ctx, Identifier label,
                                              Expr *arg) {
  return createImplicit(ctx, {Argument(SourceLoc(), label, arg)});
}

ArgumentList *ArgumentList::forImplicitUnlabelled(ASTContext &ctx,
                                                  ArrayRef<Expr *> argExprs) {
  SmallVector<Argument, 4> args;
  for (auto *argExpr : argExprs)
    args.push_back(Argument::unlabelled(argExpr));
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
  if (auto *unary = getUnlabelledUnaryExpr())
    return unary->getStartLoc();
  return getStartLoc();
}

SourceRange ArgumentList::getSourceRange() const {
  SourceLoc start = SourceLoc();
  SourceLoc end = SourceLoc();
  if (LParenLoc.isValid()) {
    start = LParenLoc;
  } else if (empty()) {
    return {SourceLoc(), SourceLoc()};
  } else {
    // Scan forward for the first valid source loc.
    for (auto &arg : *this) {
      start = arg.getExpr()->getStartLoc();
      if (start.isValid()) {
        break;
      }
    }
  }

  if (hasAnyTrailingClosures() || RParenLoc.isInvalid()) {
    if (empty()) {
      return {SourceLoc(), SourceLoc()};
    } else {
      // Scan backwards for a valid source loc.
      for (auto &arg : llvm::reverse(*this)) {
        // Default arguments are located at the start of the argument list, so
        // skip over them.
        if (isa<DefaultArgumentExpr>(arg.getExpr()))
          continue;

        auto newEnd = arg.getEndLoc();
        if (newEnd.isInvalid())
          continue;

        // There is a quirk with the backward scan logic for trailing
        // closures that can cause arguments to be flipped. If there is a
        // single trailing closure, only stop when the "end" point we hit comes
        // after the close parenthesis (if there is one).
        if (end.isInvalid() ||
            end.getOpaquePointerValue() < newEnd.getOpaquePointerValue()) {
          end = newEnd;
        }

        if (!hasAnyTrailingClosures() || RParenLoc.isInvalid() ||
            RParenLoc.getOpaquePointerValue() < end.getOpaquePointerValue())
          break;
      }
    }
  } else {
    end = RParenLoc;
  }

  if (start.isValid() && end.isValid()) {
    return {start, end};
  } else {
    return {SourceLoc(), SourceLoc()};
  }
}

ArrayRef<Identifier>
ArgumentList::getArgumentLabels(SmallVectorImpl<Identifier> &scratch) const {
  for (auto &arg : *this)
    scratch.push_back(arg.getLabel());
  return scratch;
}

ArrayRef<Expr *>
ArgumentList::getArgExprs(SmallVectorImpl<Expr *> &scratch) const {
  for (auto &arg : *this)
    scratch.push_back(arg.getExpr());
  return scratch;
}

Optional<unsigned> ArgumentList::findArgumentExpr(Expr *expr,
                                                  bool allowSemantic) const {
  for (auto idx : indices(*this)) {
    if (allowSemantic) {
      if (getExpr(idx)->getSemanticsProvidingExpr() ==
          expr->getSemanticsProvidingExpr()) {
        return idx;
      }
    } else {
      if (getExpr(idx) == expr)
        return idx;
    }
  }
  return None;
}

Expr *ArgumentList::packIntoImplicitTupleOrParen(
    ASTContext &ctx, llvm::function_ref<Type(Expr *)> getType) const {
  assert(!hasAnyInOutArgs() && "Cannot construct bare tuple/paren with inout");
  if (auto *unary = getUnlabelledUnaryExpr()) {
    auto *paren = new (ctx) ParenExpr(SourceLoc(), unary, SourceLoc());
    if (auto ty = getType(unary))
      paren->setType(ParenType::get(ctx, ty));
    paren->setImplicit();
    return paren;
  }

  SmallVector<Expr *, 8> argExprs;
  SmallVector<Identifier, 8> argLabels;
  SmallVector<TupleTypeElt, 8> tupleEltTypes;

  for (auto &arg : *this) {
    auto *argExpr = arg.getExpr();
    argExprs.push_back(argExpr);
    argLabels.push_back(arg.getLabel());
    if (auto ty = getType(argExpr))
      tupleEltTypes.emplace_back(ty, arg.getLabel());
  }
  assert(tupleEltTypes.empty() || tupleEltTypes.size() == argExprs.size());

  auto *tuple = TupleExpr::createImplicit(ctx, argExprs, argLabels);
  if (empty() || !tupleEltTypes.empty())
    tuple->setType(TupleType::get(tupleEltTypes, ctx));

  return tuple;
}

Type ArgumentList::composeTupleOrParenType(
    ASTContext &ctx, llvm::function_ref<Type(Expr *)> getType) const {
  if (auto *unary = getUnlabelledUnaryExpr()) {
    auto ty = getType(unary);
    assert(ty);
    ParameterTypeFlags flags;
    if (get(0).isInOut()) {
      ty = ty->getInOutObjectType();
      flags = flags.withInOut(true);
    }
    return ParenType::get(ctx, ty, flags);
  }
  SmallVector<TupleTypeElt, 4> elts;
  for (auto &arg : *this) {
    auto ty = getType(arg.getExpr());
    assert(ty);
    ParameterTypeFlags flags;
    if (arg.isInOut()) {
      ty = ty->getInOutObjectType();
      flags = flags.withInOut(true);
    }
    elts.emplace_back(ty, arg.getLabel(), flags);
  }
  return TupleType::get(elts, ctx);
}

bool ArgumentList::matches(ArrayRef<AnyFunctionType::Param> params,
                           llvm::function_ref<Type(Expr *)> getType) const {
  if (size() != params.size())
    return false;

  for (unsigned i = 0, n = size(); i != n; ++i) {
    auto &arg = (*this)[i];
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

bool ArgumentList::transformInto(
    SmallVectorImpl<Argument> &newArgs,
    Optional<unsigned> &newTrailingClosureIndex,
    llvm::function_ref<void(const Argument &)> fn) const {
  bool anyChanged = false;
  for (auto idx : indices(*this)) {
    auto &arg = get(idx);
    if (hasAnyTrailingClosures() && idx == *getFirstTrailingClosureIndex())
      newTrailingClosureIndex = newArgs.size();

    auto priorArgsSize = newArgs.size();
    fn(arg);
    assert(newArgs.size() >= priorArgsSize && "Cannot remove args");
    if (priorArgsSize + 1 != newArgs.size() || newArgs.back() != arg)
      anyChanged = true;
  }

  // If all the trailing closures were removed, ditch the trailing closure
  // index.
  if (newTrailingClosureIndex && *newTrailingClosureIndex == newArgs.size())
    newTrailingClosureIndex = None;

  return anyChanged;
}

bool ArgumentList::getOriginalArguments(
    SmallVectorImpl<Argument> &newArgs,
    Optional<unsigned> *newTrailingClosureIndex) const {

  Optional<unsigned> trailingClosureIdx;
  auto anyChanged = transformInto(newArgs, trailingClosureIdx, [&](auto &arg) {
    auto *argExpr = arg.getExpr();
    auto label = arg.getLabel();
    auto labelLoc = arg.getLabelLoc();

    if (isa<DefaultArgumentExpr>(argExpr))
      return;

    if (auto *varargExpr = dyn_cast<VarargExpansionExpr>(argExpr)) {
      if (auto *arrayExpr = dyn_cast<ArrayExpr>(varargExpr->getSubExpr())) {
        for (auto *elt : arrayExpr->getElements()) {
          newArgs.push_back(Argument(labelLoc, label, elt));
          label = Identifier();
          labelLoc = SourceLoc();
        }
        return;
      }
    }
    newArgs.push_back(arg);
  });

  if (newTrailingClosureIndex)
    *newTrailingClosureIndex = trailingClosureIdx;

  return anyChanged;
}
