//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements implicit derivation of the CaseIterable protocol.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Types.h"
#include "llvm/Support/raw_ostream.h"
#include "DerivedConformances.h"

using namespace swift;

/// Common preconditions for CaseIterable.
static bool canDeriveConformance(NominalTypeDecl *type) {
  // The type must be an enum.
  auto enumDecl = dyn_cast<EnumDecl>(type);
  if (!enumDecl)
    return false;

  // "Simple" enums without availability attributes can derive
  // a CaseIterable conformance.
  //
  // FIXME: Lift the availability restriction.
  return !enumDecl->hasPotentiallyUnavailableCaseValue()
      && enumDecl->hasOnlyCasesWithoutAssociatedValues();
}

/// Derive the implementation of allCases for a "simple" no-payload enum.
std::pair<BraceStmt *, bool>
deriveCaseIterable_enum_getter(AbstractFunctionDecl *funcDecl, void *) {
  auto *parentDC = funcDecl->getDeclContext();
  auto *parentEnum = parentDC->getSelfEnumDecl();
  auto enumTy = parentDC->getDeclaredTypeInContext();
  auto &C = parentDC->getASTContext();

  SmallVector<Expr *, 8> elExprs;
  for (EnumElementDecl *elt : parentEnum->getAllElements()) {
    auto *base = TypeExpr::createImplicit(enumTy, C);
    auto *apply = new (C) MemberRefExpr(base, SourceLoc(),
                                        elt, DeclNameLoc(), /*implicit*/true);
    elExprs.push_back(apply);
  }
  auto *arrayExpr = ArrayExpr::create(C, SourceLoc(), elExprs, {}, SourceLoc());

  auto *returnStmt = ReturnStmt::createImplicit(C, arrayExpr);
  auto *body = BraceStmt::create(C, SourceLoc(), ASTNode(returnStmt),
                                 SourceLoc());
  return { body, /*isTypeChecked=*/false };
}

static ArraySliceType *computeAllCasesType(NominalTypeDecl *enumDecl) {
  auto enumType = enumDecl->getDeclaredInterfaceType();
  if (!enumType || enumType->hasError())
    return nullptr;

  return ArraySliceType::get(enumType);
}

static Type deriveCaseIterable_AllCases(DerivedConformance &derived) {
  // enum SomeEnum : CaseIterable {
  //   @derived
  //   typealias AllCases = [SomeEnum]
  // }
  auto *rawInterfaceType = computeAllCasesType(cast<EnumDecl>(derived.Nominal));
  return derived.getConformanceContext()->mapTypeIntoContext(rawInterfaceType);
}

ValueDecl *DerivedConformance::deriveCaseIterable(ValueDecl *requirement) {
  auto &C = requirement->getASTContext();

  // Conformance can't be synthesized in an extension.
  if (checkAndDiagnoseDisallowedContext(requirement))
    return nullptr;

  // Check that we can actually derive CaseIterable for this type.
  if (!canDeriveConformance(Nominal))
    return nullptr;

  // Build the necessary decl.
  if (requirement->getBaseName() != Context.Id_allCases) {
    requirement->diagnose(diag::broken_case_iterable_requirement);
    return nullptr;
  }

  // Define the property.
  auto *returnTy = computeAllCasesType(Nominal);

  VarDecl *propDecl;
  PatternBindingDecl *pbDecl;
  std::tie(propDecl, pbDecl) = declareDerivedProperty(
      SynthesizedIntroducer::Var, Context.Id_allCases, returnTy, returnTy,
      /*isStatic=*/true, /*isFinal=*/true);

  propDecl->getAttrs().add(
              new (C) NonisolatedAttr(/*unsafe=*/false, /*implicit=*/true));

  // Define the getter.
  auto *getterDecl = addGetterToReadOnlyDerivedProperty(propDecl, returnTy);

  getterDecl->setBodySynthesizer(&deriveCaseIterable_enum_getter);

  addMembersToConformanceContext({propDecl, pbDecl});

  return propDecl;
}

Type DerivedConformance::deriveCaseIterable(AssociatedTypeDecl *assocType) {
  // Check that we can actually derive CaseIterable for this type.
  if (!canDeriveConformance(Nominal))
    return nullptr;

  if (assocType->getName() == Context.Id_AllCases) {
    return deriveCaseIterable_AllCases(*this);
  }

  Context.Diags.diagnose(assocType->getLoc(),
                         diag::broken_case_iterable_requirement);
  return nullptr;
}

