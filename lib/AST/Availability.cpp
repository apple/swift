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
#include "swift/Basic/Platform.h"
#include <map>

using namespace swift;

AvailabilityContext AvailabilityContext::forDeploymentTarget(ASTContext &Ctx) {
  return AvailabilityContext(
      VersionRange::allGTE(Ctx.LangOpts.getMinPlatformVersion()));
}

AvailabilityContext AvailabilityContext::forInliningTarget(ASTContext &Ctx) {
  return AvailabilityContext(
      VersionRange::allGTE(Ctx.LangOpts.MinimumInliningTargetVersion));
}

namespace {

/// The inferred availability required to access a group of declarations
/// on a single platform.
struct InferredAvailability {
  PlatformAgnosticAvailabilityKind PlatformAgnostic
    = PlatformAgnosticAvailabilityKind::None;

  llvm::Optional<llvm::VersionTuple> Introduced;
  llvm::Optional<llvm::VersionTuple> Deprecated;
  llvm::Optional<llvm::VersionTuple> Obsoleted;
  bool IsSPI = false;
};

/// The type of a function that merges two version tuples.
typedef const llvm::VersionTuple &(*MergeFunction)(
    const llvm::VersionTuple &, const llvm::VersionTuple &);

} // end anonymous namespace

/// Apply a merge function to two optional versions, returning the result
/// in Inferred.
static bool
mergeIntoInferredVersion(const llvm::Optional<llvm::VersionTuple> &Version,
                         llvm::Optional<llvm::VersionTuple> &Inferred,
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
    do {
      for (const DeclAttribute *Attr : D->getAttrs()) {
        auto *AvAttr = dyn_cast<AvailableAttr>(Attr);
        if (!AvAttr || AvAttr->isInvalid())
          continue;

        mergeWithInferredAvailability(AvAttr, Inferred[AvAttr->Platform]);

        if (Message.empty() && !AvAttr->Message.empty())
          Message = AvAttr->Message;

        if (Rename.empty() && !AvAttr->Rename.empty()) {
          Rename = AvAttr->Rename;
          RenameDecl = AvAttr->RenameDecl;
        }
      }

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

const AvailableAttr *
AvailabilityInference::attrForAnnotatedAvailableRange(const Decl *D,
                                                      ASTContext &Ctx) {
  const AvailableAttr *bestAvailAttr = nullptr;

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

llvm::Optional<AvailableAttrDeclPair>
SemanticAvailableRangeAttrRequest::evaluate(Evaluator &evaluator,
                                            const Decl *decl) const {
  if (auto attr = AvailabilityInference::attrForAnnotatedAvailableRange(
          decl, decl->getASTContext()))
    return std::make_pair(attr, decl);

  if (auto *parent =
          AvailabilityInference::parentDeclForInferredAvailability(decl))
    return parent->getSemanticAvailableRangeAttr();

  return llvm::None;
}

llvm::Optional<AvailableAttrDeclPair>
Decl::getSemanticAvailableRangeAttr() const {
  auto &eval = getASTContext().evaluator;
  return evaluateOrDefault(eval, SemanticAvailableRangeAttrRequest{this},
                           llvm::None);
}

llvm::Optional<AvailabilityContext>
AvailabilityInference::annotatedAvailableRange(const Decl *D, ASTContext &Ctx) {
  auto bestAvailAttr = attrForAnnotatedAvailableRange(D, Ctx);
  if (!bestAvailAttr)
    return llvm::None;

  return availableRange(bestAvailAttr, Ctx);
}

bool Decl::isAvailableAsSPI() const {
  return AvailabilityInference::availableRange(this, getASTContext())
    .isAvailableAsSPI();
}

llvm::Optional<AvailableAttrDeclPair>
SemanticUnavailableAttrRequest::evaluate(Evaluator &evaluator,
                                         const Decl *decl) const {
  // Directly marked unavailable.
  if (auto attr = decl->getAttrs().getUnavailable(decl->getASTContext()))
    return std::make_pair(attr, decl);

  if (auto *parent =
          AvailabilityInference::parentDeclForInferredAvailability(decl))
    return parent->getSemanticUnavailableAttr();

  return llvm::None;
}

llvm::Optional<AvailableAttrDeclPair> Decl::getSemanticUnavailableAttr() const {
  auto &eval = getASTContext().evaluator;
  return evaluateOrDefault(eval, SemanticUnavailableAttrRequest{this},
                           llvm::None);
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
  llvm::Optional<AvailabilityContext> AnnotatedRange =
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
  return AvailabilityContext{VersionRange::allGTE(attr->Introduced.value()),
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

AvailabilityContext ASTContext::getObjCMetadataUpdateCallbackAvailability() {
  return getSwift50Availability();
}

AvailabilityContext ASTContext::getObjCGetClassHookAvailability() {
  return getSwift50Availability();
}

AvailabilityContext ASTContext::getSwift50Availability() {
  auto target = LangOpts.Target;

  if (target.getArchName() == "arm64e")
    return AvailabilityContext::alwaysAvailable();

  if (target.isMacOSX()) {
    if (target.isAArch64())
      return AvailabilityContext::alwaysAvailable();

    return AvailabilityContext(
                            VersionRange::allGTE(llvm::VersionTuple(10,14,4)));
  } else if (target.isiOS()) {
    if (target.isAArch64() &&
        (target.isSimulatorEnvironment() || target.isMacCatalystEnvironment()))
      return AvailabilityContext::alwaysAvailable();

    return AvailabilityContext(
                            VersionRange::allGTE(llvm::VersionTuple(12,2)));
  } else if (target.isWatchOS()) {
    if (target.isArch64Bit())
      return AvailabilityContext::alwaysAvailable();

    return AvailabilityContext(
                            VersionRange::allGTE(llvm::VersionTuple(5,2)));
  } else {
    return AvailabilityContext::alwaysAvailable();
  }
}

AvailabilityContext ASTContext::getOpaqueTypeAvailability() {
  return getSwift51Availability();
}

AvailabilityContext ASTContext::getObjCClassStubsAvailability() {
  return getSwift51Availability();
}

AvailabilityContext ASTContext::getSwift51Availability() {
  auto target = LangOpts.Target;
  
  if (target.getArchName() == "arm64e")
    return AvailabilityContext::alwaysAvailable();

  if (target.isMacOSX()) {
    if (target.isAArch64())
      return AvailabilityContext::alwaysAvailable();

    return AvailabilityContext(
                            VersionRange::allGTE(llvm::VersionTuple(10,15,0)));
  } else if (target.isiOS()) {
    if (target.isAArch64() &&
        (target.isSimulatorEnvironment() || target.isMacCatalystEnvironment()))
      return AvailabilityContext::alwaysAvailable();

    return AvailabilityContext(
                            VersionRange::allGTE(llvm::VersionTuple(13,0,0)));
  } else if (target.isWatchOS()) {
    if (target.isArch64Bit())
      return AvailabilityContext::alwaysAvailable();

    return AvailabilityContext(
                            VersionRange::allGTE(llvm::VersionTuple(6,0,0)));
  } else {
    return AvailabilityContext::alwaysAvailable();
  }
}

AvailabilityContext ASTContext::getTypesInAbstractMetadataStateAvailability() {
  return getSwift52Availability();
}

AvailabilityContext ASTContext::getPrespecializedGenericMetadataAvailability() {
  return getSwift54Availability();
}

AvailabilityContext ASTContext::getCompareTypeContextDescriptorsAvailability() {
  return getSwift54Availability();
}

AvailabilityContext
ASTContext::getCompareProtocolConformanceDescriptorsAvailability() {
  return getSwift54Availability();
}

AvailabilityContext
ASTContext::getIntermodulePrespecializedGenericMetadataAvailability() {
  return getSwift54Availability();
}

AvailabilityContext ASTContext::getConcurrencyAvailability() {
  return getSwift55Availability();
}

AvailabilityContext ASTContext::getBackDeployedConcurrencyAvailability() {
  return getSwift51Availability();
}

AvailabilityContext ASTContext::getConcurrencyDistributedActorWithCustomExecutorAvailability() {
  return getSwift59Availability();
}

AvailabilityContext ASTContext::getDifferentiationAvailability() {
  return getSwiftFutureAvailability();
}

AvailabilityContext ASTContext::getMultiPayloadEnumTagSinglePayload() {
  return getSwift56Availability();
}

AvailabilityContext ASTContext::getObjCIsUniquelyReferencedAvailability() {
  return getSwift56Availability();
}

AvailabilityContext
ASTContext::getParameterizedExistentialRuntimeAvailability() {
  return getSwift57Availability();
}

AvailabilityContext
ASTContext::getImmortalRefCountSymbolsAvailability() {
  // TODO: replace this with a concrete swift version once we have it.
  // rdar://94185998
  return getSwiftFutureAvailability();
}

AvailabilityContext
ASTContext::getVariadicGenericTypeAvailability() {
  return getSwift59Availability();
}

AvailabilityContext ASTContext::getSwift52Availability() {
  auto target = LangOpts.Target;

  if (target.getArchName() == "arm64e")
    return AvailabilityContext::alwaysAvailable();

  if (target.isMacOSX()) {
    if (target.isAArch64())
      return AvailabilityContext::alwaysAvailable();

    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(10, 15, 4)));
  } else if (target.isiOS()) {
    if (target.isAArch64() &&
        (target.isSimulatorEnvironment() || target.isMacCatalystEnvironment()))
      return AvailabilityContext::alwaysAvailable();

    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(13, 4, 0)));
  } else if (target.isWatchOS()) {
    if (target.isArch64Bit())
      return AvailabilityContext::alwaysAvailable();

    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(6, 2, 0)));
  }
  return AvailabilityContext::alwaysAvailable();
}

