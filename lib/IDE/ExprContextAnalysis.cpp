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
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "swift/Basic/SourceManager.h"
#include "swift/IDE/CodeCompletion.h"
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

void swift::ide::typeCheckContextAt(DeclContext *DC, SourceLoc Loc) {
  while (isa<AbstractClosureExpr>(DC))
    DC = DC->getParent();

  // Make sure the extension has been bound.
  {
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
  }

  // Type-check this context.
  switch (DC->getContextKind()) {
  case DeclContextKind::AbstractClosureExpr:
  case DeclContextKind::Module:
  case DeclContextKind::FileUnit:
  case DeclContextKind::SerializedLocal:
  case DeclContextKind::EnumElementDecl:
  case DeclContextKind::GenericTypeDecl:
  case DeclContextKind::SubscriptDecl:
  case DeclContextKind::ExtensionDecl:
    // Nothing to do for these.
    break;

  case DeclContextKind::Initializer:
    if (auto *patternInit = dyn_cast<PatternBindingInitializer>(DC)) {
      if (auto *PBD = patternInit->getBinding()) {
        auto i = patternInit->getBindingIndex();
        PBD->getPattern(i)->forEachVariable(
            [](VarDecl *VD) { (void)VD->getInterfaceType(); });
        if (PBD->getInit(i)) {
          if (!PBD->isInitializerChecked(i))
            typeCheckPatternBinding(PBD, i);
        }
      }
    } else if (auto *defaultArg = dyn_cast<DefaultArgumentInitializer>(DC)) {
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(defaultArg->getParent())) {
        auto *Param = AFD->getParameters()->get(defaultArg->getIndex());
        (void)Param->getTypeCheckedDefaultExpr();
      }
    }
    break;

  case DeclContextKind::TopLevelCodeDecl:
    swift::typeCheckASTNodeAtLoc(DC, Loc);
    break;

  case DeclContextKind::AbstractFunctionDecl: {
    auto *AFD = cast<AbstractFunctionDecl>(DC);
    auto &SM = DC->getASTContext().SourceMgr;
    auto bodyRange = AFD->getBodySourceRange();
    if (SM.rangeContainsTokenLoc(bodyRange, Loc)) {
      swift::typeCheckASTNodeAtLoc(DC, Loc);
    } else {
      assert(bodyRange.isInvalid() && "The body should not be parsed if the "
                                      "completion happens in the signature");
    }
    break;
  }
  }
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

  std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
    if (TargetRange == E->getSourceRange() && !shouldIgnore(E)) {
      assert(!FoundExpr && "non-nullptr for found expr");
      FoundExpr = E;
      return {false, nullptr};
    }
    return {isInterstingRange(E), E};
  }

  std::pair<bool, Pattern *> walkToPatternPre(Pattern *P) override {
    return {isInterstingRange(P), P};
  }

  std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
    return {isInterstingRange(S), S};
  }

  bool walkToTypeReprPre(TypeRepr *T) override { return false; }
};
} // anonymous namespace

Expr *swift::ide::findParsedExpr(const DeclContext *DC,
                                 SourceRange TargetRange) {
  ExprFinder finder(DC->getASTContext().SourceMgr, TargetRange);
  const_cast<DeclContext *>(DC)->walkContext(finder);
  return finder.get();
}

//===----------------------------------------------------------------------===//
// removeCodeCompletionExpr(ASTContext, Expr)
//===----------------------------------------------------------------------===//

namespace {
// TODO: Implement other expressions?
class CCExprRemover: public ASTWalker, public ExprVisitor<CCExprRemover, Expr *> {
  ASTContext &Ctx;

public:
  bool Removed = false;

  CCExprRemover(ASTContext &Ctx) : Ctx(Ctx) {}

  Expr *visitCallExpr(CallExpr *E) {
    SourceLoc lParenLoc, rParenLoc;
    SmallVector<Identifier, 2> argLabels;
    SmallVector<SourceLoc, 2> argLabelLocs;
    SmallVector<Expr *, 2> args;
    SmallVector<TrailingClosure, 2> trailingClosures;
    bool removing = false;

    if (auto paren = dyn_cast<ParenExpr>(E->getArg())) {
      if (isa<CodeCompletionExpr>(paren->getSubExpr())) {
        lParenLoc = paren->getLParenLoc();
        rParenLoc = paren->getRParenLoc();
        removing = true;
      }
    } else if (auto tuple = dyn_cast<TupleExpr>(E->getArg())) {
      lParenLoc = tuple->getLParenLoc();
      rParenLoc = tuple->getRParenLoc();

      assert((!E->getUnlabeledTrailingClosureIndex().hasValue() ||
              (tuple->getNumElements() == E->getArgumentLabels().size() &&
               tuple->getNumElements() == E->getArgumentLabelLocs().size())) &&
             "CallExpr with trailing closure must have the same number of "
             "argument labels");
      assert(tuple->getNumElements() == E->getArgumentLabels().size());
      assert(tuple->getNumElements() == E->getArgumentLabelLocs().size() ||
             E->getArgumentLabelLocs().size() == 0);

      bool hasArgumentLabelLocs = E->getArgumentLabelLocs().size() > 0;

      for (unsigned i = 0, e = tuple->getNumElements(); i != e; ++i) {
        if (isa<CodeCompletionExpr>(tuple->getElement(i))) {
          removing = true;
          continue;
        }

        if (!E->getUnlabeledTrailingClosureIndex().hasValue() ||
            i < *E->getUnlabeledTrailingClosureIndex()) {
          // Normal arguments.
          argLabels.push_back(E->getArgumentLabels()[i]);
          if (hasArgumentLabelLocs)
            argLabelLocs.push_back(E->getArgumentLabelLocs()[i]);
          args.push_back(tuple->getElement(i));
        } else {
          // Trailing closure arguments.
          trailingClosures.emplace_back(E->getArgumentLabels()[i],
                                        E->getArgumentLabelLocs()[i],
                                        tuple->getElement(i));
        }
      }
    }
    if (removing) {
      Removed = true;
      return CallExpr::create(Ctx, E->getFn(), lParenLoc, args, argLabels,
                              argLabelLocs, rParenLoc, trailingClosures,
                              E->isImplicit());
    }
    return E;
  }

