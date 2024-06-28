//===--- DerivedConformanceAdditiveArithmetic.cpp -------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2018 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements explicit derivation of the AdditiveArithmetic protocol
// for struct types.
//
// Currently, this is gated by a frontend flag:
// `-enable-experimental-additive-arithmetic-derivation`.
//
// Swift Evolution pitch thread:
// https://forums.swift.org/t/additivearithmetic-conformance-synthesis-for-structs/26159
//
//===----------------------------------------------------------------------===//

#include "CodeSynthesis.h"
#include "TypeChecker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"
#include "DerivedConformances.h"

using namespace swift;

// Represents synthesizable math operators.
enum MathOperator {
  // `+(Self, Self)`: AdditiveArithmetic
  Add,
  // `-(Self, Self)`: AdditiveArithmetic
  Subtract,
};

static StringRef getMathOperatorName(MathOperator op) {
  switch (op) {
  case Add:
    return "+";
  case Subtract:
    return "-";
  }
  llvm_unreachable("invalid math operator kind");
}

bool DerivedConformance::canDeriveAdditiveArithmetic(NominalTypeDecl *nominal,
                                                     DeclContext *DC) {
  // Experimental `AdditiveArithmetic` derivation must be enabled.
  if (auto *SF = DC->getParentSourceFile())
    if (!isAdditiveArithmeticConformanceDerivationEnabled(*SF))
      return false;
  // Nominal type must be a struct. (No stored properties is okay.)
  auto *structDecl = dyn_cast<StructDecl>(nominal);
  if (!structDecl)
    return false;
  // Must not have any `let` stored properties with an initial value.
  // - This restriction may be lifted later with support for "true" memberwise
  //   initializers that initialize all stored properties, including initial
  //   value information.
  if (hasLetStoredPropertyWithInitialValue(nominal))
    return false;
  // All stored properties must conform to `AdditiveArithmetic`.
  auto &C = nominal->getASTContext();
  auto *proto = C.getProtocol(KnownProtocolKind::AdditiveArithmetic);
  return llvm::all_of(structDecl->getStoredProperties(), [&](VarDecl *v) {
    if (v->getInterfaceType()->hasError())
      return false;
    auto varType = DC->mapTypeIntoContext(v->getValueInterfaceType());
    return (bool) DC->getParentModule()->checkConformance(varType, proto);
  });
}

// Synthesize body for math operator.
static std::pair<BraceStmt *, bool>
deriveBodyMathOperator(AbstractFunctionDecl *funcDecl, MathOperator op) {
  auto *parentDC = funcDecl->getParent();
  auto *nominal = parentDC->getSelfNominalTypeDecl();
  auto &C = nominal->getASTContext();

  // Create memberwise initializer: `Nominal.init(...)`.
  auto *memberwiseInitDecl = nominal->getEffectiveMemberwiseInitializer();
  assert(memberwiseInitDecl && "Memberwise initializer must exist");
  auto *initDRE =
      new (C) DeclRefExpr(memberwiseInitDecl, DeclNameLoc(), /*Implicit*/ true);
  initDRE->setFunctionRefKind(FunctionRefKind::SingleApply);
  auto *nominalTypeExpr = TypeExpr::createImplicitForDecl(
      DeclNameLoc(), nominal, funcDecl,
      funcDecl->mapTypeIntoContext(nominal->getInterfaceType()));
  auto *initExpr = ConstructorRefCallExpr::create(C, initDRE, nominalTypeExpr);

  // Get operator protocol requirement.
  auto *proto = C.getProtocol(KnownProtocolKind::AdditiveArithmetic);
  auto operatorId = C.getIdentifier(getMathOperatorName(op));
  auto *operatorReq = getProtocolRequirement(proto, operatorId);

  // Create reference to operator parameters: lhs and rhs.
  auto params = funcDecl->getParameters();

  // Create expression combining lhs and rhs members using member operator.
  auto createMemberOpExpr = [&](VarDecl *member) -> Expr * {
    auto module = nominal->getModuleContext();
    auto memberType =
        parentDC->mapTypeIntoContext(member->getValueInterfaceType());
    auto confRef = module->lookupConformance(memberType, proto);
    assert(confRef && "Member does not conform to math protocol");

    // Get member type's math operator, e.g. `Member.+`.
    // Use protocol requirement declaration for the operator by default: this
    // will be dynamically dispatched.
    ValueDecl *memberOpDecl = operatorReq;
    // If conformance reference is concrete, then use concrete witness
    // declaration for the operator.
    if (confRef.isConcrete())
      if (auto *concreteMemberMethodDecl =
              confRef.getConcrete()->getWitnessDecl(operatorReq))
        memberOpDecl = concreteMemberMethodDecl;
    assert(memberOpDecl && "Member operator declaration must exist");
    auto *memberTypeExpr = TypeExpr::createImplicit(memberType, C);
    auto memberOpExpr =
        new (C) MemberRefExpr(memberTypeExpr, SourceLoc(), memberOpDecl,
                              DeclNameLoc(), /*Implicit*/ true);

    // Create expression `lhs.member <op> rhs.member`.
    // NOTE(TF-1054): create new `DeclRefExpr`s per loop iteration to avoid
    // `ConstraintSystem::resolveOverload` error.
    auto *lhsDRE =
        new (C) DeclRefExpr(params->get(0), DeclNameLoc(), /*Implicit*/ true);
    auto *rhsDRE =
        new (C) DeclRefExpr(params->get(1), DeclNameLoc(), /*Implicit*/ true);
    Expr *lhsArg = new (C) MemberRefExpr(lhsDRE, SourceLoc(), member,
                                         DeclNameLoc(), /*Implicit*/ true);
    auto *rhsArg = new (C) MemberRefExpr(rhsDRE, SourceLoc(), member,
                                         DeclNameLoc(), /*Implicit*/ true);
    return BinaryExpr::create(C, lhsArg, memberOpExpr, rhsArg,
                              /*implicit*/ true);
  };

  // Create array of member operator call expressions.
  llvm::SmallVector<Argument, 2> memberOpArgs;
  for (auto member : nominal->getStoredProperties()) {
    memberOpArgs.emplace_back(SourceLoc(), member->getName(),
                              createMemberOpExpr(member));
  }
  // Call memberwise initializer with member operator call expressions.
  auto *argList = ArgumentList::createImplicit(C, memberOpArgs);
  auto *callExpr = CallExpr::createImplicit(C, initExpr, argList);
  ASTNode returnStmt = ReturnStmt::createImplicit(C, callExpr);
  return std::pair<BraceStmt *, bool>(
      BraceStmt::create(C, SourceLoc(), returnStmt, SourceLoc(), true), false);
}

