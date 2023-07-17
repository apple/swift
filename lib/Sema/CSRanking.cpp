//===--- CSRanking.cpp - Constraint System Ranking ------------------------===//
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
// This file implements solution ranking heuristics for the
// constraint-based type checker.
//
//===----------------------------------------------------------------------===//
#include "TypeChecker.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Sema/ConstraintSystem.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Compiler.h"

using namespace swift;
using namespace constraints;

//===----------------------------------------------------------------------===//
// Statistics
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "Constraint solver overall"
STATISTIC(NumDiscardedSolutions, "Number of solutions discarded");

/// Returns \c true if \p expr takes a code completion expression as an
/// argument.
static bool exprHasCodeCompletionAsArgument(Expr *expr, ConstraintSystem &cs) {
  if (auto args = expr->getArgs()) {
    for (auto arg : *args) {
      if (isa<CodeCompletionExpr>(arg.getExpr())) {
        return true;
      }
    }
  }
  return false;
}

static bool shouldIgnoreScoreIncreaseForCodeCompletion(
    ScoreKind kind, ConstraintLocatorBuilder Locator, ConstraintSystem &cs) {
  if (kind < SK_SyncInAsync) {
    // We don't want to ignore score kinds that make the code invalid.
    return false;
  }
  auto expr = Locator.trySimplifyToExpr();
  if (!expr) {
    return false;
  }

  // These are a few hand-picked examples in which we don't want to increase the
  // score in code completion mode. Technically, to get all valid results, we
  // would like to not increase the score if the expression contains the code
  // completion token anywhere but that's not possible for performance reasons.
  // Thus, just special case the most common cases.

  // The code completion token itself.
  if (isa<CodeCompletionExpr>(expr)) {
    return true;
  }

  // An assignment where the LHS or RHS contains the code completion token (e.g.
  // an optional conversion).
  // E.g.
  // x[#^COMPLETE^#] = foo
  // let a = foo(#^COMPLETE^#)
  if (auto assign = dyn_cast<AssignExpr>(expr)) {
    if (exprHasCodeCompletionAsArgument(assign->getSrc(), cs)) {
      return true;
    } else if (exprHasCodeCompletionAsArgument(assign->getDest(), cs)) {
      return true;
    }
  }

  // If the function call takes the code completion token as an argument, the
  // call also shouldn't increase the score.
  // E.g. `foo` in
  // foo(#^COMPLETE^#)
  if (exprHasCodeCompletionAsArgument(expr, cs)) {
    return true;
  }

  // The sibling argument is the code completion expression, this allows e.g.
  // non-default literal values in sibling arguments.
  // E.g. we allow a 1 to be a double in
  // foo(1, #^COMPLETE^#)
  if (auto parent = cs.getParentExpr(expr)) {
    if (exprHasCodeCompletionAsArgument(parent, cs)) {
      return true;
    }
  }
  return false;
}

void ConstraintSystem::increaseScore(ScoreKind kind,
                                     ConstraintLocatorBuilder Locator,
                                     unsigned value) {
  if (isForCodeCompletion() &&
      shouldIgnoreScoreIncreaseForCodeCompletion(kind, Locator, *this)) {
    if (isDebugMode() && value > 0) {
      if (solverState)
        llvm::errs().indent(solverState->getCurrentIndent());
      llvm::errs() << "(not increasing '" << Score::getNameFor(kind)
      << "' score by " << value
      << " because of proximity to code completion token";
      Locator.dump(&getASTContext().SourceMgr, llvm::errs());
      llvm::errs() << ")\n";
    }
    return;
  }
  if (isDebugMode() && value > 0) {
    if (solverState)
      llvm::errs().indent(solverState->getCurrentIndent());
    llvm::errs() << "(increasing '" << Score::getNameFor(kind) << "' score by "
    << value << " @ ";
    Locator.dump(&getASTContext().SourceMgr, llvm::errs());
    llvm::errs() << ")\n";
  }

  unsigned index = static_cast<unsigned>(kind);
  CurrentScore.Data[index] += value;
}

bool ConstraintSystem::worseThanBestSolution() const {
  if (getASTContext().TypeCheckerOpts.DisableConstraintSolverPerformanceHacks)
    return false;

  if (!solverState || !solverState->BestScore ||
      CurrentScore <= *solverState->BestScore)
    return false;

  if (isDebugMode()) {
    llvm::errs().indent(solverState->getCurrentIndent())
        << "(solution is worse than the best solution)\n";
  }

  return true;
}

llvm::raw_ostream &constraints::operator<<(llvm::raw_ostream &out,
                                           const Score &score) {
  for (unsigned i = 0; i != NumScoreKinds; ++i) {
    if (i) out << ' ';
    out << score.Data[i];
  }
  return out;
}

///\ brief Compare two declarations for equality when they are used.
///
static bool sameDecl(Decl *decl1, Decl *decl2) {
  if (decl1 == decl2)
    return true;

  // All types considered identical.
  // FIXME: This is a hack. What we really want is to have substituted the
  // base type into the declaration reference, so that we can compare the
  // actual types to which two type declarations resolve. If those types are
  // equivalent, then it doesn't matter which declaration is chosen.
  if (isa<TypeDecl>(decl1) && isa<TypeDecl>(decl2))
    return true;
  
  if (decl1->getKind() != decl2->getKind())
    return false;

  return false;
}

/// Compare two overload choices for equality.
static bool sameOverloadChoice(const OverloadChoice &x,
                               const OverloadChoice &y) {
  if (x.getKind() != y.getKind())
    return false;

  switch (x.getKind()) {
  case OverloadChoiceKind::KeyPathApplication:
    // FIXME: Compare base types after substitution?
    return true;

  case OverloadChoiceKind::Decl:
  case OverloadChoiceKind::DeclViaDynamic:
  case OverloadChoiceKind::DeclViaBridge:
  case OverloadChoiceKind::DeclViaUnwrappedOptional:
  case OverloadChoiceKind::DynamicMemberLookup:
  case OverloadChoiceKind::KeyPathDynamicMemberLookup:
    return sameDecl(x.getDecl(), y.getDecl());

  case OverloadChoiceKind::TupleIndex:
    return x.getTupleIndex() == y.getTupleIndex();

  case OverloadChoiceKind::MaterializePack:
    return true;
  }

  llvm_unreachable("Unhandled OverloadChoiceKind in switch.");
}

namespace {
  /// Describes the relationship between the context types for two declarations.
  enum class SelfTypeRelationship {
    /// The types are unrelated; ignore the bases entirely.
    Unrelated,
    /// The types are equivalent.
    Equivalent,
    /// The first type is a subclass of the second.
    Subclass,
    /// The second type is a subclass of the first.
    Superclass,
    /// The first type conforms to the second
    ConformsTo,
    /// The second type conforms to the first.
    ConformedToBy
  };
} // end anonymous namespace

/// Determines whether the first type is nominally a superclass of the second
/// type, ignore generic arguments.
static bool isNominallySuperclassOf(Type type1, Type type2) {
  auto nominal1 = type1->getAnyNominal();
  if (!nominal1)
    return false;

  for (auto super2 = type2; super2; super2 = super2->getSuperclass()) {
    if (super2->getAnyNominal() == nominal1)
      return true;
  }

  return false;
}

