//===--- DotExprCodeCompletion.cpp ----------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/IDE/PostfixCompletion.h"
#include "swift/IDE/CodeCompletion.h"
#include "swift/IDE/CompletionLookup.h"
#include "swift/Sema/CompletionContextFinder.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/IDETypeChecking.h"

using namespace swift;
using namespace swift::constraints;
using namespace swift::ide;

bool PostfixCompletionCallback::Result::tryMerge(const Result &Other,
                                                 DeclContext *DC) {
  if (BaseDecl != Other.BaseDecl)
    return false;

  // These properties should match if we are talking about the same BaseDecl.
  assert(IsBaseDeclUnapplied == Other.IsBaseDeclUnapplied);
  assert(BaseIsStaticMetaType == Other.BaseIsStaticMetaType);

  auto baseTy = tryMergeBaseTypeForCompletionLookup(BaseTy, Other.BaseTy, DC);
  if (!baseTy)
    return false;

  BaseTy = baseTy;

  // There could be multiple results that have different actor isolations if the
  // closure is an argument to a function that has multiple overloads with
  // different isolations for the closure. Producing multiple results for these
  // is usually not very enlightning. For now, we just pick the first actor
  // isolation that we find. This is good enough in practice.
  // What we should really do is probably merge these two actor isolations and
  // pick the weakest isolation for each closure.

  for (auto &OtherExpectedTy : Other.ExpectedTypes) {
    auto IsEqual = [&](Type Ty) { return Ty->isEqual(OtherExpectedTy); };
    if (llvm::any_of(ExpectedTypes, IsEqual)) {
      // We already know if this expected type
      continue;
    }
    ExpectedTypes.push_back(OtherExpectedTy);
  }
  ExpectsNonVoid &= Other.ExpectsNonVoid;
  IsImpliedResult |= Other.IsImpliedResult;
  IsInAsyncContext |= Other.IsInAsyncContext;
  return true;
}

void PostfixCompletionCallback::addResult(const Result &Res) {
  for (auto idx : indices(Results)) {
    if (Results[idx].tryMerge(Res, DC))
      return;
  }
  Results.push_back(Res);
}

void PostfixCompletionCallback::fallbackTypeCheck(DeclContext *DC) {
  assert(!gotCallback());

  // Default to checking the completion expression in isolation.
  Expr *fallbackExpr = CompletionExpr;
  DeclContext *fallbackDC = DC;

  CompletionContextFinder finder(DC);
  if (finder.hasCompletionExpr()) {
    if (auto fallback = finder.getFallbackCompletionExpr()) {
      fallbackExpr = fallback->E;
      fallbackDC = fallback->DC;
    }
  }

  if (isa<AbstractClosureExpr>(fallbackDC)) {
    // If the expression is embedded in a closure, the constraint system tries
    // to retrieve that closure's type, which will fail since we won't have
    // generated any type variables for it. Thus, fallback type checking isn't
    // available in this case.
    return;
  }

  SyntacticElementTarget completionTarget(fallbackExpr, fallbackDC, CTP_Unused,
                                          Type(),
                                          /*isDiscared=*/true);

  typeCheckForCodeCompletion(completionTarget, /*needsPrecheck*/ true,
                             [&](const Solution &S) { sawSolution(S); });
}

static ActorIsolation
getClosureActorIsolation(const Solution &S, AbstractClosureExpr *ACE) {
  auto getType = [&S](Expr *E) -> Type {
    // Prefer the contextual type of the closure because it might be 'weaker'
    // than the type determined for the closure by the constraints system. E.g.
    // the contextual type might have a global actor attribute but because no
    // methods from that global actor are called in the closure, the closure has
    // a non-actor type.
    if (auto target = S.getTargetFor(dyn_cast<ClosureExpr>(E))) {
      if (auto Ty = target->getClosureContextualType())
        return Ty;
    }
    if (!S.hasType(E)) {
      return Type();
    }
    return getTypeForCompletion(S, E);
  };
  auto getClosureActorIsolationThunk = [&S](AbstractClosureExpr *ACE) {
    return getClosureActorIsolation(S, ACE);
  };
  return determineClosureActorIsolation(ACE, getType,
                                        getClosureActorIsolationThunk);
}

