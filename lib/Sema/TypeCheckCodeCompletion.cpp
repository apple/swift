//===--- TypeCheckCodeCompletion.cpp - Type Checking for Code Completion --===//
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
// This file implements various entry points for use by lib/IDE/.
//
//===----------------------------------------------------------------------===//

#include "CodeSynthesis.h"
#include "MiscDiagnostics.h"
#include "TypeCheckObjC.h"
#include "TypeCheckType.h"
#include "TypeChecker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Attr.h"
#include "swift/AST/DiagnosticSuppression.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/Statistic.h"
#include "swift/Parse/IDEInspectionCallbacks.h"
#include "swift/Parse/Lexer.h"
#include "swift/Sema/CompletionContextFinder.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/Twine.h"
#include <algorithm>

using namespace swift;
using namespace constraints;

/// Find the declaration directly referenced by this expression.
static std::pair<ValueDecl *, FunctionRefKind>
findReferencedDecl(Expr *expr, DeclNameLoc &loc) {
  do {
    expr = expr->getSemanticsProvidingExpr();

    if (auto ice = dyn_cast<ImplicitConversionExpr>(expr)) {
      expr = ice->getSubExpr();
      continue;
    }

    if (auto dre = dyn_cast<DeclRefExpr>(expr)) {
      loc = dre->getNameLoc();
      return { dre->getDecl(), dre->getFunctionRefKind() };
    }

    return { nullptr, FunctionRefKind::Unapplied };
  } while (true);
}

// Check if \p E is a call expression to curried thunk of "KeyPath as function".
// i.e. '{ `$kp$` in { $0[keyPath: $kp$] } }(keypath)'
static bool isKeyPathCurriedThunkCallExpr(Expr *E) {
  auto CE = dyn_cast<CallExpr>(E);
  if (!CE)
    return false;
  auto thunk = dyn_cast<AutoClosureExpr>(CE->getFn());
  if (!thunk)
    return false;
  if (thunk->getParameters()->size() != 1 ||
      thunk->getParameters()->get(0)->getParameterName().str() != "$kp$")
    return false;

  auto *unaryArg = CE->getArgs()->getUnlabeledUnaryExpr();
  if (!unaryArg)
    return false;
  return isa<KeyPathExpr>(unaryArg);
}

// Extract the keypath expression from the curried thunk expression.
static Expr *extractKeyPathFromCurryThunkCall(Expr *E) {
  assert(isKeyPathCurriedThunkCallExpr(E));
  return cast<CallExpr>(E)->getArgs()->getUnlabeledUnaryExpr();
}

namespace {

/// AST walker that "sanitizes" an expression for re-typechecking during
/// code completion.
///
/// FIXME: Remove this.
class SanitizeExpr : public ASTWalker {
  ASTContext &C;
  llvm::SmallDenseMap<OpaqueValueExpr *, Expr *, 4> OpenExistentials;

public:
  SanitizeExpr(ASTContext &C)
    : C(C) { }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Arguments;
  }

