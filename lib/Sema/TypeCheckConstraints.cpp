//===--- TypeCheckConstraints.cpp - Constraint-based Type Checking --------===//
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
// This file provides high-level entry points that use constraint
// systems for type checking, as well as a few miscellaneous helper
// functions that support the constraint system.
//
//===----------------------------------------------------------------------===//

#include "MiscDiagnostics.h"
#include "TypeCheckAvailability.h"
#include "TypeChecker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticSuppression.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Statistic.h"
#include "swift/IDE/TypeCheckCompletionCallback.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/SolutionResult.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"
#include <iterator>
#include <map>
#include <memory>
#include <tuple>
#include <utility>

using namespace swift;
using namespace constraints;

//===----------------------------------------------------------------------===//
// Type variable implementation.
//===----------------------------------------------------------------------===//
#pragma mark Type variable implementation

void TypeVariableType::Implementation::print(llvm::raw_ostream &OS) {
  PrintOptions PO;
  PO.PrintTypesForDebugging = true;
  getTypeVariable()->print(OS, PO);
  
  SmallVector<TypeVariableOptions, 4> bindingOptions;
  if (canBindToLValue())
    bindingOptions.push_back(TypeVariableOptions::TVO_CanBindToLValue);
  if (canBindToInOut())
    bindingOptions.push_back(TypeVariableOptions::TVO_CanBindToInOut);
  if (canBindToNoEscape())
    bindingOptions.push_back(TypeVariableOptions::TVO_CanBindToNoEscape);
  if (canBindToHole())
    bindingOptions.push_back(TypeVariableOptions::TVO_CanBindToHole);
  if (canBindToPack())
    bindingOptions.push_back(TypeVariableOptions::TVO_CanBindToPack);
  if (isPackExpansion())
    bindingOptions.push_back(TypeVariableOptions::TVO_PackExpansion);
  if (!bindingOptions.empty()) {
    OS << " [allows bindings to: ";
    interleave(bindingOptions, OS,
               [&](TypeVariableOptions option) {
                  (OS << getTypeVariableOptions(option));},
               ", ");
               OS << "]";
  }
}

SavedTypeVariableBinding::SavedTypeVariableBinding(TypeVariableType *typeVar)
  : TypeVar(typeVar), Options(typeVar->getImpl().getRawOptions()),
    ParentOrFixed(typeVar->getImpl().ParentOrFixed) { }

void SavedTypeVariableBinding::restore() {
  TypeVar->getImpl().setRawOptions(Options);
  TypeVar->getImpl().ParentOrFixed = ParentOrFixed;
}

GenericTypeParamType *
TypeVariableType::Implementation::getGenericParameter() const {
  return locator ? locator->getGenericParameter() : nullptr;
}

llvm::Optional<ExprKind>
TypeVariableType::Implementation::getAtomicLiteralKind() const {
  if (!locator || !locator->directlyAt<LiteralExpr>())
    return llvm::None;

  auto kind = getAsExpr(locator->getAnchor())->getKind();
  switch (kind) {
  case ExprKind::IntegerLiteral:
  case ExprKind::FloatLiteral:
  case ExprKind::StringLiteral:
  case ExprKind::BooleanLiteral:
  case ExprKind::NilLiteral:
    return kind;
  default:
    return llvm::None;
  }
}

bool TypeVariableType::Implementation::isClosureType() const {
  if (!(locator && locator->getAnchor()))
    return false;

  return isExpr<ClosureExpr>(locator->getAnchor()) && locator->getPath().empty();
}

bool TypeVariableType::Implementation::isClosureParameterType() const {
  if (!(locator && locator->getAnchor()))
    return false;

  return isExpr<ClosureExpr>(locator->getAnchor()) &&
         locator->isLastElement<LocatorPathElt::TupleElement>();
}

bool TypeVariableType::Implementation::isClosureResultType() const {
  if (!(locator && locator->getAnchor()))
    return false;

  return isExpr<ClosureExpr>(locator->getAnchor()) &&
         locator->isLastElement<LocatorPathElt::ClosureResult>();
}

bool TypeVariableType::Implementation::isKeyPathType() const {
  return locator && locator->isKeyPathType();
}

bool TypeVariableType::Implementation::isSubscriptResultType() const {
  if (!(locator && locator->getAnchor()))
    return false;

  return isExpr<SubscriptExpr>(locator->getAnchor()) &&
         locator->isLastElement<LocatorPathElt::FunctionResult>();
}

bool TypeVariableType::Implementation::isParameterPack() const {
  return locator
      && locator->isForGenericParameter()
      && locator->getGenericParameter()->isParameterPack();
}

bool TypeVariableType::Implementation::isCodeCompletionToken() const {
  return locator && locator->directlyAt<CodeCompletionExpr>();
}

bool TypeVariableType::Implementation::isOpaqueType() const {
  if (!locator)
    return false;

  auto GP = locator->getLastElementAs<LocatorPathElt::GenericParameter>();
  if (!GP)
    return false;

  if (auto *GPT = GP->getType()->getAs<GenericTypeParamType>()) {
    auto *decl = GPT->getDecl();
    return decl && decl->isOpaqueType();
  }

  return false;
}

void *operator new(size_t bytes, ConstraintSystem& cs,
                   size_t alignment) {
  return cs.getAllocator().Allocate(bytes, alignment);
}

bool constraints::computeTupleShuffle(TupleType *fromTuple,
                                      TupleType *toTuple,
                                      SmallVectorImpl<unsigned> &sources) {
  const unsigned unassigned = -1;
  
  auto fromElts = fromTuple->getElements();
  auto toElts = toTuple->getElements();

  SmallVector<bool, 4> consumed(fromElts.size(), false);
  sources.clear();
  sources.assign(toElts.size(), unassigned);

  // Match up any named elements.
  for (unsigned i = 0, n = toElts.size(); i != n; ++i) {
    const auto &toElt = toElts[i];

    // Skip unnamed elements.
    if (!toElt.hasName())
      continue;

    // Find the corresponding named element.
    int matched = -1;
    {
      int index = 0;
      for (auto field : fromElts) {
        if (field.getName() == toElt.getName() && !consumed[index]) {
          matched = index;
          break;
        }
        ++index;
      }
    }
    if (matched == -1)
      continue;

    // Record this match.
    sources[i] = matched;
    consumed[matched] = true;
  }  

  // Resolve any unmatched elements.
  unsigned fromNext = 0, fromLast = fromElts.size();
  auto skipToNextAvailableInput = [&] {
    while (fromNext != fromLast && consumed[fromNext])
      ++fromNext;
  };
  skipToNextAvailableInput();

  for (unsigned i = 0, n = toElts.size(); i != n; ++i) {
    // Check whether we already found a value for this element.
    if (sources[i] != unassigned)
      continue;

    // If there aren't any more inputs, we are done.
    if (fromNext == fromLast) {
      return true;
    }

    // Otherwise, assign this input to the next output element.
    const auto &elt2 = toElts[i];

    // Fail if the input element is named and we're trying to match it with
    // something with a different label.
    if (fromElts[fromNext].hasName() && elt2.hasName())
      return true;

    sources[i] = fromNext;
    consumed[fromNext] = true;
    skipToNextAvailableInput();
  }

  // Complain if we didn't reach the end of the inputs.
  if (fromNext != fromLast) {
    return true;
  }

  // If we got here, we should have claimed all the arguments.
  assert(std::find(consumed.begin(), consumed.end(), false) == consumed.end());
  return false;
}

Expr *ConstraintLocatorBuilder::trySimplifyToExpr() const {
  SmallVector<LocatorPathElt, 4> pathBuffer;
  auto anchor = getLocatorParts(pathBuffer);
  // Locators are not guaranteed to have an anchor
  // if constraint system is used to verify generic
  // requirements.
  if (!anchor.is<Expr *>())
    return nullptr;

  ArrayRef<LocatorPathElt> path = pathBuffer;

  SourceRange range;
  simplifyLocator(anchor, path, range);
  return (path.empty() ? getAsExpr(anchor) : nullptr);
}

void ParentConditionalConformance::diagnoseConformanceStack(
    DiagnosticEngine &diags, SourceLoc loc,
    ArrayRef<ParentConditionalConformance> conformances) {
  for (auto history : llvm::reverse(conformances)) {
    diags.diagnose(loc, diag::requirement_implied_by_conditional_conformance,
                   history.ConformingType, history.Protocol->getDeclaredInterfaceType());
  }
}

namespace {
/// Produce any additional syntactic diagnostics for the body of a function
/// that had a result builder applied.
class FunctionSyntacticDiagnosticWalker : public ASTWalker {
  SmallVector<DeclContext *, 4> dcStack;

public:
  FunctionSyntacticDiagnosticWalker(DeclContext *dc) { dcStack.push_back(dc); }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
    performSyntacticExprDiagnostics(expr, dcStack.back(), /*isExprStmt=*/false);

    if (auto closure = dyn_cast<ClosureExpr>(expr)) {
      if (closure->isSeparatelyTypeChecked()) {
        dcStack.push_back(closure);
        return Action::Continue(expr);
      }
    }

    return Action::SkipChildren(expr);
  }

  PostWalkResult<Expr *> walkToExprPost(Expr *expr) override {
    if (auto closure = dyn_cast<ClosureExpr>(expr)) {
      if (closure->isSeparatelyTypeChecked()) {
        assert(dcStack.back() == closure);
        dcStack.pop_back();
      }
    }

    return Action::Continue(expr);
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *stmt) override {
    performStmtDiagnostics(stmt, dcStack.back());
    return Action::Continue(stmt);
  }

  PreWalkResult<Pattern *> walkToPatternPre(Pattern *pattern) override {
    return Action::SkipChildren(pattern);
  }
  PreWalkAction walkToTypeReprPre(TypeRepr *typeRepr) override {
    return Action::SkipChildren();
  }
  PreWalkAction walkToParameterListPre(ParameterList *params) override {
    return Action::SkipChildren();
  }
};
} // end anonymous namespace