  Expr *visitExpr(Expr *E) {
    return E;
  }

  std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
    if (Removed)
      return {false, nullptr};
    E = visit(E);
    return {!Removed, E};
  }

  std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
    if (Removed)
      return {false, nullptr};
    return {true, S};
  }

  bool walkToDeclPre(Decl *D) override {
    return !Removed;
  }
};
}

bool swift::ide::removeCodeCompletionExpr(ASTContext &Ctx, Expr *&expr) {
  CCExprRemover remover(Ctx);
  expr = expr->walk(remover);
  return remover.Removed;
}

//===----------------------------------------------------------------------===//
// collectPossibleReturnTypesFromContext(DeclContext, SmallVectorImpl<Type>)
//===----------------------------------------------------------------------===//

void swift::ide::collectPossibleReturnTypesFromContext(
    DeclContext *DC, SmallVectorImpl<Type> &candidates) {
  if (auto FD = dyn_cast<AbstractFunctionDecl>(DC)) {
    auto Ty = FD->getInterfaceType();
    if (FD->getDeclContext()->isTypeContext())
      Ty = FD->getMethodInterfaceType();
    if (auto FT = Ty->getAs<AnyFunctionType>()) {
      candidates.push_back(DC->mapTypeIntoContext(FT->getResult()));
    }
  }

  if (auto ACE = dyn_cast<AbstractClosureExpr>(DC)) {
    // Try type checking the closure signature if it hasn't.
    if (!ACE->getType())
      swift::typeCheckASTNodeAtLoc(ACE->getParent(), ACE->getLoc());

    // Use the type checked type if it has.
    if (ACE->getType() && !ACE->getType()->hasError() &&
        !ACE->getResultType()->hasUnresolvedType()) {
      candidates.push_back(ACE->getResultType());
      return;
    }

    if (auto CE = dyn_cast<ClosureExpr>(ACE)) {
      if (CE->hasExplicitResultType()) {
        // If the closure has a explicit return type, use it.
        if (auto ty = CE->getExplicitResultType()) {
          candidates.push_back(ty);
          return;
        } else {
          const auto type = swift::performTypeResolution(
              CE->getExplicitResultTypeRepr(), DC->getASTContext(),
              /*isSILMode=*/false, /*isSILType=*/false,
              DC->getGenericEnvironmentOfContext(), /*GenericParams=*/nullptr,
              const_cast<DeclContext *>(DC), /*diagnostics=*/false);

          if (!type->hasError()) {
            candidates.push_back(type);
            return;
          }
        }
      } else {
        // Otherwise, check the context type of the closure.
        ExprContextInfo closureCtxInfo(CE->getParent(), CE);
        for (auto closureTy : closureCtxInfo.getPossibleTypes()) {
          if (auto funcTy = closureTy->getAs<AnyFunctionType>())
            candidates.push_back(funcTy->getResult());
        }
        if (!candidates.empty())
          return;
      }
    }

    // Even if the type checked type has unresolved types, it's better than
    // nothing.
    if (ACE->getType() && !ACE->getType()->hasError())
      candidates.push_back(ACE->getResultType());
  }
}

//===----------------------------------------------------------------------===//
// ExprContextInfo(DeclContext, SourceRange)
//===----------------------------------------------------------------------===//

namespace {
class ExprParentFinder : public ASTWalker {
  friend class ExprContextAnalyzer;
  Expr *ChildExpr;
  std::function<bool(ParentTy, ParentTy)> Predicate;

  bool arePositionsSame(Expr *E1, Expr *E2) {
    return E1->getSourceRange().Start == E2->getSourceRange().Start &&
           E1->getSourceRange().End == E2->getSourceRange().End;
  }

public:
  llvm::SmallVector<ParentTy, 5> Ancestors;
  ExprParentFinder(Expr *ChildExpr,
                   std::function<bool(ParentTy, ParentTy)> Predicate)
      : ChildExpr(ChildExpr), Predicate(Predicate) {}

  std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
    // Finish if we found the target. 'ChildExpr' might have been replaced
    // with typechecked expression. In that case, match the position.
    if (E == ChildExpr || arePositionsSame(E, ChildExpr))
      return {false, nullptr};

    if (E != ChildExpr && Predicate(E, Parent)) {
      Ancestors.push_back(E);
      return {true, E};
    }
    return {true, E};
  }

  Expr *walkToExprPost(Expr *E) override {
    if (Predicate(E, Parent))
      Ancestors.pop_back();
    return E;
  }

