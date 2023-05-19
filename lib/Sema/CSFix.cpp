//===--- CSFix.cpp - Constraint Fixes -------------------------------------===//
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
// This file implements the \c ConstraintFix class and its related types,
// which is used by constraint solver to attempt to fix constraints to be
// able to produce a solution which is easily diagnosable.
//
//===----------------------------------------------------------------------===//

#include "CSDiagnostics.h"
#include "TypeCheckConcurrency.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/RequirementSignature.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Sema/ConstraintLocator.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/CSFix.h"
#include "swift/Sema/OverloadChoice.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include <string>

using namespace swift;
using namespace constraints;

ConstraintFix::~ConstraintFix() {}

Optional<ScoreKind> ConstraintFix::impact() const {
  switch (fixBehavior) {
  case FixBehavior::AlwaysWarning:
    return None;

  case FixBehavior::Error:
    return SK_Fix;

  case FixBehavior::DowngradeToWarning:
    return SK_DisfavoredOverload;

  case FixBehavior::Suppress:
    return None;
  }
}

ASTNode ConstraintFix::getAnchor() const { return getLocator()->getAnchor(); }

void ConstraintFix::print(llvm::raw_ostream &Out) const {
  Out << "[fix: ";
  Out << getName();
  Out << ']';
  Out << " @ ";
  getLocator()->dump(&CS.getASTContext().SourceMgr, Out);
}

void ConstraintFix::dump() const {print(llvm::errs()); }

std::string ForceDowncast::getName() const {
  llvm::SmallString<16> name;
  name += "force downcast (";
  name += getFromType()->getString();
  name += " as! ";
  name += getToType()->getString();
  name += ")";
  return name.c_str();
}

bool ForceDowncast::diagnose(const Solution &solution, bool asNote) const {
  MissingExplicitConversionFailure failure(solution, getFromType(), getToType(),
                                           getLocator());
  return failure.diagnose(asNote);
}

ForceDowncast *ForceDowncast::create(ConstraintSystem &cs, Type fromType,
                                     Type toType, ConstraintLocator *locator) {
  return new (cs.getAllocator()) ForceDowncast(cs, fromType, toType, locator);
}

bool ForceOptional::diagnose(const Solution &solution, bool asNote) const {
  MissingOptionalUnwrapFailure failure(solution, getFromType(), getToType(),
                                       getLocator());
  return failure.diagnose(asNote);
}

ForceOptional *ForceOptional::create(ConstraintSystem &cs, Type fromType,
                                     Type toType, ConstraintLocator *locator) {
  return new (cs.getAllocator()) ForceOptional(cs, fromType, toType, locator);
}

bool UnwrapOptionalBase::diagnose(const Solution &solution, bool asNote) const {
  bool resultIsOptional =
      getKind() == FixKind::UnwrapOptionalBaseWithOptionalResult;
  MemberAccessOnOptionalBaseFailure failure(solution, getLocator(), MemberName,
                                            MemberBaseType, resultIsOptional);
  return failure.diagnose(asNote);
}

UnwrapOptionalBase *UnwrapOptionalBase::create(ConstraintSystem &cs,
                                               DeclNameRef member,
                                               Type memberBaseType,
                                               ConstraintLocator *locator) {
  return new (cs.getAllocator()) UnwrapOptionalBase(
      cs, FixKind::UnwrapOptionalBase, member, memberBaseType, locator);
}

UnwrapOptionalBase *UnwrapOptionalBase::createWithOptionalResult(
    ConstraintSystem &cs, DeclNameRef member, Type memberBaseType,
    ConstraintLocator *locator) {
  return new (cs.getAllocator())
      UnwrapOptionalBase(cs, FixKind::UnwrapOptionalBaseWithOptionalResult,
                         member, memberBaseType, locator);
}

bool AddAddressOf::diagnose(const Solution &solution, bool asNote) const {
  MissingAddressOfFailure failure(solution, getFromType(), getToType(),
                                  getLocator());
  return failure.diagnose(asNote);
}

AddAddressOf *AddAddressOf::create(ConstraintSystem &cs, Type argTy,
                                   Type paramTy, ConstraintLocator *locator) {
  return new (cs.getAllocator()) AddAddressOf(cs, argTy, paramTy, locator);
}

bool TreatRValueAsLValue::diagnose(const Solution &solution,
                                   bool asNote) const {
  RValueTreatedAsLValueFailure failure(solution, getLocator());
  return failure.diagnose(asNote);
}

TreatRValueAsLValue *TreatRValueAsLValue::create(ConstraintSystem &cs,
                                   ConstraintLocator *locator) {
  if (locator->isLastElement<LocatorPathElt::ApplyArgToParam>())
    locator = cs.getConstraintLocator(
        locator, LocatorPathElt::ArgumentAttribute::forInOut());

  return new (cs.getAllocator()) TreatRValueAsLValue(cs, locator);
}

bool CoerceToCheckedCast::diagnose(const Solution &solution,
                                   bool asNote) const {
  InvalidCoercionFailure failure(solution, getFromType(), getToType(),
                                 UseConditionalCast, getLocator());
  return failure.diagnose(asNote);
}

CoerceToCheckedCast *CoerceToCheckedCast::attempt(ConstraintSystem &cs,
                                                  Type fromType, Type toType,
                                                  bool useConditionalCast,
                                                  ConstraintLocator *locator) {
  // If any of the types has a type variable, don't add the fix.
  if (fromType->hasTypeVariable() || toType->hasTypeVariable())
    return nullptr;

  auto anchor = locator->getAnchor();
  if (auto *assignExpr = getAsExpr<AssignExpr>(anchor))
    anchor = assignExpr->getSrc();
  auto *coerceExpr = getAsExpr<CoerceExpr>(anchor);
  if (!coerceExpr)
    return nullptr;

  const auto castKind = TypeChecker::typeCheckCheckedCast(
      fromType, toType, CheckedCastContextKind::Coercion, cs.DC);

  // Invalid cast.
  if (castKind == CheckedCastKind::Unresolved)
    return nullptr;

  return new (cs.getAllocator())
      CoerceToCheckedCast(cs, fromType, toType, useConditionalCast, locator);
}

bool TreatArrayLiteralAsDictionary::diagnose(const Solution &solution,
                                             bool asNote) const {
  ArrayLiteralToDictionaryConversionFailure failure(solution,
                                                    getToType(), getFromType(),
                                                    getLocator());
  return failure.diagnose(asNote);
}

TreatArrayLiteralAsDictionary *
TreatArrayLiteralAsDictionary::attempt(ConstraintSystem &cs, Type dictionaryTy,
                                       Type arrayTy,
                                       ConstraintLocator *locator) {
  if (!arrayTy->isArrayType())
    return nullptr;

  // Determine the ArrayExpr from the locator.
  auto *expr = getAsExpr(simplifyLocatorToAnchor(locator));
  if (!expr)
    return nullptr;

  if (auto *AE = dyn_cast<AssignExpr>(expr))
    expr = AE->getSrc();

  auto *arrayExpr = dyn_cast<ArrayExpr>(expr);
  if (!arrayExpr)
    return nullptr;

  // This fix only applies if the array is used as a dictionary.
  auto unwrappedDict = dictionaryTy->lookThroughAllOptionalTypes();
  if (unwrappedDict->isTypeVariableOrMember())
    return nullptr;

  if (!TypeChecker::conformsToKnownProtocol(
          unwrappedDict, KnownProtocolKind::ExpressibleByDictionaryLiteral,
          cs.DC->getParentModule()))
    return nullptr;

  auto arrayLoc = cs.getConstraintLocator(arrayExpr);
  return new (cs.getAllocator())
      TreatArrayLiteralAsDictionary(cs, dictionaryTy, arrayTy, arrayLoc);
}

bool MarkExplicitlyEscaping::diagnose(const Solution &solution,
                                      bool asNote) const {
  AttributedFuncToTypeConversionFailure failure(
      solution, getFromType(), getToType(), getLocator(),
      AttributedFuncToTypeConversionFailure::Escaping);
  return failure.diagnose(asNote);
}

MarkExplicitlyEscaping *
MarkExplicitlyEscaping::create(ConstraintSystem &cs, Type lhs, Type rhs,
                               ConstraintLocator *locator) {
  if (locator->isLastElement<LocatorPathElt::ApplyArgToParam>())
    locator = cs.getConstraintLocator(
        locator, LocatorPathElt::ArgumentAttribute::forEscaping());

  return new (cs.getAllocator()) MarkExplicitlyEscaping(cs, lhs, rhs, locator);
}

bool MarkGlobalActorFunction::diagnose(const Solution &solution,
                                       bool asNote) const {
  DroppedGlobalActorFunctionAttr failure(
      solution, getFromType(), getToType(), getLocator(), fixBehavior);
  return failure.diagnose(asNote);
}

/// The fix behavior to apply to a concurrency-related diagnostic.
static Optional<FixBehavior>
getConcurrencyFixBehavior(
    ConstraintSystem &cs, ConstraintKind constraintKind,
    ConstraintLocatorBuilder locator, bool forSendable) {
  // We can only handle the downgrade for conversions.
  switch (constraintKind) {
  case ConstraintKind::Conversion:
  case ConstraintKind::ArgumentConversion:
  case ConstraintKind::Subtype:
    break;

  default:
    if (!cs.shouldAttemptFixes())
      return None;

    return FixBehavior::Error;
  }

  // For a @preconcurrency callee outside of a strict concurrency
  // context, ignore.
  if (cs.hasPreconcurrencyCallee(locator) &&
      !contextRequiresStrictConcurrencyChecking(
          cs.DC, GetClosureType{cs}, ClosureIsolatedByPreconcurrency{cs}))
    return FixBehavior::Suppress;

  // Otherwise, warn until Swift 6.
  if (!cs.getASTContext().LangOpts.isSwiftVersionAtLeast(6))
    return FixBehavior::DowngradeToWarning;

  return FixBehavior::Error;
}

MarkGlobalActorFunction *
MarkGlobalActorFunction::create(ConstraintSystem &cs, Type lhs, Type rhs,
                                ConstraintLocator *locator,
                                FixBehavior fixBehavior) {
  if (locator->isLastElement<LocatorPathElt::ApplyArgToParam>())
    locator = cs.getConstraintLocator(
        locator, LocatorPathElt::ArgumentAttribute::forGlobalActor());

  return new (cs.getAllocator()) MarkGlobalActorFunction(
      cs, lhs, rhs, locator, fixBehavior);
}

bool MarkGlobalActorFunction::attempt(ConstraintSystem &cs,
                                      ConstraintKind constraintKind,
                                      FunctionType *fromType,
                                      FunctionType *toType,
                                      ConstraintLocatorBuilder locator) {
  auto fixBehavior = getConcurrencyFixBehavior(
      cs, constraintKind, locator, /*forSendable=*/false);
  if (!fixBehavior)
    return true;

  auto *fix = MarkGlobalActorFunction::create(
      cs, fromType, toType, cs.getConstraintLocator(locator),
      *fixBehavior);

  return cs.recordFix(fix);
}

bool AddSendableAttribute::diagnose(const Solution &solution,
                                      bool asNote) const {
  AttributedFuncToTypeConversionFailure failure(
      solution, getFromType(), getToType(), getLocator(),
      AttributedFuncToTypeConversionFailure::Concurrent, fixBehavior);
  return failure.diagnose(asNote);
}

AddSendableAttribute *
AddSendableAttribute::create(ConstraintSystem &cs,
                             FunctionType *fromType,
                             FunctionType *toType,
                             ConstraintLocator *locator,
                             FixBehavior fixBehavior) {
  if (locator->isLastElement<LocatorPathElt::ApplyArgToParam>())
    locator = cs.getConstraintLocator(
        locator, LocatorPathElt::ArgumentAttribute::forConcurrent());

  return new (cs.getAllocator()) AddSendableAttribute(
      cs, fromType, toType, locator, fixBehavior);
}

bool AddSendableAttribute::attempt(ConstraintSystem &cs,
                                   ConstraintKind constraintKind,
                                   FunctionType *fromType,
                                   FunctionType *toType,
                                   ConstraintLocatorBuilder locator) {
  auto fixBehavior = getConcurrencyFixBehavior(
      cs, constraintKind, locator, /*forSendable=*/true);
  if (!fixBehavior)
    return true;

  auto *fix = AddSendableAttribute::create(
      cs, fromType, toType, cs.getConstraintLocator(locator), *fixBehavior);
  return cs.recordFix(fix);
}

bool RelabelArguments::diagnose(const Solution &solution, bool asNote) const {
  LabelingFailure failure(solution, getLocator(), getLabels());
  return failure.diagnose(asNote);
}

bool RelabelArguments::diagnoseForAmbiguity(
    CommonFixesArray commonFixes) const {
  SmallPtrSet<ValueDecl *, 4> overloadChoices;

  // First, let's find overload choice associated with each
  // re-labeling fix.
  for (const auto &fix : commonFixes) {
    auto &solution = *fix.first;

    auto calleeLocator = solution.getCalleeLocator(getLocator());
    if (!calleeLocator)
      return false;

    auto overloadChoice = solution.getOverloadChoiceIfAvailable(calleeLocator);
    if (!overloadChoice)
      return false;

    auto *decl = overloadChoice->choice.getDeclOrNull();
    if (!decl)
      return false;

    (void)overloadChoices.insert(decl);
  }

  // If all of the fixes point to the same overload choice then it's
  // exactly the same issue since the call site is static.
  if (overloadChoices.size() == 1)
    return diagnose(*commonFixes.front().first);

  return false;
}

