//===--- SourceEntityWalker.cpp - Routines for semantic source info -------===//
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

#include "swift/IDE/SourceEntityWalker.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeRepr.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Parse/Lexer.h"
#include "clang/Basic/Module.h"

using namespace swift;

namespace {

class SemaAnnotator : public ASTWalker {
  SourceEntityWalker &SEWalker;
  SmallVector<ConstructorRefCallExpr *, 2> CtorRefs;
  SmallVector<ExtensionDecl *, 2> ExtDecls;
  llvm::SmallDenseMap<OpaqueValueExpr *, Expr *, 4> OpaqueValueMap;
  llvm::SmallPtrSet<Expr *, 16> ExprsToSkip;
  bool Cancelled = false;
  Optional<AccessKind> OpAccess;

public:
  explicit SemaAnnotator(SourceEntityWalker &SEWalker)
    : SEWalker(SEWalker) { }

  bool isDone() const { return Cancelled; }

private:

  // FIXME: Remove this
  bool shouldWalkAccessorsTheOldWay() override { return true; }

  bool shouldWalkIntoGenericParams() override {
    return SEWalker.shouldWalkIntoGenericParams();
  }
  bool walkToDeclPre(Decl *D) override;
  std::pair<bool, Expr *> walkToExprPre(Expr *E) override;
  bool walkToTypeReprPre(TypeRepr *T) override;

  bool walkToDeclPost(Decl *D) override;
  Expr *walkToExprPost(Expr *E) override;
  bool walkToTypeReprPost(TypeRepr *T) override;

  std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override;
  Stmt *walkToStmtPost(Stmt *S) override;

  std::pair<bool, Pattern *> walkToPatternPre(Pattern *P) override;
  Pattern *walkToPatternPost(Pattern *P) override;

  bool handleImports(ImportDecl *Import);
  bool handleCustomAttributes(Decl *D);
  bool passModulePathElements(ImportPath::Module Path,
                              const clang::Module *ClangMod);

  bool passReference(ValueDecl *D, Type Ty, SourceLoc Loc, SourceRange Range,
                     ReferenceMetaData Data);
  bool passReference(ValueDecl *D, Type Ty, DeclNameLoc Loc, ReferenceMetaData Data);
  bool passReference(ModuleEntity Mod, ImportPath::Element IdLoc);

  bool passSubscriptReference(ValueDecl *D, SourceLoc Loc,
                              ReferenceMetaData Data, bool IsOpenBracket);
  bool passCallAsFunctionReference(ValueDecl *D, SourceLoc Loc,
                                   ReferenceMetaData Data);

  bool passCallArgNames(Expr *Fn, TupleExpr *TupleE);

  bool shouldIgnore(Decl *D);

  ValueDecl *extractDecl(Expr *Fn) const {
    Fn = Fn->getSemanticsProvidingExpr();
    if (auto *DRE = dyn_cast<DeclRefExpr>(Fn))
      return DRE->getDecl();
    if (auto ApplyE = dyn_cast<ApplyExpr>(Fn))
      return extractDecl(ApplyE->getFn());
    if (auto *ACE = dyn_cast<AutoClosureExpr>(Fn)) {
      if (auto *Unwrapped = ACE->getUnwrappedCurryThunkExpr())
        return extractDecl(Unwrapped);
    }
    return nullptr;
  }
};

} // end anonymous namespace

