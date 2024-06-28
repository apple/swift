//===----------------------------------------------------------------------===//
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

#include "AsyncRefactoring.h"
#include "Utils.h"
#include "swift/Basic/Assertions.h"

using namespace swift;
using namespace swift::refactoring::asyncrefactorings;

// The single Decl* subject of a switch statement, or nullptr if none
static Decl *singleSwitchSubject(const SwitchStmt *Switch) {
  if (auto *DRE = dyn_cast<DeclRefExpr>(Switch->getSubjectExpr()))
    return DRE->getDecl();
  return nullptr;
}

static FuncDecl *isOperator(const BinaryExpr *BE) {
  auto *AE = dyn_cast<ApplyExpr>(BE->getFn());
  if (AE) {
    auto *Callee = AE->getCalledValue();
    if (Callee && Callee->isOperator() && isa<FuncDecl>(Callee))
      return cast<FuncDecl>(Callee);
  }
  return nullptr;
}

static ConditionPath flippedConditionPath(ConditionPath Path) {
  switch (Path) {
  case ConditionPath::SUCCESS:
    return ConditionPath::FAILURE;
  case ConditionPath::FAILURE:
    return ConditionPath::SUCCESS;
  }
  llvm_unreachable("Unhandled case in switch!");
}

void CallbackClassifier::classifyInto(
    ClassifiedBlocks &Blocks, const ClosureCallbackParams &Params,
    llvm::DenseSet<SwitchStmt *> &HandledSwitches, DiagnosticEngine &DiagEngine,
    BraceStmt *Body) {
  assert(!Body->getElements().empty() && "Cannot classify empty body");
  CallbackClassifier Classifier(Blocks, Params, HandledSwitches, DiagEngine);
  Classifier.classifyNodes(Body->getElements(), Body->getRBraceLoc());
}

bool CallbackClassifier::tryClassifyNode(ASTNode Node) {
  auto *Statement = Node.dyn_cast<Stmt *>();
  if (!Statement)
    return false;

  if (auto *IS = dyn_cast<IfStmt>(Statement)) {
    NodesToPrint TempNodes;
    if (auto *BS = dyn_cast<BraceStmt>(IS->getThenStmt())) {
      TempNodes = NodesToPrint::inBraceStmt(BS);
    } else {
      TempNodes = NodesToPrint({IS->getThenStmt()}, /*commentLocs*/ {});
    }

    classifyConditional(IS, IS->getCond(), std::move(TempNodes),
                        IS->getElseStmt());
    return true;
  } else if (auto *GS = dyn_cast<GuardStmt>(Statement)) {
    classifyConditional(GS, GS->getCond(), NodesToPrint(), GS->getBody());
    return true;
  } else if (auto *SS = dyn_cast<SwitchStmt>(Statement)) {
    classifySwitch(SS);
    return true;
  } else if (auto *RS = dyn_cast<ReturnStmt>(Statement)) {
    // We can look through an implicit Void return of a SingleValueStmtExpr,
    // as that's semantically a statement.
    if (RS->hasResult() && RS->isImplicit()) {
      auto Ty = RS->getResult()->getType();
      if (Ty && Ty->isVoid()) {
        if (auto *SVE = dyn_cast<SingleValueStmtExpr>(RS->getResult()))
          return tryClassifyNode(SVE->getStmt());
      }
    }
  }
  return false;
}

bool CallbackClassifier::classifyNode(ASTNode Node) {
  auto DidClassify = tryClassifyNode(Node);
  if (!DidClassify)
    CurrentBlock->addNode(Node);
  return DiagEngine.hadAnyError();
}

void CallbackClassifier::classifyNodes(ArrayRef<ASTNode> Nodes,
                                       SourceLoc EndCommentLoc) {
  for (auto Node : Nodes) {
    auto HadError = classifyNode(Node);
    if (HadError)
      return;
  }
  // Make sure to pick up any trailing comments.
  CurrentBlock->addPossibleCommentLoc(EndCommentLoc);
}

