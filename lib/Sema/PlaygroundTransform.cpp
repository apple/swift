//===--- PlaygroundTransform.cpp - Playground Transform -------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements the playground transform for Swift.
//
//===----------------------------------------------------------------------===//

#include "InstrumenterSupport.h"

#include "swift/Subsystems.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DeclContext.h"
#include "swift/AST/Module.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/SourceFile.h"

#include <random>
#include <forward_list>

using namespace swift;
using namespace swift::instrumenter_support;

//===----------------------------------------------------------------------===//
// performPlaygroundTransform
//===----------------------------------------------------------------------===//

namespace {

class Instrumenter : InstrumenterBase {
private:
  std::mt19937_64 &RNG;
  unsigned &TmpNameIndex;
  bool HighPerformance;
  bool ExtendedCallbacks;

  DeclNameRef DebugPrintName;
  DeclNameRef PrintName;
  DeclNameRef PostPrintName;
  DeclNameRef PostPrintExtendedName;
  DeclNameRef LogWithIDName;
  DeclNameRef LogWithIDExtendedName;
  DeclNameRef LogScopeExitName;
  DeclNameRef LogScopeExitExtendedName;
  DeclNameRef LogScopeEntryName;
  DeclNameRef LogScopeEntryExtendedName;
  DeclNameRef SendDataName;

  struct BracePair {
  public:
    SourceRange BraceRange;
    enum class TargetKinds { None = 0, Break, Return, Fallthrough };
    TargetKinds TargetKind = TargetKinds::None;

    BracePair(const SourceRange &BR) : BraceRange(BR) {}
  };

  using BracePairStack = std::forward_list<BracePair>;

  BracePairStack BracePairs;
  class BracePairPusher {
    BracePairStack &BracePairs;
    bool Valid = false;

  public:
    BracePairPusher(BracePairStack &BPS, const SourceRange &BR)
        : BracePairs(BPS) {
      BracePairs.push_front(BracePair(BR));
      Valid = true;
    }
    ~BracePairPusher() {
      if (isValid()) {
        Valid = false;
        BracePairs.pop_front();
      }
    }
    void invalidate() {
      if (isValid()) {
        Valid = false;
        BracePairs.pop_front();
      }
    }
    bool isValid() { return Valid; }
  };

  class TargetKindSetter {
    BracePairStack &BracePairs;

  public:
    TargetKindSetter(BracePairStack &BPS, BracePair::TargetKinds Kind)
        : BracePairs(BPS) {
      assert(!BracePairs.empty());
      assert(BracePairs.front().TargetKind == BracePair::TargetKinds::None);
      BracePairs.front().TargetKind = Kind;
    }
    ~TargetKindSetter() {
      BracePairs.front().TargetKind = BracePair::TargetKinds::None;
    }
  };

  using ElementVector = SmallVector<swift::ASTNode, 3>;

  // Before a "return," "continue" or similar statement, emit pops of
  // all the braces up to its target.
  size_t escapeToTarget(BracePair::TargetKinds TargetKind,
                        ElementVector &Elements, size_t EI) {
    if (HighPerformance)
      return EI;

    for (const BracePair &BP : BracePairs) {
      if (BP.TargetKind == TargetKind) {
        break;
      }
      Elements.insert(Elements.begin() + EI, *buildScopeExit(BP.BraceRange));
      ++EI;
    }
    return EI;
  }

public:
  Instrumenter(ASTContext &C, DeclContext *DC, std::mt19937_64 &RNG, bool HP,
               unsigned &TmpNameIndex)
      : InstrumenterBase(C, DC), RNG(RNG), TmpNameIndex(TmpNameIndex),
        HighPerformance(HP),
        ExtendedCallbacks(C.LangOpts.hasFeature(Feature::PlaygroundExtendedCallbacks)),
        DebugPrintName(C.getIdentifier("__builtin_debugPrint")),
        PrintName(C.getIdentifier("__builtin_print")),
        PostPrintName(C.getIdentifier("__builtin_postPrint")),
        PostPrintExtendedName(C.getIdentifier("__builtin_postPrint_extended")),
        LogWithIDName(C.getIdentifier("__builtin_log_with_id")),
        LogWithIDExtendedName(C.getIdentifier("__builtin_log_with_id_extended")),
        LogScopeExitName(C.getIdentifier("__builtin_log_scope_exit")),
        LogScopeExitExtendedName(C.getIdentifier("__builtin_log_scope_exit_extended")),
        LogScopeEntryName(C.getIdentifier("__builtin_log_scope_entry")),
        LogScopeEntryExtendedName(C.getIdentifier("__builtin_log_scope_entry_extended")),
        SendDataName(C.getIdentifier("__builtin_send_data")) { }

