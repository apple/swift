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
    return Action::VisitChildrenIf(isInterstingRange(E), E);
  }

  PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override {
    return Action::VisitChildrenIf(isInterstingRange(P), P);
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    return Action::VisitChildrenIf(isInterstingRange(S), S);
  }

  PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
    return Action::SkipChildren();
  }
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

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  Expr *visitCallExpr(CallExpr *E) {
    auto *args = E->getArgs()->getOriginalArgs();

    Optional<unsigned> newTrailingClosureIdx;
    SmallVector<Argument, 4> newArgs;
    for (auto idx : indices(*args)) {
      // Update the trailing closure index if we have one.
      if (args->hasAnyTrailingClosures() &&
          idx == *args->getFirstTrailingClosureIndex()) {
        newTrailingClosureIdx = newArgs.size();
      }
      auto arg = args->get(idx);
      if (!isa<CodeCompletionExpr>(arg.getExpr()))
        newArgs.push_back(arg);
    }
    if (newArgs.size() == args->size())
      return E;

    // If we ended up removing the last trailing closure, drop the index.
    if (newTrailingClosureIdx && *newTrailingClosureIdx == newArgs.size())
      newTrailingClosureIdx = None;

    Removed = true;

    auto *argList = ArgumentList::create(
        Ctx, args->getLParenLoc(), newArgs, args->getRParenLoc(),
        newTrailingClosureIdx, E->isImplicit());
    return CallExpr::create(Ctx, E->getFn(), argList, E->isImplicit());
  }

  Expr *visitExpr(Expr *E) {
    return E;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (Removed)
      return Action::Stop();
    E = visit(E);
    return Action::SkipChildrenIf(Removed, E);
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    if (Removed)
      return Action::Stop();
    return Action::Continue(S);
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    return Action::SkipChildrenIf(Removed);
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
              DC->getGenericSignatureOfContext(), /*SILContext=*/nullptr,
              const_cast<DeclContext *>(DC), /*diagnostics=*/false);

          if (!type->hasError()) {
            candidates.push_back(DC->mapTypeIntoContext(type));
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

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    // Finish if we found the target. 'ChildExpr' might have been replaced
    // with typechecked expression. In that case, match the position.
    if (E == ChildExpr || arePositionsSame(E, ChildExpr))
      return Action::Stop();

    if (E != ChildExpr && Predicate(E, Parent)) {
      Ancestors.push_back(E);
      return Action::Continue(E);
    }
    return Action::Continue(E);
  }

  PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
    if (Predicate(E, Parent))
      Ancestors.pop_back();
    return Action::Continue(E);
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    if (Predicate(S, Parent))
      Ancestors.push_back(S);
    return Action::Continue(S);
  }

  PostWalkResult<Stmt *> walkToStmtPost(Stmt *S) override {
    if (Predicate(S, Parent))
      Ancestors.pop_back();
    return Action::Continue(S);
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    if (Predicate(D, Parent))
      Ancestors.push_back(D);
    return Action::Continue();
  }

  PostWalkAction walkToDeclPost(Decl *D) override {
    if (Predicate(D, Parent))
      Ancestors.pop_back();
    return Action::Continue();
  }

  PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override {
    if (Predicate(P, Parent))
      Ancestors.push_back(P);
    return Action::Continue(P);
  }

  PostWalkResult<Pattern *> walkToPatternPost(Pattern *P) override {
    if (Predicate(P, Parent))
      Ancestors.pop_back();
    return Action::Continue(P);
  }
};