bool CallbackClassifier::hasForceUnwrappedErrorParam(ArrayRef<ASTNode> Nodes) {
  auto *ErrParam = Params.getErrParam();
  if (!ErrParam)
    return false;

  class ErrUnwrapFinder : public ASTWalker {
    const ParamDecl *ErrParam;
    bool FoundUnwrap = false;

  public:
    explicit ErrUnwrapFinder(const ParamDecl *ErrParam) : ErrParam(ErrParam) {}
    bool foundUnwrap() const { return FoundUnwrap; }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      // Don't walk into ternary conditionals as they may have additional
      // conditions such as err != nil that make a force unwrap now valid.
      if (isa<TernaryExpr>(E))
        return Action::SkipNode(E);

      auto *FVE = dyn_cast<ForceValueExpr>(E);
      if (!FVE)
        return Action::Continue(E);

      auto *DRE = dyn_cast<DeclRefExpr>(FVE->getSubExpr());
      if (!DRE)
        return Action::Continue(E);

      if (DRE->getDecl() != ErrParam)
        return Action::Continue(E);

      // If we find the node we're looking for, make a note of it, and abort
      // the walk.
      FoundUnwrap = true;
      return Action::Stop();
    }

    PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
      // Don't walk into new explicit scopes, we only want to consider force
      // unwraps in the immediate conditional body.
      if (!S->isImplicit() && startsNewScope(S))
        return Action::SkipNode(S);
      return Action::Continue(S);
    }

    PreWalkAction walkToDeclPre(Decl *D) override {
      // Don't walk into new explicit DeclContexts.
      return Action::VisitNodeIf(D->isImplicit() || !isa<DeclContext>(D));
    }
  };
  for (auto Node : Nodes) {
    ErrUnwrapFinder walker(ErrParam);
    Node.walk(walker);
    if (walker.foundUnwrap())
      return true;
  }
  return false;
}

std::optional<ClassifiedCondition>
CallbackClassifier::classifyCallbackCondition(const CallbackCondition &Cond,
                                              const NodesToPrint &SuccessNodes,
                                              Stmt *ElseStmt) {
  if (!Cond.isValid())
    return std::nullopt;

  // If the condition involves a refutable pattern, we can't currently handle
  // it.
  if (Cond.BindPattern && Cond.BindPattern->isRefutablePattern())
    return std::nullopt;

  auto *SubjectParam = dyn_cast<ParamDecl>(Cond.Subject);
  if (!SubjectParam)
    return std::nullopt;

  // For certain types of condition, they need to be certain kinds of params.
  auto CondType = *Cond.Type;
  switch (CondType) {
  case ConditionType::NOT_NIL:
  case ConditionType::NIL:
    if (!Params.isUnwrappableParam(SubjectParam))
      return std::nullopt;
    break;
  case ConditionType::IS_TRUE:
  case ConditionType::IS_FALSE:
    if (!Params.isSuccessParam(SubjectParam))
      return std::nullopt;
    break;
  case ConditionType::SUCCESS_PATTERN:
  case ConditionType::FAILURE_PATTEN:
    if (SubjectParam != Params.getResultParam())
      return std::nullopt;
    break;
  }

  // Let's start with a success path, and flip any negative conditions.
  auto Path = ConditionPath::SUCCESS;

  // If it's an error param, that's a flip.
  if (SubjectParam == Params.getErrParam())
    Path = flippedConditionPath(Path);

  // If we have a nil, false, or failure condition, that's a flip.
  switch (CondType) {
  case ConditionType::NIL:
  case ConditionType::IS_FALSE:
  case ConditionType::FAILURE_PATTEN:
    Path = flippedConditionPath(Path);
    break;
  case ConditionType::IS_TRUE:
  case ConditionType::NOT_NIL:
  case ConditionType::SUCCESS_PATTERN:
    break;
  }

  // If we have a bool condition, it could be an Obj-C style flag check, which
  // we do some extra checking for. Otherwise, we're done.
  if (CondType != ConditionType::IS_TRUE &&
      CondType != ConditionType::IS_FALSE) {
    return ClassifiedCondition(Cond, Path, /*ObjCFlagCheck*/ false);
  }

  // Check to see if we have a known bool flag parameter that indicates
  // success or failure.
  if (auto KnownBoolFlag = Params.getKnownBoolFlagParam()) {
    if (KnownBoolFlag->Param != SubjectParam)
      return std::nullopt;

    // The path may need to be flipped depending on whether the flag indicates
    // success.
    if (!KnownBoolFlag->IsSuccessFlag)
      Path = flippedConditionPath(Path);

    return ClassifiedCondition(Cond, Path, /*ObjCStyleFlagCheck*/ true);
  }

  // If we've reached here, we have a bool flag check that isn't specified in
  // the async convention. We apply a heuristic to see if the error param is
  // force unwrapped in the conditional body. In that case, the user is
  // expecting it to be the error path, and it's more likely than not that the
  // flag value conveys no more useful information in the error block.

  // First check the success block.
  auto FoundInSuccessBlock =
      hasForceUnwrappedErrorParam(SuccessNodes.getNodes());

  // Then check the else block if we have it.
  if (ASTNode ElseNode = ElseStmt) {
    // Unwrap the BraceStmt of the else clause if needed. This is needed as
    // we won't walk into BraceStmts by default as they introduce new
    // scopes.
    ArrayRef<ASTNode> Nodes;
    if (auto *BS = dyn_cast<BraceStmt>(ElseStmt)) {
      Nodes = BS->getElements();
    } else {
      Nodes = llvm::ArrayRef(ElseNode);
    }
    if (hasForceUnwrappedErrorParam(Nodes)) {
      // If we also found an unwrap in the success block, we don't know what's
      // happening here.
      if (FoundInSuccessBlock)
        return std::nullopt;

      // Otherwise we can determine this as a success condition. Note this is
      // flipped as if the error is present in the else block, this condition
      // is for success.
      return ClassifiedCondition(Cond, ConditionPath::SUCCESS,
                                 /*ObjCStyleFlagCheck*/ true);
    }
  }

  if (FoundInSuccessBlock) {
    // Note that the path is flipped as if the error is present in the success
    // block, this condition is for failure.
    return ClassifiedCondition(Cond, ConditionPath::FAILURE,
                               /*ObjCStyleFlagCheck*/ true);
  }

  // Otherwise we can't classify this.
  return std::nullopt;
}