bool SemaAnnotator::walkToDeclPre(Decl *D) {
  if (isDone())
    return false;

  if (shouldIgnore(D)) {
    // If we return true here, the children will still be visited, but we won't
    // call walkToDeclPre on SEWalker. The corresponding walkToDeclPost call
    // on SEWalker will be prevented by the check for shouldIgnore in
    // walkToDeclPost in SemaAnnotator.
    return isa<PatternBindingDecl>(D);
  }

  if (!handleCustomAttributes(D)) {
    Cancelled = true;
    return false;
  }

  SourceLoc Loc = D->getLoc();
  unsigned NameLen = 0;
  bool IsExtension = false;

  if (auto *VD = dyn_cast<ValueDecl>(D)) {
    if (VD->hasName() && !VD->isImplicit()) {
      SourceManager &SM = VD->getASTContext().SourceMgr;
      NameLen = VD->getBaseName().userFacingName().size();
      if (Loc.isValid() && SM.extractText({Loc, 1}) == "`")
        NameLen += 2;
    }

    auto ReportParamList = [&](ParameterList *PL) {
      for (auto *PD : *PL) {
        auto Loc = PD->getArgumentNameLoc();
        if (Loc.isInvalid())
          continue;
        if (!SEWalker.visitDeclarationArgumentName(PD->getArgumentName(), Loc,
                                                   VD)) {
          Cancelled = true;
          return false;
        }
      }
      return true;
    };

    if (isa<AbstractFunctionDecl>(VD) || isa<SubscriptDecl>(VD)) {
      auto ParamList = getParameterList(VD);
      if (!ReportParamList(ParamList))
        return false;
    }
  } else if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
    SourceRange SR = SourceRange();
    if (auto *repr = ED->getExtendedTypeRepr())
      SR = repr->getSourceRange();
    Loc = SR.Start;
    if (Loc.isValid())
      NameLen = ED->getASTContext().SourceMgr.getByteDistance(SR.Start, SR.End);
    IsExtension = true;
  } else if (auto Import = dyn_cast<ImportDecl>(D)) {
    if (!handleImports(Import))
      return false;

  } else if (auto OpD = dyn_cast<OperatorDecl>(D)) {
    Loc = OpD->getLoc();
    if (Loc.isValid())
      NameLen = OpD->getName().getLength();

  } else if (auto PrecD = dyn_cast<PrecedenceGroupDecl>(D)) {
    Loc = PrecD->getLoc();
    if (Loc.isValid())
      NameLen = PrecD->getName().getLength();

  } else if (auto *ICD = dyn_cast<IfConfigDecl>(D)) {
    if (SEWalker.shouldWalkInactiveConfigRegion()) {
      for (auto Clause : ICD->getClauses()) {
        for (auto Member : Clause.Elements) {
          Member.walk(*this);
        }
      }
      return false;
    }
  }

  CharSourceRange Range = (Loc.isValid()) ? CharSourceRange(Loc, NameLen)
                                          : CharSourceRange();
  bool ShouldVisitChildren = SEWalker.walkToDeclPre(D, Range);
  // walkToDeclPost is only called when visiting children, so make sure to only
  // push the extension decl in that case (otherwise it won't be popped)
  if (IsExtension && ShouldVisitChildren) {
    ExtDecls.push_back(static_cast<ExtensionDecl*>(D));
  }
  return ShouldVisitChildren;
}

bool SemaAnnotator::walkToDeclPost(Decl *D) {
  if (isDone())
    return false;

  if (shouldIgnore(D))
    return true;

  if (isa<ExtensionDecl>(D)) {
    assert(ExtDecls.back() == D);
    ExtDecls.pop_back();
  }

  bool Continue = SEWalker.walkToDeclPost(D);
  if (!Continue)
    Cancelled = true;
  return Continue;
}