  std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
    if (Predicate(S, Parent))
      Ancestors.push_back(S);
    return {true, S};
  }

  Stmt *walkToStmtPost(Stmt *S) override {
    if (Predicate(S, Parent))
      Ancestors.pop_back();
    return S;
  }

  bool walkToDeclPre(Decl *D) override {
    if (Predicate(D, Parent))
      Ancestors.push_back(D);
    return true;
  }

  bool walkToDeclPost(Decl *D) override {
    if (Predicate(D, Parent))
      Ancestors.pop_back();
    return true;
  }

  std::pair<bool, Pattern *> walkToPatternPre(Pattern *P) override {
    if (Predicate(P, Parent))
      Ancestors.push_back(P);
    return {true, P};
  }

  Pattern *walkToPatternPost(Pattern *P) override {
    if (Predicate(P, Parent))
      Ancestors.pop_back();
    return P;
  }
};

/// Collect function (or subscript) members with the given \p name on \p baseTy.
static void collectPossibleCalleesByQualifiedLookup(
    DeclContext &DC, Type baseTy, DeclNameRef name,
    SmallVectorImpl<FunctionTypeAndDecl> &candidates) {
  auto baseInstanceTy = baseTy->getMetatypeInstanceType();
  if (!baseInstanceTy->mayHaveMembers())
    return;

  bool isOnMetaType = baseTy->is<AnyMetatypeType>();

  SmallVector<ValueDecl *, 2> decls;
  if (!DC.lookupQualified(baseInstanceTy,
                          name.withoutArgumentLabels(DC.getASTContext()),
                          NL_QualifiedDefault | NL_ProtocolMembers,
                          decls))
    return;

  llvm::DenseMap<std::pair<char, CanType>, size_t> known;
  auto *baseNominal = baseInstanceTy->getAnyNominal();
  for (auto *VD : decls) {
    if ((!isa<AbstractFunctionDecl>(VD) && !isa<SubscriptDecl>(VD)) ||
        VD->shouldHideFromEditor())
      continue;
    if (!isMemberDeclApplied(&DC, baseInstanceTy, VD))
      continue;
    Type declaredMemberType = VD->getInterfaceType();
    if (!declaredMemberType->is<AnyFunctionType>())
      continue;
    if (VD->getDeclContext()->isTypeContext()) {
      if (isa<FuncDecl>(VD)) {
        if (!isOnMetaType && VD->isStatic())
          continue;
        if (isOnMetaType == VD->isStatic())
          declaredMemberType =
              declaredMemberType->castTo<AnyFunctionType>()->getResult();
      } else if (isa<ConstructorDecl>(VD)) {
        if (!isOnMetaType)
          continue;
        declaredMemberType =
            declaredMemberType->castTo<AnyFunctionType>()->getResult();
      } else if (isa<SubscriptDecl>(VD)) {
        if (isOnMetaType != VD->isStatic())
          continue;
      }
    }

    auto subs = baseInstanceTy->getMemberSubstitutionMap(
        DC.getParentModule(), VD,
        VD->getInnermostDeclContext()->getGenericEnvironmentOfContext());
    auto fnType = declaredMemberType.subst(subs);
    if (!fnType || !fnType->is<AnyFunctionType>())
      continue;

    // If we are calling on a type alias type, replace the canonicalized type
    // in the function type with the type alias.
    if (isa<SugarType>(baseInstanceTy.getPointer())) {
      auto canBaseTy = baseInstanceTy->getCanonicalType();
      fnType = fnType.transform([&](Type t) -> Type {
        if (t->getCanonicalType()->isEqual(canBaseTy))
          return baseInstanceTy;
        return t;
      });
    }

    auto semanticContext = SemanticContextKind::CurrentNominal;
    if (baseNominal &&
        VD->getDeclContext()->getSelfNominalTypeDecl() != baseNominal)
      semanticContext = SemanticContextKind::Super;

    FunctionTypeAndDecl entry(fnType->castTo<AnyFunctionType>(), VD,
                              semanticContext);
    // Remember the index of the entry.
    auto knownResult = known.insert(
        {{VD->isStatic(), fnType->getCanonicalType()}, candidates.size()});
    if (knownResult.second) {
      candidates.push_back(entry);
      continue;
    }

    auto idx = knownResult.first->second;
    if (AvailableAttr::isUnavailable(candidates[idx].Decl) &&
        !AvailableAttr::isUnavailable(VD)) {
      // Replace the previously found "unavailable" with the "available" one.
      candidates[idx] = entry;
    }

    // Otherwise, skip redundant results.
  }
}

/// Collect function (or subscript) members with the given \p name on
/// \p baseExpr expression.
static void collectPossibleCalleesByQualifiedLookup(
    DeclContext &DC, Expr *baseExpr, DeclNameRef name,
    SmallVectorImpl<FunctionTypeAndDecl> &candidates) {
  ConcreteDeclRef ref = nullptr;

  // Re-typecheck TypeExpr so it's typechecked without the arguments which may
  // affects the inference of the generic arguments.
  if (TypeExpr *tyExpr = dyn_cast<TypeExpr>(baseExpr)) {
    if (!tyExpr->isImplicit())
      tyExpr->setType(nullptr);
  }

  Type baseTy = baseExpr->getType();
  if (!baseTy || baseTy->is<ErrorType>()) {
    auto baseTyOpt = getTypeOfCompletionContextExpr(
        DC.getASTContext(), &DC, CompletionTypeCheckKind::Normal, baseExpr,
        ref);
    if (!baseTyOpt)
      return;
    baseTy = *baseTyOpt;
  }
  baseTy = baseTy->getWithoutSpecifierType();

  // Use metatype for lookup 'super.init' if it's inside constructors.
  if (isa<SuperRefExpr>(baseExpr) && isa<ConstructorDecl>(DC) &&
      name == DeclNameRef::createConstructor())
    baseTy = MetatypeType::get(baseTy);

  collectPossibleCalleesByQualifiedLookup(DC, baseTy, name, candidates);

  // Add virtual 'subscript<Value>(keyPath: KeyPath<Root, Value>) -> Value'.
  if (name.getBaseName() == DeclBaseName::createSubscript() &&
      (baseTy->getAnyNominal() || baseTy->is<ArchetypeType>() ||
       baseTy->is<TupleType>())) {
    auto &Ctx = DC.getASTContext();

    auto *kpDecl = Ctx.getKeyPathDecl();
    Type kpTy = kpDecl->mapTypeIntoContext(kpDecl->getDeclaredInterfaceType());
    Type kpValueTy = kpTy->castTo<BoundGenericType>()->getGenericArgs()[1];
    kpTy = BoundGenericType::get(kpDecl, Type(), {baseTy, kpValueTy});

    Type fnTy = FunctionType::get(
        {AnyFunctionType::Param(kpTy, Ctx.Id_keyPath)}, kpValueTy);
    candidates.emplace_back(fnTy->castTo<AnyFunctionType>(), nullptr);
  }
}

