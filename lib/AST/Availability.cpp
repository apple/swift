//===--- Availability.cpp - Swift Availability Structures -----------------===//
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
// This file defines data structures for API availability.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Availability.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Decl.h"
#include "swift/AST/PlatformKind.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeWalker.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Platform.h"
#include "swift/ClangImporter/ClangModule.h"
#include <map>

using namespace swift;

AvailabilityContext AvailabilityContext::forDeploymentTarget(const ASTContext &Ctx) {
  return AvailabilityContext(
      VersionRange::allGTE(Ctx.LangOpts.getMinPlatformVersion()));
}

AvailabilityContext AvailabilityContext::forInliningTarget(const ASTContext &Ctx) {
  return AvailabilityContext(
      VersionRange::allGTE(Ctx.LangOpts.MinimumInliningTargetVersion));
}

AvailabilityContext AvailabilityContext::forRuntimeTarget(const ASTContext &Ctx) {
  return AvailabilityContext(
    VersionRange::allGTE(Ctx.LangOpts.RuntimeVersion));
}

namespace {

/// The inferred availability required to access a group of declarations
/// on a single platform.
struct InferredAvailability {
  PlatformAgnosticAvailabilityKind PlatformAgnostic
    = PlatformAgnosticAvailabilityKind::None;

  std::optional<llvm::VersionTuple> Introduced;
  std::optional<llvm::VersionTuple> Deprecated;
  std::optional<llvm::VersionTuple> Obsoleted;
  bool IsSPI = false;
};

/// The type of a function that merges two version tuples.
typedef const llvm::VersionTuple &(*MergeFunction)(
    const llvm::VersionTuple &, const llvm::VersionTuple &);

} // end anonymous namespace

/// Apply a merge function to two optional versions, returning the result
/// in Inferred.
static bool
mergeIntoInferredVersion(const std::optional<llvm::VersionTuple> &Version,
                         std::optional<llvm::VersionTuple> &Inferred,
                         MergeFunction Merge) {
  if (Version.has_value()) {
    if (Inferred.has_value()) {
      Inferred = Merge(Inferred.value(), Version.value());
      return *Inferred == *Version;
    } else {
      Inferred = Version;
      return true;
    }
  }
  return false;
}

/// Merge an attribute's availability with an existing inferred availability
/// so that the new inferred availability is at least as available as
/// the attribute requires.
static void mergeWithInferredAvailability(const AvailableAttr *Attr,
                                          InferredAvailability &Inferred) {
  Inferred.PlatformAgnostic
    = static_cast<PlatformAgnosticAvailabilityKind>(
      std::max(static_cast<unsigned>(Inferred.PlatformAgnostic),
               static_cast<unsigned>(Attr->getPlatformAgnosticAvailability())));

  // The merge of two introduction versions is the maximum of the two versions.
  if (mergeIntoInferredVersion(Attr->Introduced, Inferred.Introduced, std::max)) {
    Inferred.IsSPI = Attr->IsSPI;
  }

  // The merge of deprecated and obsoleted versions takes the minimum.
  mergeIntoInferredVersion(Attr->Deprecated, Inferred.Deprecated, std::min);
  mergeIntoInferredVersion(Attr->Obsoleted, Inferred.Obsoleted, std::min);
}

