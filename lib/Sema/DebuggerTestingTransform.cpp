//===--- DebuggerTestingTransform.cpp - Transform for debugger testing ----===//
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
///
/// \file This transform inserts instrumentation which the debugger can use to
/// test its expression evaluation facilities.
///
//===----------------------------------------------------------------------===//

#include "CodeSynthesis.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTNode.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DeclContext.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/Subsystems.h"

#include "TypeChecker.h"

using namespace swift;

ASTWalker::PostWalkResult<Expr *>
DiscriminatorFinder::walkToExprPost(Expr *E)  {
  auto *ACE = dyn_cast<AbstractClosureExpr>(E);
  if (!ACE)
    return Action::Continue(E);

  unsigned Discriminator = ACE->getDiscriminator();
  assert(Discriminator != AbstractClosureExpr::InvalidDiscriminator &&
         "Existing closures should have valid discriminators");
  if (Discriminator >= NextDiscriminator)
    NextDiscriminator = Discriminator + 1;
  if (FirstDiscriminator == AbstractClosureExpr::InvalidDiscriminator ||
      Discriminator < FirstDiscriminator)
    FirstDiscriminator = Discriminator;

  return Action::Continue(E);
}

unsigned DiscriminatorFinder::getNextDiscriminator() {
  if (NextDiscriminator == AbstractClosureExpr::InvalidDiscriminator)
    llvm::report_fatal_error("Out of valid closure discriminators");
  return NextDiscriminator++;
}

namespace {

/// Instrument decls with sanity-checks which the debugger can evaluate.
class DebuggerTestingTransform : public ASTWalker {
  ASTContext &Ctx;
  DiscriminatorFinder &DF;
  std::vector<DeclContext *> LocalDeclContextStack;
  const DeclNameRef StringForPrintObjectName;
  const DeclNameRef DebuggerTestingCheckExpectName;

public:
  DebuggerTestingTransform(ASTContext &Ctx, DiscriminatorFinder &DF)
      : Ctx(Ctx), DF(DF),
        StringForPrintObjectName(Ctx.getIdentifier("_stringForPrintObject")),
        DebuggerTestingCheckExpectName(
            Ctx.getIdentifier("_debuggerTestingCheckExpect")) {}

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    pushLocalDeclContext(D);

    // Skip implicit decls, because the debugger isn't used to step through
    // these.
    if (D->isImplicit())
      return Action::SkipChildren();

    // Whitelist the kinds of decls to transform.
    // TODO: Expand the set of decls visited here.
    if (auto *FD = dyn_cast<AbstractFunctionDecl>(D))
      return Action::VisitChildrenIf(FD->getBody());
    if (auto *TLCD = dyn_cast<TopLevelCodeDecl>(D))
      return Action::VisitChildrenIf(TLCD->getBody());
    if (isa<NominalTypeDecl>(D))
      return Action::Continue();

    return Action::SkipChildren();
  }

  PostWalkAction walkToDeclPost(Decl *D) override {
    popLocalDeclContext(D);
    return Action::Continue();
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    pushLocalDeclContext(E);

    // Whitelist the kinds of exprs to transform.
    // TODO: Expand the set of exprs visited here.
    if (auto *AE = dyn_cast<AssignExpr>(E))
      return insertCheckExpect(AE, AE->getDest());

    return Action::Continue(E);
  }

  PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
    popLocalDeclContext(E);
    return Action::Continue(E);
  }