/// Determine the relationship between the self types of the given declaration
/// contexts..
static std::pair<SelfTypeRelationship, ProtocolConformanceRef>
computeSelfTypeRelationship(DeclContext *dc, ValueDecl *decl1,
                            ValueDecl *decl2) {
  // If both declarations are operators, even through they
  // might have Self such types are unrelated.
  if (decl1->isOperator() && decl2->isOperator())
    return {SelfTypeRelationship::Unrelated, ProtocolConformanceRef()};

  auto *dc1 = decl1->getDeclContext();
  auto *dc2 = decl2->getDeclContext();

  // If at least one of the contexts is a non-type context, the two are
  // unrelated.
  if (!dc1->isTypeContext() || !dc2->isTypeContext())
    return {SelfTypeRelationship::Unrelated, ProtocolConformanceRef()};

  Type type1 = dc1->getDeclaredInterfaceType();
  Type type2 = dc2->getDeclaredInterfaceType();

  // If the types are equal, the answer is simple.
  if (type1->isEqual(type2))
    return {SelfTypeRelationship::Equivalent, ProtocolConformanceRef()};

  // If both types can have superclasses, which whether one is a superclass
  // of the other. The subclass is the common base type.
  if (type1->mayHaveSuperclass() && type2->mayHaveSuperclass()) {
    if (isNominallySuperclassOf(type1, type2))
      return {SelfTypeRelationship::Superclass, ProtocolConformanceRef()};

    if (isNominallySuperclassOf(type2, type1))
      return {SelfTypeRelationship::Subclass, ProtocolConformanceRef()};

    return {SelfTypeRelationship::Unrelated, ProtocolConformanceRef()};
  }

  // If neither or both are protocol types, consider the bases unrelated.
  bool isProtocol1 = isa<ProtocolDecl>(dc1);
  bool isProtocol2 = isa<ProtocolDecl>(dc2);
  if (isProtocol1 == isProtocol2)
    return {SelfTypeRelationship::Unrelated, ProtocolConformanceRef()};

  // Just one of the two is a protocol. Check whether the other conforms to
  // that protocol.
  Type protoTy = isProtocol1? type1 : type2;
  Type modelTy = isProtocol1? type2 : type1;
  auto proto = protoTy->castTo<ProtocolType>()->getDecl();

  // If the model type does not conform to the protocol, the bases are
  // unrelated.
  auto conformance = dc->getParentModule()->lookupConformance(modelTy, proto);
  if (conformance.isInvalid())
    return {SelfTypeRelationship::Unrelated, conformance};

  if (isProtocol1)
    return {SelfTypeRelationship::ConformedToBy, conformance};

  return {SelfTypeRelationship::ConformsTo, conformance};
}

/// Given two generic function declarations, signal if the first is more
/// "constrained" than the second by comparing the number of constraints
/// applied to each type parameter.
/// Note that this is not a subtype or conversion check - that takes place
/// in isDeclAsSpecializedAs.
static bool isDeclMoreConstrainedThan(ValueDecl *decl1, ValueDecl *decl2) {
  
  if (decl1->getKind() != decl2->getKind() || isa<TypeDecl>(decl1))
    return false;

  bool bothGeneric = false;
  GenericSignature sig1, sig2;

  auto func1 = dyn_cast<FuncDecl>(decl1);
  auto func2 = dyn_cast<FuncDecl>(decl2);
  if (func1 && func2) {
    bothGeneric = func1->isGeneric() && func2->isGeneric();

    sig1 = func1->getGenericSignature();
    sig2 = func2->getGenericSignature();
  }

  auto subscript1 = dyn_cast<SubscriptDecl>(decl1);
  auto subscript2 = dyn_cast<SubscriptDecl>(decl2);
  if (subscript1 && subscript2) {
    bothGeneric = subscript1->isGeneric() && subscript2->isGeneric();

    sig1 = subscript1->getGenericSignature();
    sig2 = subscript2->getGenericSignature();
  }

  if (bothGeneric) {
    auto params1 = sig1.getInnermostGenericParams();
    auto params2 = sig2.getInnermostGenericParams();
      
    if (params1.size() == params2.size()) {
      for (size_t i = 0; i < params1.size(); i++) {
        auto p1 = params1[i];
        auto p2 = params2[i];
          
        int np1 = static_cast<int>(sig1->getRequiredProtocols(p1).size());
        int np2 = static_cast<int>(sig2->getRequiredProtocols(p2).size());
        int aDelta = np1 - np2;
          
        if (aDelta)
          return aDelta > 0;
      }
    }
  }
  
  return false;
}

/// Determine whether one protocol extension is at least as specialized as
/// another.
static bool isProtocolExtensionAsSpecializedAs(DeclContext *dc1,
                                               DeclContext *dc2) {
  assert(dc1->getExtendedProtocolDecl());
  assert(dc2->getExtendedProtocolDecl());

  // If one of the protocols being extended inherits the other, prefer the
  // more specialized protocol.
  auto proto1 = dc1->getExtendedProtocolDecl();
  auto proto2 = dc2->getExtendedProtocolDecl();
  if (proto1 != proto2) {
    if (proto1->inheritsFrom(proto2))
      return true;
    if (proto2->inheritsFrom(proto1))
      return false;
  }


  // If the two generic signatures are identical, neither is as specialized
  // as the other.
  GenericSignature sig1 = dc1->getGenericSignatureOfContext();
  GenericSignature sig2 = dc2->getGenericSignatureOfContext();
  if (sig1.getCanonicalSignature() == sig2.getCanonicalSignature())
    return false;

  // Form a constraint system where we've opened up all of the requirements of
  // the second protocol extension.
  ConstraintSystem cs(dc1, llvm::None);
  OpenedTypeMap replacements;
  cs.openGeneric(dc2, sig2, ConstraintLocatorBuilder(nullptr), replacements);

  // Bind the 'Self' type from the first extension to the type parameter from
  // opening 'Self' of the second extension.
  Type selfType1 = sig1.getGenericParams()[0];
  Type selfType2 = sig2.getGenericParams()[0];
  cs.addConstraint(ConstraintKind::Bind,
                   replacements[cast<GenericTypeParamType>(selfType2->getCanonicalType())],
                   dc1->mapTypeIntoContext(selfType1),
                   nullptr);

  // Solve the system. If the first extension is at least as specialized as the
  // second, we're done.
  return cs.solveSingle().has_value();
}

/// Retrieve the adjusted parameter type for overloading purposes.
static Type getAdjustedParamType(const AnyFunctionType::Param &param) {
  auto type = param.getOldType();
  if (param.isAutoClosure())
    return type->castTo<FunctionType>()->getResult();
  return type;
}

// Is a particular parameter of a function or subscript declaration
// declared to be an IUO?
static bool paramIsIUO(const ValueDecl *decl, int paramNum) {
  return swift::getParameterAt(decl, paramNum)
    ->isImplicitlyUnwrappedOptional();
}

/// Determine whether the first declaration is as "specialized" as
/// the second declaration.
///
/// "Specialized" is essentially a form of subtyping, defined below.
static bool isDeclAsSpecializedAs(DeclContext *dc, ValueDecl *decl1,
                                  ValueDecl *decl2,
                                  bool isDynamicOverloadComparison = false) {
  return evaluateOrDefault(decl1->getASTContext().evaluator,
                           CompareDeclSpecializationRequest{
                               dc, decl1, decl2, isDynamicOverloadComparison},
                           false);
}

