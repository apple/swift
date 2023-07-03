//===--- TypeCheckAvailability.cpp - Availability Diagnostics -------------===//
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
// This file implements availability diagnostics.
//
//===----------------------------------------------------------------------===//

#include "TypeCheckAvailability.h"
#include "MiscDiagnostics.h"
#include "TypeCheckConcurrency.h"
#include "TypeCheckObjC.h"
#include "TypeChecker.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeDeclFinder.h"
#include "swift/AST/TypeRefinementContext.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/StringExtras.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/Parser.h"
#include "swift/Sema/IDETypeChecking.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/SaveAndRestore.h"
using namespace swift;

ExportContext::ExportContext(
    DeclContext *DC, AvailabilityContext runningOSVersion,
    FragileFunctionKind kind, bool spi, bool exported, bool implicit,
    bool deprecated, llvm::Optional<PlatformKind> unavailablePlatformKind)
    : DC(DC), RunningOSVersion(runningOSVersion), FragileKind(kind) {
  SPI = spi;
  Exported = exported;
  Implicit = implicit;
  Deprecated = deprecated;
  if (unavailablePlatformKind) {
    Unavailable = 1;
    Platform = unsigned(*unavailablePlatformKind);
  } else {
    Unavailable = 0;
    Platform = 0;
  }

  Reason = unsigned(ExportabilityReason::General);
}

bool swift::isExported(const ValueDecl *VD) {
  if (VD->getAttrs().hasAttribute<ImplementationOnlyAttr>())
    return false;

  // Is this part of the module's API or ABI?
  AccessScope accessScope =
      VD->getFormalAccessScope(nullptr,
                               /*treatUsableFromInlineAsPublic*/true);
  if (accessScope.isPublic())
    return true;

  // Is this a stored property in a @frozen struct or class?
  if (auto *property = dyn_cast<VarDecl>(VD))
    if (property->isLayoutExposedToClients())
      return true;

  return false;
}

static bool hasConformancesToPublicProtocols(const ExtensionDecl *ED) {
  auto protocols = ED->getLocalProtocols(ConformanceLookupKind::OnlyExplicit);
  for (const ProtocolDecl *PD : protocols) {
    AccessScope scope =
        PD->getFormalAccessScope(/*useDC*/ nullptr,
                                 /*treatUsableFromInlineAsPublic*/ true);
    if (scope.isPublic())
      return true;
  }

  return false;
}

bool swift::isExported(const ExtensionDecl *ED) {
  // An extension can only be exported if it extends an exported type.
  if (auto *NTD = ED->getExtendedNominal()) {
    if (!isExported(NTD))
      return false;
  }

  // If there are any exported members then the extension is exported.
  for (const Decl *D : ED->getMembers()) {
    if (isExported(D))
      return true;
  }

  // If the extension declares a conformance to a public protocol then the
  // extension is exported.
  if (hasConformancesToPublicProtocols(ED))
    return true;

  return false;
}

bool swift::isExported(const Decl *D) {
  if (auto *VD = dyn_cast<ValueDecl>(D)) {
    return isExported(VD);
  }
  if (auto *PBD = dyn_cast<PatternBindingDecl>(D)) {
    for (unsigned i = 0, e = PBD->getNumPatternEntries(); i < e; ++i) {
      if (auto *VD = PBD->getAnchoringVarDecl(i))
        return isExported(VD);
    }

    return false;
  }
  if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
    return isExported(ED);
  }

  return true;
}

template<typename Fn>
static void forEachOuterDecl(DeclContext *DC, Fn fn) {
  for (; !DC->isModuleScopeContext(); DC = DC->getParent()) {
    switch (DC->getContextKind()) {
    case DeclContextKind::AbstractClosureExpr:
    case DeclContextKind::TopLevelCodeDecl:
    case DeclContextKind::SerializedLocal:
    case DeclContextKind::Package:
    case DeclContextKind::Module:
    case DeclContextKind::FileUnit:
    case DeclContextKind::MacroDecl:
      break;

    case DeclContextKind::Initializer:
      if (auto *PBI = dyn_cast<PatternBindingInitializer>(DC))
        fn(PBI->getBinding());
      else if (auto *I = dyn_cast<PropertyWrapperInitializer>(DC))
        fn(I->getWrappedVar());
      break;

    case DeclContextKind::SubscriptDecl:
      fn(cast<SubscriptDecl>(DC));
      break;

    case DeclContextKind::EnumElementDecl:
      fn(cast<EnumElementDecl>(DC));
      break;

    case DeclContextKind::AbstractFunctionDecl:
      fn(cast<AbstractFunctionDecl>(DC));

      if (auto *AD = dyn_cast<AccessorDecl>(DC))
        fn(AD->getStorage());
      break;

    case DeclContextKind::GenericTypeDecl:
      fn(cast<GenericTypeDecl>(DC));
      break;

    case DeclContextKind::ExtensionDecl:
      fn(cast<ExtensionDecl>(DC));
      break;
    }
  }
}

static void computeExportContextBits(
    ASTContext &Ctx, Decl *D, bool *spi, bool *implicit, bool *deprecated,
    llvm::Optional<PlatformKind> *unavailablePlatformKind) {
  if (D->isSPI() ||
      D->isAvailableAsSPI())
    *spi = true;

  // Defer bodies are desugared to an implicit closure expression. We need to
  // dilute the meaning of "implicit" to make sure we're still checking
  // availability inside of defer statements.
  const auto isDeferBody = isa<FuncDecl>(D) && cast<FuncDecl>(D)->isDeferBody();
  if (D->isImplicit() && !isDeferBody)
    *implicit = true;

  if (D->getAttrs().getDeprecated(Ctx))
    *deprecated = true;

  if (auto *A = D->getAttrs().getUnavailable(Ctx)) {
    *unavailablePlatformKind = A->Platform;
  }

  if (auto *PBD = dyn_cast<PatternBindingDecl>(D)) {
    for (unsigned i = 0, e = PBD->getNumPatternEntries(); i < e; ++i) {
      if (auto *VD = PBD->getAnchoringVarDecl(i))
        computeExportContextBits(Ctx, VD, spi, implicit, deprecated,
                                 unavailablePlatformKind);
    }
  }
}

ExportContext ExportContext::forDeclSignature(Decl *D) {
  auto &Ctx = D->getASTContext();

  auto *DC = D->getInnermostDeclContext();
  auto fragileKind = DC->getFragileFunctionKind();
  auto runningOSVersion =
      (Ctx.LangOpts.DisableAvailabilityChecking
       ? AvailabilityContext::alwaysAvailable()
       : TypeChecker::overApproximateAvailabilityAtLocation(D->getLoc(), DC));
  bool spi = Ctx.LangOpts.LibraryLevel == LibraryLevel::SPI;
  bool implicit = false;
  bool deprecated = false;
  llvm::Optional<PlatformKind> unavailablePlatformKind;
  computeExportContextBits(Ctx, D, &spi, &implicit, &deprecated,
                           &unavailablePlatformKind);
  forEachOuterDecl(D->getDeclContext(),
                   [&](Decl *D) {
                     computeExportContextBits(Ctx, D,
                                              &spi, &implicit, &deprecated,
                                              &unavailablePlatformKind);
                   });

  bool exported = ::isExported(D);

  return ExportContext(DC, runningOSVersion, fragileKind,
                       spi, exported, implicit, deprecated,
                       unavailablePlatformKind);
}

ExportContext ExportContext::forFunctionBody(DeclContext *DC, SourceLoc loc) {
  auto &Ctx = DC->getASTContext();

  auto fragileKind = DC->getFragileFunctionKind();
  auto runningOSVersion =
      (Ctx.LangOpts.DisableAvailabilityChecking
       ? AvailabilityContext::alwaysAvailable()
       : TypeChecker::overApproximateAvailabilityAtLocation(loc, DC));

  bool spi = Ctx.LangOpts.LibraryLevel == LibraryLevel::SPI;
  bool implicit = false;
  bool deprecated = false;
  llvm::Optional<PlatformKind> unavailablePlatformKind;
  forEachOuterDecl(DC,
                   [&](Decl *D) {
                     computeExportContextBits(Ctx, D,
                                              &spi, &implicit, &deprecated,
                                              &unavailablePlatformKind);
                   });

  bool exported = false;

  return ExportContext(DC, runningOSVersion, fragileKind,
                       spi, exported, implicit, deprecated,
                       unavailablePlatformKind);
}

ExportContext ExportContext::forConformance(DeclContext *DC,
                                            ProtocolDecl *proto) {
  assert(isa<ExtensionDecl>(DC) || isa<NominalTypeDecl>(DC));
  auto where = forDeclSignature(DC->getInnermostDeclarationDeclContext());

  where.Exported &= proto->getFormalAccessScope(
      DC, /*usableFromInlineAsPublic*/true).isPublic();

  return where;
}

ExportContext ExportContext::withReason(ExportabilityReason reason) const {
  auto copy = *this;
  copy.Reason = unsigned(reason);
  return copy;
}

ExportContext ExportContext::withExported(bool exported) const {
  auto copy = *this;
  copy.Exported = isExported() && exported;
  return copy;
}

llvm::Optional<PlatformKind> ExportContext::getUnavailablePlatformKind() const {
  if (Unavailable)
    return PlatformKind(Platform);
  return llvm::None;
}

bool ExportContext::mustOnlyReferenceExportedDecls() const {
  return Exported || FragileKind.kind != FragileFunctionKind::None;
}

llvm::Optional<ExportabilityReason>
ExportContext::getExportabilityReason() const {
  if (Exported)
    return ExportabilityReason(Reason);
  return llvm::None;
}

/// Returns the first availability attribute on the declaration that is active
/// on the target platform.
static const AvailableAttr *getActiveAvailableAttribute(const Decl *D,
                                                        ASTContext &AC) {
  for (auto Attr : D->getAttrs())
    if (auto AvAttr = dyn_cast<AvailableAttr>(Attr)) {
      if (!AvAttr->isInvalid() && AvAttr->isActivePlatform(AC)) {
        return AvAttr;
      }
    }
  return nullptr;
}

/// Returns true if there is any availability attribute on the declaration
/// that is active on the target platform.
static bool hasActiveAvailableAttribute(Decl *D,
                                           ASTContext &AC) {
  return getActiveAvailableAttribute(D, AC);
}

static bool computeContainedByDeploymentTarget(TypeRefinementContext *TRC,
                                               ASTContext &ctx) {
  return TRC->getAvailabilityInfo()
                  .isContainedIn(AvailabilityContext::forDeploymentTarget(ctx));
}

/// Returns true if the reference or any of its parents is an
/// unconditional unavailable declaration for the same platform.
static bool isInsideCompatibleUnavailableDeclaration(
    const Decl *D, const ExportContext &where, const AvailableAttr *attr) {
  auto referencedPlatform = where.getUnavailablePlatformKind();
  if (!referencedPlatform)
    return false;

  if (!attr->isUnconditionallyUnavailable()) {
    return false;
  }

  // Refuse calling unavailable functions from unavailable code,
  // but allow the use of types.
  PlatformKind platform = attr->Platform;
  if (platform == PlatformKind::none && !isa<TypeDecl>(D) &&
      !isa<ExtensionDecl>(D)) {
    return false;
  }

  return (*referencedPlatform == platform ||
          inheritsAvailabilityFromPlatform(platform, *referencedPlatform));
}

namespace {

/// A class to walk the AST to build the type refinement context hierarchy.
class TypeRefinementContextBuilder : private ASTWalker {

  ASTContext &Context;

  /// Represents an entry in a stack of active type refinement contexts. The
  /// stack is used to facilitate building the TRC's tree structure. A new TRC
  /// is pushed onto this stack before visiting children whenever the current
  /// AST node requires a new context and the TRC is then popped
  /// post-visitation.
  struct ContextInfo {
    TypeRefinementContext *TRC;

    /// The AST node. This node can be null (ParentTy()),
    /// indicating that custom logic elsewhere will handle removing
    /// the context when needed.
    ParentTy ScopeNode;

    bool ContainedByDeploymentTarget;
  };
  std::vector<ContextInfo> ContextStack;

  /// Represents an entry in a stack of pending decl body type refinement
  /// contexts. TRCs in this stack should be pushed onto \p ContextStack when
  /// \p BodyStmt is encountered.
  struct DeclBodyContextInfo {
    TypeRefinementContext *TRC;
    Decl *Decl;
    Stmt *BodyStmt;
  };
  std::vector<DeclBodyContextInfo> DeclBodyContextStack;

  /// A mapping from abstract storage declarations with accessors to
  /// to the type refinement contexts for those declarations. We refer to
  /// this map to determine the appropriate parent TRC to use when
  /// walking the accessor function.
  llvm::DenseMap<AbstractStorageDecl *, TypeRefinementContext *>
      StorageContexts;

  /// A mapping from pattern binding storage declarations to the type refinement
  /// contexts for those declarations. We refer to this map to determine the
  /// appropriate parent TRC to use when walking a var decl that belongs to a
  /// pattern containing multiple vars.
  llvm::DenseMap<PatternBindingDecl *, TypeRefinementContext *>
      PatternBindingContexts;

  TypeRefinementContext *getCurrentTRC() {
    return ContextStack.back().TRC;
  }

  bool isCurrentTRCContainedByDeploymentTarget() {
    return ContextStack.back().ContainedByDeploymentTarget;
  }

  void pushContext(TypeRefinementContext *TRC, ParentTy PopAfterNode) {
    ContextInfo Info;
    Info.TRC = TRC;
    Info.ScopeNode = PopAfterNode;

    if (!ContextStack.empty() && isCurrentTRCContainedByDeploymentTarget()) {
      assert(computeContainedByDeploymentTarget(TRC, Context) &&
             "incorrectly skipping computeContainedByDeploymentTarget()");
      Info.ContainedByDeploymentTarget = true;
    } else {
      Info.ContainedByDeploymentTarget =
          computeContainedByDeploymentTarget(TRC, Context);
    }

    ContextStack.push_back(Info);
  }

  void pushDeclBodyContext(TypeRefinementContext *TRC, Decl *D, Stmt *S) {
    DeclBodyContextInfo Info;
    Info.TRC = TRC;
    Info.Decl = D;
    Info.BodyStmt = S;

    DeclBodyContextStack.push_back(Info);
  }

  const char *stackTraceAction() const {
    return "building type refinement context for";
  }

public:
  TypeRefinementContextBuilder(TypeRefinementContext *TRC, ASTContext &Context)
      : Context(Context) {
    assert(TRC);
    pushContext(TRC, ParentTy());
  }

  void build(Decl *D) {
    PrettyStackTraceDecl trace(stackTraceAction(), D);
    unsigned StackHeight = ContextStack.size();
    D->walk(*this);
    assert(ContextStack.size() == StackHeight);
    (void)StackHeight;
  }

  void build(Stmt *S) {
    PrettyStackTraceStmt trace(Context, stackTraceAction(), S);
    unsigned StackHeight = ContextStack.size();
    S->walk(*this);
    assert(ContextStack.size() == StackHeight);
    (void)StackHeight;
  }

  void build(Expr *E) {
    PrettyStackTraceExpr trace(Context, stackTraceAction(), E);
    unsigned StackHeight = ContextStack.size();
    E->walk(*this);
    assert(ContextStack.size() == StackHeight);
    (void)StackHeight;
  }

private:
  MacroWalking getMacroWalkingBehavior() const override {
    // Expansion buffers will have their type refinement contexts built lazily.
    return MacroWalking::Arguments;
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    PrettyStackTraceDecl trace(stackTraceAction(), D);

    // Adds in a parent TRC for decls which are syntactically nested but are not
    // represented that way in the AST. (Particularly, AbstractStorageDecl
    // parents for AccessorDecl children.)
    if (auto ParentTRC = getEffectiveParentContextForDecl(D)) {
      pushContext(ParentTRC, D);
    }

    // Adds in a TRC that covers the entire declaration.
    if (auto DeclTRC = getNewContextForSignatureOfDecl(D)) {
      pushContext(DeclTRC, D);

      // Possibly use this as an effective parent context later.
      recordEffectiveParentContext(D, DeclTRC);
    }

    // Create TRCs that cover only the body of the declaration.
    buildContextsForBodyOfDecl(D);
    return Action::Continue();
  }

  PostWalkAction walkToDeclPost(Decl *D) override {
    while (ContextStack.back().ScopeNode.getAsDecl() == D) {
      ContextStack.pop_back();
    }

    while (!DeclBodyContextStack.empty() &&
           DeclBodyContextStack.back().Decl == D) {
      DeclBodyContextStack.pop_back();
    }

    return Action::Continue();
  }

  TypeRefinementContext *getEffectiveParentContextForDecl(Decl *D) {
    // FIXME: Can we assert that we won't walk parent decls later that should
    //        have been returned here?
    if (auto *accessor = dyn_cast<AccessorDecl>(D)) {
      // Use TRC of the storage rather the current TRC when walking this
      // function.
      auto it = StorageContexts.find(accessor->getStorage());
      if (it != StorageContexts.end()) {
        return it->second;
      }
    } else if (auto *VD = dyn_cast<VarDecl>(D)) {
      // Use the TRC of the pattern binding decl as the parent for var decls.
      if (auto *PBD = VD->getParentPatternBinding()) {
        auto it = PatternBindingContexts.find(PBD);
        if (it != PatternBindingContexts.end()) {
          return it->second;
        }
      }
    }

    return nullptr;
  }

  /// If necessary, records a TRC so it can be returned by subsequent calls to
  /// `getEffectiveParentContextForDecl()`.
  void recordEffectiveParentContext(Decl *D, TypeRefinementContext *NewTRC) {
    if (auto *StorageDecl = dyn_cast<AbstractStorageDecl>(D)) {
      // Stash the TRC for the storage declaration to use as the parent of
      // accessor decls later.
      if (StorageDecl->hasParsedAccessors())
        StorageContexts[StorageDecl] = NewTRC;
    }

    if (auto *VD = dyn_cast<VarDecl>(D)) {
      // Stash the TRC for the var decl if its parent pattern binding decl has
      // more than one entry so that the sibling var decls can reuse it.
      if (auto *PBD = VD->getParentPatternBinding()) {
        if (PBD->getNumPatternEntries() > 1)
          PatternBindingContexts[PBD] = NewTRC;
      }
    }
  }

  /// Returns a new context to be introduced for the declaration, or nullptr
  /// if no new context should be introduced.
  TypeRefinementContext *getNewContextForSignatureOfDecl(Decl *D) {
    if (!isa<ValueDecl>(D) && !isa<ExtensionDecl>(D) && !isa<MacroExpansionDecl>(D))
      return nullptr;

    // Only introduce for an AbstractStorageDecl if it is not local. We
    // introduce for the non-local case because these may have getters and
    // setters (and these may be synthesized, so they might not even exist yet).
    if (isa<AbstractStorageDecl>(D) && D->getDeclContext()->isLocalContext())
      return nullptr;

    // Ignore implicit declarations (mainly skips over `DeferStmt` functions).
    if (D->isImplicit())
      return nullptr;

    // Skip introducing additional contexts for var decls past the first in a
    // pattern. The context necessary for the pattern as a whole was already
    // introduced if necessary by the first var decl.
    if (auto *VD = dyn_cast<VarDecl>(D)) {
      if (auto *PBD = VD->getParentPatternBinding())
        if (VD != PBD->getAnchoringVarDecl(0))
          return nullptr;
    }

    // Declarations with an explicit availability attribute always get a TRC.
    if (hasActiveAvailableAttribute(D, Context)) {
      AvailabilityContext DeclaredAvailability =
          swift::AvailabilityInference::availableRange(D, Context);

      return TypeRefinementContext::createForDecl(
          Context, D, getCurrentTRC(),
          getEffectiveAvailabilityForDeclSignature(D, DeclaredAvailability),
          DeclaredAvailability, refinementSourceRangeForDecl(D));
    }

    // Declarations without explicit availability get a TRC if they are
    // effectively less available than the surrounding context. For example, an
    // internal property in a public struct can be effectively less available
    // than the containing struct decl because the internal property will only
    // be accessed by code running at the deployment target or later.
    AvailabilityContext CurrentAvailability =
        getCurrentTRC()->getAvailabilityInfo();
    AvailabilityContext EffectiveAvailability =
        getEffectiveAvailabilityForDeclSignature(D, CurrentAvailability);
    if (CurrentAvailability.isSupersetOf(EffectiveAvailability))
      return TypeRefinementContext::createForDeclImplicit(
          Context, D, getCurrentTRC(), EffectiveAvailability,
          refinementSourceRangeForDecl(D));

    return nullptr;
  }

