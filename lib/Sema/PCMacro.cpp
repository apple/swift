//===--- PCMacro.cpp - PCMacro --------------------------------------------===//
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
//  This file implements the 'program counter simulation' for Swift.
//  Based off the PlaygroundTransform, PCMacro instruments code to call
//  functions at times that a debugger would show the program counter move.
//  It can be used to collect and display information about the flow of control
//  through Swift code in "live coding" environments like Playgrounds without
//  resorting to more heavyweight mechanisms like profiling.
//
//  More specifically, this transformation inserts calls to visible functions
//  with these names and signatures (other integer types should work too):
//
//      func __builtin_pc_before(
//          _ startLine: Int, _ endLine: Int,
//          _ startColumn: Int, _ endColumn: Int,
//          _ moduleID: Int, _ fileID: Int
//      ) -> Void
//      func __builtin_pc_after(
//          _ startLine: Int, _ endLine: Int,
//          _ startColumn: Int, _ endColumn: Int,
//          _ moduleID: Int, _ fileID: Int
//      ) -> Void
//
//  The `startLine`, `endLine`, `startColumn`, and `endColumn` parameters are
//  passed 1-based integer literals; 0 is used for invalid (i.e.
//  compiler-generated) code. The `moduleID` and `fileID` parameters are passed
//  the values of visible variables or constants named
//  `__builtin_pg_module_<module name>` and
//  `__builtin_pg_file_<file base name>`, or an integer literal 0 if suitable
//  variables are not found.
//
//  The transform inserts these calls before and after each statement, as well
//  as before and after expressions nested inside statements, such as `if` and
//  `while` conditions and `var` and `let` initial values.
//
//===----------------------------------------------------------------------===//

#include "InstrumenterSupport.h"

#include "swift/Subsystems.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Module.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"

using namespace swift;
using namespace swift::instrumenter_support;

//===----------------------------------------------------------------------===//
// performPCMacro
//===----------------------------------------------------------------------===//

