//===--- DerivedConformanceCodingKey.cpp - Derived CodingKey --------------===//
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
// This file implements explicit derivation of the CodingKey protocol for an
// enum.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "DerivedConformances.h"

using namespace swift;

/// Sets the body of the given function to `return nil`.
///
/// \param funcDecl The function whose body to set.
static std::pair<BraceStmt *, bool>
deriveNilReturn(AbstractFunctionDecl *funcDecl, void *) {
  auto *parentDC = funcDecl->getDeclContext();
  auto &C = parentDC->getASTContext();

  auto *nilExpr = new (C) NilLiteralExpr(SourceLoc(), /*Implicit=*/true);
  auto *returnStmt = new (C) ReturnStmt(SourceLoc(), nilExpr);
  auto *body = BraceStmt::create(C, SourceLoc(), ASTNode(returnStmt),
                                 SourceLoc());
  return { body, /*isTypeChecked=*/false };
}

/// Sets the body of the given function to `return self.rawValue`.
///
/// \param funcDecl The function whose body to set.
static std::pair<BraceStmt *, bool>
deriveRawValueReturn(AbstractFunctionDecl *funcDecl, void *) {
  auto *parentDC = funcDecl->getDeclContext();
  auto &C = parentDC->getASTContext();

  auto *selfRef = DerivedConformance::createSelfDeclRef(funcDecl);
  auto *memberRef =
      UnresolvedDotExpr::createImplicit(C, selfRef, C.Id_rawValue);

  auto *returnStmt = new (C) ReturnStmt(SourceLoc(), memberRef);
  auto *body = BraceStmt::create(C, SourceLoc(), ASTNode(returnStmt),
                                 SourceLoc());
  return { body, /*isTypeChecked=*/false };
}

/// Sets the body of the given function to `self.init(rawValue:)`, passing along
/// the parameter of the given constructor.
///
/// \param initDecl The constructor whose body to set.
static std::pair<BraceStmt *, bool>
deriveRawValueInit(AbstractFunctionDecl *initDecl, void *) {
  auto *parentDC = initDecl->getDeclContext();
  auto &C = parentDC->getASTContext();

  // Get the param from init({string,int}Value:). self is the first param in the
  // list; stringValue is the second.
  auto *valueParam = initDecl->getParameters()->get(0);
  auto *valueParamExpr = new (C) DeclRefExpr(ConcreteDeclRef(valueParam),
                                             DeclNameLoc(), /*Implicit=*/true);

  // rawValue param to init(rawValue:)
  auto *rawValueDecl = new (C) ParamDecl(
      SourceLoc(), SourceLoc(), C.Id_rawValue,
      SourceLoc(), C.Id_rawValue, parentDC);
  rawValueDecl->setInterfaceType(C.getIntType());
  rawValueDecl->setSpecifier(ParamSpecifier::Default);
  rawValueDecl->setImplicit();
  auto *paramList = ParameterList::createWithoutLoc(rawValueDecl);

  // self.init(rawValue:) expr
  auto *selfRef = DerivedConformance::createSelfDeclRef(initDecl);
  auto *initExpr = UnresolvedDotExpr::createImplicit(
      C, selfRef, DeclBaseName::createConstructor(), paramList);

  // Bind the value param in self.init(rawValue: {string,int}Value).
  auto *argList =
      ArgumentList::forImplicitSingle(C, C.Id_rawValue, valueParamExpr);
  auto *callExpr = CallExpr::createImplicit(C, initExpr, argList);

  auto *body = BraceStmt::create(C, SourceLoc(), ASTNode(callExpr),
                                 SourceLoc());
  return { body, /*isTypeChecked=*/false };
}