  PreWalkResult<ArgumentList *>
  walkToArgumentListPre(ArgumentList *argList) override {
    // Return the argument list to the state prior to being rewritten. This will
    // strip default arguments and expand variadic args.
    return Action::Continue(argList->getOriginalArgs());
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
    while (true) {
      // OpenExistentialExpr contains OpaqueValueExpr in its sub expression.
      if (auto OOE = dyn_cast<OpenExistentialExpr>(expr)) {
        auto archetypeVal = OOE->getOpaqueValue();
        auto base = OOE->getExistentialValue();

        bool inserted = OpenExistentials.insert({archetypeVal, base}).second;
        assert(inserted && "OpaqueValue appears multiple times?");
        (void)inserted;
        SWIFT_DEFER { OpenExistentials.erase(archetypeVal); };

        // Walk to and return the base expression to erase any existentials
        // within it.
        return Action::SkipChildren(OOE->getSubExpr()->walk(*this));
      }

      // Hacky, this behaves just like an OpenedExistential in that it changes
      // the expr tree.
      if (auto ISLE = dyn_cast<InterpolatedStringLiteralExpr>(expr)) {
        if (auto subExpr = ISLE->getAppendingExpr()->getSubExpr()) {
          if (auto opaqueValue = dyn_cast<OpaqueValueExpr>(subExpr)) {
            ISLE->getAppendingExpr()->setSubExpr(nullptr);
          }
        }
      }

      // Substitute OpaqueValue with its representing existential.
      if (auto OVE = dyn_cast<OpaqueValueExpr>(expr)) {
        auto value = OpenExistentials.find(OVE);

        if (value != OpenExistentials.end()) {
          expr = value->second;
          continue;
        } else {
          assert(OVE->isPlaceholder() &&
                 "Didn't see this OVE in a containing OpenExistentialExpr?");
        }
      }

      // Skip any implicit conversions applied to this expression.
      if (auto ICE = dyn_cast<ImplicitConversionExpr>(expr)) {
        expr = ICE->getSubExpr();
        continue;
      }

      // MakeTemporarilyEscapableExpr is typechecked expression.
      if (auto MTEE = dyn_cast<MakeTemporarilyEscapableExpr>(expr)) {
        expr = MTEE->getOriginalExpr();
        continue;
      }

      // Extract keypath from '{ `$kp$` in { $0[keyPath: $kp$] } }(keypath)'
      if (isKeyPathCurriedThunkCallExpr(expr)) {
        expr = extractKeyPathFromCurryThunkCall(expr);
        continue;
      }

      if (auto ACE = dyn_cast<AutoClosureExpr>(expr)) {
        // Restore '@autoclosure'd value.
        // This is only valid if the closure doesn't have parameters.
        if (ACE->getParameters()->size() == 0) {
          expr = ACE->getSingleExpressionBody();
          continue;
        }
        // Restore autoclosure'd function reference.
        if (auto *unwrapped = ACE->getUnwrappedCurryThunkExpr()) {
          expr = unwrapped;
          continue;
        }

        llvm_unreachable("other AutoClosureExpr must be handled specially");
      }

      // Remove any semantic expression injected by typechecking.
      if (auto EPE = dyn_cast<EditorPlaceholderExpr>(expr)) {
        EPE->setSemanticExpr(nullptr);
      }

      // If this is a closure, only walk into its children if they
      // are type-checked in the context of the enclosing expression.
      if (auto closure = dyn_cast<ClosureExpr>(expr)) {
        for (auto &Param : *closure->getParameters()) {
          Param->setSpecifier(swift::ParamSpecifier::Default);
        }
      }

      // Now, we're ready to walk into sub expressions.
      return Action::Continue(expr);
    }
  }

