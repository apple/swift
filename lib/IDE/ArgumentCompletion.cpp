//===--- ArgumentCompletion.cpp ---------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IDE/ArgumentCompletion.h"
#include "swift/IDE/CodeCompletion.h"
#include "swift/IDE/CompletionLookup.h"
#include "swift/IDE/SelectedOverloadInfo.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/IDETypeChecking.h"

using namespace swift;
using namespace swift::ide;
using namespace swift::constraints;

bool ArgumentTypeCheckCompletionCallback::addPossibleParams(
    const ArgumentTypeCheckCompletionCallback::Result &Res,
    SmallVectorImpl<PossibleParamInfo> &Params, SmallVectorImpl<Type> &Types) {
  if (!Res.ParamIdx || !Res.FuncTy) {
    // We don't really know much here. Suggest global results without a specific
    // expected type.
    return true;
  }

  if (Res.HasLabel) {
    // We already have a parameter label, suggest types
    Types.push_back(Res.ExpectedType);
    return true;
  }

  ArrayRef<AnyFunctionType::Param> ParamsToPass = Res.FuncTy->getParams();

  bool ShowGlobalCompletions = false;
  for (auto Idx : range(*Res.ParamIdx, ParamsToPass.size())) {
    bool IsCompletion = (Idx == Res.ParamIdx);

    // Stop at the first param claimed by other arguments.
    if (!IsCompletion && Res.ClaimedParamIndices.count(Idx) > 0) {
      break;
    }

    const AnyFunctionType::Param *TypeParam = &ParamsToPass[Idx];
    bool Required = !Res.DeclParamIsOptional[Idx];

    if (TypeParam->hasLabel() && !(IsCompletion && Res.IsNoninitialVariadic)) {
      // Suggest parameter label if parameter has label, we are completing in it
      // and it is not a variadic parameter that already has arguments
      PossibleParamInfo PP(TypeParam, Required);
      if (!llvm::is_contained(Params, PP)) {
        Params.push_back(std::move(PP));
      }
    } else {
      // We have a parameter that doesn't require a label. Suggest global
      // results for that type.
      ShowGlobalCompletions = true;
      Types.push_back(TypeParam->getPlainType());
    }
    if (Required) {
      // The user should only be suggested the first required param. Stop.
      break;
    }
  }
  return ShowGlobalCompletions;
}

/// Applies heuristic to determine whether the result type of \p E is
/// unconstrained, that is if the constraint system is satisfiable for any
/// result type of \p E.
static bool isExpressionResultTypeUnconstrained(const Solution &S, Expr *E) {
  ConstraintSystem &CS = S.getConstraintSystem();
  if (auto ParentExpr = CS.getParentExpr(E)) {
    if (auto Assign = dyn_cast<AssignExpr>(ParentExpr)) {
      if (isa<DiscardAssignmentExpr>(Assign->getDest())) {
        // _ = <expr> is unconstrained
        return true;
      }
    } else if (isa<RebindSelfInConstructorExpr>(ParentExpr)) {
      // super.init() is unconstrained (it always produces the correct result
      // by definition)
      return true;
    }
  }
  auto target = S.getTargetFor(E);
  if (!target)
    return false;

  assert(target->kind == SyntacticElementTarget::Kind::expression);
  switch (target->getExprContextualTypePurpose()) {
  case CTP_Unused:
    // If we aren't using the contextual type, its unconstrained by definition.
    return true;
  case CTP_Initialization: {
    // let x = <expr> is unconstrained
    auto contextualType = target->getExprContextualType();
    return !contextualType || contextualType->is<UnresolvedType>();
  }
  default:
    // Assume that it's constrained by default.
    return false;
  }
}