void constraints::performSyntacticDiagnosticsForTarget(
    const SyntacticElementTarget &target, bool isExprStmt,
    bool disableExprAvailabilityChecking) {
  auto *dc = target.getDeclContext();
  switch (target.kind) {
  case SyntacticElementTarget::Kind::expression: {
    // First emit diagnostics for the main expression.
    performSyntacticExprDiagnostics(target.getAsExpr(), dc,
                                    isExprStmt, disableExprAvailabilityChecking);
    return;
  }

  case SyntacticElementTarget::Kind::forEachStmt: {
    auto *stmt = target.getAsForEachStmt();

    // First emit diagnostics for the main expression.
    performSyntacticExprDiagnostics(stmt->getTypeCheckedSequence(), dc,
                                    isExprStmt,
                                    disableExprAvailabilityChecking);

    if (auto *whereExpr = stmt->getWhere())
      performSyntacticExprDiagnostics(whereExpr, dc, /*isExprStmt*/ false);
    return;
  }

  case SyntacticElementTarget::Kind::function: {
    FunctionSyntacticDiagnosticWalker walker(dc);
    target.getFunctionBody()->walk(walker);
    return;
  }
  case SyntacticElementTarget::Kind::closure:
  case SyntacticElementTarget::Kind::stmtCondition:
  case SyntacticElementTarget::Kind::caseLabelItem:
  case SyntacticElementTarget::Kind::patternBinding:
  case SyntacticElementTarget::Kind::uninitializedVar:
    // Nothing to do for these.
    return;
  }
  llvm_unreachable("Unhandled case in switch!");
}

#pragma mark High-level entry points
Type TypeChecker::typeCheckExpression(Expr *&expr, DeclContext *dc,
                                      ContextualTypeInfo contextualInfo,
                                      TypeCheckExprOptions options) {
  SyntacticElementTarget target(
      expr, dc, contextualInfo.purpose, contextualInfo.getType(),
      options.contains(TypeCheckExprFlags::IsDiscarded));
  auto resultTarget = typeCheckExpression(target, options);
  if (!resultTarget) {
    expr = target.getAsExpr();
    return Type();
  }

  expr = resultTarget->getAsExpr();
  return expr->getType();
}

/// FIXME: In order to remote this function both \c FrontendStatsTracer
/// and \c PrettyStackTrace* have to be updated to accept `ASTNode`
// instead of each individual syntactic element types.
llvm::Optional<SyntacticElementTarget>
TypeChecker::typeCheckExpression(SyntacticElementTarget &target,
                                 TypeCheckExprOptions options) {
  DeclContext *dc = target.getDeclContext();
  auto &Context = dc->getASTContext();
  FrontendStatsTracer StatsTracer(Context.Stats, "typecheck-expr",
                                  target.getAsExpr());
  PrettyStackTraceExpr stackTrace(Context, "type-checking", target.getAsExpr());

  return typeCheckTarget(target, options);
}

llvm::Optional<SyntacticElementTarget>
TypeChecker::typeCheckTarget(SyntacticElementTarget &target,
                             TypeCheckExprOptions options) {
  DeclContext *dc = target.getDeclContext();
  auto &Context = dc->getASTContext();
  PrettyStackTraceLocation stackTrace(Context, "type-checking-target",
                                      target.getLoc());

  // First, pre-check the target, validating any types that occur in the
  // expression and folding sequence expressions.
  if (ConstraintSystem::preCheckTarget(
          target, /*replaceInvalidRefsWithErrors=*/true,
          options.contains(TypeCheckExprFlags::LeaveClosureBodyUnchecked))) {
    return llvm::None;
  }

  // Check whether given target has a code completion token which requires
  // special handling.
  if (Context.CompletionCallback &&
      typeCheckForCodeCompletion(target, /*needsPrecheck*/ false,
                                 [&](const constraints::Solution &S) {
                                   Context.CompletionCallback->sawSolution(S);
                                 }))
    return llvm::None;

  // Construct a constraint system from this expression.
  ConstraintSystemOptions csOptions = ConstraintSystemFlags::AllowFixes;

  if (DiagnosticSuppression::isEnabled(Context.Diags))
    csOptions |= ConstraintSystemFlags::SuppressDiagnostics;

  if (options.contains(TypeCheckExprFlags::LeaveClosureBodyUnchecked))
    csOptions |= ConstraintSystemFlags::LeaveClosureBodyUnchecked;

  if (options.contains(TypeCheckExprFlags::DisableMacroExpansions))
    csOptions |= ConstraintSystemFlags::DisableMacroExpansions;

  ConstraintSystem cs(dc, csOptions);

  if (auto *expr = target.getAsExpr()) {
    // Tell the constraint system what the contextual type is.  This informs
    // diagnostics and is a hint for various performance optimizations.
    cs.setContextualType(expr, target.getExprContextualTypeLoc(),
                         target.getExprContextualTypePurpose());

    // Try to shrink the system by reducing disjunction domains. This
    // goes through every sub-expression and generate its own sub-system, to
    // try to reduce the domains of those subexpressions.
    cs.shrink(expr);
    target.setExpr(expr);
  }

  // If the client can handle unresolved type variables, leave them in the
  // system.
  auto allowFreeTypeVariables = FreeTypeVariableBinding::Disallow;

  // Attempt to solve the constraint system.
  auto viable = cs.solve(target, allowFreeTypeVariables);
  if (!viable)
    return llvm::None;

  // Apply this solution to the constraint system.
  // FIXME: This shouldn't be necessary.
  auto &solution = (*viable)[0];
  cs.applySolution(solution);

  // Apply the solution to the expression.
  auto resultTarget = cs.applySolution(solution, target);
  if (!resultTarget) {
    // Failure already diagnosed, above, as part of applying the solution.
    return llvm::None;
  }

  // Unless the client has disabled them, perform syntactic checks on the
  // expression now.
  if (!cs.shouldSuppressDiagnostics()) {
    bool isExprStmt = options.contains(TypeCheckExprFlags::IsExprStmt);
    performSyntacticDiagnosticsForTarget(
        *resultTarget, isExprStmt,
        options.contains(TypeCheckExprFlags::DisableExprAvailabilityChecking));
  }

  return *resultTarget;
}

