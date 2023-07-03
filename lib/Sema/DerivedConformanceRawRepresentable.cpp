//===--- DerivedConformanceRawRepresentable.cpp - Derived RawRepresentable ===//
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
//  This file implements implicit derivation of the RawRepresentable protocol
//  for an enum.
//
//===----------------------------------------------------------------------===//

#include "CodeSynthesis.h"
#include "TypeChecker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/APInt.h"
#include "DerivedConformances.h"
#include "TypeCheckDecl.h"

using namespace swift;

static LiteralExpr *cloneRawLiteralExpr(ASTContext &C, LiteralExpr *expr) {
  LiteralExpr *clone;
  if (auto intLit = dyn_cast<IntegerLiteralExpr>(expr)) {
    clone = new (C) IntegerLiteralExpr(intLit->getDigitsText(), expr->getLoc(),
                                       /*implicit*/ true);
    if (intLit->isNegative())
      cast<IntegerLiteralExpr>(clone)->setNegative(expr->getLoc());
  } else if (isa<NilLiteralExpr>(expr)) {
    clone = new (C) NilLiteralExpr(expr->getLoc());
  } else if (auto stringLit = dyn_cast<StringLiteralExpr>(expr)) {
    clone = new (C) StringLiteralExpr(stringLit->getValue(), expr->getLoc());
  } else if (auto floatLit = dyn_cast<FloatLiteralExpr>(expr)) {
    clone = new (C) FloatLiteralExpr(floatLit->getDigitsText(), expr->getLoc(),
                                     /*implicit*/ true);
    if (floatLit->isNegative())
      cast<FloatLiteralExpr>(clone)->setNegative(expr->getLoc());
  } else if (auto boolLit = dyn_cast<BooleanLiteralExpr>(expr)) {
    clone = new (C) BooleanLiteralExpr(boolLit->getValue(), expr->getLoc(),
                                       /*implicit*/true);
  } else {
    llvm_unreachable("invalid raw literal expr");
  }
  clone->setImplicit();
  return clone;
}

static Type deriveRawRepresentable_Raw(DerivedConformance &derived) {
  // enum SomeEnum : SomeType {
  //   @derived
  //   typealias Raw = SomeType
  // }
  auto rawInterfaceType = cast<EnumDecl>(derived.Nominal)->getRawType();
  return derived.getConformanceContext()->mapTypeIntoContext(rawInterfaceType);
}

static std::pair<BraceStmt *, bool>
deriveBodyRawRepresentable_raw(AbstractFunctionDecl *toRawDecl, void *) {
  // enum SomeEnum : SomeType {
  //   case A = 111, B = 222
  //   @derived
  //   var raw: SomeType {
  //     switch self {
  //     case A:
  //       return 111
  //     case B:
  //       return 222
  //     }
  //   }
  // }

  auto parentDC = toRawDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto enumDecl = parentDC->getSelfEnumDecl();

  Type rawTy = enumDecl->getRawType();
  assert(rawTy);
  rawTy = toRawDecl->mapTypeIntoContext(rawTy);

  if (enumDecl->isObjC()) {
    // Special case: ObjC enums are represented by their raw value, so just use
    // a bitcast.

    // return unsafeBitCast(self, to: RawType.self)
    auto functionRef = UnresolvedDeclRefExpr::createImplicit(
        C, C.getIdentifier("unsafeBitCast"), {Identifier(), C.Id_to});
    auto selfRef = DerivedConformance::createSelfDeclRef(toRawDecl);
    auto bareTypeExpr = TypeExpr::createImplicit(rawTy, C);
    auto typeExpr = new (C) DotSelfExpr(bareTypeExpr, SourceLoc(), SourceLoc());

    auto *argList = ArgumentList::forImplicitCallTo(functionRef->getName(),
                                                    {selfRef, typeExpr}, C);
    auto call = CallExpr::createImplicit(C, functionRef, argList);
    auto returnStmt = new (C) ReturnStmt(SourceLoc(), call);
    auto body = BraceStmt::create(C, SourceLoc(), ASTNode(returnStmt),
                                  SourceLoc());
    return { body, /*isTypeChecked=*/false };
  }

  Type enumType = parentDC->getDeclaredTypeInContext();

  SmallVector<ASTNode, 4> cases;
  for (auto elt : enumDecl->getAllElements()) {
    auto pat = new (C)
        EnumElementPattern(TypeExpr::createImplicit(enumType, C), SourceLoc(),
                           DeclNameLoc(), DeclNameRef(), elt, nullptr,
                           /*DC*/ toRawDecl);
    pat->setImplicit();

    auto labelItem = CaseLabelItem(pat);

    auto returnExpr = cloneRawLiteralExpr(C, elt->getRawValueExpr());
    auto returnStmt = new (C) ReturnStmt(SourceLoc(), returnExpr);

    auto body = BraceStmt::create(C, SourceLoc(),
                                  ASTNode(returnStmt), SourceLoc());

    cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                     labelItem, SourceLoc(), SourceLoc(), body,
                                     /*case body var decls*/ llvm::None));
  }

  auto selfRef = DerivedConformance::createSelfDeclRef(toRawDecl);
  auto switchStmt =
      SwitchStmt::createImplicit(LabeledStmtInfo(), selfRef, cases, C);
  auto body = BraceStmt::create(C, SourceLoc(),
                                ASTNode(switchStmt),
                                SourceLoc());
  return { body, /*isTypeChecked=*/false };
}