bool CompareDeclSpecializationRequest::evaluate(
    Evaluator &eval, DeclContext *dc, ValueDecl *decl1, ValueDecl *decl2,
    bool isDynamicOverloadComparison) const {
  auto &C = decl1->getASTContext();
  // Construct a constraint system to compare the two declarations.
  ConstraintSystem cs(dc, ConstraintSystemOptions());
  if (cs.isDebugMode()) {
    llvm::errs() << "Comparing declarations\n";
    decl1->print(llvm::errs());
    llvm::errs() << "\nand\n";
    decl2->print(llvm::errs());
    llvm::errs() << "\n(isDynamicOverloadComparison: ";
    llvm::errs() << isDynamicOverloadComparison;
    llvm::errs() << ")\n";
  }

  auto completeResult = [&cs](bool result) {
    if (cs.isDebugMode()) {
      llvm::errs() << "comparison result: "
                   << (result ? "better" : "not better")
                   << "\n";
    }
    return result;
  };

  auto *innerDC1 = decl1->getInnermostDeclContext();
  auto *innerDC2 = decl2->getInnermostDeclContext();

  auto *outerDC1 = decl1->getDeclContext();
  auto *outerDC2 = decl2->getDeclContext();

  // If the kinds are different, there's nothing we can do.
  // FIXME: This is wrong for type declarations, which we're skipping
  // entirely.
  if (decl1->getKind() != decl2->getKind() || isa<TypeDecl>(decl1))
    return completeResult(false);

  // A non-generic declaration is more specialized than a generic declaration.
  if (auto func1 = dyn_cast<AbstractFunctionDecl>(decl1)) {
    auto func2 = cast<AbstractFunctionDecl>(decl2);
    if (func1->isGeneric() != func2->isGeneric())
      return completeResult(func2->isGeneric());
  }

  if (auto subscript1 = dyn_cast<SubscriptDecl>(decl1)) {
    auto subscript2 = cast<SubscriptDecl>(decl2);
    if (subscript1->isGeneric() != subscript2->isGeneric())
      return completeResult(subscript2->isGeneric());
  }

  // Members of protocol extensions have special overloading rules.
  ProtocolDecl *inProtocolExtension1 = outerDC1->getExtendedProtocolDecl();
  ProtocolDecl *inProtocolExtension2 = outerDC2->getExtendedProtocolDecl();
  if (inProtocolExtension1 && inProtocolExtension2) {
    // Both members are in protocol extensions.
    // Determine whether the 'Self' type from the first protocol extension
    // satisfies all of the requirements of the second protocol extension.
    bool better1 = isProtocolExtensionAsSpecializedAs(outerDC1, outerDC2);
    bool better2 = isProtocolExtensionAsSpecializedAs(outerDC2, outerDC1);
    if (better1 != better2) {
      return completeResult(better1);
    }
  } else if (inProtocolExtension1 || inProtocolExtension2) {
    // One member is in a protocol extension, the other is in a concrete type.
    // Prefer the member in the concrete type.
    return completeResult(inProtocolExtension2);
  }

  // A concrete type member is always more specialised than a protocol
  // member (bearing in mind that we have already handled the case where
  // exactly one member is in a protocol extension). Only apply this rule in
  // Swift 5 mode to better maintain source compatibility under Swift 4
  // mode.
  //
  // Don't apply this rule when comparing two overloads found through
  // dynamic lookup to ensure we keep cases like this ambiguous:
  //
  //    @objc protocol P {
  //      var i: String { get }
  //    }
  //    class C {
  //      @objc var i: Int { return 0 }
  //    }
  //    func foo(_ x: AnyObject) {
  //      x.i // ensure ambiguous.
  //    }
  //
  if (C.isSwiftVersionAtLeast(5) && !isDynamicOverloadComparison) {
    auto inProto1 = isa<ProtocolDecl>(outerDC1);
    auto inProto2 = isa<ProtocolDecl>(outerDC2);
    if (inProto1 != inProto2)
      return completeResult(inProto2);
  }

  Type type1 = decl1->getInterfaceType();
  Type type2 = decl2->getInterfaceType();

  // Add curried 'self' types if necessary.
  if (!decl1->hasCurriedSelf())
    type1 = type1->addCurriedSelfType(outerDC1);

  if (!decl2->hasCurriedSelf())
    type2 = type2->addCurriedSelfType(outerDC2);

  auto openType = [&](ConstraintSystem &cs, DeclContext *innerDC,
                      DeclContext *outerDC, Type type,
                      OpenedTypeMap &replacements,
                      ConstraintLocator *locator) -> Type {
    if (auto *funcType = type->getAs<AnyFunctionType>()) {
      return cs.openFunctionType(funcType, locator, replacements, outerDC);
    }

    cs.openGeneric(outerDC, innerDC->getGenericSignatureOfContext(), locator,
                   replacements);

    return cs.openType(type, replacements, locator);
  };

  bool knownNonSubtype = false;

  auto *locator = cs.getConstraintLocator({});
  // FIXME: Locator when anchored on a declaration.
  // Get the type of a reference to the second declaration.

  OpenedTypeMap unused, replacements;
  auto openedType2 = openType(cs, innerDC1, outerDC2, type2, unused, locator);
  auto openedType1 =
      openType(cs, innerDC2, outerDC1, type1, replacements, locator);

  for (const auto &replacement : replacements) {
    if (auto mapped = innerDC1->mapTypeIntoContext(replacement.first)) {
      cs.addConstraint(ConstraintKind::Bind, replacement.second, mapped,
                       locator);
    }
  }

  // Extract the self types from the declarations, if they have them.
  auto getSelfType = [](AnyFunctionType *fnType) -> Type {
    auto params = fnType->getParams();
    assert(params.size() == 1);
    return params.front().getPlainType()->getMetatypeInstanceType();
  };

  Type selfTy1;
  Type selfTy2;
  if (outerDC1->isTypeContext()) {
    auto funcTy1 = openedType1->castTo<FunctionType>();
    selfTy1 = getSelfType(funcTy1);
    openedType1 = funcTy1->getResult();
  }
  if (outerDC2->isTypeContext()) {
    auto funcTy2 = openedType2->castTo<FunctionType>();
    selfTy2 = getSelfType(funcTy2);
    openedType2 = funcTy2->getResult();
  }

  // Determine the relationship between the 'self' types and add the
  // appropriate constraints. The constraints themselves never fail, but
  // they help deduce type variables that were opened.
  auto selfTypeRelationship = computeSelfTypeRelationship(dc, decl1, decl2);
  auto relationshipKind = selfTypeRelationship.first;
  auto conformance = selfTypeRelationship.second;
  (void)conformance;
  switch (relationshipKind) {
  case SelfTypeRelationship::Unrelated:
    // Skip the self types parameter entirely.
    break;

  case SelfTypeRelationship::Equivalent:
    cs.addConstraint(ConstraintKind::Bind, selfTy1, selfTy2, locator);
    break;

  case SelfTypeRelationship::Subclass:
    cs.addConstraint(ConstraintKind::Subtype, selfTy1, selfTy2, locator);
    break;

  case SelfTypeRelationship::Superclass:
    cs.addConstraint(ConstraintKind::Subtype, selfTy2, selfTy1, locator);
    break;

  case SelfTypeRelationship::ConformsTo:
    assert(conformance);
    cs.addConstraint(ConstraintKind::ConformsTo, selfTy1,
                     cast<ProtocolDecl>(outerDC2)->getDeclaredInterfaceType(),
                     locator);
    break;

  case SelfTypeRelationship::ConformedToBy:
    assert(conformance);
    cs.addConstraint(ConstraintKind::ConformsTo, selfTy2,
                     cast<ProtocolDecl>(outerDC1)->getDeclaredInterfaceType(),
                     locator);
    break;
  }

  bool fewerEffectiveParameters = false;
  if (!decl1->hasParameterList() && !decl2->hasParameterList()) {
    // If neither decl has a parameter list, simply check whether the first
    // type is a subtype of the second.
    cs.addConstraint(ConstraintKind::Subtype, openedType1, openedType2,
                     locator);
  } else if (decl1->hasParameterList() && decl2->hasParameterList()) {
    // Otherwise, check whether the first function type's input is a subtype
    // of the second type's inputs, i.e., can we forward the arguments?
    auto funcTy1 = openedType1->castTo<FunctionType>();
    auto funcTy2 = openedType2->castTo<FunctionType>();
    auto params1 = funcTy1->getParams();
    auto params2 = funcTy2->getParams();

    unsigned numParams1 = params1.size();
    unsigned numParams2 = params2.size();
    if (numParams1 > numParams2)
      return completeResult(false);

    // If they both have trailing closures, compare those separately.
    bool compareTrailingClosureParamsSeparately = false;
    if (numParams1 > 0 && numParams2 > 0 &&
        params1.back().getParameterType()->is<AnyFunctionType>() &&
        params2.back().getParameterType()->is<AnyFunctionType>()) {
      compareTrailingClosureParamsSeparately = true;
    }

    auto maybeAddSubtypeConstraint =
        [&](const AnyFunctionType::Param &param1,
            const AnyFunctionType::Param &param2) -> bool {
      // If one parameter is variadic and the other is not...
      if (param1.isVariadic() != param2.isVariadic()) {
        // If the first parameter is the variadic one, it's not
        // more specialized.
        if (param1.isVariadic())
          return false;

        fewerEffectiveParameters = true;
      }

      Type paramType1 = getAdjustedParamType(param1);
      Type paramType2 = getAdjustedParamType(param2);

      // Check whether the first parameter is a subtype of the second.
      cs.addConstraint(ConstraintKind::Subtype, paramType1, paramType2,
                       locator);
      return true;
    };

    auto pairMatcher = [&](unsigned idx1, unsigned idx2) -> bool {
      // Emulate behavior from when IUO was a type, where IUOs
      // were considered subtypes of plain optionals, but not
      // vice-versa.  This wouldn't normally happen, but there are
      // cases where we can rename imported APIs so that we have a
      // name collision, and where the parameter type(s) are the
      // same except for details of the kind of optional declared.
      auto param1IsIUO = paramIsIUO(decl1, idx1);
      auto param2IsIUO = paramIsIUO(decl2, idx2);
      if (param2IsIUO && !param1IsIUO)
        return false;

      if (!maybeAddSubtypeConstraint(params1[idx1], params2[idx2]))
        return false;

      return true;
    };

    ParameterListInfo paramInfo(params2, decl2, decl2->hasCurriedSelf());
    auto params2ForMatching = params2;
    if (compareTrailingClosureParamsSeparately) {
      --numParams1;
      params2ForMatching = params2.drop_back();
    }

    InputMatcher IM(params2ForMatching, paramInfo);
    if (IM.match(numParams1, pairMatcher) != InputMatcher::IM_Succeeded)
      return completeResult(false);

    fewerEffectiveParameters |= (IM.getNumSkippedParameters() != 0);

    if (compareTrailingClosureParamsSeparately)
      if (!maybeAddSubtypeConstraint(params1.back(), params2.back()))
        knownNonSubtype = true;
  }

  if (!knownNonSubtype) {
    // Solve the system.
    auto solution = cs.solveSingle(FreeTypeVariableBinding::Allow);

    // Ban value-to-optional conversions.
    if (solution && solution->getFixedScore().Data[SK_ValueToOptional] == 0)
      return completeResult(true);
  }

  // If the first function has fewer effective parameters than the
  // second, it is more specialized.
  if (fewerEffectiveParameters)
    return completeResult(true);

  return completeResult(false);
}

