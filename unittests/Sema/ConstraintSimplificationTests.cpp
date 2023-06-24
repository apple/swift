//===--- ConstraintSimplificationTests.cpp --------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SemaFixture.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Types.h"
#include "swift/Sema/ConstraintSystem.h"

using namespace swift;
using namespace swift::unittest;
using namespace swift::constraints;

TEST_F(SemaTest, TestTrailingClosureMatchRecordingForIdenticalFunctions) {
  ConstraintSystem cs(DC, ConstraintSystemOptions());

  auto intType = getStdlibType("Int");
  auto floatType = getStdlibType("Float");

  auto func = FunctionType::get(
      {FunctionType::Param(intType), FunctionType::Param(intType)}, floatType);

  cs.addConstraint(
      ConstraintKind::ApplicableFunction, func, func,
      cs.getConstraintLocator({}, ConstraintLocator::ApplyFunction));

  SmallVector<Solution, 2> solutions;
  cs.solve(solutions);

  ASSERT_EQ(solutions.size(), (unsigned)1);

  const auto &solution = solutions.front();

  auto *locator = cs.getConstraintLocator({}, ConstraintLocator::ApplyArgument);
  auto choice = solution.argumentMatchingChoices.find(locator);
  ASSERT_TRUE(choice != solution.argumentMatchingChoices.end());
  MatchCallArgumentResult expected{
      TrailingClosureMatching::Forward, {{0}, {1}}, None};
  ASSERT_EQ(choice->second, expected);
}

/// Emulates code like this:
///
/// func test(_: ((Int) -> Void)?) {}
///
/// test { $0 }
///
/// To make sure that closure resolution propagates contextual
/// information into the body of the closure even when contextual
/// type is wrapped in an optional.
TEST_F(SemaTest, TestClosureInferenceFromOptionalContext) {
  ConstraintSystem cs(DC, ConstraintSystemOptions());

  DeclAttributes closureAttrs;

  // Anonymous closure parameter
  auto paramName = Context.getIdentifier("0");

  auto *paramDecl =
      new (Context) ParamDecl(/*specifierLoc=*/SourceLoc(),
                              /*argumentNameLoc=*/SourceLoc(), paramName,
                              /*parameterNameLoc=*/SourceLoc(), paramName, DC);

  paramDecl->setSpecifier(ParamSpecifier::Default);

  auto *closure = new (Context) ClosureExpr(
      closureAttrs,
      /*bracketRange=*/SourceRange(),
      /*capturedSelfDecl=*/nullptr, ParameterList::create(Context, {paramDecl}),
      /*asyncLoc=*/SourceLoc(),
      /*throwsLoc=*/SourceLoc(),
      /*arrowLoc=*/SourceLoc(),
      /*inLoc=*/SourceLoc(),
      /*explicitResultType=*/nullptr,
      /*parent=*/DC);
  closure->setDiscriminator(0);

  closure->setImplicit();

  closure->setBody(BraceStmt::create(Context, /*startLoc=*/SourceLoc(), {},
                                     /*endLoc=*/SourceLoc()),
                   /*isSingleExpression=*/false);

  auto *closureLoc = cs.getConstraintLocator(closure);

  auto *paramTy = cs.createTypeVariable(
      cs.getConstraintLocator(closure, LocatorPathElt::TupleElement(0)),
      /*options=*/TVO_CanBindToInOut);

  auto *resultTy = cs.createTypeVariable(
      cs.getConstraintLocator(closure, ConstraintLocator::ClosureResult),
      /*options=*/0);

  auto extInfo = FunctionType::ExtInfo();

  auto defaultTy = FunctionType::get({FunctionType::Param(paramTy, paramName)},
                                     resultTy, extInfo);

  cs.setClosureType(closure, defaultTy);

  auto *closureTy = cs.createTypeVariable(closureLoc, /*options=*/0);

  cs.addUnsolvedConstraint(Constraint::create(
      cs, ConstraintKind::FallbackType, closureTy, defaultTy,
      cs.getConstraintLocator(closure), /*referencedVars=*/{}));

  auto contextualTy =
      FunctionType::get({FunctionType::Param(getStdlibType("Int"))},
                        Context.TheEmptyTupleType, extInfo);

  // Try to resolve closure:
  // - external type `paramTy` should get bound to `Int`.
  // - result type should be bound to `Void`.
  cs.resolveClosure(closureTy, OptionalType::get(contextualTy), closureLoc);

  ASSERT_TRUE(cs.simplifyType(paramTy)->isEqual(getStdlibType("Int")));
  ASSERT_TRUE(cs.simplifyType(resultTy)->isEqual(Context.TheEmptyTupleType));
}