  Stmt *transformStmt(Stmt *S) {
    switch (S->getKind()) {
    default:
      return S;
    case StmtKind::Brace:
      return transformBraceStmt(cast<BraceStmt>(S));
    case StmtKind::Defer:
      return transformDeferStmt(cast<DeferStmt>(S));
    case StmtKind::If:
      return transformIfStmt(cast<IfStmt>(S));
    case StmtKind::Guard:
      return transformGuardStmt(cast<GuardStmt>(S));
    case StmtKind::While: {
      TargetKindSetter TKS(BracePairs, BracePair::TargetKinds::Break);
      return transformWhileStmt(cast<WhileStmt>(S));
    }
    case StmtKind::RepeatWhile: {
      TargetKindSetter TKS(BracePairs, BracePair::TargetKinds::Break);
      return transformRepeatWhileStmt(cast<RepeatWhileStmt>(S));
    }
    case StmtKind::ForEach: {
      TargetKindSetter TKS(BracePairs, BracePair::TargetKinds::Break);
      return transformForEachStmt(cast<ForEachStmt>(S));
    }
    case StmtKind::Switch: {
      TargetKindSetter TKS(BracePairs, BracePair::TargetKinds::Fallthrough);
      return transformSwitchStmt(cast<SwitchStmt>(S));
    }
    case StmtKind::Do:
      return transformDoStmt(llvm::cast<DoStmt>(S));
    case StmtKind::DoCatch:
      return transformDoCatchStmt(cast<DoCatchStmt>(S));
    }
  }

  DeferStmt *transformDeferStmt(DeferStmt *DS) {
    if (auto *FD = DS->getTempDecl()) {
      // Temporarily unmark the DeferStmt's FuncDecl as implicit so it is
      // transformed (as typically implicit Decls are skipped by the
      // transformer).
      auto Implicit = FD->isImplicit();
      FD->setImplicit(false);
      auto *D = transformDecl(FD);
      D->setImplicit(Implicit);
      assert(D == FD);
    }
    return DS;
  }

  // transform*() return their input if it's unmodified,
  // or a modified copy of their input otherwise.
  IfStmt *transformIfStmt(IfStmt *IS) {
    if (Stmt *TS = IS->getThenStmt()) {
      Stmt *NTS = transformStmt(TS);
      if (NTS != TS) {
        IS->setThenStmt(NTS);
      }
    }

    if (Stmt *ES = IS->getElseStmt()) {
      Stmt *NES = transformStmt(ES);
      if (NES != ES) {
        IS->setElseStmt(NES);
      }
    }

    return IS;
  }

  GuardStmt *transformGuardStmt(GuardStmt *GS) {
    if (BraceStmt *BS = GS->getBody())
      GS->setBody(transformBraceStmt(BS));
    return GS;
  }

  WhileStmt *transformWhileStmt(WhileStmt *WS) {
    if (Stmt *B = WS->getBody()) {
      Stmt *NB = transformStmt(B);
      if (NB != B) {
        WS->setBody(NB);
      }
    }

    return WS;
  }

  RepeatWhileStmt *transformRepeatWhileStmt(RepeatWhileStmt *RWS) {
    if (Stmt *B = RWS->getBody()) {
      Stmt *NB = transformStmt(B);
      if (NB != B) {
        RWS->setBody(NB);
      }
    }

    return RWS;
  }

  ForEachStmt *transformForEachStmt(ForEachStmt *FES) {
    if (BraceStmt *B = FES->getBody()) {
      BraceStmt *NB = transformBraceStmt(B);
      if (NB != B) {
        FES->setBody(NB);
      }
    }

    return FES;
  }

  SwitchStmt *transformSwitchStmt(SwitchStmt *SS) {
    for (CaseStmt *CS : SS->getCases()) {
      if (Stmt *S = CS->getBody()) {
        if (auto *B = dyn_cast<BraceStmt>(S)) {
          BraceStmt *NB = transformBraceStmt(B);
          if (NB != B) {
            CS->setBody(NB);
          }
        }
      }
    }

    return SS;
  }

  DoStmt *transformDoStmt(DoStmt *DS) {
    if (auto *B = dyn_cast_or_null<BraceStmt>(DS->getBody())) {
      BraceStmt *NB = transformBraceStmt(B);
      if (NB != B) {
        DS->setBody(NB);
      }
    }
    return DS;
  }

  DoCatchStmt *transformDoCatchStmt(DoCatchStmt *DCS) {
    if (auto *B = dyn_cast_or_null<BraceStmt>(DCS->getBody())) {
      BraceStmt *NB = transformBraceStmt(B);
      if (NB != B) {
        DCS->setBody(NB);
      }
    }
    for (CaseStmt *C : DCS->getCatches()) {
      if (auto *CB = dyn_cast_or_null<BraceStmt>(C->getBody())) {
        BraceStmt *NCB = transformBraceStmt(CB);
        if (NCB != CB) {
          C->setBody(NCB);
        }
      }
    }
    return DCS;
  }