/// Create an implicit availability attribute for the given platform
/// and with the inferred availability.
static AvailableAttr *createAvailableAttr(PlatformKind Platform,
                                          const InferredAvailability &Inferred,
                                          StringRef Message, 
                                          StringRef Rename,
                                          ValueDecl *RenameDecl,
                                          ASTContext &Context) {
  // If there is no information that would go into the availability attribute,
  // don't create one.
  if (Inferred.PlatformAgnostic == PlatformAgnosticAvailabilityKind::None &&
      !Inferred.Introduced && !Inferred.Deprecated && !Inferred.Obsoleted &&
      Message.empty() && Rename.empty() && !RenameDecl)
    return nullptr;

  llvm::VersionTuple Introduced =
      Inferred.Introduced.value_or(llvm::VersionTuple());
  llvm::VersionTuple Deprecated =
      Inferred.Deprecated.value_or(llvm::VersionTuple());
  llvm::VersionTuple Obsoleted =
      Inferred.Obsoleted.value_or(llvm::VersionTuple());

  return new (Context)
      AvailableAttr(SourceLoc(), SourceRange(), Platform,
                    Message, Rename, RenameDecl,
                    Introduced, /*IntroducedRange=*/SourceRange(),
                    Deprecated, /*DeprecatedRange=*/SourceRange(),
                    Obsoleted, /*ObsoletedRange=*/SourceRange(),
                    Inferred.PlatformAgnostic, /*Implicit=*/true,
                    Inferred.IsSPI);
}

void AvailabilityInference::applyInferredAvailableAttrs(
    Decl *ToDecl, ArrayRef<const Decl *> InferredFromDecls,
    ASTContext &Context) {

  // Let the new AvailabilityAttr inherit the message and rename.
  // The first encountered message / rename will win; this matches the 
  // behaviour of diagnostics for 'non-inherited' AvailabilityAttrs.
  StringRef Message;
  StringRef Rename;
  ValueDecl *RenameDecl = nullptr;

  // Iterate over the declarations and infer required availability on
  // a per-platform basis.
  std::map<PlatformKind, InferredAvailability> Inferred;
  for (const Decl *D : InferredFromDecls) {
    llvm::SmallVector<const AvailableAttr *, 8> MergedAttrs;

    do {
      llvm::SmallVector<const AvailableAttr *, 8> PendingAttrs;

      for (const DeclAttribute *Attr : D->getAttrs()) {
        auto *AvAttr = dyn_cast<AvailableAttr>(Attr);
        if (!AvAttr || AvAttr->isInvalid())
          continue;

        // Skip an attribute from an outer declaration if it is for a platform
        // that was already handled implicitly by an attribute from an inner
        // declaration.
        if (llvm::any_of(MergedAttrs,
                         [&AvAttr](const AvailableAttr *MergedAttr) {
                           return inheritsAvailabilityFromPlatform(
                               AvAttr->Platform, MergedAttr->Platform);
                         }))
          continue;

        mergeWithInferredAvailability(AvAttr, Inferred[AvAttr->Platform]);
        PendingAttrs.push_back(AvAttr);

        if (Message.empty() && !AvAttr->Message.empty())
          Message = AvAttr->Message;

        if (Rename.empty() && !AvAttr->Rename.empty()) {
          Rename = AvAttr->Rename;
          RenameDecl = AvAttr->RenameDecl;
        }
      }

      MergedAttrs.append(PendingAttrs);

      // Walk up the enclosing declaration hierarchy to make sure we aren't
      // missing any inherited attributes.
      D = AvailabilityInference::parentDeclForInferredAvailability(D);
    } while (D);
  }

  DeclAttributes &Attrs = ToDecl->getAttrs();

  // Create an availability attribute for each observed platform and add
  // to ToDecl.
  for (auto &Pair : Inferred) {
    auto *Attr = createAvailableAttr(Pair.first, Pair.second, Message,
                                     Rename, RenameDecl, Context);

    if (Attr)
      Attrs.add(Attr);
  }
}

/// Returns the decl that should be considered the parent decl of the given decl
/// when looking for inherited availability annotations.
const Decl *
AvailabilityInference::parentDeclForInferredAvailability(const Decl *D) {
  if (auto *AD = dyn_cast<AccessorDecl>(D))
    return AD->getStorage();

  if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
    if (auto *NTD = ED->getExtendedNominal())
      return NTD;
  }

  if (auto *PBD = dyn_cast<PatternBindingDecl>(D)) {
    if (PBD->getNumPatternEntries() < 1)
      return nullptr;

    return PBD->getAnchoringVarDecl(0);
  }

  if (auto *OTD = dyn_cast<OpaqueTypeDecl>(D))
    return OTD->getNamingDecl();

  // Clang decls may be inaccurately parented rdar://53956555
  if (D->hasClangNode())
    return nullptr;

  // Availability is inherited from the enclosing context.
  return D->getDeclContext()->getInnermostDeclarationDeclContext();
}