AvailabilityContext ASTContext::getSwift53Availability() {
  auto target = LangOpts.Target;

  if (target.getArchName() == "arm64e")
    return AvailabilityContext::alwaysAvailable();

  if (target.isMacOSX() ) {
    if (target.isAArch64())
      return AvailabilityContext::alwaysAvailable();

    llvm::VersionTuple macOVersion53(10, 16, 0);
    macOVersion53 = canonicalizePlatformVersion(PlatformKind::macOS, macOVersion53);
    return AvailabilityContext(
        VersionRange::allGTE(macOVersion53));
  } else if (target.isiOS()) {
    if (target.isAArch64() &&
        (target.isSimulatorEnvironment() || target.isMacCatalystEnvironment()))
      return AvailabilityContext::alwaysAvailable();

    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(14, 0, 0)));
  } else if (target.isWatchOS()) {
    if (target.isArch64Bit())
      return AvailabilityContext::alwaysAvailable();

    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(7, 0, 0)));
  } else {
    return AvailabilityContext::alwaysAvailable();
  }
}

AvailabilityContext ASTContext::getSwift54Availability() {
  auto target = LangOpts.Target;

  if (target.isMacOSX()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(11, 3, 0)));
  } else if (target.isiOS()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(14, 5, 0)));
  } else if (target.isWatchOS()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(7, 4, 0)));
  } else {
    return AvailabilityContext::alwaysAvailable();
  }
}

