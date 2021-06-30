//===--- TypeCheckStmt.cpp - Type Checking for Statements -----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements semantic analysis for statements.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "TypeCheckAvailability.h"
#include "TypeCheckConcurrency.h"
#include "TypeCheckType.h"
#include "MiscDiagnostics.h"
#include "swift/Subsystems.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTScope.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/DiagnosticSuppression.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/TopCollection.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/LocalContext.h"
#include "swift/Parse/Parser.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Syntax/TokenKinds.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Timer.h"
#include <iterator>

using namespace swift;

#define DEBUG_TYPE "TypeCheckStmt"

namespace {
  class ContextualizeClosures : public ASTWalker {
    DeclContext *ParentDC;
  public:
    unsigned NextDiscriminator = 0;

    ContextualizeClosures(DeclContext *parent,
                          unsigned nextDiscriminator = 0)
      : ParentDC(parent), NextDiscriminator(nextDiscriminator) {}

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      // Autoclosures need to be numbered and potentially reparented.
      // Reparenting is required with:
      //   - nested autoclosures, because the inner autoclosure will be
      //     parented to the outer context, not the outer autoclosure
      //   - non-local initializers
      if (auto CE = dyn_cast<AutoClosureExpr>(E)) {
        // FIXME: Work around an apparent reentrancy problem with the REPL.
        // I don't understand what's going on here well enough to fix the
        // underlying issue. -Joe
        if (CE->getParent() == ParentDC
            && CE->getDiscriminator() != AutoClosureExpr::InvalidDiscriminator)
          return { false, E };
        
        assert(CE->getDiscriminator() == AutoClosureExpr::InvalidDiscriminator);
        CE->setDiscriminator(NextDiscriminator++);
        CE->setParent(ParentDC);

        // Recurse into the autoclosure body using the same sequence,
        // but parenting to the autoclosure instead of the outer closure.
        auto oldParentDC = ParentDC;
        ParentDC = CE;
        CE->getBody()->walk(*this);
        ParentDC = oldParentDC;

        TypeChecker::computeCaptures(CE);
        return { false, E };
      } 

      // Capture lists need to be reparented to enclosing autoclosures.
      if (auto CapE = dyn_cast<CaptureListExpr>(E)) {
        if (isa<AutoClosureExpr>(ParentDC)) {
          for (auto &Cap : CapE->getCaptureList()) {
            Cap.PBD->setDeclContext(ParentDC);
            Cap.getVar()->setDeclContext(ParentDC);
          }
        }
      }

      // Explicit closures start their own sequence.
      if (auto CE = dyn_cast<ClosureExpr>(E)) {
        CE->setParent(ParentDC);

        // If the closure was type checked within its enclosing context,
        // we need to walk into it with a new sequence.
        // Otherwise, it'll have been separately type-checked.
        if (!CE->isSeparatelyTypeChecked())
          CE->getBody()->walk(ContextualizeClosures(CE));

        TypeChecker::computeCaptures(CE);
        return { false, E };
      }

      // Caller-side default arguments need their @autoclosures checked.
      if (auto *DAE = dyn_cast<DefaultArgumentExpr>(E))
        if (DAE->isCallerSide() && DAE->getParamDecl()->isAutoClosure())
          DAE->getCallerSideDefaultExpr()->walk(*this);

      return { true, E };
    }

    /// We don't want to recurse into most local declarations.
    bool walkToDeclPre(Decl *D) override {
      // But we do want to walk into the initializers of local
      // variables.
      return isa<PatternBindingDecl>(D);
    }
  };

  static DeclName getDescriptiveName(AbstractFunctionDecl *AFD) {
    DeclName name = AFD->getName();
    if (!name) {
      if (auto *accessor = dyn_cast<AccessorDecl>(AFD)) {
        name = accessor->getStorage()->getName();
      }
    }
    return name;
  }

  /// Used for debugging which parts of the code are taking a long time to
  /// compile.
  class FunctionBodyTimer {
    AnyFunctionRef Function;
    llvm::TimeRecord StartTime = llvm::TimeRecord::getCurrentTime();

  public:
    FunctionBodyTimer(AnyFunctionRef Fn) : Function(Fn) {}

    ~FunctionBodyTimer() {
      llvm::TimeRecord endTime = llvm::TimeRecord::getCurrentTime(false);

      auto elapsed = endTime.getProcessTime() - StartTime.getProcessTime();
      unsigned elapsedMS = static_cast<unsigned>(elapsed * 1000);

      ASTContext &ctx = Function.getAsDeclContext()->getASTContext();
      auto *AFD = Function.getAbstractFunctionDecl();

      if (ctx.TypeCheckerOpts.DebugTimeFunctionBodies) {
        // Round up to the nearest 100th of a millisecond.
        llvm::errs() << llvm::format("%0.2f", ceil(elapsed * 100000) / 100) << "ms\t";
        Function.getLoc().print(llvm::errs(), ctx.SourceMgr);

        if (AFD) {
          llvm::errs()
            << "\t" << Decl::getDescriptiveKindName(AFD->getDescriptiveKind())
            << " " << getDescriptiveName(AFD);
        } else {
          llvm::errs() << "\t(closure)";
        }
        llvm::errs() << "\n";
      }

      const auto WarnLimit = ctx.TypeCheckerOpts.WarnLongFunctionBodies;
      if (WarnLimit != 0 && elapsedMS >= WarnLimit) {
        if (AFD) {
          ctx.Diags.diagnose(AFD, diag::debug_long_function_body,
                             AFD->getDescriptiveKind(), getDescriptiveName(AFD),
                             elapsedMS, WarnLimit);
        } else {
          ctx.Diags.diagnose(Function.getLoc(), diag::debug_long_closure_body,
                             elapsedMS, WarnLimit);
        }
      }
    }
  };
} // end anonymous namespace

void TypeChecker::contextualizeInitializer(Initializer *DC, Expr *E) {
  ContextualizeClosures CC(DC);
  E->walk(CC);
}

void TypeChecker::contextualizeTopLevelCode(TopLevelCodeDecl *TLCD) {
  auto &Context = TLCD->DeclContext::getASTContext();
  unsigned nextDiscriminator = Context.NextAutoClosureDiscriminator;
  ContextualizeClosures CC(TLCD, nextDiscriminator);
  TLCD->getBody()->walk(CC);
  assert(nextDiscriminator == Context.NextAutoClosureDiscriminator &&
         "reentrant/concurrent invocation of contextualizeTopLevelCode?");
  Context.NextAutoClosureDiscriminator = CC.NextDiscriminator;
}

/// Emits an error with a fixit for the case of unnecessary cast over a
/// OptionSet value. The primary motivation is to help with SDK changes.
/// Example:
/// \code
///   func supported() -> MyMask {
///     return Int(MyMask.Bingo.rawValue)
///   }
/// \endcode
static void tryDiagnoseUnnecessaryCastOverOptionSet(ASTContext &Ctx,
                                                    Expr *E,
                                                    Type ResultType,
                                                    ModuleDecl *module) {
  auto *NTD = ResultType->getAnyNominal();
  if (!NTD)
    return;
  auto optionSetType = dyn_cast_or_null<ProtocolDecl>(Ctx.getOptionSetDecl());
  if (!optionSetType)
    return;
  SmallVector<ProtocolConformance *, 4> conformances;
  if (!(optionSetType &&
        NTD->lookupConformance(module, optionSetType, conformances)))
    return;

  auto *CE = dyn_cast<CallExpr>(E);
  if (!CE)
    return;
  if (!isa<ConstructorRefCallExpr>(CE->getFn()))
    return;
  auto *unaryArg = CE->getArgs()->getUnaryArgExpr();
  if (!unaryArg)
    return;
  auto *ME = dyn_cast<MemberRefExpr>(unaryArg);
  if (!ME)
    return;
  ValueDecl *VD = ME->getMember().getDecl();
  if (!VD || VD->getBaseName() != Ctx.Id_rawValue)
    return;
  auto *BME = dyn_cast<MemberRefExpr>(ME->getBase());
  if (!BME)
    return;
  if (!BME->getType()->isEqual(ResultType))
    return;

  Ctx.Diags.diagnose(E->getLoc(), diag::unnecessary_cast_over_optionset,
                     ResultType)
    .highlight(E->getSourceRange())
    .fixItRemoveChars(E->getLoc(), ME->getStartLoc())
    .fixItRemove(SourceRange(ME->getDotLoc(), E->getEndLoc()));
}

/// Whether the given enclosing context is a "defer" body.
static bool isDefer(DeclContext *dc) {
  if (auto *func = dyn_cast<FuncDecl>(dc))
    return func->isDeferBody();

  return false;
}

/// Check that a labeled statement doesn't shadow another statement with the
/// same label.
static void checkLabeledStmtShadowing(
    ASTContext &ctx, SourceFile *sourceFile, LabeledStmt *ls) {
  auto name = ls->getLabelInfo().Name;
  if (name.empty() || !sourceFile || ls->getStartLoc().isInvalid())
    return;

  auto activeLabeledStmtsVec = ASTScope::lookupLabeledStmts(
      sourceFile, ls->getStartLoc());
  auto activeLabeledStmts = llvm::makeArrayRef(activeLabeledStmtsVec);
  for (auto prevLS : activeLabeledStmts.slice(1)) {
    if (prevLS->getLabelInfo().Name == name) {
      ctx.Diags.diagnose(
          ls->getLabelInfo().Loc, diag::label_shadowed, name);
      ctx.Diags.diagnose(
          prevLS->getLabelInfo().Loc, diag::invalid_redecl_prev, name);
    }
  }
}