std::pair<bool, Stmt *> SemaAnnotator::walkToStmtPre(Stmt *S) {
  if (isDone()) {
    return { false, nullptr };
  }

  bool TraverseChildren = SEWalker.walkToStmtPre(S);
  if (TraverseChildren) {
    if (auto *DeferS = dyn_cast<DeferStmt>(S)) {
      // Since 'DeferStmt::getTempDecl()' is marked as implicit, we manually
      // walk into the body.
      if (auto *FD = DeferS->getTempDecl()) {
        auto *RetS = FD->getBody()->walk(*this);
        if (!RetS)
          return { false, nullptr };
        assert(RetS == FD->getBody());
      }
      bool Continue = SEWalker.walkToStmtPost(DeferS);
      if (!Continue)
        Cancelled = true;
      // Already walked children.
      return { false, Continue ? DeferS : nullptr };
    }

    auto doSkipChildren = [&]() -> std::pair<bool, Stmt *> {
      if (!walkToStmtPost(S))
        return {false, nullptr};
      return {false, S};
    };

    auto doStopTraversal = [&]() -> std::pair<bool, Stmt *> {
      Cancelled = true;
      return {false, nullptr};
    };

    // Make sure to walk a ForEachStmt in source order. Note this is a narrow
    // fix for release/5.5. On main, this is fixed in the ASTWalker itself.
    // rdar://78781061
    if (auto *FE = dyn_cast<ForEachStmt>(S)) {
      if (auto *P = FE->getPattern()) {
        auto *NewP = P->walk(*this);
        if (!NewP)
          return doStopTraversal();
        assert(NewP == P);
      }
      if (auto *SE = FE->getSequence()) {
        auto *NewSE = SE->walk(*this);
        if (!NewSE)
          return doStopTraversal();
        assert(NewSE == SE);
      }
      if (auto *Where = FE->getWhere()) {
        auto *NewWhere = Where->walk(*this);
        if (!NewWhere)
          return doStopTraversal();
        assert(NewWhere == Where);
      }
      if (auto *IteratorNext = FE->getConvertElementExpr()) {
        auto *NewIteratorNext = IteratorNext->walk(*this);
        if (!NewIteratorNext)
          return doStopTraversal();
        assert(NewIteratorNext == IteratorNext);
      }
      if (auto *IteratorVar = FE->getIteratorVar()) {
        if (IteratorVar->walk(*this))
          return doStopTraversal();
      }
      if (auto *IteratorVarRef = FE->getIteratorVarRef()) {
        auto *NewIteratorVarRef = IteratorVarRef->walk(*this);
        if (!NewIteratorVarRef)
          return doStopTraversal();
        assert(NewIteratorVarRef == IteratorVarRef);
      }
      if (auto *Body = FE->getBody()) {
        auto *NewBody = Body->walk(*this);
        if (!NewBody)
          return doStopTraversal();
        assert(NewBody == Body);
      }
      // Already visited.
      return doSkipChildren();
    }
  }
  return { TraverseChildren, S };
}

Stmt *SemaAnnotator::walkToStmtPost(Stmt *S) {
  if (isDone()) {
    return nullptr;
  }

  bool Continue = SEWalker.walkToStmtPost(S);
  if (!Continue)
    Cancelled = true;
  return Continue ? S : nullptr;
}

static SemaReferenceKind getReferenceKind(Expr *Parent, Expr *E) {
  if (auto SA = dyn_cast_or_null<SelfApplyExpr>(Parent)) {
    if (SA->getFn() == E)
      return SemaReferenceKind::DeclMemberRef;
  }
  return SemaReferenceKind::DeclRef;
}