static void maybeMarkAsInlinable(DerivedConformance &derived,
                                 AbstractFunctionDecl *afd) {
  ASTContext &C = derived.Context;
  auto parentDC = derived.getConformanceContext();
  if (!parentDC->getParentModule()->isResilient()) {
    AccessScope access =
        afd->getFormalAccessScope(nullptr,
                                  /*treatUsableFromInlineAsPublic*/true);
    if (auto *attr = afd->getAttrs().getAttribute<UsableFromInlineAttr>())
      attr->setInvalid();
    if (access.isPublic())
      afd->getAttrs().add(new (C) InlinableAttr(/*implicit*/false));
  }
}

static VarDecl *deriveRawRepresentable_raw(DerivedConformance &derived) {
  ASTContext &C = derived.Context;

  auto enumDecl = cast<EnumDecl>(derived.Nominal);
  auto parentDC = derived.getConformanceContext();
  auto rawInterfaceType = enumDecl->getRawType();
  auto rawType = parentDC->mapTypeIntoContext(rawInterfaceType);

  // Define the property.
  VarDecl *propDecl;
  PatternBindingDecl *pbDecl;
  std::tie(propDecl, pbDecl) = derived.declareDerivedProperty(
      DerivedConformance::SynthesizedIntroducer::Var, C.Id_rawValue,
      rawInterfaceType, rawType, /*isStatic=*/false,
      /*isFinal=*/false);
  addNonIsolatedToSynthesized(enumDecl, propDecl);

  // Define the getter.
  auto getterDecl = DerivedConformance::addGetterToReadOnlyDerivedProperty(
      propDecl, rawType);
  getterDecl->setBodySynthesizer(&deriveBodyRawRepresentable_raw);

  // If the containing module is not resilient, make sure clients can get at
  // the raw value without function call overhead.
  maybeMarkAsInlinable(derived, getterDecl);

  derived.addMembersToConformanceContext({propDecl, pbDecl});

  return propDecl;
}

/// Contains information needed to synthesize a runtime version check.
struct RuntimeVersionCheck {
  PlatformKind Platform;
  llvm::VersionTuple Version;

  RuntimeVersionCheck(PlatformKind Platform, llvm::VersionTuple Version)
    : Platform(Platform), Version(Version)
  { }

  VersionRange getVersionRange() const {
    return VersionRange::allGTE(Version);
  }

