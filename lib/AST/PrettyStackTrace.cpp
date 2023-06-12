//===--- PrettyStackTrace.cpp - Swift-specific PrettyStackTraceEntries ----===//
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
//  This file implements several Swift-specific implementations of
//  PrettyStackTraceEntry.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Module.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/TypeRepr.h"
#include "swift/AST/TypeVisitor.h"
#include "swift/Basic/SourceManager.h"
#include "clang/AST/Type.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace swift;

void PrettyStackTraceDecl::print(llvm::raw_ostream &out) const {
  out << "While " << Action << ' ';
  if (!TheDecl) {
    out << "NULL declaration!\n";
    return;
  }
  printDeclDescription(out, TheDecl, TheDecl->getASTContext());
}

void PrettyStackTraceDeclAndSubst::print(llvm::raw_ostream &out) const {
  out << "While " << action << ' ';
  if (!decl) {
    out << "NULL declaration!\n";
    return;
  }
  printDeclDescription(out, decl, decl->getASTContext());

  out << "with substitution map: ";
  subst.dump(out);
}


void swift::printDeclDescription(llvm::raw_ostream &out, const Decl *D,
                                 const ASTContext &Context, bool addNewline) {
  SourceLoc loc = D->getStartLoc();
  bool hasPrintedName = false;
  if (auto *named = dyn_cast<ValueDecl>(D)) {
    if (named->hasName()) {
      out << '\'' << named->getName() << '\'';
      hasPrintedName = true;
    } else if (auto *accessor = dyn_cast<AccessorDecl>(named)) {
      auto ASD = accessor->getStorage();
      if (ASD->hasName()) {
        switch (accessor->getAccessorKind()) {
        case AccessorKind::Get:
          out << "getter";
          break;
        case AccessorKind::Set:
          out << "setter";
          break;
        case AccessorKind::WillSet:
          out << "willset";
          break;
        case AccessorKind::DidSet:
          out << "didset";
          break;
        case AccessorKind::Address:
          out << "addressor";
          break;
        case AccessorKind::MutableAddress:
          out << "mutableAddressor";
          break;
        case AccessorKind::Read:
          out << "read";
          break;
        case AccessorKind::Modify:
          out << "modify";
          break;
        case AccessorKind::Init:
          out << "init";
          break;
        }

        out << " for " << ASD->getName();
        hasPrintedName = true;
        loc = ASD->getStartLoc();
      }
    }
  } else if (auto *extension = dyn_cast<ExtensionDecl>(D)) {
    Type extendedTy = extension->getExtendedType();
    if (extendedTy) {
      out << "extension of " << extendedTy;
      hasPrintedName = true;
    }
  }

  if (!hasPrintedName)
    out << "declaration " << (const void *)D;

  if (loc.isValid()) {
    out << " (at ";
    loc.print(out, Context.SourceMgr);
    out << ')';
  } else {
    out << " (in module '" << D->getModuleContext()->getName() << "')";
  }
  if (addNewline) out << '\n';
}

void PrettyStackTraceAnyFunctionRef::print(llvm::raw_ostream &out) const {
  out << "While " << Action << ' ';
  auto &Context = TheRef.getAsDeclContext()->getASTContext();
  if (auto *AFD = TheRef.getAbstractFunctionDecl())
    printDeclDescription(out, AFD, Context);
  else {
    auto *ACE = TheRef.getAbstractClosureExpr();
    printExprDescription(out, ACE, Context);
  }
}

void PrettyStackTraceFreestandingMacroExpansion::print(
    llvm::raw_ostream &out) const {
  out << "While " << Action << ' ';
  auto &Context = Expansion->getDeclContext()->getASTContext();
  switch (Expansion->getFreestandingMacroKind()) {
  case FreestandingMacroKind::Expr:
    printExprDescription(out, cast<MacroExpansionExpr>(Expansion), Context);
    break;
  case FreestandingMacroKind::Decl:
    printDeclDescription(out, cast<MacroExpansionDecl>(Expansion), Context);
    break;
  }
}

void PrettyStackTraceExpr::print(llvm::raw_ostream &out) const {
  out << "While " << Action << ' ';
  if (!TheExpr) {
    out << "NULL expression!\n";
    return;
  }
  printExprDescription(out, TheExpr, Context);
}

void swift::printExprDescription(llvm::raw_ostream &out, const Expr *E,
                                 const ASTContext &Context, bool addNewline) {
  out << "expression at ";
  E->getSourceRange().print(out, Context.SourceMgr);
  if (addNewline) out << '\n';
}

void PrettyStackTraceStmt::print(llvm::raw_ostream &out) const {
  out << "While " << Action << ' ';
  if (!TheStmt) {
    out << "NULL statement!\n";
    return;
  }
  printStmtDescription(out, TheStmt, Context);
}

void swift::printStmtDescription(llvm::raw_ostream &out, Stmt *S,
                                 const ASTContext &Context, bool addNewline) {
  out << "statement at ";
  S->getSourceRange().print(out, Context.SourceMgr);
  if (addNewline) out << '\n';
}

