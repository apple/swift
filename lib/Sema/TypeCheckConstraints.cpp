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
#include "swift/Sema/CodeCompletionTypeChecking.h"
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
  getTypeVariable()->print(OS, PrintOptions());
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

Optional<ExprKind>
TypeVariableType::Implementation::getAtomicLiteralKind() const {
  if (!locator || !locator->directlyAt<LiteralExpr>())
    return None;

  auto kind = getAsExpr(locator->getAnchor())->getKind();
  switch (kind) {
  case ExprKind::IntegerLiteral:
  case ExprKind::FloatLiteral:
  case ExprKind::StringLiteral:
  case ExprKind::BooleanLiteral:
  case ExprKind::NilLiteral:
    return kind;
  default:
    return None;
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

bool TypeVariableType::Implementation::isCodeCompletionToken() const {
  return locator && locator->directlyAt<CodeCompletionExpr>();
}

void *operator new(size_t bytes, ConstraintSystem& cs,
                   size_t alignment) {
  return cs.getAllocator().Allocate(bytes, alignment);
}

bool constraints::computeTupleShuffle(ArrayRef<TupleTypeElt> fromTuple,
                                      ArrayRef<TupleTypeElt> toTuple,
                                      SmallVectorImpl<unsigned> &sources) {
  const unsigned unassigned = -1;
  
  SmallVector<bool, 4> consumed(fromTuple.size(), false);
  sources.clear();
  sources.assign(toTuple.size(), unassigned);

  // Match up any named elements.
  for (unsigned i = 0, n = toTuple.size(); i != n; ++i) {
    const auto &toElt = toTuple[i];

    // Skip unnamed elements.
    if (!toElt.hasName())
      continue;

    // Find the corresponding named element.
    int matched = -1;
    {
      int index = 0;
      for (auto field : fromTuple) {
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
  unsigned fromNext = 0, fromLast = fromTuple.size();
  auto skipToNextAvailableInput = [&] {
    while (fromNext != fromLast && consumed[fromNext])
      ++fromNext;
  };
  skipToNextAvailableInput();

  for (unsigned i = 0, n = toTuple.size(); i != n; ++i) {
    // Check whether we already found a value for this element.
    if (sources[i] != unassigned)
      continue;

    // If there aren't any more inputs, we are done.
    if (fromNext == fromLast) {
      return true;
    }

    // Otherwise, assign this input to the next output element.
    const auto &elt2 = toTuple[i];
    assert(!elt2.isVararg());

    // Fail if the input element is named and we're trying to match it with
    // something with a different label.
    if (fromTuple[fromNext].hasName() && elt2.hasName())
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

  std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {
    performSyntacticExprDiagnostics(expr, dcStack.back(), /*isExprStmt=*/false);

    if (auto closure = dyn_cast<ClosureExpr>(expr)) {
      if (closure->isSeparatelyTypeChecked()) {
        dcStack.push_back(closure);
        return {true, expr};
      }
    }

    return {false, expr};
  }

  Expr *walkToExprPost(Expr *expr) override {
    if (auto closure = dyn_cast<ClosureExpr>(expr)) {
      if (closure->isSeparatelyTypeChecked()) {
        assert(dcStack.back() == closure);
        dcStack.pop_back();
      }
    }

    return expr;
  }

  std::pair<bool, Stmt *> walkToStmtPre(Stmt *stmt) override {
    performStmtDiagnostics(stmt, dcStack.back());
    return {true, stmt};
  }

  std::pair<bool, Pattern *> walkToPatternPre(Pattern *pattern) override {
    return {false, pattern};
  }

  bool walkToTypeReprPre(TypeRepr *typeRepr) override { return false; }
  bool walkToParameterListPre(ParameterList *params) override { return false; }
};
} // end anonymous namespace

void constraints::performSyntacticDiagnosticsForTarget(
    const SolutionApplicationTarget &target, bool isExprStmt) {
  auto *dc = target.getDeclContext();
  switch (target.kind) {
  case SolutionApplicationTarget::Kind::expression: {
    // First emit diagnostics for the main expression.
    performSyntacticExprDiagnostics(target.getAsExpr(), dc, isExprStmt);

    // If this is a for-in statement, we also need to check the where clause if
    // present.
    if (target.isForEachStmt()) {
      if (auto *whereExpr = target.getForEachStmtInfo().whereExpr)
        performSyntacticExprDiagnostics(whereExpr, dc, /*isExprStmt*/ false);
    }
    return;
  }
  case SolutionApplicationTarget::Kind::function: {
    FunctionSyntacticDiagnosticWalker walker(dc);
    target.getFunctionBody()->walk(walker);
    return;
  }
  case SolutionApplicationTarget::Kind::stmtCondition:
  case SolutionApplicationTarget::Kind::caseLabelItem:
  case SolutionApplicationTarget::Kind::patternBinding:
  case SolutionApplicationTarget::Kind::uninitializedWrappedVar:
    // Nothing to do for these.
    return;
  }
  llvm_unreachable("Unhandled case in switch!");
}

#pragma mark High-level entry points
Type TypeChecker::typeCheckExpression(Expr *&expr, DeclContext *dc,
                                      ContextualTypeInfo contextualInfo,
                                      TypeCheckExprOptions options) {
  SolutionApplicationTarget target(
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

Optional<SolutionApplicationTarget>
TypeChecker::typeCheckExpression(
    SolutionApplicationTarget &target,
    TypeCheckExprOptions options) {
  Expr *expr = target.getAsExpr();
  DeclContext *dc = target.getDeclContext();
  auto &Context = dc->getASTContext();
  FrontendStatsTracer StatsTracer(Context.Stats,
                                  "typecheck-expr", expr);
  PrettyStackTraceExpr stackTrace(Context, "type-checking", expr);

  // First, pre-check the expression, validating any types that occur in the
  // expression and folding sequence expressions.
  if (ConstraintSystem::preCheckExpression(
          expr, dc, /*replaceInvalidRefsWithErrors=*/true)) {
    target.setExpr(expr);
    return None;
  }
  target.setExpr(expr);

  // Check whether given expression has a code completion token which requires
  // special handling.
  if (Context.CompletionCallback &&
      typeCheckForCodeCompletion(target, /*needsPrecheck*/false,
                                 [&](const constraints::Solution &S) {
        Context.CompletionCallback->sawSolution(S);
      }))
    return None;

  // Construct a constraint system from this expression.
  ConstraintSystemOptions csOptions = ConstraintSystemFlags::AllowFixes;

  if (DiagnosticSuppression::isEnabled(Context.Diags))
    csOptions |= ConstraintSystemFlags::SuppressDiagnostics;

  if (options.contains(TypeCheckExprFlags::LeaveClosureBodyUnchecked))
    csOptions |= ConstraintSystemFlags::LeaveClosureBodyUnchecked;

  ConstraintSystem cs(dc, csOptions);

  // Tell the constraint system what the contextual type is.  This informs
  // diagnostics and is a hint for various performance optimizations.
  cs.setContextualType(
      expr,
      target.getExprContextualTypeLoc(),
      target.getExprContextualTypePurpose());

  // Try to shrink the system by reducing disjunction domains. This
  // goes through every sub-expression and generate its own sub-system, to
  // try to reduce the domains of those subexpressions.
  cs.shrink(expr);
  target.setExpr(expr);

  // If the client can handle unresolved type variables, leave them in the
  // system.
  auto allowFreeTypeVariables = FreeTypeVariableBinding::Disallow;

  // Attempt to solve the constraint system.
  auto viable = cs.solve(target, allowFreeTypeVariables);
  if (!viable) {
    target.setExpr(expr);
    return None;
  }

  // Apply this solution to the constraint system.
  // FIXME: This shouldn't be necessary.
  auto &solution = (*viable)[0];
  cs.applySolution(solution);

  // Apply the solution to the expression.
  auto resultTarget = cs.applySolution(solution, target);
  if (!resultTarget) {
    // Failure already diagnosed, above, as part of applying the solution.
    return None;
  }
  Expr *result = resultTarget->getAsExpr();

  // Unless the client has disabled them, perform syntactic checks on the
  // expression now.
  if (!cs.shouldSuppressDiagnostics()) {
    bool isExprStmt = options.contains(TypeCheckExprFlags::IsExprStmt);
    performSyntacticDiagnosticsForTarget(*resultTarget, isExprStmt);
  }

  resultTarget->setExpr(result);
  return *resultTarget;
}

Type TypeChecker::typeCheckParameterDefault(Expr *&defaultValue,
                                            DeclContext *DC, Type paramType,
                                            bool isAutoClosure) {
  assert(paramType && !paramType->hasError());
  return typeCheckExpression(defaultValue, DC, /*contextualInfo=*/
                             {paramType, isAutoClosure
                                             ? CTP_AutoclosureDefaultParameter
                                             : CTP_DefaultParameter});
}

bool TypeChecker::typeCheckBinding(
    Pattern *&pattern, Expr *&initializer, DeclContext *DC,
    Type patternType, PatternBindingDecl *PBD, unsigned patternNumber) {
  SolutionApplicationTarget target =
    PBD ? SolutionApplicationTarget::forInitialization(
            initializer, DC, patternType, PBD, patternNumber,
            /*bindPatternVarsOneWay=*/false)
        : SolutionApplicationTarget::forInitialization(
            initializer, DC, patternType, pattern,
            /*bindPatternVarsOneWay=*/false);

  // Type-check the initializer.
  auto resultTarget = typeCheckExpression(target);

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
                                          Type patternType) {
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

  bool hadError = TypeChecker::typeCheckBinding(
      pattern, init, DC, patternType, PBD, patternNumber);
  if (!init) {
    PBD->setInvalid();
    return true;
  }

  PBD->setPattern(patternNumber, pattern, initContext);
  PBD->setInit(patternNumber, init);

  // Bind a property with an opaque return type to the underlying type
  // given by the initializer.
  if (auto var = pattern->getSingleVar()) {
    if (auto opaque = var->getOpaqueResultTypeDecl()) {
      if (auto convertedInit = dyn_cast<UnderlyingToOpaqueExpr>(init)) {
        auto underlyingType = convertedInit->getSubExpr()->getType()
            ->mapTypeOutOfContext();
        auto underlyingSubs = SubstitutionMap::get(
          opaque->getOpaqueInterfaceGenericSignature(),
          [&](SubstitutableType *t) -> Type {
            if (t->isEqual(opaque->getUnderlyingInterfaceType())) {
              return underlyingType;
            }
            return Type(t);
          },
          LookUpConformanceInModule(opaque->getModuleContext()));
        
        opaque->setUnderlyingTypeSubstitutions(underlyingSubs);
      } else {
        var->diagnose(diag::opaque_type_var_no_underlying_type);
      }
    }
  }

  if (hadError)
    PBD->setInvalid();

  PBD->setInitializerChecked(patternNumber);

  checkPatternBindingDeclAsyncUsage(PBD);

  return hadError;
}

bool TypeChecker::typeCheckForEachBinding(DeclContext *dc, ForEachStmt *stmt) {
  auto &Context = dc->getASTContext();

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

  auto sequenceProto = TypeChecker::getProtocol(
      dc->getASTContext(), stmt->getForLoc(), 
      stmt->getAwaitLoc().isValid() ? 
        KnownProtocolKind::AsyncSequence : KnownProtocolKind::Sequence);
  if (!sequenceProto)
    return failed();

  // Precheck the sequence.
  Expr *sequence = stmt->getSequence();
  if (ConstraintSystem::preCheckExpression(
          sequence, dc, /*replaceInvalidRefsWithErrors=*/true))
    return failed();
  stmt->setSequence(sequence);

  // Precheck the filtering condition.
  if (Expr *whereExpr = stmt->getWhere()) {
    if (ConstraintSystem::preCheckExpression(
            whereExpr, dc, /*replaceInvalidRefsWithErrors=*/true))
      return failed();

    stmt->setWhere(whereExpr);
  }

  auto target = SolutionApplicationTarget::forForEachStmt(
      stmt, sequenceProto, dc, /*bindPatternVarsOneWay=*/false);
  if (!typeCheckExpression(target))
    return failed();

  // check to see if the sequence expr is throwing (and async), if so require 
  // the stmt to have a try loc
  if (stmt->getAwaitLoc().isValid()) {
    // fetch the sequence out of the statement
    // else wise the value is potentially unresolved
    auto Ty = stmt->getSequence()->getType();
    auto module = dc->getParentModule();
    auto conformanceRef = module->lookupConformance(Ty, sequenceProto);
    
    if (conformanceRef.hasEffect(EffectKind::Throws) &&
        stmt->getTryLoc().isInvalid()) {
      auto &diags = dc->getASTContext().Diags;
      diags.diagnose(stmt->getAwaitLoc(), diag::throwing_call_unhandled, "call")
        .fixItInsert(stmt->getAwaitLoc(), "try");

      return failed();
    }
  }

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

/// Find the '~=` operator that can compare an expression inside a pattern to a
/// value of a given type.
bool TypeChecker::typeCheckExprPattern(ExprPattern *EP, DeclContext *DC,
                                       Type rhsType) {
  auto &Context = DC->getASTContext();
  FrontendStatsTracer StatsTracer(Context.Stats,
                                  "typecheck-expr-pattern", EP);
  PrettyStackTracePattern stackTrace(Context, "type-checking", EP);

  // Create a 'let' binding to stand in for the RHS value.
  auto *matchVar = new (Context) VarDecl(/*IsStatic*/false,
                                         VarDecl::Introducer::Let,
                                         EP->getLoc(),
                                         Context.getIdentifier("$match"),
                                         DC);
  matchVar->setInterfaceType(rhsType->mapTypeOutOfContext());

  matchVar->setImplicit();
  EP->setMatchVar(matchVar);

  // Find '~=' operators for the match.
  auto matchLookup =
      lookupUnqualified(DC->getModuleScopeContext(),
                        DeclNameRef(Context.Id_MatchOperator),
                        SourceLoc(), defaultUnqualifiedLookupOptions);
  auto &diags = DC->getASTContext().Diags;
  if (!matchLookup) {
    diags.diagnose(EP->getLoc(), diag::no_match_operator);
    return true;
  }
  
  SmallVector<ValueDecl*, 4> choices;
  for (auto &result : matchLookup) {
    choices.push_back(result.getValueDecl());
  }
  
  if (choices.empty()) {
    diags.diagnose(EP->getLoc(), diag::no_match_operator);
    return true;
  }
  
  // Build the 'expr ~= var' expression.
  // FIXME: Compound name locations.
  auto *matchOp =
      TypeChecker::buildRefExpr(choices, DC, DeclNameLoc(EP->getLoc()),
                                /*Implicit=*/true, FunctionRefKind::Compound);

  // Note we use getEndLoc here to have the BinaryExpr source range be the same
  // as the expr pattern source range.
  auto *matchVarRef = new (Context) DeclRefExpr(matchVar,
                                                DeclNameLoc(EP->getEndLoc()),
                                                /*Implicit=*/true);
  Expr *matchCall = BinaryExpr::create(Context, EP->getSubExpr(), matchOp,
                                       matchVarRef, /*implicit*/ true);
  // Check the expression as a condition.
  bool hadError = typeCheckCondition(matchCall, DC);
  // Save the type-checked expression in the pattern.
  EP->setMatchExpr(matchCall);
  // Set the type on the pattern.
  EP->setType(rhsType);
  return hadError;
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
        // We leave opaque types and their nested associated types alone here.
        // They're globally available.
        if (isa<OpaqueTypeArchetypeType>(root))
          return origType;
        // For other nested types, fail here so the default logic in subst()
        // for nested types applies.
        else if (root != archetypeType)
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
    MakeAbstractConformanceForGenericType());
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
  auto kind = TypeChecker::typeCheckCheckedCast(t1, t2,
                                                CheckedCastContextKind::None, dc,
                                   SourceLoc(), nullptr, SourceRange());
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

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      if (isa<ParenExpr>(E) || isa<ForceValueExpr>(E))
        return { true, E };

      // Since load expression is created by walker,
      // it's safe to stop as soon as it encounters first one
      // because it would be the one it just created.
      if (isa<LoadExpr>(E))
        return { false, nullptr };

      return { false, createLoadExpr(E) };
    }

    Expr *walkToExprPost(Expr *E) override {
      if (auto *FVE = dyn_cast<ForceValueExpr>(E))
        setType(E, getType(FVE->getSubExpr())->getOptionalObjectType());

      if (auto *PE = dyn_cast<ParenExpr>(E))
        setType(E, ParenType::get(Ctx, getType(PE->getSubExpr())));

      return E;
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

void Solution::dump() const {
  dump(llvm::errs());
}

void Solution::dump(raw_ostream &out) const {
  PrintOptions PO;
  PO.PrintTypesForDebugging = true;

  SourceManager *sm = &getConstraintSystem().getASTContext().SourceMgr;

  out << "Fixed score: " << FixedScore << "\n";

  out << "Type variables:\n";
  for (auto binding : typeBindings) {
    auto &typeVar = binding.first;
    out.indent(2);
    Type(typeVar).print(out, PO);
    out << " as ";
    binding.second.print(out, PO);
    if (auto *locator = typeVar->getImpl().getLocator()) {
      out << " @ ";
      locator->dump(sm, out);
    }
    out << "\n";
  }

  out << "\n";
  out << "Overload choices:\n";
  for (auto ovl : overloadChoices) {
    out.indent(2);
    if (ovl.first)
      ovl.first->dump(sm, out);
    out << " with ";

    auto choice = ovl.second.choice;
    switch (choice.getKind()) {
    case OverloadChoiceKind::Decl:
    case OverloadChoiceKind::DeclViaDynamic:
    case OverloadChoiceKind::DeclViaBridge:
    case OverloadChoiceKind::DeclViaUnwrappedOptional:
      choice.getDecl()->dumpRef(out);
      out << " as ";
      if (choice.getBaseType())
        out << choice.getBaseType()->getString(PO) << ".";

      out << choice.getDecl()->getBaseName() << ": "
          << ovl.second.openedType->getString(PO) << "\n";
      break;

    case OverloadChoiceKind::KeyPathApplication:
      out << "key path application root "
          << choice.getBaseType()->getString(PO) << "\n";
      break;

    case OverloadChoiceKind::DynamicMemberLookup:
    case OverloadChoiceKind::KeyPathDynamicMemberLookup:
      out << "dynamic member lookup root "
          << choice.getBaseType()->getString(PO)
          << " name='" << choice.getName() << "'\n";
      break;
  
    case OverloadChoiceKind::TupleIndex:
      out << "tuple " << choice.getBaseType()->getString(PO) << " index "
        << choice.getTupleIndex() << "\n";
      break;
    }
    out << "\n";
  }

  out << "\n";
  out << "Constraint restrictions:\n";
  for (auto &restriction : ConstraintRestrictions) {
    out.indent(2) << restriction.first.first
                  << " to " << restriction.first.second
                  << " is " << getName(restriction.second) << "\n";
  }

  out << "\n";
  out << "Trailing closure matching:\n";
  for (auto &argumentMatching : argumentMatchingChoices) {
    out.indent(2);
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

  out << "\nDisjunction choices:\n";
  for (auto &choice : DisjunctionChoices) {
    out.indent(2);
    choice.first->dump(sm, out);
    out << " is #" << choice.second << "\n";
  }

  if (!OpenedTypes.empty()) {
    out << "\nOpened types:\n";
    for (const auto &opened : OpenedTypes) {
      out.indent(2);
      opened.first->dump(sm, out);
      out << " opens ";
      llvm::interleave(
          opened.second.begin(), opened.second.end(),
          [&](OpenedType opened) {
            Type(opened.first).print(out, PO);
            out << " -> ";
            Type(opened.second).print(out, PO);
          },
          [&]() { out << ", "; });
      out << "\n";
    }
  }

  if (!OpenedExistentialTypes.empty()) {
    out << "\nOpened existential types:\n";
    for (const auto &openedExistential : OpenedExistentialTypes) {
      out.indent(2);
      openedExistential.first->dump(sm, out);
      out << " opens to " << openedExistential.second->getString(PO);
      out << "\n";
    }
  }

  if (!DefaultedConstraints.empty()) {
    out << "\nDefaulted constraints: ";
    interleave(DefaultedConstraints, [&](ConstraintLocator *locator) {
      locator->dump(sm, out);
    }, [&] {
      out << ", ";
    });
  }

  if (!Fixes.empty()) {
    out << "\nFixes:\n";
    for (auto *fix : Fixes) {
      out.indent(2);
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

  E->dump(out, getTypeOfExpr, getTypeOfTypeRepr, getTypeOfKeyPathComponent);
}

void ConstraintSystem::print(raw_ostream &out) const {
  // Print all type variables as $T0 instead of _ here.
  PrintOptions PO;
  PO.PrintTypesForDebugging = true;
  
  out << "Score: " << CurrentScore << "\n";

  for (const auto &contextualType : contextualTypes) {
    out << "Contextual Type: " << contextualType.second.getType().getString(PO);
    if (TypeRepr *TR = contextualType.second.typeLoc.getTypeRepr()) {
      out << " at ";
      TR->getSourceRange().print(out, getASTContext().SourceMgr, /*text*/false);
    }
    out << "\n";
  }

  out << "Type Variables:\n";
  for (auto tv : getTypeVariables()) {
    out.indent(2);
    Type(tv).print(out, PO);
    if (tv->getImpl().canBindToLValue())
      out << " [lvalue allowed]";
    if (tv->getImpl().canBindToInOut())
      out << " [inout allowed]";
    if (tv->getImpl().canBindToNoEscape())
      out << " [noescape allowed]";
    auto rep = getRepresentative(tv);
    if (rep == tv) {
      if (auto fixed = getFixedType(tv)) {
        out << " as ";
        Type(fixed).print(out, PO);
      } else {
        const_cast<ConstraintSystem *>(this)->getBindingsFor(tv).dump(out, 1);
      }
    } else {
      out << " equivalent to ";
      Type(rep).print(out, PO);
    }

    if (auto *locator = tv->getImpl().getLocator()) {
      out << " @ ";
      locator->dump(&getASTContext().SourceMgr, out);
    }

    out << "\n";
  }

  out << "\nActive Constraints:\n";
  for (auto &constraint : ActiveConstraints) {
    out.indent(2);
    constraint.print(out, &getASTContext().SourceMgr);
    out << "\n";
  }

  out << "\nInactive Constraints:\n";
  for (auto &constraint : InactiveConstraints) {
    out.indent(2);
    constraint.print(out, &getASTContext().SourceMgr);
    out << "\n";
  }

  if (solverState && solverState->hasRetiredConstraints()) {
    out << "\nRetired Constraints:\n";
    solverState->forEachRetired([&](Constraint &constraint) {
      out.indent(2);
      constraint.print(out, &getASTContext().SourceMgr);
      out << "\n";
    });
  }

  if (!ResolvedOverloads.empty()) {
    out << "Resolved overloads:\n";

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
            << resolved.openedType->getString(PO) << "\n";
        break;

      case OverloadChoiceKind::KeyPathApplication:
        out << "key path application root "
            << choice.getBaseType()->getString(PO) << "\n";
        break;

      case OverloadChoiceKind::DynamicMemberLookup:
      case OverloadChoiceKind::KeyPathDynamicMemberLookup:
        out << "dynamic member lookup:"
            << choice.getBaseType()->getString(PO) << "  name="
            << choice.getName() << "\n";
        break;

      case OverloadChoiceKind::TupleIndex:
        out << "tuple " << choice.getBaseType()->getString(PO) << " index "
            << choice.getTupleIndex() << "\n";
        break;
      }
    }
    out << "\n";
  }

  if (!DisjunctionChoices.empty()) {
    out << "\nDisjunction choices:\n";
    for (auto &choice : DisjunctionChoices) {
      out.indent(2);
      choice.first->dump(&getASTContext().SourceMgr, out);
      out << " is #" << choice.second << "\n";
    }
  }

  if (!OpenedTypes.empty()) {
    out << "\nOpened types:\n";
    for (const auto &opened : OpenedTypes) {
      out.indent(2);
      opened.first->dump(&getASTContext().SourceMgr, out);
      out << " opens ";
      llvm::interleave(
          opened.second.begin(), opened.second.end(),
          [&](OpenedType opened) {
            Type(opened.first).print(out, PO);
            out << " -> ";
            Type(opened.second).print(out, PO);
          },
          [&]() { out << ", "; });
      out << "\n";
    }
  }

  if (!OpenedExistentialTypes.empty()) {
    out << "\nOpened existential types:\n";
    for (const auto &openedExistential : OpenedExistentialTypes) {
      out.indent(2);
      openedExistential.first->dump(&getASTContext().SourceMgr, out);
      out << " opens to " << openedExistential.second->getString(PO);
      out << "\n";
    }
  }

  if (!DefaultedConstraints.empty()) {
    out << "\nDefaulted constraints: ";
    interleave(DefaultedConstraints, [&](ConstraintLocator *locator) {
      locator->dump(&getASTContext().SourceMgr, out);
    }, [&] {
      out << ", ";
    });
    out << "\n";
  }

  if (failedConstraint) {
    out << "\nFailed constraint:\n";
    out.indent(2);
    failedConstraint->print(out, &getASTContext().SourceMgr);
    out << "\n";
  }

  if (!Fixes.empty()) {
    out << "\nFixes:\n";
    for (auto *fix : Fixes) {
      out.indent(2);
      fix->print(out);
      out << "\n";
    }
  }
}

/// Determine the semantics of a checked cast operation.
CheckedCastKind TypeChecker::typeCheckCheckedCast(Type fromType,
                                                  Type toType,
                                                  CheckedCastContextKind contextKind,
                                                  DeclContext *dc,
                                                  SourceLoc diagLoc,
                                                  Expr *fromExpr,
                                                  SourceRange diagToRange) {
  // Determine whether we should suppress diagnostics.
  const bool suppressDiagnostics =
      contextKind == CheckedCastContextKind::None ||
      contextKind == CheckedCastContextKind::Coercion;
  assert((suppressDiagnostics || diagLoc.isValid()) &&
         "diagnostics require a valid source location");

  SourceRange diagFromRange;
  if (fromExpr)
    diagFromRange = fromExpr->getSourceRange();
  
  // If the from/to types are equivalent or convertible, this is a coercion.
  bool unwrappedIUO = false;
  if (fromType->isEqual(toType) ||
      (isConvertibleTo(fromType, toType, dc, &unwrappedIUO) &&
       !unwrappedIUO)) {
    return CheckedCastKind::Coercion;
  }
  
  // Check for a bridging conversion.
  // Anything bridges to AnyObject.
  if (toType->isAnyObject())
    return CheckedCastKind::BridgingCoercion;

  if (isObjCBridgedTo(fromType, toType, dc, &unwrappedIUO) && !unwrappedIUO){
    return CheckedCastKind::BridgingCoercion;
  }

  Type origFromType = fromType;
  Type origToType = toType;

  auto *module = dc->getParentModule();

  auto &diags = dc->getASTContext().Diags;
  bool optionalToOptionalCast = false;

  // Local function to indicate failure.
  auto failed = [&] {
    if (suppressDiagnostics) {
      return CheckedCastKind::Unresolved;
    }

    // Explicit optional-to-optional casts always succeed because a nil
    // value of any optional type can be cast to any other optional type.
    if (optionalToOptionalCast)
      return CheckedCastKind::ValueCast;

    diags.diagnose(diagLoc, diag::downcast_to_unrelated, origFromType,
                   origToType)
      .highlight(diagFromRange)
      .highlight(diagToRange);

    return CheckedCastKind::ValueCast;
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
    switch (typeCheckCheckedCast(fromType, toType,
                                 CheckedCastContextKind::None, dc,
                                 SourceLoc(), nullptr, SourceRange())) {
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
                                 dc, SourceLoc(), nullptr, SourceRange())) {
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
      // failed() here as it's possible for an empty Array, Set or Dictionary to
      // be cast to any element type at runtime (SR-6192). The one exception to
      // this is when we're checking whether we can treat a coercion as a checked
      // cast because we don't want to tell the user to use as!, as it's probably
      // the wrong suggestion.
      if (contextKind == CheckedCastContextKind::Coercion)
        return failed();
      return castKind;
    }
    llvm_unreachable("invalid cast type");
  };

  // Check for casts between specific concrete types that cannot succeed.
  if (auto toElementType = ConstraintSystem::isArrayType(toType)) {
    if (auto fromElementType = ConstraintSystem::isArrayType(fromType)) {
      return checkElementCast(*fromElementType, *toElementType,
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
                                   CheckedCastContextKind::None, dc,
                                   SourceLoc(), nullptr, SourceRange())) {
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
          return failed();
        LLVM_FALLTHROUGH;

      case CheckedCastKind::ArrayDowncast:
      case CheckedCastKind::DictionaryDowncast:
      case CheckedCastKind::SetDowncast:
      case CheckedCastKind::ValueCast:
        hasCast = true;
        break;
      }

      switch (typeCheckCheckedCast(fromKeyValue->second, toKeyValue->second,
                                   CheckedCastContextKind::None, dc,
                                   SourceLoc(), nullptr, SourceRange())) {
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
          return failed();
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
    case CheckedCastContextKind::ConditionalCast:
    case CheckedCastContextKind::ForcedCast:
      diags.diagnose(diagLoc, diag::downcast_to_unrelated, origFromType,
                     origToType)
          .highlight(diagFromRange)
          .highlight(diagToRange);

      // If we're referring to a function with a return value (not Void) then
      // emit a fix-it suggesting to add `()` to call the function
      if (auto DRE = dyn_cast<DeclRefExpr>(fromExpr)) {
        if (auto FD = dyn_cast<FuncDecl>(DRE->getDecl())) {
          if (!FD->getResultInterfaceType()->isVoid()) {
            diags.diagnose(diagLoc, diag::downcast_to_unrelated_fixit,
                           FD->getBaseIdentifier())
                .fixItInsertAfter(fromExpr->getEndLoc(), "()");
          }
        }
      }

      return CheckedCastKind::ValueCast;

    case CheckedCastContextKind::IsPattern:
    case CheckedCastContextKind::EnumElementPattern:
    case CheckedCastContextKind::IsExpr:
    case CheckedCastContextKind::None:
    case CheckedCastContextKind::Coercion:
      break;
    }
  }

  // If we can bridge through an Objective-C class, do so.
  if (Type bridgedToClass = getDynamicBridgedThroughObjCClass(dc, fromType,
                                                              toType)) {
    switch (typeCheckCheckedCast(bridgedToClass, fromType,
                                 CheckedCastContextKind::None, dc, SourceLoc(),
                                 nullptr, SourceRange())) {
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
                                 CheckedCastContextKind::None, dc, SourceLoc(),
                                 nullptr, SourceRange())) {
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
    if (auto *protocolDecl =
          dyn_cast_or_null<ProtocolDecl>(fromType->getAnyNominal())) {
      if (!couldDynamicallyConformToProtocol(toType, protocolDecl, module)) {
        return failed();
      }
    } else if (auto protocolComposition =
                   fromType->getAs<ProtocolCompositionType>()) {
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
  auto clas = toType->getClassOrBoundGenericClass();
  if (clas && clas->getForeignClassKind() == ClassDecl::ForeignKind::CFType)
    return CheckedCastKind::ValueCast;
  
  // Don't warn on casts that change the generic parameters of ObjC generic
  // classes. This may be necessary to force-fit ObjC APIs that depend on
  // covariance, or for APIs where the generic parameter annotations in the
  // ObjC headers are inaccurate.
  if (clas && clas->usesObjCGenericsModel()) {
    if (fromType->getClassOrBoundGenericClass() == clas)
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
    return cast<TupleExpr>(call->getArg())->getElement(0);

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

bool
IsCallableNominalTypeRequest::evaluate(Evaluator &evaluator, CanType ty,
                                       DeclContext *dc) const {
  auto options = defaultMemberLookupOptions;
  options |= NameLookupFlags::IgnoreAccessControl;

  // Look for a callAsFunction method.
  auto &ctx = ty->getASTContext();
  auto results =
      TypeChecker::lookupMember(dc, ty, DeclNameRef(ctx.Id_callAsFunction),
                                options);
  return llvm::any_of(results, [](LookupResultEntry entry) -> bool {
    if (auto *fd = dyn_cast<FuncDecl>(entry.getValueDecl()))
      return fd->isCallAsFunctionMethod();
    return false;
  });
}

template <class DynamicAttribute>
static bool checkForDynamicAttribute(CanType ty,
                                     llvm::function_ref<bool (Type)> hasAttribute) {
  // If this is an archetype type, check if any types it conforms to
  // (superclass or protocols) have the attribute.
  if (auto archetype = dyn_cast<ArchetypeType>(ty)) {
    for (auto proto : archetype->getConformsTo()) {
      if (hasAttribute(proto->getDeclaredInterfaceType()))
        return true;
    }
    if (auto superclass = archetype->getSuperclass()) {
      if (hasAttribute(superclass))
        return true;
    }
  }

  // If this is a protocol composition, check if any of its members have the
  // attribute.
  if (auto protocolComp = dyn_cast<ProtocolCompositionType>(ty)) {
    for (auto member : protocolComp->getMembers()) {
      if (hasAttribute(member))
        return true;
    }
  }

  // Otherwise, this must be a nominal type.
  // Neither Dynamic member lookup nor Dynamic Callable doesn't
  // work for tuples, etc.
  auto nominal = ty->getAnyNominal();
  if (!nominal)
    return false;

  // If this type has the attribute on it, then yes!
  if (nominal->getAttrs().hasAttribute<DynamicAttribute>())
    return true;

  // Check the protocols the type conforms to.
  for (auto proto : nominal->getAllProtocols()) {
    if (hasAttribute(proto->getDeclaredInterfaceType()))
      return true;
  }

  // Check the superclass if present.
  if (auto classDecl = dyn_cast<ClassDecl>(nominal)) {
    if (auto superclass = classDecl->getSuperclass()) {
      if (hasAttribute(superclass))
        return true;
    }
  }
  return false;
}

bool
HasDynamicMemberLookupAttributeRequest::evaluate(Evaluator &evaluator,
                                                 CanType ty) const {
  return checkForDynamicAttribute<DynamicMemberLookupAttr>(ty, [](Type type) {
    return type->hasDynamicMemberLookupAttribute();
  });
}

bool
HasDynamicCallableAttributeRequest::evaluate(Evaluator &evaluator,
                                             CanType ty) const {
  return checkForDynamicAttribute<DynamicCallableAttr>(ty, [](Type type) {
    return type->hasDynamicCallableAttribute();
  });
}

bool swift::shouldTypeCheckInEnclosingExpression(ClosureExpr *expr) {
  return expr->hasSingleExpressionBody();
}

void swift::forEachExprInConstraintSystem(
    Expr *expr, llvm::function_ref<Expr *(Expr *)> callback) {
  struct ChildWalker : ASTWalker {
    llvm::function_ref<Expr *(Expr *)> callback;

    ChildWalker(llvm::function_ref<Expr *(Expr *)> callback)
    : callback(callback) {}

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      if (auto closure = dyn_cast<ClosureExpr>(E)) {
        if (!shouldTypeCheckInEnclosingExpression(closure))
          return { false, callback(E) };
      }
      return { true, callback(E) };
    }

    std::pair<bool, Pattern*> walkToPatternPre(Pattern *P) override {
      return { false, P };
    }
    bool walkToDeclPre(Decl *D) override { return false; }
    bool walkToTypeReprPre(TypeRepr *T) override { return false; }
  };

  expr->walk(ChildWalker(callback));
}