  AvailabilityContext getEffectiveAvailabilityForDeclSignature(
      Decl *D, const AvailabilityContext BaseAvailability) {
    AvailabilityContext EffectiveAvailability = BaseAvailability;

    // As a special case, extension decls are treated as effectively as
    // available as the nominal type they extend, up to the deployment target.
    // This rule is a convenience for library authors who have written
    // extensions without specifying availabilty on the extension itself.
    if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
      auto ET = ED->getExtendedType();
      if (ET && !hasActiveAvailableAttribute(D, Context)) {
        EffectiveAvailability.intersectWith(
            swift::AvailabilityInference::inferForType(ET));

        // We want to require availability to be specified on extensions of
        // types that would be potentially unavailable to the module containing
        // the extension, so limit the effective availability to the deployment
        // target.
        EffectiveAvailability.unionWith(
            AvailabilityContext::forDeploymentTarget(Context));
      }
    }

    EffectiveAvailability.intersectWith(getCurrentTRC()->getAvailabilityInfo());
    if (shouldConstrainSignatureToDeploymentTarget(D))
      EffectiveAvailability.intersectWith(
          AvailabilityContext::forDeploymentTarget(Context));

    return EffectiveAvailability;
  }

  /// Checks whether the entire declaration, including its signature, should be
  /// constrained to the deployment target. Generally public API declarations
  /// are not constrained since they appear in the interface of the module and
  /// may be consumed by clients with lower deployment targets, but there are
  /// some exceptions.
  bool shouldConstrainSignatureToDeploymentTarget(Decl *D) {
    if (isCurrentTRCContainedByDeploymentTarget())
      return false;

    // As a convenience, SPI decls and explicitly unavailable decls are
    // constrained to the deployment target. There's not much benefit to
    // checking these declarations at a lower availability version floor since
    // neither can be used by API clients.
    if (D->isSPI() || AvailableAttr::isUnavailable(D))
      return true;

    return !::isExported(D);
  }

  /// Returns the source range which should be refined by declaration. This
  /// provides a convenient place to specify the refined range when it is
  /// different than the declaration's source range.
  SourceRange refinementSourceRangeForDecl(Decl *D) {
    // We require a valid range in order to be able to query for the TRC
    // corresponding to a given SourceLoc.
    // If this assert fires, it means we have probably synthesized an implicit
    // declaration without location information. The appropriate fix is
    // probably to gin up a source range for the declaration when synthesizing
    // it.
    assert(D->getSourceRange().isValid());

    if (auto *storageDecl = dyn_cast<AbstractStorageDecl>(D)) {
      // Use the declaration's availability for the context when checking
      // the bodies of its accessors.
      SourceRange Range = storageDecl->getSourceRange();

      // For a variable declaration (without accessors) we use the range of the
      // containing pattern binding declaration to make sure that we include
      // any type annotation in the type refinement context range. We also
      // need to include any attached property wrappers.
      if (auto *varDecl = dyn_cast<VarDecl>(storageDecl)) {
        if (auto *PBD = varDecl->getParentPatternBinding())
          Range = PBD->getSourceRange();

        for (auto *propertyWrapper : varDecl->getAttachedPropertyWrappers()) {
          Range.widen(propertyWrapper->getRange());
        }
      }

      // HACK: For synthesized trivial accessors we may have not a valid
      // location for the end of the braces, so in that case we will fall back
      // to using the range for the storage declaration. The right fix here is
      // to update AbstractStorageDecl::addTrivialAccessors() to take brace
      // locations and have callers of that method provide appropriate source
      // locations.
      SourceRange BracesRange = storageDecl->getBracesRange();
      if (storageDecl->hasParsedAccessors() && BracesRange.isValid()) {
        Range.widen(BracesRange);
      }

      return Range;
    }
    
    return D->getSourceRange();
  }

  void buildContextsForBodyOfDecl(Decl *D) {
    // Are we already constrained by the deployment target? If not, adding
    // new contexts won't change availability.
    if (isCurrentTRCContainedByDeploymentTarget())
      return;

    // A lambda that creates an implicit decl TRC specifying the deployment
    // target for `range` in decl `D`.
    auto createContext = [this](Decl *D, SourceRange range) {
      AvailabilityContext Availability =
          AvailabilityContext::forDeploymentTarget(Context);
      Availability.intersectWith(getCurrentTRC()->getAvailabilityInfo());

      return TypeRefinementContext::createForDeclImplicit(
          Context, D, getCurrentTRC(), Availability, range);
    };

    // Top level code always uses the deployment target.
    if (auto tlcd = dyn_cast<TopLevelCodeDecl>(D)) {
      if (auto bodyStmt = tlcd->getBody()) {
        pushDeclBodyContext(createContext(tlcd, tlcd->getSourceRange()), tlcd,
                            bodyStmt);
      }
      return;
    }

    // Function bodies use the deployment target if they are within the module's
    // resilience domain.
    if (auto afd = dyn_cast<AbstractFunctionDecl>(D)) {
      if (!afd->isImplicit() &&
          afd->getResilienceExpansion() != ResilienceExpansion::Minimal) {
        if (auto body = afd->getBody(/*canSynthesize*/ false)) {
          pushDeclBodyContext(createContext(afd, afd->getBodySourceRange()),
                              afd, body);
        }
      }
      return;
    }

    // Var decls may have associated pattern binding decls or property wrappers
    // with init expressions. Those expressions need to be constrained to the
    // deployment target unless they are exposed to clients.
    if (auto vd = dyn_cast<VarDecl>(D)) {
      if (!vd->hasInitialValue() || vd->isInitExposedToClients())
        return;

      if (auto *pbd = vd->getParentPatternBinding()) {
        int idx = pbd->getPatternEntryIndexForVarDecl(vd);
        auto *initExpr = pbd->getInit(idx);
        if (initExpr && !initExpr->isImplicit()) {
          assert(initExpr->getSourceRange().isValid());

          // Create a TRC for the init written in the source. The ASTWalker
          // won't visit these expressions so instead of pushing these onto the
          // stack we build them directly.
          auto *initTRC = createContext(vd, initExpr->getSourceRange());
          TypeRefinementContextBuilder(initTRC, Context).build(initExpr);
        }

        // Ideally any init expression would be returned by `getInit()` above.
        // However, for property wrappers it doesn't get populated until
        // typechecking completes (which is too late). Instead, we find the
        // the property wrapper attribute and use its source range to create a
        // TRC for the initializer expression.
        //
        // FIXME: Since we don't have an expression here, we can't build out its
        // TRC. If the Expr that will eventually be created contains a closure
        // expression, then it might have AST nodes that need to be refined. For
        // example, property wrapper initializers that takes block arguments
        // are not handled correctly because of this (rdar://77841331).
        for (auto *wrapper : vd->getAttachedPropertyWrappers()) {
          createContext(vd, wrapper->getRange());
        }
      }

      return;
    }
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    PrettyStackTraceStmt trace(Context, stackTraceAction(), S);

    if (consumeDeclBodyContextIfNecessary(S)) {
      return Action::Continue(S);
    }

    if (auto *IS = dyn_cast<IfStmt>(S)) {
      buildIfStmtRefinementContext(IS);
      return Action::SkipChildren(S);
    }

    if (auto *RS = dyn_cast<GuardStmt>(S)) {
      buildGuardStmtRefinementContext(RS);
      return Action::SkipChildren(S);
    }

    if (auto *WS = dyn_cast<WhileStmt>(S)) {
      buildWhileStmtRefinementContext(WS);
      return Action::SkipChildren(S);
    }

    return Action::Continue(S);
  }

  PostWalkResult<Stmt *> walkToStmtPost(Stmt *S) override {
    // If we have multiple guard statements in the same block
    // then we may have multiple refinement contexts to pop
    // after walking that block.
    while (!ContextStack.empty() &&
           ContextStack.back().ScopeNode.getAsStmt() == S) {
      ContextStack.pop_back();
    }

    return Action::Continue(S);
  }

  /// Consumes the top TRC from \p DeclBodyContextStack and pushes it onto the
  /// \p Context stack if the given \p Stmt is the matching body statement.
  /// Returns \p true if a context was pushed.
  bool consumeDeclBodyContextIfNecessary(Stmt *S) {
    if (DeclBodyContextStack.empty())
      return false;

    auto Info = DeclBodyContextStack.back();
    if (S != Info.BodyStmt)
      return false;

    pushContext(Info.TRC, Info.BodyStmt);
    DeclBodyContextStack.pop_back();
    return true;
  }

  /// Builds the type refinement hierarchy for the IfStmt if the guard
  /// introduces a new refinement context for the Then branch.
  /// There is no need for the caller to explicitly traverse the children
  /// of this node.
  void buildIfStmtRefinementContext(IfStmt *IS) {
    llvm::Optional<AvailabilityContext> ThenRange;
    llvm::Optional<AvailabilityContext> ElseRange;
    std::tie(ThenRange, ElseRange) =
        buildStmtConditionRefinementContext(IS->getCond());

    if (ThenRange.has_value()) {
      // Create a new context for the Then branch and traverse it in that new
      // context.
      auto *ThenTRC =
          TypeRefinementContext::createForIfStmtThen(Context, IS,
                                                     getCurrentTRC(),
                                                     ThenRange.value());
      TypeRefinementContextBuilder(ThenTRC, Context).build(IS->getThenStmt());
    } else {
      build(IS->getThenStmt());
    }

    Stmt *ElseStmt = IS->getElseStmt();
    if (!ElseStmt)
      return;

    // Refine the else branch if we're given a version range for that branch.
    // For now, if present, this will only be the empty range, indicating
    // that the branch is dead. We use it to suppress potential unavailability
    // and deprecation diagnostics on code that definitely will not run with
    // the current platform and minimum deployment target.
    // If we add a more precise version range lattice (i.e., one that can
    // support "<") we should create non-empty contexts for the Else branch.
    if (ElseRange.has_value()) {
      // Create a new context for the Then branch and traverse it in that new
      // context.
      auto *ElseTRC =
          TypeRefinementContext::createForIfStmtElse(Context, IS,
                                                     getCurrentTRC(),
                                                     ElseRange.value());
      TypeRefinementContextBuilder(ElseTRC, Context).build(ElseStmt);
    } else {
      build(IS->getElseStmt());
    }
  }

  /// Builds the type refinement hierarchy for the WhileStmt if the guard
  /// introduces a new refinement context for the body branch.
  /// There is no need for the caller to explicitly traverse the children
  /// of this node.
  void buildWhileStmtRefinementContext(WhileStmt *WS) {
    llvm::Optional<AvailabilityContext> BodyRange =
        buildStmtConditionRefinementContext(WS->getCond()).first;

    if (BodyRange.has_value()) {
      // Create a new context for the body and traverse it in the new
      // context.
      auto *BodyTRC = TypeRefinementContext::createForWhileStmtBody(
          Context, WS, getCurrentTRC(), BodyRange.value());
      TypeRefinementContextBuilder(BodyTRC, Context).build(WS->getBody());
    } else {
      build(WS->getBody());
    }
  }

  /// Builds the type refinement hierarchy for the GuardStmt and pushes
  /// the fallthrough context onto the context stack so that subsequent
  /// AST elements in the same scope are analyzed in the context of the
  /// fallthrough TRC.
  void buildGuardStmtRefinementContext(GuardStmt *GS) {
    // 'guard' statements fall through if all of the
    // guard conditions are true, so we refine the range after the require
    // until the end of the enclosing block.
    // if ... {
    //   guard available(...) else { return } <-- Refined range starts here
    //   ...
    // } <-- Refined range ends here
    //
    // This is slightly tricky because, unlike our other control constructs,
    // the refined region is not lexically contained inside the construct
    // introducing the refinement context.
    llvm::Optional<AvailabilityContext> FallthroughRange;
    llvm::Optional<AvailabilityContext> ElseRange;
    std::tie(FallthroughRange, ElseRange) =
        buildStmtConditionRefinementContext(GS->getCond());

    if (Stmt *ElseBody = GS->getBody()) {
      if (ElseRange.has_value()) {
        auto *TrueTRC = TypeRefinementContext::createForGuardStmtElse(
            Context, GS, getCurrentTRC(), ElseRange.value());

        TypeRefinementContextBuilder(TrueTRC, Context).build(ElseBody);
      } else {
        build(ElseBody);
      }
    }

    auto *ParentBrace = dyn_cast<BraceStmt>(Parent.getAsStmt());
    assert(ParentBrace && "Expected parent of GuardStmt to be BraceStmt");
    if (!FallthroughRange.has_value())
      return;

    // Create a new context for the fallthrough.

    auto *FallthroughTRC =
          TypeRefinementContext::createForGuardStmtFallthrough(Context, GS,
              ParentBrace, getCurrentTRC(), FallthroughRange.value());

    pushContext(FallthroughTRC, ParentBrace);
  }

  /// Build the type refinement context for a StmtCondition and return a pair
  /// of optional version ranges, the first for the true branch and the second
  /// for the false branch. A value of None for a given branch indicates that
  /// the branch does not introduce a new refinement.
  std::pair<llvm::Optional<AvailabilityContext>,
            llvm::Optional<AvailabilityContext>>
  buildStmtConditionRefinementContext(StmtCondition Cond) {

    // Any refinement contexts introduced in the statement condition
    // will end at the end of the last condition element.
    StmtConditionElement LastElement = Cond.back();
    
    // Keep track of how many nested refinement contexts we have pushed on
    // the context stack so we can pop them when we're done building the
    // context for the StmtCondition.
    unsigned NestedCount = 0;

    // Tracks the potential version range when the condition is false.
    auto FalseFlow = AvailabilityContext::neverAvailable();

    TypeRefinementContext *StartingTRC = getCurrentTRC();

    // Tracks if we're refining for availability or unavailability.
    llvm::Optional<bool> isUnavailability = llvm::None;

    for (StmtConditionElement Element : Cond) {
      TypeRefinementContext *CurrentTRC = getCurrentTRC();
      AvailabilityContext CurrentInfo = CurrentTRC->getAvailabilityInfo();
      AvailabilityContext CurrentExplicitInfo =
        CurrentTRC->getExplicitAvailabilityInfo();

      // If the element is not a condition, walk it in the current TRC.
      if (Element.getKind() != StmtConditionElement::CK_Availability) {

        // Assume any condition element that is not a #available() can
        // potentially be false, so conservatively combine the version
        // range of the current context with the accumulated false flow
        // of all other conjuncts.
        FalseFlow.unionWith(CurrentInfo);

        Element.walk(*this);
        continue;
      }

      // #available query: introduce a new refinement context for the statement
      // condition elements following it.
      auto *Query = Element.getAvailability();

      if (isUnavailability == llvm::None) {
        isUnavailability = Query->isUnavailability();
      } else if (isUnavailability != Query->isUnavailability()) {
        // Mixing availability with unavailability in the same statement will
        // cause the false flow's version range to be ambiguous. Report it.
        //
        // Technically we can support this by not refining ambiguous flows,
        // but there are currently no legitimate cases where one would have
        // to mix availability with unavailability.
        Context.Diags.diagnose(Query->getLoc(),
                               diag::availability_cannot_be_mixed);
        break;
      }

      // If this query expression has no queries, we will not introduce a new
      // refinement context. We do not diagnose here: a diagnostic will already
      // have been emitted by the parser.
      // For #unavailable, empty queries are valid as wildcards are implied.
      if (!Query->isUnavailability() && Query->getQueries().empty())
        continue;

      AvailabilitySpec *Spec = bestActiveSpecForQuery(Query);
      if (!Spec) {
        // We couldn't find an appropriate spec for the current platform,
        // so rather than refining, emit a diagnostic and just use the current
        // TRC.
        Context.Diags.diagnose(
            Query->getLoc(), diag::availability_query_required_for_platform,
            platformString(targetPlatform(Context.LangOpts)));

        continue;
      }

      AvailabilityContext NewConstraint = contextForSpec(Spec, false);
      Query->setAvailableRange(contextForSpec(Spec, true).getOSVersion());

      // When compiling zippered for macCatalyst, we need to collect both
      // a macOS version (the target version) and an iOS/macCatalyst version
      // (the target-variant). These versions will both be passed to a runtime
      // entrypoint that will check either the macOS version or the iOS
      // version depending on the kind of process this code is loaded into.
      if (Context.LangOpts.TargetVariant) {
        AvailabilitySpec *VariantSpec =
            bestActiveSpecForQuery(Query, /*ForTargetVariant*/ true);
        VersionRange VariantRange =
            contextForSpec(VariantSpec, true).getOSVersion();
        Query->setVariantAvailableRange(VariantRange);
      }

      if (Spec->getKind() == AvailabilitySpecKind::OtherPlatform) {
        // The wildcard spec '*' represents the minimum deployment target, so
        // there is no need to create a refinement context for this query.
        // Further, we won't diagnose for useless #available() conditions
        // where * matched on this platform -- presumably those conditions are
        // needed for some other platform.
        continue;
      }

      // If the explicitly-specified (via #availability) version range for the
      // current TRC is completely contained in the range for the spec, then
      // a version query can never be false, so the spec is useless.
      // If so, report this.
      if (CurrentExplicitInfo.isContainedIn(NewConstraint)) {
        // Unavailability refinements are always "useless" from a symbol
        // availability point of view, so only useless availability specs are
        // reported.
        if (isUnavailability.value()) {
          continue;
        }
        DiagnosticEngine &Diags = Context.Diags;
        if (CurrentTRC->getReason() != TypeRefinementContext::Reason::Root) {
          PlatformKind BestPlatform = targetPlatform(Context.LangOpts);
          auto *PlatformSpec =
              dyn_cast<PlatformVersionConstraintAvailabilitySpec>(Spec);

          // If possible, try to report the diagnostic in terms for the
          // platform the user uttered in the '#available()'. For a platform
          // that inherits availability from another platform it may be
          // different from the platform specified in the target triple.
          if (PlatformSpec)
            BestPlatform = PlatformSpec->getPlatform();
          Diags.diagnose(Query->getLoc(),
                         diag::availability_query_useless_enclosing_scope,
                         platformString(BestPlatform));
          Diags.diagnose(CurrentTRC->getIntroductionLoc(),
                         diag::availability_query_useless_enclosing_scope_here);
        }
      }

      if (CurrentInfo.isContainedIn(NewConstraint)) {
        // No need to actually create the refinement context if we know it is
        // useless.
        continue;
      }

      // If the #available() is not useless then there is potential false flow,
      // so join the false flow with the potential versions of the current
      // context.
      // We could be more precise here if we enriched the lattice to include
      // ranges of the form [x, y).
      FalseFlow.unionWith(CurrentInfo);

      auto *TRC = TypeRefinementContext::createForConditionFollowingQuery(
          Context, Query, LastElement, CurrentTRC, NewConstraint);

      pushContext(TRC, ParentTy());
      ++NestedCount;
    }

    llvm::Optional<AvailabilityContext> FalseRefinement = llvm::None;
    // The version range for the false branch should never have any versions
    // that weren't possible when the condition started evaluating.
    assert(FalseFlow.isContainedIn(StartingTRC->getAvailabilityInfo()));

    // If the starting version range is not completely contained in the
    // false flow version range then it must be the case that false flow range
    // is strictly smaller than the starting range (because the false flow
    // range *is* contained in the starting range), so we should introduce a
    // new refinement for the false flow.
    if (!StartingTRC->getAvailabilityInfo().isContainedIn(FalseFlow)) {
      FalseRefinement = FalseFlow;
    }

    auto makeResult = [isUnavailability](
                          llvm::Optional<AvailabilityContext> TrueRefinement,
                          llvm::Optional<AvailabilityContext> FalseRefinement) {
      if (isUnavailability.has_value() && isUnavailability.value()) {
        // If this is an unavailability check, invert the result.
        return std::make_pair(FalseRefinement, TrueRefinement);
      }
      return std::make_pair(TrueRefinement, FalseRefinement);
    };

    if (NestedCount == 0)
      return makeResult(llvm::None, FalseRefinement);

    TypeRefinementContext *NestedTRC = getCurrentTRC();
    while (NestedCount-- > 0)
      ContextStack.pop_back();

    assert(getCurrentTRC() == StartingTRC);

    return makeResult(NestedTRC->getAvailabilityInfo(), FalseRefinement);
  }