Comparison TypeChecker::compareDeclarations(DeclContext *dc,
                                            ValueDecl *decl1,
                                            ValueDecl *decl2){
  bool decl1Better = isDeclAsSpecializedAs(dc, decl1, decl2);
  bool decl2Better = isDeclAsSpecializedAs(dc, decl2, decl1);

  if (decl1Better == decl2Better)
    return Comparison::Unordered;

  return decl1Better ? Comparison::Better : Comparison::Worse;
}

static Type getUnlabeledType(Type type, ASTContext &ctx) {
  return type.transform([&](Type type) -> Type {
    if (auto *tupleType = dyn_cast<TupleType>(type.getPointer())) {
      if (tupleType->getNumElements() == 1)
        return ParenType::get(ctx, tupleType->getElementType(0));

      SmallVector<TupleTypeElt, 8> elts;
      for (auto elt : tupleType->getElements()) {
        elts.push_back(elt.getWithoutName());
      }

      return TupleType::get(elts, ctx);
    }

    return type;
  });
}

static void addKeyPathDynamicMemberOverloads(
    ArrayRef<Solution> solutions, unsigned idx1, unsigned idx2,
    SmallVectorImpl<SolutionDiff::OverloadDiff> &overloadDiff) {
  const auto &overloads1 = solutions[idx1].overloadChoices;
  const auto &overloads2 = solutions[idx2].overloadChoices;

  for (auto &entry : overloads1) {
    auto *locator = entry.first;
    if (!locator->isForKeyPathDynamicMemberLookup())
      continue;

    auto overload2 = overloads2.find(locator);
    if (overload2 == overloads2.end())
      continue;

    auto &overloadChoice1 = entry.second.choice;
    auto &overloadChoice2 = overload2->second.choice;

    SmallVector<OverloadChoice, 4> choices;
    choices.resize(solutions.size());

    choices[idx1] = overloadChoice1;
    choices[idx2] = overloadChoice2;

    overloadDiff.push_back(
        SolutionDiff::OverloadDiff{locator, std::move(choices)});
  }
}

namespace {
/// A set of type variable bindings to compare for ranking.
struct TypeBindingsToCompare {
  Type Type1;
  Type Type2;

  // These bits are used in the case where we need to compare a lone unlabeled
  // parameter with a labeled parameter, and allow us to prefer the unlabeled
  // one.
  bool Type1WasLabeled = false;
  bool Type2WasLabeled = false;

  TypeBindingsToCompare(Type type1, Type type2)
      : Type1(type1), Type2(type2) {}

  /// Whether the type bindings to compare are known to be the same.
  bool areSameTypes() const {
    return !Type1WasLabeled && !Type2WasLabeled && Type1->isEqual(Type2);
  }
};
} // end anonymous namespace

