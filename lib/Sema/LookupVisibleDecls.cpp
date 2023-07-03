//===--- LookupVisibleDecls - Swift Name Lookup Routines ------------------===//
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
// This file implements the lookupVisibleDecls interface for visiting named
// declarations.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "clang/AST/DeclObjC.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/ModuleNameLookup.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/SourceFile.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Sema/IDETypeCheckingRequests.h"
#include "swift/Sema/IDETypeChecking.h"
#include "llvm/ADT/SetVector.h"
#include <set>

using namespace swift;

namespace {
struct LookupState {
private:
  /// If \c false, an unqualified lookup of all visible decls in a
  /// DeclContext.
  ///
  /// If \c true, lookup of all visible members of a given object (possibly of
  /// metatype type).
  unsigned IsQualified : 1;

  /// Is this a qualified lookup on a metatype?
  unsigned IsOnMetatype : 1;

  /// Did we recurse into a superclass?
  unsigned IsOnSuperclass : 1;

  unsigned InheritsSuperclassInitializers : 1;

  /// Should instance members be included even if lookup is performed on a type?
  unsigned IncludeInstanceMembers : 1;

  /// Should derived protocol requirements be included?
  /// This option is only for override completion lookup.
  unsigned IncludeDerivedRequirements : 1;

  /// Should protocol extension members be included?
  unsigned IncludeProtocolExtensionMembers : 1;

  LookupState()
      : IsQualified(0), IsOnMetatype(0), IsOnSuperclass(0),
        InheritsSuperclassInitializers(0), IncludeInstanceMembers(0),
        IncludeDerivedRequirements(0), IncludeProtocolExtensionMembers(0) {}

public:
  static LookupState makeQualified() {
    LookupState Result;
    Result.IsQualified = 1;
    return Result;
  }

  static LookupState makeUnqualified() {
    LookupState Result;
    Result.IsQualified = 0;
    return Result;
  }

  bool isQualified() const { return IsQualified; }
  bool isOnMetatype() const { return IsOnMetatype; }
  bool isOnSuperclass() const { return IsOnSuperclass; }
  bool isInheritsSuperclassInitializers() const {
    return InheritsSuperclassInitializers;
  }
  bool isIncludingInstanceMembers() const { return IncludeInstanceMembers; }
  bool isIncludingDerivedRequirements() const {
    return IncludeDerivedRequirements;
  }
  bool isIncludingProtocolExtensionMembers() const {
    return IncludeProtocolExtensionMembers;
  }

  LookupState withOnMetatype() const {
    auto Result = *this;
    Result.IsOnMetatype = 1;
    return Result;
  }

  LookupState withOnSuperclass() const {
    auto Result = *this;
    Result.IsOnSuperclass = 1;
    return Result;
  }

  LookupState withInheritsSuperclassInitializers() const {
    auto Result = *this;
    Result.InheritsSuperclassInitializers = 1;
    return Result;
  }

  LookupState withoutInheritsSuperclassInitializers() const {
    auto Result = *this;
    Result.InheritsSuperclassInitializers = 0;
    return Result;
  }

  LookupState withIncludedInstanceMembers() const {
    auto Result = *this;
    Result.IncludeInstanceMembers = 1;
    return Result;
  }

  LookupState withIncludedDerivedRequirements() const {
    auto Result = *this;
    Result.IncludeDerivedRequirements = 1;
    return Result;
  }

  LookupState withIncludeProtocolExtensionMembers() const {
    auto Result = *this;
    Result.IncludeProtocolExtensionMembers = 1;
    return Result;
  }
};
} // end anonymous namespace

static bool areTypeDeclsVisibleInLookupMode(LookupState LS) {
  // Nested type declarations can be accessed only with unqualified lookup or
  // on metatypes.
  return !LS.isQualified() || LS.isOnMetatype();
}

static bool isDeclVisibleInLookupMode(ValueDecl *Member, LookupState LS,
                                      const DeclContext *FromContext) {
  // Accessors are never visible directly in the source language.
  if (isa<AccessorDecl>(Member))
    return false;
  
  // Check access when relevant.
  if (!Member->getDeclContext()->isLocalContext() &&
      !isa<GenericTypeParamDecl>(Member) && !isa<ParamDecl>(Member)) {
    if (!Member->isAccessibleFrom(FromContext))
      return false;
  }

  if (auto *FD = dyn_cast<FuncDecl>(Member)) {
    // Cannot call static functions on non-metatypes.
    if (!LS.isOnMetatype() && FD->isStatic())
      return false;

    // Otherwise, either call a function or curry it.
    return true;
  }
  if (auto *SD = dyn_cast<SubscriptDecl>(Member)) {
    // Cannot use static subscripts on non-metatypes.
    if (!LS.isOnMetatype() && SD->isStatic())
      return false;

    // Cannot use instance subscript on metatypes.
    if (LS.isOnMetatype() && !SD->isStatic() && !LS.isIncludingInstanceMembers())
      return false;

    return true;
  }
  if (auto *VD = dyn_cast<VarDecl>(Member)) {
    // Cannot use static properties on non-metatypes.
    if (!LS.isOnMetatype() && VD->isStatic())
      return false;

    // Cannot use instance properties on metatypes.
    if (LS.isOnMetatype() && !VD->isStatic() && !LS.isIncludingInstanceMembers())
      return false;

    return true;
  }
  if (isa<EnumElementDecl>(Member)) {
    // Cannot reference enum elements on non-metatypes.
    if (!LS.isOnMetatype())
      return false;
  }
  if (auto CD = dyn_cast<ConstructorDecl>(Member)) {
    if (!LS.isQualified())
      return false;
    // Constructors with stub implementations cannot be called in Swift.
    if (CD->hasStubImplementation())
      return false;
    if (LS.isOnSuperclass()) {
      // Cannot call initializers from a superclass, except for inherited
      // convenience initializers.
      return LS.isInheritsSuperclassInitializers() && CD->isInheritable();
    }
  }
  if (isa<TypeDecl>(Member))
    return areTypeDeclsVisibleInLookupMode(LS);

  return true;
}