Type TypeChecker::typeCheckParameterDefault(Expr *&defaultValue,
                                            DeclContext *DC, Type paramType,
                                            bool isAutoClosure) {
  // During normal type checking we don't type check the parameter default if
  // the param has an error type. For code completion, we also type check the
  // parameter default because it might contain the code completion token.
  assert(paramType &&
         (!paramType->hasError() || DC->getASTContext().CompletionCallback));

  auto &ctx = DC->getASTContext();

  // First, let's try to type-check default expression using interface
  // type of the parameter, if that succeeds - we are done.
  SyntacticElementTarget defaultExprTarget(
      defaultValue, DC,
      isAutoClosure ? CTP_AutoclosureDefaultParameter : CTP_DefaultParameter,
      paramType, /*isDiscarded=*/false);

  auto paramInterfaceTy = paramType->mapTypeOutOfContext();

  {
    // Buffer all of the diagnostics produced by \c typeCheckExpression
    // since in some cases we need to try type-checking again with a
    // different contextual type, see below.
    DiagnosticTransaction diagnostics(ctx.Diags);

    // First, let's try to type-check default expression using
    // archetypes, which guarantees that it would work for any
    // substitution of the generic parameter (if they are involved).
    if (auto result = typeCheckExpression(defaultExprTarget)) {
      defaultValue = result->getAsExpr();
      return defaultValue->getType();
    }

    // Caller-side defaults are always type-checked based on the concrete
    // type of the argument deduced at a particular call site.
    if (isa<MagicIdentifierLiteralExpr>(defaultValue))
      return Type();

    // Parameter type doesn't have any generic parameters mentioned
    // in it, so there is nothing to infer.
    if (!paramInterfaceTy->hasTypeParameter())
      return Type();

    // Ignore any diagnostics emitted by the original type-check.
    diagnostics.abort();
  }

  // Let's see whether it would be possible to use default expression
  // for generic parameter inference.
  //
  // First, let's check whether:
  //  - Parameter type is a generic parameter; and
  //  - It's only used in the current position in the parameter list
  //    or result. This check makes sure that generic argument
  //    could only come from an explicit argument or this expression.
  //
  // If both of aforementioned conditions are true, let's attempt
  // to open generic parameter and infer the type of this default
  // expression.
  OpenedTypeMap genericParameters;

  ConstraintSystemOptions options;
  options |= ConstraintSystemFlags::AllowFixes;

  ConstraintSystem cs(DC, options);

  auto *locator = cs.getConstraintLocator(
      defaultValue, LocatorPathElt::ContextualType(
                        defaultExprTarget.getExprContextualTypePurpose()));

  auto getCanonicalGenericParamTy = [](GenericTypeParamType *GP) {
    return cast<GenericTypeParamType>(GP->getCanonicalType());
  };

  // Find and open all of the generic parameters used by the parameter
  // and replace them with type variables.
  auto contextualTy = paramInterfaceTy.transform([&](Type type) -> Type {
    assert(!type->is<UnboundGenericType>());

    if (auto *GP = type->getAs<GenericTypeParamType>()) {
      auto openedVar = genericParameters.find(getCanonicalGenericParamTy(GP));
      if (openedVar != genericParameters.end()) {
        return openedVar->second;
      }
      return cs.openGenericParameter(DC->getParent(), GP, genericParameters,
                                     locator);
    }
    return type;
  });

  auto containsTypes = [&](Type type, OpenedTypeMap &toFind) {
    return type.findIf([&](Type nested) {
      if (auto *GP = nested->getAs<GenericTypeParamType>())
        return toFind.count(getCanonicalGenericParamTy(GP)) > 0;
      return false;
    });
  };

  auto containsGenericParamsExcluding = [&](Type type,
                                            OpenedTypeMap &exclusions) -> bool {
    return type.findIf([&](Type type) {
      if (auto *GP = type->getAs<GenericTypeParamType>())
        return !exclusions.count(getCanonicalGenericParamTy(GP));
      return false;
    });
  };

  // Anchor of this default expression i.e. function, subscript
  // or enum case.
  auto *anchor = cast<ValueDecl>(DC->getParent()->getAsDecl());

  // Check whether generic parameters are only mentioned once in
  // the anchor's signature.
  {
    auto anchorTy = anchor->getInterfaceType()->castTo<GenericFunctionType>();

    // Reject if generic parameters are used in multiple different positions
    // in the parameter list.

    llvm::SmallVector<unsigned, 2> affectedParams;
    for (unsigned i : indices(anchorTy->getParams())) {
      const auto &param = anchorTy->getParams()[i];

      if (containsTypes(param.getPlainType(), genericParameters))
        affectedParams.push_back(i);
    }

    if (affectedParams.size() > 1) {
      SmallString<32> paramBuf;
      llvm::raw_svector_ostream params(paramBuf);

      interleave(
          affectedParams, [&](const unsigned index) { params << "#" << index; },
          [&] { params << ", "; });

      ctx.Diags.diagnose(
          defaultValue->getLoc(),
          diag::
              cannot_default_generic_parameter_inferrable_from_another_parameter,
          paramInterfaceTy, params.str());
      return Type();
    }
  }

  auto signature = DC->getGenericSignatureOfContext();
  assert(signature && "generic parameter without signature?");

  auto *requirementBaseLocator = cs.getConstraintLocator(
      locator, LocatorPathElt::OpenedGeneric(signature));

  // Let's check all of the requirements this parameter is involved in,
  // If it's connected to any other generic types (directly or through
  // a dependent member type), that means it could be inferred through
  // them e.g. `T: X.Y` or `T == U`.
  {
    auto recordRequirement = [&](unsigned index, Requirement requirement,
                                 ConstraintLocator *locator) {
      cs.openGenericRequirement(DC->getParent(), index, requirement,
                                /*skipSelfProtocolConstraint=*/false, locator,
                                [&](Type type) -> Type {
                                  return cs.openType(type, genericParameters,
                                                     locator);
                                });
    };

    auto diagnoseInvalidRequirement = [&](Requirement requirement) {
      SmallString<32> reqBuf;
      llvm::raw_svector_ostream req(reqBuf);

      requirement.print(req, PrintOptions());

      ctx.Diags.diagnose(
          defaultValue->getLoc(),
          diag::cannot_default_generic_parameter_invalid_requirement,
          paramInterfaceTy, req.str());
    };

    auto requirements = signature.getRequirements();
    for (unsigned reqIdx = 0; reqIdx != requirements.size(); ++reqIdx) {
      auto &requirement = requirements[reqIdx];

      switch (requirement.getKind()) {
      case RequirementKind::SameShape:
        llvm_unreachable("Same-shape requirement not supported here");

      case RequirementKind::SameType: {
        auto lhsTy = requirement.getFirstType();
        auto rhsTy = requirement.getSecondType();

        // Unrelated requirement.
        if (!containsTypes(lhsTy, genericParameters) &&
            !containsTypes(rhsTy, genericParameters))
          continue;

        // If both sides are dependent members, that's okay because types
        // don't flow from member to the base e.g. `T.Element == U.Element`.
        if (lhsTy->is<DependentMemberType>() &&
            rhsTy->is<DependentMemberType>())
          continue;

        // Allow a subset of generic same-type requirements that only mention
        // "in scope" generic parameters e.g. `T.X == Int` or `T == U.Z`
        if (!containsGenericParamsExcluding(lhsTy, genericParameters) &&
            !containsGenericParamsExcluding(rhsTy, genericParameters)) {
          recordRequirement(reqIdx, requirement, requirementBaseLocator);
          continue;
        }

        // If there is a same-type constraint that involves out of scope
        // generic parameters mixed with in-scope ones, fail the type-check
        // since the type could be inferred through other positions.
        {
          diagnoseInvalidRequirement(requirement);
          return Type();
        }
      }

      case RequirementKind::Conformance:
      case RequirementKind::Superclass:
      case RequirementKind::Layout:
        auto adheringTy = requirement.getFirstType();

        // Unrelated requirement.
        if (!containsTypes(adheringTy, genericParameters))
          continue;

        // If adhering type has a mix or in- and out-of-scope parameters
        // mentioned we need to diagnose.
        if (containsGenericParamsExcluding(adheringTy, genericParameters)) {
          diagnoseInvalidRequirement(requirement);
          return Type();
        }

        if (requirement.getKind() == RequirementKind::Superclass) {
          auto superclassTy = requirement.getSecondType();

          if (containsGenericParamsExcluding(superclassTy, genericParameters)) {
            diagnoseInvalidRequirement(requirement);
            return Type();
          }
        }

        recordRequirement(reqIdx, requirement, requirementBaseLocator);
        break;
      }
    }
  }

  defaultExprTarget.setExprConversionType(contextualTy);
  cs.setContextualType(defaultValue,
                       defaultExprTarget.getExprContextualTypeLoc(),
                       defaultExprTarget.getExprContextualTypePurpose());

  auto viable = cs.solve(defaultExprTarget, FreeTypeVariableBinding::Disallow);
  if (!viable)
    return Type();

  auto &solution = (*viable)[0];

  cs.applySolution(solution);

  if (auto result = cs.applySolution(solution, defaultExprTarget)) {
    // Perform syntactic diagnostics on the type-checked target.
    performSyntacticDiagnosticsForTarget(*result, /*isExprStmt=*/false);
    defaultValue = result->getAsExpr();
    return defaultValue->getType();
  }

  return Type();
}

bool TypeChecker::typeCheckBinding(Pattern *&pattern, Expr *&initializer,
                                   DeclContext *DC, Type patternType,
                                   PatternBindingDecl *PBD,
                                   unsigned patternNumber,
                                   TypeCheckExprOptions options) {
  SyntacticElementTarget target =
      PBD ? SyntacticElementTarget::forInitialization(
                initializer, DC, patternType, PBD, patternNumber,
                /*bindPatternVarsOneWay=*/false)
          : SyntacticElementTarget::forInitialization(
                initializer, DC, patternType, pattern,
                /*bindPatternVarsOneWay=*/false);

  // Type-check the initializer.
  auto resultTarget = typeCheckExpression(target, options);

  if (resultTarget) {
    initializer = resultTarget->getAsExpr();
    pattern = resultTarget->getInitializationPattern();
    return false;
  }

  auto &Context = DC->getASTContext();
  initializer = target.getAsExpr();

  if (!initializer->getType())
    initializer->setType(ErrorType::get(Context));

  // Assign error types to the pattern and its variables, to prevent it from
  // being referenced by the constraint system.
  if (patternType->hasUnresolvedType() ||
      patternType->hasPlaceholder() ||
      patternType->hasUnboundGenericType()) {
    pattern->setType(ErrorType::get(Context));
  }

  pattern->forEachVariable([&](VarDecl *var) {
    // Don't change the type of a variable that we've been able to
    // compute a type for.
    if (var->hasInterfaceType() &&
        !var->getType()->hasUnboundGenericType() &&
        !var->isInvalid())
      return;

    var->setInvalid();
  });
  return true;
}

bool TypeChecker::typeCheckPatternBinding(PatternBindingDecl *PBD,
                                          unsigned patternNumber,
                                          Type patternType,
                                          TypeCheckExprOptions options) {
  Pattern *pattern = PBD->getPattern(patternNumber);
  Expr *init = PBD->getInit(patternNumber);

  // Enter an initializer context if necessary.
  PatternBindingInitializer *initContext = nullptr;
  DeclContext *DC = PBD->getDeclContext();
  if (!DC->isLocalContext()) {
    initContext = cast_or_null<PatternBindingInitializer>(
        PBD->getInitContext(patternNumber));
    if (initContext)
      DC = initContext;
  }

  // If we weren't given a pattern type, compute one now.
  if (!patternType) {
    if (pattern->hasType())
      patternType = pattern->getType();
    else {
      auto contextualPattern = ContextualPattern::forRawPattern(pattern, DC);
      patternType = typeCheckPattern(contextualPattern);
    }

    if (patternType->hasError()) {
      PBD->setInvalid();
      return true;
    }
  }

  bool hadError = TypeChecker::typeCheckBinding(pattern, init, DC, patternType,
                                                PBD, patternNumber, options);
  if (!init) {
    PBD->setInvalid();
    return true;
  }

  PBD->setPattern(patternNumber, pattern, initContext);
  PBD->setInit(patternNumber, init);

  if (hadError)
    PBD->setInvalid();
  PBD->setInitializerChecked(patternNumber);
  return hadError;
}

bool TypeChecker::typeCheckForEachBinding(DeclContext *dc, ForEachStmt *stmt) {
  auto &Context = dc->getASTContext();
  FrontendStatsTracer statsTracer(Context.Stats, "typecheck-for-each", stmt);
  PrettyStackTraceStmt stackTrace(Context, "type-checking-for-each", stmt);

  auto failed = [&]() -> bool {
    // Invalidate the pattern and the var decl.
    stmt->getPattern()->setType(ErrorType::get(Context));
    stmt->getPattern()->forEachVariable([&](VarDecl *var) {
      if (var->hasInterfaceType() && !var->isInvalid())
        return;
      var->setInvalid();
    });
    return true;
  };

  auto target = SyntacticElementTarget::forForEachStmt(
      stmt, dc, /*bindPatternVarsOneWay=*/false);
  if (!typeCheckTarget(target))
    return failed();

  // Check to see if the sequence expr is throwing (in async context),
  // if so require the stmt to have a `try`.
  if (diagnoseUnhandledThrowsInAsyncContext(dc, stmt))
    return failed();

  return false;
}

