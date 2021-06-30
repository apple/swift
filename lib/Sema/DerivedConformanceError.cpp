//===--- DerivedConformanceError.cpp - Derived Error ----------------------===//
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
//  This file implements implicit derivation of the Error
//  protocol.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "DerivedConformances.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/Types.h"
#include "swift/AST/SwiftNameTranslation.h"

using namespace swift;
using namespace swift::objc_translation;

static std::pair<BraceStmt *, bool>
deriveBodyBridgedNSError_enum_nsErrorDomain(AbstractFunctionDecl *domainDecl,
                                            void *) {
  // enum SomeEnum {
  //   @derived
  //   static var _nsErrorDomain: String {
  //     return String(reflecting: self)
  //   }
  // }

  auto M = domainDecl->getParentModule();
  auto &C = M->getASTContext();
  auto self = domainDecl->getImplicitSelfDecl();

  auto selfRef = new (C) DeclRefExpr(self, DeclNameLoc(), /*implicit*/ true);
  auto stringType = TypeExpr::createImplicitForDecl(
      DeclNameLoc(), C.getStringDecl(), domainDecl,
      C.getStringDecl()->getInterfaceType());
  auto *argList = ArgumentList::forImplicitSingle(
      C, C.getIdentifier("reflecting"), selfRef);
  auto *initReflectingCall = CallExpr::createImplicit(C, stringType, argList);
  auto ret =
    new (C) ReturnStmt(SourceLoc(), initReflectingCall, /*implicit*/ true);

  auto body = BraceStmt::create(C, SourceLoc(), ASTNode(ret), SourceLoc());
  return { body, /*isTypeChecked=*/false };
}

static std::pair<BraceStmt *, bool>
deriveBodyBridgedNSError_printAsObjCEnum_nsErrorDomain(
                    AbstractFunctionDecl *domainDecl, void *) {
  // enum SomeEnum {
  //   @derived
  //   static var _nsErrorDomain: String {
  //     return "ModuleName.SomeEnum"
  //   }
  // }

  auto M = domainDecl->getParentModule();
  auto &C = M->getASTContext();
  auto TC = domainDecl->getInnermostTypeContext();
  auto ED = TC->getSelfEnumDecl();

  StringRef value(C.AllocateCopy(getErrorDomainStringForObjC(ED)));

  auto string = new (C) StringLiteralExpr(value, SourceRange(), /*implicit*/ true);
  auto ret = new (C) ReturnStmt(SourceLoc(), string, /*implicit*/ true);
  auto body = BraceStmt::create(C, SourceLoc(),
                                ASTNode(ret),
                                SourceLoc());
  return { body, /*isTypeChecked=*/false };
}

static ValueDecl *
deriveBridgedNSError_enum_nsErrorDomain(
    DerivedConformance &derived,
    std::pair<BraceStmt *, bool> (*synthesizer)(AbstractFunctionDecl *, void*)) {
  // enum SomeEnum {
  //   @derived
  //   static var _nsErrorDomain: String {
  //     ...
  //   }
  // }

  auto stringTy = derived.Context.getStringType();

  // Define the property.
  VarDecl *propDecl;
  PatternBindingDecl *pbDecl;
  std::tie(propDecl, pbDecl) = derived.declareDerivedProperty(
      derived.Context.Id_nsErrorDomain, stringTy, stringTy, /*isStatic=*/true,
      /*isFinal=*/true);

  // Define the getter.
  auto getterDecl = derived.addGetterToReadOnlyDerivedProperty(
      propDecl, stringTy);
  getterDecl->setBodySynthesizer(synthesizer);

  derived.addMembersToConformanceContext({propDecl, pbDecl});

  return propDecl;
}

ValueDecl *DerivedConformance::deriveBridgedNSError(ValueDecl *requirement) {
  if (!isa<EnumDecl>(Nominal))
    return nullptr;

  if (requirement->getBaseName() == Context.Id_nsErrorDomain) {
    auto synthesizer = deriveBodyBridgedNSError_enum_nsErrorDomain;

    auto scope = Nominal->getFormalAccessScope(Nominal->getModuleScopeContext());
    if (scope.isPublic() || scope.isInternal())
      // PrintAsObjC may print this domain, so we should make sure we use the
      // same string it will.
      synthesizer = deriveBodyBridgedNSError_printAsObjCEnum_nsErrorDomain;

    return deriveBridgedNSError_enum_nsErrorDomain(*this, synthesizer);
  }

  Context.Diags.diagnose(requirement->getLoc(),
                         diag::broken_errortype_requirement);
  return nullptr;
}