  /// Synthesizes a statement which returns nil if the runtime version check
  /// fails, e.g. "guard #available(iOS 10, *) else { return nil }".
  Stmt *createEarlyReturnStmt(ASTContext &C) const {
    // platformSpec = "\(attr.platform) \(attr.introduced)"
    auto platformSpec = new (C) PlatformVersionConstraintAvailabilitySpec(
                            Platform, SourceLoc(),
                            Version, Version, SourceLoc()
                        );

    // otherSpec = "*"
    auto otherSpec = new (C) OtherPlatformAvailabilitySpec(SourceLoc());

    // availableInfo = "#available(\(platformSpec), \(otherSpec))"
    auto availableInfo = PoundAvailableInfo::create(
        C, SourceLoc(), SourceLoc(), { platformSpec, otherSpec }, SourceLoc(),
        false);

    // This won't be filled in by TypeCheckAvailability because we have
    // invalid SourceLocs in this area of the AST.
    availableInfo->setAvailableRange(getVersionRange());

    // earlyReturnBody = "{ return nil }"
    auto earlyReturn = new (C) FailStmt(SourceLoc(), SourceLoc());
    auto earlyReturnBody = BraceStmt::create(C, SourceLoc(),
                                             ASTNode(earlyReturn),
                                             SourceLoc(), /*implicit=*/true);

    // guardStmt = "guard \(availableInfo) else \(earlyReturnBody)"
    StmtConditionElement conds[1] = { availableInfo };
    auto guardStmt = new (C) GuardStmt(SourceLoc(), C.AllocateCopy(conds),
                                       earlyReturnBody, /*implicit=*/true);

    return guardStmt;
  }
};

/// Checks if the case will be available at runtime given the current target
/// platform. If it will never be available, returns false. If it will always
/// be available, returns true. If it will sometimes be available, adds
/// information about the runtime check needed to ensure it is available to
/// \c versionCheck and returns true.
static bool
checkAvailability(const EnumElementDecl *elt, ASTContext &C,
                  llvm::Optional<RuntimeVersionCheck> &versionCheck) {
  auto *attr = elt->getAttrs().getPotentiallyUnavailable(C);

  // Is it always available?
  if (!attr)
    return true;

  AvailableVersionComparison availability = attr->getVersionAvailability(C);

  assert(availability != AvailableVersionComparison::Available &&
         "DeclAttributes::getPotentiallyUnavailable() shouldn't "
         "return an available attribute");

  // Is it never available?
  if (availability != AvailableVersionComparison::PotentiallyUnavailable)
    return false;

  // It's conditionally available; create a version constraint and return true.
  assert(attr->getPlatformAgnosticAvailability() ==
             PlatformAgnosticAvailabilityKind::None &&
         "can only express #available(somePlatform version) checks");
  versionCheck.emplace(attr->Platform, *attr->Introduced);

  return true;
}