/// For the given \p unresolvedMemberExpr, collect possible callee types and
/// declarations.
static bool collectPossibleCalleesForUnresolvedMember(
    DeclContext &DC, UnresolvedMemberExpr *unresolvedMemberExpr,
    SmallVectorImpl<FunctionTypeAndDecl> &candidates) {
  auto collectMembers = [&](Type expectedTy) {
    if (!expectedTy->mayHaveMembers())
      return;
    collectPossibleCalleesByQualifiedLookup(DC, MetatypeType::get(expectedTy),
                                            unresolvedMemberExpr->getName(),
                                            candidates);
  };

  // Get the context of the expression itself.
  ExprContextInfo contextInfo(&DC, unresolvedMemberExpr);
  for (auto expectedTy : contextInfo.getPossibleTypes()) {
    collectMembers(expectedTy);
    // If this is an optional type, let's also check its base type.
    if (auto baseTy = expectedTy->getOptionalObjectType()) {
      collectMembers(baseTy->lookThroughAllOptionalTypes());
    }
  }
  return !candidates.empty();
}

/// For the given \c callExpr, collect possible callee types and declarations.
static bool collectPossibleCalleesForApply(
    DeclContext &DC, ApplyExpr *callExpr,
    SmallVectorImpl<FunctionTypeAndDecl> &candidates) {
  auto *fnExpr = callExpr->getFn();

  if (auto *DRE = dyn_cast<DeclRefExpr>(fnExpr)) {
    if (auto *decl = DRE->getDecl()) {
      Type declTy = fnExpr->getType();
      if ((!declTy || declTy->hasError() || declTy->hasUnresolvedType()) &&
          decl->hasInterfaceType()) {
        declTy = decl->getInterfaceType();
        declTy = decl->getInnermostDeclContext()->mapTypeIntoContext(declTy);
      }
      if (declTy) {
        declTy = declTy->getWithoutSpecifierType();
        if (auto *funcTy = declTy->getAs<AnyFunctionType>())
          candidates.emplace_back(funcTy, decl);
      }
    }
  } else if (auto *OSRE = dyn_cast<OverloadSetRefExpr>(fnExpr)) {
    for (auto *decl : OSRE->getDecls()) {
      if (decl->hasInterfaceType()) {
        auto declTy = decl->getInterfaceType();
        declTy = decl->getInnermostDeclContext()->mapTypeIntoContext(declTy);
        if (auto *funcType = declTy->getAs<AnyFunctionType>())
          candidates.emplace_back(funcType, decl);
      }
    }
  } else if (auto *UDE = dyn_cast<UnresolvedDotExpr>(fnExpr)) {
    collectPossibleCalleesByQualifiedLookup(DC, UDE->getBase(), UDE->getName(),
                                            candidates);
  } else if (auto *DSCE = dyn_cast<DotSyntaxCallExpr>(fnExpr)) {
    if (auto *DRE = dyn_cast<DeclRefExpr>(DSCE->getFn())) {
      collectPossibleCalleesByQualifiedLookup(
          DC, DSCE->getArg(), DeclNameRef(DRE->getDecl()->getName()),
          candidates);
    }
  } else if (auto CRCE = dyn_cast<ConstructorRefCallExpr>(fnExpr)) {
    collectPossibleCalleesByQualifiedLookup(
        DC, CRCE->getArg(), DeclNameRef::createConstructor(), candidates);
  } else if (auto *UME = dyn_cast<UnresolvedMemberExpr>(fnExpr)) {
    collectPossibleCalleesForUnresolvedMember(DC, UME, candidates);
  }

  if (!candidates.empty())
    return true;

  ConcreteDeclRef refDecl = nullptr;
  Type fnType = fnExpr->getType();
  if (fnType) {
    refDecl = fnExpr->getReferencedDecl();
    if (!refDecl)
      if (auto apply = dyn_cast<ApplyExpr>(fnExpr))
        refDecl = apply->getFn()->getReferencedDecl();
  }
  if (!fnType) {
    auto fnTypeOpt = getTypeOfCompletionContextExpr(
        DC.getASTContext(), &DC, CompletionTypeCheckKind::Normal, fnExpr,
        refDecl);
    if (fnTypeOpt)
      fnType = *fnTypeOpt;
  }

  if (!fnType || fnType->hasUnresolvedType() || fnType->hasError())
    return false;
  fnType = fnType->getWithoutSpecifierType();

  if (auto *AFT = fnType->getAs<AnyFunctionType>()) {
    candidates.emplace_back(AFT, refDecl.getDecl());
  } else if (auto *AMT = fnType->getAs<AnyMetatypeType>()) {
    auto baseTy = AMT->getInstanceType();
    if (isa<TypeExpr>(fnExpr) && baseTy->mayHaveMembers()) {
      collectPossibleCalleesByQualifiedLookup(
          DC, fnExpr, DeclNameRef::createConstructor(), candidates);
    }
  } else {
    // Otherwise, look for `callAsFunction` (SE-0253).
    collectPossibleCalleesByQualifiedLookup(
        DC, fnExpr, DeclNameRef(DC.getASTContext().Id_callAsFunction),
        candidates);
  }

  return !candidates.empty();
}