  /// Return the best active spec for the target platform or nullptr if no
  /// such spec exists.
  AvailabilitySpec *bestActiveSpecForQuery(PoundAvailableInfo *available,
                                           bool forTargetVariant = false) {
    OtherPlatformAvailabilitySpec *FoundOtherSpec = nullptr;
    PlatformVersionConstraintAvailabilitySpec *BestSpec = nullptr;

    for (auto *Spec : available->getQueries()) {
      if (auto *OtherSpec = dyn_cast<OtherPlatformAvailabilitySpec>(Spec)) {
        FoundOtherSpec = OtherSpec;
        continue;
      }

      auto *VersionSpec =
          dyn_cast<PlatformVersionConstraintAvailabilitySpec>(Spec);
      if (!VersionSpec)
        continue;

      // FIXME: This is not quite right: we want to handle AppExtensions
      // properly. For example, on the OSXApplicationExtension platform
      // we want to chose the OS X spec unless there is an explicit
      // OSXApplicationExtension spec.
      if (isPlatformActive(VersionSpec->getPlatform(), Context.LangOpts,
                           forTargetVariant)) {
        if (!BestSpec ||
            inheritsAvailabilityFromPlatform(VersionSpec->getPlatform(),
                                             BestSpec->getPlatform())) {
          BestSpec = VersionSpec;
        }
      }
    }

    if (BestSpec)
      return BestSpec;

    // If we have reached this point, we found no spec for our target, so
    // we return the other spec ('*'), if we found it, or nullptr, if not.
    if (FoundOtherSpec) {
      return FoundOtherSpec;
    } else if (available->isUnavailability()) {
      // For #unavailable, imply the presence of a wildcard.
      SourceLoc Loc = available->getRParenLoc();
      return new (Context) OtherPlatformAvailabilitySpec(Loc);
    } else {
      return nullptr;
    }
  }

  /// Return the availability context for the given spec.
  AvailabilityContext contextForSpec(AvailabilitySpec *Spec,
                                    bool GetRuntimeContext) {
    if (isa<OtherPlatformAvailabilitySpec>(Spec)) {
      return AvailabilityContext::alwaysAvailable();
    }

    auto *VersionSpec = cast<PlatformVersionConstraintAvailabilitySpec>(Spec);

    llvm::VersionTuple Version = (GetRuntimeContext ?
                                    VersionSpec->getRuntimeVersion() :
                                    VersionSpec->getVersion());

    return AvailabilityContext(VersionRange::allGTE(Version));
  }

  PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
    if (ContextStack.back().ScopeNode.getAsExpr() == E) {
      ContextStack.pop_back();
    }

    return Action::Continue(E);
  }
};
  
} // end anonymous namespace

void TypeChecker::buildTypeRefinementContextHierarchy(SourceFile &SF) {
  TypeRefinementContext *RootTRC = SF.getTypeRefinementContext();
  ASTContext &Context = SF.getASTContext();

  if (!RootTRC) {
    // The root type refinement context reflects the fact that all parts of
    // the source file are guaranteed to be executing on at least the minimum
    // platform version for inlining.
    auto MinPlatformReq = AvailabilityContext::forInliningTarget(Context);
    RootTRC = TypeRefinementContext::createRoot(&SF, MinPlatformReq);
    SF.setTypeRefinementContext(RootTRC);
  }

  // Build refinement contexts, if necessary, for all declarations starting
  // with StartElem.
  TypeRefinementContextBuilder Builder(RootTRC, Context);
  for (auto item : SF.getTopLevelItems()) {
    if (auto decl = item.dyn_cast<Decl *>())
      Builder.build(decl);
    else if (auto expr = item.dyn_cast<Expr *>())
      Builder.build(expr);
    else if (auto stmt = item.dyn_cast<Stmt *>())
      Builder.build(stmt);
  }
}

void TypeChecker::buildTypeRefinementContextHierarchyDelayed(SourceFile &SF, AbstractFunctionDecl *AFD) {
  // If there's no TRC for the file, we likely don't want this one either.
  // RootTRC is not set when availability checking is disabled.
  TypeRefinementContext *RootTRC = SF.getTypeRefinementContext();
  if(!RootTRC)
    return;

  if (AFD->getBodyKind() != AbstractFunctionDecl::BodyKind::Unparsed)
    return;

  // Parse the function body.
  AFD->getBody(/*canSynthesize=*/true);

  // Build the refinement context for the function body.
  ASTContext &Context = SF.getASTContext();
  auto LocalTRC = RootTRC->findMostRefinedSubContext(AFD->getLoc(), Context.SourceMgr);
  TypeRefinementContextBuilder Builder(LocalTRC, Context);
  Builder.build(AFD);
}

TypeRefinementContext *
TypeChecker::getOrBuildTypeRefinementContext(SourceFile *SF) {
  TypeRefinementContext *TRC = SF->getTypeRefinementContext();
  if (!TRC) {
    buildTypeRefinementContextHierarchy(*SF);
    TRC = SF->getTypeRefinementContext();
  }

  return TRC;
}

AvailabilityContext
TypeChecker::overApproximateAvailabilityAtLocation(SourceLoc loc,
                                                   const DeclContext *DC,
                                                   const TypeRefinementContext **MostRefined) {
  SourceFile *SF;
  if (loc.isValid())
    SF = DC->getParentModule()->getSourceFileContainingLocation(loc);
  else
    SF = DC->getParentSourceFile();
  auto &Context = DC->getASTContext();

  // If our source location is invalid (this may be synthesized code), climb
  // the decl context hierarchy until we find a location that is valid,
  // collecting availability ranges on the way up.
  // We will combine the version ranges from these annotations
  // with the TRC for the valid location to overapproximate the running
  // OS versions at the original source location.
  // Because we are climbing DeclContexts we will miss refinement contexts in
  // synthesized code that are introduced by AST elements that are themselves
  // not DeclContexts, such as  #available(..) and property declarations.
  // That is, a reference with an invalid location that is contained
  // inside a #available() and with no intermediate DeclContext will not be
  // refined. For now, this is fine -- but if we ever synthesize #available(),
  // this will be a real problem.

  // We can assume we are running on at least the minimum inlining target.
  auto OverApproximateContext = AvailabilityContext::forInliningTarget(Context);
  auto isInvalidLoc = [SF](SourceLoc loc) {
    return SF ? loc.isInvalid() : true;
  };
  while (DC && isInvalidLoc(loc)) {
    const Decl *D = DC->getInnermostDeclarationDeclContext();
    if (!D)
      break;

    loc = D->getLoc();

    llvm::Optional<AvailabilityContext> Info =
        AvailabilityInference::annotatedAvailableRange(D, Context);

    if (Info.has_value()) {
      OverApproximateContext.constrainWith(Info.value());
    }

    DC = D->getDeclContext();
  }

  if (SF && loc.isValid()) {
    TypeRefinementContext *rootTRC = getOrBuildTypeRefinementContext(SF);
    TypeRefinementContext *TRC =
        rootTRC->findMostRefinedSubContext(loc, Context.SourceMgr);
    OverApproximateContext.constrainWith(TRC->getAvailabilityInfo());
    if (MostRefined) {
      *MostRefined = TRC;
    }
  }

  return OverApproximateContext;
}

bool TypeChecker::isDeclarationUnavailable(
    const Decl *D, const DeclContext *referenceDC,
    llvm::function_ref<AvailabilityContext()> getAvailabilityContext) {
  ASTContext &Context = referenceDC->getASTContext();
  if (Context.LangOpts.DisableAvailabilityChecking) {
    return false;
  }

  if (!referenceDC->getParentSourceFile()) {
    // We only check availability if this reference is in a source file; we do
    // not check in other kinds of FileUnits.
    return false;
  }

  AvailabilityContext safeRangeUnderApprox{
      AvailabilityInference::availableRange(D, Context)};

  if (safeRangeUnderApprox.isAlwaysAvailable())
    return false;

  AvailabilityContext runningOSOverApprox = getAvailabilityContext();

  // The reference is safe if an over-approximation of the running OS
  // versions is fully contained within an under-approximation
  // of the versions on which the declaration is available. If this
  // containment cannot be guaranteed, we say the reference is
  // not available.
  return !runningOSOverApprox.isContainedIn(safeRangeUnderApprox);
}

llvm::Optional<UnavailabilityReason>
TypeChecker::checkDeclarationAvailability(const Decl *D,
                                          const ExportContext &Where) {
  // Skip computing potential unavailability if the declaration is explicitly
  // unavailable and the context is also unavailable.
  if (const AvailableAttr *Attr = AvailableAttr::isUnavailable(D))
    if (isInsideCompatibleUnavailableDeclaration(D, Where, Attr))
      return llvm::None;

  if (isDeclarationUnavailable(D, Where.getDeclContext(), [&Where] {
        return Where.getAvailabilityContext();
      })) {
    auto &Context = Where.getDeclContext()->getASTContext();
    AvailabilityContext safeRangeUnderApprox{
        AvailabilityInference::availableRange(D, Context)};

    VersionRange version = safeRangeUnderApprox.getOSVersion();
    return UnavailabilityReason::requiresVersionRange(version);
  }

  return llvm::None;
}

llvm::Optional<UnavailabilityReason>
TypeChecker::checkConformanceAvailability(const RootProtocolConformance *conf,
                                          const ExtensionDecl *ext,
                                          const ExportContext &where) {
  return checkDeclarationAvailability(ext, where);
}

/// A class that walks the AST to find the innermost (i.e., deepest) node that
/// contains a target SourceRange and matches a particular criterion.
/// This class finds the innermost nodes of interest by walking
/// down the root until it has found the target range (in a Pre-visitor)
/// and then recording the innermost node on the way back up in the
/// Post-visitors. It does its best to not search unnecessary subtrees,
/// although this is complicated by the fact that not all nodes have
/// source range information.
class InnermostAncestorFinder : private ASTWalker {
public:

  /// The type of a match predicate, which takes as input a node and its
  /// parent and returns a bool indicating whether the node matches.
  using MatchPredicate = std::function<bool(ASTNode, ASTWalker::ParentTy)>;

private:
  const SourceRange TargetRange;
  const SourceManager &SM;
  const MatchPredicate Predicate;

  bool FoundTarget = false;
  llvm::Optional<ASTNode> InnermostMatchingNode;

public:
  InnermostAncestorFinder(SourceRange TargetRange, const SourceManager &SM,
                          ASTNode SearchNode, const MatchPredicate &Predicate)
      : TargetRange(TargetRange), SM(SM), Predicate(Predicate) {
    assert(TargetRange.isValid());

    SearchNode.walk(*this);
  }

  /// Returns the innermost node containing the target range that matches
  /// the predicate.
  llvm::Optional<ASTNode> getInnermostMatchingNode() {
    return InnermostMatchingNode;
  }

  MacroWalking getMacroWalkingBehavior() const override {
    // This is SourceRange based finder. 'SM.rangeContains()' fails anyway when
    // crossing source buffers.
    return MacroWalking::Arguments;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    return getPreWalkActionFor(E);
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    return getPreWalkActionFor(S);
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    return getPreWalkActionFor(D).Action;
  }

  PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override {
    return getPreWalkActionFor(P);
  }

  PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
    return getPreWalkActionFor(T).Action;
  }

  /// Retrieve the pre-walk action for a given node, which determines whether
  /// or not it should be walked into.
  template <typename T>
  PreWalkResult<T> getPreWalkActionFor(T Node) {
    // When walking down the tree, we traverse until we have found a node
    // inside the target range. Once we have found such a node, there is no
    // need to traverse any deeper.
    if (FoundTarget)
      return Action::SkipChildren(Node);

    // If we haven't found our target yet and the node we are pre-visiting
    // doesn't have a valid range, we still have to traverse it because its
    // subtrees may have valid ranges.
    auto Range = Node->getSourceRange();
    if (Range.isInvalid())
      return Action::Continue(Node);

    // We have found our target if the range of the node we are visiting
    // is contained in the range we are looking for.
    FoundTarget = SM.rangeContains(TargetRange, Range);

    if (FoundTarget)
      return Action::SkipChildren(Node);

    // Search the subtree if the target range is inside its range.
    if (!SM.rangeContains(Range, TargetRange))
      return Action::SkipChildren(Node);

    return Action::Continue(Node);
  }

  PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
    return walkToNodePost(E);
  }

  PostWalkResult<Stmt *> walkToStmtPost(Stmt *S) override {
    return walkToNodePost(S);
  }

  PostWalkAction walkToDeclPost(Decl *D) override {
    return walkToNodePost(D).Action;
  }

  /// Once we have found the target node, look for the innermost ancestor
  /// matching our criteria on the way back up the spine of the tree.
  template <typename T>
  PostWalkResult<T> walkToNodePost(T Node) {
    if (!InnermostMatchingNode.has_value() && Predicate(Node, Parent)) {
      assert(Node->getSourceRange().isInvalid() ||
             SM.rangeContains(Node->getSourceRange(), TargetRange));

      InnermostMatchingNode = Node;
      return Action::Stop();
    }

    return Action::Continue(Node);
  }
};

/// Starting from SearchRoot, finds the innermost node containing ChildRange
/// for which Predicate returns true. Returns None if no such root is found.
static llvm::Optional<ASTNode> findInnermostAncestor(
    SourceRange ChildRange, const SourceManager &SM, ASTNode SearchRoot,
    const InnermostAncestorFinder::MatchPredicate &Predicate) {
  InnermostAncestorFinder Finder(ChildRange, SM, SearchRoot, Predicate);
  return Finder.getInnermostMatchingNode();
}

/// Given a reference range and a declaration context containing the range,
/// attempt to find a declaration containing the reference. This may not
/// be the innermost declaration containing the range.
/// Returns null if no such declaration can be found.
static const Decl *findContainingDeclaration(SourceRange ReferenceRange,
                                             const DeclContext *ReferenceDC,
                                             const SourceManager &SM) {
  auto ContainsReferenceRange = [&](const Decl *D) -> bool {
    if (ReferenceRange.isInvalid())
      return false;

    // Members of an active #if are represented both inside the
    // IfConfigDecl and in the enclosing context. Skip over the IfConfigDecl
    // so that the member declaration is found rather the #if itself.
    if (isa<IfConfigDecl>(D))
      return false;

    return SM.rangeContains(D->getSourceRange(), ReferenceRange);
  };

  if (const Decl *D = ReferenceDC->getInnermostDeclarationDeclContext()) {
    // If we have an inner declaration context, see if we can narrow the search
    // down to one of its members. This is important for properties, which don't
    // count as DeclContexts of their own but which can still introduce
    // availability.
    if (auto *IDC = dyn_cast<IterableDeclContext>(D)) {
      auto BestMember = llvm::find_if(IDC->getMembers(),
                                      ContainsReferenceRange);
      if (BestMember != IDC->getMembers().end())
        return *BestMember;
    }
    return D;
  }

  // We couldn't find a suitable node by climbing the DeclContext hierarchy, so
  // fall back to looking for a top-level declaration that contains the
  // reference range. We will hit this case for top-level elements that do not
  // themselves introduce DeclContexts, such as global variables. If we don't
  // have a reference range, there is nothing we can do, so return null.
  if (ReferenceRange.isInvalid())
    return nullptr;

  SourceFile *SF = ReferenceDC->getParentSourceFile();
  if (!SF)
    return nullptr;

  auto BestTopLevelDecl = llvm::find_if(SF->getTopLevelDecls(),
                                        ContainsReferenceRange);
  if (BestTopLevelDecl != SF->getTopLevelDecls().end())
    return *BestTopLevelDecl;

  return nullptr;
}

/// Given a declaration that allows availability attributes in the abstract
/// syntax tree, return the declaration upon which the declaration would
/// appear in concrete syntax. This function is necessary because for semantic
/// analysis, the parser attaches attributes to declarations other
/// than those on which they, concretely, appear. For these declarations (enum
/// cases and variable declarations) a Fix-It for an added availability
/// attribute should be suggested for the appropriate concrete location.
static const Decl *
concreteSyntaxDeclForAvailableAttribute(const Decl *AbstractSyntaxDecl) {
  // This function needs to be kept in sync with its counterpart,
  // abstractSyntaxDeclForAvailableAttribute().

  // The source range for VarDecls does not include 'var ' (and, in any
  // event, multiple variables can be introduced with a single 'var'),
  // so suggest adding an attribute to the PatterningBindingDecl instead.
  if (auto *VD = dyn_cast<VarDecl>(AbstractSyntaxDecl)) {
    return VD->getParentPatternBinding();
  }

  // Similarly suggest applying the Fix-It to the parent enum case rather than
  // the enum element.
  if (auto *EE = dyn_cast<EnumElementDecl>(AbstractSyntaxDecl)) {
    return EE->getParentCase();
  }

  return AbstractSyntaxDecl;
}

/// Given a declaration upon which an availability attribute would appear in
/// concrete syntax, return a declaration to which the parser
/// actually attaches the attribute in the abstract syntax tree. We use this
/// function to determine whether the concrete syntax already has an
/// availability attribute.
static const Decl *
abstractSyntaxDeclForAvailableAttribute(const Decl *ConcreteSyntaxDecl) {
  // This function needs to be kept in sync with its counterpart,
  // concreteSyntaxDeclForAvailableAttribute().

  if (auto *PBD = dyn_cast<PatternBindingDecl>(ConcreteSyntaxDecl)) {
    // Existing @available attributes in the AST are attached to VarDecls
    // rather than PatternBindingDecls, so we return the first VarDecl for
    // the pattern binding declaration.
    // This is safe, even though there may be multiple VarDecls, because
    // all parsed attribute that appear in the concrete syntax upon on the
    // PatternBindingDecl are added to all of the VarDecls for the pattern
    // binding.
    if (PBD->getNumPatternEntries() != 0) {
      return PBD->getAnchoringVarDecl(0);
    }
  } else if (auto *ECD = dyn_cast<EnumCaseDecl>(ConcreteSyntaxDecl)) {
    // Similar to the PatternBindingDecl case above, we return the
    // first EnumElementDecl.
    if (auto *Elem = ECD->getFirstElement()) {
      return Elem;
    }
  }

  return ConcreteSyntaxDecl;
}

/// Given a declaration, return a better related declaration for which
/// to suggest an @available fixit, or the original declaration
/// if no such related declaration exists.
static const Decl *relatedDeclForAvailabilityFixit(const Decl *D) {
  if (auto *accessor = dyn_cast<AccessorDecl>(D)) {
    // Suggest @available Fix-Its on property rather than individual
    // accessors.
    D = accessor->getStorage();
  }

  return abstractSyntaxDeclForAvailableAttribute(D);
}