/// Synthesizes a constructor declaration with the given parameter name and
/// type.
///
/// \param paramType The type of the parameter.
///
/// \param paramName The name of the parameter.
///
/// \param synthesizer A lambda to call to set the constructor's body.
template <typename Synthesizer>
static ValueDecl *deriveInitDecl(DerivedConformance &derived, Type paramType,
                                 Identifier paramName,
                                 const Synthesizer &synthesizer) {
  auto &C = derived.Context;
  auto *parentDC = derived.getConformanceContext();

  // rawValue
  auto *rawDecl =
      new (C) ParamDecl(SourceLoc(), SourceLoc(),
                        paramName, SourceLoc(), paramName, parentDC);
  rawDecl->setSpecifier(ParamSpecifier::Default);
  rawDecl->setInterfaceType(paramType);
  rawDecl->setImplicit();

  // init(rawValue:) name
  auto *paramList = ParameterList::createWithoutLoc(rawDecl);
  DeclName name(C, DeclBaseName::createConstructor(), paramList);

  // init(rawValue:) decl
  auto *initDecl =
    new (C) ConstructorDecl(name, SourceLoc(),
                            /*Failable=*/true, /*FailabilityLoc=*/SourceLoc(),
                            /*Async=*/false, /*AsyncLoc=*/SourceLoc(),
                            /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(),
                            paramList,
                            /*GenericParams=*/nullptr, parentDC);

  initDecl->setImplicit();

  // Synthesize the body.
  synthesizer(initDecl);

  initDecl->setAccess(derived.Nominal->getFormalAccess());

  derived.addMembersToConformanceContext({initDecl});

  return initDecl;
}

/// Synthesizes a read-only computed property with a given type and name.
///
/// \param type The type of the property.
///
/// \param name The name of the property.
///
/// \param synthesizer A lambda to call to set the property's getter.
template <typename Synthesizer>
static ValueDecl *deriveProperty(DerivedConformance &derived, Type type,
                                 Identifier name,
                                 const Synthesizer &synthesizer) {
  // Define the property.
  VarDecl *propDecl;
  PatternBindingDecl *pbDecl;
  std::tie(propDecl, pbDecl) = derived.declareDerivedProperty(
      DerivedConformance::SynthesizedIntroducer::Var, name, type, type,
      /*isStatic=*/false, /*isFinal=*/false);

  // Define the getter.
  auto *getterDecl = derived.addGetterToReadOnlyDerivedProperty(
      propDecl, type);

  // Synthesize the body.
  synthesizer(getterDecl);

  derived.addMembersToConformanceContext({propDecl, pbDecl});
  return propDecl;
}

/// Sets the body of the given function to return a string value based on
/// switching on `self`.
///
/// \param strValDecl The function whose body to set.
static std::pair<BraceStmt *, bool>
deriveBodyCodingKey_enum_stringValue(AbstractFunctionDecl *strValDecl, void *) {
  // enum SomeEnum {
  //   case A, B, C
  //   @derived var stringValue: String {
  //     switch self {
  //       case A:
  //         return "A"
  //       case B:
  //         return "B"
  //       case C:
  //         return "C"
  //     }
  //   }
  // }
  auto *parentDC = strValDecl->getDeclContext();
  auto &C = parentDC->getASTContext();

  auto *enumDecl = parentDC->getSelfEnumDecl();
  Type enumType = parentDC->getDeclaredTypeInContext();

  BraceStmt *body = nullptr;
  auto elements = enumDecl->getAllElements();
  if (elements.empty() /* empty enum */) {
    // return ""
    auto *emptyStringExpr = new (C) StringLiteralExpr("", SourceRange(),
                                                      /*Implicit=*/true);
    auto *returnStmt = new (C) ReturnStmt(SourceLoc(), emptyStringExpr);
    body = BraceStmt::create(C, SourceLoc(), ASTNode(returnStmt),
                             SourceLoc());
  } else {
    SmallVector<ASTNode, 4> cases;
    for (auto *elt : elements) {
      auto *baseTE = TypeExpr::createImplicit(enumType, C);
      auto *pat = new (C) EnumElementPattern(baseTE, SourceLoc(), DeclNameLoc(),
                                             DeclNameRef(), elt, nullptr,
                                             /*DC*/ strValDecl);
      pat->setImplicit();

      auto labelItem = CaseLabelItem(pat);

      auto *caseValue = new (C) StringLiteralExpr(elt->getNameStr(),
                                                  SourceRange(),
                                                  /*Implicit=*/true);
      auto *returnStmt = new (C) ReturnStmt(SourceLoc(), caseValue);
      auto *caseBody = BraceStmt::create(C, SourceLoc(), ASTNode(returnStmt),
                                         SourceLoc());
      cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                       labelItem, SourceLoc(), SourceLoc(),
                                       caseBody,
                                       /*case body var decls*/ llvm::None));
    }

    auto *selfRef = DerivedConformance::createSelfDeclRef(strValDecl);
    auto *switchStmt =
        SwitchStmt::createImplicit(LabeledStmtInfo(), selfRef, cases, C);
    body = BraceStmt::create(C, SourceLoc(), ASTNode(switchStmt), SourceLoc());
  }

  return { body, /*isTypeChecked=*/false };
}