static void
emitUnresolvedLabelDiagnostics(DiagnosticEngine &DE,
                               SourceLoc targetLoc, Identifier targetName,
                               TopCollection<unsigned, LabeledStmt *> corrections) {
  // If an unresolved label was used, but we have a single correction,
  // produce the specific diagnostic and fixit.
  if (corrections.size() == 1) {
    DE.diagnose(targetLoc, diag::unresolved_label_corrected,
                targetName, corrections.begin()->Value->getLabelInfo().Name)
      .highlight(SourceRange(targetLoc))
      .fixItReplace(SourceRange(targetLoc),
                    corrections.begin()->Value->getLabelInfo().Name.str());
    DE.diagnose(corrections.begin()->Value->getLabelInfo().Loc,
                diag::decl_declared_here,
                corrections.begin()->Value->getLabelInfo().Name);
  } else {
    // If we have multiple corrections or none, produce a generic diagnostic
    // and all corrections available.
    DE.diagnose(targetLoc, diag::unresolved_label, targetName)
      .highlight(SourceRange(targetLoc));
    for (auto &entry : corrections)
      DE.diagnose(entry.Value->getLabelInfo().Loc, diag::note_typo_candidate,
                  entry.Value->getLabelInfo().Name.str())
        .fixItReplace(SourceRange(targetLoc),
                      entry.Value->getLabelInfo().Name.str());
  }
}

/// Find the target of a break or continue statement without a label.
///
/// \returns the target, if one was found, or \c nullptr if no such target
/// exists.
static LabeledStmt *findUnlabeledBreakOrContinueStmtTarget(
    ASTContext &ctx, SourceFile *sourceFile, SourceLoc loc,
    bool isContinue, DeclContext *dc,
    ArrayRef<LabeledStmt *> activeLabeledStmts) {
  for (auto labeledStmt : activeLabeledStmts) {
    // 'break' with no label looks through non-loop structures
    // except 'switch'.
    // 'continue' ignores non-loop structures.
    if (!labeledStmt->requiresLabelOnJump() &&
        (!isContinue || labeledStmt->isPossibleContinueTarget())) {
      return labeledStmt;
    }
  }

  // If we're in a defer, produce a tailored diagnostic.
  if (isDefer(dc)) {
    ctx.Diags.diagnose(
        loc, diag::jump_out_of_defer, isContinue ? "continue": "break");
    return nullptr;
  }

  // If we're dealing with an unlabeled break inside of an 'if' or 'do'
  // statement, produce a more specific error.
  if (!isContinue &&
      llvm::any_of(activeLabeledStmts,
                   [&](Stmt *S) -> bool {
                     return isa<IfStmt>(S) || isa<DoStmt>(S);
                   })) {
    ctx.Diags.diagnose(
        loc, diag::unlabeled_break_outside_loop);
    return nullptr;
  }

  // Otherwise produce a generic error.
  ctx.Diags.diagnose(
      loc,
      isContinue ? diag::continue_outside_loop : diag::break_outside_loop);
  return nullptr;
}

/// Find the target of a break or continue statement.
///
/// \returns the target, if one was found, or \c nullptr if no such target
/// exists.
static LabeledStmt *findBreakOrContinueStmtTarget(
    ASTContext &ctx, SourceFile *sourceFile,
    SourceLoc loc, Identifier targetName, SourceLoc targetLoc,
    bool isContinue, DeclContext *dc) {

  // Retrieve the active set of labeled statements.
  SmallVector<LabeledStmt *, 4> activeLabeledStmts;
  activeLabeledStmts = ASTScope::lookupLabeledStmts(sourceFile, loc);

  // Handle an unlabeled break separately; that's the easy case.
  if (targetName.empty()) {
    return findUnlabeledBreakOrContinueStmtTarget(
        ctx, sourceFile, loc, isContinue, dc, activeLabeledStmts);
  }

  // Scan inside out until we find something with the right label.
  TopCollection<unsigned, LabeledStmt *> labelCorrections(3);
  for (auto labeledStmt : activeLabeledStmts) {
    if (targetName == labeledStmt->getLabelInfo().Name) {
      // Continue cannot be used to repeat switches, use fallthrough instead.
      if (isContinue && !labeledStmt->isPossibleContinueTarget()) {
        ctx.Diags.diagnose(
            loc, diag::continue_not_in_this_stmt,
            isa<SwitchStmt>(labeledStmt) ? "switch" : "if");
        return nullptr;
      }

      return labeledStmt;
    }

    unsigned distance =
      TypeChecker::getCallEditDistance(
          DeclNameRef(targetName),
          labeledStmt->getLabelInfo().Name,
          TypeChecker::UnreasonableCallEditDistance);
    if (distance < TypeChecker::UnreasonableCallEditDistance)
      labelCorrections.insert(distance, std::move(labeledStmt));
  }
  labelCorrections.filterMaxScoreRange(
    TypeChecker::MaxCallEditDistanceFromBestCandidate);

  // If we're in a defer, produce a tailored diagnostic.
  if (isDefer(dc)) {
    ctx.Diags.diagnose(
        loc, diag::jump_out_of_defer, isContinue ? "continue": "break");
    return nullptr;
  }

  // Provide potential corrections for an incorrect label.
  emitUnresolvedLabelDiagnostics(
      ctx.Diags, targetLoc, targetName, labelCorrections);
  return nullptr;
}

bool TypeChecker::typeCheckStmtConditionElement(StmtConditionElement &elt,
                                                bool &isFalsable,
                                                DeclContext *dc) {
  auto &Context = dc->getASTContext();
  if (elt.getKind() == StmtConditionElement::CK_Availability) {
    isFalsable = true;

    // Reject inlinable code using availability macros.
    PoundAvailableInfo *info = elt.getAvailability();
    if (auto *decl = dc->getAsDecl()) {
      if (decl->getAttrs().hasAttribute<InlinableAttr>() ||
          decl->getAttrs().hasAttribute<AlwaysEmitIntoClientAttr>())
        for (auto queries : info->getQueries())
          if (auto availSpec =
                  dyn_cast<PlatformVersionConstraintAvailabilitySpec>(queries))
            if (availSpec->getMacroLoc().isValid()) {
              Context.Diags.diagnose(
                  availSpec->getMacroLoc(),
                  swift::diag::availability_macro_in_inlinable,
                  decl->getDescriptiveKind());
              break;
            }
    }

    return false;
  }

  if (auto E = elt.getBooleanOrNull()) {
    assert(!E->getType() && "the bool condition is already type checked");
    bool hadError = TypeChecker::typeCheckCondition(E, dc);
    elt.setBoolean(E);
    isFalsable = true;
    return hadError;
  }
  assert(elt.getKind() != StmtConditionElement::CK_Boolean);

  // This is cleanup goop run on the various paths where type checking of the
  // pattern binding fails.
  auto typeCheckPatternFailed = [&] {
    elt.getPattern()->setType(ErrorType::get(Context));
    elt.getInitializer()->setType(ErrorType::get(Context));

    elt.getPattern()->forEachVariable([&](VarDecl *var) {
      // Don't change the type of a variable that we've been able to
      // compute a type for.
      if (var->hasInterfaceType() && !var->isInvalid())
        return;
      var->setInvalid();
    });
  };

  // Resolve the pattern.
  assert(!elt.getPattern()->hasType() &&
         "the pattern binding condition is already type checked");
  auto *pattern = TypeChecker::resolvePattern(elt.getPattern(), dc,
                                              /*isStmtCondition*/ true);
  if (!pattern) {
    typeCheckPatternFailed();
    return true;
  }
  elt.setPattern(pattern);

  TypeChecker::diagnoseDuplicateBoundVars(pattern);

  // Check the pattern, it allows unspecified types because the pattern can
  // provide type information.
  auto contextualPattern = ContextualPattern::forRawPattern(pattern, dc);
  Type patternType = TypeChecker::typeCheckPattern(contextualPattern);
  if (patternType->hasError()) {
    typeCheckPatternFailed();
    return true;
  }

  // If the pattern didn't get a type, it's because we ran into some
  // unknown types along the way. We'll need to check the initializer.
  auto init = elt.getInitializer();
  bool hadError = TypeChecker::typeCheckBinding(pattern, init, dc, patternType);
  elt.setPattern(pattern);
  elt.setInitializer(init);
  isFalsable |= pattern->isRefutablePattern();
  return hadError;
}

/// Type check the given 'if', 'while', or 'guard' statement condition.
///
/// \param stmt The conditional statement to type-check, which will be modified
/// in place.
///
/// \returns true if an error occurred, false otherwise.
static bool typeCheckConditionForStatement(LabeledConditionalStmt *stmt,
                                           DeclContext *dc) {
  bool hadError = false;
  bool hadAnyFalsable = false;
  auto cond = stmt->getCond();
  for (auto &elt : cond) {
    hadError |=
        TypeChecker::typeCheckStmtConditionElement(elt, hadAnyFalsable, dc);
  }

  // If the binding is not refutable, and there *is* an else, reject it as
  // unreachable.
  if (!hadAnyFalsable && !hadError) {
    auto &diags = dc->getASTContext().Diags;
    Diag<> msg = diag::invalid_diagnostic;
    switch (stmt->getKind()) {
    case StmtKind::If:
      msg = diag::if_always_true;
      break;
    case StmtKind::While:
      msg = diag::while_always_true;
      break;
    case StmtKind::Guard:
      msg = diag::guard_always_succeeds;
      break;
    default:
      llvm_unreachable("unknown LabeledConditionalStmt kind");
    }
    diags.diagnose(cond[0].getStartLoc(), msg);
  }

  stmt->setCond(cond);
  return false;
}

/// Verify that the pattern bindings for the cases that we're falling through
/// from and to are equivalent.
static void checkFallthroughPatternBindingsAndTypes(
    ASTContext &ctx,
    CaseStmt *caseBlock, CaseStmt *previousBlock,
    FallthroughStmt *fallthrough) {
  auto firstPattern = caseBlock->getCaseLabelItems()[0].getPattern();
  SmallVector<VarDecl *, 4> vars;
  firstPattern->collectVariables(vars);

  // We know that the typechecker has already guaranteed that all of
  // the case label items in the fallthrough have the same var
  // decls. So if we match against the case body var decls,
  // transitively we will match all of the other case label items in
  // the fallthrough destination as well.
  auto previousVars = previousBlock->getCaseBodyVariablesOrEmptyArray();
  for (auto *expected : vars) {
    bool matched = false;
    if (!expected->hasName())
      continue;

    for (auto *previous : previousVars) {
      if (!previous->hasName() ||
          expected->getName() != previous->getName()) {
        continue;
      }

      if (!previous->getType()->isEqual(expected->getType())) {
        ctx.Diags.diagnose(
            previous->getLoc(), diag::type_mismatch_fallthrough_pattern_list,
            previous->getType(), expected->getType());
        previous->setInvalid();
        expected->setInvalid();
      }

      // Ok, we found our match. Make the previous fallthrough statement var
      // decl our parent var decl.
      expected->setParentVarDecl(previous);
      matched = true;
      break;
    }

    if (!matched) {
      ctx.Diags.diagnose(
          fallthrough->getLoc(), diag::fallthrough_into_case_with_var_binding,
          expected->getName());
    }
  }
}