RelabelArguments *
RelabelArguments::create(ConstraintSystem &cs,
                         llvm::ArrayRef<Identifier> correctLabels,
                         ConstraintLocator *locator) {
  unsigned size = totalSizeToAlloc<Identifier>(correctLabels.size());
  void *mem = cs.getAllocator().Allocate(size, alignof(RelabelArguments));
  return new (mem) RelabelArguments(cs, correctLabels, locator);
}

bool MissingConformance::diagnose(const Solution &solution, bool asNote) const {
  auto *locator = getLocator();

  if (IsContextual) {
    auto &cs = solution.getConstraintSystem();
    auto context = cs.getContextualTypePurpose(locator->getAnchor());
    MissingContextualConformanceFailure failure(
        solution, context, getNonConformingType(), getProtocolType(), locator);
    return failure.diagnose(asNote);
  }

  MissingConformanceFailure failure(
      solution, locator,
      std::make_pair(getNonConformingType(), getProtocolType()));
  return failure.diagnose(asNote);
}

bool RequirementFix::diagnoseForAmbiguity(
    CommonFixesArray commonFixes) const {
  auto *primaryFix = commonFixes.front().second;
  assert(primaryFix);

  if (llvm::all_of(
          commonFixes,
          [&primaryFix](
              const std::pair<const Solution *, const ConstraintFix *> &entry) {
            return primaryFix->getLocator() == entry.second->getLocator();
          }))
    return diagnose(*commonFixes.front().first);

  // If the location is the same but there are different requirements
  // involved let's not attempt to diagnose that as an ambiguity.
  return false;
}

bool MissingConformance::isEqual(const ConstraintFix *other) const {
  auto *conformanceFix = other->getAs<MissingConformance>();
  if (!conformanceFix)
    return false;

  return IsContextual == conformanceFix->IsContextual &&
         getNonConformingType()->isEqual(
             conformanceFix->getNonConformingType()) &&
         getProtocolType()->isEqual(conformanceFix->getProtocolType());
}

MissingConformance *
MissingConformance::forContextual(ConstraintSystem &cs, Type type,
                                  Type protocolType,
                                  ConstraintLocator *locator) {
  return new (cs.getAllocator()) MissingConformance(
      cs, /*isContextual=*/true, type, protocolType, locator);
}

MissingConformance *
MissingConformance::forRequirement(ConstraintSystem &cs, Type type,
                                   Type protocolType,
                                   ConstraintLocator *locator) {
  return new (cs.getAllocator()) MissingConformance(
      cs, /*isContextual=*/false, type, protocolType, locator);
}

bool SkipSameTypeRequirement::diagnose(const Solution &solution,
                                       bool asNote) const {
  SameTypeRequirementFailure failure(solution, LHS, RHS, getLocator());
  return failure.diagnose(asNote);
}

SkipSameTypeRequirement *
SkipSameTypeRequirement::create(ConstraintSystem &cs, Type lhs, Type rhs,
                                ConstraintLocator *locator) {
  return new (cs.getAllocator()) SkipSameTypeRequirement(cs, lhs, rhs, locator);
}

bool SkipSameShapeRequirement::diagnose(const Solution &solution,
                                       bool asNote) const {
  if (getLocator()->isLastElement<LocatorPathElt::PackShape>()) {
    SameShapeExpansionFailure failure(solution, LHS, RHS, getLocator());
    return failure.diagnose(asNote);
  }

  SameShapeRequirementFailure failure(solution, LHS, RHS, getLocator());
  return failure.diagnose(asNote);
}

SkipSameShapeRequirement *
SkipSameShapeRequirement::create(ConstraintSystem &cs, Type lhs, Type rhs,
                                 ConstraintLocator *locator) {
  return new (cs.getAllocator()) SkipSameShapeRequirement(cs, lhs, rhs, locator);
}

bool SkipSuperclassRequirement::diagnose(const Solution &solution,
                                         bool asNote) const {
  SuperclassRequirementFailure failure(solution, LHS, RHS, getLocator());
  return failure.diagnose(asNote);
}

SkipSuperclassRequirement *
SkipSuperclassRequirement::create(ConstraintSystem &cs, Type lhs, Type rhs,
                                  ConstraintLocator *locator) {
  return new (cs.getAllocator())
      SkipSuperclassRequirement(cs, lhs, rhs, locator);
}

bool ContextualMismatch::diagnose(const Solution &solution, bool asNote) const {
  ContextualFailure failure(solution, getFromType(), getToType(), getLocator());
  return failure.diagnose(asNote);
}

bool ContextualMismatch::diagnoseForAmbiguity(
    CommonFixesArray commonFixes) const {
  auto getTypes =
      [&](const std::pair<const Solution *, const ConstraintFix *> &entry)
      -> std::pair<Type, Type> {
    auto &solution = *entry.first;
    auto *fix = static_cast<const ContextualMismatch *>(entry.second);

    return {solution.simplifyType(fix->getFromType()),
            solution.simplifyType(fix->getToType())};
  };

  auto etalonTypes = getTypes(commonFixes.front());
  if (llvm::all_of(
          commonFixes,
          [&](const std::pair<const Solution *, const ConstraintFix *> &entry) {
            auto types = getTypes(entry);
            return etalonTypes.first->isEqual(types.first) &&
                   etalonTypes.second->isEqual(types.second);
          })) {
    const auto &primary = commonFixes.front();
    return primary.second->diagnose(*primary.first, /*asNote=*/false);
  }

  return false;
}

ContextualMismatch *ContextualMismatch::create(ConstraintSystem &cs, Type lhs,
                                               Type rhs,
                                               ConstraintLocator *locator) {
  return new (cs.getAllocator()) ContextualMismatch(
      cs, lhs, rhs, locator, FixBehavior::Error);
}

bool AllowWrappedValueMismatch::diagnose(const Solution &solution, bool asError) const {
  WrappedValueMismatch failure(solution, getFromType(), getToType(), getLocator());
  return failure.diagnoseAsError();
}

AllowWrappedValueMismatch *AllowWrappedValueMismatch::create(ConstraintSystem &cs,
                                                             Type lhs,
                                                             Type rhs,
                                                             ConstraintLocator *locator) {
  return new (cs.getAllocator()) AllowWrappedValueMismatch(cs, lhs, rhs, locator);
}

/// Computes the contextual type information for a type mismatch of a
/// component in a structural type (tuple or function type).
///
/// \returns A tuple containing the contextual type purpose, the source type,
/// and the contextual type.
static Optional<std::tuple<ContextualTypePurpose, Type, Type>>
getStructuralTypeContext(const Solution &solution, ConstraintLocator *locator) {
  if (auto contextualTypeElt =
          locator->findLast<LocatorPathElt::ContextualType>()) {
    assert(locator->isLastElement<LocatorPathElt::ContextualType>() ||
           locator->isLastElement<LocatorPathElt::FunctionArgument>());

    auto &cs = solution.getConstraintSystem();
    auto anchor = locator->getAnchor();
    auto contextualType = cs.getContextualType(anchor, /*forConstraint=*/false);
    auto exprType = cs.getType(anchor);
    return std::make_tuple(contextualTypeElt->getPurpose(), exprType,
                           contextualType);
  } else if (auto argApplyInfo = solution.getFunctionArgApplyInfo(locator)) {
    Type fromType = argApplyInfo->getArgType();
    Type toType = argApplyInfo->getParamType();
    // In case locator points to the function result we want the
    // argument and param function types result.
    if (locator->isLastElement<LocatorPathElt::FunctionResult>()) {
      auto fromFnType = fromType->getAs<FunctionType>();
      auto toFnType = toType->getAs<FunctionType>();
      if (fromFnType && toFnType) {
        auto &cs = solution.getConstraintSystem();
        return std::make_tuple(
            cs.getContextualTypePurpose(locator->getAnchor()),
            fromFnType->getResult(), toFnType->getResult());
      }
    }
    return std::make_tuple(CTP_CallArgument, fromType, toType);
  } else if (auto *coerceExpr = getAsExpr<CoerceExpr>(locator->getAnchor())) {
    return std::make_tuple(CTP_CoerceOperand,
                           solution.getType(coerceExpr->getSubExpr()),
                           solution.getType(coerceExpr));
  } else if (auto *assignExpr = getAsExpr<AssignExpr>(locator->getAnchor())) {
    auto CTP = isa<SubscriptExpr>(assignExpr->getDest()) ? CTP_SubscriptAssignSource
                                                         : CTP_AssignSource;
    return std::make_tuple(CTP,
                           solution.getType(assignExpr->getSrc()),
                           solution.getType(assignExpr->getDest())->getRValueType());
  }
  return None;
}

bool AllowTupleTypeMismatch::coalesceAndDiagnose(
    const Solution &solution, ArrayRef<ConstraintFix *> fixes,
    bool asNote) const {
  llvm::SmallVector<unsigned, 4> indices;
  if (isElementMismatch())
    indices.push_back(*Index);

  for (auto fix : fixes) {
    auto *tupleFix = fix->getAs<AllowTupleTypeMismatch>();
    if (!tupleFix || !tupleFix->isElementMismatch())
      continue;
    indices.push_back(*tupleFix->Index);
  }

  auto *locator = getLocator();
  ContextualTypePurpose purpose;
  if (isExpr<CoerceExpr>(locator->getAnchor())) {
    purpose = CTP_CoerceOperand;
  } else if (auto *assignExpr = getAsExpr<AssignExpr>(locator->getAnchor())) {
    purpose = isa<SubscriptExpr>(assignExpr->getDest()) ? CTP_SubscriptAssignSource
                                                        : CTP_AssignSource;
  } else {
    auto &cs = getConstraintSystem();
    purpose = cs.getContextualTypePurpose(locator->getAnchor());
  }

  if (!getFromType()->is<TupleType>() || !getToType()->is<TupleType>()) {
    return false;
  }

  TupleContextualFailure failure(solution, purpose, getFromType(), getToType(),
                                 indices, locator);
  return failure.diagnose(asNote);
}

bool AllowTupleTypeMismatch::diagnose(const Solution &solution,
                                      bool asNote) const {
  return coalesceAndDiagnose(solution, {}, asNote);
}

AllowTupleTypeMismatch *
AllowTupleTypeMismatch::create(ConstraintSystem &cs, Type lhs, Type rhs,
                               ConstraintLocator *locator,
                               Optional<unsigned> index) {
  return new (cs.getAllocator())
      AllowTupleTypeMismatch(cs, lhs, rhs, locator, index);
}

bool AllowFunctionTypeMismatch::coalesceAndDiagnose(
    const Solution &solution, ArrayRef<ConstraintFix *> fixes,
    bool asNote) const {
  llvm::SmallVector<unsigned, 4> indices{ParamIndex};

  for (auto fix : fixes) {
    if (auto *fnFix = fix->getAs<AllowFunctionTypeMismatch>())
      indices.push_back(fnFix->ParamIndex);
  }

  auto *locator = getLocator();
  ContextualTypePurpose purpose;
  Type fromType;
  Type toType;

  auto contextualTypeInfo = getStructuralTypeContext(solution, locator);
  if (!contextualTypeInfo)
    return false;

  std::tie(purpose, fromType, toType) = *contextualTypeInfo;
  FunctionTypeMismatch failure(solution, purpose, fromType, toType, indices,
                               locator);
  return failure.diagnose(asNote);
}

bool AllowFunctionTypeMismatch::diagnose(const Solution &solution,
                                         bool asNote) const {
  return coalesceAndDiagnose(solution, {}, asNote);
}

bool AllowFunctionTypeMismatch::diagnoseForAmbiguity(
    CommonFixesArray commonFixes) const {
  if (ContextualMismatch::diagnoseForAmbiguity(commonFixes))
    return true;

  auto *locator = getLocator();
  // If this is a mismatch between two function types at argument
  // position, there is a tailored diagnostic for that.
  if (auto argConv =
          locator->getLastElementAs<LocatorPathElt::ApplyArgToParam>()) {
    auto &cs = getConstraintSystem();
    auto &DE = cs.getASTContext().Diags;
    auto &solution = *commonFixes[0].first;

    auto info = getStructuralTypeContext(solution, locator);
    if (!info)
      return false;

    auto *argLoc = cs.getConstraintLocator(simplifyLocatorToAnchor(locator));

    auto overload = solution.getOverloadChoiceIfAvailable(
        solution.getCalleeLocator(argLoc));
    if (!overload)
      return false;

    auto name = overload->choice.getName().getBaseName();
    DE.diagnose(getLoc(getAnchor()), diag::no_candidates_match_argument_type,
                name.userFacingName(), std::get<2>(*info),
                argConv->getParamIdx());

    for (auto &entry : commonFixes) {
      auto &solution = *entry.first;
      auto overload = solution.getOverloadChoiceIfAvailable(
          solution.getCalleeLocator(argLoc));

      if (!(overload && overload->choice.isDecl()))
        continue;

      auto *decl = overload->choice.getDecl();
      if (decl->getLoc().isValid()) {
        DE.diagnose(decl, diag::found_candidate_type,
                    solution.simplifyType(overload->adjustedOpenedType));
      }
    }

    return true;
  }

  return false;
}