  PostWalkResult<Expr *> walkToExprPost(Expr *expr) override {
    assert(!isa<ImplicitConversionExpr>(expr) &&
           "ImplicitConversionExpr should be eliminated in walkToExprPre");

    auto buildMemberRef = [&](Type memberType, Expr *base, SourceLoc dotLoc,
                              ConcreteDeclRef member, DeclNameLoc memberLoc,
                              bool implicit) -> Expr * {
      auto *memberRef = new (C)
          MemberRefExpr(base, dotLoc, member, memberLoc, implicit);

      if (memberType) {
        memberRef->setType(memberType);
        return memberRef;
      }

      return memberRef;
    };

    // A DotSyntaxCallExpr is a member reference that has already been
    // type-checked down to a call; turn it back into an overloaded
    // member reference expression.
    if (auto dotCall = dyn_cast<DotSyntaxCallExpr>(expr)) {
      DeclNameLoc memberLoc;
      auto memberAndFunctionRef = findReferencedDecl(dotCall->getFn(),
                                                     memberLoc);
      if (memberAndFunctionRef.first) {
        assert(!isa<ImplicitConversionExpr>(dotCall->getBase()));
        auto *ref = buildMemberRef(dotCall->getType(),
                                   dotCall->getBase(),
                                   dotCall->getDotLoc(),
                                   memberAndFunctionRef.first,
                                   memberLoc, expr->isImplicit());
        return Action::Continue(ref);
      }
    }

    if (auto *dynamicMember = dyn_cast<DynamicMemberRefExpr>(expr)) {
      if (auto memberRef = dynamicMember->getMember()) {
        assert(!isa<ImplicitConversionExpr>(dynamicMember->getBase()));
        auto *ref = buildMemberRef(dynamicMember->getType(),
                                   dynamicMember->getBase(),
                                   dynamicMember->getDotLoc(),
                                   memberRef,
                                   dynamicMember->getNameLoc(),
                                   expr->isImplicit());
        return Action::Continue(ref);
      }
    }

    // A DotSyntaxBaseIgnoredExpr is a static member reference that has
    // already been type-checked down to a call where the argument doesn't
    // actually matter; turn it back into an overloaded member reference
    // expression.
    if (auto dotIgnored = dyn_cast<DotSyntaxBaseIgnoredExpr>(expr)) {
      DeclNameLoc memberLoc;
      auto memberAndFunctionRef = findReferencedDecl(dotIgnored->getRHS(),
                                                     memberLoc);
      if (memberAndFunctionRef.first) {
        assert(!isa<ImplicitConversionExpr>(dotIgnored->getLHS()));
        auto *ref = buildMemberRef(dotIgnored->getType(),
                                   dotIgnored->getLHS(),
                                   dotIgnored->getDotLoc(),
                                   memberAndFunctionRef.first,
                                   memberLoc, expr->isImplicit());
        return Action::Continue(ref);
      }
    }
    return Action::Continue(expr);
  }

  /// Ignore declarations.
  PreWalkAction walkToDeclPre(Decl *decl) override {
    return Action::SkipChildren();
  }
};

}  // end namespace

static Type
getTypeOfExpressionWithoutApplying(Expr *&expr, DeclContext *dc,
                                   ConcreteDeclRef &referencedDecl,
                                 FreeTypeVariableBinding allowFreeTypeVariables) {
  auto &Context = dc->getASTContext();

  expr = expr->walk(SanitizeExpr(Context));

  FrontendStatsTracer StatsTracer(Context.Stats,
                                  "typecheck-expr-no-apply", expr);
  PrettyStackTraceExpr stackTrace(Context, "type-checking", expr);
  referencedDecl = nullptr;

  ConstraintSystemOptions options;
  options |= ConstraintSystemFlags::SuppressDiagnostics;
  if (!Context.CompletionCallback) {
    options |= ConstraintSystemFlags::LeaveClosureBodyUnchecked;
  }

  // Construct a constraint system from this expression.
  ConstraintSystem cs(dc, options);

  // Attempt to solve the constraint system.
  const Type originalType = expr->getType();
  const bool needClearType = originalType && originalType->hasError();
  const auto recoverOriginalType = [&] () {
    if (needClearType)
      expr->setType(originalType);
  };

  // If the previous checking gives the expr error type, clear the result and
  // re-check.
  if (needClearType)
    expr->setType(Type());
  SyntacticElementTarget target(expr, dc, CTP_Unused, Type(),
                                /*isDiscarded=*/false);
  auto viable = cs.solve(target, allowFreeTypeVariables);
  if (!viable) {
    recoverOriginalType();
    return Type();
  }

  // Get the expression's simplified type.
  expr = target.getAsExpr();
  auto &solution = (*viable)[0];
  auto &solutionCS = solution.getConstraintSystem();
  Type exprType = solution.simplifyType(solutionCS.getType(expr));

  assert(exprType && !exprType->hasTypeVariable() &&
         "free type variable with FreeTypeVariableBinding::GenericParameters?");
  assert(exprType && !exprType->hasPlaceholder() &&
         "type placeholder with FreeTypeVariableBinding::GenericParameters?");

  if (exprType->hasError()) {
    recoverOriginalType();
    return Type();
  }

  // Dig the declaration out of the solution.
  auto semanticExpr = expr->getSemanticsProvidingExpr();
  auto topLocator = cs.getConstraintLocator(semanticExpr);
  referencedDecl = solution.resolveLocatorToDecl(topLocator);

  if (!referencedDecl.getDecl()) {
    // Do another check in case we have a curried call from binding a function
    // reference to a variable, for example:
    //
    //   class C {
    //     func instanceFunc(p1: Int, p2: Int) {}
    //   }
    //   func t(c: C) {
    //     C.instanceFunc(c)#^COMPLETE^#
    //   }
    //
    // We need to get the referenced function so we can complete the argument
    // labels. (Note that the requirement to have labels in the curried call
    // seems inconsistent with the removal of labels from function types.
    // If this changes the following code could be removed).
    if (auto *CE = dyn_cast<CallExpr>(semanticExpr)) {
      if (auto *UDE = dyn_cast<UnresolvedDotExpr>(CE->getFn())) {
        if (isa<TypeExpr>(UDE->getBase())) {
          auto udeLocator = cs.getConstraintLocator(UDE);
          auto udeRefDecl = solution.resolveLocatorToDecl(udeLocator);
          if (auto *FD = dyn_cast_or_null<FuncDecl>(udeRefDecl.getDecl())) {
            if (FD->isInstanceMember())
              referencedDecl = udeRefDecl;
          }
        }
      }
    }
  }

  // Recover the original type if needed.
  recoverOriginalType();
  return exprType;
}