/// Collect visible members from \p Parent into \p FoundDecls .
static void collectVisibleMemberDecls(const DeclContext *CurrDC, LookupState LS,
                                      Type BaseType,
                                      IterableDeclContext *Parent,
                                      SmallVectorImpl<ValueDecl *> &FoundDecls) {
  auto check = [&](Decl *decl) {
    auto *VD = dyn_cast<ValueDecl>(decl);
    if (!VD)
      return;
    if (!isDeclVisibleInLookupMode(VD, LS, CurrDC))
      return;
    if (!evaluateOrDefault(CurrDC->getASTContext().evaluator,
        IsDeclApplicableRequest(DeclApplicabilityOwner(CurrDC, BaseType, VD)),
                           false))
      return;
    FoundDecls.push_back(VD);
  };

  for (auto Member : Parent->getAllMembers()) {
    check(Member);
    Member->visitAuxiliaryDecls([&](Decl *d) {
      check(d);
    });
  }
}

static void
synthesizePropertyWrapperVariables(IterableDeclContext *IDC);

/// Lookup members in extensions of \p LookupType, using \p BaseType as the
/// underlying type when checking any constraints on the extensions.
static void doGlobalExtensionLookup(Type BaseType,
                                    Type LookupType,
                                    SmallVectorImpl<ValueDecl *> &FoundDecls,
                                    const DeclContext *CurrDC,
                                    LookupState LS,
                                    DeclVisibilityKind Reason) {
  auto nominal = LookupType->getAnyNominal();

  // Look in each extension of this type.
  for (auto extension : nominal->getExtensions()) {
    if (!evaluateOrDefault(CurrDC->getASTContext().evaluator,
        IsDeclApplicableRequest(DeclApplicabilityOwner(CurrDC, BaseType,
                                                       extension)), false))
      continue;

    collectVisibleMemberDecls(CurrDC, LS, BaseType, extension, FoundDecls);
  }

  // Handle shadowing.
  removeShadowedDecls(FoundDecls, CurrDC);
}

/// Enumerate immediate members of the type \c LookupType and its
/// extensions, as seen from the context \c CurrDC.
///
/// Don't do lookup into superclasses or implemented protocols.  Uses
/// \p BaseType as the underlying type when checking any constraints on the
/// extensions.
static void lookupTypeMembers(Type BaseType, Type LookupType,
                              VisibleDeclConsumer &Consumer,
                              const DeclContext *CurrDC, LookupState LS,
                              DeclVisibilityKind Reason) {
  NominalTypeDecl *D = LookupType->getAnyNominal();
  assert(D && "should have a nominal type");

  Consumer.onLookupNominalTypeMembers(D, Reason);

  SmallVector<ValueDecl*, 2> FoundDecls;
  collectVisibleMemberDecls(CurrDC, LS, BaseType, D, FoundDecls);

  doGlobalExtensionLookup(BaseType, LookupType, FoundDecls, CurrDC, LS, Reason);

  // Report the declarations we found to the consumer.
  for (auto *VD : FoundDecls)
    Consumer.foundDecl(VD, Reason);
}

/// Enumerate AnyObject declarations as seen from context \c CurrDC.
static void doDynamicLookup(VisibleDeclConsumer &Consumer,
                            const DeclContext *CurrDC,
                            LookupState LS) {
  class DynamicLookupConsumer : public VisibleDeclConsumer {
    VisibleDeclConsumer &ChainedConsumer;
    LookupState LS;
    const DeclContext *CurrDC;
    llvm::DenseSet<std::pair<DeclBaseName, CanType>> FunctionsReported;
    llvm::DenseSet<CanType> SubscriptsReported;
    llvm::DenseSet<std::pair<Identifier, CanType>> PropertiesReported;

  public:
    explicit DynamicLookupConsumer(VisibleDeclConsumer &ChainedConsumer,
                                   LookupState LS, const DeclContext *CurrDC)
        : ChainedConsumer(ChainedConsumer), LS(LS), CurrDC(CurrDC) {}

    void foundDecl(ValueDecl *D, DeclVisibilityKind Reason,
                   DynamicLookupInfo) override {
      // If the declaration has an override, name lookup will also have found
      // the overridden method.  Skip this declaration, because we prefer the
      // overridden method.
      if (D->getOverriddenDecl())
        return;

      // If the declaration is not @objc, it cannot be called dynamically.
      if (!D->isObjC())
        return;

      // If the declaration is objc_direct, it cannot be called dynamically.
      if (auto clangDecl = D->getClangDecl()) {
        if (auto objCMethod = dyn_cast<clang::ObjCMethodDecl>(clangDecl)) {
          if (objCMethod->isDirectMethod())
            return;
        } else if (auto objCProperty = dyn_cast<clang::ObjCPropertyDecl>(clangDecl)) {
          if (objCProperty->isDirectProperty())
            return;
        }
      }

      if (D->isRecursiveValidation())
        return;

      switch (D->getKind()) {
#define DECL(ID, SUPER) \
      case DeclKind::ID:
#define VALUE_DECL(ID, SUPER)
#include "swift/AST/DeclNodes.def"
        llvm_unreachable("not a ValueDecl!");

      // Types cannot be found by dynamic lookup.
      case DeclKind::GenericTypeParam:
      case DeclKind::AssociatedType:
      case DeclKind::TypeAlias:
      case DeclKind::Enum:
      case DeclKind::Class:
      case DeclKind::Struct:
      case DeclKind::Protocol:
      case DeclKind::OpaqueType:
      case DeclKind::BuiltinTuple:
        return;

      // Macros cannot be found by dynamic lookup.
      case DeclKind::Macro:
        return;

      // Initializers cannot be found by dynamic lookup.
      case DeclKind::Constructor:
      case DeclKind::Destructor:
        return;

      // These cases are probably impossible here but can also just
      // be safely ignored.
      case DeclKind::Param:
      case DeclKind::Module:
      case DeclKind::EnumElement:
        return;

      // For other kinds of values, check if we already reported a decl
      // with the same signature.

      case DeclKind::Accessor:
      case DeclKind::Func: {
        auto FD = cast<FuncDecl>(D);
        assert(FD->hasImplicitSelfDecl() && "should not find free functions");
        (void)FD;

        if (FD->isInvalid())
          break;

        // Get the type without the first uncurry level with 'self'.
        CanType T = FD->getMethodInterfaceType()->getCanonicalType();

        auto Signature = std::make_pair(D->getBaseName(), T);
        if (!FunctionsReported.insert(Signature).second)
          return;
        break;
      }

      case DeclKind::Subscript: {
        auto Signature = D->getInterfaceType()->getCanonicalType();
        if (!SubscriptsReported.insert(Signature).second)
          return;
        break;
      }

      case DeclKind::Var: {
        auto *VD = cast<VarDecl>(D);
        auto Signature =
            std::make_pair(VD->getName(),
                           VD->getInterfaceType()->getCanonicalType());
        if (!PropertiesReported.insert(Signature).second)
          return;
        break;
      }
      }

      if (isDeclVisibleInLookupMode(D, LS, CurrDC))
        ChainedConsumer.foundDecl(D, DeclVisibilityKind::DynamicLookup,
                                  DynamicLookupInfo::AnyObject);
    }
  };

  DynamicLookupConsumer ConsumerWrapper(Consumer, LS, CurrDC);

  for (auto Import : namelookup::getAllImports(CurrDC)) {
    Import.importedModule->lookupClassMembers(Import.accessPath,
                                              ConsumerWrapper);
  }
}