namespace {

class Instrumenter : InstrumenterBase {
private:
  unsigned &TmpNameIndex;
  DeclNameRef LogBeforeName;
  DeclNameRef LogAfterName;

public:
  Instrumenter(ASTContext &C, DeclContext *DC, unsigned &TmpNameIndex)
      : InstrumenterBase(C, DC), TmpNameIndex(TmpNameIndex),
        LogBeforeName(C.getIdentifier("__builtin_pc_before")),
        LogAfterName(C.getIdentifier("__builtin_pc_after")) {}

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
      return transformWhileStmt(cast<WhileStmt>(S));
    }
    case StmtKind::RepeatWhile: {
      return transformRepeatWhileStmt(cast<RepeatWhileStmt>(S));
    }
    case StmtKind::ForEach: {
      return transformForEachStmt(cast<ForEachStmt>(S));
    }
    case StmtKind::Switch: {
      return transformSwitchStmt(cast<SwitchStmt>(S));
    }
    case StmtKind::Do:
      return transformDoStmt(llvm::cast<DoStmt>(S));
    case StmtKind::DoCatch:
      return transformDoCatchStmt(cast<DoCatchStmt>(S));
    }
  }

  void transformStmtCondition(StmtCondition SC, SourceLoc StartLoc) {
    // Right now, only handle if statements with one condition
    if (SC.size() == 1) {
      StmtConditionElement *SCE = SC.begin();
      switch (SCE->getKind()) {
      case StmtConditionElement::ConditionKind::CK_Boolean: {
        Expr *E = SCE->getBoolean();
        SourceLoc EndLoc = E->getEndLoc();
        if (StartLoc.isValid() && EndLoc.isValid()) {
          Expr *NE = buildInlineLoggerCall({StartLoc, EndLoc}, E);
          SCE->setBoolean(NE);
        }
      } break;
      case StmtConditionElement::ConditionKind::CK_PatternBinding: {
        Expr *E = SCE->getInitializer();
        SourceLoc EndLoc = E->getEndLoc();
        if (StartLoc.isValid() && EndLoc.isValid()) {
          Expr *NE = buildInlineLoggerCall({StartLoc, EndLoc}, E);
          SCE->setInitializer(NE);
        }
      } break;
      default:;
      }
    }
  }

  // transform*() return their input if it's unmodified,
  // or a modified copy of their input otherwise.
  IfStmt *transformIfStmt(IfStmt *IS) {
    StmtCondition SC = IS->getCond();
    transformStmtCondition(SC, IS->getStartLoc());
    IS->setCond(SC); // FIXME: is setting required?..

    if (Stmt *TS = IS->getThenStmt()) {
      Stmt *NTS = transformStmt(TS);
      if (NTS != TS) {
        IS->setThenStmt(NTS);
      }
    }

    if (Stmt *ES = IS->getElseStmt()) {
      SourceLoc ElseLoc = IS->getElseLoc(); // FIXME: got to pass this back into
                                            // transformStmt if the else stmt is
                                            // an IfStmt. Then we prepend this
                                            // range to the ifstmt highlight.
                                            // See the elseif.swift test.
      Stmt *NES = transformStmt(ES);
      if (ElseLoc.isValid()) {
        if (auto *BS = dyn_cast<BraceStmt>(NES)) {
          BraceStmt *NBS = prependLoggerCall(BS, ElseLoc);
          if (NBS != ES) {
            IS->setElseStmt(NBS);
          }
        } else if (auto *EIS = dyn_cast<IfStmt>(NES)) {
          // FIXME: here we should use the old range to show a better highlight
          // (including the previous else)
          if (EIS != ES) {
            IS->setElseStmt(EIS);
          }
        } else {
          llvm_unreachable(
              "IfStmt else stmts must be either IfStmt or BraceStmt");
        }
      } else {
        if (NES != ES) {
          IS->setElseStmt(NES);
        }
      }
    }

    return IS;
  }

  GuardStmt *transformGuardStmt(GuardStmt *GS) {
    StmtCondition SC = GS->getCond();
    transformStmtCondition(SC, GS->getStartLoc());
    GS->setCond(SC);

    if (BraceStmt *BS = GS->getBody())
      GS->setBody(transformBraceStmt(BS));
    return GS;
  }

  WhileStmt *transformWhileStmt(WhileStmt *WS) {
    StmtCondition SC = WS->getCond();
    transformStmtCondition(SC, WS->getStartLoc());
    WS->setCond(SC);

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

      // point at the for stmt, to look nice
      SourceLoc StartLoc = FES->getStartLoc();
      SourceLoc EndLoc = FES->getParsedSequence()->getEndLoc();
      // FIXME: get the 'end' of the for stmt
      // if (FD->getResultTypeRepr()) {
      //   EndLoc = FD->getResultTypeSourceRange().End;
      // } else {
      //   EndLoc = FD->getParameters()->getSourceRange().End;
      // }

      if (StartLoc.isValid() && EndLoc.isValid()) {
        BraceStmt *NNB = prependLoggerCall(NB, {StartLoc, EndLoc});
        if (NNB != B) {
          FES->setBody(NNB);
        }
      } else {
        if (NB != B) {
          FES->setBody(NB);
        }
      }
    }

    return FES;
  }

  SwitchStmt *transformSwitchStmt(SwitchStmt *SS) {
    // Get the subject range (and switch keyword) and begin by pointing at that
    // range. Then stop pointing at it (for now, until we can replace the
    // switch subject expr).
    // Insert both these stmts before the SwitchStmt.
    SourceLoc StartLoc = SS->getStartLoc();
    SourceLoc EndLoc = SS->getSubjectExpr()->getEndLoc();

    for (CaseStmt *CS : SS->getCases()) {
      if (Stmt *S = CS->getBody()) {
        if (auto *B = dyn_cast<BraceStmt>(S)) {
          BraceStmt *NB = transformBraceStmt(B);

          // Lets insert a before and after log pointing at the case statement
          // at the start of the body (just like in for loops.
          BraceStmt *NNB = nullptr;
          SourceRange CaseRange = CS->getLabelItemsRange();
          if (CaseRange.isValid()) {
            NNB = prependLoggerCall(NB, CaseRange);
          } else {
            NNB = NB;
          }

          // Now we prepend the switch log, so that it looks like switch came
          // before case
          BraceStmt *NNNB = nullptr;
          if (StartLoc.isValid() && EndLoc.isValid()) {
            NNNB = prependLoggerCall(NNB, {StartLoc, EndLoc});
          } else {
            NNNB = NNB;
          }

          if (NNNB != B) {
            CS->setBody(NNNB);
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

  Decl *transformDecl(Decl *D) {
    if (D->isImplicit())
      return D;
    if (auto *FD = dyn_cast<FuncDecl>(D)) {
      if (BraceStmt *B = FD->getBody()) {
        const ParameterList *PL = FD->getParameters();
        BraceStmt *NB = transformBraceStmt(B, PL);
        // Since it would look strange going straight to the first line in a
        // function body, we throw in a before/after pointing at the function
        // decl at the start of the transformed body
        SourceLoc StartLoc = FD->getStartLoc();
        SourceLoc EndLoc = SourceLoc();
        if (FD->getResultTypeRepr()) {
          EndLoc = FD->getResultTypeSourceRange().End;
        } else {
          EndLoc = FD->getParameters()->getSourceRange().End;
        }

        if (EndLoc.isValid())
          NB = prependLoggerCall(NB, {StartLoc, EndLoc});

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

  BraceStmt *transformBraceStmt(BraceStmt *BS,
                                const ParameterList *PL = nullptr,
                                bool TopLevel = false) override {
    ArrayRef<ASTNode> OriginalElements = BS->getElements();
    SmallVector<swift::ASTNode, 3> Elements(OriginalElements.begin(),
                                            OriginalElements.end());

    for (size_t EI = 0; EI != Elements.size(); ++EI) {
      swift::ASTNode &Element = Elements[EI];
      if (auto *E = Element.dyn_cast<Expr *>()) {
        E->walk(CF);

        Added<Stmt *> LogBefore = buildLoggerCall(LogBeforeName, E->getSourceRange());
        Added<Stmt *> LogAfter = buildLoggerCall(LogAfterName, E->getSourceRange());

        if (*LogBefore && *LogAfter) {
          Elements[EI] = *LogBefore;
          Elements.insert(Elements.begin() + (EI + 1), E);
          Elements.insert(Elements.begin() + (EI + 2), *LogAfter);
          EI += 2;
        }
      } else if (auto *S = Element.dyn_cast<Stmt *>()) {
        S->walk(CF);
        if (auto *RS = dyn_cast<ReturnStmt>(S)) {
          if (RS->hasResult()) {
            std::pair<PatternBindingDecl *, VarDecl *> PV =
                buildPatternAndVariable(RS->getResult());
            SourceLoc ResultStartLoc = RS->getResult()->getStartLoc();
            DeclRefExpr *DRE = new (Context) DeclRefExpr(
                ConcreteDeclRef(PV.second),
                ResultStartLoc.isValid() ? DeclNameLoc(ResultStartLoc)
                                         : DeclNameLoc(),
                true, // implicit
                AccessSemantics::Ordinary, RS->getResult()->getType());
            ReturnStmt *NRS = new (Context) ReturnStmt(SourceLoc(), DRE,
                                                       true); // implicit
            Added<Stmt *> LogBefore =
                buildLoggerCall(LogBeforeName, RS->getSourceRange());
            Added<Stmt *> LogAfter =
                buildLoggerCall(LogAfterName, RS->getSourceRange());
            if (*LogBefore && *LogAfter) {
              Elements[EI] = *LogBefore;
              Elements.insert(Elements.begin() + (EI + 1), PV.first);
              Elements.insert(Elements.begin() + (EI + 2), PV.second);
              Elements.insert(Elements.begin() + (EI + 3), *LogAfter);
              Elements.insert(Elements.begin() + (EI + 4), NRS);
              EI += 4;
            }
          } else {
            Added<Stmt *> LogBefore =
                buildLoggerCall(LogBeforeName, RS->getSourceRange());
            Added<Stmt *> LogAfter =
                buildLoggerCall(LogAfterName, RS->getSourceRange());
            if (*LogBefore && *LogAfter) {
              Elements[EI] = *LogBefore;
              Elements.insert(Elements.begin() + (EI + 1), *LogAfter);
              Elements.insert(Elements.begin() + (EI + 2), RS);
              EI += 2;
            }
          }
        } else if (auto *CS = dyn_cast<ContinueStmt>(S)) {
          Added<Stmt *> LogBefore = buildLoggerCall(LogBeforeName, CS->getSourceRange());
          Added<Stmt *> LogAfter = buildLoggerCall(LogAfterName, CS->getSourceRange());
          if (*LogBefore && *LogAfter) {
            Elements[EI] = *LogBefore;
            Elements.insert(Elements.begin() + (EI + 1), *LogAfter);
            Elements.insert(Elements.begin() + (EI + 2), CS);
            EI += 2;
          }

        } else if (auto *BS = dyn_cast<BreakStmt>(S)) {
          Added<Stmt *> LogBefore = buildLoggerCall(LogBeforeName, BS->getSourceRange());
          Added<Stmt *> LogAfter = buildLoggerCall(LogAfterName, BS->getSourceRange());
          if (*LogBefore && *LogAfter) {
            Elements[EI] = *LogBefore;
            Elements.insert(Elements.begin() + (EI + 1), *LogAfter);
            Elements.insert(Elements.begin() + (EI + 2), BS);
            EI += 2;
          }

        } else if (auto *FS = dyn_cast<FallthroughStmt>(S)) {
          Added<Stmt *> LogBefore = buildLoggerCall(LogBeforeName, FS->getSourceRange());
          Added<Stmt *> LogAfter = buildLoggerCall(LogAfterName, FS->getSourceRange());
          if (*LogBefore && *LogAfter) {
            Elements[EI] = *LogBefore;
            Elements.insert(Elements.begin() + (EI + 1), *LogAfter);
            Elements.insert(Elements.begin() + (EI + 2), FS);
            EI += 2;
          }

        } else {
          Stmt *NS = transformStmt(S);
          if (NS != S) {
            Elements[EI] = NS;
          }
        }
      } else if (auto *D = Element.dyn_cast<Decl *>()) {
        D->walk(CF);
        if (auto *PBD = dyn_cast<PatternBindingDecl>(D)) {
          // FIXME: Should iterate all var decls
          if (VarDecl *VD = PBD->getSingleVar()) {
            if (VD->getParentExecutableInitializer()) {

              SourceRange SR = PBD->getSourceRange();
              if (!SR.isValid()) {
                SR = PBD->getOriginalInitRange(0);
              }

              Added<Stmt *> LogBefore = buildLoggerCall(LogBeforeName, SR);
              Added<Stmt *> LogAfter = buildLoggerCall(LogAfterName, SR);

              if (*LogBefore && *LogAfter) {
                Elements[EI] = *LogBefore;
                Elements.insert(Elements.begin() + (EI + 1), D);
                Elements.insert(Elements.begin() + (EI + 2), *LogAfter);
                EI += 2;
              }
            }
          }
        } else {
          transformDecl(D);
        }
      }
    }

    return swift::BraceStmt::create(Context, BS->getLBraceLoc(), Elements,
                                    BS->getRBraceLoc());
  }

  std::pair<PatternBindingDecl *, VarDecl *>
  buildPatternAndVariable(Expr *InitExpr) {
    SmallString<16> NameBuf;
    (Twine("pctmp") + Twine(TmpNameIndex)).toVector(NameBuf);
    ++TmpNameIndex;

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

    NamedPattern *NP = NamedPattern::createImplicit(Context, VD, VD->getType());
    PatternBindingDecl *PBD = PatternBindingDecl::createImplicit(
        Context, StaticSpellingKind::None, NP, MaybeLoadInitExpr, TypeCheckDC);

    return std::make_pair(PBD, VD);
  }

  // Puts a pair of before/after calls at the start of the body, pointing at
  // that range.
  BraceStmt *prependLoggerCall(BraceStmt *BS, SourceRange SR) {
    Added<Stmt *> Before = buildLoggerCall(LogBeforeName, SR);
    Added<Stmt *> After = buildLoggerCall(LogAfterName, SR);

    ArrayRef<ASTNode> OriginalElements = BS->getElements();
    SmallVector<swift::ASTNode, 3> Elements(OriginalElements.begin(),
                                            OriginalElements.end());

    Elements.insert(Elements.begin(), {*Before, *After});
    return swift::BraceStmt::create(Context, BS->getLBraceLoc(), Elements,
                                    BS->getRBraceLoc());
  }

  // Takes an existing Expr and builds an expr that calls before, stores the
  // return value of the expr, calls after, then returns that return value.
  Expr *buildInlineLoggerCall(SourceRange SR, Expr *E) {

    if (!SR.isValid()) {
      return E;
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

    Expr *LoggerArgs[] = {
      StartLine, EndLine, StartColumn, EndColumn, ModuleExpr, FileExpr
    };

    UnresolvedDeclRefExpr *BeforeLoggerRef = new (Context)
        UnresolvedDeclRefExpr(LogBeforeName,
                              DeclRefKind::Ordinary, DeclNameLoc(SR.End));
    BeforeLoggerRef->setImplicit(true);

    ApplyExpr *BeforeLoggerCall = CallExpr::createImplicit(
        Context, BeforeLoggerRef,
        ArgumentList::forImplicitUnlabeled(Context, LoggerArgs));
    Added<ApplyExpr *> AddedBeforeLogger(BeforeLoggerCall);
    if (!doTypeCheck(Context, TypeCheckDC, AddedBeforeLogger)) {
      // typically due to 'cannot find '__builtin_pc_before' in scope'
      return E; // return E, it will be used in recovering from TC failure
    }

    UnresolvedDeclRefExpr *AfterLoggerRef = new (Context)
        UnresolvedDeclRefExpr(LogAfterName,
                              DeclRefKind::Ordinary, DeclNameLoc(SR.End));
    AfterLoggerRef->setImplicit(true);
    ApplyExpr *AfterLoggerCall = CallExpr::createImplicit(
        Context, AfterLoggerRef,
        ArgumentList::forImplicitUnlabeled(Context, LoggerArgs));
    Added<ApplyExpr *> AddedAfterLogger(AfterLoggerCall);
    if (!doTypeCheck(Context, TypeCheckDC, AddedAfterLogger)) {
      // typically due to 'cannot find '__builtin_pc_after' in scope'
      return E; // return E, it will be used in recovering from TC failure
    }

    llvm::SmallVector<Expr *, 3> TupleArgs{};
    TupleArgs.append({*AddedBeforeLogger, E, *AddedAfterLogger});
    SmallVector<Identifier, 3> ThreeArgLabels(TupleArgs.size(), Identifier());
    TupleExpr *Tup =
        TupleExpr::createImplicit(Context, TupleArgs, ThreeArgLabels);
    SmallVector<TupleTypeElt, 3> TupleTypes{};
    TupleTypes.append({TupleTypeElt(TupleType::getEmpty(Context)),
                       TupleTypeElt(E->getType()),
                       TupleTypeElt(TupleType::getEmpty(Context))});
    Tup->setType(TupleType::get(TupleTypes, Context));
    TupleElementExpr *GetOne = new (Context)
        TupleElementExpr(Tup, SourceLoc(), 1, SourceLoc(), E->getType());

    GetOne->setImplicit(true);

    Added<Expr *> AddedGet(GetOne);

    return *AddedGet;
  }

  Added<Stmt *> buildLoggerCall(DeclNameRef LoggerName, SourceRange SR) {
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

    UnresolvedDeclRefExpr *LoggerRef = new (Context)
        UnresolvedDeclRefExpr(LoggerName,
                              DeclRefKind::Ordinary, DeclNameLoc(SR.End));

    LoggerRef->setImplicit(true);

    auto *ArgList = ArgumentList::forImplicitUnlabeled(Context, {
      StartLine, EndLine, StartColumn, EndColumn, ModuleExpr, FileExpr
    });
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

    ASTNode Elements[] = {*Apply};

    BraceStmt *BS =
        BraceStmt::create(Context, SourceLoc(), Elements, SourceLoc(), true);

    return BS;
  }
};

} // end anonymous namespace

void swift::performPCMacro(SourceFile &SF) {
  class ExpressionFinder : public ASTWalker {
  private:
    unsigned TmpNameIndex = 0;

  public:
    ExpressionFinder() = default;

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkAction walkToDeclPre(Decl *D) override {
      ASTContext &ctx = D->getASTContext();
      if (auto *FD = dyn_cast<AbstractFunctionDecl>(D)) {
        if (!FD->isImplicit()) {
          if (FD->getBody()) {
            Instrumenter I(ctx, FD, TmpNameIndex);
            I.transformDecl(FD);
            return Action::SkipChildren();
          }
        }
      } else if (auto *TLCD = dyn_cast<TopLevelCodeDecl>(D)) {
        if (!TLCD->isImplicit()) {
          if (BraceStmt *Body = TLCD->getBody()) {
            Instrumenter I(ctx, TLCD, TmpNameIndex);
            BraceStmt *NewBody = I.transformBraceStmt(Body, nullptr, true);
            if (NewBody != Body) {
              TLCD->setBody(NewBody);
              TypeChecker::checkTopLevelEffects(TLCD);
              TypeChecker::contextualizeTopLevelCode(TLCD);
            }
            return Action::SkipChildren();
          }
        }
      }
      return Action::Continue();
    }
  };

  ExpressionFinder EF;
  for (Decl *D : SF.getTopLevelDecls()) {
    D->walk(EF);
  }
}
