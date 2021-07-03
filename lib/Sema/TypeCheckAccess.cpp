//===--- TypeCheckAccess.cpp - Type Checking for Access Control -----------===//
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
// This file implements access control checking.
//
//===----------------------------------------------------------------------===//

#include "TypeCheckAccess.h"
#include "TypeChecker.h"
#include "TypeCheckAvailability.h"
#include "TypeAccessScopeChecker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Import.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/TypeCheckRequests.h"

using namespace swift;

#define DEBUG_TYPE "TypeCheckAccess"

namespace {

/// Calls \p callback for each type in each requirement provided by
/// \p source.
static void forAllRequirementTypes(
    WhereClauseOwner &&source,
    llvm::function_ref<void(Type, TypeRepr *)> callback) {
  std::move(source).visitRequirements(TypeResolutionStage::Interface,
      [&](const Requirement &req, RequirementRepr *reqRepr) {
    switch (req.getKind()) {
    case RequirementKind::Conformance:
    case RequirementKind::SameType:
    case RequirementKind::Superclass:
      callback(req.getFirstType(),
               RequirementRepr::getFirstTypeRepr(reqRepr));
      callback(req.getSecondType(),
               RequirementRepr::getSecondTypeRepr(reqRepr));
      break;

    case RequirementKind::Layout:
      callback(req.getFirstType(),
               RequirementRepr::getFirstTypeRepr(reqRepr));
      break;
    }
    return false;
  });
}

/// \see checkTypeAccess
using CheckTypeAccessCallback =
    void(AccessScope, const TypeRepr *, DowngradeToWarning);

class AccessControlCheckerBase {
protected:
  bool checkUsableFromInline;

  void checkTypeAccessImpl(
      Type type, TypeRepr *typeRepr, AccessScope contextAccessScope,
      const DeclContext *useDC, bool mayBeInferred,
      llvm::function_ref<CheckTypeAccessCallback> diagnose);

  void checkTypeAccess(
      Type type, TypeRepr *typeRepr, const ValueDecl *context,
      bool mayBeInferred,
      llvm::function_ref<CheckTypeAccessCallback> diagnose);

  void checkTypeAccess(
      const TypeLoc &TL, const ValueDecl *context, bool mayBeInferred,
      llvm::function_ref<CheckTypeAccessCallback> diagnose) {
    return checkTypeAccess(TL.getType(), TL.getTypeRepr(), context,
                           mayBeInferred, diagnose);
  }

  void checkRequirementAccess(
      WhereClauseOwner &&source,
      AccessScope accessScope,
      const DeclContext *useDC,
      llvm::function_ref<CheckTypeAccessCallback> diagnose) {
    forAllRequirementTypes(std::move(source), [&](Type type, TypeRepr *typeRepr) {
      checkTypeAccessImpl(type, typeRepr, accessScope, useDC,
                          /*mayBeInferred*/false, diagnose);
    });
  }

  AccessControlCheckerBase(bool checkUsableFromInline)
      : checkUsableFromInline(checkUsableFromInline) {}

public:
  void checkGenericParamAccess(
    const GenericContext *ownerCtx,
    const Decl *ownerDecl,
    AccessScope accessScope,
    AccessLevel contextAccess);

  void checkGenericParamAccess(
    const GenericContext *ownerCtx,
    const ValueDecl *ownerDecl);
};

class TypeAccessScopeDiagnoser : private ASTWalker {
  AccessScope accessScope;
  const DeclContext *useDC;
  bool treatUsableFromInlineAsPublic;
  const ComponentIdentTypeRepr *offendingType = nullptr;

  bool walkToTypeReprPre(TypeRepr *TR) override {
    // Exit early if we've already found a problem type.
    if (offendingType)
      return false;

    auto CITR = dyn_cast<ComponentIdentTypeRepr>(TR);
    if (!CITR)
      return true;

    const ValueDecl *VD = CITR->getBoundDecl();
    if (!VD)
      return true;

    if (VD->getFormalAccessScope(useDC, treatUsableFromInlineAsPublic)
        != accessScope)
      return true;

    offendingType = CITR;
    return false;
  }

  bool walkToTypeReprPost(TypeRepr *T) override {
    // Exit early if we've already found a problem type.
    return offendingType != nullptr;
  }

  explicit TypeAccessScopeDiagnoser(AccessScope accessScope,
                                    const DeclContext *useDC,
                                    bool treatUsableFromInlineAsPublic)
    : accessScope(accessScope), useDC(useDC),
      treatUsableFromInlineAsPublic(treatUsableFromInlineAsPublic) {}

public:
  static const TypeRepr *findTypeWithScope(TypeRepr *TR,
                                           AccessScope accessScope,
                                           const DeclContext *useDC,
                                           bool treatUsableFromInlineAsPublic) {
    if (TR == nullptr)
      return nullptr;
    TypeAccessScopeDiagnoser diagnoser(accessScope, useDC,
                                       treatUsableFromInlineAsPublic);
    TR->walk(diagnoser);
    return diagnoser.offendingType;
  }
};

} // end anonymous namespace

/// Checks if the access scope of the type described by \p TL contains
/// \p contextAccessScope. If it isn't, calls \p diagnose with a TypeRepr
/// representing the offending part of \p TL.
///
/// The TypeRepr passed to \p diagnose may be null, in which case a particular
/// part of the type that caused the problem could not be found. The DeclContext
/// is never null.
///
/// If \p type might be partially inferred even when \p typeRepr is present
/// (such as for properties), pass \c true for \p mayBeInferred. (This does not
/// include implicitly providing generic parameters for the Self type, such as
/// using `Array` to mean `Array<Element>` in an extension of Array.) If
/// \p typeRepr is known to be absent, it's okay to pass \c false for
/// \p mayBeInferred.
void AccessControlCheckerBase::checkTypeAccessImpl(
    Type type, TypeRepr *typeRepr, AccessScope contextAccessScope,
    const DeclContext *useDC, bool mayBeInferred,
    llvm::function_ref<CheckTypeAccessCallback> diagnose) {

  auto &Context = useDC->getASTContext();
  if (Context.isAccessControlDisabled())
    return;
  // Don't spend time checking local declarations; this is always valid by the
  // time we get to this point.
  if (!contextAccessScope.isPublic() &&
      contextAccessScope.getDeclContext()->isLocalContext())
    return;

  AccessScope problematicAccessScope = AccessScope::getPublic();
  if (type) {
    Optional<AccessScope> typeAccessScope =
        TypeAccessScopeChecker::getAccessScope(type, useDC,
                                               checkUsableFromInline);

    // Note: This means that the type itself is invalid for this particular
    // context, because it references declarations from two incompatible scopes.
    // In this case we should have diagnosed the bad reference already.
    if (!typeAccessScope.hasValue())
      return;
    problematicAccessScope = *typeAccessScope;
  }

  auto downgradeToWarning = DowngradeToWarning::No;

  if (contextAccessScope.hasEqualDeclContextWith(problematicAccessScope) ||
      contextAccessScope.isChildOf(problematicAccessScope)) {

    // /Also/ check the TypeRepr, if present. This can be important when we're
    // unable to preserve typealias sugar that's present in the TypeRepr.
    if (!typeRepr)
      return;

    Optional<AccessScope> typeReprAccessScope =
        TypeAccessScopeChecker::getAccessScope(typeRepr, useDC,
                                               checkUsableFromInline);
    if (!typeReprAccessScope.hasValue())
      return;

    if (contextAccessScope.hasEqualDeclContextWith(*typeReprAccessScope) ||
        contextAccessScope.isChildOf(*typeReprAccessScope)) {
      // Only if both the Type and the TypeRepr follow the access rules can
      // we exit; otherwise we have to emit a diagnostic.
      return;
    }
    problematicAccessScope = *typeReprAccessScope;

  } else {
    // The type violates the rules of access control (with or without taking the
    // TypeRepr into account).

    if (typeRepr && mayBeInferred &&
        Context.LangOpts.EnableAccessControlHacks &&
        !Context.LangOpts.isSwiftVersionAtLeast(5) &&
        !useDC->getParentModule()->isResilient()) {
      // Swift 4.2 and earlier didn't check the Type when a TypeRepr was
      // present. However, this is a major hole when generic parameters are
      // inferred:
      //
      //   public let foo: Optional = VeryPrivateStruct()
      //
      // Downgrade the error to a warning in this case for source compatibility.
      Optional<AccessScope> typeReprAccessScope =
          TypeAccessScopeChecker::getAccessScope(typeRepr, useDC,
                                                 checkUsableFromInline);
      assert(typeReprAccessScope && "valid Type but not valid TypeRepr?");
      if (contextAccessScope.hasEqualDeclContextWith(*typeReprAccessScope) ||
          contextAccessScope.isChildOf(*typeReprAccessScope)) {
        downgradeToWarning = DowngradeToWarning::Yes;
      }
    }
  }

  const TypeRepr *complainRepr = TypeAccessScopeDiagnoser::findTypeWithScope(
      typeRepr, problematicAccessScope, useDC, checkUsableFromInline);

  diagnose(problematicAccessScope, complainRepr, downgradeToWarning);
}