void ArgumentTypeCheckCompletionCallback::sawSolutionImpl(const Solution &S) {
  Type ExpectedTy = getTypeForCompletion(S, CompletionExpr);

  auto &CS = S.getConstraintSystem();

  Expr *ParentCall = CompletionExpr;
  while (ParentCall && ParentCall->getArgs() == nullptr) {
    ParentCall = CS.getParentExpr(ParentCall);
  }
  if (auto TV = S.getType(CompletionExpr)->getAs<TypeVariableType>()) {
    auto Locator = TV->getImpl().getLocator();
    if (Locator->isLastElement<LocatorPathElt::PatternMatch>()) {
      // The code completion token is inside a pattern, which got rewritten from
      // a call by ResolvePattern. Thus, we aren't actually inside a call.
      // Rest 'ParentCall' to nullptr to reflect that.
      ParentCall = nullptr;
    }
  }

  if (!ParentCall || ParentCall == CompletionExpr) {
    // We might not have a call that contains the code completion expression if
    // we type-checked the fallback code completion expression that only
    // contains the code completion token, but not the surrounding call.
    return;
  }

  auto ArgInfo = getCompletionArgInfo(ParentCall, CS);
  if (!ArgInfo) {
    assert(false && "bad parent call match?");
    return;
  }
  auto ArgIdx = ArgInfo->completionIdx;

  Type ExpectedCallType;
  if (!isExpressionResultTypeUnconstrained(S, ParentCall)) {
    ExpectedCallType = getTypeForCompletion(S, ParentCall);
  }

  auto *CallLocator = CS.getConstraintLocator(ParentCall);
  auto *CalleeLocator = S.getCalleeLocator(CallLocator);

  auto Info = getSelectedOverloadInfo(S, CalleeLocator);
  if (Info.getValue() && Info.getValue()->shouldHideFromEditor()) {
    return;
  }
  // Disallow invalid initializer references
  for (auto Fix : S.Fixes) {
    if (Fix->getLocator() == CalleeLocator &&
        Fix->getKind() == FixKind::AllowInvalidInitRef) {
      return;
    }
  }

  // Find the parameter the completion was bound to (if any), as well as which
  // parameters are already bound (so we don't suggest them even when the args
  // are out of order).
  llvm::Optional<unsigned> ParamIdx;
  std::set<unsigned> ClaimedParams;
  bool IsNoninitialVariadic = false;

  ConstraintLocator *ArgumentLocator;
  ArgumentLocator =
      CS.getConstraintLocator(CallLocator, ConstraintLocator::ApplyArgument);
  auto ArgMatchChoices = S.argumentMatchingChoices.find(ArgumentLocator);
  if (ArgMatchChoices != S.argumentMatchingChoices.end()) {
    // We might not have argument matching choices when applying a subscript
    // found via @dynamicMemberLookup.
    auto Bindings = ArgMatchChoices->second.parameterBindings;

    for (auto i : indices(Bindings)) {
      bool Claimed = false;
      for (auto j : Bindings[i]) {
        if (j == ArgIdx) {
          assert(!ParamIdx);
          ParamIdx = i;
          IsNoninitialVariadic = llvm::any_of(
              Bindings[i], [j](unsigned other) { return other < j; });
        }
        // Synthesized args don't count.
        if (j < ArgInfo->argCount) {
          Claimed = true;
        }
      }
      if (Claimed) {
        ClaimedParams.insert(i);
      }
    }
  }

  bool HasLabel = false;
  if (auto PE = CS.getParentExpr(CompletionExpr)) {
    if (auto Args = PE->getArgs()) {
      HasLabel = !Args->getLabel(ArgIdx).empty();
    }
  }

  bool IsAsync = isContextAsync(S, DC);

  // If this is a duplicate of any other result, ignore this solution.
  if (llvm::any_of(Results, [&](const Result &R) {
        return R.FuncD == Info.getValue() &&
               nullableTypesEqual(R.FuncTy, Info.ValueTy) &&
               nullableTypesEqual(R.BaseType, Info.BaseTy) &&
               R.ParamIdx == ParamIdx &&
               R.IsNoninitialVariadic == IsNoninitialVariadic;
      })) {
    return;
  }

  llvm::SmallDenseMap<const VarDecl *, Type> SolutionSpecificVarTypes;
  getSolutionSpecificVarTypes(S, SolutionSpecificVarTypes);

  AnyFunctionType *FuncTy = nullptr;
  if (Info.ValueTy) {
    FuncTy = Info.ValueTy->lookThroughAllOptionalTypes()->getAs<AnyFunctionType>();
  }

  // Determine which parameters are optional. We need to do this in
  // `sawSolutionImpl` because it accesses the substitution map in
  // `Info.ValueRef`. This substitution map might contain type variables that
  // are allocated in the constraint system's arena and are freed once we reach
  // `deliverResults`.
  llvm::BitVector DeclParamIsOptional;
  if (FuncTy) {
    ArrayRef<AnyFunctionType::Param> ParamsToPass = FuncTy->getParams();
    for (auto Idx : range(0, ParamsToPass.size())) {
      bool Optional = false;
      if (Info.ValueRef) {
        if (Info.ValueRef.getDecl()->isInstanceMember() &&
            !doesMemberRefApplyCurriedSelf(Info.BaseTy,
                                           Info.ValueRef.getDecl())) {
          // We are completing in an unapplied instance function, eg.
          // struct TestStatic {
          //   func method() ->  Void {}
          // }
          // TestStatic.method(#^STATIC^#)
          // The 'self' parameter is never optional, so don't enter the check
          // below (which always assumes that self has been applied).
        } else if (const ParamDecl *DeclParam =
                       getParameterAt(Info.ValueRef, Idx)) {
          Optional |= DeclParam->isDefaultArgument();
          Optional |= DeclParam->getInterfaceType()->is<PackExpansionType>();
        }
      }
      const AnyFunctionType::Param *TypeParam = &ParamsToPass[Idx];
      Optional |= TypeParam->isVariadic();
      DeclParamIsOptional.push_back(Optional);
    }
  }

  Results.push_back({ExpectedTy, ExpectedCallType,
                     isa<SubscriptExpr>(ParentCall), Info.getValue(), FuncTy,
                     ArgIdx, ParamIdx, std::move(ClaimedParams),
                     IsNoninitialVariadic, Info.BaseTy, HasLabel, IsAsync,
                     DeclParamIsOptional, SolutionSpecificVarTypes});
}