  Decl *transformDecl(Decl *D) {
    if (D->isImplicit())
      return D;
    if (auto *FD = dyn_cast<FuncDecl>(D)) {
      if (BraceStmt *B = FD->getBody()) {
        const ParameterList *PL = FD->getParameters();
        TargetKindSetter TKS(BracePairs, BracePair::TargetKinds::Return);
        BraceStmt *NB = transformBraceStmt(B, PL);
        if (NB != B) {
          FD->setBody(NB, AbstractFunctionDecl::BodyKind::TypeChecked);
          TypeChecker::checkFunctionEffects(FD);
        }
      }
    } else if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
      for (Decl *Member : NTD->getMembers()) {
        transformDecl(Member);
      }
    }

    return D;
  }

  std::pair<Added<Expr *>, ValueDecl *> digForVariable(Expr *E) {
    switch (E->getKind()) {
    default:
      if (auto *ICE = dyn_cast<ImplicitConversionExpr>(E))
        return digForVariable(ICE->getSubExpr());
      return std::make_pair(Added<Expr *>(nullptr), nullptr);
    case ExprKind::DeclRef: {
      ValueDecl *D = cast<DeclRefExpr>(E)->getDecl();
      Added<Expr *> DRE = new (Context) DeclRefExpr(
          ConcreteDeclRef(D), cast<DeclRefExpr>(E)->getNameLoc(),
          true, // implicit
          AccessSemantics::Ordinary, cast<DeclRefExpr>(E)->getType());
      return std::make_pair(DRE, D);
    }
    case ExprKind::MemberRef: {
      std::pair<Added<Expr *>, ValueDecl *> BaseVariable =
          digForVariable(cast<MemberRefExpr>(E)->getBase());
      if (!*BaseVariable.first || !BaseVariable.second)
        return std::make_pair(nullptr, nullptr);
      ValueDecl *M = cast<MemberRefExpr>(E)->getMember().getDecl();
      Added<Expr *> MRE(new (Context) MemberRefExpr(
          *BaseVariable.first, cast<MemberRefExpr>(E)->getDotLoc(),
          ConcreteDeclRef(M), cast<MemberRefExpr>(E)->getNameLoc(),
          true, // implicit
          AccessSemantics::Ordinary));
      return std::make_pair(MRE, M);
    }
    case ExprKind::Load:
      return digForVariable(cast<LoadExpr>(E)->getSubExpr());
    case ExprKind::ForceValue: {
      std::pair<Added<Expr *>, ValueDecl *> BaseVariable =
          digForVariable(cast<ForceValueExpr>(E)->getSubExpr());
      if (!*BaseVariable.first || !BaseVariable.second)
        return std::make_pair(nullptr, nullptr);

      Added<Expr *> Forced(
          new (Context) ForceValueExpr(*BaseVariable.first, SourceLoc()));
      return std::make_pair(Forced, BaseVariable.second);
    }
    case ExprKind::InOut:
      return digForVariable(cast<InOutExpr>(E)->getSubExpr());
    }
  }

  DeclBaseName digForName(Expr *E) {
    Added<Expr *> RE = nullptr;
    ValueDecl *VD = nullptr;
    std::tie(RE, VD) = digForVariable(E);
    if (VD) {
      return VD->getBaseName();
    } else {
      return DeclBaseName();
    }
  }

  static DeclRefExpr *digForInoutDeclRef(ArgumentList *args) {
    DeclRefExpr *uniqueRef = nullptr;
    for (auto arg : *args) {
      if (auto *inout = dyn_cast<InOutExpr>(arg.getExpr())) {
        auto *ref = dyn_cast<DeclRefExpr>(
            inout->getSubExpr()->getSemanticsProvidingExpr());
        if (!ref)
          continue;

        // If we already have a reference, it's not unique.
        if (uniqueRef)
          return nullptr;

        uniqueRef = ref;
      }
    }
    return uniqueRef;
  }