/// For the given \c subscriptExpr, collect possible callee types and
/// declarations.
static bool collectPossibleCalleesForSubscript(
    DeclContext &DC, SubscriptExpr *subscriptExpr,
    SmallVectorImpl<FunctionTypeAndDecl> &candidates) {
  if (subscriptExpr->hasDecl()) {
    if (auto SD = dyn_cast<SubscriptDecl>(subscriptExpr->getDecl().getDecl())) {
      auto declType = SD->getInterfaceType();
      declType = declType.subst(subscriptExpr->getDecl().getSubstitutions());
      if (auto *funcType = declType->getAs<AnyFunctionType>())
        candidates.emplace_back(funcType, SD);
    }
  } else {
    collectPossibleCalleesByQualifiedLookup(DC, subscriptExpr->getBase(),
                                            DeclNameRef::createSubscript(),
                                            candidates);
  }
  return !candidates.empty();
}

/// Get index of \p CCExpr in \p Args. \p Args is usually a \c TupleExpr
/// or \c ParenExpr.
/// \returns \c true if success, \c false if \p CCExpr is not a part of \p Args.
static bool getPositionInArgs(DeclContext &DC, Expr *Args, Expr *CCExpr,
                              unsigned &Position, bool &HasName) {
  if (isa<ParenExpr>(Args)) {
    HasName = false;
    Position = 0;
    return true;
  }

  auto *tuple = dyn_cast<TupleExpr>(Args);
  if (!tuple)
    return false;

  auto &SM = DC.getASTContext().SourceMgr;
  for (unsigned i = 0, n = tuple->getNumElements(); i != n; ++i) {
    if (SM.isBeforeInBuffer(tuple->getElement(i)->getEndLoc(),
                            CCExpr->getStartLoc()))
      continue;
    HasName = tuple->getElementNameLoc(i).isValid();
    Position = i;
    return true;
  }
  return false;
}

/// Given an expression and its context, the analyzer tries to figure out the
/// expected type of the expression by analyzing its context.
class ExprContextAnalyzer {
  DeclContext *DC;
  Expr *ParsedExpr;
  SourceManager &SM;
  ASTContext &Context;

  // Results populated by Analyze()
  SmallVectorImpl<Type> &PossibleTypes;
  SmallVectorImpl<PossibleParamInfo> &PossibleParams;
  SmallVectorImpl<FunctionTypeAndDecl> &PossibleCallees;
  Expr *&AnalyzedExpr;
  bool &singleExpressionBody;

  void recordPossibleType(Type ty) {
    if (!ty || ty->is<ErrorType>())
      return;

    PossibleTypes.push_back(ty->getRValueType());
  }

  void recordPossibleParam(const AnyFunctionType::Param *arg, bool isRequired) {
    PossibleParams.emplace_back(arg, isRequired);
  }

  /// Collect context information at call argument position.
  bool analyzeApplyExpr(Expr *E) {
    // Collect parameter lists for possible func decls.
    SmallVector<FunctionTypeAndDecl, 2> Candidates;
    Expr *Arg = nullptr;
    if (auto *applyExpr = dyn_cast<ApplyExpr>(E)) {
      if (!collectPossibleCalleesForApply(*DC, applyExpr, Candidates))
        return false;
      Arg = applyExpr->getArg();
    } else if (auto *subscriptExpr = dyn_cast<SubscriptExpr>(E)) {
      if (!collectPossibleCalleesForSubscript(*DC, subscriptExpr, Candidates))
        return false;
      Arg = subscriptExpr->getIndex();
    } else {
      llvm_unreachable("unexpected expression kind");
    }
    assert(!Candidates.empty());
    PossibleCallees.assign(Candidates.begin(), Candidates.end());

    // Determine the position of code completion token in call argument.
    unsigned Position;
    bool HasName;
    if (!getPositionInArgs(*DC, Arg, ParsedExpr, Position, HasName))
      return false;

    // Collect possible types (or labels) at the position.
    // FIXME: Take variadic and optional parameters into account. We need to do
    //        something equivalent to 'constraints::matchCallArguments'
    {
      bool MayNeedName = !HasName && !E->isImplicit() &&
                         (isa<CallExpr>(E) | isa<SubscriptExpr>(E) ||
                          isa<UnresolvedMemberExpr>(E));
      SmallPtrSet<CanType, 4> seenTypes;
      llvm::SmallSet<std::pair<Identifier, CanType>, 4> seenArgs;
      for (auto &typeAndDecl : Candidates) {
        DeclContext *memberDC = nullptr;
        if (typeAndDecl.Decl)
          memberDC = typeAndDecl.Decl->getInnermostDeclContext();

        auto Params = typeAndDecl.Type->getParams();
        ParameterList *paramList = nullptr;
        if (auto VD = typeAndDecl.Decl) {
          paramList = getParameterList(VD);
          if (paramList && paramList->size() != Params.size())
            paramList = nullptr;
        }
        for (auto Pos = Position; Pos < Params.size(); ++Pos) {
          const auto &paramType = Params[Pos];
          Type ty = paramType.getPlainType();
          if (memberDC && ty->hasTypeParameter())
            ty = memberDC->mapTypeIntoContext(ty);

          bool canSkip =
              paramList && (paramList->get(Pos)->isDefaultArgument() ||
                            paramList->get(Pos)->isVariadic());

          if (paramType.hasLabel() && MayNeedName) {
            if (seenArgs.insert({paramType.getLabel(), ty->getCanonicalType()})
                    .second)
              recordPossibleParam(&paramType, !canSkip);
          } else {
            auto argTy = ty;
            if (paramType.isInOut())
              argTy = InOutType::get(argTy);
            else if (paramType.isAutoClosure() && argTy->is<AnyFunctionType>())
              argTy = argTy->castTo<AnyFunctionType>()->getResult();
            if (seenTypes.insert(argTy->getCanonicalType()).second)
              recordPossibleType(argTy);
          }
          if (!canSkip)
            break;
        }
        // If the argument position is out of expeceted number, indicate that
        // with optional nullptr param.
        if (Position >= Params.size()) {
          if (seenArgs.insert({Identifier(), CanType()}).second)
            recordPossibleParam(nullptr, /*isRequired=*/false);
        }
      }
    }
    return !PossibleTypes.empty() || !PossibleParams.empty();
  }