void ArgumentTypeCheckCompletionCallback::computeShadowedDecls(
    SmallPtrSetImpl<ValueDecl *> &ShadowedDecls) {
  for (size_t i = 0; i < Results.size(); ++i) {
    auto &ResultA = Results[i];
    for (size_t j = i + 1; j < Results.size(); ++j) {
      auto &ResultB = Results[j];
      if (!ResultA.FuncD || !ResultB.FuncD || !ResultA.FuncTy ||
          !ResultB.FuncTy) {
        continue;
      }
      if (ResultA.FuncD->getName() != ResultB.FuncD->getName()) {
        continue;
      }
      if (!ResultA.FuncTy->isEqual(ResultB.FuncTy)) {
        continue;
      }
      ProtocolDecl *inProtocolExtensionA =
          ResultA.FuncD->getDeclContext()->getExtendedProtocolDecl();
      ProtocolDecl *inProtocolExtensionB =
          ResultB.FuncD->getDeclContext()->getExtendedProtocolDecl();

      if (inProtocolExtensionA && !inProtocolExtensionB) {
        ShadowedDecls.insert(ResultA.FuncD);
      } else if (!inProtocolExtensionA && inProtocolExtensionB) {
        ShadowedDecls.insert(ResultB.FuncD);
      }
    }
  }
}

