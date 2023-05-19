//===--- Stmt.cpp - Swift Language Statement ASTs -------------------------===//
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
//  This file implements the Stmt class and subclasses.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Stmt.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/PointerUnion.h"

using namespace swift;

#define STMT(Id, _) \
  static_assert(IsTriviallyDestructible<Id##Stmt>::value, \
                "Stmts are BumpPtrAllocated; the destructor is never called");
#include "swift/AST/StmtNodes.def"

//===----------------------------------------------------------------------===//
// Stmt methods.
//===----------------------------------------------------------------------===//

StringRef Stmt::getKindName(StmtKind K) {
  switch (K) {
#define STMT(Id, Parent) case StmtKind::Id: return #Id;
#include "swift/AST/StmtNodes.def"
  }
  llvm_unreachable("bad StmtKind");
}

StringRef Stmt::getDescriptiveKindName(StmtKind K) {
  switch (K) {
  case StmtKind::Brace:
    return "brace";
  case StmtKind::Return:
    return "return";
  case StmtKind::Yield:
    return "yield";
  case StmtKind::Defer:
    return "defer";
  case StmtKind::If:
    return "if";
  case StmtKind::Guard:
    return "guard";
  case StmtKind::While:
    return "while";
  case StmtKind::Do:
    return "do";
  case StmtKind::DoCatch:
    return "do-catch";
  case StmtKind::RepeatWhile:
    return "repeat-while";
  case StmtKind::ForEach:
    return "for-in";
  case StmtKind::Switch:
    return "switch";
  case StmtKind::Case:
    return "case";
  case StmtKind::Break:
    return "break";
  case StmtKind::Continue:
    return "continue";
  case StmtKind::Fallthrough:
    return "fallthrough";
  case StmtKind::Fail:
    return "return";
  case StmtKind::Throw:
    return "throw";
  case StmtKind::Discard:
    return "discard";
  case StmtKind::PoundAssert:
    return "#assert";
  }
  llvm_unreachable("Unhandled case in switch!");
}

// Helper functions to check statically whether a method has been
// overridden from its implementation in Stmt.  The sort of thing you
// need when you're avoiding v-tables.
namespace {
  template <typename ReturnType, typename Class>
  constexpr bool isOverriddenFromStmt(ReturnType (Class::*)() const) {
    return true;
  }
  template <typename ReturnType>
  constexpr bool isOverriddenFromStmt(ReturnType (Stmt::*)() const) {
    return false;
  }

  template <bool IsOverridden> struct Dispatch;

  /// Dispatch down to a concrete override.
  template <> struct Dispatch<true> {
    template <class T> static SourceLoc getStartLoc(const T *S) {
      return S->getStartLoc();
    }
    template <class T> static SourceLoc getEndLoc(const T *S) {
      return S->getEndLoc();
    }
    template <class T> static SourceRange getSourceRange(const T *S) {
      return S->getSourceRange();
    }
  };

  /// Default implementations for when a method isn't overridden.
  template <> struct Dispatch<false> {
    template <class T> static SourceLoc getStartLoc(const T *S) {
      return S->getSourceRange().Start;
    }
    template <class T> static SourceLoc getEndLoc(const T *S) {
      return S->getSourceRange().End;
    }
    template <class T> static SourceRange getSourceRange(const T *S) {
      return { S->getStartLoc(), S->getEndLoc() };
    }
  };
} // end anonymous namespace