namespace {
  typedef llvm::SmallPtrSet<const TypeDecl *, 8> VisitedSet;
} // end anonymous namespace

static DeclVisibilityKind getReasonForSuper(DeclVisibilityKind Reason) {
  switch (Reason) {
  case DeclVisibilityKind::MemberOfCurrentNominal:
  case DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal:
  case DeclVisibilityKind::MemberOfSuper:
    return DeclVisibilityKind::MemberOfSuper;

  case DeclVisibilityKind::MemberOfOutsideNominal:
    return DeclVisibilityKind::MemberOfOutsideNominal;

  default:
    llvm_unreachable("should not see this kind");
  }
}

static void lookupDeclsFromProtocolsBeingConformedTo(
    Type BaseTy, VisibleDeclConsumer &Consumer, LookupState LS,
    const DeclContext *FromContext, DeclVisibilityKind Reason,
    VisitedSet &Visited) {
  NominalTypeDecl *CurrNominal = BaseTy->getAnyNominal();
  if (!CurrNominal)
    return;
  ModuleDecl *Module = FromContext->getParentModule();

  for (auto Conformance : CurrNominal->getAllConformances()) {
    auto Proto = Conformance->getProtocol();
    if (!Proto->isAccessibleFrom(FromContext))
      continue;

    // Skip unsatisfied conditional conformances.
    // We can't check them if this type has an UnboundGenericType or if they
    // couldn't be computed, so assume they conform in such cases.
    if (!BaseTy->hasUnboundGenericType()) {
      if (auto res = Conformance->getConditionalRequirementsIfAvailable()) {
        if (!res->empty() && !Module->conformsToProtocol(BaseTy, Proto))
          continue;
      }
    }

    DeclVisibilityKind ReasonForThisProtocol;
    if (Conformance->getKind() == ProtocolConformanceKind::Inherited)
      ReasonForThisProtocol = getReasonForSuper(Reason);
    else if (Reason == DeclVisibilityKind::MemberOfCurrentNominal)
      ReasonForThisProtocol =
          DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal;
    else
      ReasonForThisProtocol = getReasonForSuper(Reason);

    if (auto NormalConformance = dyn_cast<NormalProtocolConformance>(
          Conformance->getRootConformance())) {

      Consumer.onLookupNominalTypeMembers(Proto, ReasonForThisProtocol);

      for (auto Member : Proto->getMembers()) {
        // Skip associated types and value requirements that aren't visible
        // or have a corresponding witness.
        if (auto *ATD = dyn_cast<AssociatedTypeDecl>(Member)) {
          if (areTypeDeclsVisibleInLookupMode(LS) &&
              !Conformance->hasTypeWitness(ATD)) {
            Consumer.foundDecl(ATD, ReasonForThisProtocol);
          }
        } else if (auto *VD = dyn_cast<ValueDecl>(Member)) {
          if (!isDeclVisibleInLookupMode(VD, LS, FromContext))
            continue;

          if (isa<TypeAliasDecl>(VD)) {
            // Typealias declarations of the protocol are always visible in
            // types that inherits from it.
            Consumer.foundDecl(VD, ReasonForThisProtocol);
            continue;
          }
          if (!VD->isProtocolRequirement())
            continue;

          // Whether the given witness corresponds to a derived requirement.
          const auto isDerivedRequirement = [Proto](const ValueDecl *Witness) {
            return Witness->isImplicit() &&
                Proto->getKnownDerivableProtocolKind();
          };
          DeclVisibilityKind ReasonForThisDecl = ReasonForThisProtocol;
          if (const auto Witness = NormalConformance->getWitness(VD)) {
            auto *WD = Witness.getDecl();
            if (WD->getName() == VD->getName()) {
              if (LS.isIncludingDerivedRequirements() &&
                  Reason == DeclVisibilityKind::MemberOfCurrentNominal &&
                  isDerivedRequirement(WD)) {
                ReasonForThisDecl =
                    DeclVisibilityKind::MemberOfProtocolDerivedByCurrentNominal;
              } else if (!LS.isIncludingProtocolExtensionMembers() &&
                         WD->getDeclContext()->getExtendedProtocolDecl()) {
                // Don't skip this requirement.
                // Witnesses in protocol extensions aren't reported.
              } else {
                // lookupVisibleMemberDecls() generally prefers witness members
                // over requirements.
                continue;
              }
            }
          }

          Consumer.foundDecl(VD, ReasonForThisDecl);
        }
      }
    }

    // Add members from any extensions.
    if (LS.isIncludingProtocolExtensionMembers()) {
      SmallVector<ValueDecl *, 2> FoundDecls;
      doGlobalExtensionLookup(BaseTy, Proto->getDeclaredInterfaceType(),
                              FoundDecls, FromContext, LS,
                              ReasonForThisProtocol);
      for (auto *VD : FoundDecls)
        Consumer.foundDecl(VD, ReasonForThisProtocol);
    }
  }
}

static void
lookupVisibleMemberDeclsImpl(Type BaseTy, VisibleDeclConsumer &Consumer,
                             const DeclContext *CurrDC, LookupState LS,
                             DeclVisibilityKind Reason,
                             GenericSignature Sig,
                             VisitedSet &Visited);

static void
  lookupVisibleProtocolMemberDecls(Type BaseTy, const ProtocolDecl *PD,
                                   VisibleDeclConsumer &Consumer,
                                   const DeclContext *CurrDC, LookupState LS,
                                   DeclVisibilityKind Reason,
                                   GenericSignature Sig,
                                   VisitedSet &Visited) {
  if (!Visited.insert(PD).second)
    return;

  lookupTypeMembers(BaseTy, PD->getDeclaredInterfaceType(), Consumer, CurrDC,
                    LS, Reason);

  // Collect members from the inherited protocols.
  for (auto Proto : PD->getInheritedProtocols())
    lookupVisibleProtocolMemberDecls(BaseTy, Proto, Consumer, CurrDC, LS,
                                     getReasonForSuper(Reason), Sig, Visited);
}