AllowFunctionTypeMismatch *
AllowFunctionTypeMismatch::create(ConstraintSystem &cs, Type lhs, Type rhs,
                                  ConstraintLocator *locator, unsigned index) {
  return new (cs.getAllocator())
      AllowFunctionTypeMismatch(cs, lhs, rhs, locator, index);
}

bool GenericArgumentsMismatch::coalesceAndDiagnose(
    const Solution &solution, ArrayRef<ConstraintFix *> secondaryFixes,
    bool asNote) const {
  std::set<unsigned> scratch(getMismatches().begin(), getMismatches().end());

  for (auto *fix : secondaryFixes) {
    auto *genericArgsFix = fix->castTo<GenericArgumentsMismatch>();
    for (auto mismatchIdx : genericArgsFix->getMismatches())
      scratch.insert(mismatchIdx);
  }

  SmallVector<unsigned> mismatches(scratch.begin(), scratch.end());
  return diagnose(solution, mismatches, asNote);
}

bool GenericArgumentsMismatch::diagnose(const Solution &solution,
                                        bool asNote) const {
  return diagnose(solution, getMismatches(), asNote);
}

bool GenericArgumentsMismatch::diagnose(const Solution &solution,
                                        ArrayRef<unsigned> mismatches,
                                        bool asNote) const {
  GenericArgumentsMismatchFailure failure(solution, getFromType(), getToType(),
                                          mismatches, getLocator());
  return failure.diagnose(asNote);
}

GenericArgumentsMismatch *GenericArgumentsMismatch::create(
    ConstraintSystem &cs, Type actual, Type required,
    llvm::ArrayRef<unsigned> mismatches, ConstraintLocator *locator) {
  unsigned size = totalSizeToAlloc<unsigned>(mismatches.size());
  void *mem =
      cs.getAllocator().Allocate(size, alignof(GenericArgumentsMismatch));
  return new (mem)
      GenericArgumentsMismatch(cs, actual, required, mismatches, locator);
}

bool AutoClosureForwarding::diagnose(const Solution &solution,
                                     bool asNote) const {
  AutoClosureForwardingFailure failure(solution, getLocator());
  return failure.diagnose(asNote);
}

AutoClosureForwarding *AutoClosureForwarding::create(ConstraintSystem &cs,
                                                     ConstraintLocator *locator) {
  return new (cs.getAllocator()) AutoClosureForwarding(cs, locator);
}

bool AllowAutoClosurePointerConversion::diagnose(const Solution &solution,
                                                 bool asNote) const {
  AutoClosurePointerConversionFailure failure(solution, getFromType(),
                                              getToType(), getLocator());
  return failure.diagnose(asNote);
}

AllowAutoClosurePointerConversion *
AllowAutoClosurePointerConversion::create(ConstraintSystem &cs, Type pointeeType,
                                          Type pointerType,
                                          ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowAutoClosurePointerConversion(cs, pointeeType, pointerType, locator);
}

bool RemoveUnwrap::diagnose(const Solution &solution, bool asNote) const {
  NonOptionalUnwrapFailure failure(solution, BaseType, getLocator());
  return failure.diagnose(asNote);
}

RemoveUnwrap *RemoveUnwrap::create(ConstraintSystem &cs, Type baseType,
                                   ConstraintLocator *locator) {
  return new (cs.getAllocator()) RemoveUnwrap(cs, baseType, locator);
}

bool InsertExplicitCall::diagnose(const Solution &solution, bool asNote) const {
  MissingCallFailure failure(solution, getLocator());
  return failure.diagnose(asNote);
}

InsertExplicitCall *InsertExplicitCall::create(ConstraintSystem &cs,
                                               ConstraintLocator *locator) {
  return new (cs.getAllocator()) InsertExplicitCall(cs, locator);
}

bool UsePropertyWrapper::diagnose(const Solution &solution, bool asNote) const {
  ExtraneousPropertyWrapperUnwrapFailure failure(
      solution, Wrapped, UsingProjection, Base, Wrapper, getLocator());
  return failure.diagnose(asNote);
}

UsePropertyWrapper *UsePropertyWrapper::create(ConstraintSystem &cs,
                                               VarDecl *wrapped,
                                               bool usingProjection,
                                               Type base, Type wrapper,
                                               ConstraintLocator *locator) {
  return new (cs.getAllocator()) UsePropertyWrapper(
      cs, wrapped, usingProjection, base, wrapper, locator);
}

bool UseWrappedValue::diagnose(const Solution &solution, bool asNote) const {
  MissingPropertyWrapperUnwrapFailure failure(solution, PropertyWrapper,
                                              usingProjection(), Base,
                                              Wrapper, getLocator());
  return failure.diagnose(asNote);
}

UseWrappedValue *UseWrappedValue::create(ConstraintSystem &cs,
                                         VarDecl *propertyWrapper, Type base,
                                         Type wrapper,
                                         ConstraintLocator *locator) {
  return new (cs.getAllocator())
      UseWrappedValue(cs, propertyWrapper, base, wrapper, locator);
}

bool AllowInvalidPropertyWrapperType::diagnose(const Solution &solution, bool asNote) const {
  InvalidPropertyWrapperType failure(solution, wrapperType, getLocator());
  return failure.diagnose(asNote);
}

AllowInvalidPropertyWrapperType *
AllowInvalidPropertyWrapperType::create(ConstraintSystem &cs, Type wrapperType,
                                        ConstraintLocator *locator) {
  return new (cs.getAllocator()) AllowInvalidPropertyWrapperType(cs, wrapperType, locator);
}

bool RemoveProjectedValueArgument::diagnose(const Solution &solution, bool asNote) const {
  InvalidProjectedValueArgument failure(solution, wrapperType, param, getLocator());
  return failure.diagnose(asNote);
}

RemoveProjectedValueArgument *
RemoveProjectedValueArgument::create(ConstraintSystem &cs, Type wrapperType,
                                      ParamDecl *param, ConstraintLocator *locator) {
  return new (cs.getAllocator()) RemoveProjectedValueArgument(cs, wrapperType, param, locator);
}

bool UseSubscriptOperator::diagnose(const Solution &solution,
                                    bool asNote) const {
  SubscriptMisuseFailure failure(solution, getLocator());
  return failure.diagnose(asNote);
}

UseSubscriptOperator *UseSubscriptOperator::create(ConstraintSystem &cs,
                                                   ConstraintLocator *locator) {
  return new (cs.getAllocator()) UseSubscriptOperator(cs, locator);
}

bool DefineMemberBasedOnUse::diagnose(const Solution &solution,
                                      bool asNote) const {
  MissingMemberFailure failure(solution, BaseType, Name, getLocator());
  return AlreadyDiagnosed || failure.diagnose(asNote);
}

bool
DefineMemberBasedOnUse::diagnoseForAmbiguity(CommonFixesArray commonFixes) const {
  Type concreteBaseType;
  for (const auto &solutionAndFix : commonFixes) {
    const auto *solution = solutionAndFix.first;
    const auto *fix = solutionAndFix.second->getAs<DefineMemberBasedOnUse>();

    auto baseType = solution->simplifyType(fix->BaseType);
    if (!concreteBaseType)
      concreteBaseType = baseType;

    if (concreteBaseType->getCanonicalType() != baseType->getCanonicalType()) {
      auto &DE = getConstraintSystem().getASTContext().Diags;
      DE.diagnose(getLoc(getAnchor()), diag::unresolved_member_no_inference,
                  Name);
      return true;
    }
  }

  return diagnose(*commonFixes.front().first);
}

DefineMemberBasedOnUse *
DefineMemberBasedOnUse::create(ConstraintSystem &cs, Type baseType,
                               DeclNameRef member, bool alreadyDiagnosed,
                               ConstraintLocator *locator) {
  return new (cs.getAllocator())
      DefineMemberBasedOnUse(cs, baseType, member, alreadyDiagnosed, locator);
}

bool DefineMemberBasedOnUnintendedGenericParam::diagnose(
    const Solution &solution, bool asNote) const {
  UnintendedExtraGenericParamMemberFailure failure(solution, BaseType, Name,
                                                   ParamName, getLocator());
  return failure.diagnose(asNote);
}

DefineMemberBasedOnUnintendedGenericParam *
DefineMemberBasedOnUnintendedGenericParam::create(ConstraintSystem &cs,
                                                  Type baseType,
                                                  DeclNameRef member,
                                                  Identifier paramName,
                                                  ConstraintLocator *locator) {
  return new (cs.getAllocator()) DefineMemberBasedOnUnintendedGenericParam(
      cs, baseType, member, paramName, locator);
}

AllowMemberRefOnExistential *
AllowMemberRefOnExistential::create(ConstraintSystem &cs, Type baseType,
                                    ValueDecl *member, DeclNameRef memberName,
                                    ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowMemberRefOnExistential(cs, baseType, memberName, member, locator);
}

bool AllowMemberRefOnExistential::diagnose(const Solution &solution,
                                           bool asNote) const {
  InvalidMemberRefOnExistential failure(solution, getBaseType(),
                                        getMemberName(), getLocator());
  return failure.diagnose(asNote);
}

bool AllowInvalidMemberRef::diagnoseForAmbiguity(
    CommonFixesArray commonFixes) const {
  auto *primaryFix =
      static_cast<const AllowInvalidMemberRef *>(commonFixes.front().second);

  Type baseTy = primaryFix->getBaseType();
  for (const auto &entry : commonFixes) {
    auto *memberFix = static_cast<const AllowInvalidMemberRef *>(entry.second);
    if (!baseTy->isEqual(memberFix->getBaseType()))
      return false;
  }

  return diagnose(*commonFixes.front().first);
}

bool AllowTypeOrInstanceMember::diagnose(const Solution &solution,
                                         bool asNote) const {
  AllowTypeOrInstanceMemberFailure failure(solution, getBaseType(), getMember(),
                                           getMemberName(), getLocator());
  return failure.diagnose(asNote);
}

AllowTypeOrInstanceMember *
AllowTypeOrInstanceMember::create(ConstraintSystem &cs, Type baseType,
                                  ValueDecl *member, DeclNameRef usedName,
                                  ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowTypeOrInstanceMember(cs, baseType, member, usedName, locator);
}

bool AllowInvalidPartialApplication::diagnose(const Solution &solution,
                                              bool asNote) const {
  PartialApplicationFailure failure(isWarning, solution, getLocator());
  return failure.diagnose(asNote);
}

AllowInvalidPartialApplication *
AllowInvalidPartialApplication::create(bool isWarning, ConstraintSystem &cs,
                                       ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowInvalidPartialApplication(isWarning, cs, locator);
}

bool AllowInvalidInitRef::diagnose(const Solution &solution,
                                   bool asNote) const {
  switch (Kind) {
  case RefKind::DynamicOnMetatype: {
    InvalidDynamicInitOnMetatypeFailure failure(solution, BaseType, Init,
                                                BaseRange, getLocator());
    return failure.diagnose(asNote);
  }

  case RefKind::ProtocolMetatype: {
    InitOnProtocolMetatypeFailure failure(
        solution, BaseType, Init, IsStaticallyDerived, BaseRange, getLocator());
    return failure.diagnose(asNote);
  }

  case RefKind::NonConstMetatype: {
    ImplicitInitOnNonConstMetatypeFailure failure(solution, BaseType, Init,
                                                  getLocator());
    return failure.diagnose(asNote);
  }
  }
  llvm_unreachable("covered switch");
}

AllowInvalidInitRef *AllowInvalidInitRef::dynamicOnMetatype(
    ConstraintSystem &cs, Type baseTy, ConstructorDecl *init,
    SourceRange baseRange, ConstraintLocator *locator) {
  return create(RefKind::DynamicOnMetatype, cs, baseTy, init,
                /*isStaticallyDerived=*/false, baseRange, locator);
}

AllowInvalidInitRef *AllowInvalidInitRef::onProtocolMetatype(
    ConstraintSystem &cs, Type baseTy, ConstructorDecl *init,
    bool isStaticallyDerived, SourceRange baseRange,
    ConstraintLocator *locator) {
  return create(RefKind::ProtocolMetatype, cs, baseTy, init,
                isStaticallyDerived, baseRange, locator);
}

AllowInvalidInitRef *
AllowInvalidInitRef::onNonConstMetatype(ConstraintSystem &cs, Type baseTy,
                                        ConstructorDecl *init,
                                        ConstraintLocator *locator) {
  return create(RefKind::NonConstMetatype, cs, baseTy, init,
                /*isStaticallyDerived=*/false, SourceRange(), locator);
}