/// Check the correctness of a 'fallthrough' statement.
///
/// \returns true if an error occurred.
static bool checkFallthroughStmt(DeclContext *dc, FallthroughStmt *stmt) {
  CaseStmt *fallthroughSource;
  CaseStmt *fallthroughDest;
  ASTContext &ctx = dc->getASTContext();
  auto sourceFile = dc->getParentSourceFile();
  std::tie(fallthroughSource, fallthroughDest) =
      ASTScope::lookupFallthroughSourceAndDest(sourceFile, stmt->getLoc());

  if (!fallthroughSource) {
    ctx.Diags.diagnose(stmt->getLoc(), diag::fallthrough_outside_switch);
    return true;
  }
  if (!fallthroughDest) {
    ctx.Diags.diagnose(stmt->getLoc(), diag::fallthrough_from_last_case);
    return true;
  }
  stmt->setFallthroughSource(fallthroughSource);
  stmt->setFallthroughDest(fallthroughDest);

  checkFallthroughPatternBindingsAndTypes(
      ctx, fallthroughDest, fallthroughSource, stmt);
  return false;
}

namespace {
class StmtChecker : public StmtVisitor<StmtChecker, Stmt*> {
public:
  ASTContext &Ctx;

  /// DC - This is the current DeclContext.
  DeclContext *DC;

  /// Skip type checking any elements inside 'BraceStmt', also this is
  /// propagated to ConstraintSystem.
  bool LeaveBraceStmtBodyUnchecked = false;

  ASTContext &getASTContext() const { return Ctx; };

  StmtChecker(DeclContext *DC) : Ctx(DC->getASTContext()), DC(DC) { }

  //===--------------------------------------------------------------------===//
  // Helper Functions.
  //===--------------------------------------------------------------------===//
  
  bool isInDefer() const {
    return isDefer(DC);
  }
  
  template<typename StmtTy>
  bool typeCheckStmt(StmtTy *&S) {
    FrontendStatsTracer StatsTracer(getASTContext().Stats,
                                    "typecheck-stmt", S);
    PrettyStackTraceStmt trace(getASTContext(), "type-checking", S);
    StmtTy *S2 = cast_or_null<StmtTy>(visit(S));
    if (S2 == nullptr)
      return true;
    S = S2;
    performStmtDiagnostics(S, DC);
    return false;
  }

  /// Type-check an entire function body.
  bool typeCheckBody(BraceStmt *&S) {
    bool HadError = typeCheckStmt(S);
    S->walk(ContextualizeClosures(DC));
    return HadError;
  }

  void typeCheckASTNode(ASTNode &node);
  
  //===--------------------------------------------------------------------===//
  // Visit Methods.
  //===--------------------------------------------------------------------===//

  Stmt *visitBraceStmt(BraceStmt *BS);

  Stmt *visitReturnStmt(ReturnStmt *RS) {
    auto TheFunc = AnyFunctionRef::fromDeclContext(DC);

    if (!TheFunc.hasValue()) {
      getASTContext().Diags.diagnose(RS->getReturnLoc(),
                                     diag::return_invalid_outside_func);
      return nullptr;
    }
    
    // If the return is in a defer, then it isn't valid either.
    if (isInDefer()) {
      getASTContext().Diags.diagnose(RS->getReturnLoc(),
                                     diag::jump_out_of_defer, "return");
      return nullptr;
    }

    Type ResultTy = TheFunc->getBodyResultType();
    if (!ResultTy || ResultTy->hasError())
      return nullptr;

    if (!RS->hasResult()) {
      if (!ResultTy->isVoid())
        getASTContext().Diags.diagnose(RS->getReturnLoc(),
                                       diag::return_expr_missing);
      return RS;
    }

    // If the body consisted of a single return without a result
    // 
    //   func foo() -> Int {
    //     return
    //   }
    // 
    // in parseAbstractFunctionBody the return is given an empty, implicit tuple
    // as its result
    //
    //   func foo() -> Int {
    //     return ()
    //   }
    //
    // Look for that case and diagnose it as missing return expression.
    if (!ResultTy->isVoid() && TheFunc->hasSingleExpressionBody()) {
      auto expr = TheFunc->getSingleExpressionBody();
      if (expr->isImplicit() && isa<TupleExpr>(expr) &&
          cast<TupleExpr>(expr)->getNumElements() == 0) {
        getASTContext().Diags.diagnose(RS->getReturnLoc(),
                                       diag::return_expr_missing);
        return RS;
      }
    }

    Expr *E = RS->getResult();

    // In an initializer, the only expression allowed is "nil", which indicates
    // failure from a failable initializer.
    if (auto ctor = dyn_cast_or_null<ConstructorDecl>(
                                          TheFunc->getAbstractFunctionDecl())) {
      // The only valid return expression in an initializer is the literal
      // 'nil'.
      auto nilExpr = dyn_cast<NilLiteralExpr>(E->getSemanticsProvidingExpr());
      if (!nilExpr) {
        getASTContext().Diags.diagnose(RS->getReturnLoc(),
                                       diag::return_init_non_nil)
          .highlight(E->getSourceRange());
        RS->setResult(nullptr);
        return RS;
      }

      // "return nil" is only permitted in a failable initializer.
      if (!ctor->isFailable()) {
        getASTContext().Diags.diagnose(RS->getReturnLoc(),
                                       diag::return_non_failable_init)
          .highlight(E->getSourceRange());
        getASTContext().Diags.diagnose(ctor->getLoc(), diag::make_init_failable,
                    ctor->getName())
          .fixItInsertAfter(ctor->getLoc(), "?");
        RS->setResult(nullptr);
        return RS;
      }

      // Replace the "return nil" with a new 'fail' statement.
      return new (getASTContext()) FailStmt(RS->getReturnLoc(),
                                            nilExpr->getLoc(),
                                            RS->isImplicit());
    }
    
    TypeCheckExprOptions options = {};
    
    if (LeaveBraceStmtBodyUnchecked) {
      assert(DiagnosticSuppression::isEnabled(getASTContext().Diags) &&
             "Diagnosing and AllowUnresolvedTypeVariables don't seem to mix");
      options |= TypeCheckExprFlags::LeaveClosureBodyUnchecked;
    }

    ContextualTypePurpose ctp = CTP_ReturnStmt;
    if (auto func =
            dyn_cast_or_null<FuncDecl>(TheFunc->getAbstractFunctionDecl())) {
      if (func->hasSingleExpressionBody()) {
        ctp = CTP_ReturnSingleExpr;
      }
    }

    auto exprTy =
        TypeChecker::typeCheckExpression(E, DC, {ResultTy, ctp}, options);
    RS->setResult(E);

    if (!exprTy) {
      tryDiagnoseUnnecessaryCastOverOptionSet(getASTContext(), E, ResultTy,
                                              DC->getParentModule());
    }

    return RS;
  }

  Stmt *visitYieldStmt(YieldStmt *YS) {
    // If the yield is in a defer, then it isn't valid.
    if (isInDefer()) {
      getASTContext().Diags.diagnose(YS->getYieldLoc(),
                                     diag::jump_out_of_defer, "yield");
      return YS;
    }

    SmallVector<AnyFunctionType::Yield, 4> buffer;
    auto TheFunc = AnyFunctionRef::fromDeclContext(DC);
    auto yieldResults = TheFunc->getBodyYieldResults(buffer);

    auto yieldExprs = YS->getMutableYields();
    if (yieldExprs.size() != yieldResults.size()) {
      getASTContext().Diags.diagnose(YS->getYieldLoc(), diag::bad_yield_count,
                  yieldResults.size());
      return YS;
    }

    for (auto i : indices(yieldExprs)) {
      Type yieldType = yieldResults[i].getType();
      auto exprToCheck = yieldExprs[i];

      InOutExpr *inout = nullptr;

      // Classify whether we're yielding by reference or by value.
      ContextualTypePurpose contextTypePurpose;
      Type contextType = yieldType;
      if (yieldResults[i].isInOut()) {
        contextTypePurpose = CTP_YieldByReference;
        contextType = LValueType::get(contextType);

        // Check that the yielded expression is a &.
        if ((inout = dyn_cast<InOutExpr>(exprToCheck))) {
          // Strip the & off so that the constraint system doesn't complain
          // about the unparented &.
          exprToCheck = inout->getSubExpr();
        } else {
          getASTContext().Diags.diagnose(exprToCheck->getLoc(),
                      diag::missing_address_of_yield, yieldType)
            .highlight(exprToCheck->getSourceRange());
          inout = new (getASTContext()) InOutExpr(exprToCheck->getStartLoc(),
                                             exprToCheck,
                                             Type(), /*implicit*/ true);
        }
      } else {
        contextTypePurpose = CTP_YieldByValue;
      }

      TypeChecker::typeCheckExpression(exprToCheck, DC,
                                       {contextType, contextTypePurpose});

      // Propagate the change into the inout expression we stripped before.
      if (inout) {
        inout->setSubExpr(exprToCheck);
        inout->setType(InOutType::get(yieldType));
        exprToCheck = inout;
      }

      // Note that this modifies the statement's expression list in-place.
      yieldExprs[i] = exprToCheck;
    }
    return YS;
  }
  
  Stmt *visitThrowStmt(ThrowStmt *TS) {
    // Coerce the operand to the exception type.
    auto E = TS->getSubExpr();

    Type exnType = getASTContext().getErrorDecl()->getDeclaredInterfaceType();
    if (!exnType) return TS;

    TypeChecker::typeCheckExpression(E, DC, {exnType, CTP_ThrowStmt});
    TS->setSubExpr(E);

    return TS;
  }