  void analyzeExpr(Expr *Parent) {
    AnalyzedExpr = Parent;
    switch (Parent->getKind()) {
    case ExprKind::Call:
    case ExprKind::Subscript:
    case ExprKind::Binary:
    case ExprKind::PrefixUnary: {
      analyzeApplyExpr(Parent);
      break;
    }
    case ExprKind::Array: {
      if (auto type = ParsedExpr->getType()) {
        if (!type->is<UnresolvedType>()) {
          recordPossibleType(type);
          break;
        }
      }

      // Check context types of the array literal expression.
      ExprContextInfo arrayCtxtInfo(DC, Parent);
      for (auto arrayT : arrayCtxtInfo.getPossibleTypes()) {
        if (auto boundGenericT = arrayT->getAs<BoundGenericType>()) {
          // let _: [Element] = [#HERE#]
          // In this case, 'Element' is the expected type.
          if (boundGenericT->getDecl() == Context.getArrayDecl())
            recordPossibleType(boundGenericT->getGenericArgs()[0]);

          // let _: [Key : Value] = [#HERE#]
          // In this case, 'Key' is the expected type.
          if (boundGenericT->getDecl() == Context.getDictionaryDecl())
            recordPossibleType(boundGenericT->getGenericArgs()[0]);
        }
      }
      break;
    }
    case ExprKind::Dictionary: {
      // Check context types of the dictionary literal expression.
      ExprContextInfo dictCtxtInfo(DC, Parent);

      for (auto dictT : dictCtxtInfo.getPossibleTypes()) {
        if (auto boundGenericT = dictT->getAs<BoundGenericType>()) {
          if (boundGenericT->getDecl() == Context.getDictionaryDecl()) {
            if (ParsedExpr->isImplicit() && isa<TupleExpr>(ParsedExpr)) {
              // let _: [Key : Value] = [#HERE#:]
              // let _: [Key : Value] = [#HERE#:val]
              // let _: [Key : Value] = [key:#HERE#]
              // In this case, this is called by 'ExprKind::Tuple' case. Return
              // '(Key,Value)' here, 'ExprKind::Tuple' branch can decide which
              // type in the tuple type is the exprected type.
              SmallVector<TupleTypeElt, 2> elts;
              for (auto genericArg : boundGenericT->getGenericArgs())
                elts.emplace_back(genericArg);
              recordPossibleType(TupleType::get(elts, DC->getASTContext()));
            } else {
              // let _: [Key : Value] = [key: val, #HERE#]
              // In this case, assume 'Key' is the expected type.
              if (boundGenericT->getDecl() == Context.getDictionaryDecl())
                recordPossibleType(boundGenericT->getGenericArgs()[0]);
            }
          }
        }
      }
      break;
    }
    case ExprKind::If: {
      auto *IE = cast<IfExpr>(Parent);
      if (IE->isFolded() &&
          SM.rangeContains(IE->getCondExpr()->getSourceRange(),
                           ParsedExpr->getSourceRange())) {
        recordPossibleType(Context.getBoolDecl()->getDeclaredInterfaceType());
        break;
      }
      ExprContextInfo ternaryCtxtInfo(DC, Parent);
      for (auto ternaryT : ternaryCtxtInfo.getPossibleTypes())
        recordPossibleType(ternaryT);
      break;
    }
    case ExprKind::Assign: {
      auto *AE = cast<AssignExpr>(Parent);

      // Make sure code completion is on the right hand side.
      if (SM.isBeforeInBuffer(AE->getEqualLoc(), ParsedExpr->getStartLoc())) {

        // The destination is of the expected type.
        auto *destExpr = AE->getDest();
        if (auto type = destExpr->getType()) {
          recordPossibleType(type);
        } else if (auto *DRE = dyn_cast<DeclRefExpr>(destExpr)) {
          if (auto *decl = DRE->getDecl()) {
            if (decl->hasInterfaceType())
              recordPossibleType(decl->getDeclContext()->mapTypeIntoContext(
                  decl->getInterfaceType()));
          }
        }
      }
      break;
    }
    case ExprKind::Tuple: {
      TupleType *tupleT = nullptr;
      if (Parent->getType() && Parent->getType()->is<TupleType>()) {
        tupleT = Parent->getType()->castTo<TupleType>();
      } else {
        ExprContextInfo tupleCtxtInfo(DC, Parent);
        for (auto possibleT : tupleCtxtInfo.getPossibleTypes()) {
          if (auto possibleTupleT = possibleT->getAs<TupleType>()) {
            tupleT = possibleTupleT;
            break;
          }
        }
        if (!tupleT)
          return;
      }

      unsigned Position = 0;
      bool HasName;
      if (getPositionInArgs(*DC, Parent, ParsedExpr, Position, HasName)) {
        // The expected type may have fewer number of elements.
        if (Position < tupleT->getNumElements())
          recordPossibleType(tupleT->getElementType(Position));
      }
      break;
    }
    case ExprKind::Closure: {
      auto *CE = cast<ClosureExpr>(Parent);
      assert(isSingleExpressionBodyForCodeCompletion(CE->getBody()));
      singleExpressionBody = true;
      SmallVector<Type, 2> candidates;
      collectPossibleReturnTypesFromContext(CE, candidates);
      for (auto ty : candidates)
        recordPossibleType(ty);
      break;
    }
    default:
      llvm_unreachable("Unhandled expression kind.");
    }
  }