// Synthesize function declaration for the given math operator.
static ValueDecl *deriveMathOperator(DerivedConformance &derived,
                                     MathOperator op) {
  auto nominal = derived.Nominal;
  auto parentDC = derived.getConformanceContext();
  auto &C = derived.Context;
  auto selfInterfaceType = parentDC->getDeclaredInterfaceType();

  // Create parameter declaration with the given name and type.
  auto createParamDecl = [&](StringRef name, Type type) -> ParamDecl * {
    auto *param =
        new (C) ParamDecl(SourceLoc(), SourceLoc(), Identifier(), SourceLoc(),
                          C.getIdentifier(name), parentDC);
    param->setSpecifier(ParamDecl::Specifier::Default);
    param->setInterfaceType(type);
    param->setImplicit();
    return param;
  };

  ParameterList *params =
      ParameterList::create(C, {createParamDecl("lhs", selfInterfaceType),
                                createParamDecl("rhs", selfInterfaceType)});

  auto operatorId = C.getIdentifier(getMathOperatorName(op));
  DeclName operatorDeclName(C, operatorId, params);
  auto *const operatorDecl = FuncDecl::createImplicit(
      C, StaticSpellingKind::KeywordStatic, operatorDeclName,
      /*NameLoc=*/SourceLoc(),
      /*Async=*/false,
      /*Throws=*/false,
      /*ThrownType=*/Type(),
      /*GenericParams=*/nullptr, params, selfInterfaceType, parentDC);
  auto bodySynthesizer = [](AbstractFunctionDecl *funcDecl,
                            void *ctx) -> std::pair<BraceStmt *, bool> {
    auto op = (MathOperator) reinterpret_cast<intptr_t>(ctx);
    return deriveBodyMathOperator(funcDecl, op);
  };
  operatorDecl->setBodySynthesizer(bodySynthesizer, (void *)op);
  operatorDecl->setGenericSignature(parentDC->getGenericSignatureOfContext());
  operatorDecl->copyFormalAccessFrom(nominal, /*sourceIsParentContext*/ true);

  derived.addMembersToConformanceContext({operatorDecl});

  // For the effective memberwise initializer before we force the body,
  // so that it becomes part of the emitted ABI members even if we don't
  // emit the body.
  (void) nominal->getEffectiveMemberwiseInitializer();

  return operatorDecl;
}