/// Returns true if the introduced version in \p newAttr should be used instead
/// of the introduced version in \p prevAttr when both are attached to the same
/// declaration and refer to the active platform.
static bool isBetterThan(const AvailableAttr *newAttr,
                         const AvailableAttr *prevAttr) {
  assert(newAttr);

  // If there is no prevAttr, newAttr of course wins.
  if (!prevAttr)
    return true;

  // If they belong to the same platform, the one that introduces later wins.
  if (prevAttr->Platform == newAttr->Platform)
    return prevAttr->Introduced.value() < newAttr->Introduced.value();

  // If the new attribute's platform inherits from the old one, it wins.
  return inheritsAvailabilityFromPlatform(newAttr->Platform,
                                          prevAttr->Platform);
}

static const clang::DarwinSDKInfo::RelatedTargetVersionMapping *
getFallbackVersionMapping(const ASTContext &Ctx,
                          clang::DarwinSDKInfo::OSEnvPair Kind) {
  auto *SDKInfo = Ctx.getDarwinSDKInfo();
  if (SDKInfo)
    return SDKInfo->getVersionMapping(Kind);

  return Ctx.getAuxiliaryDarwinPlatformRemapInfo(Kind);
}

static std::optional<clang::VersionTuple>
getRemappedIntroducedVersionForFallbackPlatform(
    const ASTContext &Ctx, const llvm::VersionTuple &Version) {
  const auto *Mapping = getFallbackVersionMapping(
      Ctx, clang::DarwinSDKInfo::OSEnvPair(
               llvm::Triple::IOS, llvm::Triple::UnknownEnvironment,
               llvm::Triple::XROS, llvm::Triple::UnknownEnvironment));
  if (!Mapping)
    return std::nullopt;
  return Mapping->mapIntroducedAvailabilityVersion(Version);
}

static std::optional<clang::VersionTuple>
getRemappedDeprecatedObsoletedVersionForFallbackPlatform(
    const ASTContext &Ctx, const llvm::VersionTuple &Version) {
  const auto *Mapping = getFallbackVersionMapping(
      Ctx, clang::DarwinSDKInfo::OSEnvPair(
               llvm::Triple::IOS, llvm::Triple::UnknownEnvironment,
               llvm::Triple::XROS, llvm::Triple::UnknownEnvironment));
  if (!Mapping)
    return std::nullopt;
  return Mapping->mapDeprecatedObsoletedAvailabilityVersion(Version);
}

bool AvailabilityInference::updateIntroducedPlatformForFallback(
    const AvailableAttr *attr, const ASTContext &Ctx, llvm::StringRef &Platform,
    llvm::VersionTuple &PlatformVer) {
  std::optional<llvm::VersionTuple> IntroducedVersion = attr->Introduced;
  if (attr->Platform == PlatformKind::iOS && IntroducedVersion.has_value() &&
      isPlatformActive(PlatformKind::visionOS, Ctx.LangOpts)) {
    // We re-map the iOS introduced version to the corresponding visionOS version
    auto PotentiallyRemappedIntroducedVersion =
        getRemappedIntroducedVersionForFallbackPlatform(Ctx,
                                                        *IntroducedVersion);
    if (PotentiallyRemappedIntroducedVersion.has_value()) {
      Platform = swift::prettyPlatformString(PlatformKind::visionOS);
      PlatformVer = PotentiallyRemappedIntroducedVersion.value();
      return true;
    }
  }
  return false;
}