bool CallbackClassifier::classifyConditionsOf(
    StmtCondition Cond, const NodesToPrint &ThenNodesToPrint, Stmt *ElseStmt,
    ClassifiedCallbackConditions &Conditions) {
  bool UnhandledConditions = false;
  std::optional<ClassifiedCondition> ObjCFlagCheck;
  auto TryAddCond = [&](CallbackCondition CC) {
    auto Classified = classifyCallbackCondition(CC, ThenNodesToPrint, ElseStmt);

    // If we couldn't classify this, or if there are multiple Obj-C style flag
    // checks, this is unhandled.
    if (!Classified || (ObjCFlagCheck && Classified->IsObjCStyleFlagCheck)) {
      UnhandledConditions = true;
      return;
    }

    // If we've seen multiple conditions for the same subject, don't handle
    // this.
    if (!Conditions.insert({CC.Subject, *Classified}).second) {
      UnhandledConditions = true;
      return;
    }

    if (Classified->IsObjCStyleFlagCheck)
      ObjCFlagCheck = Classified;
  };

  for (auto &CondElement : Cond) {
    if (auto *BoolExpr = CondElement.getBooleanOrNull()) {
      SmallVector<Expr *, 1> Exprs;
      Exprs.push_back(BoolExpr);

      while (!Exprs.empty()) {
        auto *Next = Exprs.pop_back_val()->getSemanticsProvidingExpr();
        if (auto *ACE = dyn_cast<AutoClosureExpr>(Next))
          Next = ACE->getSingleExpressionBody()->getSemanticsProvidingExpr();

        if (auto *BE = dyn_cast_or_null<BinaryExpr>(Next)) {
          auto *Operator = isOperator(BE);
          if (Operator) {
            // If we have an && operator, decompose its arguments.
            if (Operator->getBaseName() == "&&") {
              Exprs.push_back(BE->getLHS());
              Exprs.push_back(BE->getRHS());
            } else {
              // Otherwise check to see if we have an == nil or != nil
              // condition.
              TryAddCond(CallbackCondition(BE, Operator));
            }
            continue;
          }
        }

        // Check to see if we have a lone bool condition.
        TryAddCond(CallbackCondition(Next));
      }
    } else if (auto *P = CondElement.getPatternOrNull()) {
      TryAddCond(CallbackCondition(P, CondElement.getInitializer()));
    }
  }
  return UnhandledConditions || Conditions.empty();
}