AllowInvalidInitRef *
AllowInvalidInitRef::create(RefKind kind, ConstraintSystem &cs, Type baseTy,
                            ConstructorDecl *init, bool isStaticallyDerived,
                            SourceRange baseRange, ConstraintLocator *locator) {
  return new (cs.getAllocator()) AllowInvalidInitRef(
      cs, kind, baseTy, init, isStaticallyDerived, baseRange, locator);
}

bool AllowClosureParamDestructuring::diagnose(const Solution &solution,
                                              bool asNote) const {
  ClosureParamDestructuringFailure failure(solution, ContextualType,
                                           getLocator());
  return failure.diagnose(asNote);
}

AllowClosureParamDestructuring *
AllowClosureParamDestructuring::create(ConstraintSystem &cs,
                                       FunctionType *contextualType,
                                       ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowClosureParamDestructuring(cs, contextualType, locator);
}

bool AddMissingArguments::diagnose(const Solution &solution,
                                   bool asNote) const {
  MissingArgumentsFailure failure(solution, getSynthesizedArguments(),
                                  getLocator());
  return failure.diagnose(asNote);
}

AddMissingArguments *
AddMissingArguments::create(ConstraintSystem &cs,
                            ArrayRef<SynthesizedArg> synthesizedArgs,
                            ConstraintLocator *locator) {
  unsigned size = totalSizeToAlloc<SynthesizedArg>(synthesizedArgs.size());
  void *mem = cs.getAllocator().Allocate(size, alignof(AddMissingArguments));
  return new (mem) AddMissingArguments(cs, synthesizedArgs, locator);
}

bool RemoveExtraneousArguments::diagnose(const Solution &solution,
                                         bool asNote) const {
  ExtraneousArgumentsFailure failure(solution, ContextualType,
                                     getExtraArguments(), getLocator());
  return failure.diagnose(asNote);
}

bool RemoveExtraneousArguments::isMinMaxNameShadowing(
    ConstraintSystem &cs, ConstraintLocatorBuilder locator) {
  auto *anchor = getAsExpr<CallExpr>(locator.getAnchor());
  if (!anchor)
    return false;

  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(anchor->getFn())) {
    if (auto *baseExpr = dyn_cast<DeclRefExpr>(UDE->getBase())) {
      auto *decl = baseExpr->getDecl();
      if (baseExpr->isImplicit() && decl &&
          decl->getName() == cs.getASTContext().Id_self) {
        auto memberName = UDE->getName();
        return memberName.isSimpleName("min") || memberName.isSimpleName("max");
      }
    }
  }

  return false;
}

RemoveExtraneousArguments *RemoveExtraneousArguments::create(
    ConstraintSystem &cs, FunctionType *contextualType,
    llvm::ArrayRef<IndexedParam> extraArgs, ConstraintLocator *locator) {
  unsigned size = totalSizeToAlloc<IndexedParam>(extraArgs.size());
  void *mem = cs.getAllocator().Allocate(size, alignof(RemoveExtraneousArguments));
  return new (mem)
      RemoveExtraneousArguments(cs, contextualType, extraArgs, locator);
}

bool MoveOutOfOrderArgument::diagnose(const Solution &solution,
                                      bool asNote) const {
  OutOfOrderArgumentFailure failure(solution, ArgIdx, PrevArgIdx, Bindings,
                                    getLocator());
  return failure.diagnose(asNote);
}

bool MoveOutOfOrderArgument::diagnoseForAmbiguity(
    CommonFixesArray commonFixes) const {
  auto *primaryFix =
      commonFixes.front().second->getAs<MoveOutOfOrderArgument>();
  assert(primaryFix);

  if (llvm::all_of(
          commonFixes,
          [&primaryFix](
              const std::pair<const Solution *, const ConstraintFix *> &entry) {
            return primaryFix->isEqual(entry.second);
          }))
    return diagnose(*commonFixes.front().first);

  return false;
}

bool MoveOutOfOrderArgument::isEqual(const ConstraintFix *other) const {
  auto OoOFix = other->getAs<MoveOutOfOrderArgument>();
  return OoOFix ? ArgIdx == OoOFix->ArgIdx && PrevArgIdx == OoOFix->PrevArgIdx
                : false;
}

MoveOutOfOrderArgument *MoveOutOfOrderArgument::create(
    ConstraintSystem &cs, unsigned argIdx, unsigned prevArgIdx,
    ArrayRef<ParamBinding> bindings, ConstraintLocator *locator) {
  return new (cs.getAllocator())
      MoveOutOfOrderArgument(cs, argIdx, prevArgIdx, bindings, locator);
}

bool AllowInaccessibleMember::diagnose(const Solution &solution,
                                       bool asNote) const {
  InaccessibleMemberFailure failure(solution, getMember(), getLocator());
  return failure.diagnose(asNote);
}

AllowInaccessibleMember *
AllowInaccessibleMember::create(ConstraintSystem &cs, Type baseType,
                                ValueDecl *member, DeclNameRef name,
                                ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowInaccessibleMember(cs, baseType, member, name, locator);
}

bool AllowAnyObjectKeyPathRoot::diagnose(const Solution &solution,
                                         bool asNote) const {
  AnyObjectKeyPathRootFailure failure(solution, getLocator());
  return failure.diagnose(asNote);
}

AllowAnyObjectKeyPathRoot *
AllowAnyObjectKeyPathRoot::create(ConstraintSystem &cs,
                                  ConstraintLocator *locator) {
  return new (cs.getAllocator()) AllowAnyObjectKeyPathRoot(cs, locator);
}

bool AllowMultiArgFuncKeyPathMismatch::diagnose(const Solution &solution,
                                                bool asNote) const {
  MultiArgFuncKeyPathFailure failure(solution, functionType, getLocator());
  return failure.diagnose(asNote);
}

AllowMultiArgFuncKeyPathMismatch *
AllowMultiArgFuncKeyPathMismatch::create(ConstraintSystem &cs, Type fnType,
                                         ConstraintLocator *locator) {
  return new (cs.getAllocator())
  AllowMultiArgFuncKeyPathMismatch(cs, fnType, locator);
}

bool TreatKeyPathSubscriptIndexAsHashable::diagnose(const Solution &solution,
                                                    bool asNote) const {
  KeyPathSubscriptIndexHashableFailure failure(solution, NonConformingType,
                                               getLocator());
  return failure.diagnose(asNote);
}

TreatKeyPathSubscriptIndexAsHashable *
TreatKeyPathSubscriptIndexAsHashable::create(ConstraintSystem &cs, Type type,
                                             ConstraintLocator *locator) {
  return new (cs.getAllocator())
      TreatKeyPathSubscriptIndexAsHashable(cs, type, locator);
}

bool AllowInvalidRefInKeyPath::diagnose(const Solution &solution,
                                        bool asNote) const {
  switch (Kind) {
  case RefKind::StaticMember: {
    InvalidStaticMemberRefInKeyPath failure(solution, Member, getLocator());
    return failure.diagnose(asNote);
  }

  case RefKind::EnumCase: {
    InvalidEnumCaseRefInKeyPath failure(solution, Member, getLocator());
    return failure.diagnose(asNote);
  }

  case RefKind::MutatingGetter: {
    InvalidMemberWithMutatingGetterInKeyPath failure(solution, Member,
                                                     getLocator());
    return failure.diagnose(asNote);
  }

  case RefKind::Method:
  case RefKind::Initializer: {
    InvalidMethodRefInKeyPath failure(solution, Member, getLocator());
    return failure.diagnose(asNote);
  }
  }
  llvm_unreachable("covered switch");
}

bool AllowInvalidRefInKeyPath::diagnoseForAmbiguity(
    CommonFixesArray commonFixes) const {
  auto *primaryFix =
      commonFixes.front().second->getAs<AllowInvalidRefInKeyPath>();
  assert(primaryFix);

  if (llvm::all_of(
          commonFixes,
          [&primaryFix](
              const std::pair<const Solution *, const ConstraintFix *> &entry) {
            return primaryFix->isEqual(entry.second);
          })) {
    return diagnose(*commonFixes.front().first);
  }

  return false;
}

bool AllowInvalidRefInKeyPath::isEqual(const ConstraintFix *other) const {
  auto *refFix = other->getAs<AllowInvalidRefInKeyPath>();
  return refFix ? Kind == refFix->Kind && Member == refFix->Member : false;
}

AllowInvalidRefInKeyPath *
AllowInvalidRefInKeyPath::forRef(ConstraintSystem &cs, ValueDecl *member,
                                 ConstraintLocator *locator) {
  // Referencing (instance or static) methods in key path is
  // not currently allowed.
  if (isa<FuncDecl>(member))
    return AllowInvalidRefInKeyPath::create(cs, RefKind::Method, member,
                                            locator);

  // Referencing enum cases in key path is not currently allowed.
  if (isa<EnumElementDecl>(member)) {
    return AllowInvalidRefInKeyPath::create(cs, RefKind::EnumCase, member,
                                            locator);
  }

  // Referencing initializers in key path is not currently allowed.
  if (isa<ConstructorDecl>(member))
    return AllowInvalidRefInKeyPath::create(cs, RefKind::Initializer,
                                            member, locator);

  // Referencing static members in key path is not currently allowed.
  if (member->isStatic())
    return AllowInvalidRefInKeyPath::create(cs, RefKind::StaticMember, member,
                                            locator);

  if (auto *storage = dyn_cast<AbstractStorageDecl>(member)) {
    // Referencing members with mutating getters in key path is not
    // currently allowed.
    if (storage->isGetterMutating())
      return AllowInvalidRefInKeyPath::create(cs, RefKind::MutatingGetter,
                                              member, locator);
  }

  return nullptr;
}

AllowInvalidRefInKeyPath *
AllowInvalidRefInKeyPath::create(ConstraintSystem &cs, RefKind kind,
                                 ValueDecl *member,
                                 ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowInvalidRefInKeyPath(cs, kind, member, locator);
}

KeyPathContextualMismatch *
KeyPathContextualMismatch::create(ConstraintSystem &cs, Type lhs, Type rhs,
                                  ConstraintLocator *locator) {
  return new (cs.getAllocator())
      KeyPathContextualMismatch(cs, lhs, rhs, locator);
}

bool RemoveAddressOf::diagnose(const Solution &solution, bool asNote) const {
  InvalidUseOfAddressOf failure(solution, getFromType(), getToType(),
                                getLocator());
  return failure.diagnose(asNote);
}

RemoveAddressOf *RemoveAddressOf::create(ConstraintSystem &cs, Type lhs, Type rhs,
                                         ConstraintLocator *locator) {
  return new (cs.getAllocator()) RemoveAddressOf(cs, lhs, rhs, locator);
}

RemoveReturn::RemoveReturn(ConstraintSystem &cs, Type resultTy,
                           ConstraintLocator *locator)
    : ContextualMismatch(cs, FixKind::RemoveReturn, resultTy,
                         cs.getASTContext().TheEmptyTupleType, locator) {}

bool RemoveReturn::diagnose(const Solution &solution, bool asNote) const {
  ExtraneousReturnFailure failure(solution, getLocator());
  return failure.diagnose(asNote);
}

RemoveReturn *RemoveReturn::create(ConstraintSystem &cs, Type resultTy,
                                   ConstraintLocator *locator) {
  return new (cs.getAllocator()) RemoveReturn(cs, resultTy, locator);
}

NotCompileTimeConst::NotCompileTimeConst(ConstraintSystem &cs, Type paramTy,
                                         ConstraintLocator *locator):
  ContextualMismatch(cs, FixKind::NotCompileTimeConst, paramTy,
                     cs.getASTContext().TheEmptyTupleType, locator,
                     FixBehavior::AlwaysWarning) {}

NotCompileTimeConst *
NotCompileTimeConst::create(ConstraintSystem &cs, Type paramTy,
                            ConstraintLocator *locator) {
  return new (cs.getAllocator()) NotCompileTimeConst(cs, paramTy, locator);
}

bool NotCompileTimeConst::diagnose(const Solution &solution, bool asNote) const {
  auto *locator = getLocator();
  if (auto *E = getAsExpr(locator->getAnchor())) {
    auto isAccepted = E->isSemanticallyConstExpr([&](Expr *E) {
      if (auto *UMC = dyn_cast<UnresolvedMemberChainResultExpr>(E)) {
        E = UMC->getSubExpr();
      }
      auto locator = solution.getConstraintSystem().getConstraintLocator(E);
      // Referencing an enum element directly is considered a compile-time literal.
      if (auto *d = solution.resolveLocatorToDecl(locator).getDecl()) {
        if (isa<EnumElementDecl>(d)) {
          if (!d->hasParameterList()) {
            return true;
          }
        }
      }
      return false;
    });
    if (isAccepted)
      return true;
  }

  NotCompileTimeConstFailure failure(solution, locator);
  return failure.diagnose(asNote);
}

MustBeCopyable::MustBeCopyable(ConstraintSystem &cs, Type noncopyableTy, ConstraintLocator *locator)
    : ConstraintFix(cs, FixKind::MustBeCopyable, locator, FixBehavior::Error),
      noncopyableTy(noncopyableTy) {}

bool MustBeCopyable::diagnose(const Solution &solution, bool asNote) const {
  NotCopyableFailure failure(solution, noncopyableTy, getLocator());
  return failure.diagnose(asNote);
}

