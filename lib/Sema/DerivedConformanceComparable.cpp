//===--- DerivedConformanceComparable.cpp - Derived Comparable -===//
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
//
//  This file implements implicit derivation of the Comparable protocol.
//  (Most of this code is similar to code in `DerivedConformanceEquatableHashable.cpp`)
//
//===----------------------------------------------------------------------===//

#include "CodeSynthesis.h"
#include "TypeChecker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include "DerivedConformances.h"

using namespace swift;

static std::pair<BraceStmt *, bool>
deriveBodyComparable_enum_uninhabited_lt(AbstractFunctionDecl *ltDecl, void *) {
  auto parentDC = ltDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto args = ltDecl->getParameters();
  auto aParam = args->get(0);
  auto bParam = args->get(1);

  assert(!cast<EnumDecl>(aParam->getType()->getAnyNominal())->hasCases());
  assert(!cast<EnumDecl>(bParam->getType()->getAnyNominal())->hasCases());

  SmallVector<ASTNode, 0> statements;
  auto body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc());
  return { body, /*isTypeChecked=*/true };
}

/// Derive the body for a '<' operator for an enum that has no associated
/// values. This generates code that converts each value to its integer ordinal
/// and compares them, which produces an optimal single icmp instruction.
static std::pair<BraceStmt *, bool>
deriveBodyComparable_enum_noAssociatedValues_lt(AbstractFunctionDecl *ltDecl,
                                               void *) { 
  auto parentDC = ltDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto args = ltDecl->getParameters();
  auto aParam = args->get(0);
  auto bParam = args->get(1);

  auto enumDecl = cast<EnumDecl>(aParam->getType()->getAnyNominal());

  // Generate the conversion from the enums to integer indices.
  SmallVector<ASTNode, 8> statements;
  DeclRefExpr *aIndex = DerivedConformance::convertEnumToIndex(statements, parentDC, enumDecl,
                                           aParam, ltDecl, "index_a");
  DeclRefExpr *bIndex = DerivedConformance::convertEnumToIndex(statements, parentDC, enumDecl,
                                           bParam, ltDecl, "index_b");

  // Generate the compare of the indices.
  FuncDecl *cmpFunc = C.getLessThanIntDecl();
  assert(cmpFunc && "should have a < for int as we already checked for it");

  Expr *cmpFuncExpr = new (C) DeclRefExpr(cmpFunc, DeclNameLoc(),
                                          /*implicit*/ true,
                                          AccessSemantics::Ordinary);

  auto *cmpExpr =
      BinaryExpr::create(C, aIndex, cmpFuncExpr, bIndex, /*implicit*/ true);
  statements.push_back(new (C) ReturnStmt(SourceLoc(), cmpExpr));

  BraceStmt *body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc());
  return { body, /*isTypeChecked=*/false };
}