void CallbackClassifier::classifyConditional(Stmt *Statement,
                                             StmtCondition Condition,
                                             NodesToPrint ThenNodesToPrint,
                                             Stmt *ElseStmt) {
  ClassifiedCallbackConditions CallbackConditions;
  bool UnhandledConditions = classifyConditionsOf(Condition, ThenNodesToPrint,
                                                  ElseStmt, CallbackConditions);
  auto ErrCondition = CallbackConditions.lookup(Params.getErrParam());

  if (UnhandledConditions) {
    // Some unknown conditions. If there's an else, assume we can't handle
    // and use the fallback case. Otherwise add to either the success or
    // error block depending on some heuristics, known conditions will have
    // placeholders added (ideally we'd remove them)
    // TODO: Remove known conditions and split the `if` statement

    if (IsKnownConditionPath) {
      // If we're on a known condition path, we can be lenient as we already
      // know what block we're in and can therefore just add the conditional
      // straight to it.
      CurrentBlock->addNode(Statement);
    } else if (CallbackConditions.empty()) {
      // Technically this has a similar problem, ie. the else could have
      // conditions that should be in either success/error
      CurrentBlock->addNode(Statement);
    } else if (ElseStmt) {
      DiagEngine.diagnose(Statement->getStartLoc(),
                          diag::unknown_callback_conditions);
    } else if (ErrCondition && ErrCondition->Path == ConditionPath::FAILURE) {
      Blocks.ErrorBlock.addNode(Statement);
    } else {
      for (auto &Entry : CallbackConditions) {
        if (Entry.second.Path == ConditionPath::FAILURE) {
          Blocks.ErrorBlock.addNode(Statement);
          return;
        }
      }
      Blocks.SuccessBlock.addNode(Statement);
    }
    return;
  }

  // If all the conditions were classified, make sure they're all consistently
  // on the success or failure path.
  std::optional<ConditionPath> Path;
  for (auto &Entry : CallbackConditions) {
    auto &Cond = Entry.second;
    if (!Path) {
      Path = Cond.Path;
    } else if (*Path != Cond.Path) {
      // Similar to the unknown conditions case. Add the whole if unless
      // there's an else, in which case use the fallback instead.
      // TODO: Split the `if` statement

      if (ElseStmt) {
        DiagEngine.diagnose(Statement->getStartLoc(),
                            diag::mixed_callback_conditions);
      } else {
        CurrentBlock->addNode(Statement);
      }
      return;
    }
  }
  assert(Path && "Didn't classify a path?");

  auto *ThenBlock = &Blocks.SuccessBlock;
  auto *ElseBlock = &Blocks.ErrorBlock;

  // If the condition is for a failure path, the error block is ThenBlock, and
  // the success block is ElseBlock.
  if (*Path == ConditionPath::FAILURE)
    std::swap(ThenBlock, ElseBlock);

  // We'll be dropping the statement, but make sure to keep any attached
  // comments.
  CurrentBlock->addPossibleCommentLoc(Statement->getStartLoc());

  ThenBlock->addAllBindings(CallbackConditions);

  // TODO: Handle nested ifs
  setNodes(ThenBlock, ElseBlock, std::move(ThenNodesToPrint));

  if (ElseStmt) {
    if (auto *BS = dyn_cast<BraceStmt>(ElseStmt)) {
      // If this is a guard statement, we know that we'll always exit,
      // allowing us to classify any additional nodes into the opposite block.
      auto AlwaysExits = isa<GuardStmt>(Statement);
      setNodes(ElseBlock, ThenBlock, NodesToPrint::inBraceStmt(BS),
               AlwaysExits);
    } else {
      // If we reached here, we should have an else if statement. Given we
      // know we're in the else of a known condition, temporarily flip the
      // current block, and set that we know what path we're on.
      llvm::SaveAndRestore<bool> CondScope(IsKnownConditionPath, true);
      llvm::SaveAndRestore<ClassifiedBlock *> BlockScope(CurrentBlock,
                                                         ElseBlock);
      classifyNodes(ArrayRef<ASTNode>(ElseStmt),
                    /*endCommentLoc*/ SourceLoc());
    }
  }
}