/// Walk the DeclContext hierarchy starting from D to find a declaration
/// at the member level (i.e., declared in a type context) on which to provide
/// an @available() Fix-It.
static const Decl *ancestorMemberLevelDeclForAvailabilityFixit(const Decl *D) {
  while (D) {
    D = relatedDeclForAvailabilityFixit(D);

    if (!D->isImplicit() &&
        D->getDeclContext()->isTypeContext() &&
        DeclAttribute::canAttributeAppearOnDecl(DeclAttrKind::DAK_Available,
                                                D)) {
      break;
    }

    D = cast_or_null<AbstractFunctionDecl>(
        D->getDeclContext()->getInnermostMethodContext());
  }

  return D;
}

/// Returns true if the declaration is at the type level (either a nominal
/// type, an extension, or a global function) and can support an @available
/// attribute.
static bool isTypeLevelDeclForAvailabilityFixit(const Decl *D) {
  if (!DeclAttribute::canAttributeAppearOnDecl(DeclAttrKind::DAK_Available,
                                               D)) {
    return false;
  }

  if (isa<ExtensionDecl>(D) || isa<NominalTypeDecl>(D)) {
    return true;
  }

  bool IsModuleScopeContext = D->getDeclContext()->isModuleScopeContext();

  // We consider global functions to be "type level"
  if (isa<FuncDecl>(D)) {
    return IsModuleScopeContext;
  }

  if (auto *VD = dyn_cast<VarDecl>(D)) {
    if (!IsModuleScopeContext)
      return false;

    if (PatternBindingDecl *PBD = VD->getParentPatternBinding()) {
      return PBD->getDeclContext()->isModuleScopeContext();
    }
  }

  return false;
}

/// Walk the DeclContext hierarchy starting from D to find a declaration
/// at a member level (i.e., declared in a type context) on which to provide an
/// @available() Fix-It.
static const Decl *ancestorTypeLevelDeclForAvailabilityFixit(const Decl *D) {
  assert(D);

  D = relatedDeclForAvailabilityFixit(D);

  while (D && !isTypeLevelDeclForAvailabilityFixit(D)) {
    D = D->getDeclContext()->getInnermostDeclarationDeclContext();
  }

  return D;
}

/// Given the range of a reference to an unavailable symbol and the
/// declaration context containing the reference, make a best effort find up to
/// three locations for potential fixits.
///
/// \param FoundVersionCheckNode Returns a node that can be wrapped in a
/// if #available(...) { ... } version check to fix the unavailable reference,
/// or None if such a node cannot be found.
///
/// \param FoundMemberLevelDecl Returns member-level declaration (i.e., the
///  child of a type DeclContext) for which an @available attribute would
/// fix the unavailable reference.
///
/// \param FoundTypeLevelDecl returns a type-level declaration (a
/// a nominal type, an extension, or a global function) for which an
/// @available attribute would fix the unavailable reference.
static void findAvailabilityFixItNodes(
    SourceRange ReferenceRange, const DeclContext *ReferenceDC,
    const SourceManager &SM, llvm::Optional<ASTNode> &FoundVersionCheckNode,
    const Decl *&FoundMemberLevelDecl, const Decl *&FoundTypeLevelDecl) {
  FoundVersionCheckNode = llvm::None;
  FoundMemberLevelDecl = nullptr;
  FoundTypeLevelDecl = nullptr;

  // Limit tree to search based on the DeclContext of the reference.
  const Decl *DeclarationToSearch =
      findContainingDeclaration(ReferenceRange, ReferenceDC, SM);
  if (!DeclarationToSearch)
    return;

  // Const-cast to inject into ASTNode. This search will not modify
  // the declaration.
  ASTNode SearchRoot = const_cast<Decl *>(DeclarationToSearch);

  // The node to wrap in if #available(...) { ... } is the innermost node in
  // SearchRoot that (1) can be guarded with an if statement and (2)
  // contains the ReferenceRange.
  // We make no guarantee that the Fix-It, when applied, will result in
  // semantically valid code -- but, at a minimum, it should parse. So,
  // for example, we may suggest wrapping a variable declaration in a guard,
  // which would not be valid if the variable is later used. The goal
  // is discoverability of #os() (via the diagnostic and Fix-It) rather than
  // magically fixing the code in all cases.

  InnermostAncestorFinder::MatchPredicate IsGuardable =
      [](ASTNode Node, ASTWalker::ParentTy Parent) {
        if (Expr *ParentExpr = Parent.getAsExpr()) {
          auto *ParentClosure = dyn_cast<ClosureExpr>(ParentExpr);
          if (!ParentClosure ||
              ParentClosure->isSeparatelyTypeChecked()) {
            return false;
          }
        } else if (auto *ParentStmt = Parent.getAsStmt()) {
          if (!isa<BraceStmt>(ParentStmt)) {
            return false;
          }
        } else {
          return false;
        }

        return true;
      };

  FoundVersionCheckNode =
      findInnermostAncestor(ReferenceRange, SM, SearchRoot, IsGuardable);

  // Try to find declarations on which @available attributes can be added.
  // The heuristics for finding these declarations are biased towards deeper
  // nodes in the AST to limit the scope of suggested availability regions
  // and provide a better IDE experience (it can get jumpy if Fix-It locations
  // are far away from the error needing the Fix-It).
  if (DeclarationToSearch) {
    FoundMemberLevelDecl =
        ancestorMemberLevelDeclForAvailabilityFixit(DeclarationToSearch);

    FoundTypeLevelDecl =
        ancestorTypeLevelDeclForAvailabilityFixit(DeclarationToSearch);
  }
}

/// Emit a diagnostic note and Fix-It to add an @available attribute
/// on the given declaration for the given version range.
static void fixAvailabilityForDecl(SourceRange ReferenceRange, const Decl *D,
                                   const VersionRange &RequiredRange,
                                   ASTContext &Context) {
  assert(D);

  // Don't suggest adding an @available() to a declaration where we would
  // emit a diagnostic saying it is not allowed.
  if (TypeChecker::diagnosticIfDeclCannotBePotentiallyUnavailable(D).has_value())
    return;

  if (getActiveAvailableAttribute(D, Context)) {
    // For QoI, in future should emit a fixit to update the existing attribute.
    return;
  }

  // For some declarations (variables, enum elements), the location in concrete
  // syntax to suggest the Fix-It may differ from the declaration to which
  // we attach availability attributes in the abstract syntax tree during
  // parsing.
  const Decl *ConcDecl = concreteSyntaxDeclForAvailableAttribute(D);

  DescriptiveDeclKind KindForDiagnostic = ConcDecl->getDescriptiveKind();
  SourceLoc InsertLoc;

  // To avoid exposing the pattern binding declaration to the user, get the
  // descriptive kind from one of the VarDecls. We get the Fix-It location
  // from the PatternBindingDecl unless the VarDecl has attributes,
  // in which case we get the start location of the VarDecl attributes.
  DeclAttributes AttrsForLoc;
  if (KindForDiagnostic == DescriptiveDeclKind::PatternBinding) {
    KindForDiagnostic = D->getDescriptiveKind();
    AttrsForLoc = D->getAttrs();
  } else {
    InsertLoc = ConcDecl->getAttrs().getStartLoc(/*forModifiers=*/false);
  }

  InsertLoc = D->getAttrs().getStartLoc(/*forModifiers=*/false);
  if (InsertLoc.isInvalid()) {
    InsertLoc = ConcDecl->getStartLoc();
  }

  if (InsertLoc.isInvalid())
    return;

  StringRef OriginalIndent =
      Lexer::getIndentationForLine(Context.SourceMgr, InsertLoc);
  PlatformKind Target = targetPlatform(Context.LangOpts);

  D->diagnose(diag::availability_add_attribute, KindForDiagnostic)
      .fixItInsert(InsertLoc, diag::insert_available_attr,
                   platformString(Target),
                   RequiredRange.getLowerEndpoint().getAsString(),
                   OriginalIndent);
}

/// In the special case of being in an existing, nontrivial type refinement
/// context that's close but not quite narrow enough to satisfy requirements
/// (i.e.  requirements are contained-in the existing TRC but off by a subminor
/// version), emit a diagnostic and fixit that narrows the existing TRC
/// condition to the required range.
static bool fixAvailabilityByNarrowingNearbyVersionCheck(
    SourceRange ReferenceRange,
    const DeclContext *ReferenceDC,
    const VersionRange &RequiredRange,
    ASTContext &Context,
    InFlightDiagnostic &Err) {
  const TypeRefinementContext *TRC = nullptr;
  (void)TypeChecker::overApproximateAvailabilityAtLocation(ReferenceRange.Start,
                                                           ReferenceDC, &TRC);
  if (!TRC)
    return false;
  VersionRange RunningRange = TRC->getExplicitAvailabilityInfo().getOSVersion();
  if (RunningRange.hasLowerEndpoint() &&
      RequiredRange.hasLowerEndpoint() &&
      TRC->getReason() != TypeRefinementContext::Reason::Root &&
      AvailabilityContext(RequiredRange).isContainedIn(
                                 AvailabilityContext(RunningRange))) {

    // Only fix situations that are "nearby" versions, meaning
    // disagreement on a minor-or-less version for non-macOS,
    // or disagreement on a subminor-or-less version for macOS.
    auto RunningVers = RunningRange.getLowerEndpoint();
    auto RequiredVers = RequiredRange.getLowerEndpoint();
    auto Platform = targetPlatform(Context.LangOpts);
    if (RunningVers.getMajor() != RequiredVers.getMajor())
      return false;
    if ((Platform == PlatformKind::macOS ||
         Platform == PlatformKind::macOSApplicationExtension) &&
        !(RunningVers.getMinor().has_value() &&
          RequiredVers.getMinor().has_value() &&
          RunningVers.getMinor().value() ==
          RequiredVers.getMinor().value()))
      return false;

    auto FixRange = TRC->getAvailabilityConditionVersionSourceRange(
      Platform, RunningVers);
    if (!FixRange.isValid())
      return false;
    // Have found a nontrivial type refinement context-introducer to narrow.
    Err.fixItReplace(FixRange, RequiredVers.getAsString());
    return true;
  }
  return false;
}

/// Emit a diagnostic note and Fix-It to add an if #available(...) { } guard
/// that checks for the given version range around the given node.
static void fixAvailabilityByAddingVersionCheck(
    ASTNode NodeToWrap, const VersionRange &RequiredRange,
    SourceRange ReferenceRange, ASTContext &Context) {
  // If this is an implicit variable that wraps an expression,
  // let's point to it's initializer. For example, result builder
  // transform captures expressions into implicit variables.
  if (auto *PB =
          dyn_cast_or_null<PatternBindingDecl>(NodeToWrap.dyn_cast<Decl *>())) {
    if (PB->isImplicit() && PB->getSingleVar()) {
      if (auto *init = PB->getInit(0))
        NodeToWrap = init;
    }
  }

  SourceRange RangeToWrap = NodeToWrap.getSourceRange();
  if (RangeToWrap.isInvalid())
    return;

  SourceLoc ReplaceLocStart = RangeToWrap.Start;
  StringRef ExtraIndent;
  StringRef OriginalIndent = Lexer::getIndentationForLine(
      Context.SourceMgr, ReplaceLocStart, &ExtraIndent);

  std::string IfText;
  {
    llvm::raw_string_ostream Out(IfText);

    SourceLoc ReplaceLocEnd =
        Lexer::getLocForEndOfToken(Context.SourceMgr, RangeToWrap.End);

    std::string GuardedText =
        Context.SourceMgr.extractText(CharSourceRange(Context.SourceMgr,
                                                      ReplaceLocStart,
                                                      ReplaceLocEnd)).str();

    std::string NewLine = "\n";
    std::string NewLineReplacement = (NewLine + ExtraIndent).str();

    // Indent the body of the Fix-It if. Because the body may be a compound
    // statement, we may have to indent multiple lines.
    size_t StartAt = 0;
    while ((StartAt = GuardedText.find(NewLine, StartAt)) !=
           std::string::npos) {
      GuardedText.replace(StartAt, NewLine.length(), NewLineReplacement);
      StartAt += NewLine.length();
    }

    PlatformKind Target = targetPlatform(Context.LangOpts);

    Out << "if #available(" << platformString(Target)
        << " " << RequiredRange.getLowerEndpoint().getAsString()
        << ", *) {\n";

    Out << OriginalIndent << ExtraIndent << GuardedText << "\n";

    // We emit an empty fallback case with a comment to encourage the developer
    // to think explicitly about whether fallback on earlier versions is needed.
    Out << OriginalIndent << "} else {\n";
    Out << OriginalIndent << ExtraIndent << "// Fallback on earlier versions\n";
    Out << OriginalIndent << "}";
  }

  Context.Diags.diagnose(
      ReferenceRange.Start, diag::availability_guard_with_version_check)
      .fixItReplace(RangeToWrap, IfText);
}

/// Emit suggested Fix-Its for a reference with to an unavailable symbol
/// requiting the given OS version range.
static void fixAvailability(SourceRange ReferenceRange,
                            const DeclContext *ReferenceDC,
                            const VersionRange &RequiredRange,
                            ASTContext &Context) {
  if (ReferenceRange.isInvalid())
    return;

  llvm::Optional<ASTNode> NodeToWrapInVersionCheck;
  const Decl *FoundMemberDecl = nullptr;
  const Decl *FoundTypeLevelDecl = nullptr;

  findAvailabilityFixItNodes(ReferenceRange, ReferenceDC, Context.SourceMgr,
                             NodeToWrapInVersionCheck, FoundMemberDecl,
                             FoundTypeLevelDecl);

  // Suggest wrapping in if #available(...) { ... } if possible.
  if (NodeToWrapInVersionCheck.has_value()) {
    fixAvailabilityByAddingVersionCheck(NodeToWrapInVersionCheck.value(),
                                        RequiredRange, ReferenceRange, Context);
  }

  // Suggest adding availability attributes.
  if (FoundMemberDecl) {
    fixAvailabilityForDecl(ReferenceRange, FoundMemberDecl, RequiredRange,
                           Context);
  }

  if (FoundTypeLevelDecl) {
    fixAvailabilityForDecl(ReferenceRange, FoundTypeLevelDecl, RequiredRange,
                           Context);
  }
}

void TypeChecker::diagnosePotentialUnavailability(
    SourceRange ReferenceRange, Diag<StringRef, llvm::VersionTuple> Diag,
    const DeclContext *ReferenceDC,
    const UnavailabilityReason &Reason) {
  ASTContext &Context = ReferenceDC->getASTContext();

  auto RequiredRange = Reason.getRequiredOSVersionRange();
  {
    auto Err =
      Context.Diags.diagnose(
               ReferenceRange.Start, Diag,
               prettyPlatformString(targetPlatform(Context.LangOpts)),
               Reason.getRequiredOSVersionRange().getLowerEndpoint());

    // Direct a fixit to the error if an existing guard is nearly-correct
    if (fixAvailabilityByNarrowingNearbyVersionCheck(
        ReferenceRange, ReferenceDC, RequiredRange, Context, Err))
      return;
  }
  fixAvailability(ReferenceRange, ReferenceDC, RequiredRange, Context);
}

bool TypeChecker::checkAvailability(SourceRange ReferenceRange,
                                    AvailabilityContext Availability,
                                    Diag<StringRef, llvm::VersionTuple> Diag,
                                    const DeclContext *ReferenceDC) {
  ASTContext &ctx = ReferenceDC->getASTContext();
  if (ctx.LangOpts.DisableAvailabilityChecking)
    return false;

  auto runningOS =
    TypeChecker::overApproximateAvailabilityAtLocation(
      ReferenceRange.Start, ReferenceDC);
  if (!runningOS.isContainedIn(Availability)) {
    diagnosePotentialUnavailability(
      ReferenceRange, Diag, ReferenceDC,
      UnavailabilityReason::requiresVersionRange(Availability.getOSVersion()));
    return true;
  }

  return false;
}

void TypeChecker::checkConcurrencyAvailability(SourceRange ReferenceRange,
                                               const DeclContext *ReferenceDC) {
  checkAvailability(
      ReferenceRange,
      ReferenceDC->getASTContext().getBackDeployedConcurrencyAvailability(),
      diag::availability_concurrency_only_version_newer,
      ReferenceDC);
}

/// Returns the diagnostic to emit for the potentially unavailable decl and sets
/// \p IsError accordingly.
static Diagnostic getPotentialUnavailabilityDiagnostic(
    const ValueDecl *D, const DeclContext *ReferenceDC,
    const UnavailabilityReason &Reason, bool WarnBeforeDeploymentTarget,
    bool &IsError) {
  ASTContext &Context = ReferenceDC->getASTContext();
  auto Platform = prettyPlatformString(targetPlatform(Context.LangOpts));
  auto Version = Reason.getRequiredOSVersionRange().getLowerEndpoint();

  if (Reason.requiresDeploymentTargetOrEarlier(Context)) {
    // The required OS version is at or before the deployment target so this
    // diagnostic should indicate that the decl could be unavailable to clients
    // of the module containing the reference.
    IsError = !WarnBeforeDeploymentTarget;

    return Diagnostic(
        IsError ? diag::availability_decl_only_version_newer_for_clients
                : diag::availability_decl_only_version_newer_for_clients_warn,
        D->getName(), Platform, Version, ReferenceDC->getParentModule());
  }

  IsError = true;
  return Diagnostic(diag::availability_decl_only_version_newer, D->getName(),
                    Platform, Version);
}

bool TypeChecker::diagnosePotentialUnavailability(
    const ValueDecl *D, SourceRange ReferenceRange,
    const DeclContext *ReferenceDC,
    const UnavailabilityReason &Reason,
    bool WarnBeforeDeploymentTarget = false) {
  ASTContext &Context = ReferenceDC->getASTContext();

  auto RequiredRange = Reason.getRequiredOSVersionRange();
  bool IsError;
  {
    auto Diag = Context.Diags.diagnose(
        ReferenceRange.Start,
        getPotentialUnavailabilityDiagnostic(
            D, ReferenceDC, Reason, WarnBeforeDeploymentTarget, IsError));

    // Direct a fixit to the error if an existing guard is nearly-correct
    if (fixAvailabilityByNarrowingNearbyVersionCheck(
            ReferenceRange, ReferenceDC, RequiredRange, Context, Diag))
      return IsError;
  }

  fixAvailability(ReferenceRange, ReferenceDC, RequiredRange, Context);
  return IsError;
}

void TypeChecker::diagnosePotentialAccessorUnavailability(
    const AccessorDecl *Accessor, SourceRange ReferenceRange,
    const DeclContext *ReferenceDC, const UnavailabilityReason &Reason,
    bool ForInout) {
  ASTContext &Context = ReferenceDC->getASTContext();

  assert(Accessor->isGetterOrSetter());

  const AbstractStorageDecl *ASD = Accessor->getStorage();
  DeclName Name = ASD->getName();

  auto &diag = ForInout ? diag::availability_inout_accessor_only_version_newer
                        : diag::availability_accessor_only_version_newer;

  auto RequiredRange = Reason.getRequiredOSVersionRange();
  {
    auto Err =
      Context.Diags.diagnose(
               ReferenceRange.Start, diag,
               static_cast<unsigned>(Accessor->getAccessorKind()), Name,
               prettyPlatformString(targetPlatform(Context.LangOpts)),
               Reason.getRequiredOSVersionRange().getLowerEndpoint());


    // Direct a fixit to the error if an existing guard is nearly-correct
    if (fixAvailabilityByNarrowingNearbyVersionCheck(ReferenceRange,
                                                     ReferenceDC,
                                                     RequiredRange, Context, Err))
      return;
  }

  fixAvailability(ReferenceRange, ReferenceDC, RequiredRange, Context);
}