/// Derive the body for an '==' operator for an enum where at least one of the
/// cases has associated values.
static std::pair<BraceStmt *, bool>
deriveBodyComparable_enum_hasAssociatedValues_lt(AbstractFunctionDecl *ltDecl, void *) {
  auto parentDC = ltDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto args = ltDecl->getParameters();
  auto aParam = args->get(0);
  auto bParam = args->get(1);

  Type enumType = aParam->getType();
  auto enumDecl = cast<EnumDecl>(aParam->getType()->getAnyNominal());

  SmallVector<ASTNode, 8> statements;
  SmallVector<ASTNode, 4> cases;
  unsigned elementCount = 0; // need this as `getAllElements` returns a generator

  // For each enum element, generate a case statement matching a pair containing
  // the same case, binding variables for the left- and right-hand associated
  // values.
  for (auto elt : enumDecl->getAllElements()) {
    ++elementCount;

    // .<elt>(let l0, let l1, ...)
    SmallVector<VarDecl*, 4> lhsPayloadVars;
    auto lhsSubpattern = DerivedConformance::enumElementPayloadSubpattern(elt, 'l', ltDecl,
                                                      lhsPayloadVars);
    auto *lhsBaseTE = TypeExpr::createImplicit(enumType, C);
    auto lhsElemPat = new (C)
        EnumElementPattern(lhsBaseTE, SourceLoc(), DeclNameLoc(), DeclNameRef(),
                           elt, lhsSubpattern, /*DC*/ ltDecl);
    lhsElemPat->setImplicit();

    // .<elt>(let r0, let r1, ...)
    SmallVector<VarDecl*, 4> rhsPayloadVars;
    auto rhsSubpattern = DerivedConformance::enumElementPayloadSubpattern(elt, 'r', ltDecl,
                                                      rhsPayloadVars);
    auto *rhsBaseTE = TypeExpr::createImplicit(enumType, C);
    auto rhsElemPat = new (C)
        EnumElementPattern(rhsBaseTE, SourceLoc(), DeclNameLoc(), DeclNameRef(),
                           elt, rhsSubpattern, /*DC*/ ltDecl);
    rhsElemPat->setImplicit();

    auto hasBoundDecls = !lhsPayloadVars.empty();
    Optional<MutableArrayRef<VarDecl *>> caseBodyVarDecls;
    if (hasBoundDecls) {
      // We allocated a direct copy of our lhs var decls for the case
      // body.
      auto copy = C.Allocate<VarDecl *>(lhsPayloadVars.size());
      for (unsigned i : indices(lhsPayloadVars)) {
        auto *vOld = lhsPayloadVars[i];
        auto *vNew = new (C) VarDecl(
            /*IsStatic*/ false, vOld->getIntroducer(),
            vOld->getNameLoc(), vOld->getName(), vOld->getDeclContext());
        vNew->setImplicit();
        copy[i] = vNew;
      }
      caseBodyVarDecls.emplace(copy);
    }

    // case (.<elt>(let l0, let l1, ...), .<elt>(let r0, let r1, ...))
    auto caseTuplePattern = TuplePattern::createImplicit(C, {
      TuplePatternElt(lhsElemPat), TuplePatternElt(rhsElemPat) });
    caseTuplePattern->setImplicit();

    auto labelItem = CaseLabelItem(caseTuplePattern);

    // Generate a guard statement for each associated value in the payload,
    // breaking out early if any pair is unequal. (same as Equatable synthesis.)
    // the else statement performs the lexicographic comparison.
    SmallVector<ASTNode, 8> statementsInCase;
    for (size_t varIdx = 0; varIdx < lhsPayloadVars.size(); ++varIdx) {
      auto lhsVar = lhsPayloadVars[varIdx];
      auto lhsExpr = new (C) DeclRefExpr(lhsVar, DeclNameLoc(),
                                         /*implicit*/true);
      auto rhsVar = rhsPayloadVars[varIdx];
      auto rhsExpr = new (C) DeclRefExpr(rhsVar, DeclNameLoc(),
                                         /*Implicit*/true);
      auto guardStmt = DerivedConformance::returnComparisonIfNotEqualGuard(C, 
          lhsExpr, rhsExpr);
      statementsInCase.emplace_back(guardStmt);
    }

    // If none of the guard statements caused an early exit, then all the pairs
    // were true. (equal)
    // return false 
    auto falseExpr = new (C) BooleanLiteralExpr(false, SourceLoc(),
                                               /*Implicit*/true);
    auto returnStmt = new (C) ReturnStmt(SourceLoc(), falseExpr);
    statementsInCase.push_back(returnStmt);

    auto body = BraceStmt::create(C, SourceLoc(), statementsInCase,
                                  SourceLoc());
    cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                     labelItem, SourceLoc(), SourceLoc(), body,
                                     caseBodyVarDecls));
  }

  // default: result = <enum index>(lhs) < <enum index>(rhs)
  //
  // We only generate this if the enum has more than one case. If it has exactly
  // one case, then that single case statement is already exhaustive.
  if (elementCount > 1) {
    auto defaultPattern = AnyPattern::createImplicit(C);
    auto defaultItem = CaseLabelItem::getDefault(defaultPattern);
    auto body = deriveBodyComparable_enum_noAssociatedValues_lt(ltDecl, nullptr).first;
    cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                     defaultItem, SourceLoc(), SourceLoc(),
                                     body,
                                     /*case body var decls*/ None));
  }

  // switch (a, b) { <case statements> }
  auto aRef = new (C) DeclRefExpr(aParam, DeclNameLoc(), /*implicit*/true);
  auto bRef = new (C) DeclRefExpr(bParam, DeclNameLoc(), /*implicit*/true);
  auto abExpr = TupleExpr::createImplicit(C, {aRef, bRef}, /*labels*/ {});
  auto switchStmt =
      SwitchStmt::createImplicit(LabeledStmtInfo(), abExpr, cases, C);
  statements.push_back(switchStmt);

  auto body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc());
  return { body, /*isTypeChecked=*/false };
}