bool AvailabilityInference::updateDeprecatedPlatformForFallback(
    const AvailableAttr *attr, const ASTContext &Ctx, llvm::StringRef &Platform,
    llvm::VersionTuple &PlatformVer) {
  std::optional<llvm::VersionTuple> DeprecatedVersion = attr->Deprecated;
  if (attr->Platform == PlatformKind::iOS && DeprecatedVersion.has_value() &&
      isPlatformActive(PlatformKind::visionOS, Ctx.LangOpts)) {
    // We re-map the iOS deprecated version to the corresponding visionOS version
    auto PotentiallyRemappedDeprecatedVersion =
        getRemappedDeprecatedObsoletedVersionForFallbackPlatform(
            Ctx, *DeprecatedVersion);
    if (PotentiallyRemappedDeprecatedVersion.has_value()) {
      Platform = swift::prettyPlatformString(PlatformKind::visionOS);
      PlatformVer = PotentiallyRemappedDeprecatedVersion.value();
      return true;
    }
  }
  return false;
}

bool AvailabilityInference::updateObsoletedPlatformForFallback(
    const AvailableAttr *attr, const ASTContext &Ctx, llvm::StringRef &Platform,
    llvm::VersionTuple &PlatformVer) {
  std::optional<llvm::VersionTuple> ObsoletedVersion = attr->Obsoleted;
  if (attr->Platform == PlatformKind::iOS && ObsoletedVersion.has_value() &&
      isPlatformActive(PlatformKind::visionOS, Ctx.LangOpts)) {
    // We re-map the iOS obsoleted version to the corresponding visionOS version
    auto PotentiallyRemappedObsoletedVersion =
        getRemappedDeprecatedObsoletedVersionForFallbackPlatform(
            Ctx, *ObsoletedVersion);
    if (PotentiallyRemappedObsoletedVersion.has_value()) {
      Platform = swift::prettyPlatformString(PlatformKind::visionOS);
      PlatformVer = PotentiallyRemappedObsoletedVersion.value();
      return true;
    }
  }
  return false;
}

void AvailabilityInference::updatePlatformStringForFallback(
    const AvailableAttr *attr, const ASTContext &Ctx, llvm::StringRef &Platform) {
  if (attr->Platform == PlatformKind::iOS &&
      isPlatformActive(PlatformKind::visionOS, Ctx.LangOpts)) {
    Platform = swift::prettyPlatformString(PlatformKind::visionOS);
  }
}

bool AvailabilityInference::updateBeforePlatformForFallback(
    const BackDeployedAttr *attr, const ASTContext &Ctx,
    llvm::StringRef &Platform, llvm::VersionTuple &PlatformVer) {
  auto BeforeVersion = attr->Version;
  if (attr->Platform == PlatformKind::iOS &&
      isPlatformActive(PlatformKind::visionOS, Ctx.LangOpts)) {
    // We re-map the iOS before version to the corresponding visionOS version
    auto PotentiallyRemappedIntroducedVersion =
        getRemappedIntroducedVersionForFallbackPlatform(Ctx, BeforeVersion);
    if (PotentiallyRemappedIntroducedVersion.has_value()) {
      Platform = swift::prettyPlatformString(PlatformKind::visionOS);
      PlatformVer = PotentiallyRemappedIntroducedVersion.value();
      return true;
    }
  }
  return false;
}

const AvailableAttr *
AvailabilityInference::attrForAnnotatedAvailableRange(const Decl *D,
                                                      ASTContext &Ctx) {
  const AvailableAttr *bestAvailAttr = nullptr;

  D = abstractSyntaxDeclForAvailableAttribute(D);

  for (auto Attr : D->getAttrs()) {
    auto *AvailAttr = dyn_cast<AvailableAttr>(Attr);
    if (AvailAttr == nullptr || !AvailAttr->Introduced.has_value() ||
        !AvailAttr->isActivePlatform(Ctx) ||
        AvailAttr->isLanguageVersionSpecific() ||
        AvailAttr->isPackageDescriptionVersionSpecific()) {
      continue;
    }

    if (isBetterThan(AvailAttr, bestAvailAttr))
      bestAvailAttr = AvailAttr;
  }

  return bestAvailAttr;
}

