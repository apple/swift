//===--- SILLocation.cpp - Location information for SIL nodes -------------===//
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

#include "swift/SIL/SILLocation.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Stmt.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;


SourceLoc SILLocation::getSourceLoc() const {
  if (isSILFile())
    return Loc.SILFileLoc;

  // Don't crash if the location is a DebugLoc.
  // TODO: this is a workaround until rdar://problem/25225083 is implemented.
  if (isDebugInfoLoc())
    return SourceLoc();

  return getSourceLoc(Loc.ASTNode.Primary);
}

SourceLoc SILLocation::getSourceLoc(ASTNodeTy N) const {
  if (N.isNull())
    return SourceLoc();

  if (alwaysPointsToStart() ||
      alwaysPointsToEnd() ||
      is<CleanupLocation>() ||
      is<ImplicitReturnLocation>())
    return getEndSourceLoc(N);

  // Use the start location for the ReturnKind.
  if (is<ReturnLocation>())
    return getStartSourceLoc(N);

  if (auto *decl = N.dyn_cast<Decl*>())
    return decl->getLoc();
  if (auto *expr = N.dyn_cast<Expr*>())
    return expr->getLoc();
  if (auto *stmt = N.dyn_cast<Stmt*>())
    return stmt->getStartLoc();
  if (auto *patt = N.dyn_cast<Pattern*>())
    return patt->getStartLoc();
  llvm_unreachable("impossible SILLocation");
}

SourceLoc SILLocation::getDebugSourceLoc() const {
  assert(!isDebugInfoLoc());

  if (isSILFile())
    return Loc.SILFileLoc;

  if (auto *expr = Loc.ASTNode.Primary.dyn_cast<Expr*>()) {
    // Code that has an autoclosure as location should not show up in
    // the line table (rdar://problem/14627460). Note also that the
    // closure function still has a valid DW_AT_decl_line.  Depending
    // on how we decide to resolve rdar://problem/14627460, we may
    // want to use the regular getLoc instead and rather use the
    // column info.
    if (isa<AutoClosureExpr>(expr))
      return SourceLoc();
  }

  if (Loc.ASTNode.ForDebugger)
    return getSourceLoc(Loc.ASTNode.ForDebugger);

  return getSourceLoc(Loc.ASTNode.Primary);
}

SourceLoc SILLocation::getStartSourceLoc() const {
  if (isAutoGenerated())
    return SourceLoc();
  if (isSILFile())
    return Loc.SILFileLoc;
  return getStartSourceLoc(Loc.ASTNode.Primary);
}

SourceLoc SILLocation::getStartSourceLoc(ASTNodeTy N) const {
  if (auto *decl = N.dyn_cast<Decl*>())
    return decl->getStartLoc();
  if (auto *expr = N.dyn_cast<Expr*>())
    return expr->getStartLoc();
  if (auto *stmt = N.dyn_cast<Stmt*>())
    return stmt->getStartLoc();
  if (auto *patt = N.dyn_cast<Pattern*>())
    return patt->getStartLoc();
  llvm_unreachable("impossible SILLocation");
}

SourceLoc SILLocation::getEndSourceLoc() const {
  if (isAutoGenerated())
    return SourceLoc();
  if (isSILFile())
    return Loc.SILFileLoc;
  return getEndSourceLoc(Loc.ASTNode.Primary);
}

SourceLoc SILLocation::getEndSourceLoc(ASTNodeTy N) const {
  if (auto decl = N.dyn_cast<Decl*>())
    return decl->getEndLoc();
  if (auto expr = N.dyn_cast<Expr*>())
    return expr->getEndLoc();
  if (auto stmt = N.dyn_cast<Stmt*>())
    return stmt->getEndLoc();
  if (auto patt = N.dyn_cast<Pattern*>())
    return patt->getEndLoc();
  llvm_unreachable("impossible SILLocation");
}

DeclContext *SILLocation::getAsDeclContext() const {
  if (!isASTNode())
    return nullptr;
  if (auto *D = getAsASTNode<Decl>())
    switch (D->getKind()) {
    // These four dual-inherit from DeclContext.
    case DeclKind::Func:        return cast<FuncDecl>(D);
    case DeclKind::Constructor: return cast<ConstructorDecl>(D);
    case DeclKind::Extension:   return cast<ExtensionDecl>(D);
    case DeclKind::Destructor:  return cast<DestructorDecl>(D);
    default:                    return D->getDeclContext();
    }
  if (auto *E = getAsASTNode<Expr>())
    if (auto *DC = dyn_cast<AbstractClosureExpr>(E))
      return DC;
  return nullptr;
}

SILLocation::DebugLoc SILLocation::decode(SourceLoc Loc,
                                          const SourceManager &SM) {
  DebugLoc DL;
  if (Loc.isValid()) {
    DL.Filename = SM.getBufferIdentifierForLoc(Loc);
    std::tie(DL.Line, DL.Column) = SM.getLineAndColumn(Loc);
  }
  return DL;
}