bool TypeChecker::typeCheckCondition(Expr *&expr, DeclContext *dc) {
  // If this expression is already typechecked and has type Bool, then just
  // re-typecheck it.
  if (expr->getType() && expr->getType()->isBool()) {
    auto resultTy =
        TypeChecker::typeCheckExpression(expr, dc);
    return !resultTy;
  }

  auto *boolDecl = dc->getASTContext().getBoolDecl();
  if (!boolDecl)
    return true;

  auto resultTy = TypeChecker::typeCheckExpression(
      expr, dc,
      /*contextualInfo=*/{boolDecl->getDeclaredInterfaceType(), CTP_Condition});
  return !resultTy;
}

/// Find the `~=` operator that can compare an expression inside a pattern to a
/// value of a given type.
bool TypeChecker::typeCheckExprPattern(ExprPattern *EP, DeclContext *DC,
                                       Type rhsType) {
  auto &Context = DC->getASTContext();
  FrontendStatsTracer StatsTracer(Context.Stats,
                                  "typecheck-expr-pattern", EP);
  PrettyStackTracePattern stackTrace(Context, "type-checking", EP);

  EP->getMatchVar()->setInterfaceType(rhsType->mapTypeOutOfContext());

  // Check the expression as a condition.
  auto target = SyntacticElementTarget::forExprPattern(EP);
  auto result = typeCheckExpression(target);
  if (!result)
    return true;

  // Save the type-checked expression in the pattern.
  EP->setMatchExpr(result->getAsExpr());
  // Set the type on the pattern.
  EP->setType(rhsType);
  return false;
}

static Type replaceArchetypesWithTypeVariables(ConstraintSystem &cs,
                                               Type t) {
  llvm::DenseMap<SubstitutableType *, TypeVariableType *> types;

  return t.subst(
    [&](SubstitutableType *origType) -> Type {
      auto found = types.find(origType);
      if (found != types.end())
        return found->second;

      if (auto archetypeType = dyn_cast<ArchetypeType>(origType)) {
        auto root = archetypeType->getRoot();
        // For other nested types, fail here so the default logic in subst()
        // for nested types applies.
        if (root != archetypeType)
          return Type();

        auto locator = cs.getConstraintLocator({});
        auto replacement = cs.createTypeVariable(locator,
                                                 TVO_CanBindToNoEscape);

        if (auto superclass = archetypeType->getSuperclass()) {
          cs.addConstraint(ConstraintKind::Subtype, replacement,
                           superclass, locator);
        }
        for (auto proto : archetypeType->getConformsTo()) {
          cs.addConstraint(ConstraintKind::ConformsTo, replacement,
                           proto->getDeclaredInterfaceType(), locator);
        }
        types[origType] = replacement;
        return replacement;
      }

      // FIXME: Remove this case
      assert(cast<GenericTypeParamType>(origType));
      auto locator = cs.getConstraintLocator({});
      auto replacement = cs.createTypeVariable(locator,
                                               TVO_CanBindToNoEscape);
      types[origType] = replacement;
      return replacement;
    },
    MakeAbstractConformanceForGenericType(),
    SubstFlags::SubstituteOpaqueArchetypes);
}

bool TypeChecker::typesSatisfyConstraint(Type type1, Type type2,
                                         bool openArchetypes,
                                         ConstraintKind kind, DeclContext *dc,
                                         bool *unwrappedIUO) {
  assert(!type1->hasTypeVariable() && !type2->hasTypeVariable() &&
         "Unexpected type variable in constraint satisfaction testing");

  ConstraintSystem cs(dc, ConstraintSystemOptions());
  if (openArchetypes) {
    type1 = replaceArchetypesWithTypeVariables(cs, type1);
    type2 = replaceArchetypesWithTypeVariables(cs, type2);
  }

  cs.addConstraint(kind, type1, type2, cs.getConstraintLocator({}));

  if (openArchetypes) {
    assert(!unwrappedIUO && "FIXME");
    SmallVector<Solution, 4> solutions;
    return !cs.solve(solutions, FreeTypeVariableBinding::Allow);
  }

  if (auto solution = cs.solveSingle()) {
    if (unwrappedIUO)
      *unwrappedIUO = solution->getFixedScore().Data[SK_ForceUnchecked] > 0;

    return true;
  }

  return false;
}

bool TypeChecker::isSubtypeOf(Type type1, Type type2, DeclContext *dc) {
  return typesSatisfyConstraint(type1, type2,
                                /*openArchetypes=*/false,
                                ConstraintKind::Subtype, dc);
}

bool TypeChecker::isConvertibleTo(Type type1, Type type2, DeclContext *dc,
                                  bool *unwrappedIUO) {
  return typesSatisfyConstraint(type1, type2,
                                /*openArchetypes=*/false,
                                ConstraintKind::Conversion, dc,
                                unwrappedIUO);
}

bool TypeChecker::isExplicitlyConvertibleTo(Type type1, Type type2,
                                            DeclContext *dc) {
  return (typesSatisfyConstraint(type1, type2,
                                 /*openArchetypes=*/false,
                                 ConstraintKind::Conversion, dc) ||
          isObjCBridgedTo(type1, type2, dc));
}

bool TypeChecker::isObjCBridgedTo(Type type1, Type type2, DeclContext *dc,
                                  bool *unwrappedIUO) {
  return (typesSatisfyConstraint(type1, type2,
                                 /*openArchetypes=*/false,
                                 ConstraintKind::BridgingConversion,
                                 dc, unwrappedIUO));
}

bool TypeChecker::checkedCastMaySucceed(Type t1, Type t2, DeclContext *dc) {
  auto kind = TypeChecker::typeCheckCheckedCast(
      t1, t2, CheckedCastContextKind::None, dc);
  return (kind != CheckedCastKind::Unresolved);
}

Expr *
TypeChecker::addImplicitLoadExpr(ASTContext &Context, Expr *expr,
                                 std::function<Type(Expr *)> getType,
                                 std::function<void(Expr *, Type)> setType) {
  class LoadAdder : public ASTWalker {
  private:
    using GetTypeFn = std::function<Type(Expr *)>;
    using SetTypeFn = std::function<void(Expr *, Type)>;

    ASTContext &Ctx;
    GetTypeFn getType;
    SetTypeFn setType;

  public:
    LoadAdder(ASTContext &ctx, GetTypeFn getType, SetTypeFn setType)
        : Ctx(ctx), getType(getType), setType(setType) {}

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      if (isa<ParenExpr>(E) || isa<ForceValueExpr>(E))
        return Action::Continue(E);

      // Since load expression is created by walker,
      // it's safe to stop as soon as it encounters first one
      // because it would be the one it just created.
      if (isa<LoadExpr>(E))
        return Action::Stop();

      return Action::SkipChildren(createLoadExpr(E));
    }

    PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
      if (auto *FVE = dyn_cast<ForceValueExpr>(E))
        setType(E, getType(FVE->getSubExpr())->getOptionalObjectType());

      if (auto *PE = dyn_cast<ParenExpr>(E))
        setType(E, ParenType::get(Ctx, getType(PE->getSubExpr())));

      return Action::Continue(E);
    }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::ArgumentsAndExpansion;
    }

  private:
    LoadExpr *createLoadExpr(Expr *E) {
      auto objectType = getType(E)->getRValueType();
      auto *LE = new (Ctx) LoadExpr(E, objectType);
      setType(LE, objectType);
      return LE;
    }
  };

  return expr->walk(LoadAdder(Context, getType, setType));
}

Expr *
TypeChecker::coerceToRValue(ASTContext &Context, Expr *expr,
                            llvm::function_ref<Type(Expr *)> getType,
                            llvm::function_ref<void(Expr *, Type)> setType) {
  Type exprTy = getType(expr);

  // If expr has no type, just assume it's the right expr.
  if (!exprTy)
    return expr;

  // If the type is already materializable, then we're already done.
  if (!exprTy->hasLValueType())
    return expr;

  // Walk into force optionals and coerce the source.
  if (auto *FVE = dyn_cast<ForceValueExpr>(expr)) {
    auto sub = coerceToRValue(Context, FVE->getSubExpr(), getType, setType);
    FVE->setSubExpr(sub);
    setType(FVE, getType(sub)->getOptionalObjectType());
    return FVE;
  }

  // Walk into parenthesized expressions to update the subexpression.
  if (auto paren = dyn_cast<IdentityExpr>(expr)) {
    auto sub =  coerceToRValue(Context, paren->getSubExpr(), getType, setType);
    paren->setSubExpr(sub);
    setType(paren, ParenType::get(Context, getType(sub)));
    return paren;
  }

  // Walk into 'try' and 'try!' expressions to update the subexpression.
  if (auto tryExpr = dyn_cast<AnyTryExpr>(expr)) {
    auto sub = coerceToRValue(Context, tryExpr->getSubExpr(), getType, setType);
    tryExpr->setSubExpr(sub);
    if (isa<OptionalTryExpr>(tryExpr) && !getType(sub)->hasError())
      setType(tryExpr, OptionalType::get(getType(sub)));
    else
      setType(tryExpr, getType(sub));
    return tryExpr;
  }

  // Walk into tuples to update the subexpressions.
  if (auto tuple = dyn_cast<TupleExpr>(expr)) {
    bool anyChanged = false;
    for (auto &elt : tuple->getElements()) {
      // Materialize the element.
      auto oldType = getType(elt);
      elt = coerceToRValue(Context, elt, getType, setType);

      // If the type changed at all, make a note of it.
      if (getType(elt).getPointer() != oldType.getPointer()) {
        anyChanged = true;
      }
    }

    // If any of the types changed, rebuild the tuple type.
    if (anyChanged) {
      SmallVector<TupleTypeElt, 4> elements;
      elements.reserve(tuple->getElements().size());
      for (unsigned i = 0, n = tuple->getNumElements(); i != n; ++i) {
        Type type = getType(tuple->getElement(i));
        Identifier name = tuple->getElementName(i);
        elements.push_back(TupleTypeElt(type, name));
      }
      setType(tuple, TupleType::get(elements, Context));
    }

    return tuple;
  }

  // Load lvalues.
  if (exprTy->is<LValueType>())
    return addImplicitLoadExpr(Context, expr, getType, setType);

  // Nothing to do.
  return expr;
}