  Stmt *visitPoundAssertStmt(PoundAssertStmt *PA) {
    Expr *C = PA->getCondition();
    TypeChecker::typeCheckCondition(C, DC);
    PA->setCondition(C);
    return PA;
  }
    
  Stmt *visitDeferStmt(DeferStmt *DS) {
    TypeChecker::typeCheckDecl(DS->getTempDecl());

    Expr *theCall = DS->getCallExpr();
    TypeChecker::typeCheckExpression(theCall, DC);
    DS->setCallExpr(theCall);

    return DS;
  }
  
  Stmt *visitIfStmt(IfStmt *IS) {
    typeCheckConditionForStatement(IS, DC);

    auto sourceFile = DC->getParentSourceFile();
    checkLabeledStmtShadowing(getASTContext(), sourceFile, IS);

    Stmt *S = IS->getThenStmt();
    typeCheckStmt(S);
    IS->setThenStmt(S);

    if ((S = IS->getElseStmt())) {
      typeCheckStmt(S);
      IS->setElseStmt(S);
    }
    
    return IS;
  }
  
  Stmt *visitGuardStmt(GuardStmt *GS) {
    typeCheckConditionForStatement(GS, DC);

    BraceStmt *S = GS->getBody();
    typeCheckStmt(S);
    GS->setBody(S);
    return GS;
  }

  Stmt *visitDoStmt(DoStmt *DS) {
    auto sourceFile = DC->getParentSourceFile();
    checkLabeledStmtShadowing(getASTContext(), sourceFile, DS);

    BraceStmt *S = DS->getBody();
    typeCheckStmt(S);
    DS->setBody(S);
    return DS;
  }
  
  Stmt *visitWhileStmt(WhileStmt *WS) {
    typeCheckConditionForStatement(WS, DC);

    auto sourceFile = DC->getParentSourceFile();
    checkLabeledStmtShadowing(getASTContext(), sourceFile, WS);

    Stmt *S = WS->getBody();
    typeCheckStmt(S);
    WS->setBody(S);
    
    return WS;
  }
  Stmt *visitRepeatWhileStmt(RepeatWhileStmt *RWS) {
    auto sourceFile = DC->getParentSourceFile();
    checkLabeledStmtShadowing(getASTContext(), sourceFile, RWS);

    Stmt *S = RWS->getBody();
    typeCheckStmt(S);
    RWS->setBody(S);

    Expr *E = RWS->getCond();
    TypeChecker::typeCheckCondition(E, DC);
    RWS->setCond(E);
    return RWS;
  }
  
  Stmt *visitForEachStmt(ForEachStmt *S) {
    if (TypeChecker::typeCheckForEachBinding(DC, S))
      return nullptr;

    TypeChecker::diagnoseDuplicateBoundVars(S->getPattern());

    // Type-check the body of the loop.
    auto sourceFile = DC->getParentSourceFile();
    checkLabeledStmtShadowing(getASTContext(), sourceFile, S);

    BraceStmt *Body = S->getBody();
    typeCheckStmt(Body);
    S->setBody(Body);
    
    return S;
  }

  Stmt *visitBreakStmt(BreakStmt *S) {
    if (auto target = findBreakOrContinueStmtTarget(
            getASTContext(), DC->getParentSourceFile(), S->getLoc(),
            S->getTargetName(), S->getTargetLoc(), /*isContinue=*/false,
            DC)) {
      S->setTarget(target);
    }

    return S;
  }

  Stmt *visitContinueStmt(ContinueStmt *S) {
    if (auto target = findBreakOrContinueStmtTarget(
            getASTContext(), DC->getParentSourceFile(), S->getLoc(),
            S->getTargetName(), S->getTargetLoc(), /*isContinue=*/true,
            DC)) {
      S->setTarget(target);
    }

    return S;
  }

  Stmt *visitFallthroughStmt(FallthroughStmt *S) {
    if (checkFallthroughStmt(DC, S))
      return nullptr;

    return S;
  }

  void checkCaseLabelItemPattern(CaseStmt *caseBlock, CaseLabelItem &labelItem,
                                 bool &limitExhaustivityChecks,
                                 Type subjectType) {
    Pattern *pattern = labelItem.getPattern();
    if (!labelItem.isPatternResolved()) {
      pattern = TypeChecker::resolvePattern(
          pattern, DC, /*isStmtCondition*/ false);
      if (!pattern) {
        return;
      }
    }

    // Coerce the pattern to the subject's type.
    bool coercionError = false;
    if (subjectType) {
      auto contextualPattern = ContextualPattern::forRawPattern(pattern, DC);
      TypeResolutionOptions patternOptions(TypeResolverContext::InExpression);
      auto coercedPattern = TypeChecker::coercePatternToType(
          contextualPattern, subjectType, patternOptions);
      if (coercedPattern)
        pattern = coercedPattern;
      else
        coercionError = true;
    }

    if (!subjectType || coercionError) {
      limitExhaustivityChecks = true;

      // If that failed, mark any variables binding pieces of the pattern
      // as invalid to silence follow-on errors.
      pattern->forEachVariable([&](VarDecl *VD) {
        VD->setInvalid();
      });
    }
    labelItem.setPattern(pattern, /*resolved=*/true);

    TypeChecker::diagnoseDuplicateBoundVars(pattern);

    // Otherwise for each variable in the pattern, make sure its type is
    // identical to the initial case decl and stash the previous case decl as
    // the parent of the decl.
    pattern->forEachVariable([&](VarDecl *vd) {
      if (!vd->hasName())
        return;

      // We know that prev var decls matches the initial var decl. So if we can
      // match prevVarDecls, we can also match initial var decl... So for each
      // decl in prevVarDecls...
      auto expected = vd->getParentVarDecl();
      if (!expected)
        return;

      // Then we check for errors.
      //
      // NOTE: We emit the diagnostics against the initial case label item var
      // decl associated with expected to ensure that we always emit
      // diagnostics against a single reference var decl. If we used expected
      // instead, we would emit confusing diagnostics since a correct var decl
      // after an incorrect var decl will be marked as incorrect. For instance
      // given the following case statement.
      //
      //   case .a(let x), .b(var x), .c(let x):
      //
      // if we use expected, we will emit errors saying that .b(var x) needs
      // to be a let and .c(let x) needs to be a var. Thus if one
      // automatically applied both fix-its, one would still get an error
      // producing program:
      //
      //   case .a(let x), .b(let x), .c(var x):
      //
      // More complex case label item lists could cause even longer fixup
      // sequences. Thus, we emit errors against the VarDecl associated with
      // expected in the initial case label item list.
      //
      // Luckily, it is easy for us to compute this since we only change the
      // parent field of the initial case label item's VarDecls /after/ we
      // finish updating the parent pointers of the VarDecls associated with
      // all other CaseLabelItems. So that initial group of VarDecls are
      // guaranteed to still have a parent pointer pointing at our
      // CaseStmt. Since we have setup the parent pointer VarDecl linked list
      // for all other CaseLabelItem var decls that we have already processed,
      // we can use our VarDecl linked list to find that initial case label
      // item VarDecl.
      auto *initialCaseVarDecl = expected;
      while (auto *prev = initialCaseVarDecl->getParentVarDecl()) {
        initialCaseVarDecl = prev;
      }
      assert(isa<CaseStmt>(initialCaseVarDecl->getParentPatternStmt()));

      if (!initialCaseVarDecl->isInvalid() &&
          !vd->getType()->isEqual(initialCaseVarDecl->getType())) {
        getASTContext().Diags.diagnose(
            vd->getLoc(), diag::type_mismatch_multiple_pattern_list,
            vd->getType(), initialCaseVarDecl->getType());
        vd->setInvalid();
        initialCaseVarDecl->setInvalid();
      }
    });
  }

  template <typename Iterator>
  void checkSiblingCaseStmts(Iterator casesBegin, Iterator casesEnd,
                             CaseParentKind parentKind,
                             bool &limitExhaustivityChecks, Type subjectType) {
    static_assert(
        std::is_same<typename std::iterator_traits<Iterator>::value_type,
                     CaseStmt *>::value,
        "Expected an iterator over CaseStmt *");

    // First pass: check all of the bindings.
    for (auto *caseBlock : make_range(casesBegin, casesEnd)) {
      // Bind all of the pattern variables together so we can follow the
      // "parent" pointers later on.
      bindSwitchCasePatternVars(DC, caseBlock);

      auto caseLabelItemArray = caseBlock->getMutableCaseLabelItems();
      for (auto &labelItem : caseLabelItemArray) {
        // Resolve the pattern in our case label if it has not been resolved and
        // check that our var decls follow invariants.
        checkCaseLabelItemPattern(caseBlock, labelItem, limitExhaustivityChecks,
                                  subjectType);

        // Check the guard expression, if present.
        if (auto *guard = labelItem.getGuardExpr()) {
          limitExhaustivityChecks |= TypeChecker::typeCheckCondition(guard, DC);
          labelItem.setGuardExpr(guard);
        }
      }

      // Setup the types of our case body var decls.
      for (auto *expected : caseBlock->getCaseBodyVariablesOrEmptyArray()) {
        assert(expected->hasName());
        auto prev = expected->getParentVarDecl();
        if (prev->hasInterfaceType())
          expected->setInterfaceType(prev->getInterfaceType());
      }
    }

    // Second pass: type-check the body statements.
    for (auto i = casesBegin; i != casesEnd; ++i) {
      auto *caseBlock = *i;

      // Check restrictions on '@unknown'.
      if (caseBlock->hasUnknownAttr()) {
        assert(parentKind == CaseParentKind::Switch &&
               "'@unknown' can only appear on switch cases");
        checkUnknownAttrRestrictions(
            getASTContext(), caseBlock, limitExhaustivityChecks);
      }

      BraceStmt *body = caseBlock->getBody();
      limitExhaustivityChecks |= typeCheckStmt(body);
      caseBlock->setBody(body);
    }
  }