/// Returns \c true if \p Choice refers to a function that hasn't been called
/// yet.
static bool isUnappliedFunctionRef(const OverloadChoice &Choice) {
  if (!Choice.isDecl()) {
    return false;
  }
  switch (Choice.getFunctionRefKind()) {
  case FunctionRefKind::Unapplied:
    return true;
  case FunctionRefKind::SingleApply:
    if (auto BaseTy = Choice.getBaseType()) {
      // We consider curried member calls as unapplied. E.g.
      //   MyStruct.someInstanceFunc(theInstance)#^COMPLETE^#
      // is unapplied.
      return BaseTy->is<MetatypeType>() && !Choice.getDeclOrNull()->isStatic();
    } else {
      return false;
    }
  default:
    return false;
  }
}

void PostfixCompletionCallback::sawSolutionImpl(
    const constraints::Solution &S) {
  auto &CS = S.getConstraintSystem();
  auto *ParsedExpr = CompletionExpr->getBase();
  auto *SemanticExpr = ParsedExpr->getSemanticsProvidingExpr();

  if (!S.hasType(ParsedExpr)) {
    return;
  }

  auto BaseTy = getTypeForCompletion(S, ParsedExpr);
  // If base type couldn't be determined (e.g. because base expression
  // is an invalid reference), let's not attempt to do a lookup since
  // it wouldn't produce any useful results anyway.
  if (!BaseTy)
    return;

  auto *Locator = CS.getConstraintLocator(SemanticExpr);
  Type ExpectedTy = getTypeForCompletion(S, CompletionExpr);
  Expr *ParentExpr = CS.getParentExpr(CompletionExpr);

  auto *CalleeLocator = S.getCalleeLocator(Locator);
  ValueDecl *ReferencedDecl = nullptr;
  bool IsBaseDeclUnapplied = false;
  if (auto SelectedOverload = S.getOverloadChoiceIfAvailable(CalleeLocator)) {
    ReferencedDecl = SelectedOverload->choice.getDeclOrNull();
    IsBaseDeclUnapplied = isUnappliedFunctionRef(SelectedOverload->choice);
  }

  bool BaseIsStaticMetaType = S.isStaticallyDerivedMetatype(ParsedExpr);

  bool ExpectsNonVoid = false;
  SmallVector<Type, 4> ExpectedTypes;
  if (ExpectedTy) {
    ExpectedTypes.push_back(ExpectedTy);
    ExpectsNonVoid = !ExpectedTy->isVoid();
  } else {
    // If we don't know what the expected type is, assume it must be non-Void
    // if we have a contextual type that is not unused. This prevents us from
    // suggesting Void values for e.g bindings without explicit types.
    ExpectsNonVoid |= !ParentExpr &&
                      CS.getContextualTypePurpose(CompletionExpr) != CTP_Unused;

    for (auto SAT : S.targets) {
      if (ExpectsNonVoid) {
        // ExpectsNonVoid is already set. No need to iterate further.
        break;
      }
      if (SAT.second.getAsExpr() == CompletionExpr) {
        ExpectsNonVoid |=
            SAT.second.getExprContextualTypePurpose() != CTP_Unused;
      }
    }
  }

  bool IsImpliedResult = isImpliedResult(S, CompletionExpr);

  bool IsInAsyncContext = isContextAsync(S, DC);
  llvm::DenseMap<AbstractClosureExpr *, ActorIsolation>
      ClosureActorIsolations;
  for (auto SAT : S.targets) {
    if (auto ACE = getAsExpr<AbstractClosureExpr>(SAT.second.getAsASTNode())) {
      ClosureActorIsolations[ACE] = getClosureActorIsolation(S, ACE);
    }
  }

  Result Res = {
      BaseTy,
      ReferencedDecl,
      IsBaseDeclUnapplied,
      BaseIsStaticMetaType,
      ExpectedTypes,
      ExpectsNonVoid,
      IsImpliedResult,
      IsInAsyncContext,
      ClosureActorIsolations
  };

  addResult(Res);
}

/// Returns \c true if \p T is '_OptionalNilComparisonType'.
static bool isOptionalNilComparisonType(Type T) {
  if (!T) {
    return false;
  }
  auto *nominal = T->getAnyNominal();
  if (!nominal) {
    return false;
  }
  return (nominal->isStdlibDecl() &&
          nominal->getName() ==
              nominal->getASTContext().Id_OptionalNilComparisonType);
}

static DeclRefKind getDeclRefKindOfOperator(OperatorDecl *op) {
  switch (op->getKind()) {
  case DeclKind::PrefixOperator:
    return DeclRefKind::PrefixOperator;
  case DeclKind::PostfixOperator:
    return DeclRefKind::PostfixOperator;
  case DeclKind::InfixOperator:
    return DeclRefKind::BinaryOperator;
  default:
    llvm_unreachable("unexpected operator kind");
  }
}

