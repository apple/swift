//===--- InstrumenterSupport.h - Instrumenter Support -----------*- C++ -*-===//
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

#include "TypeChecker.h"
#include "swift/AST/ASTWalker.h"

namespace swift {
namespace instrumenter_support {

template <class E> class Added {
private:
  E Contents;

public:
  Added() {}
  Added(E NewContents) : Contents(NewContents) {}
  Added(const Added<E> &rhs) : Contents(rhs.Contents) {}
  const Added<E> &operator=(const Added<E> &rhs) {
    Contents = rhs.Contents;
    return *this;
  }
  E &operator*() { return Contents; }
  E &operator->() { return Contents; }
};

class InstrumenterBase {

protected:
  ASTContext &Context;
  DeclContext *TypeCheckDC;
  llvm::Optional<DeclNameRef> ModuleIdentifier;
  llvm::Optional<DeclNameRef> FileIdentifier;

  InstrumenterBase(ASTContext &C, DeclContext *DC);
  virtual ~InstrumenterBase() = default;
  virtual void anchor();
  virtual BraceStmt *transformBraceStmt(BraceStmt *BS,
                                        const ParameterList *PL = nullptr,
                                        bool TopLevel = false) = 0;

  /// Create an expression which retrieves a valid ModuleIdentifier or
  /// FileIdentifier, if available.
  Expr *buildIDArgumentExpr(llvm::Optional<DeclNameRef> name, SourceRange SR);

  class ClosureFinder : public ASTWalker {
  private:
    InstrumenterBase &I;

  public:
    ClosureFinder(InstrumenterBase &Inst) : I(Inst) {}

    /// Walk only the expansion of the macro.
    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
      if (isa<BraceStmt>(S)) {
        return Action::SkipChildren(S); // don't walk into brace statements; we
                           // need to respect nesting!
      } else {
        return Action::Continue(S);
      }
    }
    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      if (auto *CE = dyn_cast<ClosureExpr>(E)) {
        BraceStmt *B = CE->getBody();
        if (B) {
          const ParameterList *PL = CE->getParameters();
          BraceStmt *NB = I.transformBraceStmt(B, PL);
          CE->setBody(NB, false);
          // just with the entry and exit logging this is going to
          // be more than a single expression!
        }
      }
      return Action::Continue(E);
    }
  };

  ClosureFinder CF;

  template <class T>
  bool doTypeCheck(ASTContext &Ctx, DeclContext *DC, Added<T *> &parsedExpr) {
    Expr *E = *parsedExpr;
    bool result = doTypeCheckImpl(Ctx, DC, E);
    parsedExpr = Added<T *>(dyn_cast<T>(E));
    return result;
  }
  
private:
  bool doTypeCheckImpl(ASTContext &Ctx, DeclContext *DC, Expr * &parsedExpr);
};
}
}