//===----------------------------------------------------------------------===//
// Debugging
//===----------------------------------------------------------------------===//
#pragma mark Debugging

void OverloadChoice::dump(Type adjustedOpenedType, SourceManager *sm,
                          raw_ostream &out) const {
  PrintOptions PO;
  PO.PrintTypesForDebugging = true;
  out << " with ";

  switch (getKind()) {
  case OverloadChoiceKind::Decl:
  case OverloadChoiceKind::DeclViaDynamic:
  case OverloadChoiceKind::DeclViaBridge:
  case OverloadChoiceKind::DeclViaUnwrappedOptional:
    getDecl()->dumpRef(out);
    out << " as ";
    if (getBaseType())
      out << getBaseType()->getString(PO) << ".";

    out << getDecl()->getBaseName() << ": "
        << adjustedOpenedType->getString(PO);
    break;

  case OverloadChoiceKind::KeyPathApplication:
    out << "key path application root " << getBaseType()->getString(PO);
    break;

  case OverloadChoiceKind::DynamicMemberLookup:
  case OverloadChoiceKind::KeyPathDynamicMemberLookup:
    out << "dynamic member lookup root " << getBaseType()->getString(PO)
        << " name='" << getName();
    break;

  case OverloadChoiceKind::TupleIndex:
    out << "tuple " << getBaseType()->getString(PO) << " index "
        << getTupleIndex();
    break;

  case OverloadChoiceKind::MaterializePack:
    out << "materialize pack from tuple " << getBaseType()->getString(PO);
    break;
  }
}

void Solution::dump() const { dump(llvm::errs(), 0); }

void Solution::dump(raw_ostream &out, unsigned indent) const {
  PrintOptions PO;
  PO.PrintTypesForDebugging = true;

  SourceManager *sm = &getConstraintSystem().getASTContext().SourceMgr;

  out.indent(indent) << "Fixed score:";
  FixedScore.print(out);

  out << "\n";
  out.indent(indent) << "Type variables:\n";
  std::vector<std::pair<TypeVariableType *, Type>> bindings(
      typeBindings.begin(), typeBindings.end());
  llvm::sort(bindings, [](const std::pair<TypeVariableType *, Type> &lhs,
                          const std::pair<TypeVariableType *, Type> &rhs) {
    return lhs.first->getID() < rhs.first->getID();
  });
  for (auto binding : bindings) {
    auto &typeVar = binding.first;
    out.indent(indent + 2);
    Type(typeVar).print(out, PO);
    out << " as ";
    binding.second.print(out, PO);
    if (auto *locator = typeVar->getImpl().getLocator()) {
      out << " @ ";
      locator->dump(sm, out);
    }
    out << "\n";
  }

  if (!overloadChoices.empty()) {
    out << "\n";
    out.indent(indent) << "Overload choices:";
    for (auto ovl : overloadChoices) {
      if (ovl.first) {
        out << "\n";
        out.indent(indent + 2);
        ovl.first->dump(sm, out);
      }

      auto choice = ovl.second.choice;
      choice.dump(ovl.second.adjustedOpenedType, sm, out);
    }
    out << "\n";
  }


  if (!ConstraintRestrictions.empty()) {
    out.indent(indent) << "Constraint restrictions:\n";
    for (auto &restriction : ConstraintRestrictions) {
      out.indent(indent + 2)
          << restriction.first.first << " to " << restriction.first.second
          << " is " << getName(restriction.second) << "\n";
    }
  }

  if (!argumentMatchingChoices.empty()) {
    out.indent(indent) << "Trailing closure matching:\n";
    for (auto &argumentMatching : argumentMatchingChoices) {
      out.indent(indent + 2);
      argumentMatching.first->dump(sm, out);
      switch (argumentMatching.second.trailingClosureMatching) {
      case TrailingClosureMatching::Forward:
        out << ": forward\n";
        break;
      case TrailingClosureMatching::Backward:
        out << ": backward\n";
        break;
      }
    }
  }

  if (!DisjunctionChoices.empty()) {
    out.indent(indent) << "Disjunction choices:\n";
    for (auto &choice : DisjunctionChoices) {
      out.indent(indent + 2);
      choice.first->dump(sm, out);
      out << " is #" << choice.second << "\n";
    }
  }

  if (!OpenedTypes.empty()) {
    out.indent(indent) << "Opened types:\n";
    for (const auto &opened : OpenedTypes) {
      out.indent(indent + 2);
      opened.first->dump(sm, out);
      out << " opens ";
      llvm::interleave(
          opened.second.begin(), opened.second.end(),
          [&](OpenedType opened) {
            out << "'";
            opened.second->getImpl().getGenericParameter()->print(out);
            out << "' (";
            Type(opened.first).print(out, PO);
            out << ")";
            out << " -> ";
            // Let's check whether the type variable has been bound.
            // This is important when solver is working on result
            // builder transformed code, because dependent sub-components
            // would not have parent type variables but `OpenedTypes`
            // cannot be erased, so we'll just print them as unbound.
            if (hasFixedType(opened.second)) {
              out << getFixedType(opened.second);
              out << " [from ";
              Type(opened.second).print(out, PO);
              out << "]";
            } else {
              Type(opened.second).print(out, PO);
            }
          },
          [&]() { out << ", "; });
      out << "\n";
    }
  }

  if (!OpenedExistentialTypes.empty()) {
    out.indent(indent) << "Opened existential types:\n";
    for (const auto &openedExistential : OpenedExistentialTypes) {
      out.indent(indent + 2);
      openedExistential.first->dump(sm, out);
      out << " opens to " << openedExistential.second->getString(PO);
      out << "\n";
    }
  }

  if (!DefaultedConstraints.empty()) {
    out.indent(indent) << "Defaulted constraints: ";
    interleave(DefaultedConstraints, [&](ConstraintLocator *locator) {
      locator->dump(sm, out);
    }, [&] {
      out << ", ";
    });
    out << "\n";
  }

  if (!Fixes.empty()) {
    out.indent(indent) << "Fixes:\n";
    for (auto *fix : Fixes) {
      out.indent(indent + 2);
      fix->print(out);
      out << "\n";
    }
  }
}

void ConstraintSystem::dump() const {
  print(llvm::errs());
}

void ConstraintSystem::dump(Expr *E) const {
  print(llvm::errs(), E);
}

void ConstraintSystem::print(raw_ostream &out, Expr *E) const {
  auto getTypeOfExpr = [&](Expr *E) -> Type {
    if (hasType(E))
      return getType(E);
    return Type();
  };
  auto getTypeOfTypeRepr = [&](TypeRepr *TR) -> Type {
    if (hasType(TR))
      return getType(TR);
    return Type();
  };
  auto getTypeOfKeyPathComponent = [&](KeyPathExpr *KP, unsigned I) -> Type {
    if (hasType(KP, I))
      return getType(KP, I);
    return Type();
  };

  E->dump(out, getTypeOfExpr, getTypeOfTypeRepr, getTypeOfKeyPathComponent,
          solverState ? solverState->getCurrentIndent() : 0);
  out << "\n";
}