std::optional<AvailableAttrDeclPair>
SemanticAvailableRangeAttrRequest::evaluate(Evaluator &evaluator,
                                            const Decl *decl) const {
  if (auto attr = AvailabilityInference::attrForAnnotatedAvailableRange(
          decl, decl->getASTContext()))
    return std::make_pair(attr, decl);

  if (auto *parent =
          AvailabilityInference::parentDeclForInferredAvailability(decl))
    return parent->getSemanticAvailableRangeAttr();

  return std::nullopt;
}

std::optional<AvailableAttrDeclPair>
Decl::getSemanticAvailableRangeAttr() const {
  auto &eval = getASTContext().evaluator;
  return evaluateOrDefault(eval, SemanticAvailableRangeAttrRequest{this},
                           std::nullopt);
}

std::optional<AvailabilityContext>
AvailabilityInference::annotatedAvailableRange(const Decl *D, ASTContext &Ctx) {
  auto bestAvailAttr = attrForAnnotatedAvailableRange(D, Ctx);
  if (!bestAvailAttr)
    return std::nullopt;

  return availableRange(bestAvailAttr, Ctx);
}

bool Decl::isAvailableAsSPI() const {
  return AvailabilityInference::availableRange(this, getASTContext())
    .isAvailableAsSPI();
}

std::optional<AvailableAttrDeclPair>
SemanticUnavailableAttrRequest::evaluate(Evaluator &evaluator, const Decl *decl,
                                         bool ignoreAppExtensions) const {
  // Directly marked unavailable.
  if (auto attr = decl->getAttrs().getUnavailable(decl->getASTContext(),
                                                  ignoreAppExtensions))
    return std::make_pair(attr, decl);

  if (auto *parent =
          AvailabilityInference::parentDeclForInferredAvailability(decl))
    return parent->getSemanticUnavailableAttr(ignoreAppExtensions);

  return std::nullopt;
}

std::optional<AvailableAttrDeclPair>
Decl::getSemanticUnavailableAttr(bool ignoreAppExtensions) const {
  auto &eval = getASTContext().evaluator;
  return evaluateOrDefault(
      eval, SemanticUnavailableAttrRequest{this, ignoreAppExtensions},
      std::nullopt);
}

bool Decl::isUnreachableAtRuntime() const {
  // Don't trust unavailability on declarations from clang modules.
  if (isa<ClangModuleUnit>(getDeclContext()->getModuleScopeContext()))
    return false;

  auto unavailableAttrAndDecl =
      getSemanticUnavailableAttr(/*ignoreAppExtensions=*/true);
  if (!unavailableAttrAndDecl)
    return false;

  // getSemanticUnavailableAttr() can return an @available attribute that makes
  // its declaration unavailable conditionally due to deployment target. Only
  // stub or skip a declaration that is unavailable regardless of deployment
  // target.
  auto *unavailableAttr = unavailableAttrAndDecl->first;
  if (!unavailableAttr->isUnconditionallyUnavailable())
    return false;

  // Universally unavailable declarations are always unreachable.
  if (unavailableAttr->Platform == PlatformKind::none)
    return true;

  // FIXME: Support zippered frameworks (rdar://125371621)
  // If we have a target variant (e.g. we're building a zippered macOS
  // framework) then the decl is only unreachable if it is unavailable for both
  // the primary target and the target variant.
  if (getASTContext().LangOpts.TargetVariant.has_value())
    return false;

  return true;
}

static UnavailableDeclOptimization
getEffectiveUnavailableDeclOptimization(ASTContext &ctx) {
  if (ctx.LangOpts.UnavailableDeclOptimizationMode.has_value())
    return *ctx.LangOpts.UnavailableDeclOptimizationMode;

  // FIXME: Allow unavailable decl optimization on visionOS.
  // visionOS must be ABI compatible with iOS. Enabling unavailable declaration
  // optimizations naively would break compatibility since declarations marked
  // unavailable on visionOS would be optimized regardless of whether they are
  // available on iOS. rdar://116742214
  if (ctx.LangOpts.Target.isXROS())
    return UnavailableDeclOptimization::None;

  return UnavailableDeclOptimization::None;
}