  BraceStmt *transformBraceStmt(BraceStmt *BS,
                                const ParameterList *PL = nullptr,
                                bool TopLevel = false) override {
    ArrayRef<ASTNode> OriginalElements = BS->getElements();
    using ElementVector = SmallVector<swift::ASTNode, 3>;
    ElementVector Elements(OriginalElements.begin(), OriginalElements.end());

    SourceRange SR = BS->getSourceRange();
    BracePairPusher BPP(BracePairs, SR);

    for (size_t EI = 0; EI != Elements.size(); ++EI) {
      swift::ASTNode &Element = Elements[EI];
      if (auto *E = Element.dyn_cast<Expr *>()) {
        E->walk(CF);
        if (auto *AE = dyn_cast<AssignExpr>(E)) {
          if (auto *MRE = dyn_cast<MemberRefExpr>(AE->getDest())) {
            // an assignment to a property of an object counts as a mutation of
            // that object
            Added<Expr *> Base_RE = nullptr;
            ValueDecl *BaseVD = nullptr;
            std::tie(Base_RE, BaseVD) = digForVariable(MRE->getBase());

            if (*Base_RE) {
              Added<Stmt *> Log = logDeclOrMemberRef(Base_RE);
              if (*Log) {
                Elements.insert(Elements.begin() + (EI + 1), *Log);
                ++EI;
              }
            }
          } else {
            std::pair<PatternBindingDecl *, VarDecl *> PV =
                buildPatternAndVariable(AE->getSrc());
            DeclRefExpr *DRE = new (Context)
                DeclRefExpr(ConcreteDeclRef(PV.second), DeclNameLoc(),
                            true, // implicit
                            AccessSemantics::Ordinary, AE->getSrc()->getType());
            AssignExpr *NAE =
                new (Context) AssignExpr(AE->getDest(), SourceLoc(), DRE,
                                         true); // implicit
            NAE->setType(Context.TheEmptyTupleType);
            AE->setImplicit(true);

            DeclBaseName Name = digForName(AE->getDest());
            Expr * PVVarRef = new (Context) DeclRefExpr(
                ConcreteDeclRef(PV.second), DeclNameLoc(), /*implicit=*/ true,
                AccessSemantics::Ordinary, AE->getSrc()->getType());
            Added<Stmt *> Log(
                buildLoggerCall(PVVarRef, AE->getSrc()->getSourceRange(),
                                Name.getIdentifier().str()));

            if (*Log) {
              Elements[EI] = PV.first;
              Elements.insert(Elements.begin() + (EI + 1), PV.second);
              Elements.insert(Elements.begin() + (EI + 2), *Log);
              Elements.insert(Elements.begin() + (EI + 3), NAE);
              EI += 3;
            }
          }
        } else if (auto *AE = dyn_cast<ApplyExpr>(E)) {
          bool Handled = false;
          if (auto *DRE = dyn_cast<DeclRefExpr>(AE->getFn())) {
            auto *FnD = dyn_cast<AbstractFunctionDecl>(DRE->getDecl());
            if (FnD && FnD->getModuleContext() == Context.TheStdlibModule) {
              DeclBaseName FnName = FnD->getBaseName();
              if (FnName == "print" || FnName == "debugPrint") {
                const bool isOldStyle = false;
                if (isOldStyle) {
                  const bool isDebugPrint = (FnName == "debugPrint");
                  PatternBindingDecl *ArgPattern = nullptr;
                  VarDecl *ArgVariable = nullptr;
                  Added<Stmt *> Log =
                      logPrint(isDebugPrint, AE, ArgPattern, ArgVariable);
                  if (*Log) {
                    if (ArgPattern) {
                      assert(ArgVariable);
                      Elements[EI] = ArgPattern;
                      Elements.insert(Elements.begin() + (EI + 1), ArgVariable);
                      Elements.insert(Elements.begin() + (EI + 2), *Log);
                      EI += 2;
                    } else {
                      Elements[EI] = *Log;
                    }
                  }
                } else {
                  Added<Stmt *> Log = logPostPrint(AE->getSourceRange());
                  Elements.insert(Elements.begin() + (EI + 1), *Log);
                  EI += 1;
                }
                Handled = true;
              }
            }
          }
          if (!Handled &&
              AE->getType()->isEqual(Context.TheEmptyTupleType)) {
            if (auto *DSCE = dyn_cast<DotSyntaxCallExpr>(AE->getFn())) {
              Expr *TargetExpr = DSCE->getBase();
              Added<Expr *> Target_RE;
              ValueDecl *TargetVD = nullptr;

              std::tie(Target_RE, TargetVD) = digForVariable(TargetExpr);

              if (TargetVD) {
                Added<Stmt *> Log = logDeclOrMemberRef(Target_RE);
                if (*Log) {
                  Elements.insert(Elements.begin() + (EI + 1), *Log);
                  ++EI;
                }
                Handled = true;
              }
            }
            if (!Handled) {
              if (DeclRefExpr *DRE = digForInoutDeclRef(AE->getArgs())) {
                Added<Stmt *> Log = logDeclOrMemberRef(DRE);
                if (*Log) {
                  Elements.insert(Elements.begin() + (EI + 1), *Log);
                  ++EI;
                }
              }
            }
            Handled = true; // Never log ()
          }
          if (!Handled) {
            // do the same as for all other expressions
            std::pair<PatternBindingDecl *, VarDecl *> PV =
                buildPatternAndVariable(E);
            Added<Stmt *> Log = buildLoggerCall(
                new (Context)
                    DeclRefExpr(ConcreteDeclRef(PV.second), DeclNameLoc(),
                                true, // implicit
                                AccessSemantics::Ordinary, E->getType()),
                E->getSourceRange(), "");
            if (*Log) {
              Elements[EI] = PV.first;
              Elements.insert(Elements.begin() + (EI + 1), PV.second);
              Elements.insert(Elements.begin() + (EI + 2), *Log);
              EI += 2;
            }
          }
        } else {
          if (E->getType()->getCanonicalType() != Context.TheEmptyTupleType) {
            std::pair<PatternBindingDecl *, VarDecl *> PV =
                buildPatternAndVariable(E);
            Added<Stmt *> Log = buildLoggerCall(
                new (Context)
                    DeclRefExpr(ConcreteDeclRef(PV.second), DeclNameLoc(),
                                true, // implicit
                                AccessSemantics::Ordinary, E->getType()),
                E->getSourceRange(), "");
            if (*Log) {
              Elements[EI] = PV.first;
              Elements.insert(Elements.begin() + (EI + 1), PV.second);
              Elements.insert(Elements.begin() + (EI + 2), *Log);
              EI += 2;
            }
          }
        }
      } else if (auto *S = Element.dyn_cast<Stmt *>()) {
        S->walk(CF);
        if (auto *RS = dyn_cast<ReturnStmt>(S)) {
          if (RS->hasResult()) {
            std::pair<PatternBindingDecl *, VarDecl *> PV =
                buildPatternAndVariable(RS->getResult());
            DeclRefExpr *DRE = new (Context) DeclRefExpr(
                ConcreteDeclRef(PV.second), DeclNameLoc(),
                true, // implicit
                AccessSemantics::Ordinary, RS->getResult()->getType());
            ReturnStmt *NRS = new (Context) ReturnStmt(SourceLoc(), DRE,
                                                       true); // implicit
            Added<Stmt *> Log = buildLoggerCall(
                new (Context) DeclRefExpr(
                    ConcreteDeclRef(PV.second), DeclNameLoc(),
                    true, // implicit
                    AccessSemantics::Ordinary, RS->getResult()->getType()),
                RS->getResult()->getSourceRange(), "");
            if (*Log) {
              Elements[EI] = PV.first;
              Elements.insert(Elements.begin() + (EI + 1), PV.second);
              Elements.insert(Elements.begin() + (EI + 2), *Log);
              Elements.insert(Elements.begin() + (EI + 3), NRS);
              EI += 3;
            }
          }
          EI = escapeToTarget(BracePair::TargetKinds::Return, Elements, EI);
        } else {
          if (isa<BreakStmt>(S) || isa<ContinueStmt>(S)) {
            EI = escapeToTarget(BracePair::TargetKinds::Break, Elements, EI);
          } else if (isa<FallthroughStmt>(S)) {
            EI = escapeToTarget(BracePair::TargetKinds::Fallthrough, Elements,
                                EI);
          }
          Stmt *NS = transformStmt(S);
          if (NS != S) {
            Elements[EI] = NS;
          }
        }
      } else if (auto *D = Element.dyn_cast<Decl *>()) {
        D->walk(CF);
        if (auto *PBD = dyn_cast<PatternBindingDecl>(D)) {
          if (!PBD->isAsyncLet()) {
            if (VarDecl *VD = PBD->getSingleVar()) {
              if (VD->getParentExecutableInitializer()) {
                Added<Stmt *> Log = logVarDecl(VD);
                if (*Log) {
                  Elements.insert(Elements.begin() + (EI + 1), *Log);
                  ++EI;
                }
              }
            }
          }
        } else {
          transformDecl(D);
        }
      }
    }

    // If we were given any parameters that apply to this brace block, we insert
    // log calls for them now (before any of the other statements). We only log
    // named parameters (not `{ _ in ... }`, for example).
    if (PL && !HighPerformance) {
      size_t EI = 0;
      for (const auto &PD : *PL) {
        if (PD->hasName()) {
          DeclBaseName Name = PD->getName();
          Expr *PVVarRef = new (Context)
              DeclRefExpr(PD, DeclNameLoc(), /*implicit=*/true,
                          AccessSemantics::Ordinary, PD->getTypeInContext());
          Added<Stmt *> Log(buildLoggerCall(PVVarRef, PD->getSourceRange(),
                                            Name.getIdentifier().str()));
          if (*Log) {
            Elements.insert(Elements.begin() + EI, *Log);
            EI++;
          }
        }
      }
    }

    if (!TopLevel && !HighPerformance && !BS->isImplicit()) {
      Elements.insert(Elements.begin(), *buildScopeEntry(BS->getSourceRange()));
      Elements.insert(Elements.end(), *buildScopeExit(BS->getSourceRange()));
    }

    // Remove null elements from the list.
    // FIXME: This is a band-aid used to work around the fact that the
    // above code can introduce null elements into the vector. The
    // right fix is to avoid doing that above.
    Elements.erase(std::remove_if(Elements.begin(), Elements.end(),
                                  [](ASTNode node) { return node.isNull(); }),
                   Elements.end());

    return swift::BraceStmt::create(Context, BS->getLBraceLoc(),
                                    Context.AllocateCopy(Elements),
                                    BS->getRBraceLoc());
  }