std::pair<bool, Expr *> SemaAnnotator::walkToExprPre(Expr *E) {
  assert(E);

  if (isDone()) {
    return { false, nullptr };
  }

  if (ExprsToSkip.count(E) != 0) {
    // We are skipping the expression. Call neither walkToExprPr nor
    // walkToExprPost on it
    return { false, E };
  }

  if (!SEWalker.walkToExprPre(E)) {
    return { false, E };
  }

  auto doSkipChildren = [&]() -> std::pair<bool, Expr *> {
    // If we decide to skip the children after having issued the call to
    // walkToExprPre, we need to simulate a corresponding call to walkToExprPost
    // which will not be issued by the ASTWalker if we return false in the first
    // component.
    if (!walkToExprPost(E)) {
      // walkToExprPost has cancelled the traversal. Stop.
      return { false, nullptr };
    }
    return { false, E };
  };

  auto doStopTraversal = [&]() -> std::pair<bool, Expr *> {
    Cancelled = true;
    return { false, nullptr };
  };

  if (auto *CtorRefE = dyn_cast<ConstructorRefCallExpr>(E))
    CtorRefs.push_back(CtorRefE);

  if (auto *ACE = dyn_cast<AutoClosureExpr>(E)) {
    if (auto *SubExpr = ACE->getUnwrappedCurryThunkExpr()) {
      if (auto *DRE = dyn_cast<DeclRefExpr>(SubExpr)) {
        if (!passReference(DRE->getDecl(), DRE->getType(),
                           DRE->getNameLoc(),
                           ReferenceMetaData(getReferenceKind(Parent.getAsExpr(), DRE),
                                             OpAccess)))
          return doStopTraversal();

        return doSkipChildren();
      }
    }

    return { true, E };
  }

  if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    auto *FD = dyn_cast<FuncDecl>(DRE->getDecl());
    // Handle implicit callAsFunction reference. An explicit reference will be
    // handled by the usual DeclRefExpr case below.
    if (DRE->isImplicit() && FD && FD->isCallAsFunctionMethod()) {
      ReferenceMetaData data(SemaReferenceKind::DeclMemberRef, OpAccess);
      if (!passCallAsFunctionReference(FD, DRE->getLoc(), data))
        return {false, nullptr};
      return {true, E};
    }
  }

  if (!isa<InOutExpr>(E) &&
      !isa<LoadExpr>(E) &&
      !isa<OpenExistentialExpr>(E) &&
      !isa<MakeTemporarilyEscapableExpr>(E) &&
      !isa<CollectionUpcastConversionExpr>(E) &&
      !isa<OpaqueValueExpr>(E) &&
      !isa<SubscriptExpr>(E) &&
      !isa<KeyPathExpr>(E) &&
      E->isImplicit())
    return { true, E };

  if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (auto *module = dyn_cast<ModuleDecl>(DRE->getDecl())) {
      if (!passReference(ModuleEntity(module),
                         {module->getName(), E->getLoc()}))
        return doStopTraversal();
    } else if (!passReference(DRE->getDecl(), DRE->getType(),
                              DRE->getNameLoc(),
                      ReferenceMetaData(getReferenceKind(Parent.getAsExpr(), DRE),
                                        OpAccess))) {
      return doStopTraversal();
    }
  } else if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
    {
      // This could be made more accurate if the member is nonmutating,
      // or whatever.
      Optional<AccessKind> NewOpAccess;
      if (OpAccess) {
        if (*OpAccess == AccessKind::Write)
          NewOpAccess = AccessKind::ReadWrite;
        else
          NewOpAccess = OpAccess;
      }

      llvm::SaveAndRestore<Optional<AccessKind>>
        C(this->OpAccess, NewOpAccess);

      // Visit in source order.
      if (!MRE->getBase()->walk(*this))
        return doStopTraversal();
    }

    if (!passReference(MRE->getMember().getDecl(), MRE->getType(),
                       MRE->getNameLoc(),
                       ReferenceMetaData(SemaReferenceKind::DeclMemberRef,
                                         OpAccess)))
      return doStopTraversal();

    // We already visited the children.
    return doSkipChildren();

  } else if (auto OtherCtorE = dyn_cast<OtherConstructorDeclRefExpr>(E)) {
    if (!passReference(OtherCtorE->getDecl(), OtherCtorE->getType(),
                       OtherCtorE->getConstructorLoc(),
                       ReferenceMetaData(SemaReferenceKind::DeclConstructorRef,
                                         OpAccess)))
      return doStopTraversal();

  } else if (auto *SE = dyn_cast<SubscriptExpr>(E)) {
    // Visit in source order.
    if (!SE->getBase()->walk(*this))
      return doStopTraversal();

    ValueDecl *SubscrD = nullptr;
    if (SE->hasDecl())
      SubscrD = SE->getDecl().getDecl();

    ReferenceMetaData data(SemaReferenceKind::SubscriptRef, OpAccess,
                           SE->isImplicit());

    if (SubscrD) {
      if (!passSubscriptReference(SubscrD, E->getLoc(), data, true))
        return doStopTraversal();
    }

    if (!SE->getIndex()->walk(*this))
      return doStopTraversal();

    if (SubscrD) {
      if (!passSubscriptReference(SubscrD, E->getEndLoc(), data, false))
        return doStopTraversal();
    }

    // We already visited the children.
    return doSkipChildren();

  } else if (auto *KPE = dyn_cast<KeyPathExpr>(E)) {
    for (auto &component : KPE->getComponents()) {
      switch (component.getKind()) {
      case KeyPathExpr::Component::Kind::Property:
      case KeyPathExpr::Component::Kind::Subscript: {
        auto *decl = component.getDeclRef().getDecl();
        auto loc = component.getLoc();
        SourceRange range(loc, loc);
        passReference(decl, component.getComponentType(), loc, range,
                      ReferenceMetaData(
                        (isa<SubscriptDecl>(decl)
                          ? SemaReferenceKind::SubscriptRef
                          : SemaReferenceKind::DeclMemberRef),
                        OpAccess));
        break;
      }

      case KeyPathExpr::Component::Kind::TupleElement:
      case KeyPathExpr::Component::Kind::Invalid:
      case KeyPathExpr::Component::Kind::UnresolvedProperty:
      case KeyPathExpr::Component::Kind::UnresolvedSubscript:
      case KeyPathExpr::Component::Kind::OptionalChain:
      case KeyPathExpr::Component::Kind::OptionalWrap:
      case KeyPathExpr::Component::Kind::OptionalForce:
      case KeyPathExpr::Component::Kind::Identity:
      case KeyPathExpr::Component::Kind::DictionaryKey:
        break;
      }
    }
  } else if (auto *BinE = dyn_cast<BinaryExpr>(E)) {
    // Visit in source order.
    if (!BinE->getArg()->getElement(0)->walk(*this))
      return doStopTraversal();
    if (!BinE->getFn()->walk(*this))
      return doStopTraversal();
    if (!BinE->getArg()->getElement(1)->walk(*this))
      return doStopTraversal();

    // We already visited the children.
    return doSkipChildren();

  } else if (auto TupleE = dyn_cast<TupleExpr>(E)) {
    if (auto CallE = dyn_cast_or_null<CallExpr>(Parent.getAsExpr())) {
      if (!passCallArgNames(CallE->getFn(), TupleE))
        return doStopTraversal();
    }
  } else if (auto IOE = dyn_cast<InOutExpr>(E)) {
    llvm::SaveAndRestore<Optional<AccessKind>>
      C(this->OpAccess, AccessKind::ReadWrite);

    if (!IOE->getSubExpr()->walk(*this))
      return doStopTraversal();

    // We already visited the children.
    return doSkipChildren();
  } else if (auto LE = dyn_cast<LoadExpr>(E)) {
    llvm::SaveAndRestore<Optional<AccessKind>>
      C(this->OpAccess, AccessKind::Read);

    if (!LE->getSubExpr()->walk(*this))
      return doStopTraversal();

    // We already visited the children.
    return doSkipChildren();
  } else if (auto AE = dyn_cast<AssignExpr>(E)) {
    {
      llvm::SaveAndRestore<Optional<AccessKind>>
        C(this->OpAccess, AccessKind::Write);

      if (AE->getDest() && !AE->getDest()->walk(*this))
        return doStopTraversal();
    }

    if (AE->getSrc() && !AE->getSrc()->walk(*this))
      return doStopTraversal();

    // We already visited the children.
    return doSkipChildren();
  } else if (auto OEE = dyn_cast<OpenExistentialExpr>(E)) {
    // Record opaque value.
    OpaqueValueMap[OEE->getOpaqueValue()] = OEE->getExistentialValue();
    SWIFT_DEFER {
      OpaqueValueMap.erase(OEE->getOpaqueValue());
    };

    if (!OEE->getSubExpr()->walk(*this))
      return doStopTraversal();

    return doSkipChildren();
  } else if (auto MTEE = dyn_cast<MakeTemporarilyEscapableExpr>(E)) {
    // Manually walk to original arguments in order. We don't handle
    // OpaqueValueExpr here.

    // Original non-escaping closure.
    if (!MTEE->getNonescapingClosureValue()->walk(*this))
      return doStopTraversal();

    // Body, which is called by synthesized CallExpr.
    auto *callExpr = cast<CallExpr>(MTEE->getSubExpr());
    if (!callExpr->getFn()->walk(*this))
      return doStopTraversal();

    return doSkipChildren();
  } else if (auto CUCE = dyn_cast<CollectionUpcastConversionExpr>(E)) {
    // Ignore conversion expressions. We don't handle OpaqueValueExpr here
    // because it's only in conversion expressions. Instead, just walk into
    // sub expression.
    if (!CUCE->getSubExpr()->walk(*this))
      return doStopTraversal();

    return doSkipChildren();
  } else if (auto OVE = dyn_cast<OpaqueValueExpr>(E)) {
    // Walk into mapped value.
    auto value = OpaqueValueMap.find(OVE);
    if (value != OpaqueValueMap.end()) {
      if (!value->second->walk(*this))
        return doStopTraversal();

      return doSkipChildren();
    }
  } else if (auto DMRE = dyn_cast<DynamicMemberRefExpr>(E)) {
    // Visit in source order.
    if (!DMRE->getBase()->walk(*this))
        return doStopTraversal();
    if (!passReference(DMRE->getMember().getDecl(), DMRE->getType(),
                       DMRE->getNameLoc(),
                       ReferenceMetaData(SemaReferenceKind::DynamicMemberRef,
                                         OpAccess)))
        return doStopTraversal();
    // We already visited the children.
    return doSkipChildren();
  }

  return { true, E };
}