static DiagnosticBehavior
behaviorLimitForExplicitUnavailability(
    const RootProtocolConformance *rootConf,
    const DeclContext *fromDC) {
  auto protoDecl = rootConf->getProtocol();

  // Soften errors about unavailable `Sendable` conformances depending on the
  // concurrency checking mode.
  if (protoDecl->isSpecificProtocol(KnownProtocolKind::Sendable)) {
    SendableCheckContext checkContext(fromDC);
    if (auto nominal = rootConf->getType()->getAnyNominal())
      return checkContext.diagnosticBehavior(nominal);

    return checkContext.defaultDiagnosticBehavior();
  }

  return DiagnosticBehavior::Unspecified;
}

void TypeChecker::diagnosePotentialUnavailability(
    const RootProtocolConformance *rootConf,
    const ExtensionDecl *ext,
    SourceLoc loc,
    const DeclContext *dc,
    const UnavailabilityReason &reason) {
  ASTContext &ctx = dc->getASTContext();

  auto requiredRange = reason.getRequiredOSVersionRange();
  {
    auto type = rootConf->getType();
    auto proto = rootConf->getProtocol()->getDeclaredInterfaceType();

    auto diagID = (ctx.LangOpts.EnableConformanceAvailabilityErrors
                   ? diag::conformance_availability_only_version_newer
                   : diag::conformance_availability_only_version_newer_warn);
    auto behavior = behaviorLimitForExplicitUnavailability(rootConf, dc);
    auto err =
      ctx.Diags.diagnose(
               loc, diagID,
               type, proto, prettyPlatformString(targetPlatform(ctx.LangOpts)),
               reason.getRequiredOSVersionRange().getLowerEndpoint());
    err.limitBehavior(behavior);

    // Direct a fixit to the error if an existing guard is nearly-correct
    if (fixAvailabilityByNarrowingNearbyVersionCheck(loc, dc,
                                                     requiredRange, ctx, err))
      return;
  }

  fixAvailability(loc, dc, requiredRange, ctx);
}

const AvailableAttr *TypeChecker::getDeprecated(const Decl *D) {
  if (auto *Attr = D->getAttrs().getDeprecated(D->getASTContext()))
    return Attr;

  // Treat extensions methods as deprecated if their extension
  // is deprecated.
  DeclContext *DC = D->getDeclContext();
  if (auto *ED = dyn_cast<ExtensionDecl>(DC)) {
    return getDeprecated(ED);
  }

  return nullptr;
}

static void fixItAvailableAttrRename(InFlightDiagnostic &diag,
                                     SourceRange referenceRange,
                                     const ValueDecl *renamedDecl,
                                     const AvailableAttr *attr,
                                     const Expr *call) {
  if (isa<AccessorDecl>(renamedDecl))
    return;

  ParsedDeclName parsed = swift::parseDeclName(attr->Rename);
  if (!parsed)
    return;

  bool originallyWasKnownOperatorExpr = false;
  if (call) {
    originallyWasKnownOperatorExpr =
        isa<BinaryExpr>(call) ||
        isa<PrefixUnaryExpr>(call) ||
        isa<PostfixUnaryExpr>(call);
  }
  if (parsed.isOperator() != originallyWasKnownOperatorExpr)
    return;

  auto &ctx = renamedDecl->getASTContext();
  SourceManager &sourceMgr = ctx.SourceMgr;
  if (parsed.isInstanceMember()) {
    auto *CE = dyn_cast_or_null<CallExpr>(call);
    if (!CE)
      return;

    // Replace the base of the call with the "self argument".
    // We can only do a good job with the fix-it if we have the whole call
    // expression.
    // FIXME: Should we be validating the ContextName in some way?
    unsigned selfIndex = parsed.SelfIndex.value();
    const Expr *selfExpr = nullptr;
    SourceLoc removeRangeStart;
    SourceLoc removeRangeEnd;

    auto *originalArgs = CE->getArgs()->getOriginalArgs();
    size_t numElementsWithinParens = originalArgs->size();
    numElementsWithinParens -= originalArgs->getNumTrailingClosures();
    if (selfIndex >= numElementsWithinParens)
      return;

    if (parsed.IsGetter) {
      if (numElementsWithinParens != 1)
        return;
    } else if (parsed.IsSetter) {
      if (numElementsWithinParens != 2)
        return;
    } else {
      if (parsed.ArgumentLabels.size() != originalArgs->size() - 1)
        return;
    }

    selfExpr = originalArgs->getExpr(selfIndex);

    if (selfIndex + 1 == numElementsWithinParens) {
      if (selfIndex > 0) {
        // Remove from the previous comma to the close-paren (half-open).
        removeRangeStart = originalArgs->getExpr(selfIndex - 1)->getEndLoc();
        removeRangeStart = Lexer::getLocForEndOfToken(sourceMgr,
                                                      removeRangeStart);
      } else {
        // Remove from after the open paren to the close paren (half-open).
        removeRangeStart =
            Lexer::getLocForEndOfToken(sourceMgr, originalArgs->getStartLoc());
      }

      // Prefer the r-paren location, so that we get the right behavior when
      // there's a trailing closure, but handle some implicit cases too.
      removeRangeEnd = originalArgs->getRParenLoc();
      if (removeRangeEnd.isInvalid())
        removeRangeEnd = originalArgs->getEndLoc();

    } else {
      // Remove from the label to the start of the next argument (half-open).
      SourceLoc labelLoc = originalArgs->getLabelLoc(selfIndex);
      if (labelLoc.isValid())
        removeRangeStart = labelLoc;
      else
        removeRangeStart = selfExpr->getStartLoc();

      SourceLoc nextLabelLoc = originalArgs->getLabelLoc(selfIndex + 1);
      if (nextLabelLoc.isValid())
        removeRangeEnd = nextLabelLoc;
      else
        removeRangeEnd = originalArgs->getExpr(selfIndex + 1)->getStartLoc();
    }

    // Avoid later argument label fix-its for this argument.
    if (!parsed.isPropertyAccessor()) {
      Identifier oldLabel = originalArgs->getLabel(selfIndex);
      StringRef oldLabelStr;
      if (!oldLabel.empty())
        oldLabelStr = oldLabel.str();
      parsed.ArgumentLabels.insert(parsed.ArgumentLabels.begin() + selfIndex,
                                    oldLabelStr);
    }

    if (auto *inoutSelf = dyn_cast<InOutExpr>(selfExpr))
      selfExpr = inoutSelf->getSubExpr();

    CharSourceRange selfExprRange =
        Lexer::getCharSourceRangeFromSourceRange(sourceMgr,
                                                 selfExpr->getSourceRange());
    bool needsParens = !selfExpr->canAppendPostfixExpression();

    SmallString<64> selfReplace;
    if (needsParens)
      selfReplace.push_back('(');

    // If the base is contextual member lookup and we know the type,
    // let's just prepend it, otherwise we'll end up with an incorrect fix-it.
    auto base = sourceMgr.extractText(selfExprRange);
    if (!base.empty() && base.front() == '.') {
      auto newName = attr->Rename;
      // If this is not a rename, let's not
      // even try to emit a fix-it because
      // it's going to be invalid.
      if (newName.empty())
        return;

      auto parts = newName.split('.');
      auto nominalName = parts.first;
      assert(!nominalName.empty());

      selfReplace += nominalName;
    }

    selfReplace += base;
    if (needsParens)
      selfReplace.push_back(')');

    selfReplace.push_back('.');
    selfReplace += parsed.BaseName;

    diag.fixItReplace(CE->getFn()->getSourceRange(), selfReplace);

    if (!parsed.isPropertyAccessor())
      diag.fixItRemoveChars(removeRangeStart, removeRangeEnd);

    // Continue on to diagnose any argument label renames.

  } else if (parsed.BaseName == "init" && isa_and_nonnull<CallExpr>(call)) {
    auto *CE = cast<CallExpr>(call);

    // If it is a call to an initializer (rather than a first-class reference):

    if (parsed.isMember()) {
      // replace with a "call" to the type (instead of writing `.init`)
      diag.fixItReplace(CE->getFn()->getSourceRange(), parsed.ContextName);
    } else if (auto *dotCall = dyn_cast<DotSyntaxCallExpr>(CE->getFn())) {
      // if it's a dot call, and the left side is a type (and not `self` or 
      // `super`, for example), just remove the dot and the right side, again 
      // in order to make it a "call" to the type
      if (isa<TypeExpr>(dotCall->getBase())) {
        SourceLoc removeLoc = dotCall->getDotLoc();
        if (removeLoc.isInvalid())
          return;

        diag.fixItRemove(SourceRange(removeLoc, dotCall->getFn()->getEndLoc()));
      }
    } else if (!isa<ConstructorRefCallExpr>(CE->getFn())) {
      return;
    }

    // Continue on to diagnose any constructor argument label renames.

  } else if (parsed.IsSubscript) {
    if (auto *CE = dyn_cast_or_null<CallExpr>(call)) {
      // Renaming from CallExpr to SubscriptExpr. Remove function name and
      // replace parens with square brackets.

      if (auto *DSCE = dyn_cast<DotSyntaxCallExpr>(CE->getFn())) {
        if (DSCE->getBase()->isImplicit()) {
          // If self is implicit, self must be inserted before subscript syntax.
          diag.fixItInsert(CE->getStartLoc(), "self");
        }
      }

      diag.fixItReplace(CE->getFn()->getEndLoc(), "[");
      diag.fixItReplace(CE->getEndLoc(), "]");
    }
  } else {
    // Just replace the base name.
    SmallString<64> baseReplace;

    if (!parsed.ContextName.empty()) {
      baseReplace += parsed.ContextName;
      baseReplace += '.';
    }
    baseReplace += parsed.BaseName;

    if (parsed.IsFunctionName && isa_and_nonnull<SubscriptExpr>(call)) {
      auto *SE = cast<SubscriptExpr>(call);

      // Renaming from SubscriptExpr to CallExpr. Insert function name and
      // replace square brackets with parens.
      diag.fixItReplace(SE->getArgs()->getStartLoc(),
                        ("." + baseReplace.str() + "(").str());
      diag.fixItReplace(SE->getEndLoc(), ")");
    } else {
      bool shouldEmitRenameFixit = true;
      if (auto *CE = dyn_cast_or_null<CallExpr>(call)) {
        SmallString<64> callContextName;
        llvm::raw_svector_ostream name(callContextName);
        if (auto *DCE = dyn_cast<DotSyntaxCallExpr>(CE->getDirectCallee())) {
          if (auto *TE = dyn_cast<TypeExpr>(DCE->getBase())) {
            TE->getTypeRepr()->print(name);
            if (!parsed.ContextName.empty()) {
              // If there is a context in rename function e.g.
              // `Context.function()` and call context is a `DotSyntaxCallExpr`
              // adjust the range so it replaces the base as well.
              referenceRange =
                  SourceRange(TE->getStartLoc(), referenceRange.End);
            }
          }
        }
        // Function names are the same(including context if applicable), so
        // renaming fix-it doesn't need do be produced.
        if ((parsed.ContextName.empty() ||
             parsed.ContextName == callContextName) &&
            CE->getCalledValue()->getBaseName() == parsed.BaseName) {
          shouldEmitRenameFixit = false;
        }
      }
      if (shouldEmitRenameFixit) {
        if (parsed.IsFunctionName && parsed.ArgumentLabels.empty() &&
            isa<VarDecl>(renamedDecl)) {
          // If we're going from a var to a function with no arguments, emit an
          // empty parameter list.
          baseReplace += "()";
        }

        diag.fixItReplace(referenceRange, baseReplace);
      }
    }
  }

  if (!call || !call->getArgs())
    return;

  auto *originalArgs = call->getArgs()->getOriginalArgs();
  if (parsed.IsGetter) {
    diag.fixItRemove(originalArgs->getSourceRange());
    return;
  }

  if (parsed.IsSetter) {
    const Expr *newValueExpr = nullptr;

    if (originalArgs->size() >= 1) {
      size_t newValueIndex = 0;
      if (parsed.isInstanceMember()) {
        assert(parsed.SelfIndex.value() == 0 ||
               parsed.SelfIndex.value() == 1);
        newValueIndex = !parsed.SelfIndex.value();
      }
      newValueExpr = originalArgs->getExpr(newValueIndex);
    } else {
      newValueExpr = originalArgs->getExpr(0);
    }

    diag.fixItReplaceChars(originalArgs->getStartLoc(),
                           newValueExpr->getStartLoc(), " = ");
    diag.fixItRemoveChars(
        Lexer::getLocForEndOfToken(sourceMgr, newValueExpr->getEndLoc()),
        Lexer::getLocForEndOfToken(sourceMgr, originalArgs->getEndLoc()));
    return;
  }

  if (!parsed.IsFunctionName)
    return;

  SmallVector<Identifier, 4> argumentLabelIDs;
  llvm::transform(parsed.ArgumentLabels, std::back_inserter(argumentLabelIDs),
                  [&ctx](StringRef labelStr) -> Identifier {
                    return labelStr.empty() ? Identifier()
                                            : ctx.getIdentifier(labelStr);
                  });

  // Coerce the `argumentLabelIDs` to the user supplied arguments.
  // e.g:
  //   @available(.., renamed: "new(w:x:y:z:)")
  //   func old(a: Int, b: Int..., c: String="", d: Int=0){}
  //   old(a: 1, b: 2, 3, 4, d: 5)
  // coerce
  //   argumentLabelIDs = {"w", "x", "y", "z"}
  // to
  //   argumentLabelIDs = {"w", "x", "", "", "z"}
  auto I = argumentLabelIDs.begin();

  auto updateLabelsForArg = [&](Expr *expr) -> bool {
    if (I == argumentLabelIDs.end())
      return true;

    if (isa<DefaultArgumentExpr>(expr)) {
      // Defaulted: remove param label of it.
      I = argumentLabelIDs.erase(I);
      return false;
    }

    if (auto *varargExpr = dyn_cast<VarargExpansionExpr>(expr)) {
      if (auto *arrayExpr = dyn_cast<ArrayExpr>(varargExpr->getSubExpr())) {
        auto variadicArgsNum = arrayExpr->getNumElements();
        if (variadicArgsNum == 0) {
          // No arguments: Remove param label of it.
          I = argumentLabelIDs.erase(I);
        } else if (variadicArgsNum == 1) {
          // One argument: Just advance.
          ++I;
        } else {
          ++I;

          // Two or more arguments: Insert empty labels after the first one.
          --variadicArgsNum;
          I = argumentLabelIDs.insert(I, variadicArgsNum, Identifier());
          I += variadicArgsNum;
        }
        return false;
      }
    }

    // Normal: Just advance.
    ++I;
    return false;
  };

  for (auto arg : *call->getArgs()) {
    if (updateLabelsForArg(arg.getExpr()))
      return;
  }

  if (argumentLabelIDs.size() != originalArgs->size()) {
    // Mismatched lengths; give up.
    return;
  }

  // If any of the argument labels are mismatched, perform label correction.
  for (auto i : indices(*originalArgs)) {
    // The argument label of an unlabeled trailing closure is ignored.
    if (originalArgs->isUnlabeledTrailingClosureIndex(i))
      continue;
    if (argumentLabelIDs[i] != originalArgs->getLabel(i)) {
      auto paramContext = parsed.IsSubscript ? ParameterContext::Subscript
                                             : ParameterContext::Call;
      diagnoseArgumentLabelError(ctx, originalArgs, argumentLabelIDs,
                                 paramContext, &diag);
      return;
    }
  }
}

// Must be kept in sync with diag::availability_decl_unavailable_rename and
// others.
namespace {
  enum class ReplacementDeclKind : unsigned {
    None,
    InstanceMethod,
    Property,
  };
} // end anonymous namespace

static llvm::Optional<ReplacementDeclKind>
describeRename(ASTContext &ctx, const AvailableAttr *attr, const ValueDecl *D,
               SmallVectorImpl<char> &nameBuf) {
  ParsedDeclName parsed = swift::parseDeclName(attr->Rename);
  if (!parsed)
    return llvm::None;

  // Only produce special descriptions for renames to
  // - instance members
  // - properties (or global bindings)
  // - class/static methods
  // - initializers, unless the original was known to be an initializer
  // Leave non-member renames alone, as well as renames from top-level types
  // and bindings to member types and class/static properties.
  if (!(parsed.isInstanceMember() || parsed.isPropertyAccessor() ||
        (parsed.isMember() && parsed.IsFunctionName) ||
        (parsed.BaseName == "init" &&
         !dyn_cast_or_null<ConstructorDecl>(D)))) {
    return llvm::None;
  }

  llvm::raw_svector_ostream name(nameBuf);

  if (!parsed.ContextName.empty())
    name << parsed.ContextName << '.';

  if (parsed.IsFunctionName) {
    name << parsed.formDeclName(ctx, (D && isa<SubscriptDecl>(D)));
  } else {
    name << parsed.BaseName;
  }

  if (parsed.isMember() && parsed.isPropertyAccessor())
    return ReplacementDeclKind::Property;
  if (parsed.isInstanceMember() && parsed.IsFunctionName)
    return ReplacementDeclKind::InstanceMethod;

  // We don't have enough information.
  return ReplacementDeclKind::None;
}

void TypeChecker::diagnoseIfDeprecated(SourceRange ReferenceRange,
                                       const ExportContext &Where,
                                       const ValueDecl *DeprecatedDecl,
                                       const Expr *Call) {
  const AvailableAttr *Attr = TypeChecker::getDeprecated(DeprecatedDecl);
  if (!Attr)
    return;

  // We match the behavior of clang to not report deprecation warnings
  // inside declarations that are themselves deprecated on all deployment
  // targets.
  if (Where.isDeprecated()) {
    return;
  }

  auto *ReferenceDC = Where.getDeclContext();
  auto &Context = ReferenceDC->getASTContext();
  if (!Context.LangOpts.DisableAvailabilityChecking) {
    AvailabilityContext RunningOSVersions = Where.getAvailabilityContext();
    if (RunningOSVersions.isKnownUnreachable()) {
      // Suppress a deprecation warning if the availability checking machinery
      // thinks the reference program location will not execute on any
      // deployment target for the current platform.
      return;
    }
  }

  DeclName Name;
  unsigned RawAccessorKind;
  std::tie(RawAccessorKind, Name) =
      getAccessorKindAndNameForDiagnostics(DeprecatedDecl);

  StringRef Platform = Attr->prettyPlatformString();
  llvm::VersionTuple DeprecatedVersion;
  if (Attr->Deprecated)
    DeprecatedVersion = Attr->Deprecated.value();

  if (Attr->Message.empty() && Attr->Rename.empty()) {
    Context.Diags.diagnose(
             ReferenceRange.Start, diag::availability_deprecated,
             RawAccessorKind, Name, Attr->hasPlatform(), Platform,
             Attr->Deprecated.has_value(), DeprecatedVersion,
             /*message*/ StringRef())
        .highlight(Attr->getRange());
    return;
  }

  SmallString<32> newNameBuf;
  llvm::Optional<ReplacementDeclKind> replacementDeclKind =
      describeRename(Context, Attr, /*decl*/ nullptr, newNameBuf);
  StringRef newName = replacementDeclKind ? newNameBuf.str() : Attr->Rename;

  if (!Attr->Message.empty()) {
    EncodedDiagnosticMessage EncodedMessage(Attr->Message);
    Context.Diags.diagnose(
             ReferenceRange.Start, diag::availability_deprecated,
             RawAccessorKind, Name, Attr->hasPlatform(), Platform,
             Attr->Deprecated.has_value(), DeprecatedVersion,
             EncodedMessage.Message)
        .highlight(Attr->getRange());
  } else {
    unsigned rawReplaceKind = static_cast<unsigned>(
        replacementDeclKind.value_or(ReplacementDeclKind::None));
    Context.Diags.diagnose(
             ReferenceRange.Start, diag::availability_deprecated_rename,
             RawAccessorKind, Name, Attr->hasPlatform(), Platform,
             Attr->Deprecated.has_value(), DeprecatedVersion,
             replacementDeclKind.has_value(), rawReplaceKind, newName)
      .highlight(Attr->getRange());
  }

  if (!Attr->Rename.empty() && !isa<AccessorDecl>(DeprecatedDecl)) {
    auto renameDiag = Context.Diags.diagnose(
                               ReferenceRange.Start,
                               diag::note_deprecated_rename,
                               newName);
    fixItAvailableAttrRename(renameDiag, ReferenceRange, DeprecatedDecl,
                             Attr, Call);
  }
}