  // log*() functions return a newly-created log expression to be inserted
  // after or instead of the expression they're looking at.  Only call this
  // if the variable has an initializer.
  Added<Stmt *> logVarDecl(VarDecl *VD) {
    if (isa<ConstructorDecl>(TypeCheckDC) && VD->getNameStr().equals("self")) {
      // Don't log "self" in a constructor
      return nullptr;
    }

    return buildLoggerCall(
        new (Context) DeclRefExpr(ConcreteDeclRef(VD), DeclNameLoc(),
                                  true, // implicit
                                  AccessSemantics::Ordinary, Type()),
        VD->getSourceRange(), VD->getName().str());
  }

  Added<Stmt *> logDeclOrMemberRef(Added<Expr *> RE) {
    if (auto *DRE = dyn_cast<DeclRefExpr>(*RE)) {
      VarDecl *VD = cast<VarDecl>(DRE->getDecl());

      if (isa<ConstructorDecl>(TypeCheckDC) && VD->getBaseName() == "self") {
        // Don't log "self" in a constructor
        return nullptr;
      }

      return buildLoggerCall(
          new (Context) DeclRefExpr(ConcreteDeclRef(VD), DeclNameLoc(),
                                    /*implicit=*/true),
          DRE->getSourceRange(), VD->getName().str());
    } else if (auto *MRE = dyn_cast<MemberRefExpr>(*RE)) {
      Expr *B = MRE->getBase();
      ConcreteDeclRef M = MRE->getMember();

      if (isa<ConstructorDecl>(TypeCheckDC) && digForName(B) == "self") {
        // Don't log attributes of "self" in a constructor
        return nullptr;
      }

      return buildLoggerCall(
          new (Context) MemberRefExpr(B, SourceLoc(), M, DeclNameLoc(),
                                      /*implicit=*/true),
          MRE->getSourceRange(),
          M.getDecl()->getBaseName().userFacingName());
    } else {
      return nullptr;
    }
  }