  Stmt *visitSwitchStmt(SwitchStmt *switchStmt) {
    // Type-check the subject expression.
    Expr *subjectExpr = switchStmt->getSubjectExpr();
    auto resultTy = TypeChecker::typeCheckExpression(subjectExpr, DC);
    auto limitExhaustivityChecks = !resultTy;
    if (Expr *newSubjectExpr =
            TypeChecker::coerceToRValue(getASTContext(), subjectExpr))
      subjectExpr = newSubjectExpr;
    switchStmt->setSubjectExpr(subjectExpr);
    Type subjectType = switchStmt->getSubjectExpr()->getType();

    // Type-check the case blocks.
    auto sourceFile = DC->getParentSourceFile();
    checkLabeledStmtShadowing(getASTContext(), sourceFile, switchStmt);

    // Pre-emptively visit all Decls (#if/#warning/#error) that still exist in
    // the list of raw cases.
    for (auto &node : switchStmt->getRawCases()) {
      if (!node.is<Decl *>())
        continue;
      TypeChecker::typeCheckDecl(node.get<Decl *>());
    }

    auto cases = switchStmt->getCases();
    checkSiblingCaseStmts(cases.begin(), cases.end(), CaseParentKind::Switch,
                          limitExhaustivityChecks, subjectType);

    if (!switchStmt->isImplicit()) {
      TypeChecker::checkSwitchExhaustiveness(switchStmt, DC,
                                             limitExhaustivityChecks);
    }

    return switchStmt;
  }

  Stmt *visitCaseStmt(CaseStmt *S) {
    // Cases are handled in visitSwitchStmt.
    llvm_unreachable("case stmt outside of switch?!");
  }

  Stmt *visitDoCatchStmt(DoCatchStmt *S) {
    // The labels are in scope for both the 'do' and all of the catch
    // clauses.  This allows the user to break out of (or restart) the
    // entire construct.
    auto sourceFile = DC->getParentSourceFile();
    checkLabeledStmtShadowing(getASTContext(), sourceFile, S);

    // Type-check the 'do' body.  Type failures in here will generally
    // not cause type failures in the 'catch' clauses.
    Stmt *newBody = S->getBody();
    typeCheckStmt(newBody);
    S->setBody(newBody);

    // Do-catch statements always limit exhaustivity checks.
    bool limitExhaustivityChecks = true;

    auto catches = S->getCatches();
    checkSiblingCaseStmts(catches.begin(), catches.end(),
                          CaseParentKind::DoCatch, limitExhaustivityChecks,
                          getASTContext().getExceptionType());

    return S;
  }

  Stmt *visitFailStmt(FailStmt *S) {
    // These are created as part of type-checking "return" in an initializer.
    // There is nothing more to do.
    return S;
  }

};
} // end anonymous namespace

static bool isDiscardableType(Type type) {
  return (type->hasError() ||
          type->isUninhabited() ||
          type->lookThroughAllOptionalTypes()->isVoid());
}

static void diagnoseIgnoredLiteral(ASTContext &Ctx, LiteralExpr *LE) {
  const auto getLiteralDescription = [](LiteralExpr *LE) -> StringRef {
    switch (LE->getKind()) {
    case ExprKind::IntegerLiteral: return "integer";
    case ExprKind::FloatLiteral: return "floating-point";
    case ExprKind::BooleanLiteral: return "boolean";
    case ExprKind::StringLiteral: return "string";
    case ExprKind::InterpolatedStringLiteral: return "string";
    case ExprKind::MagicIdentifierLiteral:
      return MagicIdentifierLiteralExpr::getKindString(
          cast<MagicIdentifierLiteralExpr>(LE)->getKind());
    case ExprKind::NilLiteral: return "nil";
    case ExprKind::ObjectLiteral: return "object";

    // Define an unreachable case for all non-literal expressions.
    // This way, if a new literal is added in the future, the compiler
    // will warn that a case is missing from this switch.
#define LITERAL_EXPR(Id, Parent)
#define EXPR(Id, Parent) case ExprKind::Id:
#include "swift/AST/ExprNodes.def"
      llvm_unreachable("Not a literal expression");
    }

    llvm_unreachable("Unhandled ExprKind in switch.");
  };

  Ctx.Diags.diagnose(LE->getLoc(), diag::expression_unused_literal,
                     getLiteralDescription(LE))
    .highlight(LE->getSourceRange());
}

void TypeChecker::checkIgnoredExpr(Expr *E) {
  // Skip checking if there is no type, which presumably means there was a
  // type error.
  if (!E->getType()) {
    return;
  }

  // Complain about l-values that are neither loaded nor stored.
  auto &Context = E->getType()->getASTContext();
  auto &DE = Context.Diags;
  if (E->getType()->hasLValueType()) {
    // This must stay in sync with diag::expression_unused_lvalue.
    enum {
        SK_Variable = 0,
        SK_Property,
        SK_Subscript
    } storageKind = SK_Variable;
    if (auto declRef = E->getReferencedDecl()) {
      auto decl = declRef.getDecl();
      if (isa<SubscriptDecl>(decl))
        storageKind = SK_Subscript;
      else if (decl->getDeclContext()->isTypeContext())
        storageKind = SK_Property;
    }
    DE.diagnose(E->getLoc(), diag::expression_unused_lvalue, storageKind)
      .highlight(E->getSourceRange());
    return;
  }

  // Drill through noop expressions we don't care about.
  auto valueE = E;
  while (1) {
    valueE = valueE->getValueProvidingExpr();
    
    if (auto *OEE = dyn_cast<OpenExistentialExpr>(valueE))
      valueE = OEE->getSubExpr();
    else if (auto *CRCE = dyn_cast<CovariantReturnConversionExpr>(valueE))
      valueE = CRCE->getSubExpr();
    else if (auto *EE = dyn_cast<ErasureExpr>(valueE))
      valueE = EE->getSubExpr();
    else
      break;
  }
  
  // Complain about functions that aren't called.
  // TODO: What about tuples which contain functions by-value that are
  // dead?
  if (E->getType()->is<AnyFunctionType>()) {
    bool isDiscardable = false;

    // The called function could be wrapped inside a `dot_syntax_call_expr`
    // node, for example:
    //
    // class Bar {
    //   @discardableResult
    //   func foo() -> Int { return 0 }
    //
    //   func baz() {
    //     self.foo
    //     foo
    //   }
    // }
    //
    // So look through the DSCE and get the function being called.
    auto expr =
        isa<DotSyntaxCallExpr>(E) ? cast<DotSyntaxCallExpr>(E)->getFn() : E;

    if (auto *Fn = dyn_cast<ApplyExpr>(expr)) {
      if (auto *calledValue = Fn->getCalledValue()) {
        if (auto *FD = dyn_cast<AbstractFunctionDecl>(calledValue)) {
          if (FD->getAttrs().hasAttribute<DiscardableResultAttr>()) {
            isDiscardable = true;
          }
        }
      }
    }

    if (!isDiscardable) {
      DE.diagnose(E->getLoc(), diag::expression_unused_function)
          .highlight(E->getSourceRange());
      return;
    }
  }

  // If the result of this expression is of type "Never" or "()"
  // (the latter potentially wrapped in optionals) then it is
  // safe to ignore.
  if (isDiscardableType(valueE->getType()))
    return;
  
  // Complain about '#selector'.
  if (auto *ObjCSE = dyn_cast<ObjCSelectorExpr>(valueE)) {
    DE.diagnose(ObjCSE->getLoc(), diag::expression_unused_selector_result)
      .highlight(E->getSourceRange());
    return;
  }

  // Complain about '#keyPath'.
  if (isa<KeyPathExpr>(valueE)) {
    DE.diagnose(valueE->getLoc(), diag::expression_unused_keypath_result)
      .highlight(E->getSourceRange());
    return;
  }
    
  // Always complain about 'try?'.
  if (auto *OTE = dyn_cast<OptionalTryExpr>(valueE)) {
    DE.diagnose(OTE->getTryLoc(), diag::expression_unused_optional_try)
      .highlight(E->getSourceRange());
    return;
  }

  // If we have an OptionalEvaluationExpr at the top level, then someone is
  // "optional chaining" and ignoring the result.  Produce a diagnostic if it
  // doesn't make sense to ignore it.
  if (auto *OEE = dyn_cast<OptionalEvaluationExpr>(valueE)) {
    if (auto *IIO = dyn_cast<InjectIntoOptionalExpr>(OEE->getSubExpr()))
      return checkIgnoredExpr(IIO->getSubExpr());
    if (auto *C = dyn_cast<CallExpr>(OEE->getSubExpr()))
      return checkIgnoredExpr(C);
    if (auto *OE = dyn_cast<OpenExistentialExpr>(OEE->getSubExpr()))
      return checkIgnoredExpr(OE);
  }

  if (auto *LE = dyn_cast<LiteralExpr>(valueE)) {
    diagnoseIgnoredLiteral(Context, LE);
    return;
  }

  // Check if we have a call to a function not marked with
  // '@discardableResult'.
  if (auto call = dyn_cast<ApplyExpr>(valueE)) {
    // Dig through all levels of calls.
    Expr *fn = call->getFn();
    while (true) {
      fn = fn->getSemanticsProvidingExpr();
      if (auto applyFn = dyn_cast<ApplyExpr>(fn)) {
        fn = applyFn->getFn();
      } else if (auto FVE = dyn_cast<ForceValueExpr>(fn)) {
        fn = FVE->getSubExpr();
      } else if (auto dotSyntaxRef = dyn_cast<DotSyntaxBaseIgnoredExpr>(fn)) {
        fn = dotSyntaxRef->getRHS();
      } else {
        break;
      }
    }

    // Find the callee.
    AbstractFunctionDecl *callee = nullptr;
    if (auto declRef = dyn_cast<DeclRefExpr>(fn))
      callee = dyn_cast<AbstractFunctionDecl>(declRef->getDecl());
    else if (auto ctorRef = dyn_cast<OtherConstructorDeclRefExpr>(fn))
      callee = ctorRef->getDecl();
    else if (auto memberRef = dyn_cast<MemberRefExpr>(fn))
      callee = dyn_cast<AbstractFunctionDecl>(memberRef->getMember().getDecl());
    else if (auto dynMemberRef = dyn_cast<DynamicMemberRefExpr>(fn))
      callee = dyn_cast<AbstractFunctionDecl>(
                 dynMemberRef->getMember().getDecl());
    
    // If the callee explicitly allows its result to be ignored, then don't
    // complain.
    if (callee && callee->getAttrs().getAttribute<DiscardableResultAttr>())
      return;

    // Otherwise, complain.  Start with more specific diagnostics.

    // Diagnose unused literals that were translated to implicit
    // constructor calls during CSApply / ExprRewriter::convertLiteral.
    if (call->isImplicit()) {
      auto *args = call->getArgs();
      if (auto *unaryArg = args->getUnaryArgExpr()) {
        if (auto *LE = dyn_cast<LiteralExpr>(unaryArg)) {
          diagnoseIgnoredLiteral(Context, LE);
          return;
        }
      }
    }

    // Other unused constructor calls.
    if (callee && isa<ConstructorDecl>(callee) && !call->isImplicit()) {
      DE.diagnose(fn->getLoc(), diag::expression_unused_init_result,
               callee->getDeclContext()->getDeclaredInterfaceType())
        .highlight(call->getArgs()->getSourceRange());
      return;
    }
    
    SourceRange SR1 = call->getArgs()->getSourceRange(), SR2;
    if (auto *BO = dyn_cast<BinaryExpr>(call)) {
      SR1 = BO->getLHS()->getSourceRange();
      SR2 = BO->getRHS()->getSourceRange();
    }
    
    // Otherwise, produce a generic diagnostic.
    if (callee) {
      auto &ctx = callee->getASTContext();
      if (callee->isImplicit()) {
        // Translate calls to implicit functions to their user-facing names
        if (callee->getBaseName() == ctx.Id_derived_enum_equals ||
            callee->getBaseName() == ctx.Id_derived_struct_equals) {
          DE.diagnose(fn->getLoc(), diag::expression_unused_result_operator,
                   ctx.Id_EqualsOperator)
            .highlight(SR1).highlight(SR2);
          return;
        }
      }

      auto diagID = diag::expression_unused_result_call;
      if (callee->getName().isOperator())
        diagID = diag::expression_unused_result_operator;
      
      DE.diagnose(fn->getLoc(), diagID, callee->getName())
        .highlight(SR1).highlight(SR2);
    } else
      DE.diagnose(fn->getLoc(), diag::expression_unused_result_unknown,
               isa<ClosureExpr>(fn), valueE->getType())
        .highlight(SR1).highlight(SR2);

    return;
  }

  // Produce a generic diagnostic.
  DE.diagnose(valueE->getLoc(), diag::expression_unused_result,
              valueE->getType())
    .highlight(valueE->getSourceRange());
}