bool TypeChecker::diagnoseIfDeprecated(SourceLoc loc,
                                       const RootProtocolConformance *rootConf,
                                       const ExtensionDecl *ext,
                                       const ExportContext &where) {
  const AvailableAttr *attr = TypeChecker::getDeprecated(ext);
  if (!attr)
    return false;

  // We match the behavior of clang to not report deprecation warnings
  // inside declarations that are themselves deprecated on all deployment
  // targets.
  if (where.isDeprecated()) {
    return false;
  }

  auto *dc = where.getDeclContext();
  auto &ctx = dc->getASTContext();
  if (!ctx.LangOpts.DisableAvailabilityChecking) {
    AvailabilityContext runningOSVersion = where.getAvailabilityContext();
    if (runningOSVersion.isKnownUnreachable()) {
      // Suppress a deprecation warning if the availability checking machinery
      // thinks the reference program location will not execute on any
      // deployment target for the current platform.
      return false;
    }
  }

  auto type = rootConf->getType();
  auto proto = rootConf->getProtocol()->getDeclaredInterfaceType();

  StringRef platform = attr->prettyPlatformString();
  llvm::VersionTuple deprecatedVersion;
  if (attr->Deprecated)
    deprecatedVersion = attr->Deprecated.value();

  if (attr->Message.empty()) {
    ctx.Diags.diagnose(
             loc, diag::conformance_availability_deprecated,
             type, proto, attr->hasPlatform(), platform,
             attr->Deprecated.has_value(), deprecatedVersion,
             /*message*/ StringRef())
        .highlight(attr->getRange());
    return true;
  }

  EncodedDiagnosticMessage encodedMessage(attr->Message);
  ctx.Diags.diagnose(
      loc, diag::conformance_availability_deprecated,
      type, proto, attr->hasPlatform(), platform,
      attr->Deprecated.has_value(), deprecatedVersion,
      encodedMessage.Message)
    .highlight(attr->getRange());
  return true;
}

void swift::diagnoseOverrideOfUnavailableDecl(ValueDecl *override,
                                              const ValueDecl *base,
                                              const AvailableAttr *attr) {
  ASTContext &ctx = override->getASTContext();
  auto &diags = ctx.Diags;
  if (attr->Rename.empty()) {
    EncodedDiagnosticMessage EncodedMessage(attr->Message);
    diags.diagnose(override, diag::override_unavailable,
                   override->getBaseName(), EncodedMessage.Message);

    DeclName name;
    unsigned rawAccessorKind;
    std::tie(rawAccessorKind, name) =
        getAccessorKindAndNameForDiagnostics(base);
    diags.diagnose(base, diag::availability_marked_unavailable,
                   rawAccessorKind, name);
    return;
  }

  ExportContext where = ExportContext::forDeclSignature(override);
  diagnoseExplicitUnavailability(
      base, override->getLoc(), where,
      /*Flags*/ llvm::None, [&](InFlightDiagnostic &diag) {
        ParsedDeclName parsedName = parseDeclName(attr->Rename);
        if (!parsedName || parsedName.isPropertyAccessor() ||
            parsedName.isMember() || parsedName.isOperator()) {
          return;
        }

        // Only initializers should be named 'init'.
        if (isa<ConstructorDecl>(override) ^ (parsedName.BaseName == "init")) {
          return;
        }

        if (!parsedName.IsFunctionName) {
          diag.fixItReplace(override->getNameLoc(), parsedName.BaseName);
          return;
        }

        DeclName newName = parsedName.formDeclName(ctx);
        size_t numArgs = override->getName().getArgumentNames().size();
        if (!newName || newName.getArgumentNames().size() != numArgs)
          return;

        fixDeclarationName(diag, override, newName);
      });
}

/// Emit a diagnostic for references to declarations that have been
/// marked as unavailable, either through "unavailable" or "obsoleted:".
bool swift::diagnoseExplicitUnavailability(const ValueDecl *D, SourceRange R,
                                           const ExportContext &Where,
                                           const Expr *call,
                                           DeclAvailabilityFlags Flags) {
  return diagnoseExplicitUnavailability(D, R, Where, Flags,
                                        [=](InFlightDiagnostic &diag) {
    fixItAvailableAttrRename(diag, R, D, AvailableAttr::isUnavailable(D),
                             call);
  });
}

/// Emit a diagnostic for references to declarations that have been
/// marked as unavailable, either through "unavailable" or "obsoleted:".
bool swift::diagnoseExplicitUnavailability(SourceLoc loc,
                                           const RootProtocolConformance *rootConf,
                                           const ExtensionDecl *ext,
                                           const ExportContext &where,
                                           bool useConformanceAvailabilityErrorsOption) {
  auto *attr = AvailableAttr::isUnavailable(ext);
  if (!attr)
    return false;

  // Calling unavailable code from within code with the same
  // unavailability is OK -- the eventual caller can't call the
  // enclosing code in the same situations it wouldn't be able to
  // call this code.
  if (isInsideCompatibleUnavailableDeclaration(ext, where, attr))
    return false;

  ASTContext &ctx = ext->getASTContext();
  auto &diags = ctx.Diags;

  auto type = rootConf->getType();
  auto proto = rootConf->getProtocol()->getDeclaredInterfaceType();

  StringRef platform;
  auto behavior = DiagnosticBehavior::Unspecified;
  switch (attr->getPlatformAgnosticAvailability()) {
  case PlatformAgnosticAvailabilityKind::Deprecated:
    llvm_unreachable("shouldn't see deprecations in explicit unavailability");

  case PlatformAgnosticAvailabilityKind::NoAsync:
    llvm_unreachable("shouldn't see noasync in explicit unavailability");

  case PlatformAgnosticAvailabilityKind::None:
  case PlatformAgnosticAvailabilityKind::Unavailable:
    if (attr->Platform != PlatformKind::none) {
      // This was platform-specific; indicate the platform.
      platform = attr->prettyPlatformString();
      break;
    }

    // Downgrade unavailable Sendable conformance diagnostics where
    // appropriate.
    behavior = behaviorLimitForExplicitUnavailability(
        rootConf, where.getDeclContext());
    LLVM_FALLTHROUGH;

  case PlatformAgnosticAvailabilityKind::SwiftVersionSpecific:
  case PlatformAgnosticAvailabilityKind::PackageDescriptionVersionSpecific:
    // We don't want to give further detail about these.
    platform = "";
    break;

  case PlatformAgnosticAvailabilityKind::UnavailableInSwift:
    // This API is explicitly unavailable in Swift.
    platform = "Swift";
    break;
  }

  EncodedDiagnosticMessage EncodedMessage(attr->Message);
  diags.diagnose(loc, diag::conformance_availability_unavailable,
                 type, proto,
                 platform.empty(), platform, EncodedMessage.Message)
      .limitBehavior(behavior)
      .warnUntilSwiftVersionIf(useConformanceAvailabilityErrorsOption &&
                               !ctx.LangOpts.EnableConformanceAvailabilityErrors,
                               6);

  switch (attr->getVersionAvailability(ctx)) {
  case AvailableVersionComparison::Available:
  case AvailableVersionComparison::PotentiallyUnavailable:
    llvm_unreachable("These aren't considered unavailable");

  case AvailableVersionComparison::Unavailable:
    if ((attr->isLanguageVersionSpecific() ||
         attr->isPackageDescriptionVersionSpecific())
        && attr->Introduced.has_value())
      diags.diagnose(ext, diag::conformance_availability_introduced_in_version,
                     type, proto,
                     (attr->isLanguageVersionSpecific() ?
                      "Swift" : "PackageDescription"),
                     *attr->Introduced)
        .highlight(attr->getRange());
    else
      diags.diagnose(ext, diag::conformance_availability_marked_unavailable,
                     type, proto)
        .highlight(attr->getRange());
    break;

  case AvailableVersionComparison::Obsoleted:
    // FIXME: Use of the platformString here is non-awesome for application
    // extensions.

    StringRef platformDisplayString;
    if (attr->isLanguageVersionSpecific()) {
      platformDisplayString = "Swift";
    } else if (attr->isPackageDescriptionVersionSpecific()) {
      platformDisplayString = "PackageDescription";
    } else {
      platformDisplayString = platform;
    }

    diags.diagnose(ext, diag::conformance_availability_obsoleted,
                   type, proto, platformDisplayString, *attr->Obsoleted)
      .highlight(attr->getRange());
    break;
  }
  return true;
}

/// Check if this is a subscript declaration inside String or
/// Substring that returns String, and if so return true.
bool isSubscriptReturningString(const ValueDecl *D, ASTContext &Context) {
  // Is this a subscript?
  if (!isa<SubscriptDecl>(D))
    return false;

  // Is the subscript declared in String or Substring?
  auto *declContext = D->getDeclContext();
  assert(declContext && "Expected decl context!");

  auto *stringDecl = Context.getStringDecl();
  auto *substringDecl = Context.getSubstringDecl();

  auto *typeDecl = declContext->getSelfNominalTypeDecl();
  if (!typeDecl)
    return false;

  if (typeDecl != stringDecl && typeDecl != substringDecl)
    return false;

  // Is the subscript index one we want to emit a special diagnostic
  // for, and the return type String?
  auto fnTy = D->getInterfaceType()->getAs<AnyFunctionType>();
  assert(fnTy && "Expected function type for subscript decl!");

  // We're only going to warn for BoundGenericStructType with a single
  // type argument that is not Int!
  auto params = fnTy->getParams();
  if (params.size() != 1)
    return false;

  const auto &param = params.front();
  if (param.hasLabel() || param.isVariadic() || param.isInOut())
    return false;

  auto inputTy = param.getPlainType()->getAs<BoundGenericStructType>();
  if (!inputTy)
    return false;

  auto genericArgs = inputTy->getGenericArgs();
  if (genericArgs.size() != 1)
    return false;

  // The subscripts taking T<Int> do not return Substring, and our
  // special fixit does not help here.
  auto nominalTypeParam = genericArgs[0]->getAs<NominalType>();
  if (!nominalTypeParam)
    return false;

  if (nominalTypeParam->isInt())
    return false;

  auto resultTy = fnTy->getResult()->getAs<NominalType>();
  if (!resultTy)
    return false;

  return resultTy->isString();
}

bool swift::diagnoseParameterizedProtocolAvailability(
    SourceRange ReferenceRange, const DeclContext *ReferenceDC) {
  return TypeChecker::checkAvailability(
      ReferenceRange,
      ReferenceDC->getASTContext().getParameterizedExistentialRuntimeAvailability(),
      diag::availability_parameterized_protocol_only_version_newer,
      ReferenceDC);
}

static void
maybeDiagParameterizedExistentialErasure(ErasureExpr *EE,
                                         const ExportContext &Where) {
  if (auto *OE = dyn_cast<OpaqueValueExpr>(EE->getSubExpr())) {
    auto *OAT = OE->getType()->getAs<OpenedArchetypeType>();
    if (!OAT)
      return;

    auto opened = OAT->getGenericEnvironment()->getOpenedExistentialType();
    if (!opened || !opened->hasParameterizedExistential())
      return;

    (void)diagnoseParameterizedProtocolAvailability(EE->getLoc(),
                                                    Where.getDeclContext());
  }

  if (EE->getType() &&
      EE->getType()->isAny() &&
      EE->getSubExpr()->getType()->hasParameterizedExistential()) {
    (void)diagnoseParameterizedProtocolAvailability(EE->getLoc(),
                                                    Where.getDeclContext());
  }
}

bool swift::diagnoseExplicitUnavailability(
    const ValueDecl *D,
    SourceRange R,
    const ExportContext &Where,
    DeclAvailabilityFlags Flags,
    llvm::function_ref<void(InFlightDiagnostic &)> attachRenameFixIts) {
  auto *Attr = AvailableAttr::isUnavailable(D);
  if (!Attr)
    return false;

  // Calling unavailable code from within code with the same
  // unavailability is OK -- the eventual caller can't call the
  // enclosing code in the same situations it wouldn't be able to
  // call this code.
  if (isInsideCompatibleUnavailableDeclaration(D, Where, Attr))
    return false;

  SourceLoc Loc = R.Start;
  DeclName Name;
  unsigned RawAccessorKind;
  std::tie(RawAccessorKind, Name) = getAccessorKindAndNameForDiagnostics(D);

  ASTContext &ctx = D->getASTContext();
  auto &diags = ctx.Diags;

  StringRef platform;
  switch (Attr->getPlatformAgnosticAvailability()) {
  case PlatformAgnosticAvailabilityKind::Deprecated:
    llvm_unreachable("shouldn't see deprecations in explicit unavailability");

  case PlatformAgnosticAvailabilityKind::NoAsync:
    llvm_unreachable("shouldn't see noasync with explicit unavailability");

  case PlatformAgnosticAvailabilityKind::None:
  case PlatformAgnosticAvailabilityKind::Unavailable:
    if (Attr->Platform != PlatformKind::none) {
      // This was platform-specific; indicate the platform.
      platform = Attr->prettyPlatformString();
      break;
    }
    LLVM_FALLTHROUGH;

  case PlatformAgnosticAvailabilityKind::SwiftVersionSpecific:
  case PlatformAgnosticAvailabilityKind::PackageDescriptionVersionSpecific:
    // We don't want to give further detail about these.
    platform = "";
    break;

  case PlatformAgnosticAvailabilityKind::UnavailableInSwift:
    // This API is explicitly unavailable in Swift.
    platform = "Swift";
    break;
  }

  // TODO: Consider removing this.
  // ObjC keypaths components weren't checked previously, so errors are demoted
  // to warnings to avoid source breakage. In some cases unavailable or
  // obsolete decls still map to valid ObjC runtime names, so behave correctly
  // at runtime, even though their use would produce an error outside of a
  // #keyPath expression.
  bool warnInObjCKeyPath = Flags.contains(DeclAvailabilityFlag::ForObjCKeyPath);

  if (!Attr->Rename.empty()) {
    SmallString<32> newNameBuf;
    llvm::Optional<ReplacementDeclKind> replaceKind =
        describeRename(ctx, Attr, D, newNameBuf);
    unsigned rawReplaceKind = static_cast<unsigned>(
        replaceKind.value_or(ReplacementDeclKind::None));
    StringRef newName = replaceKind ? newNameBuf.str() : Attr->Rename;
      EncodedDiagnosticMessage EncodedMessage(Attr->Message);
      auto diag =
          diags.diagnose(Loc, warnInObjCKeyPath
                         ? diag::availability_decl_unavailable_rename_warn
                         : diag::availability_decl_unavailable_rename,
                         RawAccessorKind, Name, replaceKind.has_value(),
                         rawReplaceKind, newName, EncodedMessage.Message);
      attachRenameFixIts(diag);
  } else if (isSubscriptReturningString(D, ctx)) {
    diags.diagnose(Loc, diag::availability_string_subscript_migration)
      .highlight(R)
      .fixItInsert(R.Start, "String(")
      .fixItInsertAfter(R.End, ")");

    // Skip the note emitted below.
    return true;
  } else {
    EncodedDiagnosticMessage EncodedMessage(Attr->Message);
    diags
        .diagnose(Loc, warnInObjCKeyPath
                  ? diag::availability_decl_unavailable_warn
                  : diag::availability_decl_unavailable, RawAccessorKind,
                  Name, platform.empty(), platform, EncodedMessage.Message)
        .highlight(R);
  }

  switch (Attr->getVersionAvailability(ctx)) {
  case AvailableVersionComparison::Available:
  case AvailableVersionComparison::PotentiallyUnavailable:
    llvm_unreachable("These aren't considered unavailable");

  case AvailableVersionComparison::Unavailable:
    if ((Attr->isLanguageVersionSpecific() ||
         Attr->isPackageDescriptionVersionSpecific())
        && Attr->Introduced.has_value())
      diags.diagnose(D, diag::availability_introduced_in_version,
                     RawAccessorKind, Name,
                     (Attr->isLanguageVersionSpecific() ? 
                      "Swift" : "PackageDescription"),
                     *Attr->Introduced)
        .highlight(Attr->getRange());
    else
      diags.diagnose(D, diag::availability_marked_unavailable, RawAccessorKind,
                     Name)
        .highlight(Attr->getRange());
    break;

  case AvailableVersionComparison::Obsoleted:
    // FIXME: Use of the platformString here is non-awesome for application
    // extensions.

    StringRef platformDisplayString;
    if (Attr->isLanguageVersionSpecific()) {
      platformDisplayString = "Swift";
    } else if (Attr->isPackageDescriptionVersionSpecific()) {
      platformDisplayString = "PackageDescription";
    } else {
      platformDisplayString = platform;
    }

    diags.diagnose(D, diag::availability_obsoleted,
                   RawAccessorKind, Name,
                   platformDisplayString,
                   *Attr->Obsoleted)
      .highlight(Attr->getRange());
    break;
  }
  return true;
}

namespace {
class ExprAvailabilityWalker : public ASTWalker {
  /// Describes how the next member reference will be treated as we traverse
  /// the AST.
  enum class MemberAccessContext : unsigned {
    /// The member reference is in a context where an access will call
    /// the getter.
    Getter,

    /// The member reference is in a context where an access will call
    /// the setter.
    Setter,

    /// The member reference is in a context where it will be turned into
    /// an inout argument. (Once this happens, we have to conservatively assume
    /// that both the getter and setter could be called.)
    InOut
  };

  ASTContext &Context;
  MemberAccessContext AccessContext = MemberAccessContext::Getter;
  SmallVector<const Expr *, 16> ExprStack;
  const ExportContext &Where;

public:
  explicit ExprAvailabilityWalker(const ExportContext &Where)
    : Context(Where.getDeclContext()->getASTContext()), Where(Where) {}

  bool shouldWalkIntoSeparatelyCheckedClosure(ClosureExpr *expr) override {
    return false;
  }

  bool shouldWalkIntoTapExpression() override { return false; }