static FunctionType *
getTypeOfCompletionOperatorImpl(DeclContext *DC, Expr *expr,
                                ConcreteDeclRef &referencedDecl) {
  auto &Context = DC->getASTContext();

  FrontendStatsTracer StatsTracer(Context.Stats,
                                  "typecheck-completion-operator", expr);
  PrettyStackTraceExpr stackTrace(Context, "type-checking", expr);

  expr = expr->walk(SanitizeExpr(Context));

  ConstraintSystemOptions options;
  options |= ConstraintSystemFlags::SuppressDiagnostics;

  // Construct a constraint system from this expression.
  ConstraintSystem CS(DC, options);
  expr = CS.generateConstraints(expr, DC);
  if (!expr)
    return nullptr;

  if (CS.isDebugMode()) {
    auto &log = llvm::errs();
    auto indent = CS.solverState ? CS.solverState->getCurrentIndent() : 0;
    log.indent(indent)
        << "---Initial constraints for the given expression---\n";
    expr->dump(log, indent);
    log << "\n";
    CS.print(log);
  }

  // Attempt to solve the constraint system.
  SmallVector<Solution, 4> viable;
  if (CS.solve(viable, FreeTypeVariableBinding::Disallow))
    return nullptr;

  auto &solution = viable[0];
  if (CS.isDebugMode()) {
    auto &log = llvm::errs();
    auto indent = CS.solverState ? CS.solverState->getCurrentIndent() : 0;
    log.indent(indent) << "---Solution---\n";
    solution.dump(log, indent);
  }

  // Fill the results.
  Expr *opExpr = cast<ApplyExpr>(expr)->getFn();
  referencedDecl =
      solution.resolveLocatorToDecl(CS.getConstraintLocator(opExpr));

  // Return '(ArgType[, ArgType]) -> ResultType' as a function type.
  // We don't use the type of the operator expression because we want the types
  // of the *arguments* instead of the types of the parameters.
  auto *args = cast<ApplyExpr>(expr)->getArgs();
  SmallVector<FunctionType::Param, 2> argTypes;
  for (auto arg : *args)
    argTypes.emplace_back(solution.simplifyType(CS.getType(arg.getExpr())));

  // FIXME: Verify ExtInfo state is correct, not working by accident.
  FunctionType::ExtInfo info;
  return FunctionType::get(argTypes, solution.simplifyType(CS.getType(expr)),
                           info);
}