/// Checks if the access scope of the type described by \p TL is valid for the
/// type to be the type of \p context. If it isn't, calls \p diagnose with a
/// TypeRepr representing the offending part of \p TL.
///
/// The TypeRepr passed to \p diagnose may be null, in which case a particular
/// part of the type that caused the problem could not be found.
///
/// If \p type might be partially inferred even when \p typeRepr is present
/// (such as for properties), pass \c true for \p mayBeInferred. (This does not
/// include implicitly providing generic parameters for the Self type, such as
/// using `Array` to mean `Array<Element>` in an extension of Array.) If
/// \p typeRepr is known to be absent, it's okay to pass \c false for
/// \p mayBeInferred.
void AccessControlCheckerBase::checkTypeAccess(
    Type type, TypeRepr *typeRepr, const ValueDecl *context, bool mayBeInferred,
    llvm::function_ref<CheckTypeAccessCallback> diagnose) {
  assert(!isa<ParamDecl>(context));
  const DeclContext *DC = context->getDeclContext();
  AccessScope contextAccessScope =
    context->getFormalAccessScope(
      context->getDeclContext(), checkUsableFromInline);

  checkTypeAccessImpl(type, typeRepr, contextAccessScope, DC, mayBeInferred,
                      diagnose);
}

/// Highlights the given TypeRepr, and adds a note pointing to the type's
/// declaration if possible.
///
/// Just flushes \p diag as is if \p complainRepr is null.
static void highlightOffendingType(InFlightDiagnostic &diag,
                                   const TypeRepr *complainRepr) {
  if (!complainRepr) {
    diag.flush();
    return;
  }

  diag.highlight(complainRepr->getSourceRange());
  diag.flush();

  if (auto CITR = dyn_cast<ComponentIdentTypeRepr>(complainRepr)) {
    const ValueDecl *VD = CITR->getBoundDecl();
    VD->diagnose(diag::kind_declared_here, DescriptiveDeclKind::Type);
  }
}