  MacroWalking getMacroWalkingBehavior() const override {
    // Expanded source should be type checked and diagnosed separately.
    return MacroWalking::Arguments;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    auto *DC = Where.getDeclContext();

    ExprStack.push_back(E);

    auto skipChildren = [&]() {
      ExprStack.pop_back();
      return Action::SkipChildren(E);
    };

    if (auto DR = dyn_cast<DeclRefExpr>(E)) {
      diagnoseDeclRefAvailability(DR->getDeclRef(), DR->getSourceRange(),
                                  getEnclosingApplyExpr(), llvm::None);
      maybeDiagStorageAccess(DR->getDecl(), DR->getSourceRange(), DC);
    }
    if (auto MR = dyn_cast<MemberRefExpr>(E)) {
      walkMemberRef(MR);
      return skipChildren();
    }
    if (auto OCDR = dyn_cast<OtherConstructorDeclRefExpr>(E))
      diagnoseDeclRefAvailability(OCDR->getDeclRef(),
                                  OCDR->getConstructorLoc().getSourceRange(),
                                  getEnclosingApplyExpr());
    if (auto DMR = dyn_cast<DynamicMemberRefExpr>(E))
      diagnoseDeclRefAvailability(DMR->getMember(),
                                  DMR->getNameLoc().getSourceRange(),
                                  getEnclosingApplyExpr());
    if (auto DS = dyn_cast<DynamicSubscriptExpr>(E))
      diagnoseDeclRefAvailability(DS->getMember(), DS->getSourceRange());
    if (auto S = dyn_cast<SubscriptExpr>(E)) {
      if (S->hasDecl()) {
        diagnoseDeclRefAvailability(S->getDecl(), S->getSourceRange(), S);
        maybeDiagStorageAccess(S->getDecl().getDecl(), S->getSourceRange(), DC);
      }
    }

    if (auto *LE = dyn_cast<LiteralExpr>(E)) {
      if (auto literalType = LE->getType()) {
        // Check availability of the type produced by implicit literal
        // initializer.
        if (auto *nominalDecl = literalType->getAnyNominal()) {
          diagnoseDeclAvailability(nominalDecl, LE->getSourceRange(),
                                   /*call=*/nullptr, Where);
        }
      }
      diagnoseDeclRefAvailability(LE->getInitializer(), LE->getSourceRange());
    }

    if (auto *CE = dyn_cast<CollectionExpr>(E)) {
      // Diagnose availability of implicit collection literal initializers.
      diagnoseDeclRefAvailability(CE->getInitializer(), CE->getSourceRange());
    }

    if (auto *EE = dyn_cast<ErasureExpr>(E)) {
      maybeDiagParameterizedExistentialErasure(EE, Where);
    }
    if (auto *CC = dyn_cast<ExplicitCastExpr>(E)) {
      if (!isa<CoerceExpr>(CC) && CC->getCastType() &&
          CC->getCastType()->hasParameterizedExistential()) {
        SourceLoc loc = CC->getCastTypeRepr() ? CC->getCastTypeRepr()->getLoc()
                                              : E->getLoc();
        diagnoseParameterizedProtocolAvailability(loc, Where.getDeclContext());
      }
    }
    if (auto KP = dyn_cast<KeyPathExpr>(E)) {
      maybeDiagKeyPath(KP);
    }
    if (auto A = dyn_cast<AssignExpr>(E)) {
      walkAssignExpr(A);
      return skipChildren();
    }
    if (auto IO = dyn_cast<InOutExpr>(E)) {
      walkInOutExpr(IO);
      return skipChildren();
    }
    if (auto T = dyn_cast<TypeExpr>(E)) {
      if (!T->isImplicit()) {
        diagnoseTypeAvailability(T->getTypeRepr(), T->getType(), E->getLoc(),
                                 Where);
      }
    }
    if (auto CE = dyn_cast<ClosureExpr>(E)) {
      for (auto *param : *CE->getParameters()) {
        diagnoseTypeAvailability(param->getTypeRepr(), param->getInterfaceType(),
                                 E->getLoc(), Where);
      }
      diagnoseTypeAvailability(CE->hasExplicitResultType()
                               ? CE->getExplicitResultTypeRepr()
                               : nullptr,
                               CE->getResultType(), E->getLoc(), Where);
    }
    if (auto CE = dyn_cast<ExplicitCastExpr>(E)) {
      diagnoseTypeAvailability(CE->getCastTypeRepr(), CE->getCastType(),
                               E->getLoc(), Where);
    }

    if (AbstractClosureExpr *closure = dyn_cast<AbstractClosureExpr>(E)) {
      if (shouldWalkIntoClosure(closure)) {
        walkAbstractClosure(closure);
        return skipChildren();
      }
    }
    
    if (auto EE = dyn_cast<ErasureExpr>(E)) {
      for (ProtocolConformanceRef C : EE->getConformances()) {
        diagnoseConformanceAvailability(E->getLoc(), C, Where, Type(), Type(),
                                        /*useConformanceAvailabilityErrorsOpt=*/true);
      }
    }

    return Action::Continue(E);
  }

  PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
    assert(ExprStack.back() == E);
    ExprStack.pop_back();

    return Action::Continue(E);
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {

    // We end up here when checking the output of the result builder transform,
    // which includes closures that are not "separately typechecked" and yet
    // contain statements and declarations. We need to walk them recursively,
    // since these availability for these statements is not diagnosed from
    // typeCheckStmt() as usual.
    diagnoseStmtAvailability(S, Where.getDeclContext(), /*walkRecursively=*/true);
    return Action::SkipChildren(S);
  }

  bool
  diagnoseDeclRefAvailability(ConcreteDeclRef declRef, SourceRange R,
                              const Expr *call = nullptr,
                              DeclAvailabilityFlags flags = llvm::None) const;

private:
  bool diagnoseIncDecRemoval(const ValueDecl *D, SourceRange R,
                             const AvailableAttr *Attr) const;
  bool diagnoseMemoryLayoutMigration(const ValueDecl *D, SourceRange R,
                                     const AvailableAttr *Attr,
                                     const ApplyExpr *call) const;

  /// Walks up from a potential callee to the enclosing ApplyExpr.
  const ApplyExpr *getEnclosingApplyExpr() const {
    ArrayRef<const Expr *> parents = ExprStack;
    assert(!parents.empty() && "must be called while visiting an expression");
    size_t idx = parents.size() - 1;

    do {
      if (idx == 0)
        return nullptr;
      --idx;
    } while (isa<DotSyntaxBaseIgnoredExpr>(parents[idx]) || // Mod.f(a)
             isa<SelfApplyExpr>(parents[idx]) || // obj.f(a)
             isa<IdentityExpr>(parents[idx]) || // (f)(a)
             isa<ForceValueExpr>(parents[idx]) || // f!(a)
             isa<BindOptionalExpr>(parents[idx]) || // f?(a)
             isa<FunctionConversionExpr>(parents[idx]));

    auto *call = dyn_cast<ApplyExpr>(parents[idx]);
    if (!call || call->getFn() != parents[idx+1])
      return nullptr;
    return call;
  }

  /// Walk an assignment expression, checking for availability.
  void walkAssignExpr(AssignExpr *E) {
    // We take over recursive walking of assignment expressions in order to
    // walk the destination and source expressions in different member
    // access contexts.
    Expr *Dest = E->getDest();
    if (!Dest) {
      return;
    }

    // Check the Dest expression in a setter context.
    // We have an implicit assumption here that the first MemberRefExpr
    // encountered walking (pre-order) is the Dest is the destination of the
    // write. For the moment this is fine -- but future syntax might violate
    // this assumption.
    walkInContext(E, Dest, MemberAccessContext::Setter);

    // Check RHS in getter context
    Expr *Source = E->getSrc();
    if (!Source) {
      return;
    }
    walkInContext(E, Source, MemberAccessContext::Getter);
  }
  
  /// Walk a member reference expression, checking for availability.
  void walkMemberRef(MemberRefExpr *E) {
    // Walk the base in a getter context.
    // FIXME: We may need to look at the setter too, if we're going to do
    // writeback. The AST should have this information.
    walkInContext(E, E->getBase(), MemberAccessContext::Getter);

    ConcreteDeclRef DR = E->getMember();
    // Diagnose for the member declaration itself.
    if (diagnoseDeclRefAvailability(DR, E->getNameLoc().getSourceRange(),
                                    getEnclosingApplyExpr(), llvm::None))
      return;

    // Diagnose for appropriate accessors, given the access context.
    auto *DC = Where.getDeclContext();
    maybeDiagStorageAccess(DR.getDecl(), E->getSourceRange(), DC);
  }

  /// Walk a keypath expression, checking all of its components for
  /// availability.
  void maybeDiagKeyPath(KeyPathExpr *KP) {
    auto flags = DeclAvailabilityFlags();
    if (KP->isObjC())
      flags = DeclAvailabilityFlag::ForObjCKeyPath;

    for (auto &component : KP->getComponents()) {
      switch (component.getKind()) {
      case KeyPathExpr::Component::Kind::Property:
      case KeyPathExpr::Component::Kind::Subscript: {
        auto decl = component.getDeclRef();
        auto loc = component.getLoc();
        diagnoseDeclRefAvailability(decl, loc, nullptr, flags);
        break;
      }

      case KeyPathExpr::Component::Kind::TupleElement:
        break;

      case KeyPathExpr::Component::Kind::Invalid:
      case KeyPathExpr::Component::Kind::UnresolvedProperty:
      case KeyPathExpr::Component::Kind::UnresolvedSubscript:
      case KeyPathExpr::Component::Kind::OptionalChain:
      case KeyPathExpr::Component::Kind::OptionalWrap:
      case KeyPathExpr::Component::Kind::OptionalForce:
      case KeyPathExpr::Component::Kind::Identity:
      case KeyPathExpr::Component::Kind::DictionaryKey:
      case KeyPathExpr::Component::Kind::CodeCompletion:
        break;
      }
    }
  }

  /// Walk an inout expression, checking for availability.
  void walkInOutExpr(InOutExpr *E) {
    walkInContext(E, E->getSubExpr(), MemberAccessContext::InOut);
  }

  bool shouldWalkIntoClosure(AbstractClosureExpr *closure) const {
    return true;
  }

  /// Walk an abstract closure expression, checking for availability
  void walkAbstractClosure(AbstractClosureExpr *closure) {
    // Do the walk with the closure set as the decl context of the 'where'
    auto where = ExportContext::forFunctionBody(closure, closure->getStartLoc());
    if (where.isImplicit())
      return;
    ExprAvailabilityWalker walker(where);

    // Manually dive into the body
    closure->getBody()->walk(walker);

    return;
  }


  /// Walk the given expression in the member access context.
  void walkInContext(Expr *baseExpr, Expr *E,
                     MemberAccessContext AccessContext) {
    llvm::SaveAndRestore<MemberAccessContext>
      C(this->AccessContext, AccessContext);
    E->walk(*this);
  }

  /// Emit diagnostics, if necessary, for accesses to storage where
  /// the accessor for the AccessContext is not available.
  void maybeDiagStorageAccess(const ValueDecl *VD,
                              SourceRange ReferenceRange,
                              const DeclContext *ReferenceDC) const {
    if (Context.LangOpts.DisableAvailabilityChecking)
      return;

    auto *D = dyn_cast<AbstractStorageDecl>(VD);
    if (!D)
      return;

    if (!D->requiresOpaqueAccessors()) {
      return;
    }

    // Check availability of accessor functions.
    // TODO: if we're talking about an inlineable storage declaration,
    // this probably needs to be refined to not assume that the accesses are
    // specifically using the getter/setter.
    switch (AccessContext) {
    case MemberAccessContext::Getter:
      diagAccessorAvailability(D->getOpaqueAccessor(AccessorKind::Get),
                               ReferenceRange, ReferenceDC, llvm::None);
      break;

    case MemberAccessContext::Setter:
      diagAccessorAvailability(D->getOpaqueAccessor(AccessorKind::Set),
                               ReferenceRange, ReferenceDC, llvm::None);
      break;

    case MemberAccessContext::InOut:
      diagAccessorAvailability(D->getOpaqueAccessor(AccessorKind::Get),
                               ReferenceRange, ReferenceDC,
                               DeclAvailabilityFlag::ForInout);

      diagAccessorAvailability(D->getOpaqueAccessor(AccessorKind::Set),
                               ReferenceRange, ReferenceDC,
                               DeclAvailabilityFlag::ForInout);
      break;
    }
  }

  /// Emit a diagnostic, if necessary for a potentially unavailable accessor.
  void diagAccessorAvailability(AccessorDecl *D, SourceRange ReferenceRange,
                                const DeclContext *ReferenceDC,
                                DeclAvailabilityFlags Flags) const {
    if (!D)
      return;

    Flags &= DeclAvailabilityFlag::ForInout;
    Flags |= DeclAvailabilityFlag::ContinueOnPotentialUnavailability;
    if (diagnoseDeclAvailability(D, ReferenceRange, /*call*/ nullptr, Where,
                                 Flags))
      return;
  }
};
} // end anonymous namespace

/// Diagnose uses of unavailable declarations. Returns true if a diagnostic
/// was emitted.
bool ExprAvailabilityWalker::diagnoseDeclRefAvailability(
    ConcreteDeclRef declRef, SourceRange R, const Expr *call,
    DeclAvailabilityFlags Flags) const {
  if (!declRef)
    return false;
  const ValueDecl *D = declRef.getDecl();

  if (auto *attr = AvailableAttr::isUnavailable(D)) {
    if (diagnoseIncDecRemoval(D, R, attr))
      return true;
    if (isa_and_nonnull<ApplyExpr>(call) &&
        diagnoseMemoryLayoutMigration(D, R, attr, cast<ApplyExpr>(call)))
      return true;
  }

  if (diagnoseDeclAvailability(D, R, call, Where, Flags))
      return true;

  if (R.isValid()) {
    if (diagnoseSubstitutionMapAvailability(R.Start, declRef.getSubstitutions(),
                                            Where)) {
      return true;
    }
  }

  return false;
}

/// Diagnose misuses of API in asynchronous contexts.
/// Returns true if a fatal diagnostic was emitted, false otherwise.
static bool
diagnoseDeclAsyncAvailability(const ValueDecl *D, SourceRange R,
                              const Expr *call, const ExportContext &Where) {
  // If we are in a synchronous context, don't check it
  if (!Where.getDeclContext()->isAsyncContext())
    return false;

  ASTContext &ctx = Where.getDeclContext()->getASTContext();

  if (const AbstractFunctionDecl *afd = dyn_cast<AbstractFunctionDecl>(D)) {
    if (const AbstractFunctionDecl *asyncAlt = afd->getAsyncAlternative()) {
      SourceLoc diagLoc = call ? call->getLoc() : R.Start;
      ctx.Diags.diagnose(diagLoc, diag::warn_use_async_alternative);

      if (auto *accessor = dyn_cast<AccessorDecl>(asyncAlt)) {
        SmallString<32> name;
        llvm::raw_svector_ostream os(name);
        accessor->printUserFacingName(os);
        ctx.Diags.diagnose(asyncAlt->getLoc(),
                           diag::descriptive_decl_declared_here, name);
      } else {
        ctx.Diags.diagnose(asyncAlt->getLoc(), diag::decl_declared_here,
                           asyncAlt->getName());
      }
    }
  }

  // @available(noasync) spelling
  if (const AvailableAttr *attr = D->getAttrs().getNoAsync(ctx)) {
    SourceLoc diagLoc = call ? call->getLoc() : R.Start;
    auto diag = ctx.Diags.diagnose(diagLoc, diag::async_unavailable_decl,
                                   D->getDescriptiveKind(), D->getBaseName(),
                                   attr->Message);
    diag.warnUntilSwiftVersion(6);

    if (!attr->Rename.empty()) {
      fixItAvailableAttrRename(diag, R, D, attr, call);
    }
    return true;
  }

  const bool hasUnavailableAttr =
      D->getAttrs().hasAttribute<UnavailableFromAsyncAttr>();

  if (!hasUnavailableAttr)
    return false;
  // @_unavailableFromAsync spelling
  const UnavailableFromAsyncAttr *attr =
      D->getAttrs().getAttribute<UnavailableFromAsyncAttr>();
  SourceLoc diagLoc = call ? call->getLoc() : R.Start;
  ctx.Diags
      .diagnose(diagLoc, diag::async_unavailable_decl, D->getDescriptiveKind(),
                D->getBaseName(), attr->Message)
      .warnUntilSwiftVersion(6);
  D->diagnose(diag::decl_declared_here, D->getName());
  return true;
}

/// Diagnose uses of unavailable declarations. Returns true if a diagnostic
/// was emitted.
bool swift::diagnoseDeclAvailability(const ValueDecl *D, SourceRange R,
                                     const Expr *call,
                                     const ExportContext &Where,
                                     DeclAvailabilityFlags Flags) {
  assert(!Where.isImplicit());

  // Generic parameters are always available.
  if (isa<GenericTypeParamDecl>(D))
    return false;

  // Keep track if this is an accessor.
  auto accessor = dyn_cast<AccessorDecl>(D);

  if (accessor) {
    // If the property/subscript is unconditionally unavailable, don't bother
    // with any of the rest of this.
    if (AvailableAttr::isUnavailable(accessor->getStorage()))
      return false;
  }

  if (R.isValid()) {
    if (TypeChecker::diagnoseInlinableDeclRefAccess(R.Start, D, Where))
      return true;

    if (TypeChecker::diagnoseDeclRefExportability(R.Start, D, Where))
      return true;
  }

  if (diagnoseExplicitUnavailability(D, R, Where, call, Flags))
    return true;

  if (diagnoseDeclAsyncAvailability(D, R, call, Where))
    return true;

  // Make sure not to diagnose an accessor's deprecation if we already
  // complained about the property/subscript.
  bool isAccessorWithDeprecatedStorage =
    accessor && TypeChecker::getDeprecated(accessor->getStorage());

  // Diagnose for deprecation
  if (!isAccessorWithDeprecatedStorage)
    TypeChecker::diagnoseIfDeprecated(R, Where, D, call);

  if (Flags.contains(DeclAvailabilityFlag::AllowPotentiallyUnavailableProtocol)
        && isa<ProtocolDecl>(D))
    return false;

  // Diagnose (and possibly signal) for potential unavailability
  auto maybeUnavail = TypeChecker::checkDeclarationAvailability(D, Where);
  if (!maybeUnavail.has_value())
    return false;

  auto unavailReason = maybeUnavail.value();
  auto *DC = Where.getDeclContext();
  if (Flags.contains(
          DeclAvailabilityFlag::
              AllowPotentiallyUnavailableAtOrBelowDeploymentTarget) &&
      unavailReason.requiresDeploymentTargetOrEarlier(DC->getASTContext()))
    return false;

  if (accessor) {
    bool forInout = Flags.contains(DeclAvailabilityFlag::ForInout);
    TypeChecker::diagnosePotentialAccessorUnavailability(
        accessor, R, DC, unavailReason, forInout);
  } else {
    if (!TypeChecker::diagnosePotentialUnavailability(D, R, DC, unavailReason))
      return false;
  }

  return !Flags.contains(
      DeclAvailabilityFlag::ContinueOnPotentialUnavailability);
}

/// Return true if the specified type looks like an integer of floating point
/// type.
static bool isIntegerOrFloatingPointType(Type ty, ModuleDecl *M) {
  return (TypeChecker::conformsToKnownProtocol(
            ty, KnownProtocolKind::ExpressibleByIntegerLiteral, M) ||
          TypeChecker::conformsToKnownProtocol(
            ty, KnownProtocolKind::ExpressibleByFloatLiteral, M));
}


/// If this is a call to an unavailable ++ / -- operator, try to diagnose it
/// with a fixit hint and return true.  If not, or if we fail, return false.
bool
ExprAvailabilityWalker::diagnoseIncDecRemoval(const ValueDecl *D, SourceRange R,
                                              const AvailableAttr *Attr) const {
  // We can only produce a fixit if we're talking about ++ or --.
  bool isInc = D->getBaseName() == "++";
  if (!isInc && D->getBaseName() != "--")
    return false;

  // We can only handle the simple cases of lvalue++ and ++lvalue.  This is
  // always modeled as:
  //   (postfix_unary_expr (declrefexpr ++), (inoutexpr (lvalue)))
  // if not, bail out.
  if (ExprStack.size() != 2 ||
      !isa<DeclRefExpr>(ExprStack[1]) ||
      !(isa<PostfixUnaryExpr>(ExprStack[0]) ||
        isa<PrefixUnaryExpr>(ExprStack[0])))
    return false;

  auto call = cast<ApplyExpr>(ExprStack[0]);

  // If the expression type is integer or floating point, then we can rewrite it
  // to "lvalue += 1".
  auto *DC = Where.getDeclContext();
  std::string replacement;
  if (isIntegerOrFloatingPointType(call->getType(), DC->getParentModule()))
    replacement = isInc ? " += 1" : " -= 1";
  else {
    // Otherwise, it must be an index type.  Rewrite to:
    // "lvalue = lvalue.successor()".
    auto &SM = Context.SourceMgr;
    auto CSR = Lexer::getCharSourceRangeFromSourceRange(
        SM, call->getArgs()->getSourceRange());
    replacement = " = " + SM.extractText(CSR).str();
    replacement += isInc ? ".successor()" : ".predecessor()";
  }
  
  if (!replacement.empty()) {
    DeclName Name;
    unsigned RawAccessorKind;
    std::tie(RawAccessorKind, Name) = getAccessorKindAndNameForDiagnostics(D);

    // If we emit a deprecation diagnostic, produce a fixit hint as well.
    auto diag = Context.Diags.diagnose(
        R.Start, diag::availability_decl_unavailable,
        RawAccessorKind, Name, true, "",
        "it has been removed in Swift 3");
    if (isa<PrefixUnaryExpr>(call)) {
      // Prefix: remove the ++ or --.
      diag.fixItRemove(call->getFn()->getSourceRange());
      diag.fixItInsertAfter(call->getArgs()->getEndLoc(), replacement);
    } else {
      // Postfix: replace the ++ or --.
      diag.fixItReplace(call->getFn()->getSourceRange(), replacement);
    }

    return true;
  }


  return false;
}