void StmtChecker::typeCheckASTNode(ASTNode &node) {
  // Type check the expression
  if (auto *E = node.dyn_cast<Expr *>()) {
    auto &ctx = DC->getASTContext();

    TypeCheckExprOptions options = TypeCheckExprFlags::IsExprStmt;
    bool isDiscarded =
        (!ctx.LangOpts.Playground && !ctx.LangOpts.DebuggerSupport);
    if (isDiscarded)
      options |= TypeCheckExprFlags::IsDiscarded;
    if (LeaveBraceStmtBodyUnchecked) {
      options |= TypeCheckExprFlags::LeaveClosureBodyUnchecked;
    }

    auto resultTy =
        TypeChecker::typeCheckExpression(E, DC, /*contextualInfo=*/{}, options);

    // If a closure expression is unused, the user might have intended to write
    // "do { ... }".
    auto *CE = dyn_cast<ClosureExpr>(E);
    if (CE || isa<CaptureListExpr>(E)) {
      ctx.Diags.diagnose(E->getLoc(), diag::expression_unused_closure);

      if (CE && CE->hasAnonymousClosureVars() &&
          CE->getParameters()->size() == 0) {
        ctx.Diags.diagnose(CE->getStartLoc(), diag::brace_stmt_suggest_do)
            .fixItInsert(CE->getStartLoc(), "do ");
      }
    } else if (isDiscarded && resultTy) {
      TypeChecker::checkIgnoredExpr(E);
    }

    node = E;
    return;
  }

  // Type check the statement.
  if (auto *S = node.dyn_cast<Stmt *>()) {
    typeCheckStmt(S);
    node = S;
    return;
  }

  // Type check the declaration.
  if (auto *D = node.dyn_cast<Decl *>()) {
    TypeChecker::typeCheckDecl(D);
    return;
  }

  llvm_unreachable("Type checking null ASTNode");
}

Stmt *StmtChecker::visitBraceStmt(BraceStmt *BS) {
  if (LeaveBraceStmtBodyUnchecked)
    return BS;

  // Diagnose defer statement being last one in block (only if
  // BraceStmt does not start a TopLevelDecl).
  if (!BS->empty()) {
    if (auto stmt =
            BS->getLastElement().dyn_cast<Stmt *>()) {
      if (auto deferStmt = dyn_cast<DeferStmt>(stmt)) {
        if (!isa<TopLevelCodeDecl>(DC) ||
            cast<TopLevelCodeDecl>(DC)->getBody() != BS) {
          getASTContext().Diags.diagnose(deferStmt->getStartLoc(),
                                         diag::defer_stmt_at_block_end)
              .fixItReplace(deferStmt->getStartLoc(), "do");
        }
      }
    }
  }

  for (auto &elem : BS->getElements())
    typeCheckASTNode(elem);

  return BS;
}

void TypeChecker::typeCheckASTNode(ASTNode &node, DeclContext *DC,
                                   bool LeaveBodyUnchecked) {
  StmtChecker stmtChecker(DC);
  // FIXME: 'ActiveLabeledStmts', etc. in StmtChecker are not
  // populated. Since they don't affect "type checking", it's doesn't cause
  // any issue for now. But it should be populated nonetheless.
  stmtChecker.LeaveBraceStmtBodyUnchecked = LeaveBodyUnchecked;
  stmtChecker.typeCheckASTNode(node);
}

static Type getResultBuilderType(FuncDecl *FD) {
  Type builderType = FD->getResultBuilderType();

  // For getters, fall back on looking on the attribute on the storage.
  if (!builderType) {
    auto accessor = dyn_cast<AccessorDecl>(FD);
    if (accessor && accessor->getAccessorKind() == AccessorKind::Get) {
      builderType = accessor->getStorage()->getResultBuilderType();
    }
  }

  return builderType;
}

/// Attempts to build an implicit call within the provided constructor
/// to the provided class's zero-argument super initializer.
/// @returns nullptr if there was an error and a diagnostic was emitted.
static Expr* constructCallToSuperInit(ConstructorDecl *ctor,
                                      ClassDecl *ClDecl) {
  ASTContext &Context = ctor->getASTContext();
  Expr *superRef = new (Context) SuperRefExpr(ctor->getImplicitSelfDecl(),
                                              SourceLoc(), /*Implicit=*/true);
  Expr *r = UnresolvedDotExpr::createImplicit(
      Context, superRef, DeclBaseName::createConstructor());
  r = CallExpr::createImplicitEmpty(Context, r);

  if (ctor->hasThrows())
    r = new (Context) TryExpr(SourceLoc(), r, Type(), /*implicit=*/true);

  DiagnosticSuppression suppression(ctor->getASTContext().Diags);
  auto resultTy = TypeChecker::typeCheckExpression(
      r, ctor, /*contextualInfo=*/{}, TypeCheckExprFlags::IsDiscarded);
  if (!resultTy)
    return nullptr;
  
  return r;
}

/// Check a super.init call.
///
/// \returns true if an error occurred.
static bool checkSuperInit(ConstructorDecl *fromCtor,
                           ApplyExpr *apply, bool implicitlyGenerated) {
  // Make sure we are referring to a designated initializer.
  auto otherCtorRef = dyn_cast<OtherConstructorDeclRefExpr>(
                        apply->getSemanticFn());
  if (!otherCtorRef)
    return false;
  
  auto ctor = otherCtorRef->getDecl();
  if (!ctor->isDesignatedInit()) {
    if (!implicitlyGenerated) {
      auto selfTy = fromCtor->getDeclContext()->getSelfInterfaceType();
      if (auto classTy = selfTy->getClassOrBoundGenericClass()) {
        assert(classTy->getSuperclass());
        auto &Diags = fromCtor->getASTContext().Diags;
        Diags.diagnose(apply->getArgs()->getLoc(), diag::chain_convenience_init,
                       classTy->getSuperclass());
        ctor->diagnose(diag::convenience_init_here);
      }
    }
    return true;
  }

  // For an implicitly generated super.init() call, make sure there's
  // only one designated initializer.
  if (implicitlyGenerated) {
    auto *dc = ctor->getDeclContext();
    auto *superclassDecl = dc->getSelfClassDecl();

    superclassDecl->synthesizeSemanticMembersIfNeeded(
        DeclBaseName::createConstructor());

    NLOptions subOptions = NL_QualifiedDefault;

    SmallVector<ValueDecl *, 4> lookupResults;
    fromCtor->lookupQualified(superclassDecl,
                              DeclNameRef::createConstructor(),
                              subOptions, lookupResults);

    for (auto decl : lookupResults) {
      auto superclassCtor = dyn_cast<ConstructorDecl>(decl);
      if (!superclassCtor || !superclassCtor->isDesignatedInit() ||
          superclassCtor == ctor)
        continue;

      // Found another designated initializer in the superclass. Don't add the
      // super.init() call.
      return true;
    }

    // Make sure we can reference the designated initializer correctly.
    auto loc = fromCtor->getLoc();
    const bool didDiagnose = diagnoseDeclAvailability(
        ctor, loc, nullptr,
        ExportContext::forFunctionBody(fromCtor, loc));
    if (didDiagnose) {
      fromCtor->diagnose(diag::availability_unavailable_implicit_init,
                         ctor->getDescriptiveKind(), ctor->getName(),
                         superclassDecl->getName());
    }

    // Not allowed to implicitly generate a super.init() call if the init
    // is async; that would hide the 'await' from the programmer.
    if (ctor->hasAsync()) {
      fromCtor->diagnose(diag::implicit_async_super_init);
      return true; // considered an error
    }
  }


  return false;
}

static bool isKnownEndOfConstructor(ASTNode N) {
  auto *S = N.dyn_cast<Stmt*>();
  if (!S) return false;

  return isa<ReturnStmt>(S) || isa<FailStmt>(S);
}