Expr *SemaAnnotator::walkToExprPost(Expr *E) {
  if (isDone()) {
    return nullptr;
  }

  if (isa<ConstructorRefCallExpr>(E)) {
    assert(CtorRefs.back() == E);
    CtorRefs.pop_back();
  }

  bool Continue = SEWalker.walkToExprPost(E);
  if (!Continue)
    Cancelled = true;
  return Continue ? E : nullptr;
}

bool SemaAnnotator::walkToTypeReprPre(TypeRepr *T) {
  if (isDone())
    return false;

  if (auto IdT = dyn_cast<ComponentIdentTypeRepr>(T)) {
    if (ValueDecl *VD = IdT->getBoundDecl()) {
      if (auto *ModD = dyn_cast<ModuleDecl>(VD)) {
        auto ident = IdT->getNameRef().getBaseIdentifier();
        return passReference(ModD, { ident, IdT->getLoc() });
      }

      return passReference(VD, Type(), IdT->getNameLoc(),
                           ReferenceMetaData(SemaReferenceKind::TypeRef, None));
    }
  }
  return true;
}

bool SemaAnnotator::walkToTypeReprPost(TypeRepr *T) {
  return !isDone();
}

std::pair<bool, Pattern *> SemaAnnotator::walkToPatternPre(Pattern *P) {
  if (isDone()) {
    return { false, nullptr };
  }

  if (!SEWalker.walkToPatternPre(P))
    return { false, P };

  if (P->isImplicit())
    return { true, P };

  if (auto *EP = dyn_cast<EnumElementPattern>(P)) {
    auto *Element = EP->getElementDecl();
    if (!Element)
      return { true, P };
    Type T = EP->hasType() ? EP->getType() : Type();
    return { passReference(Element, T, DeclNameLoc(EP->getLoc()),
                ReferenceMetaData(SemaReferenceKind::EnumElementRef, None)), P };
  }

  auto *TP = dyn_cast<TypedPattern>(P);
  if (!TP || !TP->isPropagatedType())
    return { true, P };

  // If the typed pattern was propagated from somewhere, just walk the
  // subpattern.  The type will be walked as a part of another TypedPattern.
  TP->getSubPattern()->walk(*this);
  return { false, P };
}