/// Sets the body of the given constructor to initialize `self` based on the
/// value of the given string param.
///
/// \param initDecl The function whose body to set.
static std::pair<BraceStmt *, bool>
deriveBodyCodingKey_init_stringValue(AbstractFunctionDecl *initDecl, void *) {
  // enum SomeEnum {
  //   case A, B, C
  //   @derived init?(stringValue: String) {
  //     switch stringValue {
  //     case "A":
  //       self = .A
  //     case "B":
  //       self = .B
  //     case "C":
  //       self = .C
  //     default:
  //       return nil
  //     }
  //   }
  // }
  auto *parentDC = initDecl->getDeclContext();
  auto &C = parentDC->getASTContext();

  auto *enumDecl = parentDC->getSelfEnumDecl();
  Type enumType = parentDC->getDeclaredTypeInContext();

  auto elements = enumDecl->getAllElements();
  if (elements.empty() /* empty enum */) {
    return deriveNilReturn(initDecl, nullptr);
  }

  auto *selfRef = DerivedConformance::createSelfDeclRef(initDecl);
  SmallVector<ASTNode, 4> cases;
  for (auto *elt : elements) {
    // Skip the cases that would return unavailable elements since those can't
    // be instantiated at runtime.
    if (elt->getAttrs().isUnavailable(C))
      continue;

    auto *litExpr = new (C) StringLiteralExpr(elt->getNameStr(), SourceRange(),
                                              /*Implicit=*/true);
    auto *litPat = ExprPattern::createImplicit(C, litExpr, /*DC*/ initDecl);

    auto labelItem = CaseLabelItem(litPat);

    auto *metaTyRef = TypeExpr::createImplicit(enumType, C);
    auto *valueExpr = new (C) MemberRefExpr(metaTyRef, SourceLoc(), elt,
                                            DeclNameLoc(), /*Implicit=*/true);

    auto *assignment = new (C) AssignExpr(selfRef, SourceLoc(), valueExpr,
                                          /*Implicit=*/true);

    auto *body = BraceStmt::create(C, SourceLoc(), ASTNode(assignment),
                                   SourceLoc());
    cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                     labelItem, SourceLoc(), SourceLoc(), body,
                                     /*case body var decls*/ llvm::None));
  }

  auto *anyPat = AnyPattern::createImplicit(C);
  auto dfltLabelItem = CaseLabelItem::getDefault(anyPat);

  auto *dfltReturnStmt = new (C) FailStmt(SourceLoc(), SourceLoc());
  auto *dfltBody = BraceStmt::create(C, SourceLoc(), ASTNode(dfltReturnStmt),
                                     SourceLoc());
  cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                   dfltLabelItem, SourceLoc(), SourceLoc(),
                                   dfltBody,
                                   /*case body var decls*/ llvm::None));

  auto *stringValueDecl = initDecl->getParameters()->get(0);
  auto *stringValueRef = new (C) DeclRefExpr(stringValueDecl, DeclNameLoc(),
                                             /*Implicit=*/true);
  auto *switchStmt =
      SwitchStmt::createImplicit(LabeledStmtInfo(), stringValueRef, cases, C);
  auto *body = BraceStmt::create(C, SourceLoc(), ASTNode(switchStmt),
                                 SourceLoc());
  return { body, /*isTypeChecked=*/false };
}

/// Returns whether the given enum is eligible for CodingKey synthesis.
static bool canSynthesizeCodingKey(DerivedConformance &derived) {
  auto enumDecl = cast<EnumDecl>(derived.Nominal);
  // Validate the enum and its raw type.
  
  // If the enum has a raw type (optional), it must be String or Int.
  Type rawType = enumDecl->getRawType();
  if (rawType) {
    auto *parentDC = derived.getConformanceContext();
    rawType = parentDC->mapTypeIntoContext(rawType);

    if (!rawType->isString() && !rawType->isInt()) {
      return false;
    }
  }

  auto inherited = enumDecl->getInherited();
  if (!inherited.empty() && inherited.front().wasValidated() &&
      inherited.front().isError())
    return false;

  // If it meets all of those requirements, we can synthesize CodingKey
  // conformance.
  return true;
}