  std::pair<PatternBindingDecl *, VarDecl *>
  maybeFixupPrintArgument(ApplyExpr *Print) {
    auto *args = Print->getArgs();
    if (args->empty())
      return std::make_pair(nullptr, nullptr);

    // Are we using print() specialized to handle a single argument,
    // or is actually only the first argument of interest and the rest are
    // extra information for print()?
    if (args->hasAnyArgumentLabels() || args->hasAnyInOutArgs()) {
      auto *argExpr = args->front().getExpr();
      auto PV = buildPatternAndVariable(argExpr);
      args->setExpr(0, new (Context) DeclRefExpr(
          ConcreteDeclRef(PV.second), DeclNameLoc(),
          true, // implicit
          AccessSemantics::Ordinary, argExpr->getType()));
      return PV;
    }

    auto *packedArg = args->packIntoImplicitTupleOrParen(Context);
    auto PV = buildPatternAndVariable(packedArg);
    auto newArg = new (Context)
        DeclRefExpr(ConcreteDeclRef(PV.second), DeclNameLoc(),
                    true, // implicit
                    AccessSemantics::Ordinary, packedArg->getType());
    Print->setArgs(ArgumentList::createImplicit(Context, args->getLParenLoc(),
                                                {Argument::unlabeled(newArg)},
                                                args->getRParenLoc()));
    return PV;
  }

  Added<Stmt *> logPrint(bool isDebugPrint, ApplyExpr *AE,
                         PatternBindingDecl *&ArgPattern,
                         VarDecl *&ArgVariable) {
    DeclNameRef LoggerName = isDebugPrint ? DebugPrintName : PrintName;

    UnresolvedDeclRefExpr *LoggerRef = new (Context) UnresolvedDeclRefExpr(
        LoggerName, DeclRefKind::Ordinary,
        DeclNameLoc(AE->getSourceRange().End));

    std::tie(ArgPattern, ArgVariable) = maybeFixupPrintArgument(AE);

    AE->setFn(LoggerRef);
    Added<ApplyExpr *> AddedApply(AE); // safe because we've fixed up the args

    if (!doTypeCheck(Context, TypeCheckDC, AddedApply)) {
      return nullptr;
    }

    return buildLoggerCallWithApply(AddedApply, AE->getSourceRange());
  }