void PrettyStackTracePattern::print(llvm::raw_ostream &out) const {
  out << "While " << Action << ' ';
  if (!ThePattern) {
    out << "NULL pattern!\n";
    return;
  }
  printPatternDescription(out, ThePattern, Context);
}

void swift::printPatternDescription(llvm::raw_ostream &out, Pattern *P,
                                    const ASTContext &Context,
                                    bool addNewline) {
  out << "pattern at ";
  P->getSourceRange().print(out, Context.SourceMgr);
  if (addNewline) out << '\n';
}

namespace {
  /// Map a Type to an interesting declaration whose source range we
  /// should print.
  struct InterestingDeclForType
      : TypeVisitor<InterestingDeclForType, Decl*> {
    Decl *visitType(TypeBase *type) {
      return nullptr;
    }
    Decl *visitUnboundGenericType(UnboundGenericType *type) {
      return type->getDecl();
    }
    Decl *visitBoundGenericType(BoundGenericType *type) {
      return type->getDecl();
    }
    Decl *visitNominalType(NominalType *type) {
      return type->getDecl();
    }
    Decl *visitTypeAliasType(TypeAliasType *type) {
      return type->getDecl();
    }
  };
} // end anonymous namespace

void PrettyStackTraceType::print(llvm::raw_ostream &out) const {
  out << "While " << Action << ' ';
  if (TheType.isNull()) {
    out << "NULL type!\n";
    return;
  }
  printTypeDescription(out, TheType, Context);
}

void swift::printTypeDescription(llvm::raw_ostream &out, Type type,
                                 const ASTContext &Context, bool addNewline) {
  out << "type '" << type << '\'';
  if (Decl *decl = InterestingDeclForType().visit(type)) {
    if (decl->getSourceRange().isValid()) {
      out << " (declared at ";
      decl->getSourceRange().print(out, Context.SourceMgr);
      out << ')';
    }
  }
  if (addNewline) out << '\n';
}

void PrettyStackTraceClangType::print(llvm::raw_ostream &out) const {
  out << "While " << Action << ' ';
  if (TheType == nullptr) {
    out << "NULL clang type!\n";
    return;
  }
  TheType->dump(out, Context);
}

void PrettyStackTraceTypeRepr::print(llvm::raw_ostream &out) const {
  out << "While " << Action << " type ";
  TheType->print(out);
  if (TheType && TheType->getSourceRange().isValid()) {
    out << " at ";
    TheType->getSourceRange().print(out, Context.SourceMgr);
  }
  out << '\n';
}

void PrettyStackTraceConformance::print(llvm::raw_ostream &out) const {
  out << "While " << Action << ' ';
  auto &Context = Conformance->getDeclContext()->getASTContext();
  printConformanceDescription(out, Conformance, Context);
}

void swift::printConformanceDescription(llvm::raw_ostream &out,
                                        const ProtocolConformance *conformance,
                                        const ASTContext &ctxt,
                                        bool addNewline) {
  if (!conformance) {
    out << "NULL protocol conformance!";
    if (addNewline) out << '\n';
    return;
  }

  out << "protocol conformance to ";
  printDeclDescription(out, conformance->getProtocol(), ctxt, /*newline*/false);
  out << " for ";
  printTypeDescription(out, conformance->getType(), ctxt, addNewline);
}

void swift::printSourceLocDescription(llvm::raw_ostream &out,
                                      SourceLoc loc, const ASTContext &ctx,
                                      bool addNewline) {
  loc.print(out, ctx.SourceMgr);
  if (addNewline) out << '\n';
}

void PrettyStackTraceLocation::print(llvm::raw_ostream &out) const {
  out << "While " << Action << " starting at ";
  printSourceLocDescription(out, Loc, Context);
}

void PrettyStackTraceGenericSignature::print(llvm::raw_ostream &out) const {
  out << "While " << Action << " generic signature ";
  GenericSig->print(out);
  if (Requirement) {
    out << " in requirement #" << *Requirement;
  }
  out << '\n';
}

void PrettyStackTraceSelector::print(llvm::raw_ostream &out) const {
  out << "While " << Action << " '" << Selector << "'";
}

void PrettyStackTraceDifferentiabilityWitness::print(
    llvm::raw_ostream &out) const {
  out << "While " << Action << ' ';
  printDifferentiabilityWitnessDescription(out, Key);
}

void swift::printDifferentiabilityWitnessDescription(
    llvm::raw_ostream &out, const SILDifferentiabilityWitnessKey key,
    bool addNewline) {
  key.print(out);
  if (addNewline)
    out << '\n';
}

void PrettyStackTraceDeclContext::print(llvm::raw_ostream &out) const {
  out << "While " << Action << " in decl context:\n";
  out << "    ---\n";
  DC->printContext(out, /*indent=*/4);
  out << "    ---\n";
}