void ConstraintSystem::print(raw_ostream &out) const {
  // Print all type variables as $T0 instead of _ here.
  PrintOptions PO;
  PO.PrintTypesForDebugging = true;

  auto indent = solverState ? solverState->getCurrentIndent() : 0;
  out.indent(indent) << "Score:";
  CurrentScore.print(out);

  for (const auto &contextualTypeEntry : contextualTypes) {
    auto info = contextualTypeEntry.second.first;
    if (!info.getType().isNull()) {
      out << "\n";
      out.indent(indent) << "Contextual Type: " << info.getType().getString(PO);
      out << " at ";

      auto &SM = getASTContext().SourceMgr;
      if (TypeRepr *TR = info.typeLoc.getTypeRepr()) {
        TR->getSourceRange().print(out, SM, /*text*/ false);
      } else {
        dumpAnchor(contextualTypeEntry.first, &SM, out);
      }
    }
  }

  out << "\n";
  out.indent(indent) << "Type Variables:\n";
  std::vector<TypeVariableType *> typeVariables(getTypeVariables().begin(),
                                                getTypeVariables().end());
  llvm::sort(typeVariables,
             [](const TypeVariableType *lhs, const TypeVariableType *rhs) {
               return lhs->getID() < rhs->getID();
             });
  for (auto tv : typeVariables) {
    out.indent(indent + 2);
    auto rep = getRepresentative(tv);
    if (rep == tv) {
      if (auto fixed = getFixedType(tv)) {
        tv->print(out, PO);
        out << " as ";
        Type(fixed).print(out, PO);
      } else {
        const_cast<ConstraintSystem *>(this)->getBindingsFor(tv).dump(out, 1);
      }
    } else {
      tv->print(out, PO);
      out << " equivalent to ";
      Type(rep).print(out, PO);
    }

    if (auto *locator = tv->getImpl().getLocator()) {
      out << " @ ";
      locator->dump(&getASTContext().SourceMgr, out);
    }

    out << "\n";
  }

  if (!ActiveConstraints.empty()) {
    out.indent(indent) << "Active Constraints:\n";
    for (auto &constraint : ActiveConstraints) {
      out.indent(indent + 2);
      constraint.print(out, &getASTContext().SourceMgr);
      out << "\n";
    }
  }

  if (!InactiveConstraints.empty()) {
    out.indent(indent) << "Inactive Constraints:\n";
    for (auto &constraint : InactiveConstraints) {
      out.indent(indent + 2);
      constraint.print(out, &getASTContext().SourceMgr);
      out << "\n";
    }
  }

  if (solverState && solverState->hasRetiredConstraints()) {
    out.indent(indent) << "Retired Constraints:\n";
    solverState->forEachRetired([&](Constraint &constraint) {
      out.indent(indent + 2);
      constraint.print(out, &getASTContext().SourceMgr);
      out << "\n";
    });
  }

  if (!ResolvedOverloads.empty()) {
    out.indent(indent) << "Resolved overloads:\n";

    // Otherwise, report the resolved overloads.
    for (auto elt : ResolvedOverloads) {
      auto resolved = elt.second;
      auto &choice = resolved.choice;
      out << "  selected overload set choice ";
      switch (choice.getKind()) {
      case OverloadChoiceKind::Decl:
      case OverloadChoiceKind::DeclViaDynamic:
      case OverloadChoiceKind::DeclViaBridge:
      case OverloadChoiceKind::DeclViaUnwrappedOptional:
        if (choice.getBaseType())
          out << choice.getBaseType()->getString(PO) << ".";
        out << choice.getDecl()->getBaseName() << ": "
            << resolved.boundType->getString(PO) << " == "
            << resolved.adjustedOpenedType->getString(PO);
        break;

      case OverloadChoiceKind::KeyPathApplication:
        out << "key path application root "
            << choice.getBaseType()->getString(PO);
        break;

      case OverloadChoiceKind::DynamicMemberLookup:
      case OverloadChoiceKind::KeyPathDynamicMemberLookup:
        out << "dynamic member lookup: "
            << choice.getBaseType()->getString(PO) << " name="
            << choice.getName();
        break;

      case OverloadChoiceKind::TupleIndex:
        out << "tuple " << choice.getBaseType()->getString(PO) << " index "
            << choice.getTupleIndex();
        break;

      case OverloadChoiceKind::MaterializePack:
        out << "materialize pack from tuple "
            << choice.getBaseType()->getString(PO);
        break;
      }
      out << " for ";
      elt.first->dump(&getASTContext().SourceMgr, out);
      out << "\n";
    }
  }

  if (!DisjunctionChoices.empty()) {
    out.indent(indent) << "Disjunction choices:\n";
    for (auto &choice : DisjunctionChoices) {
      out.indent(indent + 2);
      choice.first->dump(&getASTContext().SourceMgr, out);
      out << " is #" << choice.second << "\n";
    }
  }

  if (!OpenedTypes.empty()) {
    out.indent(indent) << "Opened types:\n";
    for (const auto &opened : OpenedTypes) {
      out.indent(indent + 2);
      opened.first->dump(&getASTContext().SourceMgr, out);
      out << " opens ";
      llvm::interleave(
          opened.second.begin(), opened.second.end(),
          [&](OpenedType opened) {
            out << "'";
            opened.second->getImpl().getGenericParameter()->print(out);
            out << "' (";
            Type(opened.first).print(out, PO);
            out << ")";
            out << " -> ";
            Type(opened.second).print(out, PO);
          },
          [&]() { out << ", "; });
      out << "\n";
    }
  }

  if (!OpenedExistentialTypes.empty()) {
    out.indent(indent) << "Opened existential types:\n";
    for (const auto &openedExistential : OpenedExistentialTypes) {
      out.indent(indent + 2);
      openedExistential.first->dump(&getASTContext().SourceMgr, out);
      out << " opens to " << openedExistential.second->getString(PO);
      out << "\n";
    }
  }

  if (!PackExpansionEnvironments.empty()) {
    out.indent(indent) << "Pack Expansion Environments:\n";
    for (const auto &env : PackExpansionEnvironments) {
      out.indent(indent + 2);
      env.first->dump(&getASTContext().SourceMgr, out);
      out << " = (" << env.second.first << ", "
          << env.second.second->getString(PO) << ")" << '\n';
    }
  }

  if (!OpenedPackExpansionTypes.empty()) {
    out.indent(indent) << "Opened pack expansion types:\n";
    for (const auto &expansion : OpenedPackExpansionTypes) {
      out.indent(indent + 2);
      out << expansion.first->getString(PO);
      out << " opens to " << expansion.second->getString(PO);
      out << "\n";
    }
  }

  if (!DefaultedConstraints.empty()) {
    out.indent(indent) << "Defaulted constraints:\n";
    interleave(DefaultedConstraints, [&](ConstraintLocator *locator) {
      locator->dump(&getASTContext().SourceMgr, out);
    }, [&] {
      out << ", ";
    });
    out << "\n";
  }

  if (failedConstraint) {
    out.indent(indent) << "Failed constraint:\n";
    failedConstraint->print(out.indent(indent + 2), &getASTContext().SourceMgr,
                            indent + 2);
    out << "\n";
  }

  if (!Fixes.empty()) {
    out.indent(indent) << "Fixes:\n";
    for (auto *fix : Fixes) {
      out.indent(indent + 2);
      fix->print(out);
      out << "\n";
    }
  }
}