  Added<Stmt *> logPostPrint(SourceRange SR) {
    return buildLoggerCallWithArgs(ExtendedCallbacks ? PostPrintExtendedName : PostPrintName, {}, SR);
  }

  std::pair<PatternBindingDecl *, VarDecl *>
  buildPatternAndVariable(Expr *InitExpr) {
    SmallString<16> NameBuf;
    (Twine("tmp") + Twine(TmpNameIndex++)).toVector(NameBuf);

    Expr *MaybeLoadInitExpr = nullptr;

    if (LValueType *LVT = InitExpr->getType()->getAs<LValueType>()) {
      MaybeLoadInitExpr =
          new (Context) LoadExpr(InitExpr, LVT->getObjectType());
    } else {
      MaybeLoadInitExpr = InitExpr;
    }

    VarDecl *VD =
        new (Context) VarDecl(/*IsStatic*/false, VarDecl::Introducer::Let,
                              SourceLoc(), Context.getIdentifier(NameBuf),
                              TypeCheckDC);
    VD->setInterfaceType(MaybeLoadInitExpr->getType()->mapTypeOutOfContext());
    VD->setImplicit();

    NamedPattern *NP = NamedPattern::createImplicit(Context, VD, VD->getTypeInContext());
    PatternBindingDecl *PBD = PatternBindingDecl::createImplicit(
        Context, StaticSpellingKind::None, NP, MaybeLoadInitExpr, TypeCheckDC);

    return std::make_pair(PBD, VD);
  }

  Added<Stmt *> buildLoggerCall(Added<Expr *> E, SourceRange SR,
                                StringRef Name) {
    Expr *NameExpr = new (Context) StringLiteralExpr(
        Context.AllocateCopy(Name), SourceRange(), /*implicit=*/true);

    std::uniform_int_distribution<unsigned> Distribution(0, 0x7fffffffu);
    const unsigned id_num = Distribution(RNG);
    Expr *IDExpr = IntegerLiteralExpr::createFromUnsigned(Context, id_num, SourceLoc());

    return buildLoggerCallWithArgs(ExtendedCallbacks ? LogWithIDExtendedName : LogWithIDName, { *E, NameExpr, IDExpr }, SR);
  }

  Added<Stmt *> buildScopeEntry(SourceRange SR) {
    return buildLoggerCallWithArgs(ExtendedCallbacks ? LogScopeEntryExtendedName : LogScopeEntryName, {}, SR);
  }

  Added<Stmt *> buildScopeExit(SourceRange SR) {
    return buildLoggerCallWithArgs(ExtendedCallbacks ? LogScopeExitExtendedName : LogScopeExitName, {}, SR);
  }

  Added<Stmt *> buildLoggerCallWithArgs(DeclNameRef LoggerName,
                                        ArrayRef<Expr *> Args,
                                        SourceRange SR) {
    // If something doesn't have a valid source range it can not be playground
    // logged. For example, a PC Macro event.
    if (!SR.isValid()) {
      return nullptr;
    }

    std::pair<unsigned, unsigned> StartLC =
        Context.SourceMgr.getPresumedLineAndColumnForLoc(SR.Start);

    std::pair<unsigned, unsigned> EndLC =
        Context.SourceMgr.getPresumedLineAndColumnForLoc(
            Lexer::getLocForEndOfToken(Context.SourceMgr, SR.End));

    Expr *StartLine = IntegerLiteralExpr::createFromUnsigned(Context, StartLC.first, SR.Start);
    Expr *EndLine = IntegerLiteralExpr::createFromUnsigned(Context, EndLC.first, SR.End);
    Expr *StartColumn = IntegerLiteralExpr::createFromUnsigned(Context, StartLC.second, SR.Start);
    Expr *EndColumn = IntegerLiteralExpr::createFromUnsigned(Context, EndLC.second, SR.End);

    Expr *ModuleExpr = buildIDArgumentExpr(ModuleIdentifier, SR);
    Expr *FileExpr = buildIDArgumentExpr(FileIdentifier, SR);

    llvm::SmallVector<Expr *, 6> ArgsWithSourceRange(Args.begin(), Args.end());

    UnresolvedDeclRefExpr *LoggerRef = new (Context)
        UnresolvedDeclRefExpr(LoggerName, DeclRefKind::Ordinary,
                              DeclNameLoc(SR.End));
    LoggerRef->setImplicit(true);

    if (ExtendedCallbacks) {
      StringRef filePath = Context.SourceMgr.getDisplayNameForLoc(SR.Start);

      Expr *FilePathExpr = new (Context) StringLiteralExpr(
          Context.AllocateCopy(filePath), SourceRange(), /*implicit=*/true);

      std::string moduleName = std::string(TypeCheckDC->getParentModule()->getName());
      Expr *ModuleNameExpr = new (Context) StringLiteralExpr(
          Context.AllocateCopy(moduleName), SourceRange(), /*implicit=*/true);

      ArgsWithSourceRange.append(
          {StartLine, EndLine, StartColumn, EndColumn, ModuleNameExpr, FilePathExpr});
    } else {
      ArgsWithSourceRange.append(
          {StartLine, EndLine, StartColumn, EndColumn, ModuleExpr, FileExpr});
    }

    auto *ArgList =
        ArgumentList::forImplicitUnlabeled(Context, ArgsWithSourceRange);
    ApplyExpr *LoggerCall =
        CallExpr::createImplicit(Context, LoggerRef, ArgList);
    Added<ApplyExpr *> AddedLogger(LoggerCall);

    if (!doTypeCheck(Context, TypeCheckDC, AddedLogger)) {
      return nullptr;
    }

    return buildLoggerCallWithApply(AddedLogger, SR);
  }