AvailabilityContext ASTContext::getSwift55Availability() {
  auto target = LangOpts.Target;

  if (target.isMacOSX() ) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(12, 0, 0)));
  } else if (target.isiOS()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(15, 0, 0)));
  } else if (target.isWatchOS()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(8, 0, 0)));
  } else {
    return AvailabilityContext::alwaysAvailable();
  }
}

AvailabilityContext ASTContext::getSwift56Availability() {
  auto target = LangOpts.Target;

  if (target.isMacOSX() ) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(12, 3, 0)));
  } else if (target.isiOS()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(15, 4, 0)));
  } else if (target.isWatchOS()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(8, 5, 0)));
  } else {
    return AvailabilityContext::alwaysAvailable();
  }
}

AvailabilityContext ASTContext::getSwift57Availability() {
  auto target = LangOpts.Target;

  if (target.isMacOSX()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(13, 0, 0)));
  } else if (target.isiOS()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(16, 0, 0)));
  } else if (target.isWatchOS()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(9, 0, 0)));
  } else {
    return AvailabilityContext::alwaysAvailable();
  }
}

AvailabilityContext ASTContext::getSwift58Availability() {
  auto target = LangOpts.Target;

  if (target.isMacOSX()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(13, 3, 0)));
  } else if (target.isiOS()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(16, 4, 0)));
  } else if (target.isWatchOS()) {
    return AvailabilityContext(
        VersionRange::allGTE(llvm::VersionTuple(9, 4, 0)));
  } else {
    return AvailabilityContext::alwaysAvailable();
  }
}

AvailabilityContext ASTContext::getSwift59Availability() {
  // TODO: Update Availability impl when Swift 5.9 is released
  return getSwiftFutureAvailability();
}

AvailabilityContext ASTContext::getSwiftFutureAvailability() {
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
ASTContext::getSwift5PlusAvailability(llvm::VersionTuple swiftVersion) {
  if (swiftVersion.getMajor() == 5) {
    switch (*swiftVersion.getMinor()) {
    case 0: return getSwift50Availability();
    case 1: return getSwift51Availability();
    case 2: return getSwift52Availability();
    case 3: return getSwift53Availability();
    case 4: return getSwift54Availability();
    case 5: return getSwift55Availability();
    case 6: return getSwift56Availability();
    case 7: return getSwift57Availability();
    case 8: return getSwift58Availability();
    case 9: return getSwift59Availability();
    default: break;
    }
  }
  llvm::report_fatal_error(
      Twine("Missing call to getSwiftXYAvailability for Swift ") +
      swiftVersion.getAsString());
}

bool ASTContext::supportsVersionedAvailability() const {
  return minimumAvailableOSVersionForTriple(LangOpts.Target).has_value();
}