/// Determine the semantics of a checked cast operation.
CheckedCastKind
TypeChecker::typeCheckCheckedCast(Type fromType, Type toType,
                                  CheckedCastContextKind contextKind,
                                  DeclContext *dc) {
  // If the from/to types are equivalent or convertible, this is a coercion.
  bool unwrappedIUO = false;
  if (fromType->isEqual(toType) ||
      (isConvertibleTo(fromType, toType, dc, &unwrappedIUO) &&
       !unwrappedIUO)) {
    return CheckedCastKind::Coercion;
  }

  // Since move-only types currently cannot conform to protocols, nor be a class
  // type, the subtyping hierarchy looks a bit like this:
  //
  //                  ~Copyable
  //                    /  \
  //                   /    \
  //       +--------- Any    noncopyable structs/enums
  //       |           |
  //   AnyObject    protocol
  //       |       existentials
  //       |            |   \
  //       +---------+  |    +-- structs/enums
  //                 |  |
  //                classes
  //           (and their subtyping)
  //
  //
  // Thus, right now, a move-only type is only a subtype of itself.
  // We also want to prevent conversions of a move-only type's metatype.
  if (fromType->getMetatypeInstanceType()->isPureMoveOnly()
      || toType->getMetatypeInstanceType()->isPureMoveOnly())
    return CheckedCastKind::Unresolved;
  
  // Check for a bridging conversion.
  // Anything bridges to AnyObject.
  if (toType->isAnyObject())
    return CheckedCastKind::BridgingCoercion;

  if (isObjCBridgedTo(fromType, toType, dc, &unwrappedIUO) && !unwrappedIUO){
    return CheckedCastKind::BridgingCoercion;
  }

  auto *module = dc->getParentModule();
  bool optionalToOptionalCast = false;

  // Local function to indicate failure.
  auto failed = [&] {
    if (contextKind == CheckedCastContextKind::Coercion)
      return CheckedCastKind::Unresolved;

    // Explicit optional-to-optional casts always succeed because a nil
    // value of any optional type can be cast to any other optional type.
    if (optionalToOptionalCast)
      return CheckedCastKind::ValueCast;

    return CheckedCastKind::Unresolved;
  };

  // TODO: Explore optionals using the same strategy used by the
  // runtime.
  // For now, if the target is more optional than the source,
  // just defer it out for the runtime to handle.
  while (auto toValueType = toType->getOptionalObjectType()) {
    auto fromValueType = fromType->getOptionalObjectType();
    if (!fromValueType) {
      return CheckedCastKind::ValueCast;
    }

    toType = toValueType;
    fromType = fromValueType;
    optionalToOptionalCast = true;
  }
  
  // On the other hand, casts can decrease optionality monadically.
  unsigned extraFromOptionals = 0;
  while (auto fromValueType = fromType->getOptionalObjectType()) {
    fromType = fromValueType;
    ++extraFromOptionals;
  }

  // If the unwrapped from/to types are equivalent or bridged, this isn't a real
  // downcast. Complain.
  auto &Context = dc->getASTContext();
  if (extraFromOptionals > 0) {
    switch (typeCheckCheckedCast(fromType, toType, CheckedCastContextKind::None,
                                 dc)) {
    case CheckedCastKind::Coercion:
    case CheckedCastKind::BridgingCoercion: {
      // Treat this as a value cast so we preserve the semantics.
      return CheckedCastKind::ValueCast;
    }

    case CheckedCastKind::ArrayDowncast:
    case CheckedCastKind::DictionaryDowncast:
    case CheckedCastKind::SetDowncast:
    case CheckedCastKind::ValueCast:
      break;

    case CheckedCastKind::Unresolved:
      return failed();
    }
  }

  auto checkElementCast = [&](Type fromElt, Type toElt,
                              CheckedCastKind castKind) -> CheckedCastKind {
    switch (typeCheckCheckedCast(fromElt, toElt, CheckedCastContextKind::None,
                                 dc)) {
    case CheckedCastKind::Coercion:
      return CheckedCastKind::Coercion;

    case CheckedCastKind::BridgingCoercion:
      return CheckedCastKind::BridgingCoercion;

    case CheckedCastKind::ArrayDowncast:
    case CheckedCastKind::DictionaryDowncast:
    case CheckedCastKind::SetDowncast:
    case CheckedCastKind::ValueCast:
      return castKind;

    case CheckedCastKind::Unresolved:
      // Even though we know the elements cannot be downcast, we cannot return
      // Unresolved here as it's possible for an empty Array, Set or Dictionary
      // to be cast to any element type at runtime
      // (https://github.com/apple/swift/issues/48744). The one exception
      // to this is when we're checking whether we can treat a coercion as a
      // checked cast because we don't want to tell the user to use as!, as it's
      // probably the wrong suggestion.
      if (contextKind == CheckedCastContextKind::Coercion)
        return CheckedCastKind::Unresolved;
      return castKind;
    }
    llvm_unreachable("invalid cast type");
  };

  // Check for casts between specific concrete types that cannot succeed.
  if (auto toElementType = toType->isArrayType()) {
    if (auto fromElementType = fromType->isArrayType()) {
      return checkElementCast(fromElementType, toElementType,
                              CheckedCastKind::ArrayDowncast);
    }
  }

  if (auto toKeyValue = ConstraintSystem::isDictionaryType(toType)) {
    if (auto fromKeyValue = ConstraintSystem::isDictionaryType(fromType)) {
      bool hasCoercion = false;
      enum { NoBridging, BridgingCoercion }
        hasBridgingConversion = NoBridging;
      bool hasCast = false;
      switch (typeCheckCheckedCast(fromKeyValue->first, toKeyValue->first,
                                   CheckedCastContextKind::None, dc)) {
      case CheckedCastKind::Coercion:
        hasCoercion = true;
        break;

      case CheckedCastKind::BridgingCoercion:
        hasBridgingConversion = std::max(hasBridgingConversion,
                                         BridgingCoercion);
        break;

      case CheckedCastKind::Unresolved:
        // Handled the same as in checkElementCast; see comment there for
        // rationale.
        if (contextKind == CheckedCastContextKind::Coercion)
          return CheckedCastKind::Unresolved;
        LLVM_FALLTHROUGH;

      case CheckedCastKind::ArrayDowncast:
      case CheckedCastKind::DictionaryDowncast:
      case CheckedCastKind::SetDowncast:
      case CheckedCastKind::ValueCast:
        hasCast = true;
        break;
      }

      switch (typeCheckCheckedCast(fromKeyValue->second, toKeyValue->second,
                                   CheckedCastContextKind::None, dc)) {
      case CheckedCastKind::Coercion:
        hasCoercion = true;
        break;

      case CheckedCastKind::BridgingCoercion:
        hasBridgingConversion = std::max(hasBridgingConversion,
                                         BridgingCoercion);
        break;

      case CheckedCastKind::Unresolved:
        // Handled the same as in checkElementCast; see comment there for
        // rationale.
        if (contextKind == CheckedCastContextKind::Coercion)
          return CheckedCastKind::Unresolved;
        LLVM_FALLTHROUGH;

      case CheckedCastKind::ArrayDowncast:
      case CheckedCastKind::DictionaryDowncast:
      case CheckedCastKind::SetDowncast:
      case CheckedCastKind::ValueCast:
        hasCast = true;
        break;
      }

      if (hasCast) return CheckedCastKind::DictionaryDowncast;
      switch (hasBridgingConversion) {
      case NoBridging:
        break;
      case BridgingCoercion:
        return CheckedCastKind::BridgingCoercion;
      }

      assert(hasCoercion && "Not a coercion?");
      (void)hasCoercion;

      return CheckedCastKind::Coercion;
    }
  }

  if (auto toElementType = ConstraintSystem::isSetType(toType)) {
    if (auto fromElementType = ConstraintSystem::isSetType(fromType)) {
      return checkElementCast(*fromElementType, *toElementType,
                              CheckedCastKind::SetDowncast);
    }
  }

  if (auto toTuple = toType->getAs<TupleType>()) {
    if (auto fromTuple = fromType->getAs<TupleType>()) {
      if (fromTuple->getNumElements() != toTuple->getNumElements())
        return failed();

      for (unsigned i = 0, n = toTuple->getNumElements(); i != n; ++i) {
        const auto &fromElt = fromTuple->getElement(i);
        const auto &toElt = toTuple->getElement(i);

        // We should only perform name validation if both elements have a label,
        // because unlabeled tuple elements can be converted to labeled ones
        // e.g.
        // 
        // let tup: (Any, Any) = (1, 1)
        // _ = tup as! (a: Int, Int)
        if ((!fromElt.getName().empty() && !toElt.getName().empty()) &&
            fromElt.getName() != toElt.getName())
          return failed();

        auto result = checkElementCast(fromElt.getType(), toElt.getType(),
                                       CheckedCastKind::ValueCast);

        if (result == CheckedCastKind::Unresolved)
          return result;
      }

      return CheckedCastKind::ValueCast;
    }
  }

  assert(!toType->isAny() && "casts to 'Any' should've been handled above");
  assert(!toType->isAnyObject() &&
         "casts to 'AnyObject' should've been handled above");

  // A cast from a function type to an existential type (except `Any`)
  // or an archetype type (with constraints) cannot succeed
  auto toArchetypeType = toType->is<ArchetypeType>();
  auto fromFunctionType = fromType->is<FunctionType>();
  auto toExistentialType = toType->isAnyExistentialType();

  auto toConstrainedArchetype = false;
  if (toArchetypeType) {
    auto archetype = toType->castTo<ArchetypeType>();
    toConstrainedArchetype = !archetype->getConformsTo().empty();
  }

  if (fromFunctionType &&
      (toExistentialType || (toArchetypeType && toConstrainedArchetype))) {
    switch (contextKind) {
    case CheckedCastContextKind::None:
    case CheckedCastContextKind::ConditionalCast:
    case CheckedCastContextKind::ForcedCast:
      return CheckedCastKind::Unresolved;

    case CheckedCastContextKind::IsPattern:
    case CheckedCastContextKind::EnumElementPattern:
    case CheckedCastContextKind::IsExpr:
    case CheckedCastContextKind::Coercion:
      break;
    }
  }

  // If we can bridge through an Objective-C class, do so.
  if (Type bridgedToClass = getDynamicBridgedThroughObjCClass(dc, fromType,
                                                              toType)) {
    switch (typeCheckCheckedCast(bridgedToClass, fromType,
                                 CheckedCastContextKind::None, dc)) {
    case CheckedCastKind::ArrayDowncast:
    case CheckedCastKind::BridgingCoercion:
    case CheckedCastKind::Coercion:
    case CheckedCastKind::DictionaryDowncast:
    case CheckedCastKind::SetDowncast:
    case CheckedCastKind::ValueCast:
      return CheckedCastKind::ValueCast;

    case CheckedCastKind::Unresolved:
      break;
    }
  }

  // If we can bridge through an Objective-C class, do so.
  if (Type bridgedFromClass = getDynamicBridgedThroughObjCClass(dc, toType,
                                                                fromType)) {
    switch (typeCheckCheckedCast(toType, bridgedFromClass,
                                 CheckedCastContextKind::None, dc)) {
    case CheckedCastKind::ArrayDowncast:
    case CheckedCastKind::BridgingCoercion:
    case CheckedCastKind::Coercion:
    case CheckedCastKind::DictionaryDowncast:
    case CheckedCastKind::SetDowncast:
    case CheckedCastKind::ValueCast:
      return CheckedCastKind::ValueCast;

    case CheckedCastKind::Unresolved:
      break;
    }
  }

  // Strip metatypes. If we can cast two types, we can cast their metatypes.
  bool metatypeCast = false;
  while (auto toMetatype = toType->getAs<MetatypeType>()) {
    auto fromMetatype = fromType->getAs<MetatypeType>();
    if (!fromMetatype)
      break;
    
    metatypeCast = true;
    toType = toMetatype->getInstanceType();
    fromType = fromMetatype->getInstanceType();
  }
  
  // Strip an inner layer of potentially existential metatype.
  bool toExistentialMetatype = false;
  bool fromExistentialMetatype = false;
  if (auto toMetatype = toType->getAs<AnyMetatypeType>()) {
    if (auto fromMetatype = fromType->getAs<AnyMetatypeType>()) {
      toExistentialMetatype = toType->is<ExistentialMetatypeType>();
      fromExistentialMetatype = fromType->is<ExistentialMetatypeType>();
      toType = toMetatype->getInstanceType();
      fromType = fromMetatype->getInstanceType();
    }
  }

  bool toArchetype = toType->is<ArchetypeType>();
  bool fromArchetype = fromType->is<ArchetypeType>();
  bool toExistential = toType->isExistentialType();
  bool fromExistential = fromType->isExistentialType();

  bool toRequiresClass;
  if (toType->isExistentialType())
    toRequiresClass = toType->getExistentialLayout().requiresClass();
  else
    toRequiresClass = toType->mayHaveSuperclass();

  bool fromRequiresClass;
  if (fromType->isExistentialType())
    fromRequiresClass = fromType->getExistentialLayout().requiresClass();
  else
    fromRequiresClass = fromType->mayHaveSuperclass();
  
  // Casts between metatypes only succeed if none of the types are existentials
  // or if one is an existential and the other is a generic type because there
  // may be protocol conformances unknown at compile time.
  if (metatypeCast) {
    if ((toExistential || fromExistential) && !(fromArchetype || toArchetype))
      return failed();
  }

  // Casts from an existential metatype to a protocol metatype always fail,
  // except when the existential type is 'Any'.
  if (fromExistentialMetatype &&
      !fromType->isAny() &&
      !toExistentialMetatype &&
      toExistential)
    return failed();

  // Casts to or from generic types can't be statically constrained in most
  // cases, because there may be protocol conformances we don't statically
  // know about.
  if (toExistential || fromExistential || fromArchetype || toArchetype ||
      toRequiresClass || fromRequiresClass) {
    // Cast to and from AnyObject always succeed.
    if (!metatypeCast &&
        !fromExistentialMetatype &&
        !toExistentialMetatype &&
        (toType->isAnyObject() || fromType->isAnyObject()))
      return CheckedCastKind::ValueCast;

    // If we have a cast from an existential type to a concrete type that we
    // statically know doesn't conform to the protocol, mark the cast as always
    // failing. For example:
    //
    // struct S {}
    // enum FooError: Error { case bar }
    //
    // func foo() {
    //   do {
    //     throw FooError.bar
    //   } catch is X { /* Will always fail */
    //     print("Caught bar error")
    //   }
    // }
    //
    auto constraint = fromType;
    if (auto existential = constraint->getAs<ExistentialType>())
      constraint = existential->getConstraintType();
    if (auto *protocolDecl =
          dyn_cast_or_null<ProtocolDecl>(constraint->getAnyNominal())) {
      if (!couldDynamicallyConformToProtocol(toType, protocolDecl, module)) {
        return failed();
      }
    } else if (auto protocolComposition =
                   constraint->getAs<ProtocolCompositionType>()) {
      if (llvm::any_of(protocolComposition->getMembers(),
                       [&](Type protocolType) {
                         if (auto protocolDecl = dyn_cast_or_null<ProtocolDecl>(
                                 protocolType->getAnyNominal())) {
                           return !couldDynamicallyConformToProtocol(
                               toType, protocolDecl, module);
                         }
                         return false;
                       })) {
        return failed();
      }
    }

    // If neither type is class-constrained, anything goes.
    if (!fromRequiresClass && !toRequiresClass)
        return CheckedCastKind::ValueCast;

    if (!fromRequiresClass && toRequiresClass) {
      // If source type is abstract, anything goes.
      if (fromExistential || fromArchetype)
        return CheckedCastKind::ValueCast;

      // Otherwise, we're casting a concrete non-class type to a
      // class-constrained archetype or existential, which will
      // probably fail, but we'll try more casts below.
    }

    if (fromRequiresClass && !toRequiresClass) {
      // If destination type is abstract, anything goes.
      if (toExistential || toArchetype)
        return CheckedCastKind::ValueCast;

      // Otherwise, we're casting a class-constrained archetype
      // or existential to a non-class concrete type, which
      // will probably fail, but we'll try more casts below.
    }

    if (fromRequiresClass && toRequiresClass) {
      // Ok, we are casting between class-like things. Let's see if we have
      // explicit superclass bounds.
      Type toSuperclass;
      if (toType->getClassOrBoundGenericClass())
        toSuperclass = toType;
      else
        toSuperclass = toType->getSuperclass();

      Type fromSuperclass;
      if (fromType->getClassOrBoundGenericClass())
        fromSuperclass = fromType;
      else
        fromSuperclass = fromType->getSuperclass();

      // Unless both types have a superclass bound, we have no further
      // information.
      if (!toSuperclass || !fromSuperclass)
        return CheckedCastKind::ValueCast;

      // Compare superclass bounds.
      if (fromSuperclass->isBindableToSuperclassOf(toSuperclass))
        return CheckedCastKind::ValueCast;

      // An upcast is also OK.
      if (toSuperclass->isBindableToSuperclassOf(fromSuperclass))
        return CheckedCastKind::ValueCast;
    }
  }

  if (toType->isAnyHashable() || fromType->isAnyHashable()) {
    return CheckedCastKind::ValueCast;
  }

  // We perform an upcast while rebinding generic parameters if it's possible
  // to substitute the generic arguments of the source type with the generic
  // archetypes of the destination type. Or, if it's possible to substitute
  // the generic arguments of the destination type with the generic archetypes
  // of the source type, we perform a downcast instead.
  if (toType->isBindableTo(fromType) || fromType->isBindableTo(toType))
    return CheckedCastKind::ValueCast;
  
  // Objective-C metaclasses are subclasses of NSObject in the ObjC runtime,
  // so casts from NSObject to potentially-class metatypes may succeed.
  if (auto nsObject = Context.getNSObjectType()) {
    if (fromType->isEqual(nsObject)) {
      if (auto toMeta = toType->getAs<MetatypeType>()) {
        if (toMeta->getInstanceType()->mayHaveSuperclass()
            || toMeta->getInstanceType()->is<ArchetypeType>())
          return CheckedCastKind::ValueCast;
      }
      if (toType->is<ExistentialMetatypeType>())
        return CheckedCastKind::ValueCast;
    }
  }

  // We can conditionally cast from NSError to an Error-conforming type.
  // This is handled in the runtime, so it doesn't need a special cast
  // kind.
  if (Context.LangOpts.EnableObjCInterop) {
    auto nsObject = Context.getNSObjectType();
    auto nsErrorTy = Context.getNSErrorType();

    if (auto errorTypeProto = Context.getProtocol(KnownProtocolKind::Error)) {
      if (conformsToProtocol(toType, errorTypeProto, module)) {
        if (nsErrorTy) {
          if (isSubtypeOf(fromType, nsErrorTy, dc)
              // Don't mask "always true" warnings if NSError is cast to
              // Error itself.
              && !isSubtypeOf(fromType, toType, dc))
            return CheckedCastKind::ValueCast;
        }
      }

      if (conformsToProtocol(fromType, errorTypeProto, module)) {
        // Cast of an error-conforming type to NSError or NSObject.
        if ((nsObject && toType->isEqual(nsObject)) ||
             (nsErrorTy && toType->isEqual(nsErrorTy)))
            return CheckedCastKind::BridgingCoercion;
      }
    }

    // Any class-like type could be dynamically cast to NSObject or NSError
    // via an Error conformance.
    if (fromType->mayHaveSuperclass() &&
        ((nsObject && toType->isEqual(nsObject)) ||
         (nsErrorTy && toType->isEqual(nsErrorTy)))) {
      return CheckedCastKind::ValueCast;
    }
  }

  // The runtime doesn't support casts to CF types and always lets them succeed.
  // This "always fails" diagnosis makes no sense when paired with the CF
  // one.
  auto clazz = toType->getClassOrBoundGenericClass();
  if (clazz && clazz->getForeignClassKind() == ClassDecl::ForeignKind::CFType)
    return CheckedCastKind::ValueCast;
  
  // Don't warn on casts that change the generic parameters of ObjC generic
  // classes. This may be necessary to force-fit ObjC APIs that depend on
  // covariance, or for APIs where the generic parameter annotations in the
  // ObjC headers are inaccurate.
  if (clazz && clazz->isTypeErasedGenericClass()) {
    if (fromType->getClassOrBoundGenericClass() == clazz)
      return CheckedCastKind::ValueCast;
  }

  return failed();
}