void SILLocation::dump(const SourceManager &SM) const {
  if (auto D = getAsASTNode<Decl>())
    llvm::errs() << Decl::getKindName(D->getKind()) << "Decl @ ";
  if (auto E = getAsASTNode<Expr>())
    llvm::errs() << Expr::getKindName(E->getKind()) << "Expr @ ";
  if (auto S = getAsASTNode<Stmt>())
    llvm::errs() << Stmt::getKindName(S->getKind()) << "Stmt @ ";
  if (auto P = getAsASTNode<Pattern>())
    llvm::errs() << Pattern::getKindName(P->getKind()) << "Pattern @ ";

  print(llvm::errs(), SM);

  if (isAutoGenerated())     llvm::errs() << ":auto";
  if (alwaysPointsToStart()) llvm::errs() << ":start";
  if (alwaysPointsToEnd())   llvm::errs() << ":end";
  if (isInTopLevel())        llvm::errs() << ":toplevel";
  if (isInPrologue())        llvm::errs() << ":prologue";
  if (isSILFile())           llvm::errs() << ":sil";
  if (hasDebugLoc()) {
    llvm::errs() << ":debug[";
    getDebugSourceLoc().print(llvm::errs(), SM);
    llvm::errs() << "]\n";
  }
}

void SILLocation::print(raw_ostream &OS, const SourceManager &SM) const {
  if (isNull())
    OS << "<no loc>";
  getSourceLoc().print(OS, SM);
}

InlinedLocation InlinedLocation::getInlinedLocation(SILLocation L) {
  if (Expr *E = L.getAsASTNode<Expr>())
    return InlinedLocation(E, L.getSpecialFlags());
  if (Stmt *S = L.getAsASTNode<Stmt>())
    return InlinedLocation(S, L.getSpecialFlags());
  if (Pattern *P = L.getAsASTNode<Pattern>())
    return InlinedLocation(P, L.getSpecialFlags());
  if (Decl *D = L.getAsASTNode<Decl>())
    return InlinedLocation(D, L.getSpecialFlags());

  if (L.isSILFile())
    return InlinedLocation(L.Loc.SILFileLoc, L.getSpecialFlags());

  if (L.isInTopLevel())
    return InlinedLocation::getModuleLocation(L.getSpecialFlags());

  if (L.isAutoGenerated()) {
    InlinedLocation IL;
    IL.markAutoGenerated();
    return IL;
  }
  llvm_unreachable("Cannot construct Inlined loc from the given location.");
}

MandatoryInlinedLocation
MandatoryInlinedLocation::getMandatoryInlinedLocation(SILLocation L) {
  if (Expr *E = L.getAsASTNode<Expr>())
    return MandatoryInlinedLocation(E, L.getSpecialFlags());
  if (Stmt *S = L.getAsASTNode<Stmt>())
    return MandatoryInlinedLocation(S, L.getSpecialFlags());
  if (Pattern *P = L.getAsASTNode<Pattern>())
    return MandatoryInlinedLocation(P, L.getSpecialFlags());
  if (Decl *D = L.getAsASTNode<Decl>())
    return MandatoryInlinedLocation(D, L.getSpecialFlags());

  if (L.isSILFile())
    return MandatoryInlinedLocation(L.Loc.SILFileLoc, L.getSpecialFlags());

  if (L.isInTopLevel())
    return MandatoryInlinedLocation::getModuleLocation(L.getSpecialFlags());

  llvm_unreachable("Cannot construct Inlined loc from the given location.");
}

CleanupLocation CleanupLocation::get(SILLocation L) {
  if (Expr *E = L.getAsASTNode<Expr>())
    return CleanupLocation(E, L.getSpecialFlags());
  if (Stmt *S = L.getAsASTNode<Stmt>())
    return CleanupLocation(S, L.getSpecialFlags());
  if (Pattern *P = L.getAsASTNode<Pattern>())
    return CleanupLocation(P, L.getSpecialFlags());
  if (Decl *D = L.getAsASTNode<Decl>())
    return CleanupLocation(D, L.getSpecialFlags());
  if (L.isNull())
    return CleanupLocation();
  if (L.isSILFile())
    return CleanupLocation();
  if (L.isDebugInfoLoc() && L.isAutoGenerated())
    return CleanupLocation();
  llvm_unreachable("Cannot construct Cleanup loc from the "
                   "given location.");
}

ReturnLocation::ReturnLocation(ReturnStmt *RS) : SILLocation(RS, ReturnKind) {}

ReturnLocation::ReturnLocation(BraceStmt *BS) : SILLocation(BS, ReturnKind) {}

ReturnStmt *ReturnLocation::get() {
  return castToASTNode<ReturnStmt>();
}

ImplicitReturnLocation::ImplicitReturnLocation(AbstractClosureExpr *E)
  : SILLocation(E, ImplicitReturnKind) { }

ImplicitReturnLocation::ImplicitReturnLocation(ReturnStmt *S)
  : SILLocation(S, ImplicitReturnKind) { }

ImplicitReturnLocation::ImplicitReturnLocation(AbstractFunctionDecl *AFD)
  : SILLocation(AFD, ImplicitReturnKind) { }

SILLocation ImplicitReturnLocation::getImplicitReturnLoc(SILLocation L) {
  assert(L.isASTNode<Expr>() ||
         L.isASTNode<ValueDecl>() ||
         L.isASTNode<PatternBindingDecl>() ||
         (L.isNull() && L.isInTopLevel()));
  L.setLocationKind(ImplicitReturnKind);
  return L;
}

AbstractClosureExpr *ImplicitReturnLocation::get() {
  return castToASTNode<AbstractClosureExpr>();
}