// Generate '$' and '_' prefixed variables for members that have attached property
// wrappers.
static void
synthesizePropertyWrapperVariables(IterableDeclContext *IDC) {
  auto SF = IDC->getAsGenericContext()->getParentSourceFile();
  if (!SF || SF->Kind == SourceFileKind::Interface)
    return;

  for (auto Member : IDC->getMembers())
    if (auto var = dyn_cast<VarDecl>(Member))
      if (var->hasAttachedPropertyWrapper()) {
        (void)var->getPropertyWrapperAuxiliaryVariables();
        (void)var->getPropertyWrapperInitializerInfo();
      }
}

static void lookupVisibleMemberDeclsImpl(
    Type BaseTy, VisibleDeclConsumer &Consumer, const DeclContext *CurrDC,
    LookupState LS, DeclVisibilityKind Reason, GenericSignature Sig,
    VisitedSet &Visited) {
  // Just look through l-valueness.  It doesn't affect name lookup.
  assert(BaseTy && "lookup into null type");
  assert(!BaseTy->hasLValueType());

  // Handle metatype references, as in "some_type.some_member".  These are
  // special and can't have extensions.
  if (auto MTT = BaseTy->getAs<AnyMetatypeType>()) {
    // The metatype represents an arbitrary named type: dig through to the
    // declared type to see what we're dealing with.
    Type Ty = MTT->getInstanceType();
    if (Ty->is<AnyMetatypeType>())
      return;

    LookupState subLS = LookupState::makeQualified().withOnMetatype();
    if (LS.isIncludingInstanceMembers()) {
      subLS = subLS.withIncludedInstanceMembers();
    }
    if (LS.isIncludingDerivedRequirements()) {
      subLS = subLS.withIncludedDerivedRequirements();
    }
    if (LS.isIncludingProtocolExtensionMembers()) {
      subLS = subLS.withIncludeProtocolExtensionMembers();
    }

    // Just perform normal dot lookup on the type see if we find extensions or
    // anything else.  For example, type SomeTy.SomeMember can look up static
    // functions, and can even look up non-static functions as well (thus
    // getting the address of the member).
    lookupVisibleMemberDeclsImpl(Ty, Consumer, CurrDC, subLS, Reason,
                                 Sig, Visited);
    return;
  }

  // Lookup module references, as on some_module.some_member.  These are
  // special and can't have extensions.
  if (ModuleType *MT = BaseTy->getAs<ModuleType>()) {
    AccessFilteringDeclConsumer FilteringConsumer(CurrDC, Consumer);
    MT->getModule()->lookupVisibleDecls(ImportPath::Access(),
                                        FilteringConsumer,
                                        NLKind::QualifiedLookup);
    return;
  }

  // If the base is AnyObject, we are doing dynamic lookup.
  if (BaseTy->isAnyObject()) {
    doDynamicLookup(Consumer, CurrDC, LS);
    return;
  }

  // If the base is a protocol, enumerate its members.
  if (ProtocolType *PT = BaseTy->getAs<ProtocolType>()) {
    lookupVisibleProtocolMemberDecls(BaseTy, PT->getDecl(),
                                     Consumer, CurrDC, LS, Reason,
                                     Sig, Visited);
    return;
  }

  // If the base is a protocol composition, enumerate members of the protocols.
  if (auto PC = BaseTy->getAs<ProtocolCompositionType>()) {
    for (auto Member : PC->getMembers())
      lookupVisibleMemberDeclsImpl(Member, Consumer, CurrDC, LS, Reason,
                                   Sig, Visited);
    return;
  }

  if (auto *existential = BaseTy->getAs<ExistentialType>()) {
    auto constraint = existential->getConstraintType();
    lookupVisibleMemberDeclsImpl(constraint, Consumer, CurrDC, LS, Reason,
                                 Sig, Visited);
    return;
  }

  // Enumerate members of archetype's requirements.
  if (ArchetypeType *Archetype = BaseTy->getAs<ArchetypeType>()) {
    for (auto Proto : Archetype->getConformsTo())
      lookupVisibleProtocolMemberDecls(
          BaseTy, Proto, Consumer, CurrDC, LS,
          Reason, Sig, Visited);

    if (auto superclass = Archetype->getSuperclass())
      lookupVisibleMemberDeclsImpl(superclass, Consumer, CurrDC, LS,
                                   Reason, Sig, Visited);
    return;
  }

  // If we're looking into a type parameter and we have a GenericSignature,
  // query the signature to resolve where we should look.
  if (BaseTy->isTypeParameter() && Sig) {
    // The type might be fully concrete via a same-type requirement.
    if (auto ConcreteTy = Sig->getConcreteType(BaseTy)) {
      BaseTy = ConcreteTy;
    } else {
      // Look into protocols of conformance requirements
      for (const auto *Proto : Sig->getRequiredProtocols(BaseTy)) {
        lookupVisibleProtocolMemberDecls(
            BaseTy, Proto, Consumer, CurrDC,
            LS, getReasonForSuper(Reason), Sig, Visited);
      }

      // Look into the superclass requirement type, if there is one.
      if (auto SuperclassTy = Sig->getSuperclassBound(BaseTy)) {
        lookupVisibleMemberDeclsImpl(SuperclassTy, Consumer, CurrDC,
                                     LS, getReasonForSuper(Reason),
                                     Sig, Visited);
      }

      return;
    }
  }

  // The members of a dynamic 'Self' type are the members of its static
  // class type.
  if (auto *const DS = BaseTy->getAs<DynamicSelfType>()) {
    BaseTy = DS->getSelfType();
  }

  auto lookupTy = BaseTy;

  llvm::SmallPtrSet<ClassDecl *, 8> Ancestors;
  {
    const auto NTD = BaseTy->getAnyNominal();
    if (NTD == nullptr)
      return;

    lookupTypeMembers(BaseTy, lookupTy, Consumer, CurrDC, LS, Reason);

    // Look into protocols only on the current nominal to avoid repeatedly
    // visiting inherited conformances.
    lookupDeclsFromProtocolsBeingConformedTo(BaseTy, Consumer, LS, CurrDC,
                                             Reason, Visited);

    const auto CD = dyn_cast<ClassDecl>(NTD);

    // FIXME: We check `getSuperclass()` here because we'll be using the
    // superclass Type below, and in ill-formed code `hasSuperclass()` could
    // be true while `getSuperclass()` returns null, because the latter
    // looks for a declaration.
    if (!CD || !CD->getSuperclass())
      return;

    // We have a superclass; switch state and look into the inheritance chain.
    Ancestors.insert(CD);

    Reason = getReasonForSuper(Reason);
    lookupTy = CD->getSuperclass();

    LS = LS.withOnSuperclass();
    if (CD->inheritsSuperclassInitializers())
      LS = LS.withInheritsSuperclassInitializers();
  }

  // Look into the inheritance chain.
  do {
    const auto CurClass = lookupTy->getClassOrBoundGenericClass();

    // FIXME: This path is no substitute for an actual circularity check.
    // The real fix is to check that the superclass doesn't introduce a
    // circular reference before it's written into the AST.
    if (!Ancestors.insert(CurClass).second)
      break;

    lookupTypeMembers(BaseTy, lookupTy, Consumer, CurrDC, LS, Reason);

    lookupTy = CurClass->getSuperclass();
    if (!CurClass->inheritsSuperclassInitializers())
      LS = LS.withoutInheritsSuperclassInitializers();
  } while (lookupTy);
}