MustBeCopyable* MustBeCopyable::create(ConstraintSystem &cs,
                                              Type noncopyableTy,
                                              ConstraintLocator *locator) {
  return new (cs.getAllocator()) MustBeCopyable(cs, noncopyableTy, locator);
}

bool MustBeCopyable::diagnoseForAmbiguity(CommonFixesArray commonFixes) const {
  // Only diagnose if all solutions agreed on the same errant non-copyable type.
  Type firstNonCopyable;
  for (const auto &solutionAndFix : commonFixes) {
    const auto *solution = solutionAndFix.first;
    const auto *fix = solutionAndFix.second->getAs<MustBeCopyable>();

    auto otherNonCopyable = solution->simplifyType(fix->noncopyableTy);
    if (!firstNonCopyable)
      firstNonCopyable = otherNonCopyable;

    if (firstNonCopyable->getCanonicalType() != otherNonCopyable->getCanonicalType()) {
      return false; // fixes differed, so decline to emit a tailored diagnostic.
    }
  }

  return diagnose(*commonFixes.front().first);
}

bool AllowInvalidPackElement::diagnose(const Solution &solution,
                                       bool asNote) const {
  InvalidPackElement failure(solution, packElementType, getLocator());
  return failure.diagnose(asNote);
}

AllowInvalidPackElement *
AllowInvalidPackElement::create(ConstraintSystem &cs,
                                Type packElementType,
                                ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowInvalidPackElement(cs, packElementType, locator);
}

bool AllowInvalidPackReference::diagnose(const Solution &solution,
                                         bool asNote) const {
  InvalidPackReference failure(solution, packType, getLocator());
  return failure.diagnose(asNote);
}

AllowInvalidPackReference *
AllowInvalidPackReference::create(ConstraintSystem &cs, Type packType,
                                  ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowInvalidPackReference(cs, packType, locator);
}

bool AllowInvalidPackExpansion::diagnose(const Solution &solution,
                                         bool asNote) const {
  InvalidPackExpansion failure(solution, getLocator());
  return failure.diagnose(asNote);
}

AllowInvalidPackExpansion *
AllowInvalidPackExpansion::create(ConstraintSystem &cs,
                                  ConstraintLocator *locator) {
  return new (cs.getAllocator()) AllowInvalidPackExpansion(cs, locator);
}

bool CollectionElementContextualMismatch::diagnose(const Solution &solution,
                                                   bool asNote) const {
  CollectionElementContextualFailure failure(
      solution, getElements(), getFromType(), getToType(), getLocator());
  return failure.diagnose(asNote);
}

CollectionElementContextualMismatch *
CollectionElementContextualMismatch::create(ConstraintSystem &cs, Type srcType,
                                            Type dstType,
                                            ConstraintLocator *locator) {
  // It's common for a single literal element to represent types of other
  // literal elements of the same kind, let's check whether that is the case
  // here and record all of the affected positions.

  SmallVector<Expr *, 4> affected;
  {
    if (auto *elementLoc = getAsExpr(simplifyLocatorToAnchor(locator))) {
      auto *typeVar = cs.getType(elementLoc)->getAs<TypeVariableType>();
      if (typeVar && typeVar->getImpl().getAtomicLiteralKind()) {
        const auto *node =
            cs.getRepresentative(typeVar)->getImpl().getGraphNode();
        for (auto *typeVar : node->getEquivalenceClass()) {
          auto *locator = typeVar->getImpl().getLocator();
          if (auto *eltLoc = getAsExpr(simplifyLocatorToAnchor(locator)))
            affected.push_back(eltLoc);
        }
      }
    }
  }

  unsigned size = totalSizeToAlloc<Expr *>(affected.size());
  void *mem = cs.getAllocator().Allocate(
      size, alignof(CollectionElementContextualMismatch));
  return new (mem) CollectionElementContextualMismatch(cs, affected, srcType,
                                                       dstType, locator);
}

bool DefaultGenericArgument::coalesceAndDiagnose(
    const Solution &solution, ArrayRef<ConstraintFix *> fixes,
    bool asNote) const {
  llvm::SmallVector<GenericTypeParamType *, 4> missingParams{Param};

  for (auto *otherFix : fixes) {
    if (auto *fix = otherFix->getAs<DefaultGenericArgument>())
      missingParams.push_back(fix->Param);
  }

  MissingGenericArgumentsFailure failure(solution, missingParams, getLocator());
  return failure.diagnose(asNote);
}

bool DefaultGenericArgument::diagnose(const Solution &solution,
                                      bool asNote) const {
  return coalesceAndDiagnose(solution, {}, asNote);
}

DefaultGenericArgument *
DefaultGenericArgument::create(ConstraintSystem &cs, GenericTypeParamType *param,
                               ConstraintLocator *locator) {
  return new (cs.getAllocator()) DefaultGenericArgument(cs, param, locator);
}

SkipUnhandledConstructInResultBuilder *
SkipUnhandledConstructInResultBuilder::create(ConstraintSystem &cs,
                                                UnhandledNode unhandled,
                                                NominalTypeDecl *builder,
                                                ConstraintLocator *locator) {
  return new (cs.getAllocator())
    SkipUnhandledConstructInResultBuilder(cs, unhandled, builder, locator);
}

bool SkipUnhandledConstructInResultBuilder::diagnose(const Solution &solution,
                                                       bool asNote) const {
  SkipUnhandledConstructInResultBuilderFailure failure(solution, unhandled,
                                                         builder, getLocator());
  return failure.diagnose(asNote);
}

bool AllowMutatingMemberOnRValueBase::diagnose(const Solution &solution,
                                               bool asNote) const {
  MutatingMemberRefOnImmutableBase failure(solution, getMember(), getLocator());
  return failure.diagnose(asNote);
}

AllowMutatingMemberOnRValueBase *
AllowMutatingMemberOnRValueBase::create(ConstraintSystem &cs, Type baseType,
                                        ValueDecl *member, DeclNameRef name,
                                        ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowMutatingMemberOnRValueBase(cs, baseType, member, name, locator);
}

bool AllowTupleSplatForSingleParameter::diagnose(const Solution &solution,
                                                 bool asNote) const {
  InvalidTupleSplatWithSingleParameterFailure failure(solution, ParamType,
                                                      getLocator());
  return failure.diagnose(asNote);
}

bool AllowTupleSplatForSingleParameter::attempt(
    ConstraintSystem &cs, SmallVectorImpl<Param> &args, ArrayRef<Param> params,
    SmallVectorImpl<SmallVector<unsigned, 1>> &bindings,
    ConstraintLocatorBuilder locator) {
  if (params.size() != 1 || args.size() <= 1)
    return true;

  const auto &param = params.front();

  if (param.isInOut() || param.isVariadic() || param.isAutoClosure())
    return true;

  auto paramTy = param.getOldType();

  // Parameter type has to be either a tuple (with the same arity as
  // argument list), or a type variable.
  if (!(paramTy->is<TupleType>() &&
        paramTy->castTo<TupleType>()->getNumElements() == args.size()))
    return true;

  SmallVector<TupleTypeElt, 4> argElts;

  for (unsigned index : indices(args)) {
    const auto &arg = args[index];

    auto label = arg.getLabel();
    auto flags = arg.getParameterFlags();

    // In situations where there is a single labeled parameter
    // we need to form a tuple which omits the label e.g.
    //
    // func foo<T>(x: (T, T)) {}
    // foo(x: 0, 1)
    //
    // We'd want to suggest argument list to be `x: (0, 1)` instead
    // of `(x: 0, 1)` which would be incorrect.
    if (param.hasLabel() && label == param.getLabel()) {
      if (index == 0) {
        label = Identifier();
      } else {
        // If label match anything other than first argument,
        // this can't be a tuple splat.
        return true;
      }
    }

    // Tuple can't have `inout` elements.
    if (flags.isInOut())
      return true;

    argElts.push_back({arg.getPlainType(), label});
  }

  bindings[0].clear();
  bindings[0].push_back(0);

  auto newArgType = TupleType::get(argElts, cs.getASTContext());

  args.clear();
  args.push_back(AnyFunctionType::Param(newArgType, param.getLabel()));

  auto *fix = new (cs.getAllocator()) AllowTupleSplatForSingleParameter(
      cs, paramTy, cs.getConstraintLocator(locator));

  return cs.recordFix(fix);
}

bool DropThrowsAttribute::diagnose(const Solution &solution,
                                   bool asNote) const {
  ThrowingFunctionConversionFailure failure(solution, getFromType(),
                                            getToType(), getLocator());
  return failure.diagnose(asNote);
}

DropThrowsAttribute *DropThrowsAttribute::create(ConstraintSystem &cs,
                                                 FunctionType *fromType,
                                                 FunctionType *toType,
                                                 ConstraintLocator *locator) {
  return new (cs.getAllocator())
      DropThrowsAttribute(cs, fromType, toType, locator);
}

bool DropAsyncAttribute::diagnose(const Solution &solution,
                                   bool asNote) const {
  AsyncFunctionConversionFailure failure(solution, getFromType(),
                                         getToType(), getLocator());
  return failure.diagnose(asNote);
}

DropAsyncAttribute *DropAsyncAttribute::create(ConstraintSystem &cs,
                                               FunctionType *fromType,
                                               FunctionType *toType,
                                               ConstraintLocator *locator) {
  return new (cs.getAllocator())
      DropAsyncAttribute(cs, fromType, toType, locator);
}

bool IgnoreContextualType::diagnose(const Solution &solution,
                                    bool asNote) const {
  ContextualFailure failure(solution, getFromType(), getToType(), getLocator());
  return failure.diagnose(asNote);
}

IgnoreContextualType *IgnoreContextualType::create(ConstraintSystem &cs,
                                                   Type resultTy,
                                                   Type specifiedTy,
                                                   ConstraintLocator *locator) {
  return new (cs.getAllocator())
      IgnoreContextualType(cs, resultTy, specifiedTy, locator);
}

bool IgnoreAssignmentDestinationType::diagnose(const Solution &solution,
                                               bool asNote) const {
  auto &cs = getConstraintSystem();
  auto *AE = getAsExpr<AssignExpr>(getAnchor());

  assert(AE);

  // Let's check whether this is a situation of chained assignment where
  // one of the steps in the chain is an assignment to self e.g.
  // `let _ = { $0 = $0 = 42 }`. Assignment chaining results in
  // type mismatch between result of the previous assignment and the next.
  {
    llvm::SaveAndRestore<AssignExpr *> anchor(AE);

    do {
      if (TypeChecker::diagnoseSelfAssignment(AE))
        return true;
    } while ((AE = dyn_cast_or_null<AssignExpr>(cs.getParentExpr(AE))));
  }

  auto CTP = isa<SubscriptExpr>(AE->getDest()) ? CTP_SubscriptAssignSource
                                               : CTP_AssignSource;

  AssignmentTypeMismatchFailure failure(
      solution, CTP, getFromType(), getToType(),
      cs.getConstraintLocator(AE->getSrc(),
                              LocatorPathElt::ContextualType(CTP)));
  return failure.diagnose(asNote);
}

bool IgnoreAssignmentDestinationType::diagnoseForAmbiguity(
    CommonFixesArray commonFixes) const {
  auto &cs = getConstraintSystem();

  // If all of the types are the same let's try to diagnose
  // this as if there is no ambiguity.
  if (ContextualMismatch::diagnoseForAmbiguity(commonFixes))
    return true;

  auto *commonLocator = getLocator();
  auto *assignment = castToExpr<AssignExpr>(commonLocator->getAnchor());

  auto &solution = *commonFixes.front().first;
  auto *calleeLocator = solution.getCalleeLocator(
      solution.getConstraintLocator(assignment->getSrc()));
  auto overload = solution.getOverloadChoiceIfAvailable(calleeLocator);
  if (!overload)
    return false;

  auto memberName = overload->choice.getName().getBaseName();
  auto destType = solution.getType(assignment->getDest());

  auto &DE = cs.getASTContext().Diags;
  // TODO(diagnostics): It might be good to add a tailored diagnostic
  // for cases like this instead of using "contextual" one.
  DE.diagnose(assignment->getSrc()->getLoc(),
              diag::no_candidates_match_result_type,
              memberName.userFacingName(),
              solution.simplifyType(destType)->getRValueType());

  for (auto &entry : commonFixes) {
    entry.second->diagnose(*entry.first, /*asNote=*/true);
  }

  return true;
}

IgnoreAssignmentDestinationType *
IgnoreAssignmentDestinationType::create(ConstraintSystem &cs, Type sourceTy,
                                        Type destTy,
                                        ConstraintLocator *locator) {
  return new (cs.getAllocator())
      IgnoreAssignmentDestinationType(cs, sourceTy, destTy, locator);
}

bool AllowInOutConversion::diagnose(const Solution &solution,
                                    bool asNote) const {
  InOutConversionFailure failure(solution, getFromType(), getToType(),
                                 getLocator());
  return failure.diagnose(asNote);
}

AllowInOutConversion *AllowInOutConversion::create(ConstraintSystem &cs,
                                                   Type argType, Type paramType,
                                                   ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowInOutConversion(cs, argType, paramType, locator);
}