bool Decl::isAvailableDuringLowering() const {
  // Unconditionally unavailable declarations should be skipped during lowering
  // when -unavailable-decl-optimization=complete is specified.
  if (getEffectiveUnavailableDeclOptimization(getASTContext()) !=
      UnavailableDeclOptimization::Complete)
    return true;

  return !isUnreachableAtRuntime();
}

bool Decl::requiresUnavailableDeclABICompatibilityStubs() const {
  // Code associated with unavailable declarations should trap at runtime if
  // -unavailable-decl-optimization=stub is specified.
  if (getEffectiveUnavailableDeclOptimization(getASTContext()) !=
      UnavailableDeclOptimization::Stub)
    return false;

  return isUnreachableAtRuntime();
}

bool UnavailabilityReason::requiresDeploymentTargetOrEarlier(
    ASTContext &Ctx) const {
  return RequiredDeploymentRange.getLowerEndpoint() <=
         AvailabilityContext::forDeploymentTarget(Ctx)
             .getOSVersion()
             .getLowerEndpoint();
}

AvailabilityContext
AvailabilityInference::annotatedAvailableRangeForAttr(const SpecializeAttr* attr,
                                                      ASTContext &ctx) {

  const AvailableAttr *bestAvailAttr = nullptr;

  for (auto *availAttr : attr->getAvailableAttrs()) {
    if (availAttr == nullptr || !availAttr->Introduced.has_value() ||
        !availAttr->isActivePlatform(ctx) ||
        availAttr->isLanguageVersionSpecific() ||
        availAttr->isPackageDescriptionVersionSpecific()) {
      continue;
    }

    if (isBetterThan(availAttr, bestAvailAttr))
      bestAvailAttr = availAttr;
  }

  if (bestAvailAttr)
    return availableRange(bestAvailAttr, ctx);

  return AvailabilityContext::alwaysAvailable();
}

AvailabilityContext AvailabilityInference::availableRange(const Decl *D,
                                                          ASTContext &Ctx) {
  std::optional<AvailabilityContext> AnnotatedRange =
      annotatedAvailableRange(D, Ctx);
  if (AnnotatedRange.has_value()) {
    return AnnotatedRange.value();
  }

  // Unlike other declarations, extensions can be used without referring to them
  // by name (they don't have one) in the source. For this reason, when checking
  // the available range of a declaration we also need to check to see if it is
  // immediately contained in an extension and use the extension's availability
  // if the declaration does not have an explicit @available attribute
  // itself. This check relies on the fact that we cannot have nested
  // extensions.

  DeclContext *DC = D->getDeclContext();
  if (auto *ED = dyn_cast<ExtensionDecl>(DC)) {
    AnnotatedRange = annotatedAvailableRange(ED, Ctx);
    if (AnnotatedRange.has_value()) {
      return AnnotatedRange.value();
    }
  }

  // Treat unannotated declarations as always available.
  return AvailabilityContext::alwaysAvailable();
}

AvailabilityContext
AvailabilityInference::availableRange(const AvailableAttr *attr,
                                      ASTContext &Ctx) {
  assert(attr->isActivePlatform(Ctx));

  llvm::VersionTuple IntroducedVersion = attr->Introduced.value();
  StringRef Platform = attr->prettyPlatformString();
  llvm::VersionTuple RemappedIntroducedVersion;
  if (AvailabilityInference::updateIntroducedPlatformForFallback(
      attr, Ctx, Platform, RemappedIntroducedVersion))
    IntroducedVersion = RemappedIntroducedVersion;

  return AvailabilityContext{VersionRange::allGTE(IntroducedVersion),
                             attr->IsSPI};
}

