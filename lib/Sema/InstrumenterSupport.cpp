//===--- InstrumenterSupport.cpp - Instrumenter Support -------------------===//
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
//  This file implements the supporting functions for writing instrumenters of
//  the Swift AST.
//
//===----------------------------------------------------------------------===//

#include "InstrumenterSupport.h"
#include "swift/AST/DiagnosticSuppression.h"
#include "swift/AST/SourceFile.h"
#include "swift/Demangling/Punycode.h"
#include "llvm/Support/Path.h"

using namespace swift;
using namespace swift::instrumenter_support;

namespace {

class ErrorGatherer : public DiagnosticConsumer {
private:
  bool error = false;
  DiagnosticEngine &diags;

public:
  ErrorGatherer(DiagnosticEngine &diags) : diags(diags) {
    diags.addConsumer(*this);
  }
  ~ErrorGatherer() override { diags.takeConsumers(); }
  void handleDiagnostic(SourceManager &SM,
                        const DiagnosticInfo &Info) override {
    if (Info.Kind == swift::DiagnosticKind::Error) {
      error = true;
    }
    DiagnosticEngine::formatDiagnosticText(llvm::errs(), Info.FormatString,
                                           Info.FormatArgs);
    llvm::errs() << "\n";
  }
  bool hadError() { return error; }
};


class ErrorFinder : public ASTWalker {
  bool error = false;

public:
  ErrorFinder() {}

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (isa<ErrorExpr>(E) || !E->getType() || E->getType()->hasError())
      error = true;

    return Action::StopIf(error, E);
  }
  PreWalkAction walkToDeclPre(Decl *D) override {
    if (auto *VD = dyn_cast<ValueDecl>(D)) {
      if (!VD->hasInterfaceType() || VD->getInterfaceType()->hasError())
        error = true;
    }
    return Action::StopIf(error);
  }
  bool hadError() { return error; }
};
} // end anonymous namespace

InstrumenterBase::InstrumenterBase(ASTContext &C, DeclContext *DC)
    : Context(C), TypeCheckDC(DC), CF(*this) {
  // Prefixes for module and file vars
  const std::string builtinPrefix = "__builtin";
  const std::string modulePrefix = "_pg_module_";
  const std::string filePrefix = "_pg_file_";

  // Setup Module identifier
  std::string moduleName =
      std::string(TypeCheckDC->getParentModule()->getName());
  Identifier moduleIdentifier =
      Context.getIdentifier(builtinPrefix + modulePrefix + moduleName);

  SmallVector<ValueDecl *, 1> results;
  TypeCheckDC->getParentModule()->lookupValue(
      moduleIdentifier, NLKind::UnqualifiedLookup, results);

  if (results.size() == 1)
    ModuleIdentifier = results.front()->createNameRef();

  // Setup File identifier
  StringRef filePath = TypeCheckDC->getParentSourceFile()->getFilename();
  StringRef fileName = llvm::sys::path::stem(filePath);

  std::string filePunycodeName;
  Punycode::encodePunycodeUTF8(fileName, filePunycodeName, true);
  Identifier fileIdentifier =
      Context.getIdentifier(builtinPrefix + modulePrefix + moduleName +
                            filePrefix + filePunycodeName);

  results.clear();
  TypeCheckDC->getParentModule()->lookupValue(
      fileIdentifier, NLKind::UnqualifiedLookup, results);

  if (results.size() == 1)
    FileIdentifier = results.front()->createNameRef();
}

void InstrumenterBase::anchor() {}

bool InstrumenterBase::doTypeCheckImpl(ASTContext &Ctx, DeclContext *DC,
                                       Expr * &parsedExpr) {
  DiagnosticSuppression suppression(Ctx.Diags);
  ErrorGatherer errorGatherer(Ctx.Diags);

  TypeChecker::typeCheckExpression(parsedExpr, DC, /*contextualInfo=*/{});

  if (parsedExpr) {
    ErrorFinder errorFinder;
    parsedExpr->walk(errorFinder);
    if (!errorFinder.hadError() && !errorGatherer.hadError()) {
      return true;
    }
  }

  return false;
}

Expr *InstrumenterBase::buildIDArgumentExpr(llvm::Optional<DeclNameRef> name,
                                            SourceRange SR) {
  if (!name)
    return IntegerLiteralExpr::createFromUnsigned(Context, 0, SR.End);

  return new (Context) UnresolvedDeclRefExpr(*name, DeclRefKind::Ordinary,
                                             DeclNameLoc(SR.End));
}
