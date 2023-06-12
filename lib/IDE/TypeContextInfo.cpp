//===--- TypeContextInfo.cpp ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IDE/TypeContextInfo.h"
#include "ExprContextAnalysis.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/USRGeneration.h"
#include "swift/IDE/TypeCheckCompletionCallback.h"
#include "swift/Parse/IDEInspectionCallbacks.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/IDETypeChecking.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/SmallSet.h"

using namespace swift;
using namespace ide;

class ContextInfoCallbacks : public CodeCompletionCallbacks,
                             public DoneParsingCallback {
  TypeContextInfoConsumer &Consumer;
  SourceLoc Loc;
  Expr *ParsedExpr = nullptr;
  DeclContext *CurDeclContext = nullptr;

  void getImplicitMembers(Type T, SmallVectorImpl<ValueDecl *> &Result);

public:
  ContextInfoCallbacks(Parser &P, TypeContextInfoConsumer &Consumer)
      : CodeCompletionCallbacks(P), DoneParsingCallback(), Consumer(Consumer) {}

  void completePostfixExprBeginning(CodeCompletionExpr *E) override;
  void completeForEachSequenceBeginning(CodeCompletionExpr *E) override;
  void completeCaseStmtBeginning(CodeCompletionExpr *E) override;

  void completeCallArg(CodeCompletionExpr *E, bool isFirst) override;
  void completeReturnStmt(CodeCompletionExpr *E) override;
  void completeYieldStmt(CodeCompletionExpr *E,
                         Optional<unsigned> yieldIndex) override;

  void completeUnresolvedMember(CodeCompletionExpr *E,
                                SourceLoc DotLoc) override;

  void doneParsing(SourceFile *SrcFile) override;
};

void ContextInfoCallbacks::completePostfixExprBeginning(CodeCompletionExpr *E) {
  CurDeclContext = P.CurDeclContext;
  ParsedExpr = E;
}

void ContextInfoCallbacks::completeForEachSequenceBeginning(
    CodeCompletionExpr *E) {
  CurDeclContext = P.CurDeclContext;
  ParsedExpr = E;
}
void ContextInfoCallbacks::completeCallArg(CodeCompletionExpr *E,
                                           bool isFirst) {
  CurDeclContext = P.CurDeclContext;
  ParsedExpr = E;
}
void ContextInfoCallbacks::completeReturnStmt(CodeCompletionExpr *E) {
  CurDeclContext = P.CurDeclContext;
  ParsedExpr = E;
}
void ContextInfoCallbacks::completeYieldStmt(CodeCompletionExpr *E,
                                             Optional<unsigned> yieldIndex) {
  CurDeclContext = P.CurDeclContext;
  ParsedExpr = E;
}
void ContextInfoCallbacks::completeUnresolvedMember(CodeCompletionExpr *E,
                                                    SourceLoc DotLoc) {
  CurDeclContext = P.CurDeclContext;
  ParsedExpr = E;
}

void ContextInfoCallbacks::completeCaseStmtBeginning(CodeCompletionExpr *E) {
  // TODO: Implement?
}

class TypeContextInfoCallback : public TypeCheckCompletionCallback {
  Expr *ParsedExpr;
  SmallVector<Type, 2> Types;

  void sawSolutionImpl(const constraints::Solution &S) override {
    if (!S.hasType(ParsedExpr)) {
      return;
    }
    if (Type T = getTypeForCompletion(S, ParsedExpr)) {
      Types.push_back(T);
    }
  }

public:
  TypeContextInfoCallback(Expr *ParsedExpr) : ParsedExpr(ParsedExpr) {}

  ArrayRef<Type> getTypes() const { return Types; }
};

void ContextInfoCallbacks::doneParsing(SourceFile *SrcFile) {
  if (!ParsedExpr)
    return;

  TypeContextInfoCallback TypeCheckCallback(ParsedExpr);
  {
    llvm::SaveAndRestore<TypeCheckCompletionCallback *> CompletionCollector(
        Context.CompletionCallback, &TypeCheckCallback);
    typeCheckContextAt(
        TypeCheckASTNodeAtLocContext::declContext(CurDeclContext),
        ParsedExpr->getLoc());
  }

  llvm::SmallSet<CanType, 2> seenTypes;
  SmallVector<TypeContextInfoItem, 2> results;

  for (auto T : TypeCheckCallback.getTypes()) {
    if (T->is<ErrorType>() || T->is<UnresolvedType>())
      continue;

    T = T->getRValueType();
    if (T->hasArchetype())
      T = T->mapTypeOutOfContext();

    // TODO: Do we need '.none' for Optionals?
    auto objT = T->lookThroughAllOptionalTypes();

    if (auto env = CurDeclContext->getGenericEnvironmentOfContext())
      objT = env->mapTypeIntoContext(T);

    if (!seenTypes.insert(objT->getCanonicalType()).second)
      continue;

    results.emplace_back(T);
    auto &item = results.back();
    getImplicitMembers(objT, item.ImplicitMembers);
  }

  Consumer.handleResults(results);
}

void ContextInfoCallbacks::getImplicitMembers(
    Type T, SmallVectorImpl<ValueDecl *> &Result) {

  if (!T->mayHaveMembers())
    return;

  class LocalConsumer : public VisibleDeclConsumer {
    DeclContext *DC;
    ModuleDecl *CurModule;
    Type T;
    SmallVectorImpl<ValueDecl *> &Result;

    bool canBeImplictMember(ValueDecl *VD) {
      if (VD->isOperator())
        return false;

      // Enum element decls can always be referenced by implicit member
      // expression.
      if (isa<EnumElementDecl>(VD))
        return true;

      // Static properties which is convertible to 'Self'.
      if (auto *Var = dyn_cast<VarDecl>(VD)) {
        if (Var->isStatic()) {
          auto declTy = T->getTypeOfMember(CurModule, Var);
          if (declTy->isEqual(T) ||
              swift::isConvertibleTo(declTy, T, /*openArchetypes=*/true, *DC))
            return true;
        }
      }

      return false;
    }

  public:
    LocalConsumer(DeclContext *DC, Type T, SmallVectorImpl<ValueDecl *> &Result)
        : DC(DC), CurModule(DC->getParentModule()), T(T), Result(Result) {}

    void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason,
                   DynamicLookupInfo) override {
      if (canBeImplictMember(VD) && !VD->shouldHideFromEditor())
        Result.push_back(VD);
    }

  } LocalConsumer(CurDeclContext, T, Result);

  lookupVisibleMemberDecls(LocalConsumer, MetatypeType::get(T),
                           Loc, CurDeclContext,
                           /*includeInstanceMembers=*/false,
                           /*includeDerivedRequirements*/false,
                           /*includeProtocolExtensionMembers*/true);
}

IDEInspectionCallbacksFactory *swift::ide::makeTypeContextInfoCallbacksFactory(
    TypeContextInfoConsumer &Consumer) {

  // CC callback factory which produces 'ContextInfoCallbacks'.
  class ContextInfoCallbacksFactoryImpl
      : public IDEInspectionCallbacksFactory {
    TypeContextInfoConsumer &Consumer;

  public:
    ContextInfoCallbacksFactoryImpl(TypeContextInfoConsumer &Consumer)
        : Consumer(Consumer) {}

    Callbacks createCallbacks(Parser &P) override {
      auto Callbacks = std::make_shared<ContextInfoCallbacks>(P, Consumer);
      return {Callbacks, Callbacks};
    }
  };

  return new ContextInfoCallbacksFactoryImpl(Consumer);
}