  void analyzeStmt(Stmt *Parent) {
    switch (Parent->getKind()) {
    case StmtKind::Return: {
      SmallVector<Type, 2> candidates;
      collectPossibleReturnTypesFromContext(DC, candidates);
      for (auto ty : candidates)
        recordPossibleType(ty);
      break;
    }
    case StmtKind::ForEach:
      if (auto SEQ = cast<ForEachStmt>(Parent)->getSequence()) {
        if (containsTarget(SEQ)) {
          recordPossibleType(
              Context.getSequenceDecl()->getDeclaredInterfaceType());
        }
      }
      break;
    case StmtKind::RepeatWhile:
    case StmtKind::If:
    case StmtKind::While:
    case StmtKind::Guard:
      if (isBoolConditionOf(Parent)) {
        recordPossibleType(Context.getBoolDecl()->getDeclaredInterfaceType());
      }
      break;
    default:
      llvm_unreachable("Unhandled statement kind.");
    }
  }

  bool isBoolConditionOf(Stmt *parent) {
    if (auto *repeat = dyn_cast<RepeatWhileStmt>(parent)) {
      return repeat->getCond() && containsTarget(repeat->getCond());
    }
    if (auto *conditional = dyn_cast<LabeledConditionalStmt>(parent)) {
      for (StmtConditionElement cond : conditional->getCond()) {
        if (auto *E = cond.getBooleanOrNull()) {
          if (containsTarget(E)) {
            return true;
          }
        }
      }
    }
    return false;
  }

  bool containsTarget(Expr *E) {
    assert(E && "expected parent expression");
    return SM.rangeContains(E->getSourceRange(), ParsedExpr->getSourceRange());
  }

  void analyzeDecl(Decl *D) {
    switch (D->getKind()) {
    case DeclKind::PatternBinding: {
      auto PBD = cast<PatternBindingDecl>(D);
      for (unsigned I : range(PBD->getNumPatternEntries())) {
        if (auto Init = PBD->getInit(I)) {
          if (containsTarget(Init)) {
            if (PBD->getPattern(I)->hasType()) {
              recordPossibleType(PBD->getPattern(I)->getType());
              break;
            }
          }
        }
      }
      break;
    }
    default:
      if (auto *FD = dyn_cast<FuncDecl>(D)) {
        assert(isSingleExpressionBodyForCodeCompletion(FD->getBody()));
        singleExpressionBody = true;
        SmallVector<Type, 2> candidates;
        collectPossibleReturnTypesFromContext(DC, candidates);
        for (auto ty : candidates)
          recordPossibleType(ty);
        break;
      }
      llvm_unreachable("Unhandled decl kind.");
    }
  }

  void analyzePattern(Pattern *P) {
    switch (P->getKind()) {
    case PatternKind::Expr: {
      auto ExprPat = cast<ExprPattern>(P);
      if (auto D = ExprPat->getMatchVar()) {
        if (D->hasInterfaceType())
          recordPossibleType(
              D->getDeclContext()->mapTypeIntoContext(D->getInterfaceType()));
      }
      break;
    }
    default:
      llvm_unreachable("Unhandled pattern kind.");
    }
  }

  void analyzeInitializer(Initializer *initDC) {
    switch (initDC->getInitializerKind()) {
    case swift::InitializerKind::PatternBinding: {
      auto initDC = cast<PatternBindingInitializer>(DC);
      auto PBD = initDC->getBinding();
      if (!PBD)
        break;
      auto pat = PBD->getPattern(initDC->getBindingIndex());
      if (pat->hasType())
        recordPossibleType(pat->getType());
      break;
    }
    case InitializerKind::DefaultArgument: {
      auto initDC = cast<DefaultArgumentInitializer>(DC);
      auto AFD = dyn_cast<AbstractFunctionDecl>(initDC->getParent());
      if (!AFD)
        return;
      auto param = AFD->getParameters()->get(initDC->getIndex());
      recordPossibleType(AFD->mapTypeIntoContext(param->getInterfaceType()));
      break;
    }
    }
  }