/// Return the type of operator function for specified LHS, or a null
/// \c Type on error.
FunctionType *
TypeChecker::getTypeOfCompletionOperator(DeclContext *DC, Expr *LHS,
                                         Identifier opName, DeclRefKind refKind,
                                         ConcreteDeclRef &referencedDecl) {

  // For the infix operator, find the actual LHS from pre-folded LHS.
  if (refKind == DeclRefKind::BinaryOperator)
    LHS = TypeChecker::findLHS(DC, LHS, opName);

  if (!LHS)
    return nullptr;

  auto LHSTy = LHS->getType();

  // FIXME: 'UnresolvedType' still might be typechecked by an operator.
  if (!LHSTy || LHSTy->is<UnresolvedType>())
    return nullptr;

  // Meta types and function types cannot be a operand of operator expressions.
  if (LHSTy->is<MetatypeType>() || LHSTy->is<AnyFunctionType>())
    return nullptr;

  auto Loc = LHS->getEndLoc();

  // Build temporary expression to typecheck.
  // We allocate these expressions on the stack because we know they can't
  // escape and there isn't a better way to allocate scratch Expr nodes.

  // Use a placeholder expr for the LHS argument to avoid sending
  // a pre-type-checked AST through the constraint system.
  OpaqueValueExpr argExpr(LHS->getSourceRange(), LHSTy,
                          /*isPlaceholder=*/true);
  UnresolvedDeclRefExpr UDRE(DeclNameRef(opName), refKind, DeclNameLoc(Loc));
  auto *opExpr = TypeChecker::resolveDeclRefExpr(
      &UDRE, DC, /*replaceInvalidRefsWithErrors=*/true);

  auto &ctx = DC->getASTContext();
  switch (refKind) {
  case DeclRefKind::PostfixOperator: {
    // (postfix_unary_expr
    //   (declref_expr name=<opName>)
    //   (argument_list
    //     (<LHS>)))
    auto *postfixExpr = PostfixUnaryExpr::create(ctx, opExpr, &argExpr);
    return getTypeOfCompletionOperatorImpl(DC, postfixExpr, referencedDecl);
  }

  case DeclRefKind::BinaryOperator: {
    // (binary_expr
    //   (declref_expr name=<opName>)
    //   (argument_list
    //     (<LHS>)
    //     (code_completion_expr)))
    CodeCompletionExpr dummyRHS(Loc);
    auto *binaryExpr = BinaryExpr::create(ctx, &argExpr, opExpr, &dummyRHS,
                                          /*implicit*/ true);
    return getTypeOfCompletionOperatorImpl(DC, binaryExpr, referencedDecl);
  }

  default:
    llvm_unreachable("Invalid DeclRefKind for operator completion");
  }
}

static bool hasTypeForCompletion(Solution &solution,
                                 CompletionContextFinder &contextAnalyzer) {
  if (contextAnalyzer.hasCompletionExpr()) {
    return solution.hasType(contextAnalyzer.getCompletionExpr());
  } else {
    return solution.hasType(
        contextAnalyzer.getKeyPathContainingCompletionComponent(),
        contextAnalyzer.getKeyPathCompletionComponentIndex());
  }
}

void TypeChecker::filterSolutionsForCodeCompletion(
    SmallVectorImpl<Solution> &solutions,
    CompletionContextFinder &contextAnalyzer) {
  // Ignore solutions that didn't end up involving the completion (e.g. due to
  // a fix to skip over/ignore it).
  llvm::erase_if(solutions, [&](Solution &S) {
    if (hasTypeForCompletion(S, contextAnalyzer))
      return false;
    // FIXME: Technically this should never happen, but it currently does in
    // result builder contexts. Re-evaluate if we can assert here when we have
    // multi-statement closure checking for result builders.
    return true;
  });

  if (solutions.size() <= 1)
    return;

  Score minScore = std::min_element(solutions.begin(), solutions.end(),
                                    [](const Solution &a, const Solution &b) {
    return a.getFixedScore() < b.getFixedScore();
  })->getFixedScore();

  llvm::erase_if(solutions, [&](const Solution &S) {
    return S.getFixedScore().Data[SK_Fix] > minScore.Data[SK_Fix];
  });
}