static std::pair<BraceStmt *, bool>
deriveBodyRawRepresentable_init(AbstractFunctionDecl *initDecl, void *) {
  // enum SomeEnum : SomeType {
  //   case A = 111, B = 222
  //   @available(iOS 10, *) case C = 333
  //   @derived
  //   init?(rawValue: SomeType) {
  //     switch rawValue {
  //     case 111:
  //       self = .A
  //     case 222:
  //       self = .B
  //     case 333:
  //       guard #available(iOS 10, *) else { return nil }
  //       self = .C
  //     default:
  //       return nil
  //     }
  //   }
  // }
  
  auto parentDC = initDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto nominalTypeDecl = parentDC->getSelfNominalTypeDecl();
  auto enumDecl = cast<EnumDecl>(nominalTypeDecl);

  Type rawTy = enumDecl->getRawType();
  assert(rawTy);
  rawTy = initDecl->mapTypeIntoContext(rawTy);

  bool isStringEnum = rawTy->isString();
  llvm::SmallVector<Expr *, 16> stringExprs;

  Type enumType = parentDC->getDeclaredTypeInContext();

  auto selfDecl = cast<ConstructorDecl>(initDecl)->getImplicitSelfDecl();
  
  SmallVector<ASTNode, 4> cases;
  unsigned Idx = 0;
  for (auto elt : enumDecl->getAllElements()) {
    // First, check case availability. If the case will definitely be
    // unavailable, skip it. If it might be unavailable at runtime, save
    // information about that check in versionCheck and keep processing this
    // element.
    llvm::Optional<RuntimeVersionCheck> versionCheck(llvm::None);
    if (!checkAvailability(elt, C, versionCheck))
      continue;

    // litPat = elt.rawValueExpr as a pattern
    LiteralExpr *litExpr = cloneRawLiteralExpr(C, elt->getRawValueExpr());
    if (isStringEnum) {
      // In case of a string enum we are calling the _findStringSwitchCase
      // function from the library and switching on the returned Int value.
      stringExprs.push_back(litExpr);
      litExpr = IntegerLiteralExpr::createFromUnsigned(C, Idx, SourceLoc()); 
    }
    auto *litPat = ExprPattern::createImplicit(C, litExpr, /*DC*/ initDecl);

    /// Statements in the body of this case.
    SmallVector<ASTNode, 2> stmts;

    // If checkAvailability() discovered we need a runtime version check,
    // add it now.
    if (versionCheck.has_value())
      stmts.push_back(ASTNode(versionCheck->createEarlyReturnStmt(C)));

    // Create a statement which assigns the case to self.

    // valueExpr = "\(enumType).\(elt)"
    auto metaTyRef = TypeExpr::createImplicit(enumType, C);
    auto valueExpr = new (C) MemberRefExpr(metaTyRef, SourceLoc(),
                                           elt, DeclNameLoc(), /*implicit*/true);
    
    // assignment = "self = \(valueExpr)"
    auto selfRef = new (C) DeclRefExpr(selfDecl, DeclNameLoc(),
                                       /*implicit*/true,
                                       AccessSemantics::DirectToStorage);
    auto assignment = new (C) AssignExpr(selfRef, SourceLoc(), valueExpr,
                                         /*implicit*/ true);

    stmts.push_back(ASTNode(assignment));
    
    // body = "{ \(stmts) }" (the braces are silent)
    auto body = BraceStmt::create(C, SourceLoc(),
                                  stmts, SourceLoc());

    // cases.append("case \(litPat): \(body)")
    cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                     CaseLabelItem(litPat), SourceLoc(),
                                     SourceLoc(), body,
                                     /*case body var decls*/ llvm::None));
    ++Idx;
  }

  auto anyPat = AnyPattern::createImplicit(C);
  auto dfltLabelItem = CaseLabelItem::getDefault(anyPat);

  auto dfltReturnStmt = new (C) FailStmt(SourceLoc(), SourceLoc());
  auto dfltBody = BraceStmt::create(C, SourceLoc(),
                                    ASTNode(dfltReturnStmt), SourceLoc());
  cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                   dfltLabelItem, SourceLoc(), SourceLoc(),
                                   dfltBody,
                                   /*case body var decls*/ llvm::None));

  auto rawDecl = initDecl->getParameters()->get(0);
  auto rawRef = new (C) DeclRefExpr(rawDecl, DeclNameLoc(), /*implicit*/true);
  Expr *switchArg = rawRef;
  if (isStringEnum) {
    // Call _findStringSwitchCase with an array of strings as argument.
    auto *Fun = UnresolvedDeclRefExpr::createImplicit(
        C, C.getIdentifier("_findStringSwitchCase"));
    auto *strArray = ArrayExpr::create(C, SourceLoc(), stringExprs, {},
                                       SourceLoc());
    Argument args[] = {
      Argument(SourceLoc(), C.getIdentifier("cases"), strArray),
      Argument(SourceLoc(), C.getIdentifier("string"), rawRef)
    };
    auto *argList = ArgumentList::createImplicit(C, args);
    switchArg = CallExpr::createImplicit(C, Fun, argList);
  }
  auto switchStmt =
      SwitchStmt::createImplicit(LabeledStmtInfo(), switchArg, cases, C);
  auto body = BraceStmt::create(C, SourceLoc(),
                                ASTNode(switchStmt),
                                SourceLoc());
  return { body, /*isTypeChecked=*/false };
}