Pattern *SemaAnnotator::walkToPatternPost(Pattern *P) {
  if (isDone())
     return nullptr;

  bool Continue = SEWalker.walkToPatternPost(P);
  if (!Continue)
    Cancelled = true;
  return Continue ? P : nullptr;
}

bool SemaAnnotator::handleCustomAttributes(Decl *D) {
  // CustomAttrs of non-param VarDecls are handled when this method is called
  // on their containing PatternBindingDecls (see below).
  if (isa<VarDecl>(D) && !isa<ParamDecl>(D))
    return true;

  if (auto *PBD = dyn_cast<PatternBindingDecl>(D)) {
    if (auto *SingleVar = PBD->getSingleVar()) {
      D = SingleVar;
    } else {
      return true;
    }
  }
  for (auto *customAttr : D->getAttrs().getAttributes<CustomAttr, true>()) {
    if (auto *Repr = customAttr->getTypeRepr()) {
      if (!Repr->walk(*this))
        return false;
    }
    if (auto *SemaInit = customAttr->getSemanticInit()) {
      if (!SemaInit->isImplicit()) {
        assert(customAttr->getArg());
        if (!SemaInit->walk(*this))
          return false;
        // Don't walk this again via the associated PatternBindingDecl's
        // initializer
        ExprsToSkip.insert(SemaInit);
      }
    } else if (auto *Arg = customAttr->getArg()) {
      if (!Arg->walk(*this))
        return false;
    }
  }
  return true;
}

bool SemaAnnotator::handleImports(ImportDecl *Import) {
  auto Mod = Import->getModule();
  if (!Mod)
    return true;

  auto ClangMod = Mod->findUnderlyingClangModule();
  if (ClangMod && ClangMod->isSubModule()) {
    if (!passModulePathElements(Import->getModulePath(), ClangMod))
      return false;
  } else {
    if (!passReference(Mod, Import->getModulePath().front()))
      return false;
  }

  auto Decls = Import->getDecls();
  if (Decls.size() == 1) {
    // FIXME: ImportDecl should store a DeclNameLoc.
    // FIXME: Handle overloaded funcs too by passing a reference for each?
    if (!passReference(Decls.front(), Type(), DeclNameLoc(Import->getEndLoc()),
        ReferenceMetaData(SemaReferenceKind::DeclRef, None)))
      return false;
  }

  return true;
}