swift::DynamicLookupInfo::DynamicLookupInfo(
    SubscriptDecl *subscript, Type baseType,
    DeclVisibilityKind originalVisibility)
    : kind(KeyPathDynamicMember) {
  keypath.subscript = subscript;
  keypath.baseType = baseType;
  keypath.originalVisibility = originalVisibility;
}

const DynamicLookupInfo::KeyPathDynamicMemberInfo &
swift::DynamicLookupInfo::getKeyPathDynamicMember() const {
  assert(kind == KeyPathDynamicMember);
  return keypath;
}

namespace {

struct FoundDeclTy {
  ValueDecl *D;
  DeclVisibilityKind Reason;
  DynamicLookupInfo dynamicLookupInfo;

  FoundDeclTy(ValueDecl *D, DeclVisibilityKind Reason,
              DynamicLookupInfo dynamicLookupInfo)
      : D(D), Reason(Reason), dynamicLookupInfo(dynamicLookupInfo) {}

  friend bool operator==(const FoundDeclTy &LHS, const FoundDeclTy &RHS) {
    // If this ever changes - e.g. to include Reason - be sure to also update
    // DenseMapInfo<FoundDeclTy>::getHashValue().
    return LHS.D == RHS.D;
  }
};

} // end anonymous namespace

namespace llvm {

template <> struct DenseMapInfo<FoundDeclTy> {
  static inline FoundDeclTy getEmptyKey() {
    return FoundDeclTy{nullptr, DeclVisibilityKind::LocalVariable, {}};
  }

  static inline FoundDeclTy getTombstoneKey() {
    return FoundDeclTy{reinterpret_cast<ValueDecl *>(0x1),
                       DeclVisibilityKind::LocalVariable,
                       {}};
  }

  static unsigned getHashValue(const FoundDeclTy &Val) {
    // Note: FoundDeclTy::operator== only considers D, so don't hash Reason here.
    return llvm::hash_value(Val.D);
  }

  static bool isEqual(const FoundDeclTy &LHS, const FoundDeclTy &RHS) {
    return LHS == RHS;
  }
};

} // namespace llvm

// If a class 'Base' conforms to 'Proto', and my base type is a subclass
// 'Derived' of 'Base', use 'Base' not 'Derived' as the 'Self' type in the
// substitution map.
static Type getBaseTypeForMember(ModuleDecl *M, const ValueDecl *OtherVD,
                                 Type BaseTy) {
  if (auto *Proto = OtherVD->getDeclContext()->getSelfProtocolDecl()) {
    if (BaseTy->getClassOrBoundGenericClass()) {
      if (auto Conformance = M->lookupConformance(BaseTy, Proto)) {
        auto *Superclass = Conformance.getConcrete()
                               ->getRootConformance()
                               ->getType()
                               ->getClassOrBoundGenericClass();
        return BaseTy->getSuperclassForDecl(Superclass);
      }
    }
  }

  return BaseTy;
}

namespace {

class OverrideFilteringConsumer : public VisibleDeclConsumer {
public:
  llvm::SmallVector<std::pair<NominalTypeDecl *, DeclVisibilityKind>, 2>
      nominals;
  llvm::SetVector<FoundDeclTy> Results;
  llvm::SmallVector<ValueDecl *, 8> Decls;
  llvm::SetVector<FoundDeclTy> FilteredResults;
  llvm::DenseMap<DeclBaseName, llvm::SmallVector<ValueDecl *, 2>> DeclsByName;
  Type BaseTy;
  const DeclContext *DC;

  OverrideFilteringConsumer(Type BaseTy, const DeclContext *DC)
      : BaseTy(BaseTy->getMetatypeInstanceType()),
        DC(DC) {
    assert(!BaseTy->hasLValueType());
    assert(DC && BaseTy);
  }

  void onLookupNominalTypeMembers(NominalTypeDecl *NTD,
                                  DeclVisibilityKind Reason) override {
    nominals.emplace_back(NTD, Reason);
  }

  void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason,
                 DynamicLookupInfo dynamicLookupInfo) override {
    if (!Results.insert({VD, Reason, dynamicLookupInfo}))
      return;

    DeclsByName[VD->getBaseName()] = {};
    Decls.push_back(VD);
  }

