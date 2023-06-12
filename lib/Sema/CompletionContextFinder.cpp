//===--- CompletionContextFinder.cpp --------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Sema/CompletionContextFinder.h"
#include "swift/Parse/Lexer.h"
#include "swift/Sema/SyntacticElementTarget.h"

using namespace swift;
using namespace constraints;
using Fallback = CompletionContextFinder::Fallback;

CompletionContextFinder::CompletionContextFinder(
    SyntacticElementTarget target, DeclContext *DC)
    : InitialExpr(target.getAsExpr()), InitialDC(DC) {
  assert(DC);
  target.walk(*this);
}

ASTWalker::PreWalkResult<Expr *>
CompletionContextFinder::walkToExprPre(Expr *E) {
  if (auto *closure = dyn_cast<ClosureExpr>(E)) {
    Contexts.push_back({closure->hasSingleExpressionBody()
                            ? ContextKind::SingleStmtClosure
                            : ContextKind::MultiStmtClosure,
                        closure});
  }

  if (isa<InterpolatedStringLiteralExpr>(E)) {
    Contexts.push_back({ContextKind::StringInterpolation, E});
  }

  if (isa<ApplyExpr>(E) || isa<SequenceExpr>(E) ||
      isa<SingleValueStmtExpr>(E)) {
    Contexts.push_back({ContextKind::FallbackExpression, E});
  }

  if (auto *Error = dyn_cast<ErrorExpr>(E)) {
    Contexts.push_back({ContextKind::ErrorExpression, E});
    if (auto *OrigExpr = Error->getOriginalExpr()) {
      OrigExpr->walk(*this);
      if (hasCompletionExpr())
        return Action::Stop();
    }
  }

  if (auto *CCE = dyn_cast<CodeCompletionExpr>(E)) {
    CompletionNode = CCE;
    return Action::Stop();
  }
  if (auto *KeyPath = dyn_cast<KeyPathExpr>(E)) {
    for (auto &component : KeyPath->getComponents()) {
      if (component.getKind() == KeyPathExpr::Component::Kind::CodeCompletion) {
        CompletionNode = KeyPath;
        return Action::Stop();
      }
    }
    // Code completion in key paths is modelled by a code completion component
    // Don't walk the key path's parsed expressions.
    return Action::SkipChildren(E);
  }

  return Action::Continue(E);
}

ASTWalker::PostWalkResult<Expr *>
CompletionContextFinder::walkToExprPost(Expr *E) {
  if (isa<ClosureExpr>(E) || isa<InterpolatedStringLiteralExpr>(E) ||
      isa<ApplyExpr>(E) || isa<SequenceExpr>(E) || isa<ErrorExpr>(E) ||
      isa<SingleValueStmtExpr>(E)) {
    assert(Contexts.back().E == E);
    Contexts.pop_back();
  }
  return Action::Continue(E);
}

size_t CompletionContextFinder::getKeyPathCompletionComponentIndex() const {
  size_t ComponentIndex = 0;
  auto Components = getKeyPathContainingCompletionComponent()->getComponents();
  for (auto &Component : Components) {
    if (Component.getKind() == KeyPathExpr::Component::Kind::CodeCompletion) {
      break;
    } else {
      ComponentIndex++;
    }
  }
  assert(ComponentIndex < Components.size() &&
         "No completion component in the key path?");
  return ComponentIndex;
}

Optional<Fallback> CompletionContextFinder::getFallbackCompletionExpr() const {
  if (!hasCompletionExpr()) {
    // Creating a fallback expression only makes sense if we are completing in
    // an expression, not when we're completing in a key path.
    return None;
  }

  Optional<Fallback> fallback;
  bool separatePrecheck = false;
  DeclContext *fallbackDC = InitialDC;

  // Find the outermost fallback expression within the innermost error
  // expression or multi-statement closure, keeping track of its decl context.
  for (auto context : Contexts) {
    switch (context.Kind) {
    case ContextKind::StringInterpolation:
      LLVM_FALLTHROUGH;
    case ContextKind::FallbackExpression:
      if (!fallback && context.E != InitialExpr)
        fallback = Fallback{context.E, fallbackDC, separatePrecheck};
      continue;

    case ContextKind::SingleStmtClosure:
      if (!fallback && context.E != InitialExpr)
        fallback = Fallback{context.E, fallbackDC, separatePrecheck};
      fallbackDC = cast<AbstractClosureExpr>(context.E);
      continue;

    case ContextKind::MultiStmtClosure:
      fallbackDC = cast<AbstractClosureExpr>(context.E);
      LLVM_FALLTHROUGH;
    case ContextKind::ErrorExpression:;
      fallback = None;
      separatePrecheck = true;
      continue;
    }
  }

  if (fallback)
    return fallback;

  if (getCompletionExpr() != InitialExpr)
    return Fallback{getCompletionExpr(), fallbackDC, separatePrecheck};
  return None;
}

bool swift::containsIDEInspectionTarget(SourceRange range,
                                        const SourceManager &SourceMgr) {
  if (range.isInvalid())
    return false;
  auto charRange = Lexer::getCharSourceRangeFromSourceRange(SourceMgr, range);
  return SourceMgr.rangeContainsIDEInspectionTarget(charRange);
}