private:
  /// Return N as a local DeclContext if possible, or return nullptr.
  DeclContext *getLocalDeclContext(ASTNode N) const {
    DeclContext *DC = N.getAsDeclContext();
    return (DC && DC->isLocalContext()) ? DC : nullptr;
  }

  /// If N is a local DeclContext, push it onto the context stack.
  void pushLocalDeclContext(ASTNode N) {
    if (auto *LDC = getLocalDeclContext(N))
      LocalDeclContextStack.push_back(LDC);
  }

  /// If N is a local DeclContext, pop it off the context stack.
  void popLocalDeclContext(ASTNode N) {
    if (getLocalDeclContext(N))
      LocalDeclContextStack.pop_back();
  }

  /// Get the current local DeclContext. This is used to create closures in the
  /// right context.
  DeclContext *getCurrentDeclContext() const {
    assert(!LocalDeclContextStack.empty() && "Missing decl context");
    return LocalDeclContextStack.back();
  }

  /// Try to extract a DeclRefExpr or MemberRefExpr from the expression.
  Expr *extractDeclOrMemberRef(Expr *E) {
    while (!isa<DeclRefExpr>(E) && !isa<MemberRefExpr>(E)) {
      // TODO: Try more ways to extract interesting decl refs.
      if (auto *Subscript = dyn_cast<SubscriptExpr>(E))
        E = Subscript->getBase();
      else if (auto *InOut = dyn_cast<InOutExpr>(E))
        E = InOut->getSubExpr();
      else if (auto *MemberRef = dyn_cast<MemberRefExpr>(E))
        E = MemberRef->getBase();
      else
        return nullptr;
    }
    return E;
  }

  /// Attempt to create a functionally-equivalent replacement for OriginalExpr,
  /// given that DstExpr identifies the target of some mutating update, and that
  /// DstExpr is a subexpression of OriginalExpr.
  ///
  /// The return value contains 1) a flag indicating whether or not to
  /// recursively transform the children of the transformed expression, and 2)
  /// the transformed expression itself.
  PreWalkResult<Expr *> insertCheckExpect(Expr *OriginalExpr, Expr *DstExpr) {
    auto *DstRef = extractDeclOrMemberRef(DstExpr);
    if (!DstRef)
      return Action::Continue(OriginalExpr);

    ValueDecl *DstDecl;
    if (auto *DRE = dyn_cast<DeclRefExpr>(DstRef))
      DstDecl = DRE->getDecl();
    else {
      auto *MRE = cast<MemberRefExpr>(DstRef);
      DstDecl = MRE->getMember().getDecl();
    }
    if (!DstDecl->hasName())
      return Action::Continue(OriginalExpr);

    // Don't capture variables which aren't default-initialized.
    if (auto *VD = dyn_cast<VarDecl>(DstDecl))
      if (!VD->isParentExecutabledInitialized() &&
          !(isa<ParamDecl>(VD) &&
            cast<ParamDecl>(VD)->isInOut()))
        return Action::Continue(OriginalExpr);

    // Rewrite the original expression into this:
    // call
    //   closure {
    //     $OriginalExpr
    //     checkExpect("$Varname", _stringForPrintObject($Varname))
    //   }

    // Create "$Varname".
    llvm::SmallString<256> DstNameBuf;
    const DeclName DstDN = DstDecl->getName();
    StringRef DstName = Ctx.AllocateCopy(DstDN.getString(DstNameBuf));
    assert(!DstName.empty() && "Varname must be non-empty");
    Expr *Varname = new (Ctx) StringLiteralExpr(DstName, SourceRange());
    Varname->setImplicit(true);

    // Create _stringForPrintObject($Varname).
    auto *PODeclRef = new (Ctx)
        UnresolvedDeclRefExpr(StringForPrintObjectName,
                              DeclRefKind::Ordinary, DeclNameLoc());

    std::function<DeclRefExpr *(DeclRefExpr *)> cloneDeclRef =
        [&](DeclRefExpr *DRE) {
          auto *ref = new (Ctx) DeclRefExpr(
              DRE->getDeclRef(),
              /*Loc=*/DeclNameLoc(),
              /*Implicit=*/true, DRE->getAccessSemantics(), DRE->getType());

          if (auto hopTarget = DRE->isImplicitlyAsync())
            ref->setImplicitlyAsync(*hopTarget);

          ref->setImplicitlyThrows(DRE->isImplicitlyThrows());

          return ref;
        };

    std::function<MemberRefExpr *(MemberRefExpr *)> cloneMemberRef =
        [&](MemberRefExpr *M) {
          auto *base = M->getBase();

          if (auto *DRE = dyn_cast<DeclRefExpr>(base))
            base = cloneDeclRef(DRE);
          else if (auto *M = dyn_cast<MemberRefExpr>(base))
            base = cloneMemberRef(M);

          auto *ref = new (Ctx)
              MemberRefExpr(base, /*dotLoc=*/SourceLoc(), M->getDecl(),
                            /*loc=*/DeclNameLoc(),
                            /*Implicit=*/true, M->getAccessSemantics());
          ref->setType(M->getType());

          return ref;
        };

    // Let's make a copy of either decl or member ref without source
    // information. It's invalid to have decl reference expressions
    // reused, each reference should get a fresh expression.
    if (auto *DRE = dyn_cast<DeclRefExpr>(DstRef)) {
      DstRef = cloneDeclRef(DRE);
    } else {
      DstRef = cloneMemberRef(cast<MemberRefExpr>(DstRef));
    }

    auto *POArgList = ArgumentList::forImplicitUnlabeled(Ctx, {DstRef});
    auto *POCall = CallExpr::createImplicit(Ctx, PODeclRef, POArgList);
    POCall->setThrows(false);

    // Create the call to checkExpect.
    UnresolvedDeclRefExpr *CheckExpectDRE = new (Ctx)
        UnresolvedDeclRefExpr(DebuggerTestingCheckExpectName,
                              DeclRefKind::Ordinary, DeclNameLoc());
    auto *CheckArgList =
        ArgumentList::forImplicitUnlabeled(Ctx, {Varname, POCall});
    auto *CheckExpectExpr =
        CallExpr::createImplicit(Ctx, CheckExpectDRE, CheckArgList);
    CheckExpectExpr->setThrows(false);

    // Create the closure.
    auto *Params = ParameterList::createEmpty(Ctx);
    auto *Closure = new (Ctx)
        ClosureExpr(DeclAttributes(), SourceRange(), nullptr, Params,
                    SourceLoc(), SourceLoc(),
                    SourceLoc(), SourceLoc(), nullptr,
                    getCurrentDeclContext());
    Closure->setImplicit(true);
    Closure->setDiscriminator(DF.getNextDiscriminator());

    // TODO: Save and return the value of $OriginalExpr.
    ASTNode ClosureElements[] = {OriginalExpr, CheckExpectExpr};
    auto *ClosureBody = BraceStmt::create(Ctx, SourceLoc(), ClosureElements,
                                          SourceLoc(), /*Implicit=*/true);
    Closure->setBody(ClosureBody, /*isSingleExpression=*/false);

    // Call the closure.
    auto *ClosureCall = CallExpr::createImplicitEmpty(Ctx, Closure);
    ClosureCall->setThrows(false);

    // TODO: typeCheckExpression() seems to assign types to everything here,
    // but may not be sufficient in some cases.
    Expr *FinalExpr = ClosureCall;
    if (!TypeChecker::typeCheckExpression(FinalExpr, getCurrentDeclContext(),
                                          /*contextualInfo=*/{}))
      llvm::report_fatal_error("Could not type-check instrumentation");

    // Captures have to be computed after the closure is type-checked. This
    // ensures that the type checker can infer <noescape> for captured values.
    TypeChecker::computeCaptures(Closure);

    return Action::SkipChildren(FinalExpr);
  }
};

} // end anonymous namespace

void swift::performDebuggerTestingTransform(SourceFile &SF) {
  // Walk over all decls in the file to find the next available closure
  // discriminator.
  DiscriminatorFinder DF;
  for (Decl *D : SF.getTopLevelDecls())
    D->walk(DF);

  // Instrument the decls with checkExpect() sanity-checks.
  for (Decl *D : SF.getTopLevelDecls()) {
    DebuggerTestingTransform Transform{D->getASTContext(), DF};
    D->walk(Transform);
    swift::verify(D);
  }
}
