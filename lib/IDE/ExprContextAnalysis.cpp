//===--- ExprContextAnalysis.cpp - Expession context analysis -------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "ExprContextAnalysis.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DeclContext.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/SourceManager.h"
#include "swift/IDE/CodeCompletionResult.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Subsystems.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/SmallSet.h"

using namespace swift;
using namespace ide;

//===----------------------------------------------------------------------===//
// typeCheckContextAt(DeclContext, SourceLoc)
//===----------------------------------------------------------------------===//

void swift::ide::typeCheckContextAt(TypeCheckASTNodeAtLocContext TypeCheckCtx,
                                    SourceLoc Loc) {
  // Make sure the extension has been bound.
  auto DC = TypeCheckCtx.getDeclContext();
  // Even if the extension is invalid (e.g. nested in a function or another
  // type), we want to know the "intended nominal" of the extension so that
  // we can know the type of 'Self'.
  SmallVector<ExtensionDecl *, 1> extensions;
  for (auto typeCtx = DC->getInnermostTypeContext(); typeCtx != nullptr;
       typeCtx = typeCtx->getParent()->getInnermostTypeContext()) {
    if (auto *ext = dyn_cast<ExtensionDecl>(typeCtx))
      extensions.push_back(ext);
  }
  while (!extensions.empty()) {
    extensions.back()->computeExtendedNominal();
    extensions.pop_back();
  }

  // If the completion happens in the inheritance clause of the extension,
  // 'DC' is the parent of the extension. We need to iterate the top level
  // decls to find it. In theory, we don't need the extended nominal in the
  // inheritance clause, but ASTScope lookup requires that. We don't care
  // unless 'DC' is not 'SourceFile' because non-toplevel extensions are
  // 'canNeverBeBound()' anyway.
  if (auto *SF = dyn_cast<SourceFile>(DC)) {
    auto &SM = DC->getASTContext().SourceMgr;
    for (auto *decl : SF->getTopLevelDecls())
      if (auto *ext = dyn_cast<ExtensionDecl>(decl))
        if (SM.rangeContainsTokenLoc(ext->getSourceRange(), Loc))
          ext->computeExtendedNominal();
  }

  swift::typeCheckASTNodeAtLoc(TypeCheckCtx, Loc);
}

//===----------------------------------------------------------------------===//
// findParsedExpr(DeclContext, Expr)
//===----------------------------------------------------------------------===//

namespace {
class ExprFinder : public ASTWalker {
  SourceManager &SM;
  SourceRange TargetRange;
  Expr *FoundExpr = nullptr;

  template <typename NodeType> bool isInterstingRange(NodeType *Node) {
    return SM.rangeContains(Node->getSourceRange(), TargetRange);
  }

  bool shouldIgnore(Expr *E) {
    // E.g. instanceOfDerived.methodInBaseReturningSelf().#^HERE^#'
    // When calling a method in a base class returning 'Self', the call
    // expression itself has the type of the base class. That is wrapped with
    // CovariantReturnConversionExpr which downcasts it to the derived class.
    if (isa<CovariantReturnConversionExpr>(E))
      return false;

    // E.g. TypeName(#^HERE^#
    // In this case, we want the type expression instead of a reference to the
    // initializer.
    if (isa<ConstructorRefCallExpr>(E))
      return true;

    // Ignore other implicit expression.
    if (E->isImplicit())
      return true;

    return false;
  }

public:
  ExprFinder(SourceManager &SM, SourceRange TargetRange)
      : SM(SM), TargetRange(TargetRange) {}

  Expr *get() const { return FoundExpr; }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (TargetRange == E->getSourceRange() && !shouldIgnore(E)) {
      assert(!FoundExpr && "non-nullptr for found expr");
      FoundExpr = E;
      return Action::Stop();
    }
    return Action::VisitNodeIf(isInterstingRange(E), E);
  }

  PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override {
    return Action::VisitNodeIf(isInterstingRange(P), P);
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    return Action::VisitNodeIf(isInterstingRange(S), S);
  }

  PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
    return Action::SkipNode();
  }
};
} // anonymous namespace

Expr *swift::ide::findParsedExpr(const DeclContext *DC,
                                 SourceRange TargetRange) {
  ExprFinder finder(DC->getASTContext().SourceMgr, TargetRange);
  const_cast<DeclContext *>(DC)->walkContext(finder);
  return finder.get();
}