bool SemaAnnotator::passModulePathElements(
    ImportPath::Module Path,
    const clang::Module *ClangMod) {

  assert(ClangMod && "can't passModulePathElements of null ClangMod");

  // Visit parent, if any, first.
  if (ClangMod->Parent && Path.hasSubmodule())
    if (!passModulePathElements(Path.getParentPath(), ClangMod->Parent))
      return false;

  return passReference(ClangMod, Path.back());
}

bool SemaAnnotator::passSubscriptReference(ValueDecl *D, SourceLoc Loc,
                                           ReferenceMetaData Data,
                                           bool IsOpenBracket) {
  CharSourceRange Range = Loc.isValid()
                        ? CharSourceRange(Loc, 1)
                        : CharSourceRange();

  bool Continue =
      SEWalker.visitSubscriptReference(D, Range, Data, IsOpenBracket);
  if (!Continue)
    Cancelled = true;
  return Continue;
}

bool SemaAnnotator::passCallAsFunctionReference(ValueDecl *D, SourceLoc Loc,
                                                ReferenceMetaData Data) {
  CharSourceRange Range =
      Loc.isValid() ? CharSourceRange(Loc, 1) : CharSourceRange();

  bool Continue = SEWalker.visitCallAsFunctionReference(D, Range, Data);
  if (!Continue)
    Cancelled = true;
  return Continue;
}

bool SemaAnnotator::
passReference(ValueDecl *D, Type Ty, DeclNameLoc Loc, ReferenceMetaData Data) {
  SourceManager &SM = D->getASTContext().SourceMgr;
  SourceLoc BaseStart = Loc.getBaseNameLoc(), BaseEnd = BaseStart;
  if (BaseStart.isValid() && SM.extractText({BaseStart, 1}) == "`")
    BaseEnd = Lexer::getLocForEndOfToken(SM, BaseStart.getAdvancedLoc(1));
  return passReference(D, Ty, BaseStart, {BaseStart, BaseEnd}, Data);
}

bool SemaAnnotator::
passReference(ValueDecl *D, Type Ty, SourceLoc BaseNameLoc, SourceRange Range,
              ReferenceMetaData Data) {
  TypeDecl *CtorTyRef = nullptr;
  ExtensionDecl *ExtDecl = nullptr;

  if (auto *TD = dyn_cast<TypeDecl>(D)) {
    if (!CtorRefs.empty() && BaseNameLoc.isValid()) {
      Expr *Fn = CtorRefs.back()->getFn();
      if (Fn->getLoc() == BaseNameLoc) {
        D = extractDecl(Fn);
        CtorTyRef = TD;
      }
    }

    if (!ExtDecls.empty() && BaseNameLoc.isValid()) {
      SourceLoc ExtTyLoc = SourceLoc();
      if (auto *repr = ExtDecls.back()->getExtendedTypeRepr())
        ExtTyLoc = repr->getLoc();
      if (ExtTyLoc.isValid() && ExtTyLoc == BaseNameLoc) {
        ExtDecl = ExtDecls.back();
      }
    }
  }

  if (D == nullptr) {
    // FIXME: When does this happen?
    assert(false && "unhandled reference");
    return true;
  }

  CharSourceRange CharRange =
    Lexer::getCharSourceRangeFromSourceRange(D->getASTContext().SourceMgr,
                                             Range);
  bool Continue = SEWalker.visitDeclReference(D, CharRange, CtorTyRef, ExtDecl,
                                              Ty, Data);
  if (!Continue)
    Cancelled = true;
  return Continue;
}

bool SemaAnnotator::passReference(ModuleEntity Mod,
                                  ImportPath::Element IdLoc) {
  if (IdLoc.Loc.isInvalid())
    return true;
  unsigned NameLen = IdLoc.Item.getLength();
  CharSourceRange Range{ IdLoc.Loc, NameLen };
  bool Continue = SEWalker.visitModuleReference(Mod, Range);
  if (!Continue)
    Cancelled = true;
  return Continue;
}