  void filterDecls(VisibleDeclConsumer &Consumer) {
    for (auto nominal : nominals) {
      Consumer.onLookupNominalTypeMembers(nominal.first, nominal.second);
    }

    removeOverriddenDecls(Decls);
    removeShadowedDecls(Decls, DC);

    size_t index = 0;
    for (const auto &DeclAndReason : Results) {
      if (index >= Decls.size())
        break;
      if (DeclAndReason.D != Decls[index])
        continue;

      ++index;

      auto *const VD = DeclAndReason.D;
      const auto Reason = DeclAndReason.Reason;

      // If this kind of declaration doesn't participate in overriding, there's
      // no filtering to do here.
      if (!isa<AbstractFunctionDecl>(VD) &&
          !isa<AbstractStorageDecl>(VD) &&
          !isa<AssociatedTypeDecl>(VD)) {
        FilteredResults.insert(DeclAndReason);
        continue;
      }

      if (VD->isRecursiveValidation())
        continue;

      auto &PossiblyConflicting = DeclsByName[VD->getBaseName()];

      if (VD->isInvalid()) {
        FilteredResults.insert(DeclAndReason);
        PossiblyConflicting.push_back(VD);
        continue;
      }

      ModuleDecl *M = DC->getParentModule();

      // If the base type is AnyObject, we might be doing a dynamic
      // lookup, so the base type won't match the type of the member's
      // context type.
      //
      // If the base type is not a nominal type, we can't substitute
      // the member type.
      //
      // If the member is a free function and not a member of a type,
      // don't substitute either.
      bool shouldSubst = (Reason != DeclVisibilityKind::DynamicLookup &&
                          !BaseTy->isAnyObject() && !BaseTy->hasTypeVariable() &&
                          !BaseTy->hasUnboundGenericType() &&
                          (BaseTy->getNominalOrBoundGenericNominal() ||
                           BaseTy->is<ArchetypeType>()) &&
                          VD->getDeclContext()->isTypeContext());

      /// Substitute generic parameters in the signature of a found decl. The
      /// returned type can be used to determine if we have already found a
      /// conflicting declaration.
      auto substGenericArgs = [&](CanType SignatureType, ValueDecl *VD,
                                  Type BaseTy) -> CanType {
        if (!SignatureType || !shouldSubst) {
          return SignatureType;
        }
        if (auto GenFuncSignature =
                SignatureType->getAs<GenericFunctionType>()) {
          GenericEnvironment *GenEnv;
          if (auto *GenCtx = VD->getAsGenericContext()) {
            GenEnv = GenCtx->getGenericEnvironment();
          } else {
            GenEnv = DC->getGenericEnvironmentOfContext();
          }
          auto subs = BaseTy->getMemberSubstitutionMap(M, VD, GenEnv);
          auto CT = GenFuncSignature->substGenericArgs(subs);
          if (!CT->hasError()) {
            return CT->getCanonicalType();
          }
        }
        return SignatureType;
      };

      auto FoundSignature = VD->getOverloadSignature();
      auto FoundSignatureType =
          substGenericArgs(VD->getOverloadSignatureType(), VD, BaseTy);

      bool FoundConflicting = false;
      for (auto I = PossiblyConflicting.begin(), E = PossiblyConflicting.end();
           I != E; ++I) {
        auto *const OtherVD = *I;
        if (OtherVD->isRecursiveValidation())
          continue;

        if (OtherVD->isInvalid())
          continue;

        auto OtherSignature = OtherVD->getOverloadSignature();
        auto ActualBaseTy = getBaseTypeForMember(M, OtherVD, BaseTy);
        auto OtherSignatureType = substGenericArgs(
            OtherVD->getOverloadSignatureType(), OtherVD, ActualBaseTy);

        if (conflicting(M->getASTContext(), FoundSignature, FoundSignatureType,
                        OtherSignature, OtherSignatureType,
                        /*wouldConflictInSwift5*/nullptr,
                        /*skipProtocolExtensionCheck*/true)) {
          FoundConflicting = true;

          if (!AvailableAttr::isUnavailable(VD)) {
            bool preferVD = (
                // Prefer derived requirements over their witnesses.
                Reason == DeclVisibilityKind::
                              MemberOfProtocolDerivedByCurrentNominal ||
                // Prefer available one.
                AvailableAttr::isUnavailable(OtherVD) ||
                // Prefer more accessible one.
                VD->getFormalAccess() > OtherVD->getFormalAccess());
            if (preferVD) {
              FilteredResults.remove(
                  FoundDeclTy(OtherVD, DeclVisibilityKind::LocalVariable, {}));
              FilteredResults.insert(DeclAndReason);
              *I = VD;
            }
          }
        }
      }

      if (!FoundConflicting) {
        FilteredResults.insert(DeclAndReason);
        PossiblyConflicting.push_back(VD);
      }
    }

    for (auto Result : FilteredResults)
      Consumer.foundDecl(Result.D, Result.Reason, Result.dynamicLookupInfo);
  }

  bool seenBaseName(DeclBaseName name) {
    return DeclsByName.find(name) != DeclsByName.end();
  }
};

struct KeyPathDynamicMemberConsumer : public VisibleDeclConsumer {
  VisibleDeclConsumer &consumer;
  std::function<bool(DeclBaseName)> seenStaticBaseName;
  llvm::DenseSet<DeclBaseName> seen;

  SubscriptDecl *currentSubscript = nullptr;
  Type currentBaseType = Type();

  KeyPathDynamicMemberConsumer(VisibleDeclConsumer &consumer,
                               std::function<bool(DeclBaseName)> seenBaseName)
      : consumer(consumer), seenStaticBaseName(std::move(seenBaseName)) {}

  bool checkShadowed(ValueDecl *VD) {
    // Dynamic lookup members are only visible if they are not shadowed by
    // other members.
    return !isa<SubscriptDecl>(VD) && seen.insert(VD->getBaseName()).second &&
           !seenStaticBaseName(VD->getBaseName());
  }

  void onLookupNominalTypeMembers(NominalTypeDecl *NTD,
                                  DeclVisibilityKind Reason) override {
    consumer.onLookupNominalTypeMembers(NTD, Reason);
  }

  void foundDecl(ValueDecl *VD, DeclVisibilityKind reason,
                 DynamicLookupInfo dynamicLookupInfo) override {
    assert(dynamicLookupInfo.getKind() !=
           DynamicLookupInfo::KeyPathDynamicMember);

    // Only variables and subscripts are allowed in a keypath.
    if (!isa<AbstractStorageDecl>(VD))
      return;

    // Dynamic lookup members are only visible if they are not shadowed by
    // non-dynamic members.
    if (checkShadowed(VD))
      consumer.foundDecl(VD, DeclVisibilityKind::DynamicLookup,
                         {currentSubscript, currentBaseType, reason});
  }

  struct SubscriptChange {
    KeyPathDynamicMemberConsumer &consumer;
    SubscriptDecl *oldSubscript;
    Type oldBaseType;