/// If the expression is an implicit call to _forceBridgeFromObjectiveC or
/// _conditionallyBridgeFromObjectiveC, returns the argument of that call.
static Expr *lookThroughBridgeFromObjCCall(ASTContext &ctx, Expr *expr) {
  auto call = dyn_cast<CallExpr>(expr);
  if (!call || !call->isImplicit())
    return nullptr;

  auto callee = call->getCalledValue();
  if (!callee)
    return nullptr;

  if (callee == ctx.getForceBridgeFromObjectiveC() ||
      callee == ctx.getConditionallyBridgeFromObjectiveC())
    return call->getArgs()->getExpr(0);

  return nullptr;
}

/// If the expression has the effect of a forced downcast, find the
/// underlying forced downcast expression.
ForcedCheckedCastExpr *swift::findForcedDowncast(ASTContext &ctx, Expr *expr) {
  expr = expr->getSemanticsProvidingExpr();
  
  // Simple case: forced checked cast.
  if (auto forced = dyn_cast<ForcedCheckedCastExpr>(expr)) {
    return forced;
  }

  // If we have an implicit force, look through it.
  if (auto forced = dyn_cast<ForceValueExpr>(expr)) {
    if (forced->isImplicit()) {
      expr = forced->getSubExpr();
    }
  }

  // Skip through optional evaluations and binds.
  auto skipOptionalEvalAndBinds = [](Expr *expr) -> Expr* {
    do {
      if (!expr->isImplicit())
        break;

      if (auto optionalEval = dyn_cast<OptionalEvaluationExpr>(expr)) {
        expr = optionalEval->getSubExpr();
        continue;
      }

      if (auto bindOptional = dyn_cast<BindOptionalExpr>(expr)) {
        expr = bindOptional->getSubExpr();
        continue;
      }
      
      break;
    } while (true);

    return expr;
  };

  auto sub = skipOptionalEvalAndBinds(expr);
  
  // If we have an explicit cast, we're done.
  if (auto *FCE = dyn_cast<ForcedCheckedCastExpr>(sub))
    return FCE;

  // Otherwise, try to look through an implicit _forceBridgeFromObjectiveC() call.
  if (auto arg = lookThroughBridgeFromObjCCall(ctx, sub)) {
    sub = skipOptionalEvalAndBinds(arg);
    if (auto *FCE = dyn_cast<ForcedCheckedCastExpr>(sub))
      return FCE;
  }

  return nullptr;
}

void ConstraintSystem::forEachExpr(
    Expr *expr, llvm::function_ref<Expr *(Expr *)> callback) {
  struct ChildWalker : ASTWalker {
    ConstraintSystem &CS;
    llvm::function_ref<Expr *(Expr *)> callback;

    ChildWalker(ConstraintSystem &CS,
                llvm::function_ref<Expr *(Expr *)> callback)
        : CS(CS), callback(callback) {}

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      auto *NewE = callback(E);
      if (!NewE)
        return Action::Stop();

      if (auto closure = dyn_cast<ClosureExpr>(E)) {
        if (!CS.participatesInInference(closure))
          return Action::SkipChildren(NewE);
      }
      return Action::Continue(NewE);
    }

    PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override {
      return Action::SkipChildren(P);
    }
    PreWalkAction walkToDeclPre(Decl *D) override {
      return Action::SkipChildren();
    }
    PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
      return Action::SkipChildren();
    }
  };

  expr->walk(ChildWalker(*this, callback));
}