/// Collect function (or subscript) members with the given \p name on \p baseTy.
static void collectPossibleCalleesByQualifiedLookup(
    DeclContext &DC, Type baseTy, DeclNameRef name, SourceLoc loc,
    SmallVectorImpl<FunctionTypeAndDecl> &candidates) {
  auto baseInstanceTy = baseTy->getMetatypeInstanceType();
  if (!baseInstanceTy->mayHaveMembers())
    return;

  if (name == DeclNameRef::createConstructor()) {
    // Existential types cannot be instantiated. e.g. 'MyProtocol()'.
    if (baseInstanceTy->isExistentialType())
      return;

    // 'AnyObject' is not initializable.
    if (baseInstanceTy->isAnyObject())
      return;
  }

  // Make sure we've resolved implicit members.
  namelookup::installSemanticMembersIfNeeded(baseInstanceTy, name);

  bool isOnMetaType = baseTy->is<AnyMetatypeType>();

  SmallVector<ValueDecl *, 2> decls;
  if (!DC.lookupQualified(baseInstanceTy,
                          name.withoutArgumentLabels(), loc,
                          NL_QualifiedDefault | NL_ProtocolMembers,
                          decls))
    return;

  llvm::DenseMap<std::pair<char, CanType>, size_t> known;
  auto *baseNominal = baseInstanceTy->getAnyNominal();
  for (auto *VD : decls) {
    if ((!isa<AbstractFunctionDecl>(VD) && !isa<SubscriptDecl>(VD) &&
         !isa<EnumElementDecl>(VD)) ||
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
      } else if (isa<EnumElementDecl>(VD)) {
        if (!isOnMetaType)
          continue;
        declaredMemberType =
            declaredMemberType->castTo<AnyFunctionType>()->getResult();
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

  if (auto ice = dyn_cast<ImplicitConversionExpr>(baseExpr))
    baseExpr = ice->getSyntacticSubExpr();

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

  // Use metatype for lookup 'super.init' and 'self.init' if it's inside
  // constructors.
  if (name == DeclNameRef::createConstructor() && isa<ConstructorDecl>(DC)) {
    bool isSuperCall = isa<SuperRefExpr>(baseExpr);
    bool isSelfCall = false;
    if (auto declRef = dyn_cast<DeclRefExpr>(baseExpr)) {
      if (declRef->getDecl()->getName() == DC.getASTContext().Id_self) {
        isSelfCall = true;
      }
    }
    if (isSuperCall || isSelfCall) {
      baseTy = MetatypeType::get(baseTy);
    }
  }

  collectPossibleCalleesByQualifiedLookup(DC, baseTy, name,
                                          baseExpr->getLoc(),
                                          candidates);

  // Add virtual 'subscript<Value>(keyPath: KeyPath<Root, Value>) -> Value'.
  if (name.getBaseName() == DeclBaseName::createSubscript() &&
      (baseTy->getAnyNominal() || baseTy->is<ArchetypeType>() ||
       baseTy->is<TupleType>())) {
    auto &Ctx = DC.getASTContext();

    auto *kpDecl = Ctx.getKeyPathDecl();
    Type kpTy = kpDecl->mapTypeIntoContext(kpDecl->getDeclaredInterfaceType());
    Type kpValueTy = kpTy->castTo<BoundGenericType>()->getGenericArgs()[1];
    kpTy = BoundGenericType::get(kpDecl, Type(), {baseTy, kpValueTy});

    // FIXME: Verify ExtInfo state is correct, not working by accident.
    FunctionType::ExtInfo info;
    Type fnTy = FunctionType::get(
        {AnyFunctionType::Param(kpTy, Ctx.Id_keyPath)}, kpValueTy, info);
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
                                            unresolvedMemberExpr->getLoc(),
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
          DC, DSCE->getBase(), DeclNameRef(DRE->getDecl()->getName()),
          candidates);
    }
  } else if (auto CRCE = dyn_cast<ConstructorRefCallExpr>(fnExpr)) {
    collectPossibleCalleesByQualifiedLookup(
        DC, CRCE->getBase(), DeclNameRef::createConstructor(), candidates);
  } else if (auto TE = dyn_cast<TypeExpr>(fnExpr)) {
    collectPossibleCalleesByQualifiedLookup(
        DC, TE, DeclNameRef::createConstructor(), candidates);
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

  if (!fnType || fnType->hasError())
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

/// Get index of \p CCExpr in \p Args.
/// \returns \c true if success, \c false if \p CCExpr is not a part of \p Args.
static bool getPositionInArgs(DeclContext &DC, ArgumentList *Args, Expr *CCExpr,
                              unsigned &Position, bool &HasName) {
  auto &SM = DC.getASTContext().SourceMgr;
  for (auto idx : indices(*Args)) {
    auto arg = Args->get(idx);
    if (SM.isBeforeInBuffer(arg.getExpr()->getEndLoc(), CCExpr->getStartLoc()))
      continue;
    HasName = arg.getLabelLoc().isValid();
    Position = idx;
    return true;
  }
  return false;
}

/// Get index of \p CCExpr in \p TE.
/// \returns \c true if success, \c false if \p CCExpr is not a part of \p Args.
static bool getPositionInTuple(DeclContext &DC, TupleExpr *TE, Expr *CCExpr,
                               unsigned &Position, bool &HasName) {
  auto &SM = DC.getASTContext().SourceMgr;
  for (auto idx : indices(TE->getElements())) {
    if (SM.isBeforeInBuffer(TE->getElement(idx)->getEndLoc(),
                            CCExpr->getStartLoc()))
      continue;
    HasName = TE->getElementNameLoc(idx).isValid();
    Position = idx;
    return true;
  }
  return false;
}

/// Get index of \p CCExpr in \p Params. Note that the position in \p Params may
/// be different than the position in \p Args if there are defaulted arguments
/// in \p Params which don't occur in \p Args.
///
/// \returns the position index number on success, \c None if \p CCExpr is not
/// a part of \p Args.
static Optional<unsigned>
getPositionInParams(DeclContext &DC, const ArgumentList *Args, Expr *CCExpr,
                    ArrayRef<AnyFunctionType::Param> Params, bool Lenient) {
  auto &SM = DC.getASTContext().SourceMgr;
  unsigned PosInParams = 0;
  unsigned PosInArgs = 0;
  bool LastParamWasVariadic = false;
  // We advance PosInArgs until we find argument that is after the code
  // completion token, which is when we stop.
  // For each argument, we try to find a matching parameter either by matching
  // argument labels, in which case PosInParams may be advanced by more than 1,
  // or by advancing PosInParams and PosInArgs both by 1.
  for (; PosInArgs < Args->size(); ++PosInArgs) {
    if (!SM.isBeforeInBuffer(Args->getExpr(PosInArgs)->getEndLoc(),
                             CCExpr->getStartLoc())) {
      // The arg is after the code completion position. Stop.
      if (LastParamWasVariadic && Args->getLabel(PosInArgs).empty()) {
        // If the last parameter was variadic and this argument stands by itself
        // without a label, assume that it belongs to the previous vararg
        // list.
        PosInParams--;
      }
      break;
    }

    auto ArgName = Args->getLabel(PosInArgs);
    // If the last parameter we matched was variadic, we claim all following
    // unlabeled arguments for that variadic parameter -> advance PosInArgs but
    // not PosInParams.
    if (LastParamWasVariadic && ArgName.empty()) {
      continue;
    } else {
      LastParamWasVariadic = false;
    }

    // Look for a matching parameter label.
    bool AdvancedPosInParams = false;
    for (unsigned i = PosInParams; i < Params.size(); ++i) {
      if (Params[i].getLabel() == ArgName) {
        // We have found a label match. Advance the position in the params
        // to point to the param after the one with this label.
        PosInParams = i + 1;
        AdvancedPosInParams = true;
        if (Params[i].isVariadic()) {
          LastParamWasVariadic = true;
        }
        break;
      }
    }

    if (!AdvancedPosInParams && Args->isTrailingClosureIndex(PosInArgs)) {
      // If the argument is a trailing closure, it can't match non-function
      // parameters. Advance to the next function parameter.
      for (unsigned i = PosInParams; i < Params.size(); ++i) {
        if (Params[i].getParameterType()->is<FunctionType>()) {
          PosInParams = i + 1;
          AdvancedPosInParams = true;
          break;
        }
      }
    }

    if (!AdvancedPosInParams) {
      if (Lenient) {
        // We haven't performed any special advance logic. Assume the argument
        // and parameter match, so advance PosInParams by 1.
        PosInParams += 1;
      } else {
        // If there is no matching argument label. These arguments can't be
        // applied to the params.
        return None;
      }
    }
  }
  if (PosInArgs < Args->size() && PosInParams < Params.size()) {
    // We didn't search until the end, so we found a position in Params. Success
    return PosInParams;
  } else {
    return None;
  }
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
  bool &implicitSingleExpressionReturn;

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
    ArgumentList *Args = nullptr;
    if (auto *applyExpr = dyn_cast<ApplyExpr>(E)) {
      if (!collectPossibleCalleesForApply(*DC, applyExpr, Candidates))
        return false;
      Args = applyExpr->getArgs();
    } else if (auto *subscriptExpr = dyn_cast<SubscriptExpr>(E)) {
      if (!collectPossibleCalleesForSubscript(*DC, subscriptExpr, Candidates))
        return false;
      Args = subscriptExpr->getArgs();
    } else {
      llvm_unreachable("unexpected expression kind");
    }
    assert(!Candidates.empty());
    PossibleCallees.assign(Candidates.begin(), Candidates.end());

    // Determine the position of code completion token in call argument.
    unsigned PositionInArgs;
    bool HasName;
    if (!getPositionInArgs(*DC, Args, ParsedExpr, PositionInArgs, HasName))
      return false;

    // Collect possible types (or labels) at the position.
    {
      bool MayBeArgForLabeledParam =
          HasName || E->isImplicit() ||
          (!isa<CallExpr>(E) && !isa<SubscriptExpr>(E) &&
           !isa<UnresolvedMemberExpr>(E));

      // If the completion position cannot be the actual argument, it must be
      // able to be an argument label.
      bool MayBeLabel = !MayBeArgForLabeledParam;

      // Alternatively, the code completion position may complete to an argument
      // label if we are currently completing variadic args.
      // E.g.
      // func foo(x: Int..., y: Int...) {}
      // foo(x: 1, #^COMPLETE^#)
      // #^COMPLETE^# may complete to either an additional variadic arg or to
      // the argument label `y`.
      //
      // Varargs are represented by a VarargExpansionExpr that contains an
      // ArrayExpr on the call side.
      if (auto Vararg =
              dyn_cast<VarargExpansionExpr>(Args->getExpr(PositionInArgs))) {
        if (auto Array = dyn_cast_or_null<ArrayExpr>(Vararg->getSubExpr())) {
          if (Array->getNumElements() > 0 &&
              !isa<CodeCompletionExpr>(Array->getElement(0))) {
            // We can only complete as argument label if we have at least one
            // proper vararg before the code completion token. We shouldn't be
            // suggesting labels for:
            // foo(x: #^COMPLETE^#)
            MayBeLabel = true;
          }
        }
      }
      SmallPtrSet<CanType, 4> seenTypes;
      llvm::SmallSet<std::pair<Identifier, CanType>, 4> seenArgs;
      llvm::SmallVector<Optional<unsigned>, 2> posInParams;
      {
        bool found = false;
        auto *originalArgs = Args->getOriginalArgs();
        for (auto &typeAndDecl : Candidates) {
          Optional<unsigned> pos = getPositionInParams(
              *DC, originalArgs, ParsedExpr, typeAndDecl.Type->getParams(),
              /*lenient=*/false);
          posInParams.push_back(pos);
          found |= pos.has_value();
        }
        if (!found) {
          // If applicable overload is not found, retry with considering
          // non-matching argument labels mis-typed.
          for (auto i : indices(Candidates)) {
            posInParams[i] = getPositionInParams(
                *DC, originalArgs, ParsedExpr, Candidates[i].Type->getParams(),
                /*lenient=*/true);
          }
        }
      }
      assert(posInParams.size() == Candidates.size());

      for (auto i : indices(Candidates)) {
        if (!posInParams[i].has_value()) {
          // If the argument doesn't have a matching position in the parameters,
          // indicate that with optional nullptr param.
          if (seenArgs.insert({Identifier(), CanType()}).second)
            recordPossibleParam(nullptr, /*isRequired=*/false);
          continue;
        }

        auto &typeAndDecl = Candidates[i];
        DeclContext *memberDC = nullptr;
        if (typeAndDecl.Decl)
          memberDC = typeAndDecl.Decl->getInnermostDeclContext();

        auto Params = typeAndDecl.Type->getParams();
        auto PositionInParams = *posInParams[i];

        ParameterList *paramList = nullptr;
        if (auto VD = typeAndDecl.Decl) {
          paramList = getParameterList(VD);
          if (paramList && paramList->size() != Params.size())
            paramList = nullptr;
        }
        for (auto Pos = PositionInParams; Pos < Params.size(); ++Pos) {
          const auto &paramType = Params[Pos];
          Type ty = paramType.getPlainType();
          if (memberDC && ty->hasTypeParameter())
            ty = memberDC->mapTypeIntoContext(ty);

          bool canSkip =
              paramList && (paramList->get(Pos)->isDefaultArgument() ||
                            paramList->get(Pos)->isVariadic());

          if (MayBeLabel && paramType.hasLabel()) {
            if (seenArgs.insert({paramType.getLabel(), ty->getCanonicalType()})
                    .second)
              recordPossibleParam(&paramType, !canSkip);
          }

          if (MayBeArgForLabeledParam || !paramType.hasLabel()) {
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
          if (boundGenericT->isArray())
            recordPossibleType(boundGenericT->getGenericArgs()[0]);

          // let _: [Key : Value] = [#HERE#]
          // In this case, 'Key' is the expected type.
          if (boundGenericT->isDictionary())
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
          if (boundGenericT->isDictionary()) {
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
              if (boundGenericT->isDictionary())
                recordPossibleType(boundGenericT->getGenericArgs()[0]);
            }
          }
        }
      }
      break;
    }
    case ExprKind::Ternary: {
      auto *IE = cast<TernaryExpr>(Parent);
      if (IE->isFolded() &&
          SM.rangeContains(IE->getCondExpr()->getSourceRange(),
                           ParsedExpr->getSourceRange())) {
        recordPossibleType(Context.getBoolType());
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
      if (getPositionInTuple(*DC, cast<TupleExpr>(Parent), ParsedExpr, Position,
                             HasName)) {
        // The expected type may have fewer number of elements.
        if (Position < tupleT->getNumElements())
          recordPossibleType(tupleT->getElementType(Position));
      }
      break;
    }
    case ExprKind::Closure: {
      auto *CE = cast<ClosureExpr>(Parent);
      assert(hasImplicitSingleExpressionReturn(CE->getBody()));
      implicitSingleExpressionReturn = true;
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
      if (auto SEQ = cast<ForEachStmt>(Parent)->getParsedSequence()) {
        if (containsTarget(SEQ)) {
          recordPossibleType(Context.getSequenceType());
        }
      }
      break;
    case StmtKind::RepeatWhile:
    case StmtKind::If:
    case StmtKind::While:
    case StmtKind::Guard:
      if (isBoolConditionOf(Parent)) {
        recordPossibleType(Context.getBoolType());
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
        assert(hasImplicitSingleExpressionReturn(FD->getBody()));
        implicitSingleExpressionReturn = true;
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
      if (!ExprPat->isResolved())
        break;

      auto D = ExprPat->getMatchVar();
      if (!D || !D->hasInterfaceType())
        break;

      auto *DC = D->getDeclContext();
      recordPossibleType(DC->mapTypeIntoContext(D->getInterfaceType()));
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
    case InitializerKind::PropertyWrapper: {
      auto initDC = cast<PropertyWrapperInitializer>(DC);
      auto AFD = dyn_cast<AbstractFunctionDecl>(initDC->getParent());
      if (!AFD)
        return;
      auto *var = initDC->getWrappedVar();
      recordPossibleType(AFD->mapTypeIntoContext(var->getInterfaceType()));
      break;
    }

    case InitializerKind::RuntimeAttribute: {
      // This never appears in AST.
      break;
    }
    }
  }

  /// Whether the given \c BraceStmt, which must be the body of a function or
  /// closure, contains a single expression that would be implicitly returned if
  /// the single-expression-body transform had been performed.
  ///
  /// We cannot use hasSingleExpressionBody, because we explicitly do not use
  /// the single-expression-body transform when there is a code-completion in
  /// the expression in order to avoid a base expression affecting the type, and
  /// need to distinguish whether the single expression body was explicitly
  /// returned (in which case the expression's type *must* match the expected
  /// return type) or not (in which case it *may* match, as the user could intend
  /// it only as the first statement of many that they haven't finished writing
  /// yet.
  static bool hasImplicitSingleExpressionReturn(BraceStmt *body) {
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
      Expr *&AnalyzedExpr, bool &implicitSingleExpressionReturn)
      : DC(DC), ParsedExpr(ParsedExpr), SM(DC->getASTContext().SourceMgr),
        Context(DC->getASTContext()), PossibleTypes(PossibleTypes),
        PossibleParams(PossibleArgs), PossibleCallees(PossibleCallees),
        AnalyzedExpr(AnalyzedExpr),
        implicitSingleExpressionReturn(implicitSingleExpressionReturn) {}

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
          auto argsRange = call->getArgs()->getSourceRange();
          auto exprRange = ParsedExpr->getSourceRange();
          return !SM.rangeContains(fnRange, exprRange) &&
                 SM.rangeContains(argsRange, exprRange);
        }
        case ExprKind::Subscript: {
          // Iff the cursor is in index position.
          auto argsRange = cast<SubscriptExpr>(E)->getArgs()->getSourceRange();
          return SM.rangeContains(argsRange, ParsedExpr->getSourceRange());
        }
        case ExprKind::Binary:
        case ExprKind::PrefixUnary:
        case ExprKind::Assign:
        case ExprKind::Dictionary:
        case ExprKind::Ternary:
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
          return hasImplicitSingleExpressionReturn(
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
              return hasImplicitSingleExpressionReturn(body);
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
                               implicitSingleExpressionReturn);
  Analyzer.Analyze();
}