/// Given the bound types of two constructor overloads, returns their parameter
/// list types as tuples to compare for solution ranking, or \c None if they
/// shouldn't be compared.
static llvm::Optional<TypeBindingsToCompare>
getConstructorParamsAsTuples(ASTContext &ctx, Type boundTy1, Type boundTy2) {
  auto choiceTy1 =
      boundTy1->lookThroughAllOptionalTypes()->getAs<FunctionType>();
  auto choiceTy2 =
      boundTy2->lookThroughAllOptionalTypes()->getAs<FunctionType>();

  // If the type variables haven't been bound to functions yet, let's not try
  // and rank them.
  if (!choiceTy1 || !choiceTy2)
    return llvm::None;

  auto initParams1 = choiceTy1->getParams();
  auto initParams2 = choiceTy2->getParams();
  if (initParams1.size() != initParams2.size())
    return llvm::None;

  // Don't compare if there are variadic differences. This preserves the
  // behavior of when we'd compare through matchTupleTypes with the parameter
  // flags intact.
  for (auto idx : indices(initParams1)) {
    if (initParams1[idx].isVariadic() != initParams2[idx].isVariadic())
      return llvm::None;
  }

  // Awful hack needed to preserve source compatibility: If we have single
  // variadic parameters to compare, where one has a label and the other does
  // not, e.g (x: Int...) and (Int...), compare the parameter types by
  // themselves, and make a note of which one has the label.
  //
  // This is needed because previously we would build a TupleType for a single
  // unlabeled variadic parameter (Int...), which would let us compare it with
  // a labeled parameter (x: Int...) and prefer the unlabeled version. With the
  // parameter flags stripped however, (Int...) would become a paren type,
  // which we wouldn't compare with the tuple type (x: Int...). To preserve the
  // previous behavior in this case, just do a type comparison for the param
  // types, and record where we stripped a label. The ranking logic can then use
  // this to prefer the unlabeled variant. This is only needed in the single
  // parameter case, as other cases will compare as tuples the same as before.
  // In cases where variadics aren't used, we may end up trying to compare
  // parens with tuples, but that's consistent with what we previously did.
  //
  // Note we can just do checks on initParams1, as we've already established
  // sizes and variadic bits are consistent.
  if (initParams1.size() == 1 && initParams1[0].isVariadic() &&
      initParams1[0].hasLabel() != initParams2[0].hasLabel()) {
    TypeBindingsToCompare bindings(initParams1[0].getParameterType(),
                                   initParams2[0].getParameterType());
    if (initParams1[0].hasLabel()) {
      bindings.Type1WasLabeled = true;
    } else {
      bindings.Type2WasLabeled = true;
    }
    return bindings;
  }

  auto tuple1 = AnyFunctionType::composeTuple(
      ctx, initParams1, ParameterFlagHandling::IgnoreNonEmpty);
  auto tuple2 = AnyFunctionType::composeTuple(
      ctx, initParams2, ParameterFlagHandling::IgnoreNonEmpty);
  return TypeBindingsToCompare(tuple1, tuple2);
}