    SubscriptChange(KeyPathDynamicMemberConsumer &consumer,
                    SubscriptDecl *newSubscript, Type newBaseType)
        : consumer(consumer), oldSubscript(newSubscript),
          oldBaseType(newBaseType) {
      std::swap(consumer.currentSubscript, oldSubscript);
      std::swap(consumer.currentBaseType, oldBaseType);
    }
    ~SubscriptChange() {
      consumer.currentSubscript = oldSubscript;
      consumer.currentBaseType = oldBaseType;
    }
  };
};
} // end anonymous namespace

static void lookupVisibleDynamicMemberLookupDecls(
    Type baseType, SourceLoc loc, KeyPathDynamicMemberConsumer &consumer,
    const DeclContext *dc, LookupState LS, DeclVisibilityKind reason,
    GenericSignature Sig, VisitedSet &visited,
    llvm::DenseSet<TypeBase *> &seenDynamicLookup);

/// Enumerates all members of \c baseType, including both directly visible and
/// members visible by keypath dynamic member lookup.
///
/// \note This is an implementation detail of \c lookupVisibleMemberDecls and
/// exists to create the correct recursion for dynamic member lookup.
static void lookupVisibleMemberAndDynamicMemberDecls(
    Type baseType, SourceLoc loc, VisibleDeclConsumer &consumer,
    KeyPathDynamicMemberConsumer &dynamicMemberConsumer, const DeclContext *DC,
    LookupState LS, DeclVisibilityKind reason, GenericSignature Sig,
    VisitedSet &visited, llvm::DenseSet<TypeBase *> &seenDynamicLookup) {
  lookupVisibleMemberDeclsImpl(baseType, consumer, DC, LS, reason, Sig, visited);
  lookupVisibleDynamicMemberLookupDecls(baseType, loc, dynamicMemberConsumer,
                                        DC, LS, reason, Sig, visited,
                                        seenDynamicLookup);
}

/// Enumerates all keypath dynamic members of \c baseType, as seen from the
/// context \c dc.
///
/// If \c baseType is \c \@dynamicMemberLookup, this looks up any keypath
/// dynamic member subscripts and looks up the members of the keypath's root
/// type.
static void lookupVisibleDynamicMemberLookupDecls(
    Type baseType, SourceLoc loc, KeyPathDynamicMemberConsumer &consumer,
    const DeclContext *dc, LookupState LS, DeclVisibilityKind reason,
    GenericSignature Sig, VisitedSet &visited,
    llvm::DenseSet<TypeBase *> &seenDynamicLookup) {
  if (!seenDynamicLookup.insert(baseType.getPointer()).second)
    return;

  if (!baseType->hasDynamicMemberLookupAttribute())
    return;

  auto &ctx = dc->getASTContext();

  // Lookup the `subscript(dynamicMember:)` methods in this type.
  DeclNameRef subscriptName(
      { ctx, DeclBaseName::createSubscript(), { ctx.Id_dynamicMember} });

  SmallVector<ValueDecl *, 2> subscripts;
  dc->lookupQualified(baseType, subscriptName, loc,
                      NL_QualifiedDefault | NL_ProtocolMembers, subscripts);

  for (ValueDecl *VD : subscripts) {
    auto *subscript = dyn_cast<SubscriptDecl>(VD);
    if (!subscript)
      continue;

    auto rootType = evaluateOrDefault(subscript->getASTContext().evaluator,
      RootTypeOfKeypathDynamicMemberRequest{subscript}, Type());
    if (rootType.isNull())
      continue;

    auto subs =
        baseType->getMemberSubstitutionMap(dc->getParentModule(), subscript);
    auto memberType = rootType.subst(subs);
    if (!memberType->mayHaveMembers())
      continue;

    KeyPathDynamicMemberConsumer::SubscriptChange sub(consumer, subscript,
                                                      baseType);

    lookupVisibleMemberAndDynamicMemberDecls(memberType, loc, consumer,
                                             consumer, dc, LS, reason, Sig,
                                             visited, seenDynamicLookup);
  }
}

/// Enumerate all members in \c BaseTy (including members of extensions,
/// superclasses and implemented protocols), as seen from the context \c CurrDC.
///
/// This operation corresponds to a standard "dot" lookup operation like "a.b"
/// where 'self' is the type of 'a'.  This operation is only valid after name
/// binding.
static void lookupVisibleMemberDecls(
    Type BaseTy, SourceLoc loc, VisibleDeclConsumer &Consumer,
    const DeclContext *CurrDC, LookupState LS,
    DeclVisibilityKind Reason, GenericSignature Sig) {
  OverrideFilteringConsumer overrideConsumer(BaseTy, CurrDC);
  KeyPathDynamicMemberConsumer dynamicConsumer(
      Consumer,
      [&](DeclBaseName name) { return overrideConsumer.seenBaseName(name); });

  VisitedSet Visited;
  llvm::DenseSet<TypeBase *> seenDynamicLookup;
  lookupVisibleMemberAndDynamicMemberDecls(
      BaseTy, loc, overrideConsumer, dynamicConsumer, CurrDC, LS, Reason,
      Sig, Visited, seenDynamicLookup);

  // Report the declarations we found to the real consumer.
  overrideConsumer.filterDecls(Consumer);
}