bool TypeChecker::typeCheckForCodeCompletion(
    SyntacticElementTarget &target, bool needsPrecheck,
    llvm::function_ref<void(const Solution &)> callback) {
  auto *DC = target.getDeclContext();
  auto &Context = DC->getASTContext();
  // First of all, let's check whether given target expression
  // does indeed have the code completion location in it.
  {
    auto range = target.getSourceRange();
    if (range.isInvalid() ||
        !containsIDEInspectionTarget(range, Context.SourceMgr))
      return false;
  }

  if (getAsExpr(target.getAsASTNode())) {
    SanitizeExpr sanitizer(Context);
    target = *target.walk(sanitizer);
  }

  CompletionContextFinder contextAnalyzer(target, DC);

  // If there was no completion expr (e.g. if the code completion location was
  // among tokens that were skipped over during parser error recovery) bail.
  if (!contextAnalyzer.hasCompletion())
    return false;

  // Interpolation components are type-checked separately.
  if (contextAnalyzer.locatedInStringInterpolation())
    return false;

  // FIXME: There is currently no way to distinguish between
  // multi-statement closures which are result builder bodies
  // (that are type-checked together with enclosing context)
  // and regular closures which are type-checked separately.

  if (needsPrecheck) {
    // First, pre-check the expression, validating any types that occur in the
    // expression and folding sequence expressions.
    auto failedPreCheck =
        ConstraintSystem::preCheckTarget(target,
                                         /*replaceInvalidRefsWithErrors=*/true,
                                         /*leaveClosureBodiesUnchecked=*/true);

    if (failedPreCheck)
      return false;
  }

  enum class CompletionResult { Ok, NotApplicable, Fallback };

  auto solveForCodeCompletion =
      [&](SyntacticElementTarget &target) -> CompletionResult {
    ConstraintSystemOptions options;
    options |= ConstraintSystemFlags::AllowFixes;
    options |= ConstraintSystemFlags::SuppressDiagnostics;
    options |= ConstraintSystemFlags::ForCodeCompletion;
    if (!Context.CompletionCallback) {
      options |= ConstraintSystemFlags::LeaveClosureBodyUnchecked;
    }

    ConstraintSystem cs(DC, options);

    llvm::SmallVector<Solution, 4> solutions;

    // If solve failed to generate constraints or with some other
    // issue, we need to fallback to type-checking a sub-expression.
    cs.setTargetFor(target.getAsExpr(), target);
    if (!cs.solveForCodeCompletion(target, solutions))
      return CompletionResult::Fallback;

    // Similarly, if the type-check didn't produce any solutions, fall back
    // to type-checking a sub-expression in isolation.
    if (solutions.empty())
      return CompletionResult::Fallback;

    // FIXME: instead of filtering, expose the score and viability to clients.
    // Remove solutions that skipped over/ignored the code completion point
    // or that require fixes and have a score that is worse than the best.
    filterSolutionsForCodeCompletion(solutions, contextAnalyzer);

    llvm::for_each(solutions, callback);
    return CompletionResult::Ok;
  };

  switch (solveForCodeCompletion(target)) {
  case CompletionResult::Ok:
    return true;

  case CompletionResult::NotApplicable:
    return false;

  case CompletionResult::Fallback:
    break;
  }

  // Determine the best subexpression to use based on the collected context
  // of the code completion expression.
  if (auto fallback = contextAnalyzer.getFallbackCompletionExpr()) {
    if (auto *expr = target.getAsExpr()) {
      assert(fallback->E != expr);
      (void)expr;
    }
    SyntacticElementTarget completionTarget(fallback->E, fallback->DC,
                                            CTP_Unused,
                                            /*contextualType=*/Type(),
                                            /*isDiscarded=*/true);
    typeCheckForCodeCompletion(completionTarget, fallback->SeparatePrecheck,
                               callback);
  }
  return true;
}