ExpandArrayIntoVarargs *
ExpandArrayIntoVarargs::attempt(ConstraintSystem &cs, Type argType,
                                Type paramType,
                                ConstraintLocatorBuilder builder) {
  auto *locator = cs.getConstraintLocator(builder);

  auto argLoc = locator->getLastElementAs<LocatorPathElt::ApplyArgToParam>();
  if (!(argLoc && argLoc->getParameterFlags().isVariadic()))
    return nullptr;

  auto elementType = argType->isArrayType();
  if (!elementType)
    return nullptr;

  ConstraintSystem::TypeMatchOptions options;
  options |= ConstraintSystem::TypeMatchFlags::TMF_ApplyingFix;
  options |= ConstraintSystem::TypeMatchFlags::TMF_GenerateConstraints;

  auto result = cs.matchTypes(elementType, paramType, ConstraintKind::Subtype,
                              options, builder);

  if (result.isFailure())
    return nullptr;

  return new (cs.getAllocator())
      ExpandArrayIntoVarargs(cs, argType, paramType, locator);
}

bool ExpandArrayIntoVarargs::diagnose(const Solution &solution,
                                      bool asNote) const {
  ExpandArrayIntoVarargsFailure failure(solution, getFromType(), getToType(),
                                        getLocator());
  return failure.diagnose(asNote);
}

bool ExplicitlyConstructRawRepresentable::diagnose(const Solution &solution,
                                                   bool asNote) const {
  MissingRawRepresentableInitFailure failure(solution, RawReprType,
                                             ExpectedType, getLocator());
  return failure.diagnose(asNote);
}

ExplicitlyConstructRawRepresentable *
ExplicitlyConstructRawRepresentable::create(ConstraintSystem &cs,
                                            Type rawReprType, Type expectedType,
                                            ConstraintLocator *locator) {
  return new (cs.getAllocator()) ExplicitlyConstructRawRepresentable(
      cs, rawReprType, expectedType, locator);
}

bool UseRawValue::diagnose(const Solution &solution, bool asNote) const {
  MissingRawValueFailure failure(solution, RawReprType, ExpectedType,
                                 getLocator());
  return failure.diagnose(asNote);
}

UseRawValue *UseRawValue::create(ConstraintSystem &cs, Type rawReprType,
                                 Type expectedType,
                                 ConstraintLocator *locator) {
  return new (cs.getAllocator())
      UseRawValue(cs, rawReprType, expectedType, locator);
}

unsigned AllowArgumentMismatch::getParamIdx() const {
  const auto *locator = getLocator();
  auto elt = locator->castLastElementTo<LocatorPathElt::ApplyArgToParam>();
  return elt.getParamIdx();
}

bool AllowArgumentMismatch::diagnose(const Solution &solution,
                                     bool asNote) const {
  ArgumentMismatchFailure failure(solution, getFromType(), getToType(),
                                  getLocator());
  return failure.diagnose(asNote);
}

AllowArgumentMismatch *
AllowArgumentMismatch::create(ConstraintSystem &cs, Type argType,
                              Type paramType, ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowArgumentMismatch(cs, argType, paramType, locator);
}

bool RemoveInvalidCall::diagnose(const Solution &solution, bool asNote) const {
  ExtraneousCallFailure failure(solution, getLocator());
  return failure.diagnose(asNote);
}

RemoveInvalidCall *RemoveInvalidCall::create(ConstraintSystem &cs,
                                             ConstraintLocator *locator) {
  return new (cs.getAllocator()) RemoveInvalidCall(cs, locator);
}

bool TreatEphemeralAsNonEphemeral::diagnose(const Solution &solution,
                                            bool asNote) const {
  NonEphemeralConversionFailure failure(solution, getLocator(), getFromType(),
                                        getToType(), ConversionKind,
                                        fixBehavior);
  return failure.diagnose(asNote);
}

TreatEphemeralAsNonEphemeral *TreatEphemeralAsNonEphemeral::create(
    ConstraintSystem &cs, ConstraintLocator *locator, Type srcType,
    Type dstType, ConversionRestrictionKind conversionKind,
    bool downgradeToWarning) {
  return new (cs.getAllocator()) TreatEphemeralAsNonEphemeral(
      cs, locator, srcType, dstType, conversionKind,
      downgradeToWarning ? FixBehavior::DowngradeToWarning
                         : FixBehavior::Error);
}

std::string TreatEphemeralAsNonEphemeral::getName() const {
  std::string name;
  name += "treat ephemeral as non-ephemeral for ";
  name += ::getName(ConversionKind);
  return name;
}

bool SpecifyBaseTypeForContextualMember::diagnose(const Solution &solution,
                                                  bool asNote) const {
  MissingContextualBaseInMemberRefFailure failure(solution, MemberName,
                                                  getLocator());
  return failure.diagnose(asNote);
}

SpecifyBaseTypeForContextualMember *SpecifyBaseTypeForContextualMember::create(
    ConstraintSystem &cs, DeclNameRef member, ConstraintLocator *locator) {
  return new (cs.getAllocator())
      SpecifyBaseTypeForContextualMember(cs, member, locator);
}

std::string SpecifyClosureParameterType::getName() const {
  std::string name;
  llvm::raw_string_ostream OS(name);

  auto *closure = castToExpr<ClosureExpr>(getAnchor());
  auto paramLoc =
      getLocator()->castLastElementTo<LocatorPathElt::TupleElement>();

  auto *PD = closure->getParameters()->get(paramLoc.getIndex());

  OS << "specify type for parameter ";
  OS << "'" << PD->getParameterName() << "'";

  return OS.str();
}

bool SpecifyClosureParameterType::diagnose(const Solution &solution,
                                           bool asNote) const {
  UnableToInferClosureParameterType failure(solution, getLocator());
  return failure.diagnose(asNote);
}

SpecifyClosureParameterType *
SpecifyClosureParameterType::create(ConstraintSystem &cs,
                                    ConstraintLocator *locator) {
  return new (cs.getAllocator()) SpecifyClosureParameterType(cs, locator);
}

bool SpecifyClosureReturnType::diagnose(const Solution &solution,
                                        bool asNote) const {
  UnableToInferClosureReturnType failure(solution, getLocator());
  return failure.diagnose(asNote);
}

SpecifyClosureReturnType *
SpecifyClosureReturnType::create(ConstraintSystem &cs,
                                 ConstraintLocator *locator) {
  return new (cs.getAllocator()) SpecifyClosureReturnType(cs, locator);
}

bool SpecifyObjectLiteralTypeImport::diagnose(const Solution &solution,
                                              bool asNote) const {
  UnableToInferProtocolLiteralType failure(solution, getLocator());
  return failure.diagnose(asNote);
}

SpecifyObjectLiteralTypeImport *
SpecifyObjectLiteralTypeImport::create(ConstraintSystem &cs,
                                       ConstraintLocator *locator) {
  return new (cs.getAllocator()) SpecifyObjectLiteralTypeImport(cs, locator);
}

AllowNonClassTypeToConvertToAnyObject::AllowNonClassTypeToConvertToAnyObject(
    ConstraintSystem &cs, Type type, ConstraintLocator *locator)
    : ContextualMismatch(cs, FixKind::AllowNonClassTypeToConvertToAnyObject,
                         type, cs.getASTContext().getAnyObjectType(), locator) {
}

bool AllowNonClassTypeToConvertToAnyObject::diagnose(const Solution &solution,
                                                     bool asNote) const {
  auto *locator = getLocator();

  NonClassTypeToAnyObjectConversionFailure failure(solution, getFromType(),
                                                   getToType(), locator);

  return failure.diagnose(asNote);
}

AllowNonClassTypeToConvertToAnyObject *
AllowNonClassTypeToConvertToAnyObject::create(ConstraintSystem &cs, Type type,
                                              ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowNonClassTypeToConvertToAnyObject(cs, type, locator);
}

bool AddQualifierToAccessTopLevelName::diagnose(const Solution &solution,
                                                bool asNote) const {
  MissingQualifierInMemberRefFailure failure(solution, getLocator());
  return failure.diagnose(asNote);
}

AddQualifierToAccessTopLevelName *
AddQualifierToAccessTopLevelName::create(ConstraintSystem &cs,
                                         ConstraintLocator *locator) {
  return new (cs.getAllocator()) AddQualifierToAccessTopLevelName(cs, locator);
}

bool AllowCoercionToForceCast::diagnose(const Solution &solution,
                                        bool asNote) const {
  CoercionAsForceCastFailure failure(solution, getFromType(), getToType(),
                                     getLocator());
  return failure.diagnose(asNote);
}

AllowCoercionToForceCast *
AllowCoercionToForceCast::create(ConstraintSystem &cs, Type fromType,
                                 Type toType, ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowCoercionToForceCast(cs, fromType, toType, locator);
}

bool AllowKeyPathRootTypeMismatch::diagnose(const Solution &solution,
                                            bool asNote) const {
  KeyPathRootTypeMismatchFailure failure(solution, getFromType(), getToType(),
                                         getLocator());
  return failure.diagnose(asNote);
}

AllowKeyPathRootTypeMismatch *
AllowKeyPathRootTypeMismatch::create(ConstraintSystem &cs, Type lhs, Type rhs,
                                     ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowKeyPathRootTypeMismatch(cs, lhs, rhs, locator);
}

SpecifyKeyPathRootType *
SpecifyKeyPathRootType::create(ConstraintSystem &cs,
                               ConstraintLocator *locator) {
  return new (cs.getAllocator())
      SpecifyKeyPathRootType(cs, locator);
}

bool SpecifyKeyPathRootType::diagnose(const Solution &solution,
                                      bool asNote) const {
  UnableToInferKeyPathRootFailure failure(solution, getLocator());
  
  return failure.diagnose(asNote);
}

bool UnwrapOptionalBaseKeyPathApplication::diagnose(const Solution &solution,
                                                    bool asNote) const {
  MissingOptionalUnwrapKeyPathFailure failure(solution, getFromType(),
                                              getToType(), getLocator());
  return failure.diagnose(asNote);
}

UnwrapOptionalBaseKeyPathApplication *
UnwrapOptionalBaseKeyPathApplication::attempt(ConstraintSystem &cs, Type baseTy,
                                              Type rootTy,
                                              ConstraintLocator *locator) {
  if(baseTy->hasTypeVariable() || rootTy->hasTypeVariable())
    return nullptr;

  if (!isExpr<SubscriptExpr>(locator->getAnchor()))
    return nullptr;
  
  // Only diagnose this if base is an optional type and we only have a
  // single level of optionality so we can safely suggest unwrapping.
  auto nonOptionalTy = baseTy->getOptionalObjectType();
  if (!nonOptionalTy || nonOptionalTy->getOptionalObjectType())
    return nullptr;

  auto result =
      cs.matchTypes(nonOptionalTy, rootTy, ConstraintKind::Subtype,
                    ConstraintSystem::TypeMatchFlags::TMF_ApplyingFix, locator);
  if (result.isFailure())
    return nullptr;

  return new (cs.getAllocator())
      UnwrapOptionalBaseKeyPathApplication(cs, baseTy, rootTy, locator);
}

bool SpecifyLabelToAssociateTrailingClosure::diagnose(const Solution &solution,
                                                      bool asNote) const {
  TrailingClosureRequiresExplicitLabel failure(solution, getLocator());
  return failure.diagnose(asNote);
}

SpecifyLabelToAssociateTrailingClosure *
SpecifyLabelToAssociateTrailingClosure::create(ConstraintSystem &cs,
                                               ConstraintLocator *locator) {
  return new (cs.getAllocator())
      SpecifyLabelToAssociateTrailingClosure(cs, locator);
}

bool AllowKeyPathWithoutComponents::diagnose(const Solution &solution,
                                             bool asNote) const {
  InvalidEmptyKeyPathFailure failure(solution, getLocator());
  return failure.diagnose(asNote);
}

AllowKeyPathWithoutComponents *
AllowKeyPathWithoutComponents::create(ConstraintSystem &cs,
                                      ConstraintLocator *locator) {
  return new (cs.getAllocator()) AllowKeyPathWithoutComponents(cs, locator);
}

bool IgnoreInvalidResultBuilderBody::diagnose(const Solution &solution,
                                              bool asNote) const {
  return true; // Already diagnosed by `matchResultBuilder`.
}

IgnoreInvalidResultBuilderBody *
IgnoreInvalidResultBuilderBody::create(ConstraintSystem &cs,
                                       ConstraintLocator *locator) {
  return new (cs.getAllocator()) IgnoreInvalidResultBuilderBody(cs, locator);
}

bool IgnoreInvalidASTNode::diagnose(const Solution &solution,
                                    bool asNote) const {
  return true; // Already diagnosed by the producer of ErrorExpr or ErrorType.
}

IgnoreInvalidASTNode *IgnoreInvalidASTNode::create(ConstraintSystem &cs,
                                                   ConstraintLocator *locator) {
  return new (cs.getAllocator()) IgnoreInvalidASTNode(cs, locator);
}

bool SpecifyContextualTypeForNil::diagnose(const Solution &solution,
                                           bool asNote) const {
  MissingContextualTypeForNil failure(solution, getLocator());
  return failure.diagnose(asNote);
}