/// Check for problems specific to the body of a constructor within a
/// class, involving (e.g.) implicit calls to the superclass initializer and
/// issues related to designated/convenience initializers.
static void checkClassConstructorBody(ClassDecl *classDecl,
                                      ConstructorDecl *ctor,
                                      BraceStmt *body) {
  ASTContext &ctx = classDecl->getASTContext();
  bool wantSuperInitCall = false;
  bool isDelegating = false;
  auto initKindAndExpr = ctor->getDelegatingOrChainedInitKind();
  switch (initKindAndExpr.initKind) {
  case BodyInitKind::Delegating:
    isDelegating = true;
    wantSuperInitCall = false;
    break;

  case BodyInitKind::Chained:
    checkSuperInit(ctor, initKindAndExpr.initExpr, false);

    /// A convenience initializer cannot chain to a superclass constructor.
    if (ctor->isConvenienceInit()) {
      ctx.Diags.diagnose(initKindAndExpr.initExpr->getLoc(),
                         diag::delegating_convenience_super_init,
                         ctor->getDeclContext()->getDeclaredInterfaceType());
    }

    LLVM_FALLTHROUGH;

  case BodyInitKind::None:
    wantSuperInitCall = false;
    break;

  case BodyInitKind::ImplicitChained:
    wantSuperInitCall = true;
    break;
  }

  // A class designated initializer must never be delegating.
  if (ctor->isDesignatedInit() && isDelegating) {
    if (classDecl->getForeignClassKind() == ClassDecl::ForeignKind::CFType) {
      ctor->diagnose(diag::delegating_designated_init_in_extension,
                     ctor->getDeclContext()->getDeclaredInterfaceType());
    } else {
      ctor->diagnose(diag::delegating_designated_init,
                     ctor->getDeclContext()->getDeclaredInterfaceType())
          .fixItInsert(ctor->getLoc(), "convenience ");
    }

    ctx.Diags.diagnose(initKindAndExpr.initExpr->getLoc(), diag::delegation_here);
  }

  if (classDecl->isActor())
    checkActorConstructorBody(classDecl, ctor, body);

  // An inlinable constructor in a class must always be delegating,
  // unless the class is '@_fixed_layout'.
  // Note: This is specifically not using isFormallyResilient. We relax this
  // rule for classes in non-resilient modules so that they can have inlinable
  // constructors, as long as those constructors don't reference private
  // declarations.
  if (!isDelegating && classDecl->isResilient()) {
    auto kind = ctor->getFragileFunctionKind();
    if (kind.kind != FragileFunctionKind::None) {
      ctor->diagnose(diag::class_designated_init_inlinable_resilient,
                     classDecl->getDeclaredInterfaceType(),
                     static_cast<unsigned>(kind.kind));
    }
  }

  // If we don't want a super.init call, we're done.
  if (!wantSuperInitCall)
    return;

  // Find a default initializer in the superclass.
  Expr *SuperInitCall = constructCallToSuperInit(ctor, classDecl);
  if (!SuperInitCall)
    return;

  // If the initializer we found is a designated initializer, we're okay.
  class FindOtherConstructorRef : public ASTWalker {
  public:
    ApplyExpr *Found = nullptr;

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      if (auto apply = dyn_cast<ApplyExpr>(E)) {
        if (isa<OtherConstructorDeclRefExpr>(apply->getSemanticFn())) {
          Found = apply;
          return { false, E };
        }
      }

      return { Found == nullptr, E };
    }
  };

  FindOtherConstructorRef finder;
  SuperInitCall->walk(finder);
  if (!checkSuperInit(ctor, finder.Found, true)) {
    // Store the super.init expression within the constructor declaration
    // to be emitted during SILGen.
    ctor->setSuperInitCall(SuperInitCall);
  }
}

bool TypeCheckASTNodeAtLocRequest::evaluate(Evaluator &evaluator,
                                            DeclContext *DC,
                                            SourceLoc Loc) const {
  auto &ctx = DC->getASTContext();
  assert(DiagnosticSuppression::isEnabled(ctx.Diags) &&
         "Diagnosing and Single ASTNode type checknig don't mix");

  // Find innermost ASTNode at Loc from DC. Results the reference to the found
  // ASTNode and the decl context of it.
  class ASTNodeFinder : public ASTWalker {
    SourceManager &SM;
    SourceLoc Loc;
    ASTNode *FoundNode = nullptr;
    DeclContext *DC = nullptr;

  public:
    ASTNodeFinder(SourceManager &SM, SourceLoc Loc) : SM(SM), Loc(Loc) {}

    bool isNull() const { return !FoundNode; }
    ASTNode &getRef() const {
      assert(FoundNode);
      return *FoundNode;
    }
    DeclContext *getDeclContext() const { return DC; }

    std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
      if (auto *brace = dyn_cast<BraceStmt>(S)) {
        auto braceCharRange = Lexer::getCharSourceRangeFromSourceRange(
            SM, brace->getSourceRange());
        // Unless this brace contains the loc, there's nothing to do.
        if (!braceCharRange.contains(Loc))
          return {false, S};

        // Reset the node found in a parent context.
        if (!brace->isImplicit())
          FoundNode = nullptr;

        for (ASTNode &node : brace->getElements()) {
          if (SM.isBeforeInBuffer(Loc, node.getStartLoc()))
            break;

          // NOTE: We need to check the character loc here because the target
          // loc can be inside the last token of the node. i.e. interpolated
          // string.
          SourceLoc endLoc = Lexer::getLocForEndOfToken(SM, node.getEndLoc());
          if (SM.isBeforeInBuffer(endLoc, Loc) || endLoc == Loc)
            continue;

          // 'node' may be the target node, except 'CaseStmt' which cannot be
          // type checked alone.
          if (!node.isStmt(StmtKind::Case))
            FoundNode = &node;

          // Walk into the node to narrow down.
          node.walk(*this);

          break;
        }
        // Already walked into.
        return {false, nullptr};
      }

      return {true, S};
    }

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      if (SM.isBeforeInBuffer(Loc, E->getStartLoc()))
        return {false, E};

      SourceLoc endLoc = Lexer::getLocForEndOfToken(SM, E->getEndLoc());
      if (SM.isBeforeInBuffer(endLoc, Loc))
        return {false, E};

      // Don't walk into 'TapExpr'. They should be type checked with parent
      // 'InterpolatedStringLiteralExpr'.
      if (isa<TapExpr>(E))
        return {false, E};

      if (auto closure = dyn_cast<ClosureExpr>(E)) {
        // NOTE: When a client wants to type check a closure signature, it
        // requests with closure's 'getLoc()' location.
        if (Loc == closure->getLoc())
          return {false, E};

        DC = closure;
      }
      return {true, E};
    }

    bool walkToDeclPre(Decl *D) override {
      if (auto *newDC = dyn_cast<DeclContext>(D))
        DC = newDC;
      return true;
    }

  } finder(ctx.SourceMgr, Loc);
  DC->walkContext(finder);

  // Nothing found at the location, or the decl context does not own the 'Loc'.
  if (finder.isNull())
    return true;

  DC = finder.getDeclContext();

  if (auto *AFD = dyn_cast<AbstractFunctionDecl>(DC)) {
    if (AFD->isBodyTypeChecked())
      return false;

    ASTScope::expandFunctionBody(AFD);
  }

  // Function builder function doesn't support partial type checking.
  if (auto *func = dyn_cast<FuncDecl>(DC)) {
    if (Type builderType = getResultBuilderType(func)) {
      auto optBody =
          TypeChecker::applyResultBuilderBodyTransform(func, builderType);
      if (optBody && *optBody) {
        // Wire up the function body now.
        func->setBody(*optBody, AbstractFunctionDecl::BodyKind::TypeChecked);
        return false;
      }
      // FIXME: We failed to apply the result builder transform. Fall back to
      // just type checking the node that contains the code completion token.
      // This may be missing some context from the result builder but in
      // practice it often contains sufficient information to provide a decent
      // level of code completion that's better than providing nothing at all.
      // The proper solution would be to only partially type check the result
      // builder so that this fall back would not be necessary.
    } else if (func->hasSingleExpressionBody() &&
                func->getResultInterfaceType()->isVoid()) {
       // The function returns void.  We don't need an explicit return, no matter
       // what the type of the expression is.  Take the inserted return back out.
      func->getBody()->setLastElement(func->getSingleExpressionBody());
    }
  }

  // The enclosing closure might be a single expression closure or a function
  // builder closure. In such cases, the body elements are type checked with
  // the closure itself. So we need to try type checking the enclosing closure
  // signature first unless it has already been type checked.
  if (auto CE = dyn_cast<ClosureExpr>(DC)) {
    if (CE->getBodyState() == ClosureExpr::BodyState::Parsed) {
      swift::typeCheckASTNodeAtLoc(CE->getParent(), CE->getLoc());
      // We need the actor isolation of the closure to be set so that we can
      // annotate results that are on the same global actor.
      // Since we are evaluating TypeCheckASTNodeAtLocRequest for every closure
      // from outermost to innermost, we don't want to call checkActorIsolation,
      // because that would cause actor isolation to be checked multiple times
      // for nested closures. Instead, call determineClosureActorIsolation
      // directly and set the closure's actor isolation manually. We can
      // guarantee of that the actor isolation of enclosing closures have their
      // isolation checked before nested ones are being checked by the way
      // TypeCheckASTNodeAtLocRequest is called multiple times, as described
      // above.
      auto ActorIsolation = determineClosureActorIsolation(CE);
      CE->setActorIsolation(ActorIsolation);
      if (CE->getBodyState() != ClosureExpr::BodyState::ReadyForTypeChecking)
        return false;
    }
  }

  TypeChecker::typeCheckASTNode(finder.getRef(), DC,
                                /*LeaveBodyUnchecked=*/true);
  return false;
}