static Optional<Type> getTypeOfCompletionContextExpr(
                        DeclContext *DC,
                        CompletionTypeCheckKind kind,
                        Expr *&parsedExpr,
                        ConcreteDeclRef &referencedDecl) {
  if (constraints::ConstraintSystem::preCheckExpression(
          parsedExpr, DC,
          /*replaceInvalidRefsWithErrors=*/true,
          /*leaveClosureBodiesUnchecked=*/true))
    return None;

  switch (kind) {
  case CompletionTypeCheckKind::Normal:
    // Handle below.
    break;

  case CompletionTypeCheckKind::KeyPath:
    referencedDecl = nullptr;
    if (auto keyPath = dyn_cast<KeyPathExpr>(parsedExpr)) {
      auto components = keyPath->getComponents();
      if (!components.empty()) {
        auto &last = components.back();
        if (last.isResolved()) {
          if (last.getKind() == KeyPathExpr::Component::Kind::Property)
            referencedDecl = last.getDeclRef();
          Type lookupTy = last.getComponentType();
          ASTContext &Ctx = DC->getASTContext();
          if (auto bridgedClass = Ctx.getBridgedToObjC(DC, lookupTy))
            return bridgedClass;
          return lookupTy;
        }
      }
    }

    return None;
  }

  Type originalType = parsedExpr->getType();
  if (auto T = getTypeOfExpressionWithoutApplying(parsedExpr, DC,
                 referencedDecl, FreeTypeVariableBinding::UnresolvedType))
    return T;

  // Try to recover if we've made any progress.
  if (parsedExpr &&
      !isa<ErrorExpr>(parsedExpr) &&
      parsedExpr->getType() &&
      !parsedExpr->getType()->hasError() &&
      (originalType.isNull() ||
       !parsedExpr->getType()->isEqual(originalType))) {
    return parsedExpr->getType();
  }

  return None;
}

/// Return the type of an expression parsed during code completion, or
/// a null \c Type on error.
Optional<Type> swift::getTypeOfCompletionContextExpr(
                        ASTContext &Ctx,
                        DeclContext *DC,
                        CompletionTypeCheckKind kind,
                        Expr *&parsedExpr,
                        ConcreteDeclRef &referencedDecl) {
  DiagnosticSuppression suppression(Ctx.Diags);

  // Try to solve for the actual type of the expression.
  return ::getTypeOfCompletionContextExpr(DC, kind, parsedExpr,
                                          referencedDecl);
}

/// Return the type of operator function for specified LHS, or a null
/// \c Type on error.
FunctionType *
swift::getTypeOfCompletionOperator(DeclContext *DC, Expr *LHS,
                                   Identifier opName, DeclRefKind refKind,
                                   ConcreteDeclRef &referencedDecl) {
  auto &ctx = DC->getASTContext();
  DiagnosticSuppression suppression(ctx.Diags);
  return TypeChecker::getTypeOfCompletionOperator(DC, LHS, opName, refKind,
                                                  referencedDecl);
}

bool swift::typeCheckExpression(DeclContext *DC, Expr *&parsedExpr) {
  auto &ctx = DC->getASTContext();

  parsedExpr = parsedExpr->walk(SanitizeExpr(ctx));

  DiagnosticSuppression suppression(ctx.Diags);
  auto resultTy = TypeChecker::typeCheckExpression(
      parsedExpr, DC,
      /*contextualInfo=*/{}, TypeCheckExprFlags::LeaveClosureBodyUnchecked);
  return !resultTy;
}

LookupResult
swift::lookupSemanticMember(DeclContext *DC, Type ty, DeclName name) {
  return TypeChecker::lookupMember(DC, ty, DeclNameRef(name),
                                   SourceLoc(), None);
}