/// Return type of \c getOperatorCompletionTypes.
struct OperatorResultTypes {
  /// If we are trying to complete a binary operator, the type the operator
  /// expects for the RHS. Null for postfix operators.
  Type RHSType;

  /// The type the operator returns when called.
  Type ResultType;

  bool operator==(const OperatorResultTypes &Other) const {
    return nullableTypesEqual(RHSType, Other.RHSType) &&
           nullableTypesEqual(ResultType, Other.ResultType);
  }
};

/// Builds a constriant system that tries applying the operator \p op on a LHS
/// of type \p LHSType. If that succeeds, returns the result type of the
/// operator call and (in case of binary operators) the expected type for the
/// RHS.
static SmallVector<OperatorResultTypes>
getOperatorCompletionTypes(DeclContext *DC, Type LHSType, OperatorDecl *Op) {
  ConstraintSystemOptions options;
  options |= ConstraintSystemFlags::SuppressDiagnostics;

  ConstraintSystem CS(DC, options);

  // The source loc of the generated expression doesn't matter.
  SourceLoc Loc;

  // We represent the LHS and RHS by CodeCompletionExprs because there's no
  // other better choice. rhs will have its type set in the constraint system
  // below and, in case of binary operators, rhs will be inspected for its type
  // when the constraint system has been solved.
  CodeCompletionExpr LHS(Loc);
  CodeCompletionExpr RHS(Loc);

  UnresolvedDeclRefExpr UDRE(DeclNameRef(Op->getName()),
                             getDeclRefKindOfOperator(Op), DeclNameLoc(Loc));
  DiagnosticTransaction IgnoreDiags(DC->getASTContext().Diags);
  Expr *OpExpr =
      resolveDeclRefExpr(&UDRE, DC, /*replaceInvalidRefsWithErrors=*/true);
  IgnoreDiags.abort();
  if (isa<ErrorExpr>(OpExpr)) {
    // If we couldn't resolve the operator (e.g. because there is only an
    // operator definition but no decls that implement it), we can't call the
    // operator.
    return {};
  }

  Expr *OpCallExpr;
  switch (Op->getKind()) {
  case DeclKind::PrefixOperator:
    // Don't insert prefix operators in postfix position.
    return {};
  case DeclKind::PostfixOperator:
    OpCallExpr = PostfixUnaryExpr::create(DC->getASTContext(), OpExpr, &LHS);
    break;
  case DeclKind::InfixOperator:
    OpCallExpr = BinaryExpr::create(DC->getASTContext(), &LHS, OpExpr, &RHS,
                                    /*implicit*/ true);
    break;
  default:
    llvm_unreachable("unexpected operator kind");
  }

  CS.preCheckExpression(OpCallExpr, DC, /*replaceInvalidRefsWithErrors=*/true);
  OpCallExpr = CS.generateConstraints(OpCallExpr, DC);

  CS.assignFixedType(CS.getType(&LHS)->getAs<TypeVariableType>(), LHSType);

  SmallVector<Solution, 1> Solutions;
  CS.solve(Solutions);

  SmallVector<OperatorResultTypes> Results;
  for (auto &S : Solutions) {
    Type RHSType;
    if (Op->getKind() == DeclKind::InfixOperator) {
      RHSType = getTypeForCompletion(S, &RHS);
    }
    Type ResultType = getTypeForCompletion(S, OpCallExpr);

    OperatorResultTypes ResultTypes = {RHSType, ResultType};
    if (llvm::is_contained(Results, ResultTypes)) {
      continue;
    }

    if (S.getFixedScore().Data[SK_ValueToOptional] > 0) {
      if (Op->getName().str() == "??" || isOptionalNilComparisonType(RHSType)) {
        // Don't suggest optional operators that need to demote the LHS to an
        // Optional to become applicable.
        continue;
      }
    }

    Results.push_back(ResultTypes);
  }

  return Results;
}