BraceStmt *
TypeCheckFunctionBodyRequest::evaluate(Evaluator &evaluator,
                                       AbstractFunctionDecl *AFD) const {
  ASTContext &ctx = AFD->getASTContext();

  Optional<FunctionBodyTimer> timer;
  const auto &tyOpts = ctx.TypeCheckerOpts;
  if (tyOpts.DebugTimeFunctionBodies || tyOpts.WarnLongFunctionBodies)
    timer.emplace(AFD);

  BraceStmt *body = AFD->getBody();
  assert(body && "Expected body to type-check");

  // It's possible we synthesized an already type-checked body, in which case
  // we're done.
  if (AFD->isBodyTypeChecked())
    return body;

  auto errorBody = [&]() {
    // If we had an error, return an ErrorExpr body instead of returning the
    // un-type-checked body.
    // FIXME: This should be handled by typeCheckExpression.
    auto range = AFD->getBodySourceRange();
    return BraceStmt::create(ctx, range.Start,
                             {new (ctx) ErrorExpr(range, ErrorType::get(ctx))},
                             range.End);
  };

  bool alreadyTypeChecked = false;
  if (auto *func = dyn_cast<FuncDecl>(AFD)) {
    if (Type builderType = getResultBuilderType(func)) {
      if (auto optBody =
              TypeChecker::applyResultBuilderBodyTransform(
                func, builderType)) {
        if (!*optBody)
          return errorBody();

        body = *optBody;
        alreadyTypeChecked = true;

        body->walk(ContextualizeClosures(AFD));
      }
    } else if (func->hasSingleExpressionBody() &&
               func->getResultInterfaceType()->isVoid()) {
      // The function returns void.  We don't need an explicit return, no matter
      // what the type of the expression is.  Take the inserted return back out.
      body->setLastElement(func->getSingleExpressionBody());
    }
  } else if (isa<ConstructorDecl>(AFD) &&
             (body->empty() ||
                !isKnownEndOfConstructor(body->getLastElement()))) {
    // For constructors, we make sure that the body ends with a "return" stmt,
    // which we either implicitly synthesize, or the user can write.  This
    // simplifies SILGen.
    SmallVector<ASTNode, 8> Elts(body->getElements().begin(),
                                 body->getElements().end());
    Elts.push_back(new (ctx) ReturnStmt(body->getRBraceLoc(),
                                        /*value*/nullptr,
                                        /*implicit*/true));
    body = BraceStmt::create(ctx, body->getLBraceLoc(), Elts,
                             body->getRBraceLoc(), body->isImplicit());
  }

  // Typechecking, in particular ApplySolution is going to replace closures
  // with OpaqueValueExprs and then try to do lookups into the closures.
  // So, build out the body now.
  ASTScope::expandFunctionBody(AFD);

  // Type check the function body if needed.
  bool hadError = false;
  if (!alreadyTypeChecked) {
    StmtChecker SC(AFD);
    hadError = SC.typeCheckBody(body);
  }

  // If this was a function with a single expression body, let's see
  // if implicit return statement came out to be `Never` which means
  // that we have eagerly converted something like `{ fatalError() }`
  // into `{ return fatalError() }` that has to be corrected here.
  if (isa<FuncDecl>(AFD) && cast<FuncDecl>(AFD)->hasSingleExpressionBody()) {
    if (auto *stmt = body->getLastElement().dyn_cast<Stmt *>()) {
      if (auto *retStmt = dyn_cast<ReturnStmt>(stmt)) {
        if (retStmt->isImplicit() && retStmt->hasResult()) {
          auto returnType = retStmt->getResult()->getType();
          if (returnType && returnType->isUninhabited())
            body->setLastElement(retStmt->getResult());
        }
      }
    }
  }

  // Class constructor checking.
  if (auto *ctor = dyn_cast<ConstructorDecl>(AFD)) {
    if (auto classDecl = ctor->getDeclContext()->getSelfClassDecl()) {
      checkActorConstructor(classDecl, ctor);
      checkClassConstructorBody(classDecl, ctor, body);
    }
  }

  // Temporarily wire up the function body for some extra checks.
  // FIXME: Eliminate this.
  AFD->setBody(body, AbstractFunctionDecl::BodyKind::TypeChecked);

  // If nothing went wrong yet, perform extra checking.
  if (!hadError)
    performAbstractFuncDeclDiagnostics(AFD);

  TypeChecker::computeCaptures(AFD);
  if (!AFD->getDeclContext()->isLocalContext()) {
    checkFunctionActorIsolation(AFD);
    checkFunctionAsyncUsage(AFD);
    TypeChecker::checkFunctionEffects(AFD);
  }

  return hadError ? errorBody() : body;
}

bool TypeChecker::typeCheckClosureBody(ClosureExpr *closure) {
  TypeChecker::checkClosureAttributes(closure);
  TypeChecker::checkParameterList(closure->getParameters(), closure);

  BraceStmt *body = closure->getBody();

  Optional<FunctionBodyTimer> timer;
  const auto &tyOpts = closure->getASTContext().TypeCheckerOpts;
  if (tyOpts.DebugTimeFunctionBodies || tyOpts.WarnLongFunctionBodies)
    timer.emplace(closure);

  bool HadError = StmtChecker(closure).typeCheckBody(body);
  if (body) {
    closure->setBody(body, closure->hasSingleExpressionBody());
  }
  closure->setBodyState(ClosureExpr::BodyState::SeparatelyTypeChecked);
  return HadError;
}

bool TypeChecker::typeCheckTapBody(TapExpr *expr, DeclContext *DC) {
  // We intentionally use typeCheckStmt instead of typeCheckBody here
  // because we want to contextualize TapExprs with the body they're in.
  BraceStmt *body = expr->getBody();
  bool HadError = StmtChecker(DC).typeCheckStmt(body);
  if (body) {
    expr->setBody(body);
  }
  return HadError;
}

void TypeChecker::typeCheckTopLevelCodeDecl(TopLevelCodeDecl *TLCD) {
  // We intentionally use typeCheckStmt instead of typeCheckBody here
  // because we want to contextualize all the TopLevelCode
  // declarations simultaneously.
  BraceStmt *Body = TLCD->getBody();
  StmtChecker(TLCD).typeCheckStmt(Body);
  TLCD->setBody(Body);
  checkTopLevelActorIsolation(TLCD);
  checkTopLevelEffects(TLCD);
  performTopLevelDeclDiagnostics(TLCD);
}

void swift::checkUnknownAttrRestrictions(
    ASTContext &ctx, CaseStmt *caseBlock,
    bool &limitExhaustivityChecks) {
  CaseStmt *fallthroughDest = caseBlock->findNextCaseStmt();
  if (caseBlock->getCaseLabelItems().size() != 1) {
    assert(!caseBlock->getCaseLabelItems().empty() &&
           "parser should not produce case blocks with no items");
    ctx.Diags.diagnose(caseBlock->getLoc(),
                       diag::unknown_case_multiple_patterns)
        .highlight(caseBlock->getCaseLabelItems()[1].getSourceRange());
    limitExhaustivityChecks = true;
  }

  if (fallthroughDest != nullptr) {
    if (!caseBlock->isDefault())
      ctx.Diags.diagnose(caseBlock->getLoc(),
                         diag::unknown_case_must_be_last);
    limitExhaustivityChecks = true;
  }

  const auto &labelItem = caseBlock->getCaseLabelItems().front();
  if (labelItem.getGuardExpr() && !labelItem.isDefault()) {
    ctx.Diags.diagnose(labelItem.getStartLoc(),
                                   diag::unknown_case_where_clause)
        .highlight(labelItem.getGuardExpr()->getSourceRange());
  }

  const Pattern *pattern =
      labelItem.getPattern()->getSemanticsProvidingPattern();
  if (!isa<AnyPattern>(pattern)) {
    ctx.Diags.diagnose(labelItem.getStartLoc(),
                       diag::unknown_case_must_be_catchall)
        .highlight(pattern->getSourceRange());
  }
}

void swift::bindSwitchCasePatternVars(DeclContext *dc, CaseStmt *caseStmt) {
  llvm::SmallDenseMap<Identifier, std::pair<VarDecl *, bool>, 4> latestVars;
  auto recordVar = [&](Pattern *pattern, VarDecl *var) {
    if (!var->hasName())
      return;

    // If there is an existing variable with this name, set it as the
    // parent of this new variable.
    auto &entry = latestVars[var->getName()];
    if (entry.first) {
      assert(!var->getParentVarDecl() ||
             var->getParentVarDecl() == entry.first);
      var->setParentVarDecl(entry.first);

      // Check for a mutability mismatch.
      if (pattern && entry.second != var->isLet()) {
        // Find the original declaration.
        auto initialCaseVarDecl = entry.first;
        while (auto parentVar = initialCaseVarDecl->getParentVarDecl())
          initialCaseVarDecl = parentVar;

        auto diag = var->diagnose(diag::mutability_mismatch_multiple_pattern_list,
                                  var->isLet(), initialCaseVarDecl->isLet());

        BindingPattern *foundVP = nullptr;
        pattern->forEachNode([&](Pattern *P) {
          if (auto *VP = dyn_cast<BindingPattern>(P))
            if (VP->getSingleVar() == var)
              foundVP = VP;
        });
        if (foundVP)
          diag.fixItReplace(foundVP->getLoc(),
                            initialCaseVarDecl->isLet() ? "let" : "var");

        var->setInvalid();
        initialCaseVarDecl->setInvalid();
      }
    } else {
      entry.second = var->isLet();
    }

    // Record this variable as the latest with this name.
    entry.first = var;
  };

  // Wire up the parent var decls for each variable that occurs within
  // the patterns of each case item. in source order.
  for (auto &caseItem : caseStmt->getMutableCaseLabelItems()) {
    // Resolve the pattern.
    auto *pattern = caseItem.getPattern();
    if (!caseItem.isPatternResolved()) {
      pattern = TypeChecker::resolvePattern(
          pattern, dc, /*isStmtCondition=*/false);
      if (!pattern)
        continue;
    }

    caseItem.setPattern(pattern, /*resolved=*/true);
    pattern->forEachVariable( [&](VarDecl *var) {
      recordVar(pattern, var);
    });
  }

  // Wire up the case body variables to the latest patterns.
  for (auto bodyVar : caseStmt->getCaseBodyVariablesOrEmptyArray()) {
    recordVar(nullptr, bodyVar);
  }
}