  // Assumes Apply has already been type-checked.
  Added<Stmt *> buildLoggerCallWithApply(Added<ApplyExpr *> Apply,
                                         SourceRange SR) {
    std::pair<PatternBindingDecl *, VarDecl *> PV =
        buildPatternAndVariable(*Apply);

    DeclRefExpr *DRE =
        new (Context) DeclRefExpr(ConcreteDeclRef(PV.second), DeclNameLoc(),
                                  true, // implicit
                                  AccessSemantics::Ordinary, Apply->getType());

    UnresolvedDeclRefExpr *SendDataRef = new (Context)
        UnresolvedDeclRefExpr(SendDataName, DeclRefKind::Ordinary,
                              DeclNameLoc());

    SendDataRef->setImplicit(true);

    auto *ArgList = ArgumentList::forImplicitUnlabeled(Context, {DRE});
    Expr *SendDataCall =
        CallExpr::createImplicit(Context, SendDataRef, ArgList);
    Added<Expr *> AddedSendData(SendDataCall);

    if (!doTypeCheck(Context, TypeCheckDC, AddedSendData)) {
      return nullptr;
    }

    ASTNode Elements[] = {PV.first, PV.second, SendDataCall};

    BraceStmt *BS =
        BraceStmt::create(Context, SourceLoc(), Elements, SourceLoc(), true);

    return BS;
  }
};

} // end anonymous namespace

void swift::performPlaygroundTransform(SourceFile &SF, bool HighPerformance) {
  class ExpressionFinder : public ASTWalker {
  private:
    ASTContext &ctx;
    std::mt19937_64 RNG;
    bool HighPerformance;
    unsigned TmpNameIndex = 0;

  public:
    ExpressionFinder(ASTContext &ctx, bool HP) : ctx(ctx), HighPerformance(HP) {}

    // FIXME: Remove this
    bool shouldWalkAccessorsTheOldWay() override { return true; }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkAction walkToDeclPre(Decl *D) override {
      if (auto *FD = dyn_cast<AbstractFunctionDecl>(D)) {
        if (!FD->isImplicit()) {
          if (BraceStmt *Body = FD->getBody()) {
            const ParameterList *PL = FD->getParameters();
            Instrumenter I(ctx, FD, RNG, HighPerformance, TmpNameIndex);
            BraceStmt *NewBody = I.transformBraceStmt(Body, PL);
            if (NewBody != Body) {
              FD->setBody(NewBody, AbstractFunctionDecl::BodyKind::TypeChecked);
              TypeChecker::checkFunctionEffects(FD);
            }
            return Action::SkipChildren();
          }
        }
      } else if (auto *TLCD = dyn_cast<TopLevelCodeDecl>(D)) {
        if (!TLCD->isImplicit()) {
          if (BraceStmt *Body = TLCD->getBody()) {
            Instrumenter I(ctx, TLCD, RNG, HighPerformance, TmpNameIndex);
            BraceStmt *NewBody = I.transformBraceStmt(Body, nullptr, true);
            if (NewBody != Body) {
              TLCD->setBody(NewBody);
              TypeChecker::checkTopLevelEffects(TLCD);
            }
            return Action::SkipChildren();
          }
        }
      }
      return Action::Continue();
    }
  };

  ExpressionFinder EF(SF.getASTContext(), HighPerformance);
  SF.walk(EF);
}