  /// Whether the given \c BraceStmt, which must be the body of a function or
  /// closure, should be treated as a single-expression return for the purposes
  /// of code-completion.
  ///
  /// We cannot use hasSingleExpressionBody, because we explicitly do not use
  /// the single-expression-body when there is code-completion in the expression
  /// in order to avoid a base expression affecting the type. However, now that
  /// we've typechecked, we will take the context type into account.
  static bool isSingleExpressionBodyForCodeCompletion(BraceStmt *body) {
    if (body->getNumElements() == 2) {
      if (auto *D = body->getFirstElement().dyn_cast<Decl *>()) {
        // Step into nested active clause.
        while (auto *ICD = dyn_cast<IfConfigDecl>(D)) {
          auto ACE = ICD->getActiveClauseElements();
          if (ACE.size() == 1) {
            return body->getLastElement().is<Expr *>();
          } else if (ACE.size() == 2) {
            if (auto *ND = ACE.front().dyn_cast<Decl *>()) {
              D = ND;
              continue;
            }
          }
          break;
        }
      }
    }
    return body->getNumElements() == 1 && body->getLastElement().is<Expr *>();
  }

public:
  ExprContextAnalyzer(
      DeclContext *DC, Expr *ParsedExpr, SmallVectorImpl<Type> &PossibleTypes,
      SmallVectorImpl<PossibleParamInfo> &PossibleArgs,
      SmallVectorImpl<FunctionTypeAndDecl> &PossibleCallees,
      Expr *&AnalyzedExpr, bool &singleExpressionBody)
      : DC(DC), ParsedExpr(ParsedExpr), SM(DC->getASTContext().SourceMgr),
        Context(DC->getASTContext()), PossibleTypes(PossibleTypes),
        PossibleParams(PossibleArgs), PossibleCallees(PossibleCallees),
        AnalyzedExpr(AnalyzedExpr),
        singleExpressionBody(singleExpressionBody) {}

  void Analyze() {
    // We cannot analyze without target.
    if (!ParsedExpr)
      return;

    ExprParentFinder Finder(ParsedExpr, [&](ASTWalker::ParentTy Node,
                                            ASTWalker::ParentTy Parent) {
      if (auto E = Node.getAsExpr()) {
        switch (E->getKind()) {
        case ExprKind::Call: {
          // Iff the cursor is in argument position.
          auto call = cast<CallExpr>(E);
          auto fnRange = call->getFn()->getSourceRange();
          auto argsRange = call->getArg()->getSourceRange();
          auto exprRange = ParsedExpr->getSourceRange();
          return !SM.rangeContains(fnRange, exprRange) &&
                 SM.rangeContains(argsRange, exprRange);
        }
        case ExprKind::Subscript: {
          // Iff the cursor is in index position.
          auto argsRange = cast<SubscriptExpr>(E)->getIndex()->getSourceRange();
          return SM.rangeContains(argsRange, ParsedExpr->getSourceRange());
        }
        case ExprKind::Binary:
        case ExprKind::PrefixUnary:
        case ExprKind::Assign:
        case ExprKind::Dictionary:
        case ExprKind::If:
          return true;
        case ExprKind::Array:
          return (!Parent.getAsExpr() ||
                  !isa<VarargExpansionExpr>(Parent.getAsExpr()));
        case ExprKind::Tuple: {
          auto ParentE = Parent.getAsExpr();
          return !ParentE ||
                 (!isa<CallExpr>(ParentE) && !isa<SubscriptExpr>(ParentE) &&
                  !isa<BinaryExpr>(ParentE));
        }
        case ExprKind::Closure:
          return isSingleExpressionBodyForCodeCompletion(
              cast<ClosureExpr>(E)->getBody());
        default:
          return false;
        }
      } else if (auto S = Node.getAsStmt()) {
        switch (S->getKind()) {
        case StmtKind::Return:
        case StmtKind::ForEach:
        case StmtKind::RepeatWhile:
        case StmtKind::If:
        case StmtKind::While:
        case StmtKind::Guard:
          return true;
        default:
          return false;
        }
      } else if (auto D = Node.getAsDecl()) {
        switch (D->getKind()) {
        case DeclKind::PatternBinding:
          return true;
        default:
          if (auto *FD = dyn_cast<FuncDecl>(D))
            if (auto *body = FD->getBody())
              return isSingleExpressionBodyForCodeCompletion(body);
          return false;
        }
      } else if (auto P = Node.getAsPattern()) {
        switch (P->getKind()) {
        case PatternKind::Expr:
          return true;
        default:
          return false;
        }
      } else
        return false;
    });

    // For 'Initializer' context, we need to look into its parent.
    auto analyzeDC = isa<Initializer>(DC) ? DC->getParent() : DC;
    analyzeDC->walkContext(Finder);

    if (Finder.Ancestors.empty()) {
      // There's no parent context in DC. But still, the parent of the
      // initializer might constrain the initializer's type.
      if (auto initDC = dyn_cast<Initializer>(DC))
        analyzeInitializer(initDC);
      return;
    }

    auto &P = Finder.Ancestors.back();
    if (auto Parent = P.getAsExpr()) {
      analyzeExpr(Parent);
    } else if (auto Parent = P.getAsStmt()) {
      analyzeStmt(Parent);
    } else if (auto Parent = P.getAsDecl()) {
      analyzeDecl(Parent);
    } else if (auto Parent = P.getAsPattern()) {
      analyzePattern(Parent);
    }
  }
};

} // end anonymous namespace

ExprContextInfo::ExprContextInfo(DeclContext *DC, Expr *TargetExpr) {
  ExprContextAnalyzer Analyzer(DC, TargetExpr, PossibleTypes, PossibleParams,
                               PossibleCallees, AnalyzedExpr,
                               singleExpressionBody);
  Analyzer.Analyze();
}