void ArgumentTypeCheckCompletionCallback::deliverResults(
    bool IncludeSignature, SourceLoc Loc, DeclContext *DC,
    ide::CodeCompletionContext &CompletionCtx,
    CodeCompletionConsumer &Consumer) {
  ASTContext &Ctx = DC->getASTContext();
  CompletionLookup Lookup(CompletionCtx.getResultSink(), Ctx, DC,
                          &CompletionCtx);

  SmallPtrSet<ValueDecl *, 4> ShadowedDecls;
  computeShadowedDecls(ShadowedDecls);

  // Perform global completion as a fallback if we don't have any results.
  bool shouldPerformGlobalCompletion = Results.empty();
  SmallVector<Type, 4> ExpectedCallTypes;
  for (auto &Result : Results) {
    ExpectedCallTypes.push_back(Result.ExpectedCallType);
  }

  SmallVector<Type, 8> ExpectedTypes;

  if (IncludeSignature && !Results.empty()) {
    Lookup.setHaveLParen(true);
    Lookup.setExpectedTypes(ExpectedCallTypes,
                            /*isImplicitSingleExpressionReturn=*/false);

    for (auto &Result : Results) {
      auto SemanticContext = SemanticContextKind::None;
      NominalTypeDecl *BaseNominal = nullptr;
      if (Result.BaseType) {
        Type BaseTy = Result.BaseType;
        if (auto InstanceTy = BaseTy->getMetatypeInstanceType()) {
          BaseTy = InstanceTy;
        }
        if ((BaseNominal = BaseTy->getAnyNominal())) {
          SemanticContext = SemanticContextKind::CurrentNominal;
          if (Result.FuncD &&
              Result.FuncD->getDeclContext()->getSelfNominalTypeDecl() !=
                  BaseNominal) {
            SemanticContext = SemanticContextKind::Super;
          }
        } else if (BaseTy->is<TupleType>() || BaseTy->is<SubstitutableType>()) {
          SemanticContext = SemanticContextKind::CurrentNominal;
        }
      }
      if (SemanticContext == SemanticContextKind::None && Result.FuncD) {
        if (Result.FuncD->getDeclContext()->isTypeContext()) {
          SemanticContext = SemanticContextKind::CurrentNominal;
        } else if (Result.FuncD->getDeclContext()->isLocalContext()) {
          SemanticContext = SemanticContextKind::Local;
        } else if (Result.FuncD->getModuleContext() == DC->getParentModule()) {
          SemanticContext = SemanticContextKind::CurrentModule;
        }
      }
      if (Result.FuncTy) {
        if (auto FuncTy = Result.FuncTy) {
          if (ShadowedDecls.count(Result.FuncD) == 0) {
            // Don't show call pattern completions if the function is
            // overridden.
            if (Result.IsSubscript) {
              assert(SemanticContext != SemanticContextKind::None);
              auto *SD = dyn_cast_or_null<SubscriptDecl>(Result.FuncD);
              Lookup.addSubscriptCallPattern(FuncTy, SD, SemanticContext);
            } else {
              auto *FD = dyn_cast_or_null<AbstractFunctionDecl>(Result.FuncD);
              Lookup.addFunctionCallPattern(FuncTy, FD, SemanticContext);
            }
          }
        }
      }
    }
    Lookup.setHaveLParen(false);

    shouldPerformGlobalCompletion |=
        !Lookup.FoundFunctionCalls || Lookup.FoundFunctionsWithoutFirstKeyword;
  } else if (!Results.empty()) {
    SmallVector<PossibleParamInfo, 8> Params;
    for (auto &Ret : Results) {
      shouldPerformGlobalCompletion |=
          addPossibleParams(Ret, Params, ExpectedTypes);
    }
    Lookup.addCallArgumentCompletionResults(Params);
  }

  if (shouldPerformGlobalCompletion) {
    llvm::SmallDenseMap<const VarDecl *, Type> SolutionSpecificVarTypes;
    if (!Results.empty()) {
      SolutionSpecificVarTypes = Results[0].SolutionSpecificVarTypes;
    }

    WithSolutionSpecificVarTypesRAII VarTypes(SolutionSpecificVarTypes);

    for (auto &Result : Results) {
      ExpectedTypes.push_back(Result.ExpectedType);
      Lookup.setSolutionSpecificVarTypes(Result.SolutionSpecificVarTypes);
    }
    Lookup.setExpectedTypes(ExpectedTypes, false);
    bool IsInAsyncContext = llvm::any_of(
        Results, [](const Result &Res) { return Res.IsInAsyncContext; });
    Lookup.setCanCurrDeclContextHandleAsync(IsInAsyncContext);
    Lookup.getValueCompletionsInDeclContext(Loc);
    Lookup.getSelfTypeCompletionInDeclContext(Loc, /*isForDeclResult=*/false);

    // Add any keywords that can be used in an argument expr position.
    addSuperKeyword(CompletionCtx.getResultSink(), DC);
    addExprKeywords(CompletionCtx.getResultSink(), DC);
  }

  deliverCompletionResults(CompletionCtx, Lookup, DC, Consumer);
}