SpecifyContextualTypeForNil *
SpecifyContextualTypeForNil::create(ConstraintSystem &cs,
                                    ConstraintLocator *locator) {
  return new (cs.getAllocator()) SpecifyContextualTypeForNil(cs, locator);
}

bool SpecifyTypeForPlaceholder::diagnose(const Solution &solution,
                                           bool asNote) const {
  CouldNotInferPlaceholderType failure(solution, getLocator());
  return failure.diagnose(asNote);
}

SpecifyTypeForPlaceholder *
SpecifyTypeForPlaceholder::create(ConstraintSystem &cs,
                                  ConstraintLocator *locator) {
  return new (cs.getAllocator()) SpecifyTypeForPlaceholder(cs, locator);
}

bool AllowRefToInvalidDecl::diagnose(const Solution &solution,
                                     bool asNote) const {
  ReferenceToInvalidDeclaration failure(solution, getLocator());
  return failure.diagnose(asNote);
}

AllowRefToInvalidDecl *
AllowRefToInvalidDecl::create(ConstraintSystem &cs,
                              ConstraintLocator *locator) {
  return new (cs.getAllocator()) AllowRefToInvalidDecl(cs, locator);
}

bool IgnoreResultBuilderWithReturnStmts::diagnose(const Solution &solution,
                                                  bool asNote) const {
  InvalidReturnInResultBuilderBody failure(solution, BuilderType, getLocator());
  return failure.diagnose(asNote);
}

IgnoreResultBuilderWithReturnStmts *
IgnoreResultBuilderWithReturnStmts::create(ConstraintSystem &cs, Type builderTy,
                                           ConstraintLocator *locator) {
  return new (cs.getAllocator())
      IgnoreResultBuilderWithReturnStmts(cs, builderTy, locator);
}

bool IgnoreUnresolvedPatternVar::diagnose(const Solution &solution,
                                          bool asNote) const {
  // An unresolved AnyPatternDecl means there was some issue in the match
  // that means we couldn't infer the pattern. We don't have a diagnostic to
  // emit here, the failure should be diagnosed by the fix for expression.
  return false;
}

IgnoreUnresolvedPatternVar *
IgnoreUnresolvedPatternVar::create(ConstraintSystem &cs, Pattern *pattern,
                                   ConstraintLocator *locator) {
  return new (cs.getAllocator())
      IgnoreUnresolvedPatternVar(cs, pattern, locator);
}

bool SpecifyBaseTypeForOptionalUnresolvedMember::diagnose(
    const Solution &solution, bool asNote) const {
  MemberMissingExplicitBaseTypeFailure failure(solution, MemberName,
                                               getLocator());
  return failure.diagnose(asNote);
}

SpecifyBaseTypeForOptionalUnresolvedMember *
SpecifyBaseTypeForOptionalUnresolvedMember::attempt(
    ConstraintSystem &cs, ConstraintKind kind, Type baseTy,
    DeclNameRef memberName, FunctionRefKind functionRefKind,
    MemberLookupResult result, ConstraintLocator *locator) {

  if (kind != ConstraintKind::UnresolvedValueMember)
    return nullptr;

  // None or only one viable candidate, there is no ambiguity.
  if (result.ViableCandidates.size() <= 1)
    return nullptr;

  // Only diagnose those situations for static members.
  if (!baseTy->is<MetatypeType>())
    return nullptr;

  // Don't diagnose for function members e.g. Foo? = .none(0).
  if (functionRefKind != FunctionRefKind::Unapplied)
    return nullptr;

  Type underlyingBaseType = baseTy->getMetatypeInstanceType();
  if (!underlyingBaseType->getNominalOrBoundGenericNominal())
    return nullptr;

  if (!underlyingBaseType->getOptionalObjectType())
    return nullptr;

  auto unwrappedType = underlyingBaseType->lookThroughAllOptionalTypes();
  bool allOptionalBaseCandidates = true;
  auto filterViableCandidates =
      [&](SmallVector<OverloadChoice, 4> &candidates,
          SmallVector<OverloadChoice, 4> &viableCandidates,
          bool &allOptionalBase) {
        for (OverloadChoice choice : candidates) {
          if (!choice.isDecl())
            continue;

          auto memberDecl = choice.getDecl();
          if (isa<FuncDecl>(memberDecl))
            continue;
          if (memberDecl->isInstanceMember())
            continue;

          // Disable this warning for ambiguities related to a
          // static member lookup in generic context because it's
          // possible to declare a member with the same name on
          // a concrete type and in an extension of a protocol
          // that type conforms to e.g.:
          //
          // struct S : P { static var test: S { ... }
          //
          // extension P where Self == S { static var test: { ... } }
          //
          // And use that in an optional context e.g. passing `.test`
          // to a parameter of expecting `S?`.
          if (auto *extension =
                  dyn_cast<ExtensionDecl>(memberDecl->getDeclContext())) {
            if (extension->getSelfProtocolDecl()) {
              allOptionalBase = false;
              break;
            }
          }

          allOptionalBase &= bool(choice.getBaseType()
                                      ->getMetatypeInstanceType()
                                      ->getOptionalObjectType());

          if (auto EED = dyn_cast<EnumElementDecl>(memberDecl)) {
            if (!EED->hasAssociatedValues())
              viableCandidates.push_back(choice);
          } else if (auto VD = dyn_cast<VarDecl>(memberDecl)) {
            if (unwrappedType->hasTypeVariable() ||
                VD->getInterfaceType()->isEqual(unwrappedType))
              viableCandidates.push_back(choice);
          }
        }
      };

  SmallVector<OverloadChoice, 4> viableCandidates;
  filterViableCandidates(result.ViableCandidates, viableCandidates,
                         allOptionalBaseCandidates);

  // Also none or only one viable candidate after filtering candidates, there is
  // no ambiguity.
  if (viableCandidates.size() <= 1)
    return nullptr;

  // Right now, name lookup only unwraps a single layer of optionality, which
  // for cases where base type is a multi-optional type e.g. Foo?? it only
  // finds optional base candidates. To produce the correct warning we perform
  // an extra lookup on unwrapped type.
  if (!allOptionalBaseCandidates)
    return new (cs.getAllocator())
        SpecifyBaseTypeForOptionalUnresolvedMember(cs, memberName, locator);

  MemberLookupResult unwrappedResult =
      cs.performMemberLookup(kind, memberName, MetatypeType::get(unwrappedType),
                             functionRefKind, locator,
                             /*includeInaccessibleMembers*/ false);
  SmallVector<OverloadChoice, 4> unwrappedViableCandidates;
  filterViableCandidates(unwrappedResult.ViableCandidates,
                         unwrappedViableCandidates, allOptionalBaseCandidates);
  if (unwrappedViableCandidates.empty())
    return nullptr;

  return new (cs.getAllocator())
      SpecifyBaseTypeForOptionalUnresolvedMember(cs, memberName, locator);
}

AllowCheckedCastCoercibleOptionalType *
AllowCheckedCastCoercibleOptionalType::create(ConstraintSystem &cs,
                                              Type fromType, Type toType,
                                              CheckedCastKind kind,
                                              ConstraintLocator *locator) {
  return new (cs.getAllocator()) AllowCheckedCastCoercibleOptionalType(
      cs, fromType, toType, kind, locator);
}

bool AllowCheckedCastCoercibleOptionalType::diagnose(const Solution &solution,
                                                     bool asNote) const {
  CoercibleOptionalCheckedCastFailure failure(
      solution, getFromType(), getToType(), CastKind, getLocator());
  return failure.diagnose(asNote);
}

AllowNoopCheckedCast *AllowNoopCheckedCast::create(ConstraintSystem &cs,
                                                   Type fromType, Type toType,
                                                   CheckedCastKind kind,
                                                   ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowNoopCheckedCast(cs, fromType, toType, kind, locator);
}

bool AllowNoopCheckedCast::diagnose(const Solution &solution,
                                    bool asNote) const {
  NoopCheckedCast warning(solution, getFromType(), getToType(), CastKind,
                          getLocator());
  return warning.diagnose(asNote);
}

AllowNoopExistentialToCFTypeCheckedCast *
AllowNoopExistentialToCFTypeCheckedCast::attempt(ConstraintSystem &cs,
                                                 Type fromType, Type toType,
                                                 CheckedCastKind kind,
                                                 ConstraintLocator *locator) {
  if (!isExpr<IsExpr>(locator->getAnchor()))
    return nullptr;

  if (!fromType->isExistentialType())
    return nullptr;

  const auto *cls = toType->getAs<ClassType>();
  if (!(cls && cls->getDecl()->getForeignClassKind() ==
                   ClassDecl::ForeignKind::CFType))
    return nullptr;

  return new (cs.getAllocator()) AllowNoopExistentialToCFTypeCheckedCast(
      cs, fromType, toType, kind, locator);
}

bool AllowNoopExistentialToCFTypeCheckedCast::diagnose(const Solution &solution,
                                                       bool asNote) const {
  NoopExistentialToCFTypeCheckedCast warning(
      solution, getFromType(), getToType(), CastKind, getLocator());
  return warning.diagnose(asNote);
}

// Although function types maybe compile-time convertible because
// compiler can emit thunks at SIL to handle the conversion when
// required, only conversions that are supported by the runtime are
// when types are trivially equal or non-throwing from type is equal
// to throwing to type without throwing clause conversions are not
// possible at runtime.
bool AllowUnsupportedRuntimeCheckedCast::runtimeSupportedFunctionTypeCast(
    FunctionType *fnFromType, FunctionType *fnToType) {
  if (fnFromType->isEqual(fnToType)) {
    return true;
  } else if (!fnFromType->isThrowing() && fnToType->isThrowing()) {
    return fnFromType->isEqual(
        fnToType->getWithoutThrowing()->castTo<FunctionType>());
  }
  // Runtime cannot perform such conversion.
  return false;
}

AllowUnsupportedRuntimeCheckedCast *
AllowUnsupportedRuntimeCheckedCast::attempt(ConstraintSystem &cs, Type fromType,
                                            Type toType, CheckedCastKind kind,
                                            ConstraintLocator *locator) {
  auto fnFromType = fromType->getAs<FunctionType>();
  auto fnToType = toType->getAs<FunctionType>();

  if (!(fnFromType && fnToType))
    return nullptr;

  if (runtimeSupportedFunctionTypeCast(fnFromType, fnToType))
    return nullptr;

  return new (cs.getAllocator())
      AllowUnsupportedRuntimeCheckedCast(cs, fromType, toType, kind, locator);
}

bool AllowUnsupportedRuntimeCheckedCast::diagnose(const Solution &solution,
                                                 bool asNote) const {
  UnsupportedRuntimeCheckedCastFailure failure(
      solution, getFromType(), getToType(), CastKind, getLocator());
  return failure.diagnose(asNote);
}

AllowCheckedCastToUnrelated *
AllowCheckedCastToUnrelated::attempt(ConstraintSystem &cs, Type fromType,
                                     Type toType, CheckedCastKind kind,
                                     ConstraintLocator *locator) {
  // Explicit optional-to-optional casts always succeed because a nil
  // value of any optional type can be cast to any other optional type.
  if (fromType->getOptionalObjectType() && toType->getOptionalObjectType()) {
    return nullptr;
  }
  return new (cs.getAllocator())
      AllowCheckedCastToUnrelated(cs, fromType, toType, kind, locator);
}

bool AllowCheckedCastToUnrelated::diagnose(const Solution &solution,
                                           bool asNote) const {
  CheckedCastToUnrelatedFailure warning(solution, getFromType(), getToType(),
                                        CastKind, getLocator());
  return warning.diagnose(asNote);
}

bool AllowInvalidStaticMemberRefOnProtocolMetatype::diagnose(
    const Solution &solution, bool asNote) const {
  InvalidMemberRefOnProtocolMetatype failure(solution, getLocator());
  return failure.diagnose(asNote);
}

AllowInvalidStaticMemberRefOnProtocolMetatype *
AllowInvalidStaticMemberRefOnProtocolMetatype::create(
    ConstraintSystem &cs, ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowInvalidStaticMemberRefOnProtocolMetatype(cs, locator);
}

bool AllowNonOptionalWeak::diagnose(const Solution &solution,
                                    bool asNote) const {
  InvalidWeakAttributeUse failure(solution, getLocator());
  return failure.diagnose(asNote);
}

AllowNonOptionalWeak *AllowNonOptionalWeak::create(ConstraintSystem &cs,
                                                   ConstraintLocator *locator) {
  return new (cs.getAllocator()) AllowNonOptionalWeak(cs, locator);
}

AllowTupleLabelMismatch *
AllowTupleLabelMismatch::create(ConstraintSystem &cs, Type fromType,
                                Type toType, ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowTupleLabelMismatch(cs, fromType, toType, locator);
}

bool AllowTupleLabelMismatch::diagnose(const Solution &solution,
                                       bool asNote) const {
  TupleLabelMismatchWarning warning(solution, getFromType(), getToType(),
                                    getLocator());
  return warning.diagnose(asNote);
}