void CallbackClassifier::setNodes(ClassifiedBlock *Block,
                                  ClassifiedBlock *OtherBlock,
                                  NodesToPrint Nodes, bool AlwaysExitsScope) {
  // Drop an explicit trailing 'return' or 'break' if we can.
  bool HasTrailingReturnOrBreak = Nodes.hasTrailingReturnOrBreak();
  if (HasTrailingReturnOrBreak)
    Nodes.dropTrailingReturnOrBreakIfPossible();

  // If we know we're exiting the scope, we can set IsKnownConditionPath, as
  // we know any future nodes should be classified into the other block.
  if (HasTrailingReturnOrBreak || AlwaysExitsScope) {
    CurrentBlock = OtherBlock;
    IsKnownConditionPath = true;
    Block->addAllNodes(std::move(Nodes));
  } else {
    Block->addAllNodes(std::move(Nodes));
  }
}

void CallbackClassifier::classifySwitch(SwitchStmt *SS) {
  auto *ResultParam = Params.getResultParam();
  if (singleSwitchSubject(SS) != ResultParam) {
    CurrentBlock->addNode(SS);
    return;
  }

  // We'll be dropping the switch, but make sure to keep any attached
  // comments.
  CurrentBlock->addPossibleCommentLoc(SS->getStartLoc());

  // Push the cases into a vector. This is only done to eagerly evaluate the
  // AsCaseStmtRange sequence so we can know what the last case is.
  SmallVector<CaseStmt *, 2> Cases;
  Cases.append(SS->getCases().begin(), SS->getCases().end());

  for (auto *CS : Cases) {
    if (CS->hasFallthroughDest()) {
      DiagEngine.diagnose(CS->getLoc(), diag::callback_with_fallthrough);
      return;
    }

    if (CS->isDefault()) {
      DiagEngine.diagnose(CS->getLoc(), diag::callback_with_default);
      return;
    }

    auto Items = CS->getCaseLabelItems();
    if (Items.size() > 1) {
      DiagEngine.diagnose(CS->getLoc(), diag::callback_multiple_case_items);
      return;
    }

    if (Items[0].getWhereLoc().isValid()) {
      DiagEngine.diagnose(CS->getLoc(), diag::callback_where_case_item);
      return;
    }

    auto *Block = &Blocks.SuccessBlock;
    auto *OtherBlock = &Blocks.ErrorBlock;
    auto SuccessNodes = NodesToPrint::inBraceStmt(CS->getBody());

    // Classify the case pattern.
    auto CC = classifyCallbackCondition(
        CallbackCondition(ResultParam, &Items[0]), SuccessNodes,
        /*elseStmt*/ nullptr);
    if (!CC) {
      DiagEngine.diagnose(CS->getLoc(), diag::unknown_callback_case_item);
      return;
    }

    if (CC->Path == ConditionPath::FAILURE)
      std::swap(Block, OtherBlock);

    // We'll be dropping the case, but make sure to keep any attached
    // comments. Because these comments will effectively be part of the
    // previous case, add them to CurrentBlock.
    CurrentBlock->addPossibleCommentLoc(CS->getStartLoc());

    // Make sure to grab trailing comments in the last case stmt.
    if (CS == Cases.back())
      Block->addPossibleCommentLoc(SS->getRBraceLoc());

    setNodes(Block, OtherBlock, std::move(SuccessNodes));
    Block->addBinding(*CC);
  }
  // Mark this switch statement as having been transformed.
  HandledSwitches.insert(SS);
}