bool SemaAnnotator::passCallArgNames(Expr *Fn, TupleExpr *TupleE) {
  ValueDecl *D = extractDecl(Fn);
  if (!D)
    return true; // continue.

  ArrayRef<Identifier> ArgNames = TupleE->getElementNames();
  ArrayRef<SourceLoc> ArgLocs = TupleE->getElementNameLocs();
  for (auto i : indices(ArgNames)) {
    Identifier Name = ArgNames[i];
    if (Name.empty())
      continue;

    SourceLoc Loc = ArgLocs[i];
    if (Loc.isInvalid())
      continue;

    CharSourceRange Range{ Loc, Name.getLength() };
    bool Continue = SEWalker.visitCallArgName(Name, Range, D);
    if (!Continue) {
      Cancelled = true;
      return false;
    }
  }

  return true;
}

bool SemaAnnotator::shouldIgnore(Decl *D) {
  // TODO: There should really be a separate field controlling whether
  //       constructors are visited or not
  return D->isImplicit() && !isa<ConstructorDecl>(D);
}

bool SourceEntityWalker::walk(SourceFile &SrcFile) {
  SemaAnnotator Annotator(*this);
  return performWalk(Annotator, [&]() { return SrcFile.walk(Annotator); });
}

bool SourceEntityWalker::walk(ModuleDecl &Mod) {
  SemaAnnotator Annotator(*this);
  return performWalk(Annotator, [&]() { return Mod.walk(Annotator); });
}

bool SourceEntityWalker::walk(Stmt *S) {
  SemaAnnotator Annotator(*this);
  return performWalk(Annotator, [&]() { return S->walk(Annotator); });
}

bool SourceEntityWalker::walk(Expr *E) {
  SemaAnnotator Annotator(*this);
  return performWalk(Annotator, [&]() { return E->walk(Annotator); });
}

bool SourceEntityWalker::walk(Pattern *P) {
  SemaAnnotator Annotator(*this);
  return performWalk(Annotator, [&]() { return P->walk(Annotator); });
}

bool SourceEntityWalker::walk(Decl *D) {
  SemaAnnotator Annotator(*this);
  return performWalk(Annotator, [&]() { return D->walk(Annotator); });
}

bool SourceEntityWalker::walk(DeclContext *DC) {
  SemaAnnotator Annotator(*this);
  return performWalk(Annotator, [&]() { return DC->walkContext(Annotator); });
}

bool SourceEntityWalker::walk(ASTNode N) {
  if (auto *E = N.dyn_cast<Expr*>())
    return walk(E);
  if (auto *S = N.dyn_cast<Stmt*>())
    return walk(S);
  if (auto *D = N.dyn_cast<Decl*>())
    return walk(D);

  llvm_unreachable("unsupported AST node");
}

bool SourceEntityWalker::visitDeclReference(ValueDecl *D, CharSourceRange Range,
                                            TypeDecl *CtorTyRef,
                                            ExtensionDecl *ExtTyRef, Type T,
                                            ReferenceMetaData Data) {
  return true;
}

bool SourceEntityWalker::visitSubscriptReference(ValueDecl *D,
                                                 CharSourceRange Range,
                                                 ReferenceMetaData Data,
                                                 bool IsOpenBracket) {
  // Most of the clients treat subscript reference the same way as a
  // regular reference when called on the open bracket and
  // ignore the closing one.
  return IsOpenBracket
             ? visitDeclReference(D, Range, nullptr, nullptr, Type(), Data)
             : true;
}

bool SourceEntityWalker::visitCallAsFunctionReference(ValueDecl *D,
                                                      CharSourceRange Range,
                                                      ReferenceMetaData Data) {
  return true;
}

bool SourceEntityWalker::visitCallArgName(Identifier Name,
                                          CharSourceRange Range,
                                          ValueDecl *D) {
  return true;
}

bool SourceEntityWalker::
visitDeclarationArgumentName(Identifier Name, SourceLoc Start, ValueDecl *D) {
  return true;
}

bool SourceEntityWalker::visitModuleReference(ModuleEntity Mod,
                                              CharSourceRange Range) {
  return true;
}

void SourceEntityWalker::anchor() {}