AllowAssociatedValueMismatch *
AllowAssociatedValueMismatch::create(ConstraintSystem &cs, Type fromType,
                                     Type toType, ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowAssociatedValueMismatch(cs, fromType, toType, locator);
}

bool AllowAssociatedValueMismatch::diagnose(const Solution &solution,
                                            bool asNote) const {
  AssociatedValueMismatchFailure failure(solution, getFromType(), getToType(),
                                         getLocator());
  return failure.diagnose(asNote);
}

bool AllowSwiftToCPointerConversion::diagnose(const Solution &solution,
                                              bool asNote) const {
  SwiftToCPointerConversionInInvalidContext failure(solution, getLocator());
  return failure.diagnose(asNote);
}

AllowSwiftToCPointerConversion *
AllowSwiftToCPointerConversion::create(ConstraintSystem &cs,
                                       ConstraintLocator *locator) {
  return new (cs.getAllocator()) AllowSwiftToCPointerConversion(cs, locator);
}

bool IgnoreDefaultExprTypeMismatch::diagnose(const Solution &solution,
                                             bool asNote) const {
  DefaultExprTypeMismatch failure(solution, getFromType(), getToType(),
                                  getLocator());
  return failure.diagnose(asNote);
}

IgnoreDefaultExprTypeMismatch *
IgnoreDefaultExprTypeMismatch::create(ConstraintSystem &cs, Type argType,
                                      Type paramType,
                                      ConstraintLocator *locator) {
  return new (cs.getAllocator())
      IgnoreDefaultExprTypeMismatch(cs, argType, paramType, locator);
}

bool AddExplicitExistentialCoercion::diagnose(const Solution &solution,
                                              bool asNote) const {
  MissingExplicitExistentialCoercion failure(solution, ErasedResultType,
                                             getLocator(), fixBehavior);
  return failure.diagnose(asNote);
}

bool AddExplicitExistentialCoercion::isRequired(
    ConstraintSystem &cs, Type resultTy,
    llvm::function_ref<Optional<Type>(TypeVariableType *)> findExistentialType,
    ConstraintLocatorBuilder locator) {
  using ExistentialTypeFinder =
      llvm::function_ref<Optional<Type>(TypeVariableType *)>;

  struct CoercionChecker : public TypeWalker {
    bool RequiresCoercion = false;

    ConstraintSystem &cs;
    ExistentialTypeFinder GetExistentialType;

    CoercionChecker(ConstraintSystem &cs,
                    ExistentialTypeFinder getExistentialType)
        : cs(cs), GetExistentialType(getExistentialType) {}

    Action walkToTypePre(Type componentTy) override {
      // In cases where result references a member type, we need to check
      // whether such type would is resolved to concrete or not.
      if (auto *member = componentTy->getAs<DependentMemberType>()) {
        auto memberBaseTy = getBaseTypeOfDependentMemberChain(member);

        auto typeVar = memberBaseTy->getAs<TypeVariableType>();
        if (!typeVar)
          return Action::SkipChildren;

        // If the base is an opened existential type, let's see whether
        // erase would produce an existential in this case and if so,
        // we need to check whether any requirements are going to be lost
        // in process.

        auto existentialType = GetExistentialType(typeVar);
        if (!existentialType)
          return Action::SkipChildren;

        auto erasedMemberTy = typeEraseOpenedExistentialReference(
            Type(member), *existentialType, typeVar, TypePosition::Covariant);

        // If result is an existential type and the base has `where` clauses
        // associated with its associated types, the call needs a coercion.
        if (erasedMemberTy->isExistentialType() &&
            hasConstrainedAssociatedTypes(member, *existentialType)) {
          RequiresCoercion = true;
          return Action::Stop;
        }

        if (erasedMemberTy->isExistentialType() &&
             erasedMemberTy->hasTypeParameter()) {
          RequiresCoercion = true;
          return Action::Stop;
        }

        return Action::SkipChildren;
      }

      // The case where there is a direct access to opened existential type
      // e.g. `$T` or `[$T]`.
      if (auto *typeVar = componentTy->getAs<TypeVariableType>()) {
        if (auto existentialType = GetExistentialType(typeVar)) {
          RequiresCoercion |= hasAnyConstrainedAssociatedTypes(
              (*existentialType)->getExistentialLayout());
          return RequiresCoercion ? Action::Stop : Action::SkipChildren;
        }
      }

      return Action::Continue;
    }

  private:
    /// Check whether the given member type has any of its associated
    /// types constrained by requirements associated with the given
    /// existential type.
    static bool hasConstrainedAssociatedTypes(DependentMemberType *member,
                                              Type existentialTy) {
      auto layout = existentialTy->getExistentialLayout();
      for (auto *protocol : layout.getProtocols()) {
        auto requirementSig = protocol->getRequirementSignature();
        if (hasConstrainedAssociatedTypes(member,
                                          requirementSig.getRequirements()))
          return true;
      }
      return false;
    }

    /// Check whether the given member type has any of its associated
    /// types constrained by the given requirement set.
    static bool
    hasConstrainedAssociatedTypes(DependentMemberType *member,
                                  ArrayRef<Requirement> requirements) {
      for (const auto &req : requirements) {
        switch (req.getKind()) {
        case RequirementKind::SameShape:
          llvm_unreachable("Same-shape requirement not supported here");

        case RequirementKind::Superclass:
        case RequirementKind::Conformance:
        case RequirementKind::Layout: {
          if (isAnchoredOn(req.getFirstType(), member->getAssocType()))
            return true;
          break;
        }

        case RequirementKind::SameType: {
          auto lhsTy = req.getFirstType();
          auto rhsTy = req.getSecondType();

          if (isAnchoredOn(lhsTy, member->getAssocType()) ||
              isAnchoredOn(rhsTy, member->getAssocType()))
            return true;

          break;
        }
        }
      }

      return false;
    }

    /// Check whether any of the protocols mentioned in the given
    /// existential layout have constraints associated with their
    /// associated types via a `where` clause, for example:
    /// `associatedtype A: P where A.B == Int`
    static bool hasAnyConstrainedAssociatedTypes(ExistentialLayout layout) {
      for (auto *protocol : layout.getProtocols()) {
        auto requirementSig = protocol->getRequirementSignature();
        for (const auto &req : requirementSig.getRequirements()) {
          switch (req.getKind()) {
          case RequirementKind::SameShape:
            llvm_unreachable("Same-shape requirement not supported here");

          case RequirementKind::Conformance:
          case RequirementKind::Layout:
          case RequirementKind::Superclass: {
            if (getMemberChainDepth(req.getFirstType()) > 1)
              return true;
            break;
          }

          case RequirementKind::SameType:
            auto lhsTy = req.getFirstType();
            auto rhsTy = req.getSecondType();

            if (getMemberChainDepth(lhsTy) > 1 ||
                getMemberChainDepth(rhsTy) > 1)
              return true;

            break;
          }
        }
      }
      return false;
    }

    /// Check whether the given type is a dependent member type and
    /// is anchored on the given associated type e.g. type is `A.B.C`
    /// and associated type is `A.B`.
    static bool isAnchoredOn(Type type, AssociatedTypeDecl *assocTy,
                             unsigned depth = 0) {
      if (auto *member = type->getAs<DependentMemberType>()) {
        if (member->getAssocType() == assocTy)
          return depth > 0;

        return isAnchoredOn(member->getBase(), assocTy, depth + 1);
      }

      return false;
    }

    static unsigned getMemberChainDepth(Type type, unsigned currDepth = 0) {
      if (auto *memberTy = type->getAs<DependentMemberType>())
        return getMemberChainDepth(memberTy->getBase(), currDepth + 1);
      return currDepth;
    }

    static Type getBaseTypeOfDependentMemberChain(DependentMemberType *member) {
      if (!member->getBase())
        return member;

      auto base = member->getBase();

      if (auto *DMT = base->getAs<DependentMemberType>())
        return getBaseTypeOfDependentMemberChain(DMT);

      return base;
    }
  };

  // First, let's check whether coercion is already there.
  if (auto *anchor = getAsExpr(locator.getAnchor())) {
    // If this is erasure related to `Self`, let's look through
    // the call, if any.
    if (auto *UDE = dyn_cast<UnresolvedDotExpr>(anchor)) {
      // If this is an implicit `makeIterator` call, let's skip the check.
      if (UDE->isImplicit() &&
          cs.getContextualTypePurpose(UDE->getBase()) == CTP_ForEachSequence)
        return false;

      auto parentExpr = cs.getParentExpr(anchor);
      if (parentExpr && isa<CallExpr>(parentExpr))
        anchor = parentExpr;
    }

    auto *parent = cs.getParentExpr(anchor);
    // Support both `as` and `as!` coercions.
    if (parent &&
        (isa<CoerceExpr>(parent) || isa<ForcedCheckedCastExpr>(parent)))
      return false;
  }

  CoercionChecker check(cs, findExistentialType);
  resultTy.walk(check);

  return check.RequiresCoercion;
}

bool AddExplicitExistentialCoercion::isRequired(
    ConstraintSystem &cs, Type resultTy,
    ArrayRef<std::pair<TypeVariableType *, OpenedArchetypeType *>>
        openedExistentials,
    ConstraintLocatorBuilder locator) {
  return isRequired(
      cs, resultTy,
      [&](TypeVariableType *typeVar) -> Optional<Type> {
        auto opened =
            llvm::find_if(openedExistentials, [&typeVar](const auto &entry) {
              return typeVar == entry.first;
            });

        if (opened == openedExistentials.end())
          return None;

        return opened->second->getExistentialType();
      },
      locator);
}

AddExplicitExistentialCoercion *
AddExplicitExistentialCoercion::create(ConstraintSystem &cs, Type resultTy,
                                       ConstraintLocator *locator) {
  FixBehavior fixBehavior = FixBehavior::Error;
  return new (cs.getAllocator())
      AddExplicitExistentialCoercion(cs, resultTy, locator, fixBehavior);
}

bool RenameConflictingPatternVariables::diagnose(const Solution &solution,
                                                 bool asNote) const {
  ConflictingPatternVariables failure(solution, ExpectedType,
                                      getConflictingVars(), getLocator());
  return failure.diagnose(asNote);
}

RenameConflictingPatternVariables *
RenameConflictingPatternVariables::create(ConstraintSystem &cs, Type expectedTy,
                                          ArrayRef<VarDecl *> conflicts,
                                          ConstraintLocator *locator) {
  unsigned size = totalSizeToAlloc<VarDecl *>(conflicts.size());
  void *mem = cs.getAllocator().Allocate(
      size, alignof(RenameConflictingPatternVariables));
  return new (mem)
      RenameConflictingPatternVariables(cs, expectedTy, conflicts, locator);
}

bool MacroMissingPound::diagnose(const Solution &solution,
                                                 bool asNote) const {
  AddMissingMacroPound failure(solution, macro, getLocator());
  return failure.diagnose(asNote);
}

MacroMissingPound *
MacroMissingPound::create(ConstraintSystem &cs, MacroDecl *macro,
                          ConstraintLocator *locator) {
  return new (cs.getAllocator()) MacroMissingPound(cs, macro, locator);
}

bool AllowGlobalActorMismatch::diagnose(const Solution &solution,
                                        bool asNote) const {
  GlobalActorFunctionMismatchFailure failure(solution, getFromType(),
                                             getToType(), getLocator());
  return failure.diagnose(asNote);
}

AllowGlobalActorMismatch *
AllowGlobalActorMismatch::create(ConstraintSystem &cs, Type fromType,
                                 Type toType, ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowGlobalActorMismatch(cs, fromType, toType, locator);
}

bool DestructureTupleToMatchPackExpansionParameter::diagnose(
    const Solution &solution, bool asNote) const {
  DestructureTupleToUseWithPackExpansionParameter failure(solution, ParamShape,
                                                          getLocator());
  return failure.diagnose(asNote);
}

DestructureTupleToMatchPackExpansionParameter *
DestructureTupleToMatchPackExpansionParameter::create(
    ConstraintSystem &cs, PackType *paramShapeTy, ConstraintLocator *locator) {
  return new (cs.getAllocator())
      DestructureTupleToMatchPackExpansionParameter(cs, paramShapeTy, locator);
}

bool AllowValueExpansionWithoutPackReferences::diagnose(
    const Solution &solution, bool asNote) const {
  ValuePackExpansionWithoutPackReferences failure(solution, getLocator());
  return failure.diagnose(asNote);
}

AllowValueExpansionWithoutPackReferences *
AllowValueExpansionWithoutPackReferences::create(ConstraintSystem &cs,
                                                 ConstraintLocator *locator) {
  return new (cs.getAllocator())
      AllowValueExpansionWithoutPackReferences(cs, locator);
}

bool IgnoreMissingEachKeyword::diagnose(const Solution &solution,
                                        bool asNote) const {
  MissingEachForValuePackReference failure(solution, ValuePackType,
                                           getLocator());
  return failure.diagnose(asNote);
}

IgnoreMissingEachKeyword *
IgnoreMissingEachKeyword::create(ConstraintSystem &cs, Type valuePackTy,
                                 ConstraintLocator *locator) {
  return new (cs.getAllocator())
      IgnoreMissingEachKeyword(cs, valuePackTy, locator);
}