/// If this is a call to an unavailable sizeof family function, diagnose it
/// with a fixit hint and return true. If not, or if we fail, return false.
bool
ExprAvailabilityWalker::diagnoseMemoryLayoutMigration(const ValueDecl *D,
                                                      SourceRange R,
                                                      const AvailableAttr *Attr,
                                                      const ApplyExpr *call) const {

  if (!D->getModuleContext()->isStdlibModule())
    return false;

  StringRef Property;
  if (D->getBaseName() == "sizeof") {
    Property = "size";
  } else if (D->getBaseName() == "alignof") {
    Property = "alignment";
  } else if (D->getBaseName() == "strideof") {
    Property = "stride";
  }

  if (Property.empty())
    return false;

  auto *args = call->getArgs();
  auto *subject = args->getUnlabeledUnaryExpr();
  if (!subject)
    return false;

  DeclName Name;
  unsigned RawAccessorKind;
  std::tie(RawAccessorKind, Name) = getAccessorKindAndNameForDiagnostics(D);

  EncodedDiagnosticMessage EncodedMessage(Attr->Message);
  auto diag =
      Context.Diags.diagnose(
          R.Start, diag::availability_decl_unavailable, RawAccessorKind,
          Name, true, "", EncodedMessage.Message);
  diag.highlight(R);

  StringRef Prefix = "MemoryLayout<";
  StringRef Suffix = ">.";

  if (auto DTE = dyn_cast<DynamicTypeExpr>(subject)) {
    // Replace `sizeof(type(of: x))` with `MemoryLayout<X>.size`, where `X` is
    // the static type of `x`. The previous spelling misleadingly hinted that
    // `sizeof(_:)` might return the size of the *dynamic* type of `x`, when
    // it is not the case.
    auto valueType = DTE->getBase()->getType()->getRValueType();
    if (!valueType || valueType->hasError()) {
      // If we don't have a suitable argument, we can't emit a fixit.
      return true;
    }
    // Note that in rare circumstances we may be destructively replacing the
    // source text. For example, we'd replace `sizeof(type(of: doSomething()))`
    // with `MemoryLayout<T>.size`, if T is the return type of `doSomething()`.
    diag.fixItReplace(call->getSourceRange(),
                   (Prefix + valueType->getString() + Suffix + Property).str());
  } else {
    SourceRange PrefixRange(call->getStartLoc(), args->getLParenLoc());
    SourceRange SuffixRange(args->getRParenLoc());

    // We must remove `.self`.
    if (auto *DSE = dyn_cast<DotSelfExpr>(subject))
      SuffixRange.Start = DSE->getDotLoc();

    diag
      .fixItReplace(PrefixRange, Prefix)
      .fixItReplace(SuffixRange, (Suffix + Property).str());
  }

  return true;
}

/// Diagnose uses of unavailable declarations.
void swift::diagnoseExprAvailability(const Expr *E, DeclContext *DC) {
  auto where = ExportContext::forFunctionBody(DC, E->getStartLoc());
  if (where.isImplicit())
    return;
  ExprAvailabilityWalker walker(where);
  const_cast<Expr*>(E)->walk(walker);
}

namespace {

class StmtAvailabilityWalker : public BaseDiagnosticWalker {
  DeclContext *DC;
  bool WalkRecursively;

public:
  explicit StmtAvailabilityWalker(DeclContext *dc, bool walkRecursively)
    : DC(dc), WalkRecursively(walkRecursively) {}

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    if (!WalkRecursively && isa<BraceStmt>(S))
      return Action::SkipChildren(S);

    return Action::Continue(S);
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (WalkRecursively)
      diagnoseExprAvailability(E, DC);
    return Action::SkipChildren(E);
  }

  PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
    auto where = ExportContext::forFunctionBody(DC, T->getStartLoc());
    diagnoseTypeReprAvailability(T, where);
    return Action::SkipChildren();
  }

  PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override {
    if (auto *IP = dyn_cast<IsPattern>(P)) {
      auto where = ExportContext::forFunctionBody(DC, P->getLoc());
      diagnoseTypeAvailability(IP->getCastType(), P->getLoc(), where);
    }

    return Action::Continue(P);
  }
};
}

void swift::diagnoseStmtAvailability(const Stmt *S, DeclContext *DC,
                                     bool walkRecursively) {
  // We'll visit the individual statements when we check them.
  if (!walkRecursively && isa<BraceStmt>(S))
    return;

  StmtAvailabilityWalker walker(DC, walkRecursively);
  const_cast<Stmt*>(S)->walk(walker);
}

namespace {

class TypeReprAvailabilityWalker : public ASTWalker {
  const ExportContext &where;
  DeclAvailabilityFlags flags;

  bool checkIdentTypeRepr(IdentTypeRepr *ITR) {
    if (auto *typeDecl = ITR->getBoundDecl()) {
      auto range = ITR->getNameLoc().getSourceRange();
      if (diagnoseDeclAvailability(typeDecl, range, nullptr, where, flags))
        return true;
    }

    bool foundAnyIssues = false;

    if (auto *GTR = dyn_cast<GenericIdentTypeRepr>(ITR)) {
      auto genericFlags = flags;
      genericFlags -= DeclAvailabilityFlag::AllowPotentiallyUnavailableProtocol;

      for (auto *genericArg : GTR->getGenericArgs()) {
        if (diagnoseTypeReprAvailability(genericArg, where, genericFlags))
          foundAnyIssues = true;
      }
    }

    return foundAnyIssues;
  }

public:
  bool foundAnyIssues = false;

  TypeReprAvailabilityWalker(const ExportContext &where,
                             DeclAvailabilityFlags flags)
      : where(where), flags(flags) {}

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
    auto *declRefTR = dyn_cast<DeclRefTypeRepr>(T);
    if (!declRefTR)
      return Action::Continue();

    auto *baseComp = declRefTR->getBaseComponent();
    if (auto *identBase = dyn_cast<IdentTypeRepr>(baseComp)) {
      if (checkIdentTypeRepr(identBase)) {
        foundAnyIssues = true;
        return Action::SkipChildren();
      }
    } else if (diagnoseTypeReprAvailability(baseComp, where, flags)) {
      foundAnyIssues = true;
      return Action::SkipChildren();
    }

    if (auto *memberTR = dyn_cast<MemberTypeRepr>(T)) {
      for (auto *comp : memberTR->getMemberComponents()) {
        // If a parent type is unavailable, don't go on to diagnose
        // the member since that will just produce a redundant
        // diagnostic.
        if (checkIdentTypeRepr(comp)) {
          foundAnyIssues = true;
          break;
        }
      }
    }

    // We've already visited all the children above, so we don't
    // need to recurse.
    return Action::SkipChildren();
  }
};

}

bool swift::diagnoseTypeReprAvailability(const TypeRepr *T,
                                         const ExportContext &where,
                                         DeclAvailabilityFlags flags) {
  if (!T)
    return false;
  TypeReprAvailabilityWalker walker(where, flags);
  const_cast<TypeRepr*>(T)->walk(walker);
  return walker.foundAnyIssues;
}

namespace {

class ProblematicTypeFinder : public TypeDeclFinder {
  SourceLoc Loc;
  const ExportContext &Where;
  DeclAvailabilityFlags Flags;

public:
  ProblematicTypeFinder(SourceLoc Loc, const ExportContext &Where,
                        DeclAvailabilityFlags Flags)
      : Loc(Loc), Where(Where), Flags(Flags) {}

  void visitTypeDecl(TypeDecl *decl) {
    // We only need to diagnose exportability here. Availability was
    // already checked on the TypeRepr.
    if (Where.mustOnlyReferenceExportedDecls())
      TypeChecker::diagnoseDeclRefExportability(Loc, decl, Where);
  }

  Action visitNominalType(NominalType *ty) override {
    visitTypeDecl(ty->getDecl());

    // If some generic parameters are missing, don't check conformances.
    if (ty->hasUnboundGenericType())
      return Action::Continue;

    // When the DeclContext parameter to getContextSubstitutionMap()
    // is a protocol declaration, the receiver must be a concrete
    // type, so it doesn't make sense to perform this check on
    // protocol types.
    if (isa<ProtocolType>(ty))
      return Action::Continue;

    ModuleDecl *useModule = Where.getDeclContext()->getParentModule();
    auto subs = ty->getContextSubstitutionMap(useModule, ty->getDecl());
    (void) diagnoseSubstitutionMapAvailability(Loc, subs, Where);
    return Action::Continue;
  }

  Action visitBoundGenericType(BoundGenericType *ty) override {
    visitTypeDecl(ty->getDecl());

    ModuleDecl *useModule = Where.getDeclContext()->getParentModule();
    auto subs = ty->getContextSubstitutionMap(useModule, ty->getDecl());
    (void)diagnoseSubstitutionMapAvailability(
        Loc, subs, Where,
        /*depTy=*/Type(),
        /*replacementTy=*/Type(),
        /*useConformanceAvailabilityErrorsOption=*/false,
        /*suppressParameterizationCheckForOptional=*/ty->isOptional());
    return Action::Continue;
  }

  Action visitTypeAliasType(TypeAliasType *ty) override {
    visitTypeDecl(ty->getDecl());

    auto subs = ty->getSubstitutionMap();
    (void) diagnoseSubstitutionMapAvailability(Loc, subs, Where);
    return Action::Continue;
  }

  // We diagnose unserializable Clang function types in the
  // post-visitor so that we diagnose any unexportable component
  // types first.
  Action walkToTypePost(Type T) override {
    if (Where.mustOnlyReferenceExportedDecls()) {
      if (auto fnType = T->getAs<AnyFunctionType>()) {
        if (auto clangType = fnType->getClangTypeInfo().getType()) {
          auto *DC = Where.getDeclContext();
          auto &ctx = DC->getASTContext();
          auto loader = ctx.getClangModuleLoader();
          // Serialization will serialize the sugared type if it can,
          // but we need the canonical type to be serializable or else
          // canonicalization (e.g. in SIL) might break things.
          if (!loader->isSerializable(clangType, /*check canonical*/ true)) {
            ctx.Diags.diagnose(Loc, diag::unexportable_clang_function_type, T);
          }
        }
      }
    }

    if (auto *TT = T->getAs<TupleType>()) {
      for (auto component : TT->getElementTypes()) {
        // Let the walker find inner tuple types, we only want to diagnose
        // non-compound components.
        if (component->is<TupleType>())
          continue;

        if (component->hasParameterizedExistential())
          (void)diagnoseParameterizedProtocolAvailability(
              Loc, Where.getDeclContext());
      }
    }

    return TypeDeclFinder::walkToTypePost(T);
  }
};

}

void swift::diagnoseTypeAvailability(Type T, SourceLoc loc,
                                     const ExportContext &where,
                                     DeclAvailabilityFlags flags) {
  if (!T)
    return;
  T.walk(ProblematicTypeFinder(loc, where, flags));
}

void swift::diagnoseTypeAvailability(const TypeRepr *TR, Type T, SourceLoc loc,
                                     const ExportContext &where,
                                     DeclAvailabilityFlags flags) {
  if (diagnoseTypeReprAvailability(TR, where, flags))
    return;
  diagnoseTypeAvailability(T, loc, where, flags);
}

static void diagnoseMissingConformance(
    SourceLoc loc, Type type, ProtocolDecl *proto, const DeclContext *fromDC) {
  assert(proto->isSpecificProtocol(KnownProtocolKind::Sendable));
  diagnoseMissingSendableConformance(loc, type, fromDC);
}

bool
swift::diagnoseConformanceAvailability(SourceLoc loc,
                                       ProtocolConformanceRef conformance,
                                       const ExportContext &where,
                                       Type depTy, Type replacementTy,
                                       bool useConformanceAvailabilityErrorsOption) {
  assert(!where.isImplicit());

  if (!conformance.isConcrete())
    return false;

  const ProtocolConformance *concreteConf = conformance.getConcrete();
  const RootProtocolConformance *rootConf = concreteConf->getRootConformance();

  // Diagnose "missing" conformances where we needed a conformance but
  // didn't have one.
  auto *DC = where.getDeclContext();
  if (auto builtinConformance = dyn_cast<BuiltinProtocolConformance>(rootConf)){
    if (builtinConformance->isMissing()) {
      diagnoseMissingConformance(loc, builtinConformance->getType(),
                                 builtinConformance->getProtocol(), DC);
    }
  }

  auto maybeEmitAssociatedTypeNote = [&]() {
    if (!depTy && !replacementTy)
      return;

    Type selfTy = rootConf->getProtocol()->getProtocolSelfType();
    if (!depTy->isEqual(selfTy)) {
      auto &ctx = DC->getASTContext();
      ctx.Diags.diagnose(
          loc,
          diag::assoc_conformance_from_implementation_only_module,
          depTy, replacementTy->getCanonicalType());
    }
  };

  if (auto *ext = dyn_cast<ExtensionDecl>(rootConf->getDeclContext())) {
    if (TypeChecker::diagnoseConformanceExportability(loc, rootConf, ext, where,
                                                      useConformanceAvailabilityErrorsOption)) {
      maybeEmitAssociatedTypeNote();
      return true;
    }

    if (diagnoseExplicitUnavailability(loc, rootConf, ext, where,
                                       useConformanceAvailabilityErrorsOption)) {
      maybeEmitAssociatedTypeNote();
      return true;
    }

    // Diagnose (and possibly signal) for potential unavailability
    auto maybeUnavail = TypeChecker::checkConformanceAvailability(
        rootConf, ext, where);
    if (maybeUnavail.has_value()) {
      TypeChecker::diagnosePotentialUnavailability(rootConf, ext, loc, DC,
                                                   maybeUnavail.value());
      maybeEmitAssociatedTypeNote();
      return true;
    }

    // Diagnose for deprecation
    if (TypeChecker::diagnoseIfDeprecated(loc, rootConf, ext, where)) {
      maybeEmitAssociatedTypeNote();

      // Deprecation is just a warning, so keep going with checking the
      // substitution map below.
    }
  }

  // Now, check associated conformances.
  SubstitutionMap subConformanceSubs = concreteConf->getSubstitutionMap();
  if (diagnoseSubstitutionMapAvailability(loc, subConformanceSubs, where,
                                          depTy, replacementTy,
                                          useConformanceAvailabilityErrorsOption))
    return true;

  return false;
}

bool
swift::diagnoseSubstitutionMapAvailability(SourceLoc loc,
                                           SubstitutionMap subs,
                                           const ExportContext &where,
                                           Type depTy, Type replacementTy,
                                           bool useConformanceAvailabilityErrorsOption,
                                           bool suppressParameterizationCheckForOptional) {
  bool hadAnyIssues = false;
  for (ProtocolConformanceRef conformance : subs.getConformances()) {
    if (diagnoseConformanceAvailability(loc, conformance, where,
                                        depTy, replacementTy,
                                        useConformanceAvailabilityErrorsOption))
      hadAnyIssues = true;
  }

  // If we're looking at \c (any P)? (or any other depth of optional) then
  // there's no availability problem.
  if (suppressParameterizationCheckForOptional)
    return hadAnyIssues;

  for (auto replacement : subs.getReplacementTypes()) {
    if (replacement->hasParameterizedExistential())
      if (diagnoseParameterizedProtocolAvailability(loc,
                                                    where.getDeclContext()))
        hadAnyIssues = true;
  }
  return hadAnyIssues;
}

/// Should we warn that \p decl needs an explicit availability annotation
/// in -require-explicit-availability mode?
static bool declNeedsExplicitAvailability(const Decl *decl) {
  auto &ctx = decl->getASTContext();

  // Don't require an introduced version on platforms that don't support
  // versioned availability.
  if (!ctx.supportsVersionedAvailability())
    return false;

  // Skip non-public decls.
  if (auto valueDecl = dyn_cast<const ValueDecl>(decl)) {
    AccessScope scope =
      valueDecl->getFormalAccessScope(/*useDC*/nullptr,
                                      /*treatUsableFromInlineAsPublic*/true);
    if (!scope.isPublic())
      return false;
  }

  // Skip functions emitted into clients, SPI or implicit.
  if (decl->getAttrs().hasAttribute<AlwaysEmitIntoClientAttr>() ||
      decl->isSPI() ||
      decl->isImplicit())
    return false;

  // Skip unavailable decls.
  if (AvailableAttr::isUnavailable(decl))
    return false;

  // Warn on decls without an introduction version.
  auto safeRangeUnderApprox = AvailabilityInference::availableRange(decl, ctx);
  return !safeRangeUnderApprox.getOSVersion().hasLowerEndpoint();
}

void swift::checkExplicitAvailability(Decl *decl) {
  // Skip if the command line option was not set and
  // accessors as we check the pattern binding decl instead.
  auto &ctx = decl->getASTContext();
  auto DiagLevel = ctx.LangOpts.RequireExplicitAvailability;
  if (!DiagLevel || isa<AccessorDecl>(decl))
    return;

  // Only look at decls at module level or in extensions.
  // This could be changed to force having attributes on all decls.
  if (!decl->getDeclContext()->isModuleScopeContext() &&
      !isa<ExtensionDecl>(decl->getDeclContext())) return;

  if (auto extension = dyn_cast<ExtensionDecl>(decl)) {
    // decl should be either a ValueDecl or an ExtensionDecl.
    auto extended = extension->getExtendedNominal();
    if (!extended || !extended->getFormalAccessScope().isPublic())
      return;

    // Skip extensions without public members or conformances.
    auto members = extension->getMembers();
    auto hasMembers = std::any_of(members.begin(), members.end(),
                                  [](const Decl *D) -> bool {
      if (auto VD = dyn_cast<ValueDecl>(D))
        if (declNeedsExplicitAvailability(VD))
          return true;
      return false;
    });

    auto hasProtocols = hasConformancesToPublicProtocols(extension);

    if (!hasMembers && !hasProtocols) return;

  } else if (auto pbd = dyn_cast<PatternBindingDecl>(decl)) {
    // Check the first var instead.
    if (pbd->getNumPatternEntries() == 0)
      return;

    llvm::SmallVector<VarDecl *, 2> vars;
    pbd->getPattern(0)->collectVariables(vars);
    if (vars.empty())
      return;

    decl = vars.front();
  }

  if (declNeedsExplicitAvailability(decl)) {
    auto diag = decl->diagnose(diag::public_decl_needs_availability);
    diag.limitBehavior(*DiagLevel);

    auto suggestPlatform = ctx.LangOpts.RequireExplicitAvailabilityTarget;
    if (!suggestPlatform.empty()) {
      auto InsertLoc = decl->getAttrs().getStartLoc(/*forModifiers=*/false);
      if (InsertLoc.isInvalid())
        InsertLoc = decl->getStartLoc();

      if (InsertLoc.isInvalid())
        return;

      std::string AttrText;
      {
         llvm::raw_string_ostream Out(AttrText);

         StringRef OriginalIndent = Lexer::getIndentationForLine(
           ctx.SourceMgr, InsertLoc);
         Out << "@available(" << suggestPlatform << ", *)\n"
             << OriginalIndent;
      }

      diag.fixItInsert(InsertLoc, AttrText);
    }
  }
}