/// Derive an '<' operator implementation for an enum.
static ValueDecl *
deriveComparable_lt(
    DerivedConformance &derived,
    std::pair<BraceStmt *, bool> (*bodySynthesizer)(AbstractFunctionDecl *,
                                                    void *)) {
  ASTContext &C = derived.Context;

  auto parentDC = derived.getConformanceContext();
  auto selfIfaceTy = parentDC->getDeclaredInterfaceType();

  auto getParamDecl = [&](StringRef s) -> ParamDecl * {
    auto *param = new (C) ParamDecl(SourceLoc(),
                                    SourceLoc(), Identifier(), SourceLoc(),
                                    C.getIdentifier(s), parentDC);
    param->setSpecifier(ParamSpecifier::Default);
    param->setInterfaceType(selfIfaceTy);
    param->setImplicit();
    return param;
  };

  ParameterList *params = ParameterList::create(C, {
    getParamDecl("a"),
    getParamDecl("b")
  });

  auto boolTy = C.getBoolType();

  Identifier generatedIdentifier;
  if (parentDC->getParentModule()->isResilient()) {
    generatedIdentifier = C.Id_LessThanOperator;
  } else {
    assert(selfIfaceTy->getEnumOrBoundGenericEnum());
    generatedIdentifier = C.Id_derived_enum_less_than;
  }

  DeclName name(C, generatedIdentifier, params);
  auto *const comparableDecl = FuncDecl::createImplicit(
      C, StaticSpellingKind::KeywordStatic, name, /*NameLoc=*/SourceLoc(),
      /*Async=*/false,
      /*Throws=*/false,
      /*GenericParams=*/nullptr, params, boolTy, parentDC);
  comparableDecl->setUserAccessible(false);

  // Add the @_implements(Comparable, < (_:_:)) attribute
  if (generatedIdentifier != C.Id_LessThanOperator) {
    auto comparable = C.getProtocol(KnownProtocolKind::Comparable);
    SmallVector<Identifier, 2> argumentLabels = { Identifier(), Identifier() };
    auto comparableDeclName = DeclName(C, DeclBaseName(C.Id_LessThanOperator),
                                   argumentLabels);
    comparableDecl->getAttrs().add(ImplementsAttr::create(parentDC,
                                                          comparable,
                                                          comparableDeclName));
  }

  if (!C.getLessThanIntDecl()) {
    derived.ConformanceDecl->diagnose(diag::no_less_than_overload_for_int);
    return nullptr;
  }

  addNonIsolatedToSynthesized(derived.Nominal, comparableDecl);

  comparableDecl->setBodySynthesizer(bodySynthesizer);

  comparableDecl->copyFormalAccessFrom(derived.Nominal, /*sourceIsParentContext*/ true);

  // Add the operator to the parent scope.
  derived.addMembersToConformanceContext({comparableDecl});

  return comparableDecl;
}

// for now, only enums can synthesize `Comparable`, so this function can take 
// an `EnumDecl` instead of a `NominalTypeDecl`
bool 
DerivedConformance::canDeriveComparable(DeclContext *context, EnumDecl *enumeration) {
  // The type must be an enum.
  if (!enumeration) {
      return false;
  }
  auto comparable = context->getASTContext().getProtocol(KnownProtocolKind::Comparable);
  if (!comparable) {
      return false; // not sure what should be done here instead
  }
  // The cases must not have non-comparable associated values or raw backing
  return allAssociatedValuesConformToProtocol(context, enumeration, comparable) && !enumeration->hasRawType();
}

ValueDecl *DerivedConformance::deriveComparable(ValueDecl *requirement) {
  if (checkAndDiagnoseDisallowedContext(requirement)) {
    return nullptr;
  }
  if (requirement->getBaseName() != "<") {
    requirement->diagnose(diag::broken_comparable_requirement);
    return nullptr;
  }
  
  // Build the necessary decl.
  auto enumeration = dyn_cast<EnumDecl>(this->Nominal);
  assert(enumeration);
  
  std::pair<BraceStmt *, bool> (*synthesizer)(AbstractFunctionDecl *, void *);
  if (enumeration->hasCases()) {
    if (enumeration->hasOnlyCasesWithoutAssociatedValues()) {
      synthesizer = &deriveBodyComparable_enum_noAssociatedValues_lt;
    } else {
      synthesizer = &deriveBodyComparable_enum_hasAssociatedValues_lt;
    }
  } else {
    synthesizer = &deriveBodyComparable_enum_uninhabited_lt;
  }
  return deriveComparable_lt(*this, synthesizer);
}

void DerivedConformance::tryDiagnoseFailedComparableDerivation(
    DeclContext *DC, NominalTypeDecl *nominal) {
  auto &ctx = DC->getASTContext();
  auto *comparableProto = ctx.getProtocol(KnownProtocolKind::Comparable);
  diagnoseAnyNonConformingMemberTypes(DC, nominal, comparableProto);
  diagnoseIfSynthesisUnsupportedForDecl(nominal, comparableProto);

  if (auto enumDecl = dyn_cast<EnumDecl>(nominal)) {
    if (enumDecl->hasRawType() && !enumDecl->getRawType()->is<ErrorType>()) {
      auto rawType = enumDecl->getRawType();
      auto rawTypeLoc = enumDecl->getInherited()[0].getSourceRange().Start;
      ctx.Diags.diagnose(rawTypeLoc,
                         diag::comparable_synthesis_raw_value_not_allowed,
                         rawType, nominal->getDeclaredInterfaceType(),
                         comparableProto->getDeclaredInterfaceType());
    }
  }
}