void AccessControlCheckerBase::checkGenericParamAccess(
    const GenericContext *ownerCtx,
    const Decl *ownerDecl,
    AccessScope accessScope,
    AccessLevel contextAccess) {
  if (!ownerCtx->isGenericContext())
    return;

 // This must stay in sync with diag::generic_param_access.
  enum class ACEK {
    Parameter = 0,
    Requirement
  } accessControlErrorKind;
  auto minAccessScope = AccessScope::getPublic();
  const TypeRepr *complainRepr = nullptr;
  auto downgradeToWarning = DowngradeToWarning::Yes;

  auto callbackACEK = ACEK::Parameter;

  auto callback = [&](AccessScope typeAccessScope,
                      const TypeRepr *thisComplainRepr,
                      DowngradeToWarning thisDowngrade) {
    if (typeAccessScope.isChildOf(minAccessScope) ||
        (thisDowngrade == DowngradeToWarning::No &&
         downgradeToWarning == DowngradeToWarning::Yes) ||
        (!complainRepr &&
         typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
      minAccessScope = typeAccessScope;
      complainRepr = thisComplainRepr;
      accessControlErrorKind = callbackACEK;
      downgradeToWarning = thisDowngrade;
    }
  };

  auto *DC = ownerDecl->getDeclContext();

  if (auto params = ownerCtx->getGenericParams()) {
    for (auto param : *params) {
      if (param->getInherited().empty())
        continue;
      assert(param->getInherited().size() == 1);
      checkTypeAccessImpl(param->getInherited().front().getType(),
                          param->getInherited().front().getTypeRepr(),
                          accessScope, DC, /*mayBeInferred*/false,
                          callback);
    }
  }

  callbackACEK = ACEK::Requirement;

  if (ownerCtx->getTrailingWhereClause()) {
    checkRequirementAccess(WhereClauseOwner(
                             const_cast<GenericContext *>(ownerCtx)),
                           accessScope, DC, callback);
  }

  if (minAccessScope.isPublic())
    return;

  auto &Context = ownerDecl->getASTContext();

  // FIXME: Promote these to an error in the next -swift-version break.
  if (Context.LangOpts.EnableAccessControlHacks &&
      (isa<SubscriptDecl>(ownerDecl) || isa<TypeAliasDecl>(ownerDecl)))
    downgradeToWarning = DowngradeToWarning::Yes;

  if (checkUsableFromInline) {
    if (Context.LangOpts.EnableAccessControlHacks &&
        !Context.isSwiftVersionAtLeast(5))
      downgradeToWarning = DowngradeToWarning::Yes;

    auto diagID = diag::generic_param_usable_from_inline;
    if (downgradeToWarning == DowngradeToWarning::Yes)
      diagID = diag::generic_param_usable_from_inline_warn;
    auto diag =
        Context.Diags.diagnose(ownerDecl, diagID, ownerDecl->getDescriptiveKind(),
                               accessControlErrorKind == ACEK::Requirement);
    highlightOffendingType(diag, complainRepr);
    return;
  }

  auto minAccess = minAccessScope.accessLevelForDiagnostics();

  bool isExplicit =
    ownerDecl->getAttrs().hasAttribute<AccessControlAttr>() ||
    isa<ProtocolDecl>(DC);
  auto diagID = diag::generic_param_access;
  if (downgradeToWarning == DowngradeToWarning::Yes)
    diagID = diag::generic_param_access_warn;
  auto diag = Context.Diags.diagnose(
      ownerDecl, diagID, ownerDecl->getDescriptiveKind(), isExplicit,
      contextAccess, minAccess, isa<FileUnit>(DC),
      accessControlErrorKind == ACEK::Requirement);
  highlightOffendingType(diag, complainRepr);
}

void AccessControlCheckerBase::checkGenericParamAccess(
    const GenericContext *ownerCtx,
    const ValueDecl *ownerDecl) {
  checkGenericParamAccess(ownerCtx, ownerDecl,
                          ownerDecl->getFormalAccessScope(
                              nullptr, checkUsableFromInline),
                          ownerDecl->getFormalAccess());
}

namespace {
class AccessControlChecker : public AccessControlCheckerBase,
                             public DeclVisitor<AccessControlChecker> {
public:

  AccessControlChecker(bool allowUsableFromInline)
    : AccessControlCheckerBase(allowUsableFromInline) {}

  AccessControlChecker()
      : AccessControlCheckerBase(/*checkUsableFromInline=*/false) {}

  void visit(Decl *D) {
    if (D->isInvalid() || D->isImplicit())
      return;

    DeclVisitor<AccessControlChecker>::visit(D);
  }

  // Force all kinds to be handled at a lower level.
  void visitDecl(Decl *D) = delete;
  void visitValueDecl(ValueDecl *D) = delete;

#define UNREACHABLE(KIND, REASON) \
  void visit##KIND##Decl(KIND##Decl *D) { \
    llvm_unreachable(REASON); \
  }
  UNREACHABLE(Import, "cannot appear in a type context")
  UNREACHABLE(Extension, "cannot appear in a type context")
  UNREACHABLE(TopLevelCode, "cannot appear in a type context")
  UNREACHABLE(Operator, "cannot appear in a type context")
  UNREACHABLE(PrecedenceGroup, "cannot appear in a type context")
  UNREACHABLE(Module, "cannot appear in a type context")

  UNREACHABLE(IfConfig, "does not have access control")
  UNREACHABLE(PoundDiagnostic, "does not have access control")
  UNREACHABLE(Param, "does not have access control")
  UNREACHABLE(GenericTypeParam, "does not have access control")
  UNREACHABLE(MissingMember, "does not have access control")
#undef UNREACHABLE

#define UNINTERESTING(KIND) \
  void visit##KIND##Decl(KIND##Decl *D) {}

  UNINTERESTING(EnumCase) // Handled at the EnumElement level.
  UNINTERESTING(Var) // Handled at the PatternBinding level.
  UNINTERESTING(Destructor) // Always correct.
  UNINTERESTING(Accessor) // Handled by the Var or Subscript.

  /// \see visitPatternBindingDecl
  void checkNamedPattern(const NamedPattern *NP, bool isTypeContext,
                         const llvm::DenseSet<const VarDecl *> &seenVars) {
    const VarDecl *theVar = NP->getDecl();
    if (seenVars.count(theVar) || theVar->isInvalid())
      return;

    checkTypeAccess(theVar->getInterfaceType(), nullptr, theVar,
                    /*mayBeInferred*/false,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *complainRepr,
                        DowngradeToWarning downgradeToWarning) {
      auto typeAccess = typeAccessScope.accessLevelForDiagnostics();
      bool isExplicit = theVar->getAttrs().hasAttribute<AccessControlAttr>() ||
                        isa<ProtocolDecl>(theVar->getDeclContext());
      auto theVarAccess =
          isExplicit ? theVar->getFormalAccess()
                     : typeAccessScope.requiredAccessForDiagnostics();
      auto diagID = diag::pattern_type_access_inferred;
      if (downgradeToWarning == DowngradeToWarning::Yes)
        diagID = diag::pattern_type_access_inferred_warn;
      auto &DE = theVar->getASTContext().Diags;
      auto diag = DE.diagnose(NP->getLoc(), diagID, theVar->isLet(),
                              isTypeContext, isExplicit, theVarAccess,
                              isa<FileUnit>(theVar->getDeclContext()),
                              typeAccess, theVar->getInterfaceType());
    });
  }

  void checkTypedPattern(const TypedPattern *TP, bool isTypeContext,
                         llvm::DenseSet<const VarDecl *> &seenVars) {
    VarDecl *anyVar = nullptr;
    TP->forEachVariable([&](VarDecl *V) {
      seenVars.insert(V);
      anyVar = V;
    });
    if (!anyVar)
      return;

    checkTypeAccess(TP->hasType() ? TP->getType() : Type(),
                    TP->getTypeRepr(), anyVar, /*mayBeInferred*/true,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *complainRepr,
                        DowngradeToWarning downgradeToWarning) {
      auto typeAccess = typeAccessScope.accessLevelForDiagnostics();
      bool isExplicit = anyVar->getAttrs().hasAttribute<AccessControlAttr>() ||
                        isa<ProtocolDecl>(anyVar->getDeclContext());
      auto diagID = diag::pattern_type_access;
      if (downgradeToWarning == DowngradeToWarning::Yes)
        diagID = diag::pattern_type_access_warn;
      auto anyVarAccess =
          isExplicit ? anyVar->getFormalAccess()
                     : typeAccessScope.requiredAccessForDiagnostics();
      auto &DE = anyVar->getASTContext().Diags;
      auto diag = DE.diagnose(
          TP->getLoc(), diagID, anyVar->isLet(), isTypeContext, isExplicit,
          anyVarAccess, isa<FileUnit>(anyVar->getDeclContext()), typeAccess);
      highlightOffendingType(diag, complainRepr);
    });

    // Check the property wrapper types.
    for (auto attr : anyVar->getAttachedPropertyWrappers()) {
      checkTypeAccess(attr->getType(), attr->getTypeRepr(), anyVar,
                      /*mayBeInferred=*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeToWarning) {
        auto typeAccess = typeAccessScope.accessLevelForDiagnostics();
        bool isExplicit =
            anyVar->getAttrs().hasAttribute<AccessControlAttr>() ||
            isa<ProtocolDecl>(anyVar->getDeclContext());
        auto anyVarAccess =
            isExplicit ? anyVar->getFormalAccess()
                       : typeAccessScope.requiredAccessForDiagnostics();
        auto diag = anyVar->diagnose(diag::property_wrapper_type_access,
                                     anyVar->isLet(),
                                     isTypeContext,
                                     isExplicit,
                                     anyVarAccess,
                                     isa<FileUnit>(anyVar->getDeclContext()),
                                     typeAccess);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitPatternBindingDecl(PatternBindingDecl *PBD) {
    bool isTypeContext = PBD->getDeclContext()->isTypeContext();

    llvm::DenseSet<const VarDecl *> seenVars;
    for (auto idx : range(PBD->getNumPatternEntries())) {
      PBD->getPattern(idx)->forEachNode([&](const Pattern *P) {
        if (auto *NP = dyn_cast<NamedPattern>(P)) {
          // Only check individual variables if we didn't check an enclosing
          // TypedPattern.
          checkNamedPattern(NP, isTypeContext, seenVars);
          return;
        }

        auto *TP = dyn_cast<TypedPattern>(P);
        if (!TP)
          return;
        checkTypedPattern(TP, isTypeContext, seenVars);
      });
      seenVars.clear();
    }
  }

  void visitTypeAliasDecl(TypeAliasDecl *TAD) {
    checkGenericParamAccess(TAD, TAD);

    checkTypeAccess(TAD->getUnderlyingType(),
                    TAD->getUnderlyingTypeRepr(), TAD, /*mayBeInferred*/false,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *complainRepr,
                        DowngradeToWarning downgradeToWarning) {
      auto typeAccess = typeAccessScope.accessLevelForDiagnostics();
      bool isExplicit =
        TAD->getAttrs().hasAttribute<AccessControlAttr>() ||
        isa<ProtocolDecl>(TAD->getDeclContext());
      auto diagID = diag::type_alias_underlying_type_access;
      if (downgradeToWarning == DowngradeToWarning::Yes)
        diagID = diag::type_alias_underlying_type_access_warn;
      auto aliasAccess = isExplicit
        ? TAD->getFormalAccess()
        : typeAccessScope.requiredAccessForDiagnostics();
      auto diag = TAD->diagnose(diagID, isExplicit, aliasAccess, typeAccess,
                                isa<FileUnit>(TAD->getDeclContext()));
      highlightOffendingType(diag, complainRepr);
    });
  }

  void visitOpaqueTypeDecl(OpaqueTypeDecl *OTD) {
    // TODO(opaque): The constraint class/protocols on the opaque interface, as
    // well as the naming decl for the opaque type, need to be accessible.
  }

  void visitAssociatedTypeDecl(AssociatedTypeDecl *assocType) {
    // This must stay in sync with diag::associated_type_access.
    enum {
      ACEK_DefaultDefinition = 0,
      ACEK_Requirement
    } accessControlErrorKind;
    auto minAccessScope = AccessScope::getPublic();
    const TypeRepr *complainRepr = nullptr;
    auto downgradeToWarning = DowngradeToWarning::No;

    std::for_each(assocType->getInherited().begin(),
                  assocType->getInherited().end(),
                  [&](TypeLoc requirement) {
      checkTypeAccess(requirement, assocType, /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *thisComplainRepr,
                          DowngradeToWarning downgradeDiag) {
        if (typeAccessScope.isChildOf(minAccessScope) ||
            (!complainRepr &&
             typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
          minAccessScope = typeAccessScope;
          complainRepr = thisComplainRepr;
          accessControlErrorKind = ACEK_Requirement;
          downgradeToWarning = downgradeDiag;
        }
      });
    });
    checkTypeAccess(assocType->getDefaultDefinitionType(),
                    assocType->getDefaultDefinitionTypeRepr(), assocType,
                    /*mayBeInferred*/false,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *thisComplainRepr,
                        DowngradeToWarning downgradeDiag) {
      if (typeAccessScope.isChildOf(minAccessScope) ||
          (!complainRepr &&
           typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
        minAccessScope = typeAccessScope;
        complainRepr = thisComplainRepr;
        accessControlErrorKind = ACEK_DefaultDefinition;
        downgradeToWarning = downgradeDiag;
      }
    });

    checkRequirementAccess(assocType,
                           assocType->getFormalAccessScope(),
                           assocType->getDeclContext(),
                           [&](AccessScope typeAccessScope,
                               const TypeRepr *thisComplainRepr,
                               DowngradeToWarning downgradeDiag) {
      if (typeAccessScope.isChildOf(minAccessScope) ||
          (!complainRepr &&
           typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
        minAccessScope = typeAccessScope;
        complainRepr = thisComplainRepr;
        accessControlErrorKind = ACEK_Requirement;
        downgradeToWarning = downgradeDiag;

        // Swift versions before 5.0 did not check requirements on the
        // protocol's where clause, so emit a warning.
        auto &Context = assocType->getASTContext();
        if (Context.LangOpts.EnableAccessControlHacks &&
            !Context.isSwiftVersionAtLeast(5))
          downgradeToWarning = DowngradeToWarning::Yes;
      }
    });

    if (!minAccessScope.isPublic()) {
      auto minAccess = minAccessScope.accessLevelForDiagnostics();
      auto diagID = diag::associated_type_access;
      if (downgradeToWarning == DowngradeToWarning::Yes)
        diagID = diag::associated_type_access_warn;
      auto diag = assocType->diagnose(diagID, assocType->getFormalAccess(),
                                      minAccess, accessControlErrorKind);
      highlightOffendingType(diag, complainRepr);
    }
  }

  void visitEnumDecl(EnumDecl *ED) {
    checkGenericParamAccess(ED, ED);

    if (ED->hasRawType()) {
      Type rawType = ED->getRawType();
      auto rawTypeLocIter = std::find_if(ED->getInherited().begin(),
                                         ED->getInherited().end(),
                                         [&](TypeLoc inherited) {
        if (!inherited.wasValidated())
          return false;
        return inherited.getType().getPointer() == rawType.getPointer();
      });
      if (rawTypeLocIter == ED->getInherited().end())
        return;
      checkTypeAccess(rawType, rawTypeLocIter->getTypeRepr(), ED,
                      /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeToWarning) {
        auto typeAccess = typeAccessScope.accessLevelForDiagnostics();
        bool isExplicit = ED->getAttrs().hasAttribute<AccessControlAttr>();
        auto diagID = diag::enum_raw_type_access;
        if (downgradeToWarning == DowngradeToWarning::Yes)
          diagID = diag::enum_raw_type_access_warn;
        auto enumDeclAccess = isExplicit
          ? ED->getFormalAccess()
          : typeAccessScope.requiredAccessForDiagnostics();
        auto diag = ED->diagnose(diagID, isExplicit, enumDeclAccess, typeAccess,
                                 isa<FileUnit>(ED->getDeclContext()));
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitStructDecl(StructDecl *SD) {
    checkGenericParamAccess(SD, SD);
  }

  void visitClassDecl(ClassDecl *CD) {
    checkGenericParamAccess(CD, CD);

    if (const NominalTypeDecl *superclassDecl = CD->getSuperclassDecl()) {
      // Be slightly defensive here in the presence of badly-ordered
      // inheritance clauses.
      auto superclassLocIter = std::find_if(CD->getInherited().begin(),
                                            CD->getInherited().end(),
                                            [&](TypeLoc inherited) {
        if (!inherited.wasValidated())
          return false;
        Type ty = inherited.getType();
        if (ty->is<ProtocolCompositionType>())
          if (auto superclass = ty->getExistentialLayout().explicitSuperclass)
            ty = superclass;
        return ty->getAnyNominal() == superclassDecl;
      });
      // Sanity check: we couldn't find the superclass for whatever reason
      // (possibly because it's synthetic or something), so don't bother
      // checking it.
      if (superclassLocIter == CD->getInherited().end())
        return;

      auto &Context = CD->getASTContext();
      auto outerDowngradeToWarning = DowngradeToWarning::No;
      if (superclassDecl->isGenericContext() &&
          Context.LangOpts.EnableAccessControlHacks &&
          !Context.isSwiftVersionAtLeast(5)) {
        // Swift 4 failed to properly check this if the superclass was generic,
        // because the above loop was too strict.
        outerDowngradeToWarning = DowngradeToWarning::Yes;
      }

      checkTypeAccess(CD->getSuperclass(), superclassLocIter->getTypeRepr(), CD,
                      /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeToWarning) {
        auto typeAccess = typeAccessScope.accessLevelForDiagnostics();
        bool isExplicit = CD->getAttrs().hasAttribute<AccessControlAttr>();
        auto diagID = diag::class_super_access;
        if (downgradeToWarning == DowngradeToWarning::Yes ||
            outerDowngradeToWarning == DowngradeToWarning::Yes)
          diagID = diag::class_super_access_warn;
        auto classDeclAccess = isExplicit
          ? CD->getFormalAccess()
          : typeAccessScope.requiredAccessForDiagnostics();

        auto diag =
            CD->diagnose(diagID, isExplicit, classDeclAccess, typeAccess,
                         isa<FileUnit>(CD->getDeclContext()),
                         superclassLocIter->getTypeRepr() != complainRepr);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitProtocolDecl(ProtocolDecl *proto) {
    // This must stay in sync with diag::protocol_access.
    enum {
      PCEK_Refine = 0,
      PCEK_Requirement
    } protocolControlErrorKind;

    auto minAccessScope = AccessScope::getPublic();
    const TypeRepr *complainRepr = nullptr;
    auto downgradeToWarning = DowngradeToWarning::No;
    DescriptiveDeclKind declKind = DescriptiveDeclKind::Protocol;

    // FIXME: Hack to ensure that we've computed the types involved here.
    ASTContext &ctx = proto->getASTContext();
    for (unsigned i : indices(proto->getInherited())) {
      (void)evaluateOrDefault(ctx.evaluator,
                              InheritedTypeRequest{
                                proto, i, TypeResolutionStage::Interface},
                              Type());
    }

    auto declKindForType = [](Type type) {
      if (isa<TypeAliasType>(type.getPointer()))
        return DescriptiveDeclKind::TypeAlias;
      else if (auto nominal = type->getAnyNominal())
        return nominal->getDescriptiveKind();
      else
        return DescriptiveDeclKind::Type;
    };

    std::for_each(proto->getInherited().begin(),
                  proto->getInherited().end(),
                  [&](TypeLoc requirement) {
      checkTypeAccess(requirement, proto, /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *thisComplainRepr,
                          DowngradeToWarning downgradeDiag) {
        if (typeAccessScope.isChildOf(minAccessScope) ||
            (!complainRepr &&
             typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
          minAccessScope = typeAccessScope;
          complainRepr = thisComplainRepr;
          protocolControlErrorKind = PCEK_Refine;
          downgradeToWarning = downgradeDiag;
          declKind = declKindForType(requirement.getType());
        }
      });
    });

    forAllRequirementTypes(proto, [&](Type type, TypeRepr *typeRepr) {
      checkTypeAccess(
          type, typeRepr, proto,
          /*mayBeInferred*/ false,
          [&](AccessScope typeAccessScope, const TypeRepr *thisComplainRepr,
              DowngradeToWarning downgradeDiag) {
            if (typeAccessScope.isChildOf(minAccessScope) ||
                (!complainRepr &&
                 typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
              minAccessScope = typeAccessScope;
              complainRepr = thisComplainRepr;
              protocolControlErrorKind = PCEK_Requirement;
              downgradeToWarning = downgradeDiag;
              declKind = declKindForType(type);
              // Swift versions before 5.0 did not check requirements on the
              // protocol's where clause, so emit a warning.
              auto &Context = proto->getASTContext();
              if (Context.LangOpts.EnableAccessControlHacks &&
                  !Context.isSwiftVersionAtLeast(5))
                downgradeToWarning = DowngradeToWarning::Yes;
            }
          });
    });

    if (!minAccessScope.isPublic()) {
      auto minAccess = minAccessScope.accessLevelForDiagnostics();
      bool isExplicit = proto->getAttrs().hasAttribute<AccessControlAttr>();
      auto protoAccess = isExplicit
          ? proto->getFormalAccess()
          : minAccessScope.requiredAccessForDiagnostics();
      auto diagID = diag::protocol_access;
      if (downgradeToWarning == DowngradeToWarning::Yes)
        diagID = diag::protocol_access_warn;
      auto diag = proto->diagnose(
          diagID, isExplicit, protoAccess, protocolControlErrorKind, minAccess,
          isa<FileUnit>(proto->getDeclContext()), declKind);
      highlightOffendingType(diag, complainRepr);
    }
  }

  void visitSubscriptDecl(SubscriptDecl *SD) {
    checkGenericParamAccess(SD, SD);

    auto minAccessScope = AccessScope::getPublic();
    const TypeRepr *complainRepr = nullptr;
    auto downgradeToWarning = DowngradeToWarning::No;
    bool problemIsElement = false;

    for (auto &P : *SD->getIndices()) {
      checkTypeAccess(
          P->getInterfaceType(), P->getTypeRepr(), SD, /*mayBeInferred*/ false,
          [&](AccessScope typeAccessScope, const TypeRepr *thisComplainRepr,
              DowngradeToWarning downgradeDiag) {
            if (typeAccessScope.isChildOf(minAccessScope) ||
                (!complainRepr &&
                 typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
              minAccessScope = typeAccessScope;
              complainRepr = thisComplainRepr;
              downgradeToWarning = downgradeDiag;
            }
          });
    }

    checkTypeAccess(SD->getElementInterfaceType(), SD->getElementTypeRepr(),
                    SD, /*mayBeInferred*/false,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *thisComplainRepr,
                        DowngradeToWarning downgradeDiag) {
      if (typeAccessScope.isChildOf(minAccessScope) ||
          (!complainRepr &&
           typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
        minAccessScope = typeAccessScope;
        complainRepr = thisComplainRepr;
        downgradeToWarning = downgradeDiag;
        problemIsElement = true;
      }
    });

    if (!minAccessScope.isPublic()) {
      auto minAccess = minAccessScope.accessLevelForDiagnostics();
      bool isExplicit =
        SD->getAttrs().hasAttribute<AccessControlAttr>() ||
        isa<ProtocolDecl>(SD->getDeclContext());
      auto diagID = diag::subscript_type_access;
      if (downgradeToWarning == DowngradeToWarning::Yes)
        diagID = diag::subscript_type_access_warn;
      auto subscriptDeclAccess = isExplicit
        ? SD->getFormalAccess()
        : minAccessScope.requiredAccessForDiagnostics();
      auto diag = SD->diagnose(diagID, isExplicit, subscriptDeclAccess,
                               minAccess, problemIsElement);
      highlightOffendingType(diag, complainRepr);
    }
  }

  void visitAbstractFunctionDecl(AbstractFunctionDecl *fn) {
    bool isTypeContext = fn->getDeclContext()->isTypeContext();

    checkGenericParamAccess(fn, fn);

    // This must stay in sync with diag::function_type_access.
    enum {
      FK_Function = 0,
      FK_Method,
      FK_Initializer
    };

    auto minAccessScope = AccessScope::getPublic();
    const TypeRepr *complainRepr = nullptr;
    auto downgradeToWarning = DowngradeToWarning::No;

    bool hasInaccessibleParameterWrapper = false;
    for (auto *P : *fn->getParameters()) {
      // Check for inaccessible API property wrappers attached to the parameter.
      if (P->hasExternalPropertyWrapper()) {
        auto wrapperAttrs = P->getAttachedPropertyWrappers();
        for (auto index : indices(wrapperAttrs)) {
          auto wrapperType = P->getAttachedPropertyWrapperType(index);
          auto wrapperTypeRepr = wrapperAttrs[index]->getTypeRepr();
          checkTypeAccess(wrapperType, wrapperTypeRepr, fn, /*mayBeInferred*/ false,
              [&](AccessScope typeAccessScope, const TypeRepr *thisComplainRepr,
                  DowngradeToWarning downgradeDiag) {
                if (typeAccessScope.isChildOf(minAccessScope) ||
                    (!complainRepr &&
                     typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
                  minAccessScope = typeAccessScope;
                  complainRepr = thisComplainRepr;
                  downgradeToWarning = downgradeDiag;
                  hasInaccessibleParameterWrapper = true;
                }
              });
        }
      }

      checkTypeAccess(
          P->getInterfaceType(), P->getTypeRepr(), fn, /*mayBeInferred*/ false,
          [&](AccessScope typeAccessScope, const TypeRepr *thisComplainRepr,
              DowngradeToWarning downgradeDiag) {
            if (typeAccessScope.isChildOf(minAccessScope) ||
                (!complainRepr &&
                 typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
              minAccessScope = typeAccessScope;
              complainRepr = thisComplainRepr;
              downgradeToWarning = downgradeDiag;
            }
          });
    }

    bool problemIsResult = false;
    if (auto FD = dyn_cast<FuncDecl>(fn)) {
      checkTypeAccess(FD->getResultInterfaceType(), FD->getResultTypeRepr(),
                      FD, /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *thisComplainRepr,
                          DowngradeToWarning downgradeDiag) {
        if (typeAccessScope.isChildOf(minAccessScope) ||
            (!complainRepr &&
             typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
          minAccessScope = typeAccessScope;
          complainRepr = thisComplainRepr;
          downgradeToWarning = downgradeDiag;
          problemIsResult = true;
        }
      });
    }

    if (!minAccessScope.isPublic()) {
      auto minAccess = minAccessScope.accessLevelForDiagnostics();
      auto functionKind = isa<ConstructorDecl>(fn)
        ? FK_Initializer
        : isTypeContext ? FK_Method : FK_Function;
      bool isExplicit =
        fn->getAttrs().hasAttribute<AccessControlAttr>() ||
        isa<ProtocolDecl>(fn->getDeclContext());
      auto fnAccess = isExplicit
        ? fn->getFormalAccess()
        : minAccessScope.requiredAccessForDiagnostics();

      auto diagID = diag::function_type_access;
      if (downgradeToWarning == DowngradeToWarning::Yes)
        diagID = diag::function_type_access_warn;
      auto diag = fn->diagnose(diagID, isExplicit, fnAccess,
                               isa<FileUnit>(fn->getDeclContext()), minAccess,
                               functionKind, problemIsResult,
                               hasInaccessibleParameterWrapper);
      highlightOffendingType(diag, complainRepr);
    }
  }

  void visitEnumElementDecl(EnumElementDecl *EED) {
    if (!EED->hasAssociatedValues())
      return;
    for (auto &P : *EED->getParameterList()) {
      checkTypeAccess(
          P->getInterfaceType(), P->getTypeRepr(), EED, /*mayBeInferred*/ false,
          [&](AccessScope typeAccessScope, const TypeRepr *complainRepr,
              DowngradeToWarning downgradeToWarning) {
            auto typeAccess = typeAccessScope.accessLevelForDiagnostics();
            auto diagID = diag::enum_case_access;
            if (downgradeToWarning == DowngradeToWarning::Yes)
              diagID = diag::enum_case_access_warn;
            auto diag =
                EED->diagnose(diagID, EED->getFormalAccess(), typeAccess);
            highlightOffendingType(diag, complainRepr);
          });
    }
  }
};

class UsableFromInlineChecker : public AccessControlCheckerBase,
                                public DeclVisitor<UsableFromInlineChecker> {
public:
  UsableFromInlineChecker()
      : AccessControlCheckerBase(/*checkUsableFromInline=*/true) {}

  static bool shouldSkipChecking(const ValueDecl *VD) {
    if (VD->getFormalAccess() != AccessLevel::Internal)
      return true;
    return !VD->isUsableFromInline();
  };

  void visit(Decl *D) {
    auto &Context = D->getASTContext();
    if (Context.LangOpts.EnableAccessControlHacks &&
        !Context.isSwiftVersionAtLeast(4, 2))
      return;

    if (D->isInvalid() || D->isImplicit())
      return;

    if (auto *VD = dyn_cast<ValueDecl>(D))
      if (shouldSkipChecking(VD))
        return;

    DeclVisitor<UsableFromInlineChecker>::visit(D);
  }

  // Force all kinds to be handled at a lower level.
  void visitDecl(Decl *D) = delete;
  void visitValueDecl(ValueDecl *D) = delete;

#define UNREACHABLE(KIND, REASON) \
  void visit##KIND##Decl(KIND##Decl *D) { \
    llvm_unreachable(REASON); \
  }
  UNREACHABLE(Import, "cannot appear in a type context")
  UNREACHABLE(Extension, "cannot appear in a type context")
  UNREACHABLE(TopLevelCode, "cannot appear in a type context")
  UNREACHABLE(Operator, "cannot appear in a type context")
  UNREACHABLE(PrecedenceGroup, "cannot appear in a type context")
  UNREACHABLE(Module, "cannot appear in a type context")

  UNREACHABLE(Param, "does not have access control")
  UNREACHABLE(GenericTypeParam, "does not have access control")
  UNREACHABLE(MissingMember, "does not have access control")
#undef UNREACHABLE

#define UNINTERESTING(KIND) \
  void visit##KIND##Decl(KIND##Decl *D) {}

  UNINTERESTING(IfConfig) // Does not have access control.
  UNINTERESTING(PoundDiagnostic) // Does not have access control.
  UNINTERESTING(EnumCase) // Handled at the EnumElement level.
  UNINTERESTING(Var) // Handled at the PatternBinding level.
  UNINTERESTING(Destructor) // Always correct.
  UNINTERESTING(Accessor) // Handled by the Var or Subscript.
  UNINTERESTING(OpaqueType) // Handled by the Var or Subscript.

  /// If \p VD's layout is exposed by a @frozen struct or class, return said
  /// struct or class.
  ///
  /// Stored instance properties in @frozen structs and classes must always use
  /// public/@usableFromInline types. In these cases, check the access against
  /// the struct instead of the VarDecl, and customize the diagnostics.
  static const ValueDecl *
  getFixedLayoutStructContext(const VarDecl *VD) {
    if (VD->isLayoutExposedToClients())
      return dyn_cast<NominalTypeDecl>(VD->getDeclContext());

    return nullptr;
  }

  /// \see visitPatternBindingDecl
  void checkNamedPattern(const NamedPattern *NP,
                         bool isTypeContext,
                         const llvm::DenseSet<const VarDecl *> &seenVars) {
    const VarDecl *theVar = NP->getDecl();
    auto *fixedLayoutStructContext = getFixedLayoutStructContext(theVar);
    if (!fixedLayoutStructContext && shouldSkipChecking(theVar))
      return;
    // Only check individual variables if we didn't check an enclosing
    // TypedPattern.
    if (seenVars.count(theVar) || theVar->isInvalid())
      return;

    checkTypeAccess(
        theVar->getInterfaceType(), nullptr,
        fixedLayoutStructContext ? fixedLayoutStructContext : theVar,
        /*mayBeInferred*/ false,
        [&](AccessScope typeAccessScope, const TypeRepr *complainRepr,
            DowngradeToWarning downgradeToWarning) {
          auto &Ctx = theVar->getASTContext();
          auto diagID = diag::pattern_type_not_usable_from_inline_inferred;
          if (fixedLayoutStructContext) {
            diagID = diag::pattern_type_not_usable_from_inline_inferred_frozen;
          } else if (Ctx.LangOpts.EnableAccessControlHacks &&
                     !Ctx.isSwiftVersionAtLeast(5)) {
            diagID = diag::pattern_type_not_usable_from_inline_inferred_warn;
          }
          Ctx.Diags.diagnose(NP->getLoc(), diagID, theVar->isLet(),
                             isTypeContext, theVar->getInterfaceType());
        });
  }

  /// \see visitPatternBindingDecl
  void checkTypedPattern(const TypedPattern *TP,
                         bool isTypeContext,
                         llvm::DenseSet<const VarDecl *> &seenVars) {
    // FIXME: We need an access level to check against, so we pull one out
    // of some random VarDecl in the pattern. They're all going to be the
    // same, but still, ick.
    VarDecl *anyVar = nullptr;
    TP->forEachVariable([&](VarDecl *V) {
      seenVars.insert(V);
      anyVar = V;
    });
    if (!anyVar)
      return;
    auto *fixedLayoutStructContext = getFixedLayoutStructContext(anyVar);
    if (!fixedLayoutStructContext && shouldSkipChecking(anyVar))
      return;

    checkTypeAccess(
        TP->hasType() ? TP->getType() : Type(),
        TP->getTypeRepr(),
        fixedLayoutStructContext ? fixedLayoutStructContext : anyVar,
        /*mayBeInferred*/ true,
        [&](AccessScope typeAccessScope, const TypeRepr *complainRepr,
            DowngradeToWarning downgradeToWarning) {
          auto &Ctx = anyVar->getASTContext();
          auto diagID = diag::pattern_type_not_usable_from_inline;
          if (fixedLayoutStructContext)
            diagID = diag::pattern_type_not_usable_from_inline_frozen;
          else if (Ctx.LangOpts.EnableAccessControlHacks &&
                   !Ctx.isSwiftVersionAtLeast(5))
            diagID = diag::pattern_type_not_usable_from_inline_warn;
          auto diag = Ctx.Diags.diagnose(TP->getLoc(), diagID, anyVar->isLet(),
                                         isTypeContext);
          highlightOffendingType(diag, complainRepr);
        });

    for (auto attr : anyVar->getAttachedPropertyWrappers()) {
      checkTypeAccess(attr->getType(), attr->getTypeRepr(),
                      fixedLayoutStructContext ? fixedLayoutStructContext
                                               : anyVar,
                      /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeToWarning) {
        auto diag = anyVar->diagnose(
            diag::property_wrapper_type_not_usable_from_inline,
            anyVar->isLet(), isTypeContext);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitPatternBindingDecl(PatternBindingDecl *PBD) {
    bool isTypeContext = PBD->getDeclContext()->isTypeContext();

    llvm::DenseSet<const VarDecl *> seenVars;
    for (auto idx : range(PBD->getNumPatternEntries())) {
      PBD->getPattern(idx)->forEachNode([&](const Pattern *P) {
        if (auto *NP = dyn_cast<NamedPattern>(P)) {
          checkNamedPattern(NP, isTypeContext, seenVars);
          return;
        }

        auto *TP = dyn_cast<TypedPattern>(P);
        if (!TP)
          return;
        checkTypedPattern(TP, isTypeContext, seenVars);
      });
      seenVars.clear();
    }
  }

  void visitTypeAliasDecl(TypeAliasDecl *TAD) {
    checkGenericParamAccess(TAD, TAD);

    checkTypeAccess(TAD->getUnderlyingType(),
                    TAD->getUnderlyingTypeRepr(), TAD, /*mayBeInferred*/false,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *complainRepr,
                        DowngradeToWarning downgradeToWarning) {
      auto diagID = diag::type_alias_underlying_type_not_usable_from_inline;
      auto &Context = TAD->getASTContext();
      if (Context.LangOpts.EnableAccessControlHacks &&
          !Context.isSwiftVersionAtLeast(5))
        diagID = diag::type_alias_underlying_type_not_usable_from_inline_warn;
      auto diag = TAD->diagnose(diagID);
      highlightOffendingType(diag, complainRepr);
    });
  }

  void visitAssociatedTypeDecl(AssociatedTypeDecl *assocType) {
    // This must stay in sync with diag::associated_type_not_usable_from_inline.
    enum {
      ACEK_DefaultDefinition = 0,
      ACEK_Requirement
    };

    std::for_each(assocType->getInherited().begin(),
                  assocType->getInherited().end(),
                  [&](TypeLoc requirement) {
      checkTypeAccess(requirement, assocType, /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
            const TypeRepr *complainRepr,
            DowngradeToWarning downgradeDiag) {
        auto diagID = diag::associated_type_not_usable_from_inline;
        auto &Context = assocType->getASTContext();
        if (Context.LangOpts.EnableAccessControlHacks &&
            !Context.isSwiftVersionAtLeast(5))
          diagID = diag::associated_type_not_usable_from_inline_warn;
        auto diag = assocType->diagnose(diagID, ACEK_Requirement);
        highlightOffendingType(diag, complainRepr);
      });
    });
    checkTypeAccess(assocType->getDefaultDefinitionType(),
                    assocType->getDefaultDefinitionTypeRepr(), assocType,
                     /*mayBeInferred*/false,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *complainRepr,
                        DowngradeToWarning downgradeDiag) {
      auto diagID = diag::associated_type_not_usable_from_inline;
      auto &Context = assocType->getASTContext();
      if (Context.LangOpts.EnableAccessControlHacks &&
          !Context.isSwiftVersionAtLeast(5))
        diagID = diag::associated_type_not_usable_from_inline_warn;
      auto diag = assocType->diagnose(diagID, ACEK_DefaultDefinition);
      highlightOffendingType(diag, complainRepr);
    });

    if (assocType->getTrailingWhereClause()) {
      auto accessScope =
        assocType->getFormalAccessScope(nullptr);
      checkRequirementAccess(assocType,
                             accessScope,
                             assocType->getDeclContext(),
                             [&](AccessScope typeAccessScope,
                                 const TypeRepr *complainRepr,
                                 DowngradeToWarning downgradeDiag) {
        auto diagID = diag::associated_type_not_usable_from_inline;
        auto &Context = assocType->getASTContext();
        if (Context.LangOpts.EnableAccessControlHacks &&
            !Context.isSwiftVersionAtLeast(5))
          diagID = diag::associated_type_not_usable_from_inline_warn;
        auto diag = assocType->diagnose(diagID, ACEK_Requirement);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitEnumDecl(const EnumDecl *ED) {
    checkGenericParamAccess(ED, ED);

    if (ED->hasRawType()) {
      Type rawType = ED->getRawType();
      auto rawTypeLocIter = std::find_if(ED->getInherited().begin(),
                                         ED->getInherited().end(),
                                         [&](TypeLoc inherited) {
        if (!inherited.wasValidated())
          return false;
        return inherited.getType().getPointer() == rawType.getPointer();
      });
      if (rawTypeLocIter == ED->getInherited().end())
        return;
      checkTypeAccess(rawType, rawTypeLocIter->getTypeRepr(), ED,
                       /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeToWarning) {
        auto diagID = diag::enum_raw_type_not_usable_from_inline;
        auto &Context = ED->getASTContext();
        if (Context.LangOpts.EnableAccessControlHacks &&
            !Context.isSwiftVersionAtLeast(5))
          diagID = diag::enum_raw_type_not_usable_from_inline_warn;
        auto diag = ED->diagnose(diagID);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitStructDecl(StructDecl *SD) {
    checkGenericParamAccess(SD, SD);
  }

  void visitClassDecl(ClassDecl *CD) {
    checkGenericParamAccess(CD, CD);

    if (CD->hasSuperclass()) {
      const NominalTypeDecl *superclassDecl = CD->getSuperclassDecl();
      // Be slightly defensive here in the presence of badly-ordered
      // inheritance clauses.
      auto superclassLocIter = std::find_if(CD->getInherited().begin(),
                                            CD->getInherited().end(),
                                            [&](TypeLoc inherited) {
        if (!inherited.wasValidated())
          return false;
        Type ty = inherited.getType();
        if (ty->is<ProtocolCompositionType>())
          if (auto superclass = ty->getExistentialLayout().explicitSuperclass)
            ty = superclass;
        return ty->getAnyNominal() == superclassDecl;
      });
      // Sanity check: we couldn't find the superclass for whatever reason
      // (possibly because it's synthetic or something), so don't bother
      // checking it.
      if (superclassLocIter == CD->getInherited().end())
        return;

      checkTypeAccess(CD->getSuperclass(), superclassLocIter->getTypeRepr(), CD,
                       /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeToWarning) {
        auto diagID = diag::class_super_not_usable_from_inline;
        auto &Context = CD->getASTContext();
        if (Context.LangOpts.EnableAccessControlHacks &&
            !Context.isSwiftVersionAtLeast(5))
          diagID = diag::class_super_not_usable_from_inline_warn;
        auto diag = CD->diagnose(diagID, superclassLocIter->getTypeRepr() !=
                                             complainRepr);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitProtocolDecl(ProtocolDecl *proto) {
    // This must stay in sync with diag::protocol_usable_from_inline.
    enum {
      PCEK_Refine = 0,
      PCEK_Requirement
    };

    std::for_each(proto->getInherited().begin(),
                  proto->getInherited().end(),
                  [&](TypeLoc requirement) {
      checkTypeAccess(requirement, proto, /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeDiag) {
        auto diagID = diag::protocol_usable_from_inline;
        auto &Context = proto->getASTContext();
        if (Context.LangOpts.EnableAccessControlHacks &&
            !Context.isSwiftVersionAtLeast(5))
          diagID = diag::protocol_usable_from_inline_warn;
        auto diag = proto->diagnose(diagID, PCEK_Refine);
        highlightOffendingType(diag, complainRepr);
      });
    });

    if (proto->getTrailingWhereClause()) {
      auto accessScope = proto->getFormalAccessScope(nullptr,
                                                     /*checkUsableFromInline*/true);
      checkRequirementAccess(proto,
                             accessScope,
                             proto->getDeclContext(),
                             [&](AccessScope typeAccessScope,
                                 const TypeRepr *complainRepr,
                                 DowngradeToWarning downgradeDiag) {
        auto diagID = diag::protocol_usable_from_inline;
        auto &Context = proto->getASTContext();
        if (Context.LangOpts.EnableAccessControlHacks &&
            !Context.isSwiftVersionAtLeast(5))
          diagID = diag::protocol_usable_from_inline_warn;
        auto diag = proto->diagnose(diagID, PCEK_Requirement);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitSubscriptDecl(SubscriptDecl *SD) {
    checkGenericParamAccess(SD, SD);

    for (auto &P : *SD->getIndices()) {
      checkTypeAccess(
          P->getInterfaceType(), P->getTypeRepr(), SD, /*mayBeInferred*/ false,
          [&](AccessScope typeAccessScope, const TypeRepr *complainRepr,
              DowngradeToWarning downgradeDiag) {
            auto diagID = diag::subscript_type_usable_from_inline;
            auto &Context = SD->getASTContext();
            if (Context.LangOpts.EnableAccessControlHacks &&
                !Context.isSwiftVersionAtLeast(5))
              diagID = diag::subscript_type_usable_from_inline_warn;
            auto diag = SD->diagnose(diagID, /*problemIsElement=*/false);
            highlightOffendingType(diag, complainRepr);
          });
    }

    checkTypeAccess(SD->getElementInterfaceType(), SD->getElementTypeRepr(),
                    SD, /*mayBeInferred*/false,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *complainRepr,
                        DowngradeToWarning downgradeDiag) {
      auto diagID = diag::subscript_type_usable_from_inline;
      auto &Context = SD->getASTContext();
      if (Context.LangOpts.EnableAccessControlHacks &&
          !Context.isSwiftVersionAtLeast(5))
      diagID = diag::subscript_type_usable_from_inline_warn;
      auto diag = SD->diagnose(diagID, /*problemIsElement=*/true);
      highlightOffendingType(diag, complainRepr);
    });
  }

  void visitAbstractFunctionDecl(AbstractFunctionDecl *fn) {
    bool isTypeContext = fn->getDeclContext()->isTypeContext();

    checkGenericParamAccess(fn, fn);

    // This must stay in sync with diag::function_type_usable_from_inline.
    enum {
      FK_Function = 0,
      FK_Method,
      FK_Initializer
    };

    auto functionKind = isa<ConstructorDecl>(fn)
      ? FK_Initializer
      : isTypeContext ? FK_Method : FK_Function;

    for (auto *P : *fn->getParameters()) {
      // Check for inaccessible API property wrappers attached to the parameter.
      if (P->hasExternalPropertyWrapper()) {
        auto wrapperAttrs = P->getAttachedPropertyWrappers();
        for (auto index : indices(wrapperAttrs)) {
          auto wrapperType = P->getAttachedPropertyWrapperType(index);
          auto wrapperTypeRepr = wrapperAttrs[index]->getTypeRepr();
          checkTypeAccess(wrapperType, wrapperTypeRepr, fn, /*mayBeInferred*/ false,
              [&](AccessScope typeAccessScope, const TypeRepr *complainRepr,
                  DowngradeToWarning downgradeDiag) {
                auto diagID = diag::function_type_usable_from_inline;
                if (!fn->getASTContext().isSwiftVersionAtLeast(5))
                  diagID = diag::function_type_usable_from_inline_warn;
                auto diag = fn->diagnose(diagID, functionKind,
                                         /*problemIsResult=*/false,
                                         /*inaccessibleWrapper=*/true);
                highlightOffendingType(diag, complainRepr);
              });
        }
      }

      checkTypeAccess(
          P->getInterfaceType(), P->getTypeRepr(), fn, /*mayBeInferred*/ false,
          [&](AccessScope typeAccessScope, const TypeRepr *complainRepr,
              DowngradeToWarning downgradeDiag) {
            auto diagID = diag::function_type_usable_from_inline;
            auto &Context = fn->getASTContext();
            if (Context.LangOpts.EnableAccessControlHacks &&
                !Context.isSwiftVersionAtLeast(5))
              diagID = diag::function_type_usable_from_inline_warn;
            auto diag = fn->diagnose(diagID, functionKind,
                                     /*problemIsResult=*/false,
                                     /*inaccessibleWrapper=*/false);
            highlightOffendingType(diag, complainRepr);
          });
    }

    if (auto FD = dyn_cast<FuncDecl>(fn)) {
      checkTypeAccess(FD->getResultInterfaceType(), FD->getResultTypeRepr(),
                      FD, /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeDiag) {
        auto diagID = diag::function_type_usable_from_inline;
        auto &Context = fn->getASTContext();
        if (Context.LangOpts.EnableAccessControlHacks &&
            !Context.isSwiftVersionAtLeast(5))
          diagID = diag::function_type_usable_from_inline_warn;
        auto diag = fn->diagnose(diagID, functionKind,
                                 /*problemIsResult=*/true,
                                 /*inaccessibleWrapper=*/false);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitEnumElementDecl(EnumElementDecl *EED) {
    if (!EED->hasAssociatedValues())
      return;
    for (auto &P : *EED->getParameterList()) {
      checkTypeAccess(
          P->getInterfaceType(), P->getTypeRepr(), EED, /*mayBeInferred*/ false,
          [&](AccessScope typeAccessScope, const TypeRepr *complainRepr,
              DowngradeToWarning downgradeToWarning) {
            auto diagID = diag::enum_case_usable_from_inline;
            auto &Context = EED->getASTContext();
            if (Context.LangOpts.EnableAccessControlHacks &&
                !Context.isSwiftVersionAtLeast(5))
              diagID = diag::enum_case_usable_from_inline_warn;
            auto diag = EED->diagnose(diagID);
            highlightOffendingType(diag, complainRepr);
          });
    }
  }
};
} // end anonymous namespace

/// Returns the kind of origin, implementation-only import or SPI declaration,
/// that restricts exporting \p decl from the given file and context.
///
/// Local variant to swift::getDisallowedOriginKind for downgrade to warnings.
DisallowedOriginKind
swift::getDisallowedOriginKind(const Decl *decl,
                               const ExportContext &where,
                               DowngradeToWarning &downgradeToWarning) {
  downgradeToWarning = DowngradeToWarning::No;
  ModuleDecl *M = decl->getModuleContext();
  auto *SF = where.getDeclContext()->getParentSourceFile();
  if (SF->isImportedImplementationOnly(M)) {
    // Temporarily downgrade implementation-only exportability in SPI to
    // a warning.
    if (where.isSPI())
      downgradeToWarning = DowngradeToWarning::Yes;

    // Even if the current module is @_implementationOnly, Swift should
    // not report an error in the cases where the decl is also exported from
    // a non @_implementationOnly module. Thus, we check to see if there is
    // a visible access path to the Clang decl, and only error out in case
    // there is none.
    auto filter = ModuleDecl::ImportFilter(
        {ModuleDecl::ImportFilterKind::Exported,
         ModuleDecl::ImportFilterKind::Default,
         ModuleDecl::ImportFilterKind::SPIAccessControl,
         ModuleDecl::ImportFilterKind::ShadowedByCrossImportOverlay});
    SmallVector<ImportedModule, 4> sfImportedModules;
    SF->getImportedModules(sfImportedModules, filter);
    if (auto clangDecl = decl->getClangDecl()) {
      for (auto redecl : clangDecl->redecls()) {
        if (auto tagReDecl = dyn_cast<clang::TagDecl>(redecl)) {
          // This is a forward declaration. We ignore visibility of those.
          if (tagReDecl->getBraceRange().isInvalid()) {
            continue;
          }
        }
        auto moduleWrapper =
            decl->getASTContext().getClangModuleLoader()->getWrapperForModule(
                redecl->getOwningModule());
        auto visibleAccessPath =
            find_if(sfImportedModules, [&moduleWrapper](auto importedModule) {
              return importedModule.importedModule == moduleWrapper ||
                     !importedModule.importedModule
                          ->isImportedImplementationOnly(moduleWrapper);
            });
        if (visibleAccessPath != sfImportedModules.end()) {
          return DisallowedOriginKind::None;
        }
      }
    }
    // Implementation-only imported, cannot be reexported.
    return DisallowedOriginKind::ImplementationOnly;
  } else if (decl->isSPI() && !where.isSPI()) {
    // SPI can only be exported in SPI.
    return where.getDeclContext()->getParentModule() == M ?
      DisallowedOriginKind::SPILocal :
      DisallowedOriginKind::SPIImported;
  }

  return DisallowedOriginKind::None;
};

namespace {

/// Diagnose declarations whose signatures refer to unavailable types.
class DeclAvailabilityChecker : public DeclVisitor<DeclAvailabilityChecker> {
  ExportContext Where;

  void checkType(Type type, const TypeRepr *typeRepr, const Decl *context,
                 ExportabilityReason reason=ExportabilityReason::General,
                 bool allowUnavailableProtocol=false) {
    // Don't bother checking errors.
    if (type && type->hasError())
      return;

    DeclAvailabilityFlags flags = None;

    // We allow a type to conform to a protocol that is less available than
    // the type itself. This enables a type to retroactively model or directly
    // conform to a protocol only available on newer OSes and yet still be used on
    // older OSes.
    //
    // To support this, inside inheritance clauses we allow references to
    // protocols that are unavailable in the current type refinement context.
    if (allowUnavailableProtocol)
      flags |= DeclAvailabilityFlag::AllowPotentiallyUnavailableProtocol;

    diagnoseTypeAvailability(typeRepr, type, context->getLoc(),
                             Where.withReason(reason), flags);
  }

  void checkGenericParams(const GenericContext *ownerCtx,
                          const ValueDecl *ownerDecl) {
    if (!ownerCtx->isGenericContext())
      return;

    if (auto params = ownerCtx->getGenericParams()) {
      for (auto param : *params) {
        if (param->getInherited().empty())
          continue;
        assert(param->getInherited().size() == 1);
        auto inherited = param->getInherited().front();
        checkType(inherited.getType(), inherited.getTypeRepr(), ownerDecl);
      }
    }

    if (ownerCtx->getTrailingWhereClause()) {
      forAllRequirementTypes(WhereClauseOwner(
                               const_cast<GenericContext *>(ownerCtx)),
                             [&](Type type, TypeRepr *typeRepr) {
        checkType(type, typeRepr, ownerDecl);
      });
    }
  }

public:
  explicit DeclAvailabilityChecker(ExportContext where)
    : Where(where) {}

  // Force all kinds to be handled at a lower level.
  void visitDecl(Decl *D) = delete;
  void visitValueDecl(ValueDecl *D) = delete;

#define UNREACHABLE(KIND, REASON) \
  void visit##KIND##Decl(KIND##Decl *D) { \
    llvm_unreachable(REASON); \
  }
  UNREACHABLE(Import, "not applicable")
  UNREACHABLE(TopLevelCode, "not applicable")
  UNREACHABLE(Module, "not applicable")

  UNREACHABLE(Param, "handled by the enclosing declaration")
  UNREACHABLE(GenericTypeParam, "handled by the enclosing declaration")
  UNREACHABLE(MissingMember, "handled by the enclosing declaration")
#undef UNREACHABLE

#define UNINTERESTING(KIND) \
  void visit##KIND##Decl(KIND##Decl *D) {}

  UNINTERESTING(PrefixOperator) // Does not reference other decls.
  UNINTERESTING(PostfixOperator) // Does not reference other decls.
  UNINTERESTING(IfConfig) // Not applicable.
  UNINTERESTING(PoundDiagnostic) // Not applicable.
  UNINTERESTING(EnumCase) // Handled at the EnumElement level.
  UNINTERESTING(Destructor) // Always correct.
  UNINTERESTING(Accessor) // Handled by the Var or Subscript.
  UNINTERESTING(OpaqueType) // TODO

  // Handled at the PatternBinding level; if the pattern has a simple
  // "name: TheType" form, we can get better results by diagnosing the TypeRepr.
  UNINTERESTING(Var)

  /// \see visitPatternBindingDecl
  void checkNamedPattern(const NamedPattern *NP,
                         const llvm::DenseSet<const VarDecl *> &seenVars) {
    const VarDecl *theVar = NP->getDecl();

    // Only check the type of individual variables if we didn't check an
    // enclosing TypedPattern.
    if (seenVars.count(theVar))
      return;

    checkType(theVar->getValueInterfaceType(), /*typeRepr*/nullptr, theVar);
  }

  /// \see visitPatternBindingDecl
  void checkTypedPattern(PatternBindingDecl *PBD,
                         const TypedPattern *TP,
                         llvm::DenseSet<const VarDecl *> &seenVars) {
    // FIXME: We need to figure out if this is a stored or computed property,
    // so we pull out some random VarDecl in the pattern. They're all going to
    // be the same, but still, ick.
    const VarDecl *anyVar = nullptr;
    TP->forEachVariable([&](VarDecl *V) {
      seenVars.insert(V);
      anyVar = V;
    });

    checkType(TP->hasType() ? TP->getType() : Type(),
              TP->getTypeRepr(), anyVar ? (Decl *)anyVar : (Decl *)PBD);

    // Check the property wrapper types.
    if (anyVar) {
      for (auto attr : anyVar->getAttachedPropertyWrappers()) {
        checkType(attr->getType(), attr->getTypeRepr(), anyVar,
                  ExportabilityReason::PropertyWrapper);
      }

      if (auto attr = anyVar->getAttachedResultBuilder()) {
        checkType(anyVar->getResultBuilderType(),
                  attr->getTypeRepr(), anyVar,
                  ExportabilityReason::ResultBuilder);
      }
    }
  }

  void visitPatternBindingDecl(PatternBindingDecl *PBD) {
    llvm::DenseSet<const VarDecl *> seenVars;
    for (auto idx : range(PBD->getNumPatternEntries())) {
      PBD->getPattern(idx)->forEachNode([&](const Pattern *P) {
        if (auto *NP = dyn_cast<NamedPattern>(P)) {
          checkNamedPattern(NP, seenVars);
          return;
        }

        auto *TP = dyn_cast<TypedPattern>(P);
        if (!TP)
          return;
        checkTypedPattern(PBD, TP, seenVars);
      });
      seenVars.clear();
    }
  }

  void visitTypeAliasDecl(TypeAliasDecl *TAD) {
    checkGenericParams(TAD, TAD);
    checkType(TAD->getUnderlyingType(),
              TAD->getUnderlyingTypeRepr(), TAD);
  }

  void visitAssociatedTypeDecl(AssociatedTypeDecl *assocType) {
    llvm::for_each(assocType->getInherited(),
                   [&](TypeLoc requirement) {
      checkType(requirement.getType(), requirement.getTypeRepr(),
                assocType);
    });
    checkType(assocType->getDefaultDefinitionType(),
              assocType->getDefaultDefinitionTypeRepr(), assocType);

    if (assocType->getTrailingWhereClause()) {
      forAllRequirementTypes(assocType,
                             [&](Type type, TypeRepr *typeRepr) {
        checkType(type, typeRepr, assocType);
      });
    }
  }

  void visitNominalTypeDecl(const NominalTypeDecl *nominal) {
    checkGenericParams(nominal, nominal);

    llvm::for_each(nominal->getInherited(),
                   [&](TypeLoc inherited) {
      checkType(inherited.getType(), inherited.getTypeRepr(),
                nominal, ExportabilityReason::General,
                /*allowUnavailableProtocol=*/true);
    });
  }

  void visitProtocolDecl(ProtocolDecl *proto) {
    llvm::for_each(proto->getInherited(),
                  [&](TypeLoc requirement) {
      checkType(requirement.getType(), requirement.getTypeRepr(), proto,
                ExportabilityReason::General,
                /*allowUnavailableProtocol=*/false);
    });

    if (proto->getTrailingWhereClause()) {
      forAllRequirementTypes(proto, [&](Type type, TypeRepr *typeRepr) {
        checkType(type, typeRepr, proto);
      });
    }
  }

  void visitSubscriptDecl(SubscriptDecl *SD) {
    checkGenericParams(SD, SD);

    for (auto &P : *SD->getIndices()) {
      checkType(P->getInterfaceType(), P->getTypeRepr(), SD);
    }
    checkType(SD->getElementInterfaceType(), SD->getElementTypeRepr(), SD);
  }

  void visitAbstractFunctionDecl(AbstractFunctionDecl *fn) {
    checkGenericParams(fn, fn);

    for (auto *P : *fn->getParameters()) {
      auto wrapperAttrs = P->getAttachedPropertyWrappers();
      for (auto index : indices(wrapperAttrs)) {
        auto wrapperType = P->getAttachedPropertyWrapperType(index);
        checkType(wrapperType, wrapperAttrs[index]->getTypeRepr(), fn);
      }

      checkType(P->getInterfaceType(), P->getTypeRepr(), fn);
    }
  }

  void visitFuncDecl(FuncDecl *FD) {
    visitAbstractFunctionDecl(FD);
    checkType(FD->getResultInterfaceType(), FD->getResultTypeRepr(), FD);

    if (auto attr = FD->getAttachedResultBuilder()) {
      checkType(FD->getResultBuilderType(),
                attr->getTypeRepr(), FD,
                ExportabilityReason::ResultBuilder);
    }
  }

  void visitEnumElementDecl(EnumElementDecl *EED) {
    if (!EED->hasAssociatedValues())
      return;
    for (auto &P : *EED->getParameterList())
      checkType(P->getInterfaceType(), P->getTypeRepr(), EED);
  }

  void checkConstrainedExtensionRequirements(ExtensionDecl *ED,
                                             bool hasExportedMembers) {
    if (!ED->getTrailingWhereClause())
      return;

    ExportabilityReason reason =
        hasExportedMembers ? ExportabilityReason::ExtensionWithPublicMembers
                           : ExportabilityReason::ExtensionWithConditionalConformances;

    forAllRequirementTypes(ED, [&](Type type, TypeRepr *typeRepr) {
      checkType(type, typeRepr, ED, reason);
    });
  }

  void visitExtensionDecl(ExtensionDecl *ED) {
    auto extendedType = ED->getExtendedNominal();
    assert(extendedType && "valid extension with no extended type?");
    if (!extendedType)
      return;

    // The rules here are tricky.
    //
    // 1) If the extension defines conformances, the conformed-to protocols
    // must be exported.
    llvm::for_each(ED->getInherited(),
                   [&](TypeLoc inherited) {
      checkType(inherited.getType(), inherited.getTypeRepr(),
                ED, ExportabilityReason::General,
                /*allowUnavailableProtocol=*/true);
    });

    auto wasWhere = Where;

    // 2) If the extension contains exported members, the as-written
    // extended type should be exportable.
    bool hasExportedMembers = llvm::any_of(ED->getMembers(),
                                           [](const Decl *member) -> bool {
      auto *valueMember = dyn_cast<ValueDecl>(member);
      if (!valueMember)
        return false;
      return isExported(valueMember);
    });

    Where = wasWhere.withExported(hasExportedMembers);
    checkType(ED->getExtendedType(), ED->getExtendedTypeRepr(), ED,
              ExportabilityReason::ExtensionWithPublicMembers);

    // 3) If the extension contains exported members or defines conformances,
    // the 'where' clause must only name exported types.
    Where = wasWhere.withExported(hasExportedMembers ||
                                  !ED->getInherited().empty());
    checkConstrainedExtensionRequirements(ED, hasExportedMembers);
  }

  void checkPrecedenceGroup(const PrecedenceGroupDecl *PGD,
                            const Decl *refDecl, SourceLoc diagLoc,
                            SourceRange refRange) {
    // Bail on invalid predence groups. This can happen when the user spells a
    // relation element that doesn't actually exist.
    if (!PGD) {
      return;
    }

    const SourceFile *SF = refDecl->getDeclContext()->getParentSourceFile();
    ModuleDecl *M = PGD->getModuleContext();
    if (!SF->isImportedImplementationOnly(M))
      return;

    auto &DE = PGD->getASTContext().Diags;
    auto diag =
        DE.diagnose(diagLoc, diag::decl_from_hidden_module,
                    PGD->getDescriptiveKind(), PGD->getName(),
                    static_cast<unsigned>(ExportabilityReason::General), M->getName(),
                    static_cast<unsigned>(DisallowedOriginKind::ImplementationOnly)
                    );
    if (refRange.isValid())
      diag.highlight(refRange);
    diag.flush();
    PGD->diagnose(diag::decl_declared_here, PGD->getName());
  }

  void visitInfixOperatorDecl(InfixOperatorDecl *IOD) {
    // FIXME: Handle operator designated types (which also applies to prefix
    // and postfix operators).
    if (auto *precedenceGroup = IOD->getPrecedenceGroup()) {
      if (!IOD->getIdentifiers().empty()) {
        checkPrecedenceGroup(precedenceGroup, IOD, IOD->getLoc(),
                             IOD->getIdentifiers().front().Loc);
      }
    }
  }

  void visitPrecedenceGroupDecl(PrecedenceGroupDecl *PGD) {
    llvm::for_each(PGD->getLowerThan(),
                   [&](const PrecedenceGroupDecl::Relation &relation) {
      checkPrecedenceGroup(relation.Group, PGD, PGD->getLowerThanLoc(),
                           relation.NameLoc);
    });
    llvm::for_each(PGD->getHigherThan(),
                   [&](const PrecedenceGroupDecl::Relation &relation) {
      checkPrecedenceGroup(relation.Group, PGD, PGD->getHigherThanLoc(),
                           relation.NameLoc);
    });
  }
};

} // end anonymous namespace

static void checkExtensionGenericParamAccess(const ExtensionDecl *ED) {
  auto *AA = ED->getAttrs().getAttribute<AccessControlAttr>();
  if (!AA)
    return;
  AccessLevel userSpecifiedAccess = AA->getAccess();

  AccessScope desiredAccessScope = AccessScope::getPublic();
  switch (userSpecifiedAccess) {
  case AccessLevel::Private:
    assert((ED->isInvalid() ||
            ED->getDeclContext()->isModuleScopeContext()) &&
           "non-top-level extensions make 'private' != 'fileprivate'");
    LLVM_FALLTHROUGH;
  case AccessLevel::FilePrivate: {
    const DeclContext *DC = ED->getModuleScopeContext();
    bool isPrivate = (userSpecifiedAccess == AccessLevel::Private);
    desiredAccessScope = AccessScope(DC, isPrivate);
    break;
  }
  case AccessLevel::Internal:
    desiredAccessScope = AccessScope(ED->getModuleContext());
    break;
  case AccessLevel::Public:
  case AccessLevel::Open:
    break;
  }

  AccessControlChecker().checkGenericParamAccess(
      ED, ED, desiredAccessScope, userSpecifiedAccess);
}

DisallowedOriginKind swift::getDisallowedOriginKind(const Decl *decl,
                                                    const ExportContext &where) {
  auto downgradeToWarning = DowngradeToWarning::No;
  return getDisallowedOriginKind(decl, where, downgradeToWarning);
}

void swift::checkAccessControl(Decl *D) {
  if (isa<ValueDecl>(D) || isa<PatternBindingDecl>(D)) {
    bool allowInlineable =
        D->getDeclContext()->isInSpecializeExtensionContext();
    AccessControlChecker(allowInlineable).visit(D);
    UsableFromInlineChecker().visit(D);
  } else if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
    checkExtensionGenericParamAccess(ED);
  }

  if (isa<AccessorDecl>(D))
    return;

  auto where = ExportContext::forDeclSignature(D);
  if (where.isImplicit())
    return;

  DeclAvailabilityChecker(where).visit(D);
}