static void lookupVisibleDeclsImpl(VisibleDeclConsumer &Consumer,
                                   const DeclContext *DC,
                                   bool IncludeTopLevel, SourceLoc Loc) {
  const SourceManager &SM = DC->getASTContext().SourceMgr;
  auto MemberReason = DeclVisibilityKind::MemberOfCurrentNominal;

  // If we are inside of a method, check to see if there are any ivars in scope,
  // and if so, whether this is a reference to one of them.
  while (!DC->isModuleScopeContext()) {
    GenericParamList *GenericParams = nullptr;
    Type ExtendedType;
    auto LS = LookupState::makeUnqualified();
    LS = LS.withIncludeProtocolExtensionMembers();

    // Skip initializer contexts, we will not find any declarations there.
    if (isa<Initializer>(DC)) {
      // For non-'lazy' decls, lookup on the meta type.
      if (!isa<PatternBindingInitializer>(DC) ||
          !cast<PatternBindingInitializer>(DC)->getInitializedLazyVar())
        LS = LS.withOnMetatype();
      DC = DC->getParentForLookup();
    }

    // We don't look for generic parameters if we are in the context of a
    // nominal type: they will be looked up anyways via `lookupVisibleMemberDecls`.
    if (DC && !isa<NominalTypeDecl>(DC)) {
      if (auto *decl = DC->getAsDecl()) {
        if (auto GC = decl->getAsGenericContext()) {
          auto params = GC->getGenericParams();
          namelookup::FindLocalVal(SM, Loc, Consumer).checkGenericParams(params);
        }
      }
    }

    if (auto *SE = dyn_cast<SubscriptDecl>(DC)) {
      ExtendedType = SE->getDeclContext()->getSelfTypeInContext();
      DC = DC->getParentForLookup();
      if (SE->isStatic())
        LS = LS.withOnMetatype();
    } else if (auto *AFD = dyn_cast<AbstractFunctionDecl>(DC)) {

      // Look for local variables; normally, the parser resolves these
      // for us, but it can't do the right thing inside local types.
      // FIXME: when we can parse and typecheck the function body partially for
      // code completion, AFD->getBody() check can be removed.
      if (Loc.isValid() &&
          AFD->getBodySourceRange().isValid() &&
          SM.rangeContainsTokenLoc(AFD->getBodySourceRange(), Loc) &&
          AFD->getBody()) {
        namelookup::FindLocalVal(SM, Loc, Consumer).visit(AFD->getBody());
      }

      if (auto *P = AFD->getImplicitSelfDecl()) {
        namelookup::FindLocalVal(SM, Loc, Consumer).checkValueDecl(
          const_cast<ParamDecl *>(P), DeclVisibilityKind::FunctionParameter);
      }

      namelookup::FindLocalVal(SM, Loc, Consumer).checkParameterList(
        AFD->getParameters());

      GenericParams = AFD->getGenericParams();

      if (AFD->getDeclContext()->isTypeContext()) {
        ExtendedType = AFD->getDeclContext()->getSelfTypeInContext();
        DC = DC->getParentForLookup();

        if (auto *FD = dyn_cast<FuncDecl>(AFD))
          if (FD->isStatic())
            LS = LS.withOnMetatype();
      }
    } else if (auto CE = dyn_cast<ClosureExpr>(DC)) {
      if (Loc.isValid()) {
        namelookup::FindLocalVal(SM, Loc, Consumer).visit(CE->getBody());
        if (auto P = CE->getParameters()) {
          namelookup::FindLocalVal(SM, Loc, Consumer).checkParameterList(P);
        }
      }
    } else if (auto ED = dyn_cast<ExtensionDecl>(DC)) {
      ExtendedType = ED->getSelfTypeInContext();
    } else if (auto ND = dyn_cast<NominalTypeDecl>(DC)) {
      ExtendedType = ND->getSelfTypeInContext();
    }

    // If we're inside a function context, we've already moved to
    // the parent DC, so we have to check the function's generic
    // parameters first.
    if (GenericParams) {
      namelookup::FindLocalVal localVal(SM, Loc, Consumer);
      localVal.checkGenericParams(GenericParams);
    }

    // Check the generic parameters of our context.
    GenericParamList *dcGenericParams = nullptr;
    if (auto nominal = dyn_cast<NominalTypeDecl>(DC))
      dcGenericParams = nominal->getGenericParams();
    else if (auto ext = dyn_cast<ExtensionDecl>(DC))
      dcGenericParams = ext->getGenericParams();
    else if (auto subscript = dyn_cast<SubscriptDecl>(DC))
      dcGenericParams = subscript->getGenericParams();

    while (dcGenericParams) {
      namelookup::FindLocalVal localVal(SM, Loc, Consumer);
      localVal.checkGenericParams(dcGenericParams);
      dcGenericParams = dcGenericParams->getOuterParameters();
    }

    if (ExtendedType) {
      ::lookupVisibleMemberDecls(ExtendedType, Loc, Consumer, DC, LS,
                                 MemberReason, nullptr);

      // Going outside the current type context.
      MemberReason = DeclVisibilityKind::MemberOfOutsideNominal;
    }

    DC = DC->getParentForLookup();
  }

  if (auto SF = dyn_cast<SourceFile>(DC)) {
    if (Loc.isValid()) {
      // Look for local variables in top-level code; normally, the parser
      // resolves these for us, but it can't do the right thing for
      // local types.
      namelookup::FindLocalVal(SM, Loc, Consumer).checkSourceFile(*SF);
    }

    if (IncludeTopLevel) {
      auto &cached = SF->getCachedVisibleDecls();
      if (!cached.empty()) {
        for (auto result : cached)
          Consumer.foundDecl(result, DeclVisibilityKind::VisibleAtTopLevel);
        return;
      }
    }
  }

  if (IncludeTopLevel) {
    using namespace namelookup;
    SmallVector<ValueDecl *, 0> moduleResults;
    lookupVisibleDeclsInModule(DC, {}, moduleResults,
                               NLKind::UnqualifiedLookup,
                               ResolutionKind::Overloadable,
                               DC);
    for (auto result : moduleResults)
      Consumer.foundDecl(result, DeclVisibilityKind::VisibleAtTopLevel);

    if (auto SF = dyn_cast<SourceFile>(DC))
      SF->cacheVisibleDecls(std::move(moduleResults));
  }
}

void swift::lookupVisibleDecls(VisibleDeclConsumer &Consumer,
                               const DeclContext *DC,
                               bool IncludeTopLevel,
                               SourceLoc Loc) {
  if (Loc.isInvalid()) {
    lookupVisibleDeclsImpl(Consumer, DC, IncludeTopLevel, Loc);
    return;
  }
  UsableFilteringDeclConsumer FilteringConsumer(DC->getASTContext().SourceMgr,
                                                DC, Loc, Consumer);
  lookupVisibleDeclsImpl(FilteringConsumer, DC, IncludeTopLevel, Loc);
}

void swift::lookupVisibleMemberDecls(VisibleDeclConsumer &Consumer, Type BaseTy,
                                     SourceLoc loc, const DeclContext *CurrDC,
                                     bool includeInstanceMembers,
                                     bool includeDerivedRequirements,
                                     bool includeProtocolExtensionMembers,
                                     GenericSignature Sig) {
  assert(CurrDC);
  LookupState ls = LookupState::makeQualified();
  if (includeInstanceMembers) {
    ls = ls.withIncludedInstanceMembers();
  }
  if (includeDerivedRequirements) {
    ls = ls.withIncludedDerivedRequirements();
  }
  if (includeProtocolExtensionMembers) {
    ls = ls.withIncludeProtocolExtensionMembers();
  }

  ::lookupVisibleMemberDecls(BaseTy, loc, Consumer, CurrDC, ls,
                             DeclVisibilityKind::MemberOfCurrentNominal,
                             Sig);
}