// Synthesize body for a property computed property getter.
static std::pair<BraceStmt *, bool>
deriveBodyPropertyGetter(AbstractFunctionDecl *funcDecl, ProtocolDecl *proto,
                         ValueDecl *reqDecl) {
  auto *parentDC = funcDecl->getParent();
  auto *nominal = parentDC->getSelfNominalTypeDecl();
  auto &C = nominal->getASTContext();

  auto *memberwiseInitDecl = nominal->getEffectiveMemberwiseInitializer();
  assert(memberwiseInitDecl && "Memberwise initializer must exist");
  auto *initDRE =
      new (C) DeclRefExpr(memberwiseInitDecl, DeclNameLoc(), /*Implicit*/ true);
  initDRE->setFunctionRefKind(FunctionRefKind::SingleApply);

  auto *nominalTypeExpr = TypeExpr::createImplicitForDecl(
      DeclNameLoc(), nominal, funcDecl,
      funcDecl->mapTypeIntoContext(nominal->getInterfaceType()));
  auto *initExpr = ConstructorRefCallExpr::create(C, initDRE, nominalTypeExpr);

  auto createMemberPropertyExpr = [&](VarDecl *member) -> Expr * {
    auto memberType =
        parentDC->mapTypeIntoContext(member->getValueInterfaceType());
    Expr *memberExpr = nullptr;
    // If the property is static, create a type expression: `Member`.
    if (reqDecl->isStatic()) {
      memberExpr = TypeExpr::createImplicit(memberType, C);
    }
    // If the property is not static, create a member ref expression:
    // `self.member`.
    else {
      auto *selfDecl = funcDecl->getImplicitSelfDecl();
      auto *selfDRE =
          new (C) DeclRefExpr(selfDecl, DeclNameLoc(), /*Implicit*/ true);
      memberExpr =
          new (C) MemberRefExpr(selfDRE, SourceLoc(), member, DeclNameLoc(),
                                /*Implicit*/ true);
    }
    auto *module = nominal->getModuleContext();
    auto confRef = module->lookupConformance(memberType, proto);
    assert(confRef && "Member does not conform to `AdditiveArithmetic`");
    // If conformance reference is not concrete, then concrete witness
    // declaration for property cannot be resolved. Return reference to
    // protocol requirement: this will be dynamically dispatched.
    if (!confRef.isConcrete()) {
      return new (C) MemberRefExpr(memberExpr, SourceLoc(), reqDecl,
                                   DeclNameLoc(), /*Implicit*/ true);
    }
    // Otherwise, return reference to concrete witness declaration.
    auto conf = confRef.getConcrete();
    auto *witnessDecl = conf->getWitnessDecl(reqDecl);
    return new (C) MemberRefExpr(memberExpr, SourceLoc(), witnessDecl,
                                 DeclNameLoc(), /*Implicit*/ true);
  };

  // Create array of `member.<property>` expressions.
  llvm::SmallVector<Argument, 2> args;
  for (auto member : nominal->getStoredProperties()) {
    args.emplace_back(SourceLoc(), member->getName(),
                      createMemberPropertyExpr(member));
  }
  // Call memberwise initializer with member property expressions.
  auto *callExpr = CallExpr::createImplicit(
      C, initExpr, ArgumentList::createImplicit(C, args));
  ASTNode returnStmt = ReturnStmt::createImplicit(C, callExpr);
  auto *braceStmt =
      BraceStmt::create(C, SourceLoc(), returnStmt, SourceLoc(), true);
  return std::pair<BraceStmt *, bool>(braceStmt, false);
}

// Synthesize body for the `AdditiveArithmetic.zero` computed property getter.
static std::pair<BraceStmt *, bool>
deriveBodyAdditiveArithmetic_zero(AbstractFunctionDecl *funcDecl, void *) {
  auto &C = funcDecl->getASTContext();
  auto *addArithProto = C.getProtocol(KnownProtocolKind::AdditiveArithmetic);
  auto *zeroReq = getProtocolRequirement(addArithProto, C.Id_zero);
  return deriveBodyPropertyGetter(funcDecl, addArithProto, zeroReq);
}

// Synthesize the static property declaration for `AdditiveArithmetic.zero`.
static ValueDecl *deriveAdditiveArithmetic_zero(DerivedConformance &derived) {
  auto &C = derived.Context;
  auto *nominal = derived.Nominal;
  auto *parentDC = derived.getConformanceContext();

  auto returnInterfaceTy = nominal->getDeclaredInterfaceType();
  auto returnTy = parentDC->mapTypeIntoContext(returnInterfaceTy);

  // Create property declaration.
  VarDecl *propDecl;
  PatternBindingDecl *pbDecl;
  std::tie(propDecl, pbDecl) = derived.declareDerivedProperty(
      DerivedConformance::SynthesizedIntroducer::Var, C.Id_zero,
      returnInterfaceTy, returnTy, /*isStatic*/ true,
      /*isFinal*/ true);

  // Create property getter.
  auto *getterDecl =
      derived.addGetterToReadOnlyDerivedProperty(propDecl, returnTy);
  getterDecl->setBodySynthesizer(deriveBodyAdditiveArithmetic_zero, nullptr);

  derived.addMembersToConformanceContext({propDecl, pbDecl});

  // For the effective memberwise initializer before we force the body,
  // so that it becomes part of the emitted ABI members even if we don't
  // emit the body.
  (void) nominal->getEffectiveMemberwiseInitializer();

  return propDecl;
}

ValueDecl *
DerivedConformance::deriveAdditiveArithmetic(ValueDecl *requirement) {
  // Diagnose conformances in disallowed contexts.
  if (checkAndDiagnoseDisallowedContext(requirement))
    return nullptr;
  if (requirement->getBaseName() == Context.getIdentifier("+"))
    return deriveMathOperator(*this, Add);
  if (requirement->getBaseName() == Context.getIdentifier("-"))
    return deriveMathOperator(*this, Subtract);
  if (requirement->getBaseName() == Context.Id_zero)
    return deriveAdditiveArithmetic_zero(*this);
  Context.Diags.diagnose(requirement->getLoc(),
                         diag::broken_additive_arithmetic_requirement);
  return nullptr;
}