static ConstructorDecl *
deriveRawRepresentable_init(DerivedConformance &derived) {
  ASTContext &C = derived.Context;

  auto enumDecl = cast<EnumDecl>(derived.Nominal);
  auto parentDC = derived.getConformanceContext();
  auto rawInterfaceType = enumDecl->getRawType();
  auto rawType = parentDC->mapTypeIntoContext(rawInterfaceType);


  assert([&]() -> bool {
    return TypeChecker::conformsToKnownProtocol(
        rawType, KnownProtocolKind::Equatable,
        derived.getParentModule());
  }());

  auto *rawDecl = new (C)
      ParamDecl(SourceLoc(), SourceLoc(),
                C.Id_rawValue, SourceLoc(), C.Id_rawValue, parentDC);
  rawDecl->setSpecifier(ParamSpecifier::Default);
  rawDecl->setInterfaceType(rawInterfaceType);
  rawDecl->setImplicit();
  auto paramList = ParameterList::createWithoutLoc(rawDecl);
  
  DeclName name(C, DeclBaseName::createConstructor(), paramList);
  
  auto initDecl =
    new (C) ConstructorDecl(name, SourceLoc(),
                            /*Failable=*/ true, /*FailabilityLoc=*/SourceLoc(),
                            /*Async=*/false, /*AsyncLoc=*/SourceLoc(),
                            /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(),
                            paramList,
                            /*GenericParams=*/nullptr, parentDC);
  
  initDecl->setImplicit();
  initDecl->setBodySynthesizer(&deriveBodyRawRepresentable_init);
  addNonIsolatedToSynthesized(enumDecl, initDecl);
  initDecl->copyFormalAccessFrom(enumDecl, /*sourceIsParentContext*/true);

  // If the containing module is not resilient, make sure clients can construct
  // an instance without function call overhead.
  maybeMarkAsInlinable(derived, initDecl);

  derived.addMembersToConformanceContext({initDecl});
  return initDecl;
}

bool DerivedConformance::canDeriveRawRepresentable(DeclContext *DC,
                                                   NominalTypeDecl *type) {
  auto enumDecl = dyn_cast<EnumDecl>(type);
  if (!enumDecl)
    return false;

  Type rawType = enumDecl->getRawType();
  if (!rawType || rawType->hasError())
    return false;

  if (!computeAutomaticEnumValueKind(enumDecl))
    return false;

  rawType = DC->mapTypeIntoContext(rawType);

  auto inherited = enumDecl->getInherited();
  if (!inherited.empty() && inherited.front().wasValidated() &&
      inherited.front().isError())
    return false;

  // The raw type must be Equatable, so that we have a suitable ~= for
  // synthesized switch statements.
  if (!TypeChecker::conformsToKnownProtocol(rawType, KnownProtocolKind::Equatable,
                                            DC->getParentModule()))
    return false;

  auto &C = type->getASTContext();
  auto rawValueDecls = enumDecl->lookupDirect(DeclName(C.Id_RawValue));
  if (rawValueDecls.size() > 1)
    return false;

  // Check that the RawValue matches the expected raw type.
  if (!rawValueDecls.empty()) {
    if (auto alias = dyn_cast<TypeDecl>(rawValueDecls.front())) {
      auto ty = alias->getDeclaredInterfaceType();
      if (!DC->mapTypeIntoContext(ty)->isEqual(rawType)) {
        return false;
      }
    }
  }

  // There must be enum elements.
  if (enumDecl->getAllElements().empty())
    return false;

  // Have the type-checker validate that:
  // - the enum elements all have the same type
  // - they all match the enum type
  for (auto elt : enumDecl->getAllElements()) {
    // We cannot synthesize raw representable conformance for an enum with
    // cases that have a payload.
    if (elt->hasAssociatedValues())
      return false;

    if (elt->isInvalid()) {
      return false;
    }
  }

  // If it meets all of those requirements, we can synthesize RawRepresentable conformance.
  return true;
}

ValueDecl *DerivedConformance::deriveRawRepresentable(ValueDecl *requirement) {

  // Check preconditions for synthesized conformance.
  if (!canDeriveRawRepresentable(cast<DeclContext>(ConformanceDecl), Nominal))
    return nullptr;

  if (requirement->getBaseName() == Context.Id_rawValue)
    return deriveRawRepresentable_raw(*this);

  if (requirement->getBaseName() == DeclBaseName::createConstructor())
    return deriveRawRepresentable_init(*this);

  Context.Diags.diagnose(requirement->getLoc(),
                         diag::broken_raw_representable_requirement);
  return nullptr;
}

Type DerivedConformance::deriveRawRepresentable(AssociatedTypeDecl *assocType) {

  // Check preconditions for synthesized conformance.
  if (!canDeriveRawRepresentable(cast<DeclContext>(ConformanceDecl), Nominal))
    return nullptr;

  if (assocType->getName() == Context.Id_RawValue) {
    return deriveRawRepresentable_Raw(*this);
  }

  Context.Diags.diagnose(assocType->getLoc(),
                         diag::broken_raw_representable_requirement);
  return nullptr;
}