SolutionCompareResult ConstraintSystem::compareSolutions(
    ConstraintSystem &cs, ArrayRef<Solution> solutions,
    const SolutionDiff &diff, unsigned idx1, unsigned idx2) {
  if (cs.isDebugMode()) {
    llvm::errs().indent(cs.solverState->getCurrentIndent())
        << "comparing solutions " << idx1 << " and " << idx2 << "\n";
  }

  // Whether the solutions are identical.
  bool identical = true;

  // Compare the fixed scores by themselves.
  if (solutions[idx1].getFixedScore() != solutions[idx2].getFixedScore()) {
    return solutions[idx1].getFixedScore() < solutions[idx2].getFixedScore()
             ? SolutionCompareResult::Better
             : SolutionCompareResult::Worse;
  }
  
  // Compute relative score.
  unsigned score1 = 0;
  unsigned score2 = 0;
  
  auto foundRefinement1 = false;
  auto foundRefinement2 = false;

  bool isStdlibOptionalMPlusOperator1 = false;
  bool isStdlibOptionalMPlusOperator2 = false;

  bool isVarAndNotProtocol1 = false;
  bool isVarAndNotProtocol2 = false;

  auto getWeight = [&](ConstraintLocator *locator) -> unsigned {
    if (auto *anchor = locator->getAnchor().dyn_cast<Expr *>()) {
      auto weight = cs.getExprDepth(anchor);
      if (weight)
        return *weight + 1;
    }

    return 1;
  };

  SmallVector<SolutionDiff::OverloadDiff, 4> overloadDiff(diff.overloads);
  // Single type of keypath dynamic member lookup could refer to different
  // member overloads, we have to do a pair-wise comparison in such cases
  // otherwise ranking would miss some viable information e.g.
  // `_ = arr[0..<3]` could refer to subscript through writable or read-only
  // key path and each of them could also pick overload which returns `Slice<T>`
  // or `ArraySlice<T>` (assuming that `arr` is something like `Box<[Int]>`).
  addKeyPathDynamicMemberOverloads(solutions, idx1, idx2, overloadDiff);

  // Compare overload sets.
  for (auto &overload : overloadDiff) {
    unsigned weight = getWeight(overload.locator);

    auto choice1 = overload.choices[idx1];
    auto choice2 = overload.choices[idx2];

    // If the systems made the same choice, there's nothing interesting here.
    if (sameOverloadChoice(choice1, choice2))
      continue;

    // If constraint system is underconstrained e.g. because there are
    // editor placeholders, it's possible to end up with multiple solutions
    // where each ambiguous declaration is going to have its own overload kind:
    //
    // func foo(_: Int) -> [Int] { ... }
    // func foo(_: Double) -> (result: String, count: Int) { ... }
    //
    // _ = foo(<#arg#>).count
    //
    // In this case solver would produce 2 solutions: one where `count`
    // is a property reference on `[Int]` and another one is tuple access
    // for a `count:` element.
    if (choice1.isDecl() != choice2.isDecl())
      return SolutionCompareResult::Incomparable;

    auto decl1 = choice1.getDecl();
    auto dc1 = decl1->getDeclContext();
    auto decl2 = choice2.getDecl();
    auto dc2 = decl2->getDeclContext();

    // The two systems are not identical. If the decls in question are distinct
    // protocol members, let the checks below determine if the two choices are
    // 'identical' or not. This allows us to structurally unify disparate
    // protocol members during overload resolution.
    // FIXME: Along with the FIXME below, this is a hack to work around
    // problems with restating requirements in protocols.
    identical = false;

    if (cs.Context.CompletionCallback) {
      // Don't rank based on overload choices of function calls that contain the
      // code completion token.
      ASTNode anchor = simplifyLocatorToAnchor(overload.locator);
      if (auto expr = getAsExpr(anchor)) {
        // If the anchor is a called function, also don't rank overload choices
        // if any of the arguments contain the code completion token.
        if (auto apply = dyn_cast_or_null<ApplyExpr>(cs.getParentExpr(expr))) {
          if (apply->getFn() == expr) {
            anchor = apply;
          }
        }
      }
      if (anchor && cs.containsIDEInspectionTarget(anchor)) {
        continue;
      }
    }

    bool decl1InSubprotocol = false;
    bool decl2InSubprotocol = false;
    if (dc1->getContextKind() == DeclContextKind::GenericTypeDecl &&
        dc1->getContextKind() == dc2->getContextKind()) {
      auto pd1 = dyn_cast<ProtocolDecl>(dc1);
      auto pd2 = dyn_cast<ProtocolDecl>(dc2);

      // FIXME: This hack tells us to prefer members of subprotocols over
      // those of the protocols they inherit, if all else fails.
      // If we were properly handling overrides of protocol members when
      // requirements get restated, it would not be necessary.
      if (pd1 && pd2 && pd1 != pd2) {
        identical = true;
        decl1InSubprotocol = pd1->inheritsFrom(pd2);
        decl2InSubprotocol = pd2->inheritsFrom(pd1);
      }
    }
    
    // If the kinds of overload choice don't match...
    if (choice1.getKind() != choice2.getKind()) {
      identical = false;
      
      // A declaration found directly beats any declaration found via dynamic
      // lookup, bridging, or optional unwrapping.
      if ((choice1.getKind() == OverloadChoiceKind::Decl) &&
          (choice2.getKind() == OverloadChoiceKind::DeclViaDynamic ||
           choice2.getKind() == OverloadChoiceKind::DeclViaBridge ||
           choice2.getKind() == OverloadChoiceKind::DeclViaUnwrappedOptional)) {
        score1 += weight;
        continue;
      }

      if ((choice1.getKind() == OverloadChoiceKind::DeclViaDynamic ||
           choice1.getKind() == OverloadChoiceKind::DeclViaBridge ||
           choice1.getKind() == OverloadChoiceKind::DeclViaUnwrappedOptional) &&
          choice2.getKind() == OverloadChoiceKind::Decl) {
        score2 += weight;
        continue;
      }

      if (choice1.getKind() == OverloadChoiceKind::KeyPathDynamicMemberLookup) {
        if (choice2.getKind() == OverloadChoiceKind::DynamicMemberLookup)
          // Dynamic member lookup through a keypath is better than one using
          // string because it carries more type information.
          score1 += weight;
        else
          // Otherwise let's prefer non-dynamic declaration.
          score2 += weight;

        continue;
      }

      if (choice2.getKind() == OverloadChoiceKind::KeyPathDynamicMemberLookup) {
        if (choice1.getKind() == OverloadChoiceKind::DynamicMemberLookup)
          // Dynamic member lookup through a keypath is better than one using
          // string because it carries more type information.
          score2 += weight;
        else
          // Otherwise let's prefer non-dynamic declaration.
          score1 += weight;

        continue;
      }

      continue;
    }

    // The kinds of overload choice match, but the contents don't.
    switch (choice1.getKind()) {
    case OverloadChoiceKind::TupleIndex:
    case OverloadChoiceKind::MaterializePack:
      continue;

    case OverloadChoiceKind::KeyPathApplication:
      llvm_unreachable("Never considered different");

    case OverloadChoiceKind::DeclViaDynamic:
    case OverloadChoiceKind::Decl:
    case OverloadChoiceKind::DeclViaBridge:
    case OverloadChoiceKind::DeclViaUnwrappedOptional:
    case OverloadChoiceKind::DynamicMemberLookup:
    case OverloadChoiceKind::KeyPathDynamicMemberLookup:
      break;
    }

    // We don't apply some ranking rules to overloads found through dynamic
    // lookup in order to keep a few potentially ill-formed cases ambiguous.
    bool isDynamicOverloadComparison =
        choice1.getKind() == OverloadChoiceKind::DeclViaDynamic &&
        choice2.getKind() == OverloadChoiceKind::DeclViaDynamic;

    // Determine whether one declaration is more specialized than the other.
    bool firstAsSpecializedAs = false;
    bool secondAsSpecializedAs = false;
    if (isDeclAsSpecializedAs(cs.DC, decl1, decl2,
                              isDynamicOverloadComparison)) {
      score1 += weight;
      firstAsSpecializedAs = true;
    }
    if (isDeclAsSpecializedAs(cs.DC, decl2, decl1,
                              isDynamicOverloadComparison)) {
      score2 += weight;
      secondAsSpecializedAs = true;
    }

    // If each is as specialized as the other, and both are constructors,
    // check the constructor kind.
    if (firstAsSpecializedAs && secondAsSpecializedAs) {
      if (auto ctor1 = dyn_cast<ConstructorDecl>(decl1)) {
        if (auto ctor2 = dyn_cast<ConstructorDecl>(decl2)) {
          if (ctor1->getInitKind() != ctor2->getInitKind()) {
            if (ctor1->getInitKind() < ctor2->getInitKind())
              score1 += weight;
            else
              score2 += weight;
          } else if (ctor1->getInitKind() ==
                     CtorInitializerKind::Convenience) {
            
            // If both are convenience initializers, and the instance type of
            // one is a subtype of the other's, favor the subtype constructor.
            auto resType1 = ctor1->mapTypeIntoContext(
                ctor1->getResultInterfaceType());
            auto resType2 = ctor2->mapTypeIntoContext(
                ctor2->getResultInterfaceType());
            
            if (!resType1->isEqual(resType2)) {
              if (TypeChecker::isSubtypeOf(resType1, resType2, cs.DC)) {
                score1 += weight;
              } else if (TypeChecker::isSubtypeOf(resType2, resType1, cs.DC)) {
                score2 += weight;
              }
            }
          }
        }
      }
    }

    // If both declarations come from Clang, and one is a type and the other
    // is a function, prefer the function.
    if (decl1->hasClangNode() &&
        decl2->hasClangNode() &&
        ((isa<TypeDecl>(decl1) &&
          isa<AbstractFunctionDecl>(decl2)) ||
         (isa<AbstractFunctionDecl>(decl1) &&
          isa<TypeDecl>(decl2)))) {
      if (isa<TypeDecl>(decl1))
        score2 += weight;
      else
        score1 += weight;
    }

    // A class member is always better than a curried instance member.
    // If the members agree on instance-ness, a property is better than a
    // method (because a method is usually immediately invoked).
    if (!decl1->isInstanceMember() && decl2->isInstanceMember())
      score1 += weight;
    else if (!decl2->isInstanceMember() && decl1->isInstanceMember())
      score2 += weight;
    else if (isa<VarDecl>(decl1) && isa<FuncDecl>(decl2))
      score1 += weight;
    else if (isa<VarDecl>(decl2) && isa<FuncDecl>(decl1))
      score2 += weight;

    // If both are class properties with the same name, prefer
    // the one attached to the subclass because it could only be
    // found if requested directly.
    if (!decl1->isInstanceMember() && !decl2->isInstanceMember()) {
      if (isa<VarDecl>(decl1) && isa<VarDecl>(decl2)) {
        auto *nominal1 = dc1->getSelfNominalTypeDecl();
        auto *nominal2 = dc2->getSelfNominalTypeDecl();

        if (nominal1 && nominal2 && nominal1 != nominal2) {
          auto base1 = nominal1->getDeclaredType();
          auto base2 = nominal2->getDeclaredType();

          if (isNominallySuperclassOf(base1, base2))
            score2 += weight;

          if (isNominallySuperclassOf(base2, base1))
            score1 += weight;
        }
      }
    }

    // If we haven't found a refinement, record whether one overload is in
    // any way more constrained than another. We'll only utilize this
    // information in the case of a potential ambiguity.
    if (!(foundRefinement1 && foundRefinement2)) {
      if (isDeclMoreConstrainedThan(decl1, decl2)) {
        foundRefinement1 = true;
      }
      
      if (isDeclMoreConstrainedThan(decl2, decl1)) {
        foundRefinement2 = true;
      }
    }

    // FIXME: The rest of the hack for restating requirements.
    if (!(foundRefinement1 && foundRefinement2)) {
      if (identical && decl1InSubprotocol != decl2InSubprotocol) {
        foundRefinement1 = decl1InSubprotocol;
        foundRefinement2 = decl2InSubprotocol;
      }
    }

    // Swift 4.1 compatibility hack: If everything else is considered equal,
    // favour a property on a concrete type over a protocol property member.
    //
    // This hack is required due to changes in shadowing behaviour where a
    // protocol property member will no longer shadow a property on a concrete
    // type, which created unintentional ambiguities in 4.2. This hack ensures
    // we at least keep these cases unambiguous in Swift 5 under Swift 4
    // compatibility mode. Don't however apply this hack for decls found through
    // dynamic lookup, as we want the user to have to disambiguate those.
    //
    // This is intentionally narrow in order to best preserve source
    // compatibility under Swift 4 mode by ensuring we don't introduce any new
    // ambiguities. This will become a more general "is more specialised" rule
    // in Swift 5 mode.
    if (!cs.getASTContext().isSwiftVersionAtLeast(5) &&
        choice1.getKind() != OverloadChoiceKind::DeclViaDynamic &&
        choice2.getKind() != OverloadChoiceKind::DeclViaDynamic &&
        isa<VarDecl>(decl1) && isa<VarDecl>(decl2)) {
      auto *nominal1 = dc1->getSelfNominalTypeDecl();
      auto *nominal2 = dc2->getSelfNominalTypeDecl();
      if (nominal1 && nominal2 && nominal1 != nominal2) {
        isVarAndNotProtocol1 = !isa<ProtocolDecl>(nominal1);
        isVarAndNotProtocol2 = !isa<ProtocolDecl>(nominal2);
      }
    }

    // FIXME: Lousy hack for ?? to prefer the catamorphism (flattening)
    // over the mplus (non-flattening) overload if all else is equal.
    if (decl1->getBaseName() == "??") {
      assert(decl2->getBaseName() == "??");

      auto check = [](const ValueDecl *VD) -> bool {
        if (!VD->getModuleContext()->isStdlibModule())
          return false;
        auto fnTy = VD->getInterfaceType()->castTo<AnyFunctionType>();
        if (!fnTy->getResult()->getOptionalObjectType())
          return false;

        // Check that the standard library hasn't added another overload of
        // the ?? operator.
        auto params = fnTy->getParams();
        assert(params.size() == 2);

        auto param1 = params[0].getParameterType();
        auto param2 = params[1].getParameterType()->castTo<AnyFunctionType>();

        assert(param1->getOptionalObjectType());
        assert(params[1].isAutoClosure());
        assert(param2->getResult()->getOptionalObjectType());

        (void) param1;
        (void) param2;

        return true;
      };

      isStdlibOptionalMPlusOperator1 = check(decl1);
      isStdlibOptionalMPlusOperator2 = check(decl2);
    }
  }

  // Compare the type variable bindings.
  llvm::DenseMap<TypeVariableType *, TypeBindingsToCompare> typeDiff;

  const auto &bindings1 = solutions[idx1].typeBindings;
  const auto &bindings2 = solutions[idx2].typeBindings;

  for (const auto &binding1 : bindings1) {
    auto *typeVar = binding1.first;
    auto *loc = typeVar->getImpl().getLocator();

    // Check whether this is the overload type for a short-form init call
    // 'X(...)' or 'self.init(...)' call.
    auto isShortFormOrSelfDelegatingConstructorBinding = false;
    if (auto initMemberTypeElt =
            loc->getLastElementAs<LocatorPathElt::ConstructorMemberType>()) {
      isShortFormOrSelfDelegatingConstructorBinding =
          initMemberTypeElt->isShortFormOrSelfDelegatingConstructor();
    }

    // If the type variable isn't one for which we should be looking at the
    // bindings, don't.
    if (!typeVar->getImpl().prefersSubtypeBinding() &&
        !isShortFormOrSelfDelegatingConstructorBinding) {
      continue;
    }

    // If both solutions have a binding for this type variable
    // let's consider it.
    auto binding2 = bindings2.find(typeVar);
    if (binding2 == bindings2.end())
      continue;

    TypeBindingsToCompare typesToCompare(binding1.second, binding2->second);

    // For short-form and self-delegating init calls, we want to prefer
    // parameter lists with subtypes over supertypes. To do this, compose tuples
    // for the bound parameter lists, and compare them in the type diff. This
    // logic preserves the behavior of when we used to bind the parameter list
    // as a tuple to a TVO_PrefersSubtypeBinding type variable for such calls.
    // FIXME: We should come up with a better way of doing this, though note we
    // have some ranking and subtyping rules specific to tuples that we may need
    // to preserve to avoid breaking source.
    if (isShortFormOrSelfDelegatingConstructorBinding) {
      auto diffs = getConstructorParamsAsTuples(
          cs.getASTContext(), typesToCompare.Type1, typesToCompare.Type2);
      if (!diffs)
        continue;
      typesToCompare = *diffs;
    }

    if (!typesToCompare.areSameTypes())
      typeDiff.insert({typeVar, typesToCompare});
  }

  for (auto &binding : typeDiff) {
    auto types = binding.second;
    auto type1 = types.Type1;
    auto type2 = types.Type2;

    // If either of the types still contains type variables, we can't
    // compare them.
    // FIXME: This is really unfortunate. More type variable sharing
    // (when it's sane) would help us do much better here.
    if (type1->hasTypeVariable() || type2->hasTypeVariable()) {
      identical = false;
      continue;
    }

    // With introduction of holes it's currently possible to form solutions
    // with UnresolvedType bindings, we need to account for that in
    // ranking. If one solution has a hole for a given type variable
    // it's always worse than any non-hole type other solution might have.
    if (type1->is<UnresolvedType>() || type2->is<UnresolvedType>()) {
      if (type1->is<UnresolvedType>()) {
        ++score2;
      } else {
        ++score1;
      }

      identical = false;
      continue;
    }

    // If one type is a subtype of the other, but not vice-versa,
    // we prefer the system with the more-constrained type.
    // FIXME: Collapse this check into the second check.
    auto type1Better = TypeChecker::isSubtypeOf(type1, type2, cs.DC);
    auto type2Better = TypeChecker::isSubtypeOf(type2, type1, cs.DC);
    if (type1Better || type2Better) {
      if (type1Better)
        ++score1;
      if (type2Better)
        ++score2;

      // Prefer the unlabeled form of a type.
      auto unlabeled1 = getUnlabeledType(type1, cs.getASTContext());
      auto unlabeled2 = getUnlabeledType(type2, cs.getASTContext());
      if (unlabeled1->isEqual(unlabeled2)) {
        if (type1->isEqual(unlabeled1) && !types.Type1WasLabeled) {
          ++score1;
          continue;
        }
        if (type2->isEqual(unlabeled2) && !types.Type2WasLabeled) {
          ++score2;
          continue;
        }
      }

      identical = false;
      continue;
    }

    // The systems are not considered equivalent.
    identical = false;

    // Archetypes are worse than concrete types (i.e. non-placeholder and
    // non-archetype)
    // FIXME: Total hack.
    if (type1->is<ArchetypeType>() && !type2->is<ArchetypeType>() &&
        !type2->is<PlaceholderType>()) {
      ++score2;
      continue;
    } else if (type2->is<ArchetypeType>() && !type1->is<ArchetypeType>() &&
               !type1->is<PlaceholderType>()) {
      ++score1;
      continue;
    }

    // FIXME:
    // This terrible hack is in place to support equality comparisons of non-
    // equatable option types to 'nil'. Until we have a way to constrain a type
    // variable on "!Equatable", if all other aspects of the overload choices
    // are equal, favor the overload that does not require an implicit literal
    // argument conversion to 'nil'.
    // Post-1.0, we'll need to remove this hack in favor of richer constraint
    // declarations.
    if (!(score1 || score2)) {
      if (auto nominalType2 = type2->getNominalOrBoundGenericNominal()) {
        if ((nominalType2->getName() ==
             cs.getASTContext().Id_OptionalNilComparisonType)) {
          ++score2;
        }
      }

      if (auto nominalType1 = type1->getNominalOrBoundGenericNominal()) {
        if ((nominalType1->getName() ==
             cs.getASTContext().Id_OptionalNilComparisonType)) {
          ++score1;
        }
      }
    }
  }

  // All other things considered equal, if any overload choice is more
  // more constrained than the other, increment the score.
  if (score1 == score2) {
    if (foundRefinement1) {
      ++score1;
    }
    if (foundRefinement2) {
      ++score2;
    }
  }

  // FIXME: All other things being equal, prefer the catamorphism (flattening)
  // overload of ?? over the mplus (non-flattening) overload.
  if (score1 == score2) {
    // This is correct: we want to /disprefer/ the mplus.
    score2 += isStdlibOptionalMPlusOperator1;
    score1 += isStdlibOptionalMPlusOperator2;
  }

  // All other things being equal, apply the Swift 4.1 compatibility hack for
  // preferring var members in concrete types over a protocol requirement
  // (see the comment above for the rationale of this hack).
  if (!cs.getASTContext().isSwiftVersionAtLeast(5) && score1 == score2) {
    score1 += isVarAndNotProtocol1;
    score2 += isVarAndNotProtocol2;
  }

  // FIXME: There are type variables and overloads not common to both solutions
  // that haven't been considered. They make the systems different, but don't
  // affect ranking. We need to handle this.

  // If the scores are different, we have a winner.
  if (score1 != score2) {
    return score1 > score2? SolutionCompareResult::Better
                          : SolutionCompareResult::Worse;
  }

  // Neither system wins; report whether they were identical or not.
  return identical? SolutionCompareResult::Identical
                  : SolutionCompareResult::Incomparable;
}