template <class T> static SourceRange getSourceRangeImpl(const T *S) {
  static_assert(isOverriddenFromStmt(&T::getSourceRange) ||
                (isOverriddenFromStmt(&T::getStartLoc) &&
                 isOverriddenFromStmt(&T::getEndLoc)),
                "Stmt subclass must implement either getSourceRange() "
                "or getStartLoc()/getEndLoc()");
  return Dispatch<isOverriddenFromStmt(&T::getSourceRange)>::getSourceRange(S);
}
SourceRange Stmt::getSourceRange() const {
  switch (getKind()) {
#define STMT(ID, PARENT)                                           \
  case StmtKind::ID: return getSourceRangeImpl(cast<ID##Stmt>(this));
#include "swift/AST/StmtNodes.def"
  }
  
  llvm_unreachable("statement type not handled!");
}

template <class T> static SourceLoc getStartLocImpl(const T *S) {
  return Dispatch<isOverriddenFromStmt(&T::getStartLoc)>::getStartLoc(S);
}
SourceLoc Stmt::getStartLoc() const {
  switch (getKind()) {
#define STMT(ID, PARENT)                                           \
  case StmtKind::ID: return getStartLocImpl(cast<ID##Stmt>(this));
#include "swift/AST/StmtNodes.def"
  }

  llvm_unreachable("statement type not handled!");
}

template <class T> static SourceLoc getEndLocImpl(const T *S) {
  return Dispatch<isOverriddenFromStmt(&T::getEndLoc)>::getEndLoc(S);
}
SourceLoc Stmt::getEndLoc() const {
  switch (getKind()) {
#define STMT(ID, PARENT)                                           \
  case StmtKind::ID: return getEndLocImpl(cast<ID##Stmt>(this));
#include "swift/AST/StmtNodes.def"
  }

  llvm_unreachable("statement type not handled!");
}

BraceStmt::BraceStmt(SourceLoc lbloc, ArrayRef<ASTNode> elts,
                     SourceLoc rbloc, Optional<bool> implicit)
  : Stmt(StmtKind::Brace, getDefaultImplicitFlag(implicit, lbloc)),
    LBLoc(lbloc), RBLoc(rbloc)
{
  Bits.BraceStmt.NumElements = elts.size();
  std::uninitialized_copy(elts.begin(), elts.end(),
                          getTrailingObjects<ASTNode>());

#ifndef NDEBUG
  for (auto elt : elts)
    if (auto *decl = elt.dyn_cast<Decl *>())
      assert(!isa<AccessorDecl>(decl) && "accessors should not be added here");
#endif
}

BraceStmt *BraceStmt::create(ASTContext &ctx, SourceLoc lbloc,
                             ArrayRef<ASTNode> elts, SourceLoc rbloc,
                             Optional<bool> implicit) {
  assert(std::none_of(elts.begin(), elts.end(),
                      [](ASTNode node) -> bool { return node.isNull(); }) &&
         "null element in BraceStmt");

  void *Buffer = ctx.Allocate(totalSizeToAlloc<ASTNode>(elts.size()),
                              alignof(BraceStmt));
  return ::new(Buffer) BraceStmt(lbloc, elts, rbloc, implicit);
}

SourceLoc BraceStmt::getStartLoc() const {
  if (LBLoc) {
    return LBLoc;
  }
  return getContentStartLoc();
}

SourceLoc BraceStmt::getEndLoc() const {
  if (RBLoc) {
    return RBLoc;
  }
  return getContentEndLoc();
}

SourceLoc BraceStmt::getContentStartLoc() const {
  for (auto elt : getElements()) {
    if (auto loc = elt.getStartLoc()) {
      return loc;
    }
  }
  return SourceLoc();
}

SourceLoc BraceStmt::getContentEndLoc() const {
  for (auto elt : llvm::reverse(getElements())) {
    if (auto loc = elt.getEndLoc()) {
      return loc;
    }
  }
  return SourceLoc();
}

ASTNode BraceStmt::findAsyncNode() {
  // TODO: Statements don't track their ASTContext/evaluator, so I am not making
  // this a request. It probably should be a request at some point.
  //
  // While we're at it, it would be very nice if this could be a const
  // operation, but the AST-walking is not a const operation.

  // A walker that looks for 'async' and 'await' expressions
  // that aren't nested within closures or nested declarations.
  class FindInnerAsync : public ASTWalker {
    ASTNode AsyncNode;

    /// Walk only the macro arguments.
    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
      // If we've found an 'await', record it and terminate the traversal.
      if (isa<AwaitExpr>(expr)) {
        AsyncNode = expr;
        return Action::Stop();
      }

      // Do not recurse into other closures.
      if (isa<ClosureExpr>(expr))
        return Action::SkipChildren(expr);

      return Action::Continue(expr);
    }

    PreWalkAction walkToDeclPre(Decl *decl) override {
      // Do not walk into function or type declarations.
      if (auto *patternBinding = dyn_cast<PatternBindingDecl>(decl)) {
        if (patternBinding->isAsyncLet())
          AsyncNode = patternBinding;

        return Action::Continue();
      }

      return Action::SkipChildren();
    }

    PreWalkResult<Stmt *> walkToStmtPre(Stmt *stmt) override {
      if (auto forEach = dyn_cast<ForEachStmt>(stmt)) {
        if (forEach->getAwaitLoc().isValid()) {
          AsyncNode = forEach;
          return Action::Stop();
        }
      }

      return Action::Continue(stmt);
    }

  public:
    ASTNode getAsyncNode() { return AsyncNode; }
  };

  FindInnerAsync asyncFinder;
  walk(asyncFinder);

  return asyncFinder.getAsyncNode();
}

static bool hasSingleActiveElement(ArrayRef<ASTNode> elts) {
  while (true) {
    // Single element brace.
    if (elts.size() == 1)
      return true;

    // See if we have a #if as the first element of a 2 element brace, if so we
    // can recuse into its active clause. If so, the second element will be the
    // active element.
    if (elts.size() == 2) {
      if (auto *D = elts.front().dyn_cast<Decl *>()) {
        if (auto *ICD = dyn_cast<IfConfigDecl>(D)) {
          elts = ICD->getActiveClauseElements();
          continue;
        }
      }
    }
    return false;
  }
}

ASTNode BraceStmt::getSingleActiveElement() const {
  return hasSingleActiveElement(getElements()) ? getLastElement() : nullptr;
}

Expr *BraceStmt::getSingleActiveExpression() const {
  return getSingleActiveElement().dyn_cast<Expr *>();
}

Stmt *BraceStmt::getSingleActiveStatement() const {
  return getSingleActiveElement().dyn_cast<Stmt *>();
}

IsSingleValueStmtResult Stmt::mayProduceSingleValue(Evaluator &eval) const {
  return evaluateOrDefault(eval, IsSingleValueStmtRequest{this},
                           IsSingleValueStmtResult::circularReference());
}

IsSingleValueStmtResult Stmt::mayProduceSingleValue(ASTContext &ctx) const {
  return mayProduceSingleValue(ctx.evaluator);
}

SourceLoc ReturnStmt::getStartLoc() const {
  if (ReturnLoc.isInvalid() && Result)
    return Result->getStartLoc();
  return ReturnLoc;
}
SourceLoc ReturnStmt::getEndLoc() const {
  if (Result && Result->getEndLoc().isValid())
    return Result->getEndLoc();
  return ReturnLoc;
}

YieldStmt *YieldStmt::create(const ASTContext &ctx, SourceLoc yieldLoc,
                             SourceLoc lpLoc, ArrayRef<Expr*> yields,
                             SourceLoc rpLoc, Optional<bool> implicit) {
  void *buffer = ctx.Allocate(totalSizeToAlloc<Expr*>(yields.size()),
                              alignof(YieldStmt));
  return ::new(buffer) YieldStmt(yieldLoc, lpLoc, yields, rpLoc, implicit);
}

SourceLoc YieldStmt::getEndLoc() const {
  return RPLoc.isInvalid() ? getYields()[0]->getEndLoc() : RPLoc;
}

SourceLoc ThrowStmt::getEndLoc() const { return SubExpr->getEndLoc(); }

SourceLoc DiscardStmt::getEndLoc() const { return SubExpr->getEndLoc(); }

SourceLoc DeferStmt::getEndLoc() const {
  return tempDecl->getBody()->getEndLoc();
}

/// Dig the original user's body of the defer out for AST fidelity.
BraceStmt *DeferStmt::getBodyAsWritten() const {
  return tempDecl->getBody();
}

bool LabeledStmt::isPossibleContinueTarget() const {
  switch (getKind()) {
#define LABELED_STMT(ID, PARENT)
#define STMT(ID, PARENT) case StmtKind::ID:
#include "swift/AST/StmtNodes.def"
    llvm_unreachable("not a labeled statement");

  // Sema has diagnostics with hard-coded expectations about what
  // statements return false from this method.
  case StmtKind::If:
  case StmtKind::Guard:
  case StmtKind::Switch:
    return false;

  case StmtKind::Do:
  case StmtKind::DoCatch:
  case StmtKind::RepeatWhile:
  case StmtKind::ForEach:
  case StmtKind::While:
    return true;
  }
  llvm_unreachable("statement kind unhandled!");
}

bool LabeledStmt::requiresLabelOnJump() const {
  switch (getKind()) {
#define LABELED_STMT(ID, PARENT)
#define STMT(ID, PARENT) case StmtKind::ID:
#include "swift/AST/StmtNodes.def"
    llvm_unreachable("not a labeled statement");

  case StmtKind::If:
  case StmtKind::Do:
  case StmtKind::DoCatch:
  case StmtKind::Guard: // Guard doesn't allow labels, so no break/continue.
    return true;

  case StmtKind::RepeatWhile:
  case StmtKind::ForEach:
  case StmtKind::Switch:
  case StmtKind::While:
    return false;
  }
  llvm_unreachable("statement kind unhandled!");
}

void ForEachStmt::setPattern(Pattern *p) {
  Pat = p;
  Pat->markOwnedByStatement(this);
}

Expr *ForEachStmt::getTypeCheckedSequence() const {
  return iteratorVar ? iteratorVar->getInit(/*index=*/0) : nullptr;
}

DoCatchStmt *DoCatchStmt::create(ASTContext &ctx, LabeledStmtInfo labelInfo,
                                 SourceLoc doLoc, Stmt *body,
                                 ArrayRef<CaseStmt *> catches,
                                 Optional<bool> implicit) {
  void *mem = ctx.Allocate(totalSizeToAlloc<CaseStmt *>(catches.size()),
                           alignof(DoCatchStmt));
  return ::new (mem) DoCatchStmt(labelInfo, doLoc, body, catches, implicit);
}

bool CaseLabelItem::isSyntacticallyExhaustive() const {
  return getGuardExpr() == nullptr && !getPattern()->isRefutablePattern();
}

bool DoCatchStmt::isSyntacticallyExhaustive() const {
  for (auto clause : getCatches()) {
    for (auto &LabelItem : clause->getCaseLabelItems()) {
      if (LabelItem.isSyntacticallyExhaustive())
        return true;
    }
  }
  return false;
}

void LabeledConditionalStmt::setCond(StmtCondition e) {
  // When set a condition into a Conditional Statement, inform each of the
  // variables bound in any patterns that this is the owning statement for the
  // pattern.
  for (auto &elt : e)
    if (auto pat = elt.getPatternOrNull())
      pat->markOwnedByStatement(this);
  
  Cond = e;
}

/// Whether or not this conditional stmt rebinds self with a `let self`
/// or `let self = self` condition. If `requireLoadExpr` is `true`,
/// additionally requires that the RHS of the self condition is a `LoadExpr`.
bool LabeledConditionalStmt::rebindsSelf(ASTContext &Ctx,
                                         bool requireLoadExpr) const {
  return llvm::any_of(getCond(), [&Ctx, requireLoadExpr](const auto &cond) {
    return cond.rebindsSelf(Ctx, requireLoadExpr);
  });
}

/// Whether or not this conditional stmt rebinds self with a `let self`
/// or `let self = self` condition. If `requireLoadExpr` is `true`,
/// additionally requires that the RHS of the self condition is a `LoadExpr`.
bool StmtConditionElement::rebindsSelf(ASTContext &Ctx,
                                       bool requireLoadExpr) const {
  auto pattern = getPatternOrNull();
  if (!pattern) {
    return false;
  }

  // Check whether or not this pattern defines a new `self` decl
  bool isSelfRebinding = false;
  if (pattern->getBoundName() == Ctx.Id_self) {
    isSelfRebinding = true;
  }

  else if (auto OSP = dyn_cast<OptionalSomePattern>(pattern)) {
    if (auto subPattern = OSP->getSubPattern()) {
      isSelfRebinding = subPattern->getBoundName() == Ctx.Id_self;
    }
  }

  if (!isSelfRebinding) {
    return false;
  }

  // Check that the RHS expr is exactly `self` and not something else
  Expr *exprToCheckForDRE = getInitializerOrNull();
  if (!exprToCheckForDRE) {
    return false;
  }

  if (requireLoadExpr && !isa<LoadExpr>(exprToCheckForDRE)) {
    return false;
  }

  if (auto *load = dyn_cast<LoadExpr>(exprToCheckForDRE)) {
    exprToCheckForDRE = load->getSubExpr();
  }

  if (auto *DRE = dyn_cast<DeclRefExpr>(
          exprToCheckForDRE->getSemanticsProvidingExpr())) {
    auto *decl = DRE->getDecl();
    return decl && decl->hasName() ? decl->getName().isSimpleName(Ctx.Id_self)
                                   : false;
  }

  return false;
}

PoundAvailableInfo *
PoundAvailableInfo::create(ASTContext &ctx, SourceLoc PoundLoc,
                           SourceLoc LParenLoc,
                           ArrayRef<AvailabilitySpec *> queries,
                           SourceLoc RParenLoc, bool isUnavailability) {
  unsigned size = totalSizeToAlloc<AvailabilitySpec *>(queries.size());
  void *Buffer = ctx.Allocate(size, alignof(PoundAvailableInfo));
  return ::new (Buffer) PoundAvailableInfo(PoundLoc, LParenLoc, queries,
                                           RParenLoc, isUnavailability);
}

SourceLoc PoundAvailableInfo::getEndLoc() const {
  if (RParenLoc.isInvalid()) {
    if (NumQueries == 0) {
      if (LParenLoc.isInvalid())
        return PoundLoc;
      return LParenLoc;
    }
    return getQueries()[NumQueries - 1]->getSourceRange().End;
  }
  return RParenLoc;
}

SourceRange StmtConditionElement::getSourceRange() const {
  switch (getKind()) {
  case StmtConditionElement::CK_Boolean:
    return getBoolean()->getSourceRange();
  case StmtConditionElement::CK_Availability:
    return getAvailability()->getSourceRange();
  case StmtConditionElement::CK_HasSymbol:
    return getHasSymbolInfo()->getSourceRange();
  case StmtConditionElement::CK_PatternBinding:
    SourceLoc Start;
    if (IntroducerLoc.isValid())
      Start = IntroducerLoc;
    else
      Start = getPattern()->getStartLoc();
    
    SourceLoc End = getInitializer()->getEndLoc();
    if (Start.isValid() && End.isValid()) {
      return SourceRange(Start, End);
    } else {
      return SourceRange();
    }
  }

  llvm_unreachable("Unhandled StmtConditionElement in switch.");
}

PoundHasSymbolInfo *PoundHasSymbolInfo::create(ASTContext &Ctx,
                                               SourceLoc PoundLoc,
                                               SourceLoc LParenLoc,
                                               Expr *SymbolExpr,
                                               SourceLoc RParenLoc) {
  return new (Ctx)
      PoundHasSymbolInfo(PoundLoc, LParenLoc, SymbolExpr, RParenLoc);
}

SourceLoc StmtConditionElement::getStartLoc() const {
  switch (getKind()) {
  case StmtConditionElement::CK_Boolean:
    return getBoolean()->getStartLoc();
  case StmtConditionElement::CK_Availability:
    return getAvailability()->getStartLoc();
  case StmtConditionElement::CK_PatternBinding:
    return getSourceRange().Start;
  case StmtConditionElement::CK_HasSymbol:
    return getHasSymbolInfo()->getStartLoc();
  }

  llvm_unreachable("Unhandled StmtConditionElement in switch.");
}

SourceLoc StmtConditionElement::getEndLoc() const {
  switch (getKind()) {
  case StmtConditionElement::CK_Boolean:
    return getBoolean()->getEndLoc();
  case StmtConditionElement::CK_Availability:
    return getAvailability()->getEndLoc();
  case StmtConditionElement::CK_PatternBinding:
    return getSourceRange().End;
  case StmtConditionElement::CK_HasSymbol:
    return getHasSymbolInfo()->getEndLoc();
  }

  llvm_unreachable("Unhandled StmtConditionElement in switch.");
}

static StmtCondition exprToCond(Expr *C, ASTContext &Ctx) {
  StmtConditionElement Arr[] = { StmtConditionElement(C) };
  return Ctx.AllocateCopy(Arr);
}

IfStmt::IfStmt(SourceLoc IfLoc, Expr *Cond, Stmt *Then, SourceLoc ElseLoc,
               Stmt *Else, Optional<bool> implicit, ASTContext &Ctx)
  : IfStmt(LabeledStmtInfo(), IfLoc, exprToCond(Cond, Ctx), Then, ElseLoc, Else,
           implicit) {
}

ArrayRef<Stmt *> IfStmt::getBranches(SmallVectorImpl<Stmt *> &scratch) const {
  assert(scratch.empty());
  scratch.push_back(getThenStmt());

  auto *elseBranch = getElseStmt();
  while (elseBranch) {
    if (auto *IS = dyn_cast<IfStmt>(elseBranch)) {
      // Look through else ifs.
      elseBranch = IS->getElseStmt();
      scratch.push_back(IS->getThenStmt());
      continue;
    }
    // An unconditional else, we're done.
    scratch.push_back(elseBranch);
    break;
  }
  return scratch;
}

bool IfStmt::isSyntacticallyExhaustive() const {
  auto *elseBranch = getElseStmt();
  while (elseBranch) {
    // Look through else ifs.
    if (auto *IS = dyn_cast<IfStmt>(elseBranch)) {
      elseBranch = IS->getElseStmt();
      continue;
    }
    // An unconditional else.
    return true;
  }
  return false;
}

GuardStmt::GuardStmt(SourceLoc GuardLoc, Expr *Cond, BraceStmt *Body,
                     Optional<bool> implicit, ASTContext &Ctx)
  : GuardStmt(GuardLoc, exprToCond(Cond, Ctx), Body, implicit) {
    
}
  


SourceLoc RepeatWhileStmt::getEndLoc() const { return Cond->getEndLoc(); }

SourceRange CaseLabelItem::getSourceRange() const {
  if (auto *E = getGuardExpr())
    return { getPattern()->getStartLoc(), E->getEndLoc() };
  return getPattern()->getSourceRange();
}
SourceLoc CaseLabelItem::getStartLoc() const {
  return getPattern()->getStartLoc();
}
SourceLoc CaseLabelItem::getEndLoc() const {
  if (auto *E = getGuardExpr())
    return E->getEndLoc();
  return getPattern()->getEndLoc();
}

CaseStmt::CaseStmt(CaseParentKind parentKind, SourceLoc itemIntroducerLoc,
                   ArrayRef<CaseLabelItem> caseLabelItems,
                   SourceLoc unknownAttrLoc, SourceLoc itemTerminatorLoc,
                   BraceStmt *body,
                   Optional<MutableArrayRef<VarDecl *>> caseBodyVariables,
                   Optional<bool> implicit,
                   NullablePtr<FallthroughStmt> fallthroughStmt)
    : Stmt(StmtKind::Case, getDefaultImplicitFlag(implicit, itemIntroducerLoc)),
      UnknownAttrLoc(unknownAttrLoc), ItemIntroducerLoc(itemIntroducerLoc),
      ItemTerminatorLoc(itemTerminatorLoc), ParentKind(parentKind),
      BodyAndHasFallthrough(body, fallthroughStmt.isNonNull()),
      CaseBodyVariables(caseBodyVariables) {
  Bits.CaseStmt.NumPatterns = caseLabelItems.size();
  assert(Bits.CaseStmt.NumPatterns > 0 &&
         "case block must have at least one pattern");
  assert(
      !(parentKind == CaseParentKind::DoCatch && fallthroughStmt.isNonNull()) &&
      "Only switch cases can have a fallthrough.");
  if (hasFallthroughDest()) {
    *getTrailingObjects<FallthroughStmt *>() = fallthroughStmt.get();
  }

  MutableArrayRef<CaseLabelItem> items{getTrailingObjects<CaseLabelItem>(),
                                       Bits.CaseStmt.NumPatterns};

  // At the beginning mark all of our var decls as being owned by this
  // statement. In the typechecker we wireup the case stmt var decl list since
  // we know everything is lined up/typechecked then.
  for (unsigned i : range(Bits.CaseStmt.NumPatterns)) {
    new (&items[i]) CaseLabelItem(caseLabelItems[i]);
    items[i].getPattern()->markOwnedByStatement(this);
  }
  for (auto *vd : caseBodyVariables.value_or(MutableArrayRef<VarDecl *>())) {
    vd->setParentPatternStmt(this);
  }
}

CaseStmt *CaseStmt::create(ASTContext &ctx, CaseParentKind ParentKind,
                           SourceLoc caseLoc,
                           ArrayRef<CaseLabelItem> caseLabelItems,
                           SourceLoc unknownAttrLoc, SourceLoc colonLoc,
                           BraceStmt *body,
                           Optional<MutableArrayRef<VarDecl *>> caseVarDecls,
                           Optional<bool> implicit,
                           NullablePtr<FallthroughStmt> fallthroughStmt) {
  void *mem =
      ctx.Allocate(totalSizeToAlloc<FallthroughStmt *, CaseLabelItem>(
                       fallthroughStmt.isNonNull(), caseLabelItems.size()),
                   alignof(CaseStmt));
  return ::new (mem)
      CaseStmt(ParentKind, caseLoc, caseLabelItems, unknownAttrLoc, colonLoc,
               body, caseVarDecls, implicit, fallthroughStmt);
}

DoStmt *DoStmt::createImplicit(ASTContext &C, LabeledStmtInfo labelInfo,
                               ArrayRef<ASTNode> body) {
  return new (C) DoStmt(labelInfo, /*doLoc=*/SourceLoc(),
                        BraceStmt::createImplicit(C, body),
                        /*implicit=*/true);
}

SourceLoc DoStmt::getStartLoc() const {
  if (auto LabelOrDoLoc = getLabelLocOrKeywordLoc(DoLoc)) {
    return LabelOrDoLoc;
  }
  return Body->getStartLoc();
}

SourceLoc DoStmt::getEndLoc() const {
  return Body->getEndLoc();
}

namespace {

template<typename CaseIterator>
CaseStmt *findNextCaseStmt(
    CaseIterator first, CaseIterator last, const CaseStmt *caseStmt) {
  for(auto caseIter = first; caseIter != last; ++caseIter) {
    if (*caseIter == caseStmt) {
      ++caseIter;
      return caseIter == last ? nullptr : *caseIter;
    }
  }

  return nullptr;
}

}

CaseStmt *CaseStmt::findNextCaseStmt() const {
  auto parent = getParentStmt();
  if (!parent)
    return nullptr;

  if (auto switchParent = dyn_cast<SwitchStmt>(parent)) {
    return ::findNextCaseStmt(
        switchParent->getCases().begin(), switchParent->getCases().end(),
        this);
  }

  auto doCatchParent = cast<DoCatchStmt>(parent);
  return ::findNextCaseStmt(
      doCatchParent->getCatches().begin(), doCatchParent->getCatches().end(),
      this);
}

SwitchStmt *SwitchStmt::create(LabeledStmtInfo LabelInfo, SourceLoc SwitchLoc,
                               Expr *SubjectExpr,
                               SourceLoc LBraceLoc,
                               ArrayRef<ASTNode> Cases,
                               SourceLoc RBraceLoc,
                               SourceLoc EndLoc,
                               ASTContext &C) {
#ifndef NDEBUG
  for (auto N : Cases)
    assert((N.is<Stmt*>() && isa<CaseStmt>(N.get<Stmt*>())) ||
           (N.is<Decl*>() && (isa<IfConfigDecl>(N.get<Decl*>()) ||
                              isa<PoundDiagnosticDecl>(N.get<Decl*>()))));
#endif

  void *p = C.Allocate(totalSizeToAlloc<ASTNode>(Cases.size()),
                       alignof(SwitchStmt));
  SwitchStmt *theSwitch = ::new (p) SwitchStmt(LabelInfo, SwitchLoc,
                                               SubjectExpr, LBraceLoc,
                                               Cases.size(), RBraceLoc,
                                               EndLoc);

  std::uninitialized_copy(Cases.begin(), Cases.end(),
                          theSwitch->getTrailingObjects<ASTNode>());
  for (auto *caseStmt : theSwitch->getCases())
    caseStmt->setParentStmt(theSwitch);

  return theSwitch;
}

LabeledStmt *BreakStmt::getTarget() const {
  auto &eval = getDeclContext()->getASTContext().evaluator;
  return evaluateOrDefault(eval, BreakTargetRequest{this}, nullptr);
}

LabeledStmt *ContinueStmt::getTarget() const {
  auto &eval = getDeclContext()->getASTContext().evaluator;
  return evaluateOrDefault(eval, ContinueTargetRequest{this}, nullptr);
}

SourceLoc swift::extractNearestSourceLoc(const Stmt *S) {
  return S->getStartLoc();
}

ArrayRef<Stmt *>
SwitchStmt::getBranches(SmallVectorImpl<Stmt *> &scratch) const {
  assert(scratch.empty());
  for (auto *CS : getCases())
    scratch.push_back(CS->getBody());
  return scratch;
}

// See swift/Basic/Statistic.h for declaration: this enables tracing Stmts, is
// defined here to avoid too much layering violation / circular linkage
// dependency.

struct StmtTraceFormatter : public UnifiedStatsReporter::TraceFormatter {
  void traceName(const void *Entity, raw_ostream &OS) const override {
    if (!Entity)
      return;
    const Stmt *S = static_cast<const Stmt *>(Entity);
    OS << Stmt::getKindName(S->getKind());
  }
  void traceLoc(const void *Entity, SourceManager *SM,
                clang::SourceManager *CSM, raw_ostream &OS) const override {
    if (!Entity)
      return;
    const Stmt *S = static_cast<const Stmt *>(Entity);
    S->getSourceRange().print(OS, *SM, false);
  }
};

static StmtTraceFormatter TF;

template<>
const UnifiedStatsReporter::TraceFormatter*
FrontendStatsTracer::getTraceFormatter<const Stmt *>() {
  return &TF;
}
