//===--- UnresolvedMemberCodeCompletion.cpp -------------------------------===//
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

#include "swift/IDE/UnresolvedMemberCompletion.h"
#include "swift/IDE/CodeCompletion.h"
#include "swift/IDE/CompletionLookup.h"
#include "swift/Sema/CompletionContextFinder.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/IDETypeChecking.h"

using namespace swift;
using namespace swift::constraints;
using namespace swift::ide;

bool UnresolvedMemberTypeCheckCompletionCallback::Result::canBeMergedWith(
    const Result &Other, DeclContext &DC) const {
  if (!isConvertibleTo(ExpectedTy, Other.ExpectedTy, /*openArchetypes=*/true,
                       DC) &&
      !isConvertibleTo(Other.ExpectedTy, ExpectedTy, /*openArchetypes=*/true,
                       DC)) {
    return false;
  }
  return true;
}

void UnresolvedMemberTypeCheckCompletionCallback::Result::merge(
    const Result &Other, DeclContext &DC) {
  assert(canBeMergedWith(Other, DC));
  if (!ExpectedTy->isEqual(Other.ExpectedTy) &&
      isConvertibleTo(ExpectedTy, Other.ExpectedTy, /*openArchetypes=*/true,
                      DC)) {
    // ExpectedTy is more general than Other.ExpectedTy. Complete based on the
    // more general type because it offers more completion options.
    ExpectedTy = Other.ExpectedTy;
  }

  IsImplicitSingleExpressionReturn |= Other.IsImplicitSingleExpressionReturn;
  IsInAsyncContext |= Other.IsInAsyncContext;
}

void UnresolvedMemberTypeCheckCompletionCallback::addExprResult(
    const Result &Res) {
  auto ExistingRes =
      llvm::find_if(ExprResults, [&Res, DC = DC](const Result &ExistingResult) {
        return ExistingResult.canBeMergedWith(Res, *DC);
      });
  if (ExistingRes != ExprResults.end()) {
    ExistingRes->merge(Res, *DC);
  } else {
    ExprResults.push_back(Res);
  }
}

void UnresolvedMemberTypeCheckCompletionCallback::sawSolutionImpl(
    const constraints::Solution &S) {
  auto &CS = S.getConstraintSystem();
  Type ExpectedTy = getTypeForCompletion(S, CompletionExpr);

  bool IsAsync = isContextAsync(S, DC);

  // If the type couldn't be determined (e.g. because there isn't any context
  // to derive it from), let's not attempt to do a lookup since it wouldn't
  // produce any useful results anyway.
  if (ExpectedTy) {
    bool SingleExprBody = isImplicitSingleExpressionReturn(CS, CompletionExpr);
    Result Res = {ExpectedTy, SingleExprBody, IsAsync};
    addExprResult(Res);
  }

  if (auto PatternType = getPatternMatchType(S, CompletionExpr)) {
    auto IsEqual = [&](const Result &R) {
      return R.ExpectedTy->isEqual(PatternType);
    };
    if (!llvm::any_of(EnumPatternTypes, IsEqual)) {
      EnumPatternTypes.push_back({PatternType,
                                  /*IsImplicitSingleExpressionReturn=*/false,
                                  IsAsync});
    }
  }
}

void UnresolvedMemberTypeCheckCompletionCallback::deliverResults(
    DeclContext *DC, SourceLoc DotLoc,
    ide::CodeCompletionContext &CompletionCtx,
    CodeCompletionConsumer &Consumer) {
  ASTContext &Ctx = DC->getASTContext();
  CompletionLookup Lookup(CompletionCtx.getResultSink(), Ctx, DC,
                          &CompletionCtx);

  assert(DotLoc.isValid());
  Lookup.setHaveDot(DotLoc);
  Lookup.shouldCheckForDuplicates(ExprResults.size() + EnumPatternTypes.size() >
                                  1);

  // Get the canonical versions of the top-level types
  SmallPtrSet<CanType, 4> originalTypes;
  for (auto &Result : ExprResults)
    originalTypes.insert(Result.ExpectedTy->getCanonicalType());

  for (auto &Result : ExprResults) {
    Lookup.setExpectedTypes({Result.ExpectedTy},
                            Result.IsImplicitSingleExpressionReturn,
                            /*expectsNonVoid*/ true);
    Lookup.setIdealExpectedType(Result.ExpectedTy);
    Lookup.setCanCurrDeclContextHandleAsync(Result.IsInAsyncContext);

    // For optional types, also get members of the unwrapped type if it's not
    // already equivalent to one of the top-level types. Handling it via the top
    // level type and not here ensures we give the correct type relation
    // (identical, rather than convertible).
    if (Result.ExpectedTy->getOptionalObjectType()) {
      Type Unwrapped = Result.ExpectedTy->lookThroughAllOptionalTypes();
      if (originalTypes.insert(Unwrapped->getCanonicalType()).second)
        Lookup.getUnresolvedMemberCompletions(Unwrapped);
    }
    Lookup.getUnresolvedMemberCompletions(Result.ExpectedTy);
  }

  // Offer completions when interpreting the pattern match as an
  // EnumElementPattern.
  for (auto &Result : EnumPatternTypes) {
    Type Ty = Result.ExpectedTy;
    Lookup.setExpectedTypes({Ty}, /*IsImplicitSingleExpressionReturn=*/false,
                            /*expectsNonVoid=*/true);
    Lookup.setIdealExpectedType(Ty);
    Lookup.setCanCurrDeclContextHandleAsync(Result.IsInAsyncContext);

    // We can pattern match MyEnum against Optional<MyEnum>
    if (Ty->getOptionalObjectType()) {
      Type Unwrapped = Ty->lookThroughAllOptionalTypes();
      Lookup.getEnumElementPatternCompletions(Unwrapped);
    }

    Lookup.getEnumElementPatternCompletions(Ty);
  }

  deliverCompletionResults(CompletionCtx, Lookup, DC, Consumer);
}