/// Adds applicable operator suggestions to \p Lookup.
static void addOperatorResults(Type LHSType, ArrayRef<OperatorDecl *> Operators,
                               DeclContext *DC, CompletionLookup &Lookup) {
  for (auto Op : Operators) {
    switch (Op->getKind()) {
    case DeclKind::PrefixOperator:
      break;
    case DeclKind::PostfixOperator:
      for (auto operatorType : getOperatorCompletionTypes(DC, LHSType, Op)) {
        Lookup.addPostfixOperatorCompletion(Op, operatorType.ResultType);
      }
      break;
    case DeclKind::InfixOperator:
      for (auto operatorType : getOperatorCompletionTypes(DC, LHSType, Op)) {
        Lookup.addInfixOperatorCompletion(Op, operatorType.ResultType,
                                          operatorType.RHSType);
      }
      break;
    default:
      llvm_unreachable("unexpected operator kind");
    }
  }
  if (LHSType->hasLValueType()) {
    Lookup.addAssignmentOperator(LHSType->getRValueType());
  }
  if (auto ValueT = LHSType->getRValueType()->getOptionalObjectType()) {
    Lookup.addPostfixBang(ValueT);
  }
}

void PostfixCompletionCallback::collectResults(
    SourceLoc DotLoc, bool IsInSelector, bool IncludeOperators,
    bool HasLeadingSpace, CodeCompletionContext &CompletionCtx) {
  ASTContext &Ctx = DC->getASTContext();
  CompletionLookup Lookup(CompletionCtx.getResultSink(), Ctx, DC,
                          &CompletionCtx);

  if (DotLoc.isValid()) {
    assert(!IncludeOperators && "We shouldn't be suggesting operators if we "
                                "are completing after a dot");
    Lookup.setHaveDot(DotLoc);
  }
  Lookup.setHaveLeadingSpace(HasLeadingSpace);

  Expr *BaseExpr = CompletionExpr->getBase();
  Lookup.setIsSuperRefExpr(isa<SuperRefExpr>(BaseExpr));

  if (auto *DRE = dyn_cast<DeclRefExpr>(BaseExpr))
    Lookup.setIsSelfRefExpr(DRE->getDecl()->getName() == Ctx.Id_self);

  if (isa<BindOptionalExpr>(BaseExpr) || isa<ForceValueExpr>(BaseExpr))
    Lookup.setIsUnwrappedOptional(true);

  if (IsInSelector) {
    Lookup.includeInstanceMembers();
    Lookup.setPreferFunctionReferencesToCalls();
  }
  SmallVector<OperatorDecl *> Operators;
  if (IncludeOperators) {
    Lookup.collectOperators(Operators);
  }

  // The type context that is being used for global results.
  ExpectedTypeContext UnifiedTypeContext;
  UnifiedTypeContext.setPreferNonVoid(true);
  bool UnifiedCanHandleAsync = false;

  // The base types of the result for which we already returned results.
  // Used so we only return keyword and operator completions once for each base
  // type.
  llvm::SmallPtrSet<Type, 2> ProcessedBaseTypes;

  Lookup.shouldCheckForDuplicates(Results.size() > 1);

  for (auto &Result : Results) {
    Lookup.setCanCurrDeclContextHandleAsync(Result.IsInAsyncContext);
    Lookup.setClosureActorIsolations(Result.ClosureActorIsolations);
    Lookup.setIsStaticMetatype(Result.BaseIsStaticMetaType);
    if (!ProcessedBaseTypes.contains(Result.BaseTy)) {
      Lookup.getPostfixKeywordCompletions(Result.BaseTy, BaseExpr);
    }
    Lookup.setExpectedTypes(Result.ExpectedTypes, Result.IsImpliedResult,
                            Result.ExpectsNonVoid);
    if (isDynamicLookup(Result.BaseTy))
      Lookup.setIsDynamicLookup();
    Lookup.getValueExprCompletions(Result.BaseTy, Result.BaseDecl,
                                   Result.IsBaseDeclUnapplied);

    // `==`, `<=` etc can be used on `Void` because `Void` is just an empty
    // tuple. But that doesn’t really make sense so we shouldn't be suggesting
    // any operators based on `Void`.
    if (IncludeOperators && !Result.BaseIsStaticMetaType &&
        !Result.BaseTy->isVoid() &&
        !ProcessedBaseTypes.contains(Result.BaseTy)) {
      addOperatorResults(Result.BaseTy, Operators, DC, Lookup);
    }

    UnifiedTypeContext.merge(*Lookup.getExpectedTypeContext());
    UnifiedCanHandleAsync |= Result.IsInAsyncContext;

    ProcessedBaseTypes.insert(Result.BaseTy);
  }

  collectCompletionResults(CompletionCtx, Lookup, DC, UnifiedTypeContext,
                           UnifiedCanHandleAsync);
}