llvm::Optional<unsigned>
ConstraintSystem::findBestSolution(SmallVectorImpl<Solution> &viable,
                                   bool minimize) {
  // Don't spend time filtering solutions if we already hit a threshold.
  if (isTooComplex(viable))
    return llvm::None;

  if (viable.empty())
    return llvm::None;
  if (viable.size() == 1)
    return 0;

  if (isDebugMode()) {
    auto indent = solverState->getCurrentIndent();
    auto &log = llvm::errs();

    log.indent(indent) << "Comparing " << viable.size()
                       << " viable solutions\n";
    for (unsigned i = 0, n = viable.size(); i != n; ++i) {
      log << "\n";
      log.indent(indent) << "--- Solution #" << i << " ---\n";
      viable[i].dump(llvm::errs(), indent);
    }
  }

  SolutionDiff diff(viable);

  // Find a potential best.
  SmallVector<bool, 16> losers(viable.size(), false);
  Score bestScore = viable.front().getFixedScore();
  unsigned bestIdx = 0;
  for (unsigned i = 1, n = viable.size(); i != n; ++i) {
    auto currScore = viable[i].getFixedScore();

    if (currScore < bestScore)
      bestScore = currScore;

    switch (compareSolutions(*this, viable, diff, i, bestIdx)) {
    case SolutionCompareResult::Identical:
      // FIXME: Might want to warn about this in debug builds, so we can
      // find a way to eliminate the redundancy in the search space.
    case SolutionCompareResult::Incomparable:
      break;

    case SolutionCompareResult::Worse:
      losers[i] = true;
      break;

    case SolutionCompareResult::Better:
      losers[bestIdx] = true;
      bestIdx = i;
      break;
    }

    // Give up if we're out of time.
    if (isTooComplex(/*solutions=*/{}))
      return llvm::None;
  }

  // Make sure that our current best is better than all of the solved systems.
  bool ambiguous = false;
  for (unsigned i = 0, n = viable.size(); i != n && !ambiguous; ++i) {
    if (i == bestIdx)
      continue;

    switch (compareSolutions(*this, viable, diff, bestIdx, i)) {
    case SolutionCompareResult::Identical:
      // FIXME: Might want to warn about this in debug builds, so we can
      // find a way to eliminate the redundancy in the search space.
      break;

    case SolutionCompareResult::Better:
      losers[i] = true;
      break;

    case SolutionCompareResult::Worse:
      losers[bestIdx] = true;
      LLVM_FALLTHROUGH;

    case SolutionCompareResult::Incomparable:
      // If we're not supposed to minimize the result set, just return eagerly.
      if (!minimize)
        return llvm::None;

      ambiguous = true;
      break;
    }

    // Give up if we're out of time.
    if (isTooComplex(/*solutions=*/{}))
      return llvm::None;
  }

  // If the result was not ambiguous, we're done.
  if (!ambiguous) {
    NumDiscardedSolutions += viable.size() - 1;
    return bestIdx;
  }

  if (!minimize)
    return llvm::None;

  // Remove any solution that is worse than some other solution.
  unsigned outIndex = 0;
  for (unsigned i = 0, n = viable.size(); i != n; ++i) {
    // Skip over the losing solutions.
    if (viable[i].getFixedScore() > bestScore)
      continue;

    // If we have skipped any solutions, move this solution into the next
    // open position.
    if (outIndex < i)
      viable[outIndex] = std::move(viable[i]);

    ++outIndex;
  }

  viable.erase(viable.begin() + outIndex, viable.end());
  NumDiscardedSolutions += viable.size() - outIndex;

  return llvm::None;
}