namespace {
/// Infers the availability required to access a type.
class AvailabilityInferenceTypeWalker : public TypeWalker {
public:
  ASTContext &AC;
  AvailabilityContext AvailabilityInfo = AvailabilityContext::alwaysAvailable();

  AvailabilityInferenceTypeWalker(ASTContext &AC) : AC(AC) {}

  Action walkToTypePre(Type ty) override {
    if (auto *nominalDecl = ty->getAnyNominal()) {
      AvailabilityInfo.intersectWith(
          AvailabilityInference::availableRange(nominalDecl, AC));
    }

    return Action::Continue;
  }
};
} // end anonymous namespace


AvailabilityContext AvailabilityInference::inferForType(Type t) {
  AvailabilityInferenceTypeWalker walker(t->getASTContext());
  t.walk(walker);
  return walker.AvailabilityInfo;
}

AvailabilityContext ASTContext::getSwiftFutureAvailability() const {
  auto target = LangOpts.Target;

  if (target.isMacOSX() ) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(99, 99, 0)));
  } else if (target.isiOS()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(99, 99, 0)));
  } else if (target.isWatchOS()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(99, 99, 0)));
  } else {
    return AvailabilityContext::alwaysAvailable();
  }
}

AvailabilityContext
ASTContext::getSwiftAvailability(unsigned major, unsigned minor) const {
  auto target = LangOpts.Target;

  // Deal with special cases for Swift 5.3 and lower
  if (major == 5 && minor <= 3) {
    if (target.getArchName() == "arm64e")
      return AvailabilityContext::alwaysAvailable();
    if (target.isMacOSX() && target.isAArch64())
      return AvailabilityContext::alwaysAvailable();
    if (target.isiOS() && target.isAArch64()
        && (target.isSimulatorEnvironment()
            || target.isMacCatalystEnvironment()))
      return AvailabilityContext::alwaysAvailable();
    if (target.isWatchOS() && target.isArch64Bit())
      return AvailabilityContext::alwaysAvailable();
  }

  switch (major) {
#define MAJOR_VERSION(V) case V: switch (minor) {
#define END_MAJOR_VERSION(V) } break;
#define PLATFORM(P, V)                                                  \
    if (IS_PLATFORM(P))                                                 \
      return AvailabilityContext(VersionRange::allGTE(llvm::VersionTuple V));
#define IS_PLATFORM(P) PLATFORM_TEST_##P
#define FUTURE                  return getSwiftFutureAvailability();
#define PLATFORM_TEST_macOS     target.isMacOSX()
#define PLATFORM_TEST_iOS       target.isiOS()
#define PLATFORM_TEST_watchOS   target.isWatchOS()
#define PLATFORM_TEST_xrOS      target.isXROS()

#define _SECOND(A, B) B
#define SECOND(T) _SECOND T

#define RUNTIME_VERSION(V, PLATFORMS)           \
     case SECOND(V):                            \
        PLATFORMS                               \
        return AvailabilityContext::alwaysAvailable();

    #include "swift/AST/RuntimeVersions.def"

#undef PLATFORM_TEST_macOS
#undef PLATFORM_TEST_iOS
#undef PLATFORM_TEST_watchOS
#undef PLATFORM_TEST_xrOS
#undef _SECOND
#undef SECOND

  case 99:
    if (minor == 99)
      return getSwiftFutureAvailability();
    break;
  }

  llvm::report_fatal_error(
    Twine("Missing runtime version data for Swift ") +
    Twine(major) + Twine('.') + Twine(minor));
}

bool ASTContext::supportsVersionedAvailability() const {
  return minimumAvailableOSVersionForTriple(LangOpts.Target).has_value();
}

// FIXME: Rename abstractSyntaxDeclForAvailableAttribute since it's useful
// for more attributes than `@available`.
const Decl *
swift::abstractSyntaxDeclForAvailableAttribute(const Decl *ConcreteSyntaxDecl) {
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
    for (auto index : range(PBD->getNumPatternEntries())) {
      if (auto VD = PBD->getAnchoringVarDecl(index))
        return VD;
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