ValueDecl *DerivedConformance::deriveCodingKey(ValueDecl *requirement) {

  // We can only synthesize CodingKey for enums.
  auto *enumDecl = dyn_cast<EnumDecl>(Nominal);
  if (!enumDecl)
    return nullptr;

  // Check other preconditions for synthesized conformance.
  if (!canSynthesizeCodingKey(*this))
    return nullptr;

  auto rawType = enumDecl->getRawType();
  auto name = requirement->getBaseName();
  if (name == Context.Id_stringValue) {
    // Synthesize `var stringValue: String { get }`
    auto stringType = Context.getStringType();
    auto synth = [rawType](AbstractFunctionDecl *getterDecl) {
      if (rawType && rawType->isString()) {
        // enum SomeStringEnum : String {
        //   case A, B, C
        //   @derived var stringValue: String {
        //     return self.rawValue
        //   }
        getterDecl->setBodySynthesizer(&deriveRawValueReturn);
      } else {
        // enum SomeEnum {
        //   case A, B, C
        //   @derived var stringValue: String {
        //     switch self {
        //       case A:
        //         return "A"
        //       case B:
        //         return "B"
        //       case C:
        //         return "C"
        //     }
        //   }
        // }
        getterDecl->setBodySynthesizer(&deriveBodyCodingKey_enum_stringValue);
      }
    };

    return deriveProperty(*this, stringType, Context.Id_stringValue, synth);

  } else if (name == Context.Id_intValue) {
    // Synthesize `var intValue: Int? { get }`
    auto intType = Context.getIntType();
    auto optionalIntType = OptionalType::get(intType);

    auto synth = [rawType](AbstractFunctionDecl *getterDecl) {
      if (rawType && rawType->isInt()) {
        // enum SomeIntEnum : Int {
        //   case A = 1, B = 2, C = 3
        //   @derived var intValue: Int? {
        //     return self.rawValue
        //   }
        // }
        getterDecl->setBodySynthesizer(&deriveRawValueReturn);
      } else {
        // enum SomeEnum {
        //   case A, B, C
        //   @derived var intValue: Int? {
        //     return nil
        //   }
        // }
        getterDecl->setBodySynthesizer(&deriveNilReturn);
      }
    };

    return deriveProperty(*this, optionalIntType, Context.Id_intValue, synth);
  } else if (name == DeclBaseName::createConstructor()) {
    auto argumentNames = requirement->getName().getArgumentNames();
    if (argumentNames.size() == 1) {
      if (argumentNames[0] == Context.Id_stringValue) {
        // Derive `init?(stringValue:)`
        auto stringType = Context.getStringType();
        auto synth = [rawType](AbstractFunctionDecl *initDecl) {
          if (rawType && rawType->isString()) {
            // enum SomeStringEnum : String {
            //   case A = "a", B = "b", C = "c"
            //   @derived init?(stringValue: String) {
            //     self.init(rawValue: stringValue)
            //   }
            // }
            initDecl->setBodySynthesizer(&deriveRawValueInit);
          } else {
            // enum SomeEnum {
            //   case A, B, C
            //   @derived init?(stringValue: String) {
            //     switch stringValue {
            //     case "A":
            //       self = .A
            //     case "B":
            //       self = .B
            //     case "C":
            //       self = .C
            //     default:
            //       return nil
            //     }
            //   }
            // }
            initDecl->setBodySynthesizer(&deriveBodyCodingKey_init_stringValue);
          }
        };

        return deriveInitDecl(*this, stringType, Context.Id_stringValue, synth);
      } else if (argumentNames[0] == Context.Id_intValue) {
        // Synthesize `init?(intValue:)`
        auto intType = Context.getIntType();
        auto synthesizer = [rawType](AbstractFunctionDecl *initDecl) {
          if (rawType && rawType->isInt()) {
            // enum SomeIntEnum : Int {
            //   case A = 1, B = 2, C = 3
            //   @derived init?(intValue: Int) {
            //     self.init(rawValue: intValue)
            //   }
            // }
            initDecl->setBodySynthesizer(&deriveRawValueInit);
          } else {
            // enum SomeEnum {
            //   case A, B, C
            //   @derived init?(intValue: Int) {
            //     return nil
            //   }
            // }
            initDecl->setBodySynthesizer(&deriveNilReturn);
          }
        };

        return deriveInitDecl(*this, intType, Context.Id_intValue, synthesizer);
      }
    }
  }

  Context.Diags.diagnose(requirement->getLoc(),
                         diag::broken_coding_key_requirement);
  return nullptr;
}