SolutionDiff::SolutionDiff(ArrayRef<Solution> solutions) {
  if (solutions.size() <= 1)
    return;

  // Populate the overload choices with the first solution.
  llvm::DenseMap<ConstraintLocator *, SmallVector<OverloadChoice, 2>>
    overloadChoices;
  for (auto choice : solutions[0].overloadChoices) {
    overloadChoices[choice.first].push_back(choice.second.choice);
  }

  // Find the type variables and overload locators common to all of the
  // solutions.
  for (auto &solution : solutions.slice(1)) {
    // For each overload locator for which we have an overload choice in
    // all of the previous solutions. Check whether we have an overload choice
    // in this solution.
    SmallVector<ConstraintLocator *, 4> removeOverloadChoices;
    for (auto &overloadChoice : overloadChoices) {
      auto known = solution.overloadChoices.find(overloadChoice.first);
      if (known == solution.overloadChoices.end()) {
        removeOverloadChoices.push_back(overloadChoice.first);
        continue;
      }

      // Add this solution's overload choice to the results.
      overloadChoice.second.push_back(known->second.choice);
    }

    // Remove those overload locators for which this solution did not have
    // an overload choice.
    for (auto overloadChoice : removeOverloadChoices) {
      overloadChoices.erase(overloadChoice);
    }
  }

  for (auto &overloadChoice : overloadChoices) {
    OverloadChoice singleChoice = overloadChoice.second[0];
    for (auto choice : overloadChoice.second) {
      if (sameOverloadChoice(singleChoice, choice))
        continue;

      // We have a difference. Add this set of overload choices to the diff.
      this->overloads.push_back(SolutionDiff::OverloadDiff{
          overloadChoice.first, std::move(overloadChoice.second)});
      break;
    }
  }
}

InputMatcher::InputMatcher(const ArrayRef<AnyFunctionType::Param> params,
                           const ParameterListInfo &paramInfo)
    : NumSkippedParameters(0), ParamInfo(paramInfo),
      Params(params) {}

InputMatcher::Result
InputMatcher::match(int numInputs,
                    std::function<bool(unsigned, unsigned)> pairMatcher) {

  int inputIdx = 0;
  int numParams = Params.size();
  for (int i = 0; i < numParams; ++i) {
    // If we've claimed all of the inputs, the rest of the parameters should
    // be either default or variadic.
    if (inputIdx == numInputs) {
      if (!ParamInfo.hasDefaultArgument(i) && !Params[i].isVariadic())
        return IM_HasUnmatchedParam;
      ++NumSkippedParameters;
      continue;
    }

    // If there is a default for parameter, while there are still some
    // input left unclaimed, it could only mean that default parameters
    // are intermixed e.g.
    //
    // inputs: (a: Int)
    // params: (q: String = "", a: Int)
    //
    // or
    // inputs: (a: Int, c: Int)
    // params: (a: Int, b: Int = 0, c: Int)
    //
    // and we shouldn't claim any input and just skip such parameter.
    if ((numInputs - inputIdx) < (numParams - i) &&
        ParamInfo.hasDefaultArgument(i)) {
      ++NumSkippedParameters;
      continue;
    }

    // Call custom function to match the input-parameter pair.
    if (!pairMatcher(inputIdx, i))
      return IM_CustomPairMatcherFailed;

    // claim the input as used.
    ++inputIdx;
  }

  if (inputIdx < numInputs)
    return IM_HasUnclaimedInput;

  return IM_Succeeded;
}
