//===--- TypeCheckProtocol.cpp - Protocol Checking ------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements semantic analysis for protocols, in particular, checking
// whether a given type conforms to a given protocol.
//===----------------------------------------------------------------------===//

#include "TypeCheckProtocol.h"
#include "DerivedConformances.h"
#include "MiscDiagnostics.h"
#include "TypeAccessScopeChecker.h"
#include "TypeCheckAccess.h"
#include "TypeCheckAvailability.h"
#include "TypeCheckBitwise.h"
#include "TypeCheckConcurrency.h"
#include "TypeCheckDistributed.h"
#include "TypeCheckEffects.h"
#include "TypeCheckInvertible.h"
#include "TypeCheckObjC.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/AccessScope.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DistributedDecl.h"
#include "swift/AST/Effects.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PotentialMacroExpansions.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/RequirementMatch.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeDeclFinder.h"
#include "swift/AST/TypeMatcher.h"
#include "swift/AST/TypeWalker.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/StringExtras.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SaveAndRestore.h"

#define DEBUG_TYPE "Protocol conformance checking"
#include "llvm/Support/Debug.h"

using namespace swift;

namespace {
  /// Whether any of the given optional adjustments is an error (vs. a
  /// warning).
  bool hasAnyError(ArrayRef<OptionalAdjustment> adjustments) {
    for (const auto &adjustment : adjustments)
      if (adjustment.isError())
        return true;

    return false;
  }
}

namespace {
  /// The kind of variance (none, covariance, contravariance) to apply
  /// when comparing types from a witness to types in the requirement
  /// we're matching it against.
  enum class VarianceKind {
    None,
    Covariant,
    Contravariant
   };
} // end anonymous namespace

static std::tuple<Type, Type, OptionalAdjustmentKind>
getTypesToCompare(ValueDecl *reqt, Type reqtType, bool reqtTypeIsIUO,
                  Type witnessType, bool witnessTypeIsIUO,
                  VarianceKind variance) {
  // If the witness type is noescape but the requirement type is not,
  // adjust the witness type to be escaping; likewise for sendability. This
  // permits a limited form of covariance.
  auto applyAdjustment = [&](TypeAdjustment adjustment) {
    // Sometimes the witness has a function type, but the requirement has
    // something else (a dependent type we need to infer, in the most relevant
    // case). In that situation, should we behave as though the requirement type
    // *did* need the adjustment, or as though it *did not*?
    //
    // For noescape, we want to behave as though it was necessary because any
    // function type not in a parameter is, more or less, implicitly @escaping.
    // For Sendable, we want to behave as though it was not necessary because
    // function types that aren't in a parameter can be Sendable or not.
    // FIXME: Should we check for a Sendable bound on the requirement type?
    bool inRequirement = (adjustment != TypeAdjustment::NoescapeToEscaping);
    Type adjustedReqtType =
      adjustInferredAssociatedType(adjustment, reqtType, inRequirement);

    bool inWitness = false;
    Type adjustedWitnessType =
      adjustInferredAssociatedType(adjustment, witnessType, inWitness);

    switch (variance) {
    case VarianceKind::None:
      break;
    case VarianceKind::Covariant:
      if (inRequirement && !inWitness)
        reqtType = adjustedReqtType;
      break;
    case VarianceKind::Contravariant:
      if (inWitness && !inRequirement)
        witnessType = adjustedWitnessType;
      break;
    }
  };

  applyAdjustment(TypeAdjustment::NoescapeToEscaping);
  applyAdjustment(TypeAdjustment::NonsendableToSendable);

  // For @objc protocols, deal with differences in the optionality.
  // FIXME: It probably makes sense to extend this to non-@objc
  // protocols as well, but this requires more testing.
  OptionalAdjustmentKind optAdjustment = OptionalAdjustmentKind::None;

  if (!reqt->isObjC())
    return std::make_tuple(reqtType, witnessType, optAdjustment);

  bool reqtIsOptional = false;
  if (Type reqtValueType = reqtType->getOptionalObjectType()) {
    reqtIsOptional = true;
    reqtType = reqtValueType;
  }
  bool witnessIsOptional = false;
  if (Type witnessValueType = witnessType->getOptionalObjectType()) {
    witnessIsOptional = true;
    witnessType = witnessValueType;
  }

  // When the requirement is an IUO, all is permitted, because we
  // assume that the user knows more about the signature than we
  // have information in the protocol.
  if (reqtTypeIsIUO)
    return std::make_tuple(reqtType, witnessType, optAdjustment);

  if (reqtIsOptional) {
    if (witnessIsOptional) {
      if (witnessTypeIsIUO)
        optAdjustment = OptionalAdjustmentKind::IUOToOptional;
    } else {
      switch (variance) {
      case VarianceKind::None:
      case VarianceKind::Contravariant:
        optAdjustment = OptionalAdjustmentKind::ConsumesUnhandledNil;
        break;

      case VarianceKind::Covariant:
        optAdjustment = OptionalAdjustmentKind::WillNeverProduceNil;
        break;
      }
    }
  } else if (witnessIsOptional) {
    if (witnessTypeIsIUO) {
      optAdjustment = OptionalAdjustmentKind::RemoveIUO;
    } else {
      switch (variance) {
      case VarianceKind::None:
      case VarianceKind::Covariant:
        optAdjustment = OptionalAdjustmentKind::ProducesUnhandledNil;
        break;

      case VarianceKind::Contravariant:
        optAdjustment = OptionalAdjustmentKind::WillNeverConsumeNil;
        break;
      }
    }
  }

  return std::make_tuple(reqtType, witnessType, optAdjustment);
}

/// Check that the Objective-C method(s) provided by the witness have
/// the same selectors as those required by the requirement.
static bool checkObjCWitnessSelector(ValueDecl *req, ValueDecl *witness) {
  // Simple case: for methods and initializers, check that the selectors match.
  if (auto reqFunc = dyn_cast<AbstractFunctionDecl>(req)) {
    auto witnessFunc = cast<AbstractFunctionDecl>(witness);
    if (reqFunc->getObjCSelector() == witnessFunc->getObjCSelector())
      return false;

    auto diagInfo = getObjCMethodDiagInfo(witnessFunc);
    auto diag = witness->diagnose(
        diag::objc_witness_selector_mismatch, diagInfo.first, diagInfo.second,
        witnessFunc->getObjCSelector(), reqFunc->getObjCSelector());
    fixDeclarationObjCName(diag, witnessFunc,
                           witnessFunc->getObjCSelector(),
                           reqFunc->getObjCSelector());

    return true;
  }

  // Otherwise, we have an abstract storage declaration.
  auto reqStorage = cast<AbstractStorageDecl>(req);
  auto witnessStorage = cast<AbstractStorageDecl>(witness);

  // FIXME: Check property names!

  // Check the getter.
  if (auto reqGetter = reqStorage->getParsedAccessor(AccessorKind::Get)) {
    auto *witnessGetter =
      witnessStorage->getSynthesizedAccessor(AccessorKind::Get);
    if (checkObjCWitnessSelector(reqGetter, witnessGetter))
      return true;
  }

  // Check the setter.
  if (auto reqSetter = reqStorage->getParsedAccessor(AccessorKind::Set)) {
    auto *witnessSetter =
      witnessStorage->getSynthesizedAccessor(AccessorKind::Set);
    if (checkObjCWitnessSelector(reqSetter, witnessSetter))
      return true;
  }

  return false;
}

// Find a standin declaration to place the diagnostic at for the
// given accessor kind.
static ValueDecl *getStandinForAccessor(AbstractStorageDecl *witness,
                                        AccessorKind requirementKind) {
  // If the storage actually explicitly provides that accessor, great.
  if (auto accessor = witness->getParsedAccessor(requirementKind))
    return accessor;

  // If it didn't, check to see if it provides something else that corresponds
  // to the requirement.
  switch (requirementKind) {
  case AccessorKind::Get:
  case AccessorKind::DistributedGet:
    if (auto read = witness->getParsedAccessor(AccessorKind::Read))
      return read;
    if (auto addressor = witness->getParsedAccessor(AccessorKind::Address))
      return addressor;
    break;

  case AccessorKind::Read:
    if (auto getter = witness->getParsedAccessor(AccessorKind::Get))
      return getter;
    if (auto addressor = witness->getParsedAccessor(AccessorKind::Address))
      return addressor;
    break;

  case AccessorKind::Modify:
    if (auto setter = witness->getParsedAccessor(AccessorKind::Set))
      return setter;
    if (auto addressor = witness->getParsedAccessor(AccessorKind::MutableAddress))
      return addressor;
    break;

  case AccessorKind::Set:
    if (auto modify = witness->getParsedAccessor(AccessorKind::Modify))
      return modify;
    if (auto addressor = witness->getParsedAccessor(AccessorKind::MutableAddress))
      return addressor;
    break;

#define OPAQUE_ACCESSOR(ID, KEYWORD)
#define ACCESSOR(ID) \
  case AccessorKind::ID:
#include "swift/AST/AccessorKinds.def"
    llvm_unreachable("unexpected accessor requirement");
  }

  // Otherwise, just diagnose starting at the storage declaration itself.
  return witness;
}

/// Given a witness, a requirement, and an existing `RequirementMatch` result,
/// check if the requirement's `@differentiable` attributes are met by the
/// witness.
/// - If `result` is not viable, do nothing.
/// - If requirement's `@differentiable` attributes are met, update `result`
///   with the matched derivative generic signature.
/// - Otherwise, returns a "missing `@differentiable` attribute"
///   `RequirementMatch`.
static void
matchWitnessDifferentiableAttr(DeclContext *dc, ValueDecl *req,
                               ValueDecl *witness, RequirementMatch &result) {
  if (!result.isViable())
    return;

  // Get the requirement and witness attributes.
  const auto &reqAttrs = req->getAttrs();
  const auto &witnessAttrs = witness->getAttrs();

  // For all `@differentiable` attributes of the protocol requirement, check
  // that the witness has a derivative configuration with exactly the same
  // parameter indices, or one with "superset" parameter indices. If there
  // exists a witness derivative configuration with "superset" parameter
  // indices, create an implicit `@differentiable` attribute for the witness
  // with the exact parameter indices from the requirement `@differentiable`
  // attribute.
  ASTContext &ctx = witness->getASTContext();
  auto *witnessAFD = dyn_cast<AbstractFunctionDecl>(witness);
  if (auto *witnessASD = dyn_cast<AbstractStorageDecl>(witness))
    witnessAFD = witnessASD->getOpaqueAccessor(AccessorKind::Get);
  // NOTE: Validate `@differentiable` attributes by calling
  // `getParameterIndices`. This is important for type-checking
  // `@differentiable` attributes in non-primary files to skip invalid
  // attributes and to resolve derivative configurations, used below.
  for (auto *witnessDiffAttr :
       witnessAttrs.getAttributes<DifferentiableAttr>()) {
    (void)witnessDiffAttr->getParameterIndices();
  }
  for (auto *reqDiffAttr : reqAttrs.getAttributes<DifferentiableAttr>()) {
    (void)reqDiffAttr->getParameterIndices();
  }
  for (auto *reqDiffAttr : reqAttrs.getAttributes<DifferentiableAttr>()) {
    bool foundExactConfig = false;
    std::optional<AutoDiffConfig> supersetConfig = std::nullopt;
    for (auto witnessConfig :
           witnessAFD->getDerivativeFunctionConfigurations()) {
      // All the witness's derivative generic requirements must be satisfied
      // by the requirement's derivative generic requirements OR by the
      // conditional conformance requirements.
      if (witnessConfig.derivativeGenericSignature) {
        bool genericRequirementsSatisfied = true;
        auto reqDiffGenSig = reqDiffAttr->getDerivativeGenericSignature();
        auto conformanceGenSig = dc->getGenericSignatureOfContext();
        for (const auto &req :
             witnessConfig.derivativeGenericSignature.getRequirements()) {
          auto substReq = req.subst(result.WitnessSubstitutions);
          bool reqDiffGenSigSatisfies =
              reqDiffGenSig && !substReq.hasError() &&
              reqDiffGenSig->isRequirementSatisfied(substReq);
          bool conformanceGenSigSatisfies =
              conformanceGenSig &&
              conformanceGenSig->isRequirementSatisfied(req);
          if (!reqDiffGenSigSatisfies && !conformanceGenSigSatisfies) {
            genericRequirementsSatisfied = false;
            break;
          }
        }
        if (!genericRequirementsSatisfied)
          continue;
      }

      if (witnessConfig.parameterIndices ==
          reqDiffAttr->getParameterIndices()) {
        foundExactConfig = true;
        // Store the matched witness derivative generic signature.
        result.DerivativeGenSig = witnessConfig.derivativeGenericSignature;
        break;
      }
      if (witnessConfig.parameterIndices->isSupersetOf(
              reqDiffAttr->getParameterIndices()))
        supersetConfig = witnessConfig;
    }

    // If no exact witness derivative configuration was found, check conditions
    // for creating an implicit witness `@differentiable` attribute with the
    // exact derivative configuration.
    if (!foundExactConfig) {
      auto witnessInDifferentFile =
          dc->getParentSourceFile() !=
          witness->getDeclContext()->getParentSourceFile();
      auto witnessInDifferentTypeContext =
          dc->getInnermostTypeContext() !=
          witness->getDeclContext()->getInnermostTypeContext();
      // Produce an error instead of creating an implicit `@differentiable`
      // attribute if any of the following conditions are met:
      // - The witness is in a different file than the conformance
      //   declaration.
      // - The witness is in a different type context (i.e. extension) than
      //   the conformance declaration, and there is no existing
      //   `@differentiable` attribute that covers the required differentiation
      //   parameters.
      if (witnessInDifferentFile ||
          (witnessInDifferentTypeContext && !supersetConfig)) {
        // FIXME(TF-1014): `@differentiable` attribute diagnostic does not
        // appear if associated type inference is involved.
        if (auto *vdWitness = dyn_cast<VarDecl>(witness)) {
          result = RequirementMatch(
              getStandinForAccessor(vdWitness, AccessorKind::Get),
              MatchKind::MissingDifferentiableAttr, reqDiffAttr);
        } else {
          result = RequirementMatch(
              witness, MatchKind::MissingDifferentiableAttr, reqDiffAttr);
        }
      }

      // Otherwise, the witness must:
      // - Have a "superset" derivative configuration.
      // - Have less than public visibility.
      //   - `@differentiable` attributes are really only significant for
      //     public declarations: it improves usability to not require
      //     explicit `@differentiable` attributes for less-visible
      //     declarations.
      //
      // If these conditions are met, an implicit `@differentiable` attribute
      // with the exact derivative configuration can be created.
      bool success = false;
      bool createImplicitWitnessAttribute =
          supersetConfig || witness->getFormalAccess() < AccessLevel::Public;
      if (createImplicitWitnessAttribute) {
        auto derivativeGenSig = witnessAFD->getGenericSignature();
        if (supersetConfig)
          derivativeGenSig = supersetConfig->derivativeGenericSignature;
        // Use source location of the witness declaration as the source location
        // of the implicit `@differentiable` attribute.
        auto *newAttr = DifferentiableAttr::create(
            witnessAFD, /*implicit*/ true, witness->getLoc(), witness->getLoc(),
            reqDiffAttr->getDifferentiabilityKind(),
            reqDiffAttr->getParameterIndices(),
            derivativeGenSig);
        // If the implicit attribute is inherited from a protocol requirement's
        // attribute, store the protocol requirement attribute's location for
        // use in diagnostics.
        if (witness->getFormalAccess() < AccessLevel::Public) {
          newAttr->getImplicitlyInheritedDifferentiableAttrLocation(
              reqDiffAttr->getLocation());
        }
        auto insertion = ctx.DifferentiableAttrs.try_emplace(
            {witnessAFD, newAttr->getParameterIndices()}, newAttr);
        // Valid `@differentiable` attributes are uniqued by original function
        // and parameter indices. Reject duplicate attributes.
        if (!insertion.second) {
          newAttr->setInvalid();
        } else {
          witness->getAttrs().add(newAttr);
          success = true;
          // Register derivative function configuration.
          auto *resultIndices =
            autodiff::getFunctionSemanticResultIndices(witnessAFD,
                                                       newAttr->getParameterIndices());
          witnessAFD->addDerivativeFunctionConfiguration(
              {newAttr->getParameterIndices(), resultIndices,
               newAttr->getDerivativeGenericSignature()});
          // Store the witness derivative generic signature.
          result.DerivativeGenSig = newAttr->getDerivativeGenericSignature();
        }
      }
      if (!success) {
        LLVM_DEBUG({
          llvm::dbgs() << "Protocol requirement match failure: missing "
                          "`@differentiable` attribute for witness ";
          witnessAFD->dumpRef(llvm::dbgs());
          llvm::dbgs() << " from requirement ";
          req->dumpRef(llvm::dbgs());
          llvm::dbgs() << '\n';
        });
        // FIXME(TF-1014): `@differentiable` attribute diagnostic does not
        // appear if associated type inference is involved.
        if (auto *vdWitness = dyn_cast<VarDecl>(witness)) {
          result = RequirementMatch(
              getStandinForAccessor(vdWitness, AccessorKind::Get),
              MatchKind::MissingDifferentiableAttr, reqDiffAttr);
        } else {
          result = RequirementMatch(
              witness, MatchKind::MissingDifferentiableAttr, reqDiffAttr);
        }
      }
    }
  }
}

/// A property or subscript witness must have the same or fewer
/// effects specifiers than the protocol requirement.
///
/// \returns None iff the witness satisfies the requirement's effects limit.
///          Otherwise, it returns the RequirementMatch that describes the
///          problem.
static std::optional<RequirementMatch>
checkEffects(AbstractStorageDecl *witness, AbstractStorageDecl *req) {

  if (!witness->isLessEffectfulThan(req, EffectKind::Async))
    return RequirementMatch(getStandinForAccessor(witness, AccessorKind::Get),
                            MatchKind::AsyncConflict);

  if (!witness->isLessEffectfulThan(req, EffectKind::Throws))
    return RequirementMatch(getStandinForAccessor(witness, AccessorKind::Get),
                            MatchKind::ThrowsConflict);

  return std::nullopt; // OK
}

RequirementMatch swift::matchWitness(
    DeclContext *dc, ValueDecl *req, ValueDecl *witness,
    llvm::function_ref<
        std::tuple<std::optional<RequirementMatch>, Type, Type>(void)>
        setup,
    llvm::function_ref<std::optional<RequirementMatch>(Type, Type)> matchTypes,
    llvm::function_ref<RequirementMatch(bool, ArrayRef<OptionalAdjustment>)>
        finalize) {

  assert(!req->isInvalid() && "Cannot have an invalid requirement here");

  /// Make sure the witness is of the same kind as the requirement.
  if (req->getKind() != witness->getKind()) {
    // An enum case can witness:
    // 1. A static get-only property requirement, as long as the property's
    //    type is `Self` or it matches the type of the enum explicitly.
    // 2. A static function requirement, if the enum case has a payload
    //    and the payload types and labels match the function and the
    //    function returns `Self` or the type of the enum.
    //
    // If there are any discrepencies, we'll diagnose it later. For now,
    // let's assume the match is valid.
    if (!((isa<VarDecl>(req) || isa<FuncDecl>(req)) &&
          isa<EnumElementDecl>(witness)))
      return RequirementMatch(witness, MatchKind::KindConflict);
  }

  // If we're currently validating the witness, bail out.
  if (witness->isRecursiveValidation()) {
    return RequirementMatch(witness, MatchKind::Circularity);
  }
  
  // If the witness is invalid, record that and stop now.
  if (witness->isInvalid()) {
    return RequirementMatch(witness, MatchKind::WitnessInvalid);
  }

  // Get the requirement and witness attributes.
  const auto &reqAttrs = req->getAttrs();
  const auto &witnessAttrs = witness->getAttrs();

  // Perform basic matching of the requirement and witness.
  bool decomposeFunctionType = false;
  bool ignoreReturnType = false;
  Type reqThrownError;
  Type witnessThrownError;
  if (isa<FuncDecl>(req) && isa<FuncDecl>(witness)) {
    auto funcReq = cast<FuncDecl>(req);
    auto funcWitness = cast<FuncDecl>(witness);

    // Either both must be 'static' or neither.
    if (funcReq->isStatic() != funcWitness->isStatic() &&
        !(funcReq->isOperator() &&
          !funcWitness->getDeclContext()->isTypeContext()))
      return RequirementMatch(witness, MatchKind::StaticNonStaticConflict);

    // If we require a prefix operator and the witness is not a prefix operator,
    // these don't match.
    if (reqAttrs.hasAttribute<PrefixAttr>() &&
        !witnessAttrs.hasAttribute<PrefixAttr>())
      return RequirementMatch(witness, MatchKind::PrefixNonPrefixConflict);

    // If we require a postfix operator and the witness is not a postfix
    // operator, these don't match.
    if (reqAttrs.hasAttribute<PostfixAttr>() &&
        !witnessAttrs.hasAttribute<PostfixAttr>())
      return RequirementMatch(witness, MatchKind::PostfixNonPostfixConflict);

    // Check that the mutating bit is ok.
    if (!funcReq->isMutating() && funcWitness->isMutating())
      return RequirementMatch(witness, MatchKind::MutatingConflict);

    // If the requirement has an explicit 'rethrows' argument, the witness
    // must be 'rethrows', too.
    if (reqAttrs.hasAttribute<RethrowsAttr>()) {
      auto reqRethrowingKind =
          funcReq->getPolymorphicEffectKind(EffectKind::Throws);
      auto witnessRethrowingKind =
          funcWitness->getPolymorphicEffectKind(EffectKind::Throws);

      assert(reqRethrowingKind != PolymorphicEffectKind::Always &&
             reqRethrowingKind != PolymorphicEffectKind::None);

      switch (witnessRethrowingKind) {
      case PolymorphicEffectKind::None:
      case PolymorphicEffectKind::Invalid:
      case PolymorphicEffectKind::ByClosure:
      case PolymorphicEffectKind::AsyncSequenceRethrows:
        break;

      case PolymorphicEffectKind::ByConformance: {
        // A by-conformance `rethrows` witness cannot witness a
        // by-conformance `rethrows` requirement unless the protocol
        // is @rethrows. Otherwise, we don't have enough information
        // at the call site to assess if the conformance actually
        // throws or not.
        auto *proto = cast<ProtocolDecl>(req->getDeclContext());
        if (reqRethrowingKind == PolymorphicEffectKind::ByConformance &&
            proto->hasPolymorphicEffect(EffectKind::Throws))
          break;

        return RequirementMatch(witness,
                                MatchKind::RethrowsByConformanceConflict);
      }

      case PolymorphicEffectKind::Always:
        // If the witness is using typed throws in a manner that looks
        // like rethrows, allow it.
        if (isRethrowLikeTypedThrows(funcWitness))
          break;

        return RequirementMatch(witness, MatchKind::RethrowsConflict);
      }
    }
    // We want to decompose the parameters to handle them separately.
    decomposeFunctionType = true;
  } else if (auto *witnessASD = dyn_cast<AbstractStorageDecl>(witness)) {
    auto *reqASD = cast<AbstractStorageDecl>(req);
    
    // Check that the static-ness matches.
    if (reqASD->isStatic() != witnessASD->isStatic())
      return RequirementMatch(witness, MatchKind::StaticNonStaticConflict);

    // Check that the compile-time constness matches.
    if (reqASD->isCompileTimeConst() && !witnessASD->isCompileTimeConst()) {
      return RequirementMatch(witness, MatchKind::CompileTimeConstConflict);
    }

    // If the requirement is settable and the witness is not, reject it.
    if (reqASD->isSettable(req->getDeclContext()) &&
        !witnessASD->isSettable(witness->getDeclContext()))
      return RequirementMatch(witness, MatchKind::SettableConflict);

    // Validate that the 'mutating' bit lines up for getters and setters.
    if (!reqASD->isGetterMutating() && witnessASD->isGetterMutating())
      return RequirementMatch(getStandinForAccessor(witnessASD, AccessorKind::Get),
                              MatchKind::MutatingConflict);

    if (reqASD->isSettable(req->getDeclContext())) {
      if (!reqASD->isSetterMutating() && witnessASD->isSetterMutating())
        return RequirementMatch(getStandinForAccessor(witnessASD, AccessorKind::Set),
                                MatchKind::MutatingConflict);
    }

    // Check for async mismatches.
    if (!witnessASD->isLessEffectfulThan(reqASD, EffectKind::Async)) {
      return RequirementMatch(
          getStandinForAccessor(witnessASD, AccessorKind::Get),
          MatchKind::AsyncConflict);
    }

    // Check that the witness has no more effects than the requirement.
    if (auto problem = checkEffects(witnessASD, reqASD))
      return problem.value();

    // Decompose the parameters for subscript declarations.
    decomposeFunctionType = isa<SubscriptDecl>(req);

    // Dig out the thrown error types from the getter so we can compare them
    // later.
    auto getThrownErrorType = [](AbstractStorageDecl *asd) -> Type {
      if (auto getter = asd->getEffectfulGetAccessor()) {
        if (Type thrownErrorType = getter->getThrownInterfaceType()) {
          return thrownErrorType;
        } else if (getter->hasThrows()) {
          return asd->getASTContext().getAnyExistentialType();
        }
      }

      return asd->getASTContext().getNeverType();
    };

    reqThrownError = getThrownErrorType(reqASD);
    witnessThrownError = getThrownErrorType(witnessASD);
  } else if (isa<ConstructorDecl>(witness)) {
    decomposeFunctionType = true;
    ignoreReturnType = true;
  } else if (auto *enumCase = dyn_cast<EnumElementDecl>(witness)) {
    // An enum case with associated values can satisfy only a
    // method requirement.
    if (enumCase->hasAssociatedValues() && isa<VarDecl>(req))
      return RequirementMatch(witness, MatchKind::EnumCaseWithAssociatedValues);

    // An enum case can satisfy only a method or property requirement.
    if (!isa<VarDecl>(req) && !isa<FuncDecl>(req))
      return RequirementMatch(witness, MatchKind::KindConflict);

    // An enum case can satisfy only a static requirement.
    if (!req->isStatic())
      return RequirementMatch(witness, MatchKind::StaticNonStaticConflict);

    // An enum case cannot satisfy a settable property requirement.
    if (isa<VarDecl>(req) &&
        cast<VarDecl>(req)->isSettable(req->getDeclContext()))
      return RequirementMatch(witness, MatchKind::SettableConflict);

    decomposeFunctionType = enumCase->hasAssociatedValues();
  }

  // If the requirement is @objc, the witness must not be marked with @nonobjc.
  // @objc-ness will be inferred (separately) and the selector will be checked
  // later.
  if (req->isObjC() && witness->getAttrs().hasAttribute<NonObjCAttr>())
    return RequirementMatch(witness, MatchKind::NonObjC);

  // Set up the match, determining the requirement and witness types
  // in the process.
  Type reqType, witnessType;
  {
    std::optional<RequirementMatch> result;
    std::tie(result, reqType, witnessType) = setup();
    if (result) {
      return std::move(result.value());
    }
  }

  SmallVector<OptionalAdjustment, 2> optionalAdjustments;
  const bool anyRenaming = req->getName() != witness->getName();
  if (decomposeFunctionType) {
    // Decompose function types into parameters and result type.
    auto reqFnType = reqType->castTo<AnyFunctionType>();
    auto reqResultType = reqFnType->getResult()->getRValueType();
    auto witnessFnType = witnessType->castTo<AnyFunctionType>();
    auto witnessResultType = witnessFnType->getResult()->getRValueType();

    // Result types must match.
    // FIXME: Could allow (trivial?) subtyping here.
    if (!ignoreReturnType) {
      auto reqTypeIsIUO = req->isImplicitlyUnwrappedOptional();
      auto witnessTypeIsIUO = witness->isImplicitlyUnwrappedOptional();
      auto types =
          getTypesToCompare(req, reqResultType, reqTypeIsIUO, witnessResultType,
                            witnessTypeIsIUO, VarianceKind::Covariant);

      // Record optional adjustment, if any.
      if (std::get<2>(types) != OptionalAdjustmentKind::None) {
        optionalAdjustments.push_back(
          OptionalAdjustment(std::get<2>(types)));
      }

      if (!req->isObjC() &&
          !isa_and_nonnull<clang::CXXMethodDecl>(witness->getClangDecl()) &&
          reqTypeIsIUO != witnessTypeIsIUO)
        return RequirementMatch(witness, MatchKind::TypeConflict, witnessType);

      if (auto result = matchTypes(std::get<0>(types), std::get<1>(types))) {
        return std::move(result.value());
      }
    }

    // Parameter types and kinds must match. Start by decomposing the input
    // types into sets of tuple elements.
    // Decompose the input types into parameters.
    auto reqParams = reqFnType->getParams();
    auto witnessParams = witnessFnType->getParams();

    // If the number of parameters doesn't match, we're done.
    if (reqParams.size() != witnessParams.size())
      return RequirementMatch(witness, MatchKind::TypeConflict, 
                              witnessType);

    ParameterList *witnessParamList = getParameterList(witness);
    assert(witnessParamList->size() == witnessParams.size());

    ParameterList *reqParamList = getParameterList(req);
    assert(reqParamList->size() == reqParams.size());

    // Match each of the parameters.
    for (unsigned i = 0, n = reqParams.size(); i != n; ++i) {
      // Variadic bits must match.
      // FIXME: Specialize the match failure kind
      if (reqParams[i].isVariadic() != witnessParams[i].isVariadic())
        return RequirementMatch(witness, MatchKind::TypeConflict, witnessType);

      if (reqParams[i].isInOut() != witnessParams[i].isInOut())
        return RequirementMatch(witness, MatchKind::TypeConflict, witnessType);

      auto reqParamDecl = reqParamList->get(i);
      auto witnessParamDecl = witnessParamList->get(i);

      auto reqParamTypeIsIUO = reqParamDecl->isImplicitlyUnwrappedOptional();
      auto witnessParamTypeIsIUO =
          witnessParamDecl->isImplicitlyUnwrappedOptional();

      // Gross hack: strip a level of unchecked-optionality off both
      // sides when matching against a protocol imported from Objective-C.
      auto types =
          getTypesToCompare(req, reqParams[i].getOldType(), reqParamTypeIsIUO,
                            witnessParams[i].getOldType(), witnessParamTypeIsIUO,
                            VarianceKind::Contravariant);

      // Record any optional adjustment that occurred.
      if (std::get<2>(types) != OptionalAdjustmentKind::None) {
        optionalAdjustments.push_back(
          OptionalAdjustment(std::get<2>(types), i));
      }

      if (!req->isObjC() && reqParamTypeIsIUO != witnessParamTypeIsIUO)
        return RequirementMatch(witness, MatchKind::TypeConflict, witnessType);

      if (auto result = matchTypes(std::get<0>(types), std::get<1>(types))) {
        return std::move(result.value());
      }
    }

    if (witnessFnType->hasExtInfo()) {
      // If the witness is 'async', the requirement must be.
      if (witnessFnType->getExtInfo().isAsync() &&
            !reqFnType->getExtInfo().isAsync()) {
        return RequirementMatch(witness, MatchKind::AsyncConflict);
      }

      // If witness is sync, the requirement cannot be @objc and 'async'
      if (!witnessFnType->getExtInfo().isAsync() &&
            (req->isObjC() && reqFnType->getExtInfo().isAsync())) {
        return RequirementMatch(witness, MatchKind::AsyncConflict);
      }

      if (!reqThrownError) {
        // Save the thrown error types of the requirement and witness so we
        // can check them later.
        reqThrownError = reqFnType->getEffectiveThrownErrorTypeOrNever();
        witnessThrownError = witnessFnType->getEffectiveThrownErrorTypeOrNever();
      }
    }
  } else {
    auto reqTypeIsIUO = req->isImplicitlyUnwrappedOptional();
    auto witnessTypeIsIUO = witness->isImplicitlyUnwrappedOptional();
    auto types = getTypesToCompare(req, reqType, reqTypeIsIUO, witnessType,
                                   witnessTypeIsIUO, VarianceKind::None);

    // Record optional adjustment, if any.
    if (std::get<2>(types) != OptionalAdjustmentKind::None) {
      optionalAdjustments.push_back(
        OptionalAdjustment(std::get<2>(types)));
    }

    if (!req->isObjC() && reqTypeIsIUO != witnessTypeIsIUO)
      return RequirementMatch(witness, MatchKind::TypeConflict, witnessType);

    if (auto result = matchTypes(std::get<0>(types), std::get<1>(types))) {
      return std::move(result.value());
    }
  }

  // Check the thrown error types. This includes 'any Error' and 'Never' for
  // untyped throws and non-throwing cases as well.
  if (reqThrownError && witnessThrownError) {
    auto thrownErrorTypes = getTypesToCompare(
        req, reqThrownError, false, witnessThrownError, false,
        VarianceKind::None);

    Type reqThrownError = std::get<0>(thrownErrorTypes);
    Type witnessThrownError = std::get<1>(thrownErrorTypes);
    switch (compareThrownErrorsForSubtyping(witnessThrownError, reqThrownError,
                                            dc)) {
    case ThrownErrorSubtyping::DropsThrows:
    case ThrownErrorSubtyping::Mismatch:
      return RequirementMatch(witness, MatchKind::ThrowsConflict);

    case ThrownErrorSubtyping::ExactMatch:
      // All is well.
      break;

    case ThrownErrorSubtyping::Subtype:
      // If there were no type parameters, we're done.
      if (!reqThrownError->hasTypeParameter())
        break;

      LLVM_FALLTHROUGH;

    case ThrownErrorSubtyping::Dependent:
      // We need to perform type matching
      if (auto result = matchTypes(reqThrownError, witnessThrownError)) {
        return std::move(result.value());
      }
      break;
    }
  }

  // Now finalize the match.
  auto result = finalize(anyRenaming, optionalAdjustments);
  // Check if the requirement's `@differentiable` attributes are satisfied by
  // the witness.
  matchWitnessDifferentiableAttr(dc, req, witness, result);
  return result;
}

/// Checks \p reqEnvCache for a requirement environment appropriate for
/// \p reqSig and \p covariantSelf. If one isn't there, it gets created from
/// the rest of the parameters.
///
/// Note that this means RequirementEnvironmentCaches must not be shared across
/// multiple protocols or conformances.
static const RequirementEnvironment &getOrCreateRequirementEnvironment(
    WitnessChecker::RequirementEnvironmentCache &reqEnvCache,
    DeclContext *dc, GenericSignature reqSig, ProtocolDecl *proto,
    ClassDecl *covariantSelf, RootProtocolConformance *conformance) {
  WitnessChecker::RequirementEnvironmentCacheKey cacheKey(reqSig.getPointer(),
                                                          covariantSelf);
  auto cacheIter = reqEnvCache.find(cacheKey);
  if (cacheIter == reqEnvCache.end()) {
    RequirementEnvironment reqEnv(dc, reqSig, proto, covariantSelf,
                                  conformance);
    cacheIter = reqEnvCache.insert({cacheKey, std::move(reqEnv)}).first;
  }
  return cacheIter->getSecond();
}

static std::optional<RequirementMatch>
findMissingGenericRequirementForSolutionFix(
    constraints::Solution &solution, constraints::ConstraintFix *fix,
    ValueDecl *witness, ProtocolConformance *conformance,
    const RequirementEnvironment &reqEnvironment) {
  Type type, missingType;
  RequirementKind requirementKind;

  using namespace constraints;

  switch (fix->getKind()) {
  case FixKind::AddConformance: {
    auto missingConform = (MissingConformance *)fix;
    requirementKind = RequirementKind::Conformance;
    type = missingConform->getNonConformingType();
    missingType = missingConform->getProtocolType();
    break;
  }
  case FixKind::SkipSameTypeRequirement: {
    requirementKind = RequirementKind::SameType;
    auto requirementFix = (SkipSameTypeRequirement *)fix;
    type = requirementFix->lhsType();
    missingType = requirementFix->rhsType();
    break;
  }
  case FixKind::SkipSameShapeRequirement: {
    requirementKind = RequirementKind::SameShape;
    auto requirementFix = (SkipSameShapeRequirement *)fix;
    type = requirementFix->lhsType();
    missingType = requirementFix->rhsType();
    break;
  }
  case FixKind::SkipSuperclassRequirement: {
    requirementKind = RequirementKind::Superclass;
    auto requirementFix = (SkipSuperclassRequirement *)fix;
    type = requirementFix->subclassType();
    missingType = requirementFix->superclassType();
    break;
  }
  default:
    return std::optional<RequirementMatch>();
  }

  type = solution.simplifyType(type);
  missingType = solution.simplifyType(missingType);

  if (auto *env = conformance->getGenericEnvironment()) {
    // We use subst() with LookUpConformanceInModule here, because
    // associated type inference failures mean that we can end up
    // here with a DependentMemberType with an ArchetypeType base.
    missingType = missingType.subst(
      [&](SubstitutableType *type) -> Type {
        return env->mapTypeIntoContext(type->mapTypeOutOfContext());
      },
      LookUpConformanceInModule(conformance->getDeclContext()->getParentModule()));
  }

  auto missingRequirementMatch = [&](Type type) -> RequirementMatch {
    Requirement requirement(requirementKind, type, missingType);
    return RequirementMatch(witness, MatchKind::MissingRequirement,
                            requirement);
  };

  if (type->is<DependentMemberType>())
    return missingRequirementMatch(type);

  type = type->mapTypeOutOfContext();
  if (type->hasTypeParameter())
    if (auto env = conformance->getGenericEnvironment())
      if (auto assocType = env->mapTypeIntoContext(type))
        return missingRequirementMatch(assocType);

  auto reqSubMap = reqEnvironment.getRequirementToWitnessThunkSubs();
  auto proto = conformance->getProtocol();
  Type selfTy = proto->getSelfInterfaceType().subst(reqSubMap);
  if (type->isEqual(selfTy)) {
    type = conformance->getType();

    // e.g. `extension P where Self == C { func foo() { ... } }`
    // and `C` doesn't actually conform to `P`.
    if (type->isEqual(missingType)) {
      requirementKind = RequirementKind::Conformance;
      missingType = proto->getDeclaredInterfaceType();
    }

    if (auto agt = type->getAs<AnyGenericType>())
      type = agt->getDecl()->getDeclaredInterfaceType();
    return missingRequirementMatch(type);
  }

  return std::optional<RequirementMatch>();
}

/// Determine the set of effects on a given declaration.
static PossibleEffects getEffects(ValueDecl *value) {
  if (auto func = dyn_cast<AbstractFunctionDecl>(value)) {
    PossibleEffects result;
    if (func->hasThrows())
      result |= EffectKind::Throws;
    if (func->hasAsync())
      result |= EffectKind::Async;
    return result;
  }

  if (auto storage = dyn_cast<AbstractStorageDecl>(value)) {
    if (auto accessor = storage->getEffectfulGetAccessor())
      return getEffects(accessor);
  }

  return PossibleEffects();
}

RequirementMatch
swift::matchWitness(WitnessChecker::RequirementEnvironmentCache &reqEnvCache,
                    ProtocolDecl *proto, RootProtocolConformance *conformance,
                    DeclContext *dc, ValueDecl *req, ValueDecl *witness) {
  using namespace constraints;

  // Initialized by the setup operation.
  std::optional<ConstraintSystem> cs;
  ConstraintLocator *reqLocator = nullptr;
  ConstraintLocator *witnessLocator = nullptr;
  Type witnessType, openWitnessType;
  Type reqType;

  GenericSignature reqSig = proto->getGenericSignature();
  if (auto *funcDecl = dyn_cast<AbstractFunctionDecl>(req)) {
    if (funcDecl->isGeneric())
      reqSig = funcDecl->getGenericSignature();
  } else if (auto *subscriptDecl = dyn_cast<SubscriptDecl>(req)) {
    if (subscriptDecl->isGeneric())
      reqSig = subscriptDecl->getGenericSignature();
  }

  ClassDecl *covariantSelf = nullptr;
  if (witness->getDeclContext()->getExtendedProtocolDecl()) {
    if (auto *classDecl = dc->getSelfClassDecl()) {
      if (!classDecl->isSemanticallyFinal()) {
        // If the requirement's type does not involve any associated types,
        // we use a class-constrained generic parameter as the 'Self' type
        // in the witness thunk.
        //
        // This allows the following code to type check:
        //
        // protocol P {
        //   func f() -> Self
        // }
        //
        // extension P {
        //   func f() { return self }
        // }
        //
        // class C : P {}
        //
        // When we call (C() as P).f(), we want to pass the 'Self' type
        // from the call site, not the static 'Self' type of the conformance.
        //
        // On the other hand, if the requirement's type contains associated
        // types, we use the static 'Self' type, to preserve backward
        // compatibility with code that uses this pattern:
        //
        // protocol P {
        //   associatedtype T = Self
        //   func f() -> T
        // }
        //
        // extension P where Self.T == Self {
        //   func f() -> Self { return self }
        // }
        //
        // class C : P {}
        //
        // It would have been much nicer to just ban this completely if
        // the class 'C' is not final, but there is a great deal of existing
        // code out there that relies on this behavior, most commonly by
        // defining a non-final class conforming to 'Collection' which uses
        // the default witness for 'Collection.Iterator', which is defined
        // as 'IndexingIterator<Self>'.
        const auto selfRefInfo = req->findExistentialSelfReferences(
            proto->getDeclaredInterfaceType(),
            /*treatNonResultCovariantSelfAsInvariant=*/true);
        if (!selfRefInfo.assocTypeRef) {
          covariantSelf = classDecl;
        }
      }
    }
  }

  const RequirementEnvironment &reqEnvironment =
      getOrCreateRequirementEnvironment(reqEnvCache, dc, reqSig, proto,
                                        covariantSelf, conformance);

  // Set up the constraint system for matching.
  auto setup =
      [&]() -> std::tuple<std::optional<RequirementMatch>, Type, Type> {
    // Construct a constraint system to use to solve the equality between
    // the required type and the witness type.
    cs.emplace(dc, ConstraintSystemFlags::AllowFixes);

    auto syntheticSig = reqEnvironment.getWitnessThunkSignature();
    auto *syntheticEnv = syntheticSig.getGenericEnvironment();
    auto reqSubMap = reqEnvironment.getRequirementToWitnessThunkSubs();

    Type selfTy = proto->getSelfInterfaceType().subst(reqSubMap);
    if (syntheticEnv)
      selfTy = syntheticEnv->mapTypeIntoContext(selfTy);

    // Open up the type of the requirement.
    reqLocator =
        cs->getConstraintLocator(req, ConstraintLocator::ProtocolRequirement);
    OpenedTypeMap reqReplacements;
    reqType = cs->getTypeOfMemberReference(selfTy, req, dc,
                                           /*isDynamicResult=*/false,
                                           FunctionRefKind::DoubleApply,
                                           reqLocator,
                                           &reqReplacements)
        .adjustedReferenceType;
    reqType = reqType->getRValueType();

    // For any type parameters we replaced in the witness, map them
    // to the corresponding archetypes in the witness's context.
    for (const auto &replacement : reqReplacements) {
      auto replacedInReq = Type(replacement.first).subst(reqSubMap);

      // If substitution failed, skip the requirement. This only occurs in
      // invalid code.
      if (replacedInReq->hasError())
        continue;

      if (syntheticEnv) {
        replacedInReq = syntheticEnv->mapTypeIntoContext(replacedInReq);
      }

      if (auto packType = replacedInReq->getAs<PackType>()) {
        if (auto unwrapped = packType->unwrapSingletonPackExpansion())
          replacedInReq = unwrapped->getPatternType();
      }

      cs->addConstraint(ConstraintKind::Bind, replacement.second, replacedInReq,
                        reqLocator);
    }

    // Open up the witness type.
    witnessType = witness->getInterfaceType();
    witnessLocator =
        cs->getConstraintLocator(req, LocatorPathElt::Witness(witness));
    if (witness->getDeclContext()->isTypeContext()) {
      openWitnessType = cs->getTypeOfMemberReference(
          selfTy, witness, dc, /*isDynamicResult=*/false,
          FunctionRefKind::DoubleApply, witnessLocator)
        .adjustedReferenceType;
    } else {
      openWitnessType = cs->getTypeOfReference(
          witness, FunctionRefKind::DoubleApply, witnessLocator,
          /*useDC=*/nullptr)
        .adjustedReferenceType;
    }
    openWitnessType = openWitnessType->getRValueType();

    return std::make_tuple(std::nullopt, reqType, openWitnessType);
  };

  // Match a type in the requirement to a type in the witness.
  auto matchTypes = [&](Type reqType,
                        Type witnessType) -> std::optional<RequirementMatch> {
    cs->addConstraint(ConstraintKind::Bind, reqType, witnessType,
                      witnessLocator);
    // FIXME: Check whether this has already failed.
    return std::nullopt;
  };

  // Finalize the match.
  auto finalize = [&](bool anyRenaming, 
                      ArrayRef<OptionalAdjustment> optionalAdjustments) 
                        -> RequirementMatch {
    // Try to solve the system disallowing free type variables, because
    // that would resolve in incorrect substitution matching when witness
    // type has free type variables present as well.
    auto solution = cs->solveSingle(FreeTypeVariableBinding::Disallow,
                                    /* allowFixes */ true);

    // If the types would match but for some other missing conformance, find and
    // call that out.
    if (solution && conformance && solution->Fixes.size()) {
      for (auto fix : solution->Fixes) {
        if (auto result = findMissingGenericRequirementForSolutionFix(
                *solution, fix, witness, conformance, reqEnvironment))
          return *result;
      }
    }

    bool requiresNonSendable = [&]() {
      if (!solution)
        return false;

      // If the *only* problems are that `@Sendable` attributes are missing,
      // allow the match in some circumstances.
      if (!solution->Fixes.empty()) {
        return llvm::all_of(solution->Fixes,
                            [](constraints::ConstraintFix *fix) {
                              return fix->getKind() ==
                                     constraints::FixKind::AddSendableAttribute;
                            });
      }

      // If there are no other issues, let's check whether this are
      // missing Sendable conformances when matching ObjC requirements.
      // This is not an error until Swift 6 because `swift_attr` wasn't
      // allowed in type contexts initially.
      return req->isObjC() &&
             solution->getFixedScore()
                     .Data[SK_MissingSynthesizableConformance] > 0;
    }();

    if (!solution || !solution->Fixes.empty()) {
      if (!requiresNonSendable)
        return RequirementMatch(witness, MatchKind::TypeConflict,
                                witnessType);
    }

    MatchKind matchKind = MatchKind::ExactMatch;
    if (hasAnyError(optionalAdjustments))
      matchKind = MatchKind::OptionalityConflict;
    else if (anyRenaming)
      matchKind = MatchKind::RenamedMatch;
    else if (requiresNonSendable)
      matchKind = MatchKind::RequiresNonSendable;
    else if (getEffects(req) - getEffects(witness))
      // when the difference is non-empty, the witness has fewer effects.
      matchKind = MatchKind::FewerEffects;

    assert(getEffects(req).contains(getEffects(witness))
               && "witness has more effects than requirement?");

    // Success. Form the match result.
    RequirementMatch result(witness,
                            matchKind,
                            witnessType,
                            reqEnvironment,
                            optionalAdjustments);

    // Compute the set of substitutions we'll need for the witness.
    auto witnessSig =
      witness->getInnermostDeclContext()->getGenericSignatureOfContext();
    result.WitnessSubstitutions =
        solution->computeSubstitutions(witness, witnessSig, witnessLocator);

    return result;
  };

  return matchWitness(dc, req, witness, setup, matchTypes, finalize);
}

bool
swift::witnessHasImplementsAttrForRequiredName(ValueDecl *witness,
                                               ValueDecl *requirement) {
  if (auto A = witness->getAttrs().getAttribute<ImplementsAttr>()) {
    return A->getMemberName() == requirement->getName();
  }
  return false;
}

bool
swift::witnessHasImplementsAttrForExactRequirement(ValueDecl *witness,
                                                   ValueDecl *requirement) {
  assert(requirement->isProtocolRequirement());
  auto *PD = cast<ProtocolDecl>(requirement->getDeclContext());
  if (auto A = witness->getAttrs().getAttribute<ImplementsAttr>()) {
    if (auto *OtherPD = A->getProtocol(witness->getDeclContext())) {
      if (OtherPD == PD) {
        return A->getMemberName() == requirement->getName();
      }
    }
  }
  return false;
}

/// Determine whether one requirement match is better than the other.
static bool isBetterMatch(DeclContext *dc, ValueDecl *requirement,
                          const RequirementMatch &match1,
                          const RequirementMatch &match2) {

  // Special case to prefer a witness with @_implements(Foo, bar) over one without
  // it, when the requirement was exactly for Foo.bar.
  bool match1ImplementsAttr =
    witnessHasImplementsAttrForExactRequirement(match1.Witness,
                                                requirement);
  bool match2ImplementsAttr =
    witnessHasImplementsAttrForExactRequirement(match2.Witness,
                                                requirement);
  if (match1ImplementsAttr && !match2ImplementsAttr) {
    return true;
  } else if (!match1ImplementsAttr && match2ImplementsAttr) {
    return false;
  }

  // Check whether one declaration is better than the other.
  const auto comparisonResult =
      TypeChecker::compareDeclarations(dc, match1.Witness, match2.Witness);
  switch (comparisonResult) {
  case Comparison::Better:
    return true;

  case Comparison::Worse:
    return false;

  case Comparison::Unordered:
    break;
  }

  // Earlier match kinds are better. This prefers exact matches over matches
  // that require renaming, for example.
  if (match1.Kind != match2.Kind)
    return static_cast<unsigned>(match1.Kind)
             < static_cast<unsigned>(match2.Kind);

  return false;
}

WitnessChecker::WitnessChecker(ASTContext &ctx, ProtocolDecl *proto,
                               Type adoptee, DeclContext *dc)
    : Context(ctx), Proto(proto), Adoptee(adoptee), DC(dc) {}

static void
lookupValueWitnessesViaImplementsAttr(
    DeclContext *DC, ValueDecl *req, SmallVector<ValueDecl *, 4> &witnesses) {

  auto name = req->createNameRef();
  auto *nominal = DC->getSelfNominalTypeDecl();

  NLOptions subOptions = (NL_ProtocolMembers | NL_IncludeAttributeImplements);

  nominal->synthesizeSemanticMembersIfNeeded(name.getFullName());

  SmallVector<ValueDecl *, 4> lookupResults;
  DC->lookupQualified(nominal, name, nominal->getLoc(),
                      subOptions, lookupResults);

  for (auto decl : lookupResults) {
    if (!isa<ProtocolDecl>(decl->getDeclContext()))
      if (witnessHasImplementsAttrForExactRequirement(decl, req))
        witnesses.push_back(decl);
  }

  removeOverriddenDecls(witnesses);
  removeShadowedDecls(witnesses, DC);
}

/// Determine whether the given context may expand an operator with the given name.
static bool contextMayExpandOperator(
    DeclContext *dc, DeclBaseName operatorName
) {
  TypeOrExtensionDecl decl;
  if (auto nominal = dyn_cast<NominalTypeDecl>(dc))
    decl = nominal;
  else if (auto ext = dyn_cast<ExtensionDecl>(dc))
    decl = ext;
  else
    return false;

  // If the context declaration itself is within a macro expansion, it may
  // have an operator.
  if (auto sourceFile = dc->getParentSourceFile()) {
    if (sourceFile->getFulfilledMacroRole())
      return true;
  }

  ASTContext &ctx = dc->getASTContext();
  auto potentialExpansions = evaluateOrDefault(
      ctx.evaluator, PotentialMacroExpansionsInContextRequest{decl},
      PotentialMacroExpansions());
  return potentialExpansions.shouldExpandForName(operatorName);
}

SmallVector<ValueDecl *, 4>
swift::lookupValueWitnesses(DeclContext *DC, ValueDecl *req, bool *ignoringNames) {
  assert(!isa<AssociatedTypeDecl>(req) && "Not for lookup for type witnesses*");
  assert(req->isProtocolRequirement());

  SmallVector<ValueDecl *, 4> witnesses;

  // Do an initial check to see if there are any @_implements remappings
  // for this requirement.
  lookupValueWitnessesViaImplementsAttr(DC, req, witnesses);

  auto reqName = req->createNameRef();
  auto reqBaseName = reqName.withoutArgumentLabels();

  // An operator function is the only kind of witness that requires global
  // lookup. However, because global lookup doesn't enter local contexts,
  // an additional, qualified lookup is warranted when the conforming type
  // is declared in a local context or when the operator could come from a
  // macro expansion.
  const bool doUnqualifiedLookup = req->isOperator();
  const bool doQualifiedLookup =
      !req->isOperator() || DC->getParent()->getLocalContext() ||
      contextMayExpandOperator(DC, req->getName().getBaseName());

  if (doUnqualifiedLookup) {
    auto lookup = TypeChecker::lookupUnqualified(DC->getModuleScopeContext(),
                                                 reqBaseName, SourceLoc(),
                                                 defaultUnqualifiedLookupOptions);
    for (auto candidate : lookup) {
      auto decl = candidate.getValueDecl();
      if (!isa<ProtocolDecl>(decl->getDeclContext()) &&
          swift::isMemberOperator(cast<FuncDecl>(decl), DC->getSelfInterfaceType())) {
        witnesses.push_back(decl);
      }
    }
  }

  if (doQualifiedLookup) {
    auto *nominal = DC->getSelfNominalTypeDecl();
    nominal->synthesizeSemanticMembersIfNeeded(reqName.getFullName());

    // Unqualified lookup would have already found candidates from protocol
    // extensions, including those that match only by base name. Take care not
    // to restate them in the resulting list, or else an otherwise valid
    // conformance will become ambiguous.
    const NLOptions options =
        doUnqualifiedLookup ? NLOptions(0) : NL_ProtocolMembers;

    SmallVector<ValueDecl *, 4> lookupResults;
    bool addedAny = false;
    DC->lookupQualified(nominal, reqName, nominal->getLoc(),
                        options, lookupResults);
    for (auto *decl : lookupResults) {
      // a distributed thunk is the witness
      if (!isa<ProtocolDecl>(decl->getDeclContext())) {
        auto func = dyn_cast<AbstractFunctionDecl>(req);
        if (func && func->isDistributedThunk()) {
          if (auto candidate = dyn_cast<AbstractFunctionDecl>(decl)) {
            if (auto thunk = candidate->getDistributedThunk()) {
              witnesses.push_back(thunk);
              addedAny = true;
            }
          }
        } else {
          witnesses.push_back(decl);
          addedAny = true;
        }
      }
    };

    // If we didn't find anything with the appropriate name, look
    // again using only the base name.
    if (!addedAny && ignoringNames) {
      lookupResults.clear();
      DC->lookupQualified(nominal, reqBaseName, nominal->getLoc(),
                          options, lookupResults);
      for (auto *decl : lookupResults) {
        if (!isa<ProtocolDecl>(decl->getDeclContext())) {
          witnesses.push_back(decl);
        }
      }

      *ignoringNames = true;
    }

    removeOverriddenDecls(witnesses);
    removeShadowedDecls(witnesses, DC);
  }

  assert(llvm::none_of(witnesses, [](ValueDecl *decl) {
    return isa<ProtocolDecl>(decl->getDeclContext());
  }));

  return witnesses;
}

bool WitnessChecker::findBestWitness(
                               ValueDecl *requirement,
                               bool *ignoringNames,
                               NormalProtocolConformance *conformance,
                               SmallVectorImpl<RequirementMatch> &matches,
                               unsigned &numViable,
                               unsigned &bestIdx,
                               bool &doNotDiagnoseMatches) {
  enum Attempt {
    Regular,
    OperatorsFromOverlay,
    Done
  };

  bool anyFromUnconstrainedExtension;
  numViable = 0;
  for (Attempt attempt = Regular; numViable == 0 && attempt != Done;
       attempt = static_cast<Attempt>(attempt + 1)) {
    SmallVector<ValueDecl *, 4> witnesses;
    switch (attempt) {
    case Regular:
      witnesses = lookupValueWitnesses(DC, requirement, ignoringNames);
      break;
    case OperatorsFromOverlay: {
      // If we have a Clang declaration, the matching operator might be in the
      // overlay for that module.
      if (!requirement->isOperator())
        continue;

      auto *clangModule =
          dyn_cast<ClangModuleUnit>(DC->getModuleScopeContext());
      if (!clangModule)
        continue;

      DeclContext *overlay = clangModule->getOverlayModule();
      if (!overlay)
        continue;

      auto lookup = TypeChecker::lookupUnqualified(
          overlay, requirement->createNameRef(), SourceLoc(),
          defaultUnqualifiedLookupOptions);
      for (auto candidate : lookup)
        witnesses.push_back(candidate.getValueDecl());
      break;
    }
    case Done:
      llvm_unreachable("should have exited loop");
    }

    // Match each of the witnesses to the requirement.
    anyFromUnconstrainedExtension = false;
    bestIdx = 0;

    for (auto witness : witnesses) {
      // Don't match anything in a protocol.
      // FIXME: When default implementations come along, we can try to match
      // these when they're default implementations coming from another
      // (unrelated) protocol.
      if (isa<ProtocolDecl>(witness->getDeclContext())) {
        continue;
      }

      auto match = matchWitness(ReqEnvironmentCache, Proto, conformance,
                                DC, requirement, witness);
      if (match.isViable()) {
        ++numViable;
        bestIdx = matches.size();
      } else if (match.Kind == MatchKind::WitnessInvalid) {
        doNotDiagnoseMatches = true;
      }

      if (auto *ext = dyn_cast<ExtensionDecl>(match.Witness->getDeclContext())){
        if (!ext->isConstrainedExtension() && ext->getExtendedProtocolDecl())
          anyFromUnconstrainedExtension = true;
      }

      matches.push_back(std::move(match));
    }
  }

  // If there are multiple viable matches, drop any that are less available than the
  // requirement.
  if (numViable > 1) {
    SmallVector<RequirementMatch, 2> checkedMatches;
    bool foundCheckedMatch = false;

    for (auto match : matches) {
      if (!match.isViable()) {
        checkedMatches.push_back(match);
      } else if (checkWitness(requirement, match).Kind != CheckKind::Availability) {
        foundCheckedMatch = true;
        checkedMatches.push_back(match);
      }
    }

    // If none of the matches were at least as available as the requirement, don't
    // drop any of them; this will produce better diagnostics.
    if (foundCheckedMatch)
      std::swap(checkedMatches, matches);
  }

  if (numViable == 0) {
    // Assume any missing value witnesses for a conformance in a module
    // interface can be treated as opaque.
    // FIXME: ...but we should do something better about types.
    if (conformance && !conformance->isInvalid()) {
      if (auto *SF = DC->getParentSourceFile()) {
        if (SF->Kind == SourceFileKind::Interface) {
          auto match = matchWitness(ReqEnvironmentCache, Proto,
                                    conformance, DC, requirement, requirement);
          if (match.isViable()) {
            numViable = 1;
            bestIdx = matches.size();
            matches.push_back(std::move(match));
            return true;
          }
        }
      }
    }

    if (anyFromUnconstrainedExtension &&
        conformance != nullptr &&
        conformance->isInvalid()) {
      doNotDiagnoseMatches = true;
    }

    return false;
  }

  // If there numerous viable matches, throw out the non-viable matches
  // and try to find a "best" match.
  bool isReallyBest = true;
  if (numViable > 1) {
    matches.erase(std::remove_if(matches.begin(), matches.end(),
                                 [](const RequirementMatch &match) {
                                   return !match.isViable();
                                 }),
                  matches.end());

    // Find the best match.
    bestIdx = 0;
    for (unsigned i = 1, n = matches.size(); i != n; ++i) {
      if (isBetterMatch(DC, requirement, matches[i], matches[bestIdx]))
        bestIdx = i;
    }

    // Make sure it is, in fact, the best.
    for (unsigned i = 0, n = matches.size(); i != n; ++i) {
      if (i == bestIdx)
        continue;

      if (!isBetterMatch(DC, requirement, matches[bestIdx], matches[i])) {
        isReallyBest = false;
        break;
      }
    }
  }

  // If there are multiple equally-good candidates, we fail.
  return isReallyBest;
}

ConformanceAccessScope ConformanceAccessScopeRequest::evaluate(
    Evaluator &evaluator, DeclContext *dc, ProtocolDecl *proto) const {
  AccessScope result = proto->getFormalAccessScope(dc);
  bool witnessesMustBeUsableFromInline = false;

  auto *nominal = dc->getSelfNominalTypeDecl();

  // We're either looking at a concrete conformance, or the default witness
  // table for a resilient protocol.
  if (!isa<ProtocolDecl>(nominal)) {
    // Compute the intersection of the conforming type's access scope
    // and the protocol's access scope.
    auto scopeIntersection =
        result.intersectWith(nominal->getFormalAccessScope(dc));
    assert(scopeIntersection.has_value());
    result = scopeIntersection.value();

    if (!result.isPublic()) {
      witnessesMustBeUsableFromInline =
          proto->getFormalAccessScope(
            dc, /*usableFromInlineAsPublic*/true).isPublic() &&
          nominal->getFormalAccessScope(
            dc, /*usableFromInlineAsPublic*/true).isPublic();
    }
  } else {
    if (!result.isPublic()) {
      witnessesMustBeUsableFromInline =
          proto->getFormalAccessScope(
            dc, /*usableFromInlineAsPublic*/true).isPublic();
    }
  }

  return std::make_pair(result, witnessesMustBeUsableFromInline);
}

static bool checkWitnessAccess(DeclContext *dc,
                               ValueDecl *requirement,
                               ValueDecl *witness,
                               bool *isSetter) {
  *isSetter = false;

  auto *proto = cast<ProtocolDecl>(requirement->getDeclContext());

  auto requiredAccessScope = evaluateOrDefault(
      dc->getASTContext().evaluator, ConformanceAccessScopeRequest{dc, proto},
      std::make_pair(AccessScope::getPublic(), false));

  auto actualScopeToCheck = requiredAccessScope.first;

  // Setting the 'forConformance' flag means that we admit witnesses in
  // protocol extensions that we can see, but are not necessarily as
  // visible as the conforming type and protocol.
  if (!witness->isAccessibleFrom(actualScopeToCheck.getDeclContext(),
                                 /*forConformance=*/true)) {
    // Special case: if we have `@testable import` of the witness's module,
    // allow the witness to match if it would have matched for just this file.
    // That is, if '@testable' allows us to see the witness here, it should
    // allow us to see it anywhere, because any other client could also add
    // their own `@testable import`.
    // Same with @_private(sourceFile:) import.
    if (auto parentFile = dc->getParentSourceFile()) {
      const ModuleDecl *witnessModule = witness->getModuleContext();
      if (parentFile->getParentModule() != witnessModule &&
          parentFile->hasTestableOrPrivateImport(witness->getFormalAccess(),
                                                 witness) &&
          witness->isAccessibleFrom(parentFile)) {
        actualScopeToCheck = parentFile;
      }
    }

    if (actualScopeToCheck.hasEqualDeclContextWith(requiredAccessScope.first))
      return true;
  }

  if (auto *requirementASD = dyn_cast<AbstractStorageDecl>(requirement)) {
    if (requirementASD->isSettable(dc)) {
      *isSetter = true;

      auto witnessASD = cast<AbstractStorageDecl>(witness);

      // See above about the forConformance flag.
      if (!witnessASD->isSetterAccessibleFrom(actualScopeToCheck.getDeclContext(),
                                              /*forConformance=*/true))
        return true;
    }
  }

  return false;
}

bool WitnessChecker::
checkWitnessAvailability(ValueDecl *requirement,
                         ValueDecl *witness,
                         AvailabilityContext *requiredAvailability) {
  return (!getASTContext().LangOpts.DisableAvailabilityChecking &&
          !TypeChecker::isAvailabilitySafeForConformance(
              Proto, requirement, witness, DC, *requiredAvailability));
}

RequirementCheck WitnessChecker::checkWitness(ValueDecl *requirement,
                                              const RequirementMatch &match) {
  if (!match.OptionalAdjustments.empty())
    return CheckKind::OptionalityConflict;

  auto requiredAccessScope = evaluateOrDefault(
      Context.evaluator, ConformanceAccessScopeRequest{DC, Proto},
      std::make_pair(AccessScope::getPublic(), false));

  bool isSetter = false;
  if (checkWitnessAccess(DC, requirement, match.Witness, &isSetter)) {
    CheckKind kind = (isSetter
                      ? CheckKind::AccessOfSetter
                      : CheckKind::Access);

    return RequirementCheck(kind, requiredAccessScope.first);
  }

  if (requiredAccessScope.second) {
    bool witnessIsUsableFromInline = match.Witness->getFormalAccessScope(
        DC, /*usableFromInlineAsPublic*/true).isPublic();
    if (!witnessIsUsableFromInline)
      return CheckKind::UsableFromInline;
  }

  auto requiredAvailability = AvailabilityContext::alwaysAvailable();
  if (checkWitnessAvailability(requirement, match.Witness,
                               &requiredAvailability)) {
    return RequirementCheck(CheckKind::Availability, requiredAvailability);
  }

  if (requirement->getAttrs().isUnavailable(getASTContext()) &&
      match.Witness->getDeclContext() == DC) {
    return RequirementCheck(CheckKind::Unavailable);
  }

  // A non-failable initializer requirement cannot be satisfied
  // by a failable initializer.
  if (auto ctor = dyn_cast<ConstructorDecl>(requirement)) {
    if (!ctor->isFailable()) {
      auto witnessCtor = cast<ConstructorDecl>(match.Witness);

      if (witnessCtor->isFailable()) {
        if (witnessCtor->isImplicitlyUnwrappedOptional()) {
          // Only allowed for non-@objc protocols.
          if (Proto->isObjC())
            return CheckKind::ConstructorFailability;
        } else {
          return CheckKind::ConstructorFailability;
        }
      }
    }
  }

  if (match.Witness->getAttrs().isUnavailable(getASTContext()) &&
      !requirement->getAttrs().isUnavailable(getASTContext())) {
    auto nominalOrExtensionIsUnavailable = [&]() {
      if (auto extension = dyn_cast<ExtensionDecl>(DC)) {
        if (extension->getAttrs().isUnavailable(getASTContext()))
          return true;
      }

      if (auto adoptingNominal = DC->getSelfNominalTypeDecl()) {
        if (adoptingNominal->getSemanticUnavailableAttr())
          return true;
      }

      return false;
    };

    // Allow unavailable nominals or extension to have unavailable witnesses.
    if (!nominalOrExtensionIsUnavailable())
      return CheckKind::WitnessUnavailable;
  }

  return CheckKind::Success;
}

# pragma mark Witness resolution

/// Retrieve the Objective-C method key from the given function.
ObjCRequirementMap::MethodKey
ObjCRequirementMap::getObjCMethodKey(AbstractFunctionDecl *func) {
  return std::make_pair(func->getObjCSelector(), func->isInstanceMember());
}

/// Precompute map for getObjCRequirements().
ObjCRequirementMap
ObjCRequirementMapRequest::evaluate(Evaluator &evaluator,
                                    const ProtocolDecl *proto) const {
  // This map only applies to Obj-C protocols so it's wasteful to evaluate this
  // request and cache the result for non-Obj-C protocols.
  assert(proto->isObjC());

  ObjCRequirementMap map;
  for (auto requirement : proto->getProtocolRequirements()) {
    auto funcRequirement = dyn_cast<AbstractFunctionDecl>(requirement);
    if (!funcRequirement)
      continue;

    map.addRequirement(funcRequirement);
  }

  return map;
}

/// @returns a non-null requirement if the given requirement is part of a
/// group of ObjC requirements that share the same ObjC method key.
/// The first such requirement that the predicate function returns true for
/// is the requirement required by this function. Otherwise, nullptr is
/// returned.
static ValueDecl *getObjCRequirementSibling(
    ProtocolDecl *proto, ValueDecl *requirement,
    llvm::function_ref<bool(AbstractFunctionDecl *)> predicate) {
  if (!proto->isObjC())
    return nullptr;

  assert(requirement->isProtocolRequirement());
  assert(proto == requirement->getDeclContext()->getAsDecl());

  // We only care about functions
  if (auto fnRequirement = dyn_cast<AbstractFunctionDecl>(requirement)) {
    auto map = proto->getObjCRequiremenMap();
    auto similarRequirements = map.getRequirements(fnRequirement);
    // ... whose selector is one that maps to multiple requirement declarations.
    for (auto candidate : similarRequirements) {
      if (candidate == fnRequirement)
        continue; // skip the requirement we're trying to resolve.

      if (!predicate(candidate))
        continue; // skip if doesn't match requirements

      return candidate;
    }
  }

  return nullptr;
}

static void emitDelayedDiags(NormalProtocolConformance *conformance) {
  auto *dc = conformance->getDeclContext();
  auto diags = dc->getASTContext().takeDelayedConformanceDiags(conformance);
  bool alreadyComplained = false;
  for (const auto &diag: diags) {
    // Complain that the type does not conform, once.
    if (diag.IsError && !alreadyComplained) {
      diagnoseConformanceFailure(dc->getSelfInterfaceType(),
                                 conformance->getProtocol(),
                                 dc,
                                 conformance->getLoc());
      alreadyComplained = true;
    }

    diag.Callback(conformance);
  }
}

namespace {

/// This is a wrapper of multiple instances of ConformanceChecker to allow us
/// to diagnose and fix code from a more global perspective; for instance,
/// having this wrapper can help issue a fixit that inserts protocol stubs from
/// multiple protocols under checking.
class MultiConformanceChecker {
  ASTContext &Context;
  llvm::SmallVector<ValueDecl*, 16> UnsatisfiedReqs;
  llvm::SmallVector<NormalProtocolConformance*, 4> AllConformances;
  llvm::SmallPtrSet<ValueDecl *, 8> CoveredMembers;

  /// Check one conformance.
  void checkIndividualConformance(
    NormalProtocolConformance *conformance);

  /// Determine whether the given requirement was left unsatisfied.
  bool isUnsatisfiedReq(NormalProtocolConformance *conformance, ValueDecl *req);

public:
  MultiConformanceChecker(ASTContext &ctx) : Context(ctx) {}

  ASTContext &getASTContext() const { return Context; }

  /// Add a conformance into the batched checker.
  void addConformance(NormalProtocolConformance *conformance) {
    AllConformances.push_back(conformance);
  }

  /// Peek the unsatisfied requirements collected during conformance checking.
  ArrayRef<ValueDecl*> getUnsatisfiedRequirements() {
    return llvm::ArrayRef(UnsatisfiedReqs);
  }

  /// Whether this member is "covered" by one of the conformances.
  bool isCoveredMember(ValueDecl *member) const {
    return CoveredMembers.count(member) > 0;
  }

  /// Check all conformances and emit diagnosis globally.
  void checkAllConformances();
};

}

bool MultiConformanceChecker::isUnsatisfiedReq(
    NormalProtocolConformance *conformance, ValueDecl *req) {
  if (conformance->isInvalid()) return false;
  if (isa<TypeDecl>(req)) return false;

  auto witness = conformance->hasWitness(req)
                     ? conformance->getWitnessUncached(req).getDecl()
                     : nullptr;

  if (!witness) {
    auto *proto = conformance->getProtocol();

    // If another @objc requirement refers to the same Objective-C
    // method, this requirement isn't unsatisfied.
    if (getObjCRequirementSibling(
            proto, req, [conformance](AbstractFunctionDecl *cand) {
              return static_cast<bool>(conformance->getWitness(cand));
            })) {
      return false;
    }

    // An optional requirement might not have a witness.
    return req->getAttrs().hasAttribute<OptionalAttr>();
  }

  // If the witness lands within the declaration context of the conformance,
  // record it as a "covered" member.
  if (witness->getDeclContext() == conformance->getDeclContext())
    CoveredMembers.insert(witness);

  // The witness might come from a protocol or protocol extension.
  if (witness->getDeclContext()->getSelfProtocolDecl())
    return true;

  return false;
}

static void
diagnoseMatch(ModuleDecl *module, NormalProtocolConformance *conformance,
              ValueDecl *req, const RequirementMatch &match);

static void diagnoseProtocolStubFixit(
    ASTContext &ctx,
    NormalProtocolConformance *conformance,
    ArrayRef<ASTContext::MissingWitness> missingWitnesses);

void MultiConformanceChecker::checkAllConformances() {
  llvm::SmallVector<ASTContext::MissingWitness, 2> MissingWitnesses;

  bool anyInvalid = false;
  for (auto *conformance : AllConformances) {
    checkIndividualConformance(conformance);
    anyInvalid |= conformance->isInvalid();
    if (!anyInvalid) {
      // Check whether there are any unsatisfied requirements.
      auto proto = conformance->getProtocol();

      for (auto *req : proto->getProtocolRequirements()) {
        // If the requirement is unsatisfied, we might want to warn
        // about near misses; record it.
        if (isUnsatisfiedReq(conformance, req)) {
          UnsatisfiedReqs.push_back(req);
          continue;
        }
      }
    }

    // Don't diagnose missing witnesses if we can't conform to the protocol
    // at all.
    if (conformance->getProtocol()->hasMissingRequirements()) {
      assert(conformance->isInvalid());
      continue;
    }

    auto LocalMissing = Context.takeDelayedMissingWitnesses(conformance);
    if (LocalMissing.empty())
      continue;

    MissingWitnesses.append(LocalMissing.begin(), LocalMissing.end());

    // Diagnose the missing witnesses.
    for (auto &Missing : LocalMissing) {
      auto requirement = Missing.requirement;
      auto matches = Missing.matches;

      Context.addDelayedConformanceDiag(conformance, true,
        [requirement, matches](NormalProtocolConformance *conformance) {
          auto dc = conformance->getDeclContext();
          auto *protocol = conformance->getProtocol();
          auto *nominal = dc->getSelfNominalTypeDecl();
          // Possibly diagnose reason for automatic derivation failure
          DerivedConformance::tryDiagnoseFailedDerivation(dc, nominal, protocol);
          // Diagnose each of the matches.
          for (const auto &match : matches) {
            diagnoseMatch(dc->getParentModule(), conformance, requirement, match);
          }
        });
    }
  }

  // Emit missing witness fixits for all conformances in the batch.
  if (!MissingWitnesses.empty()) {
    for (auto *conformance : llvm::reverse(AllConformances)) {
      if (Context.hasDelayedConformanceErrors(conformance)) {
        diagnoseProtocolStubFixit(Context, conformance,
                                  MissingWitnesses);
        break;
      }
    }
  }

  // Emit diagnostics at the very end.
  for (auto *conformance : AllConformances) {
      emitDelayedDiags(conformance);
  }
}

static void diagnoseConformanceImpliedByConditionalConformance(
    DiagnosticEngine &Diags, NormalProtocolConformance *conformance,
    NormalProtocolConformance *implyingConf) {
  auto proto = conformance->getProtocol();
  Type protoType = proto->getDeclaredInterfaceType();
  auto implyingProto = implyingConf->getProtocol()->getDeclaredInterfaceType();
  auto loc = implyingConf->getLoc();
  Diags.diagnose(loc, diag::conditional_conformances_cannot_imply_conformances,
                 conformance->getType(), implyingProto, protoType);

  // Now we get down to business: constructing a few options for new
  // extensions. They all look like:
  //
  // extension T: ProtoType where ... {
  //     <# witnesses #>
  // }
  //
  // The options are:
  //
  // - if possible, the original bounds relaxed, when the requirements match the
  //   conforming protocol, e.g. 'X: Hashable where T: Hashable' often
  //   corresponds to 'X: Equatable where T: Equatable'. This fixit is included
  //   if all the requirements are conformance constraints to the protocol
  //   that implies the conformance.
  // - the same bounds: ... is copied from the implying extension
  // - new bounds: ... becomes a placeholder
  //
  // We could also suggest adding ", ProtoType" to the existing extension,
  // but we don't think having multiple conformances in a single extension
  // (especially conditional ones) is good Swift style, and so we don't
  // want to encourage it.

  auto ext = cast<ExtensionDecl>(implyingConf->getDeclContext());
  auto &ctxt = ext->getASTContext();

  auto &SM = ctxt.SourceMgr;
  StringRef extraIndent;
  StringRef indent = Lexer::getIndentationForLine(SM, loc, &extraIndent);

  // First, the bits that aren't the requirements are the same for all the
  // types.
  llvm::SmallString<128> prefix;
  llvm::SmallString<128> suffix;
  {
    llvm::raw_svector_ostream prefixStream(prefix);
    llvm::raw_svector_ostream suffixStream(suffix);

    prefixStream << "extension " << ext->getExtendedType() << ": " << protoType << " ";
    suffixStream << " {\n"
                 << indent << extraIndent << "<#witnesses#>\n"
                 << indent << "}\n\n"
                 << indent;
  }

  if (!ctxt.LangOpts.DiagnosticsEditorMode) {
    // The fixits below are too complicated for the command line: the suggested
    // code ends up not being displayed, and the text by itself doesn't help. So
    // instead we skip all that and just have some text.
    Diags.diagnose(loc,
                   diag::note_explicitly_state_conditional_conformance_noneditor,
                   prefix.str());
    return;
  }

  // First, we do the fixit for "matching" requirements (i.e. X: P where T: P).
  bool matchingIsValid = true;
  llvm::SmallString<128> matchingFixit = prefix;
  {
    llvm::raw_svector_ostream matchingStream(matchingFixit);
    matchingStream << "where ";
    bool first = true;
    for (const auto &req : implyingConf->getConditionalRequirements()) {
      auto firstType = req.getFirstType();
      // T: ImplyingProto => T: Proto
      if (req.getKind() == RequirementKind::Conformance &&
          req.getSecondType()->isEqual(implyingProto)) {
        auto comma = first ? "" : ", ";
        matchingStream << comma << firstType << ": " << protoType;
        first = false;
        continue;
      }
      // something didn't work out, so give up on this fixit.
      matchingIsValid = false;
      break;
    }
  }

  if (matchingIsValid) {
    matchingFixit += suffix;
    Diags
        .diagnose(loc,
                  diag::note_explicitly_state_conditional_conformance_relaxed)
        .fixItInsert(loc, matchingFixit);
  }

  // Next, do the fixit for using the same requirements, but be resilient to a
  // missing `where` clause: this is one of a few fixits that get emitted here,
  // and so is a very low priority diagnostic, and so shouldn't crash.
  if (auto TWC = ext->getTrailingWhereClause()) {
    llvm::SmallString<128> sameFixit = prefix;
    auto CSR =
        Lexer::getCharSourceRangeFromSourceRange(SM, TWC->getSourceRange());
    sameFixit += SM.extractText(CSR);
    sameFixit += suffix;
    Diags
        .diagnose(loc, diag::note_explicitly_state_conditional_conformance_same)
        .fixItInsert(loc, sameFixit);
  }

  // And finally, just the generic new-requirements one:
  llvm::SmallString<128> differentFixit = prefix;
  differentFixit += "where <#requirements#>";
  differentFixit += suffix;
  Diags
      .diagnose(loc,
                diag::note_explicitly_state_conditional_conformance_different)
      .fixItInsert(loc, differentFixit);
}

/// Determine whether there are additional semantic checks for conformance
/// to the given protocol. This should return true when @unchecked can be
/// used to disable those semantic checks.
static bool hasAdditionalSemanticChecks(ProtocolDecl *proto) {
  return proto->isSpecificProtocol(KnownProtocolKind::Sendable);
}

/// Determine whether a conformance to this protocol can be determined at
/// runtime for an arbitrary type.
static bool hasRuntimeConformanceInfo(ProtocolDecl *proto) {
  return !proto->isMarkerProtocol()
      || proto->isSpecificProtocol(KnownProtocolKind::Copyable)
      || proto->isSpecificProtocol(KnownProtocolKind::Escapable);
}

static void ensureRequirementsAreSatisfied(ASTContext &ctx,
                                           NormalProtocolConformance *conformance);

/// Determine whether the type \c T conforms to the protocol \c Proto,
/// recording the complete witness table if it does.
void MultiConformanceChecker::
checkIndividualConformance(NormalProtocolConformance *conformance) {
  PrettyStackTraceConformance trace("type-checking", conformance);

  switch (conformance->getState()) {
    case ProtocolConformanceState::Incomplete:
      // Check the rest of the conformance below.
      break;

    case ProtocolConformanceState::Checking:
    case ProtocolConformanceState::Complete:
      // Nothing to do.
      return;
  }

  // Dig out some of the fields from the conformance.
  Type T = conformance->getType();
  DeclContext *DC = conformance->getDeclContext();
  auto Proto = conformance->getProtocol();
  auto ProtoType = Proto->getDeclaredInterfaceType();
  SourceLoc ComplainLoc = conformance->getLoc();

  // Note that we are checking this conformance now.
  conformance->setState(ProtocolConformanceState::Checking);
  SWIFT_DEFER { conformance->setState(ProtocolConformanceState::Complete); };

  // If the protocol itself is invalid, there's nothing we can do.
  if (Proto->isInvalid()) {
    conformance->setInvalid();
    return;
  }

  // If the protocol requires a class, non-classes are a non-starter.
  if (Proto->requiresClass() && !DC->getSelfClassDecl()) {
    Context.Diags.diagnose(ComplainLoc,
                           diag::non_class_cannot_conform_to_class_protocol, T,
                           ProtoType);
    conformance->setInvalid();
    return;
  }

  if (T->isActorType()) {
    if (auto globalActor = Proto->getGlobalActorAttr()) {
      Context.Diags.diagnose(ComplainLoc,
                             diag::actor_cannot_conform_to_global_actor_protocol, T,
                             ProtoType);

      CustomAttr *attr;
      NominalTypeDecl *actor;

      std::tie(attr, actor) = *globalActor;

      Context.Diags.diagnose(attr->getLocation(),
                             diag::protocol_isolated_to_global_actor_here, ProtoType,
                             actor->getDeclaredInterfaceType());

      conformance->setInvalid();
      return;
    }
  }

  if (Proto->isObjC()) {
    // Foreign classes cannot conform to objc protocols.
    if (auto clazz = DC->getSelfClassDecl()) {
      std::optional<decltype(diag::cf_class_cannot_conform_to_objc_protocol)>
          diagKind;
      switch (clazz->getForeignClassKind()) {
        case ClassDecl::ForeignKind::Normal:
          break;
        case ClassDecl::ForeignKind::CFType:
          diagKind = diag::cf_class_cannot_conform_to_objc_protocol;
          break;
        case ClassDecl::ForeignKind::RuntimeOnly:
          diagKind = diag::objc_runtime_visible_cannot_conform_to_objc_protocol;
          break;
      }
      if (diagKind) {
        Context.Diags.diagnose(ComplainLoc, diagKind.value(), T, ProtoType);
        conformance->setInvalid();
        return;
      }
    }

    // @objc protocols can't be conditionally-conformed to. We could, in theory,
    // front-load the requirement checking to generic-instantiation time (rather
    // than conformance-lookup/construction time) and register the conformance
    // with the Obj-C runtime when they're satisfied, but we'd still have solve
    // the problem with extensions that we check for below.
    if (!conformance->getConditionalRequirements().empty()) {
      Context.Diags.diagnose(ComplainLoc,
                             diag::objc_protocol_cannot_have_conditional_conformance,
                             T, ProtoType);
      conformance->setInvalid();
      return;
    }
    // And... even if it isn't conditional, we still don't currently support
    // @objc protocols in extensions of Swift generic classes, because there's
    // no stable Objective-C class object to install the protocol conformance
    // category onto.
    if (auto ext = dyn_cast<ExtensionDecl>(DC)) {
      if (auto classDecl = ext->getSelfClassDecl()) {
        if (classDecl->isGenericContext()) {
          if (!classDecl->isTypeErasedGenericClass()) {
            Context.Diags.diagnose(ComplainLoc,
                                   diag::objc_protocol_in_generic_extension,
                                   classDecl->isGeneric(), T, ProtoType);
            conformance->setInvalid();
            return;
          }
        }
      }
    }
  }

  // Not every protocol/type is compatible with conditional conformances.
  auto conditionalReqs = conformance->getConditionalRequirements();
  if (!conditionalReqs.empty()) {
    auto nestedType = DC->getSelfInterfaceType();
    if (nestedType->getAnyNominal()) {
      // Obj-C generics cannot be looked up at runtime, so we don't support
      // conditional conformances involving them. Check the full stack of nested
      // types for any obj-c ones.
      while (nestedType) {
        if (auto clazz = nestedType->getClassOrBoundGenericClass()) {
          if (clazz->isTypeErasedGenericClass()) {
            Context.Diags.diagnose(ComplainLoc,
                                   diag::objc_generics_cannot_conditionally_conform,
                                   T, ProtoType);
            conformance->setInvalid();
            return;
          }
        }

        nestedType = nestedType->getNominalParent();
      }
    }

    // If the protocol to which we are conditionally conforming is not a marker
    // protocol, the conditional requirements must not involve conformance to a
    // protocol that cannot be evaluated at runtime, like most marker protocols.
    if (!Proto->isMarkerProtocol()) {
      for (const auto &req : conditionalReqs) {
        if (req.getKind() == RequirementKind::Conformance &&
            !hasRuntimeConformanceInfo(req.getProtocolDecl())) {
          Context.Diags.diagnose(
            ComplainLoc, diag::marker_protocol_conditional_conformance,
            Proto->getName(), req.getFirstType(),
            req.getProtocolDecl()->getName());
          conformance->setInvalid();
        }
      }
    }
  }

  // If the protocol contains missing requirements, it can't be conformed to
  // at all.
  if (Proto->hasMissingRequirements()) {
    bool hasDiagnosed = false;
    auto *protoFile = Proto->getModuleScopeContext();
    if (auto *serialized = dyn_cast<SerializedASTFile>(protoFile)) {
      const auto effectiveVers =
          getASTContext().LangOpts.EffectiveLanguageVersion;
      if (serialized->getLanguageVersionBuiltWith() != effectiveVers) {
        Context.Diags.diagnose(ComplainLoc,
                               diag::protocol_has_missing_requirements_versioned,
                               T, ProtoType, serialized->getLanguageVersionBuiltWith(),
                               effectiveVers);
        hasDiagnosed = true;
      }
    }
    if (!hasDiagnosed) {
      Context.Diags.diagnose(ComplainLoc, diag::protocol_has_missing_requirements,
                             T, ProtoType);
    }
    conformance->setInvalid();
    return;
  }

  // Complain about the use of @unchecked for protocols that don't have
  // additional semantic checks.
  if (conformance->isUnchecked() && !hasAdditionalSemanticChecks(Proto)) {
    Context.Diags.diagnose(
      ComplainLoc, diag::unchecked_conformance_not_special, ProtoType);
  }

  if (conformance->getSourceKind() == ConformanceEntryKind::Implied &&
      !Proto->isMarkerProtocol()) {
    // We've got something like:
    //
    //   protocol Foo : Proto {}
    //   extension SomeType : Foo {}
    //
    // We don't want to allow this when the SomeType : Foo conformance is
    // conditional
    auto implyingConf = conformance->getImplyingConformance();
    // There might be a long chain of implications, e.g. protocol Foo: Proto {}
    // protocol Bar: Foo {} extension SomeType: Bar {}, so keep looking all the
    // way up.
    while (implyingConf->getSourceKind() == ConformanceEntryKind::Implied) {
      implyingConf = implyingConf->getImplyingConformance();
    }

    // If the conditional requirements all have the form `T : Copyable`, then
    // we accept the implied conformance with the same conditional requirements.
    auto implyingCondReqs = implyingConf->getConditionalRequirements();
    bool allCondReqsInvertible = llvm::all_of(implyingCondReqs,
        [&](Requirement req) {
          return (req.getKind() == RequirementKind::Conformance &&
                  req.getProtocolDecl()->getInvertibleProtocolKind());
        });

    if (!allCondReqsInvertible) {
      // FIXME:
      // We shouldn't suggest including witnesses for the conformance, because
      // those suggestions will go in the current DeclContext, but really they
      // should go into the new extension we (might) suggest here.

      diagnoseConformanceImpliedByConditionalConformance(
          Context.Diags, conformance, implyingConf);

      conformance->setInvalid();
    }
  }

  // Check that T conforms to all inherited protocols.
  for (auto InheritedProto : Proto->getInheritedProtocols()) {
    auto InheritedConformance =
      DC->getParentModule()->lookupConformance(T, InheritedProto);
    if (InheritedConformance.isInvalid() ||
        !InheritedConformance.isConcrete()) {
      diagnoseConformanceFailure(T, InheritedProto, DC, ComplainLoc);

      // Recursive call already diagnosed this problem, but tack on a note
      // to establish the relationship.
      if (ComplainLoc.isValid()) {
        Context.Diags.diagnose(Proto, diag::inherited_protocol_does_not_conform,
                               T, InheritedProto->getDeclaredInterfaceType());
      }

      conformance->setInvalid();
      return;
    }
  }

  if (conformance->isComplete())
    return;

  // Resolve all of the type witnesses.
  evaluateOrDefault(Context.evaluator,
                    ResolveTypeWitnessesRequest{conformance},
                    evaluator::SideEffect());

  // Check the requirements from the requirement signature.
  ensureRequirementsAreSatisfied(Context, conformance);

  // Check non-type requirements.
  conformance->resolveValueWitnesses();
}

/// Add the next associated type deduction to the string representation
/// of the deductions, used in diagnostics.
static void addAssocTypeDeductionString(llvm::SmallString<128> &str,
                                        AssociatedTypeDecl *assocType,
                                        Type deduced) {
  if (str.empty())
    str = " [with ";
  else
    str += ", ";

  str += assocType->getName().str();
  str += " = ";
  str += deduced.getString();
}

/// Clean up the given declaration type for display purposes.
static Type getTypeForDisplay(ModuleDecl *module, ValueDecl *decl) {
  // For a constructor, we only care about the parameter types.
  if (auto ctor = dyn_cast<ConstructorDecl>(decl)) {
    return AnyFunctionType::composeTuple(
        module->getASTContext(),
        ctor->getMethodInterfaceType()->castTo<FunctionType>()->getParams(),
        ParameterFlagHandling::IgnoreNonEmpty);
  }

  Type type = decl->getInterfaceType();

  // Redeclaration checking might mark a candidate as `invalid` and
  // reset it's type to ErrorType, let's dig out original type to
  // make the diagnostic better.
  //
  // FIXME: Remove this once setInvalid() goes away.
  if (auto errorType = type->getAs<ErrorType>()) {
    auto originalType = errorType->getOriginalType();
    if (!originalType || !originalType->is<AnyFunctionType>())
      return type;

    type = originalType;
  }

  // If we're not in a type context, just grab the interface type.
  if (!decl->getDeclContext()->isTypeContext())
    return type;

  // For functions, strip off the 'Self' parameter clause.
  if (isa<AbstractFunctionDecl>(decl)) {
    if (auto genericFn = type->getAs<GenericFunctionType>()) {
      auto sig = genericFn->getGenericSignature();
      auto resultFn = genericFn->getResult()->castTo<FunctionType>();
      return GenericFunctionType::get(sig,
                                      resultFn->getParams(),
                                      resultFn->getResult(),
                                      resultFn->getExtInfo());
    }

    return type->castTo<FunctionType>()->getResult();
  }

  return type;
}

/// Clean up the given requirement type for display purposes.
static Type getRequirementTypeForDisplay(ModuleDecl *module,
                                         NormalProtocolConformance *conformance,
                                         ValueDecl *req) {
  auto type = getTypeForDisplay(module, req);

  auto substType = [&](Type type, bool isResult) -> Type {
    // Replace generic type parameters and associated types with their
    // witnesses, when we have them.
    auto selfTy = conformance->getProtocol()->getSelfInterfaceType();
    auto substSelfTy = conformance->getType();
    if (isResult && substSelfTy->getClassOrBoundGenericClass())
      substSelfTy = DynamicSelfType::get(selfTy, module->getASTContext());
    return type.subst([&](SubstitutableType *dependentType) {
                        if (dependentType->isEqual(selfTy))
                          return substSelfTy;

                        return Type(dependentType);
                      },
                      LookUpConformanceInModule(module));
  };

  if (auto fnTy = type->getAs<AnyFunctionType>()) {
    SmallVector<AnyFunctionType::Param, 4> params;
    for (auto param : fnTy->getParams()) {
      params.push_back(
        param.withType(
          substType(param.getPlainType(),
        /*result*/false)));
    }

    auto result = substType(fnTy->getResult(), /*result*/true);

    auto genericSig = fnTy->getOptGenericSignature();
    if (genericSig) {
      if (genericSig.getGenericParams().size() > 1) {
        genericSig = GenericSignature::get(
          genericSig.getGenericParams().slice(1),
          genericSig.getRequirements());
      } else {
        genericSig = nullptr;
      }
    }

    if (genericSig) {
      return GenericFunctionType::get(genericSig, params, result,
                                      fnTy->getExtInfo());
    }
    return FunctionType::get(params, result, fnTy->getExtInfo());
  }

  return substType(type, /*result*/ true);
}

diag::RequirementKind
swift::getProtocolRequirementKind(ValueDecl *Requirement) {
  assert(Requirement->isProtocolRequirement());

  if (isa<ConstructorDecl>(Requirement))
    return diag::RequirementKind::Constructor;
  if (isa<FuncDecl>(Requirement))
    return diag::RequirementKind::Func;
  if (isa<VarDecl>(Requirement))
    return diag::RequirementKind::Var;
  assert(isa<SubscriptDecl>(Requirement) && "Unhandled requirement kind");
  return diag::RequirementKind::Subscript;
}

SourceLoc OptionalAdjustment::getOptionalityLoc(ValueDecl *witness) const {
  // For non-parameter adjustments, use the result type or whole type,
  // as appropriate.
  if (!isParameterAdjustment()) {
    // For a function, use the result type.
    if (auto func = dyn_cast<FuncDecl>(witness)) {
      return getOptionalityLoc(
               func->getResultTypeRepr());
    }

    // For a subscript, use the element type.
    if (auto subscript = dyn_cast<SubscriptDecl>(witness)) {
      return getOptionalityLoc(subscript->getElementTypeRepr());
    }

    // Otherwise, we have a variable.
    // FIXME: Dig into the pattern.
    return SourceLoc();
  }

  // For parameter adjustments, dig out the pattern.
  ParameterList *params = nullptr;
  if (isa<AbstractFunctionDecl>(witness) || isa<SubscriptDecl>(witness)) {
    params = getParameterList(witness);
  } else {
    return SourceLoc();
  }

  return getOptionalityLoc(params->get(getParameterIndex())->getTypeRepr());
}

SourceLoc OptionalAdjustment::getOptionalityLoc(TypeRepr *tyR) const {
  if (!tyR)
    return SourceLoc();

  switch (getKind()) {
  case OptionalAdjustmentKind::None:
    llvm_unreachable("not an adjustment");

  case OptionalAdjustmentKind::ConsumesUnhandledNil:
  case OptionalAdjustmentKind::WillNeverProduceNil:
    // The location of the '?' to be inserted is after the type.
    return tyR->getEndLoc();

  case OptionalAdjustmentKind::ProducesUnhandledNil:
  case OptionalAdjustmentKind::WillNeverConsumeNil:
  case OptionalAdjustmentKind::RemoveIUO:
  case OptionalAdjustmentKind::IUOToOptional:
    // Find the location of optionality, below.
    break;
  }

  if (auto optRepr = dyn_cast<OptionalTypeRepr>(tyR))
    return optRepr->getQuestionLoc();

  if (auto iuoRepr = dyn_cast<ImplicitlyUnwrappedOptionalTypeRepr>(tyR))
    return iuoRepr->getExclamationLoc();

  return SourceLoc();
}

namespace {
/// Describes the position for optional adjustment made to a witness.
///
/// This is used by the following diagnostics:
/// 1) 'err_protocol_witness_optionality',
/// 2) 'warn_protocol_witness_optionality'
/// 3) 'protocol_witness_optionality_conflict'
enum class OptionalAdjustmentPosition : unsigned {
  /// The type of a variable.
  VarType = 0,
  /// The result type of something.
  Result = 1,
  /// The parameter type of something.
  Param = 2,
  /// The parameter types of something.
  MultipleParam = 3,
  /// Both return and parameter adjustments.
  ParamAndReturn = 4,
};
} // end anonymous namespace

/// Classify the provided optionality issues for use in diagnostics.
static OptionalAdjustmentPosition classifyOptionalityIssues(
    const SmallVectorImpl<OptionalAdjustment> &adjustments,
    ValueDecl *requirement) {
  unsigned numParameterAdjustments = 0;
  bool hasNonParameterAdjustment = false;
  for (const auto &adjustment : adjustments) {
    if (adjustment.isParameterAdjustment())
      ++numParameterAdjustments;
    else
      hasNonParameterAdjustment = true;
  }

  if (hasNonParameterAdjustment) {
    if (numParameterAdjustments > 0)
      return OptionalAdjustmentPosition::ParamAndReturn;

    if (isa<VarDecl>(requirement))
      return OptionalAdjustmentPosition::VarType;

    return OptionalAdjustmentPosition::Result;
  }

  // Only parameter adjustments.
  assert(numParameterAdjustments > 0 && "No adjustments?");
  return numParameterAdjustments == 1
             ? OptionalAdjustmentPosition::Param
             : OptionalAdjustmentPosition::MultipleParam;
}

static void addOptionalityFixIts(
    const SmallVectorImpl<OptionalAdjustment> &adjustments,
    const ASTContext &ctx,
    ValueDecl *witness, 
    InFlightDiagnostic &diag) {
  for (const auto &adjustment : adjustments) {
    SourceLoc adjustmentLoc = adjustment.getOptionalityLoc(witness);
    if (adjustmentLoc.isInvalid())
      continue;

    switch (adjustment.getKind()) {
    case OptionalAdjustmentKind::None:
      llvm_unreachable("not an optional adjustment");

    case OptionalAdjustmentKind::ProducesUnhandledNil:
    case OptionalAdjustmentKind::WillNeverConsumeNil:
    case OptionalAdjustmentKind::RemoveIUO:
      diag.fixItRemove(adjustmentLoc);
      break;

    case OptionalAdjustmentKind::WillNeverProduceNil:
    case OptionalAdjustmentKind::ConsumesUnhandledNil:
      diag.fixItInsertAfter(adjustmentLoc, "?");
      break;

    case OptionalAdjustmentKind::IUOToOptional:
      diag.fixItReplace(adjustmentLoc, "?");
      break;
    }
  }

}

/// Diagnose a requirement match, describing what went wrong (or not).
static void
diagnoseMatch(ModuleDecl *module, NormalProtocolConformance *conformance,
              ValueDecl *req, const RequirementMatch &match) {

  // If the name doesn't match and that's not the only problem,
  // it is likely this witness wasn't intended to be a match at all, so omit
  // diagnosis.
  if (match.Kind != MatchKind::RenamedMatch &&
      !match.Witness->getAttrs().hasAttribute<ImplementsAttr>() &&
      match.Witness->getName() &&
      req->getName() != match.Witness->getName() &&
      !isa<EnumElementDecl>(match.Witness))
    return;

  // Form a string describing the associated type deductions.
  // FIXME: Determine which associated types matter, and only print those.
  llvm::SmallString<128> withAssocTypes;
  for (auto assocType : conformance->getProtocol()->getAssociatedTypeMembers()) {
    if (conformance->usesDefaultDefinition(assocType)) {
      Type witness = conformance->getTypeWitness(assocType);
      addAssocTypeDeductionString(withAssocTypes, assocType, witness);
    }
  }
  if (!withAssocTypes.empty())
    withAssocTypes += "]";

  auto &diags = module->getASTContext().Diags;
  switch (match.Kind) {
  case MatchKind::ExactMatch:
  case MatchKind::FewerEffects:
    diags.diagnose(match.Witness, diag::protocol_witness_exact_match,
                   withAssocTypes);
    break;

  case MatchKind::RequiresNonSendable:
    diags.diagnose(match.Witness, diag::protocol_witness_non_sendable,
                   withAssocTypes,
                   module->getASTContext().isSwiftVersionAtLeast(6));
    break;

  case MatchKind::RenamedMatch: {
    auto diag = diags.diagnose(match.Witness, diag::protocol_witness_renamed,
                               req->getName(), withAssocTypes);

    // Fix the name.
    fixDeclarationName(diag, match.Witness, req->getName());

    // Also fix the Objective-C name, if needed.
    if (!match.Witness->canInferObjCFromRequirement(req))
      fixDeclarationObjCName(diag, match.Witness,
                             match.Witness->getObjCRuntimeName(),
                             req->getObjCRuntimeName());
    break;
  }

  case MatchKind::KindConflict:
    diags.diagnose(match.Witness, diag::protocol_witness_kind_conflict,
                   getProtocolRequirementKind(req));
    break;

  case MatchKind::WitnessInvalid:
    // Don't bother to diagnose invalid witnesses; we've already complained
    // about them.
    break;

  case MatchKind::Circularity:
    diags.diagnose(match.Witness, diag::protocol_witness_circularity);
    break;

  case MatchKind::TypeConflict: {
    auto witnessType = getTypeForDisplay(module, match.Witness);

    if (!isa<TypeDecl>(req) && !isa<EnumElementDecl>(match.Witness)) {
      computeFixitsForOverriddenDeclaration(match.Witness, req, [&](bool){
        return diags.diagnose(match.Witness,
                              diag::protocol_witness_type_conflict,
                              witnessType, withAssocTypes);
      });
    } else {
      diags.diagnose(match.Witness,
                     diag::protocol_witness_type_conflict,
                     witnessType, withAssocTypes);
    }
    break;
  }

  case MatchKind::MissingRequirement:
    diags.diagnose(match.Witness, diag::protocol_witness_missing_requirement,
                   match.WitnessType, match.MissingRequirement->getSecondType(),
                   (unsigned)match.MissingRequirement->getKind());
    break;

  case MatchKind::AsyncConflict:
    diags.diagnose(match.Witness, diag::protocol_witness_async_conflict,
                   cast<AbstractFunctionDecl>(match.Witness)->hasAsync(),
                   req->isObjC());
    break;

  case MatchKind::ThrowsConflict:
    diags.diagnose(match.Witness, diag::protocol_witness_throws_conflict);
    break;

  case MatchKind::OptionalityConflict: {
    auto &adjustments = match.OptionalAdjustments;
    auto issues =
        static_cast<unsigned>(classifyOptionalityIssues(adjustments, req));
    auto diag = diags.diagnose(match.Witness,
                               diag::protocol_witness_optionality_conflict,
                               issues, withAssocTypes);
    addOptionalityFixIts(adjustments,
                         match.Witness->getASTContext(),
                         match.Witness,
                         diag);
    break;
  }
  case MatchKind::CompileTimeConstConflict: {
    auto witness = match.Witness;
    auto missing = !witness->getAttrs().getAttribute<CompileTimeConstAttr>();
    auto diag = diags.diagnose(witness, diag::protocol_witness_const_conflict,
                   missing);
    if (missing) {
      diag.fixItInsert(witness->getAttributeInsertionLoc(true), "_const");
    }
    break;
  }
  case MatchKind::StaticNonStaticConflict: {
    auto witness = match.Witness;
    auto diag = diags.diagnose(witness, diag::protocol_witness_static_conflict,
                               !req->isInstanceMember());
    if (isa<EnumElementDecl>(witness))
      break;
    if (req->isInstanceMember()) {
      SourceLoc loc;
      if (auto FD = dyn_cast<FuncDecl>(witness)) {
        loc = FD->getStaticLoc();
      } else if (auto VD = dyn_cast<VarDecl>(witness)) {
        if (auto PBD = VD->getParentPatternBinding()) {
          loc = PBD->getStaticLoc();
        }
      } else if (auto SD = dyn_cast<SubscriptDecl>(witness)) {
        loc = SD->getStaticLoc();
      } else {
        llvm_unreachable("Unexpected witness");
      }
      if (loc.isValid())
        diag.fixItRemove(loc);
    } else {
      diag.fixItInsert(witness->getAttributeInsertionLoc(true), "static ");
    }
    break;
  }

  case MatchKind::SettableConflict: {
    auto witness = match.Witness;
    auto diag =
        diags.diagnose(witness, diag::protocol_witness_settable_conflict);
    if (auto VD = dyn_cast<VarDecl>(witness)) {
      if (VD->hasStorage()) {
        if (auto PBD = VD->getParentPatternBinding()) {
          diag.fixItReplace(PBD->getStartLoc(), getTokenText(tok::kw_var));
        }
      }
    }
    break;
  }

  case MatchKind::PrefixNonPrefixConflict: {
    auto witness = match.Witness;
    auto diag = diags.diagnose(
        witness, diag::protocol_witness_prefix_postfix_conflict, false,
        witness->getAttrs().hasAttribute<PostfixAttr>() ? 2 : 0);
    // We already emit a fix-it when we're missing the attribute, so only
    // emit a fix-it if the attribute is there, but is not correct.
    if (auto attr = witness->getAttrs().getAttribute<PostfixAttr>()) {
      diag.fixItReplace(attr->getLocation(), "prefix");
    }
    break;
  }

  case MatchKind::PostfixNonPostfixConflict: {
    auto witness = match.Witness;
    auto diag = diags.diagnose(
        witness, diag::protocol_witness_prefix_postfix_conflict, true,
        witness->getAttrs().hasAttribute<PrefixAttr>() ? 1 : 0);
    // We already emit a fix-it when we're missing the attribute, so only
    // emit a fix-it if the attribute is there, but is not correct.
    if (auto attr = witness->getAttrs().getAttribute<PrefixAttr>()) {
      diag.fixItReplace(attr->getLocation(), "postfix");
    }
    break;
  }
  case MatchKind::MutatingConflict:
    diags.diagnose(match.Witness,
                   diag::protocol_witness_mutation_modifier_conflict,
                   SelfAccessKind::Mutating);
    break;
  case MatchKind::NonMutatingConflict:
    // Don't bother about this, because a non-mutating witness can satisfy
    // a mutating requirement.
    break;
  case MatchKind::ConsumingConflict:
    diags.diagnose(match.Witness,
                   diag::protocol_witness_mutation_modifier_conflict,
                   SelfAccessKind::Consuming);
    break;
  case MatchKind::RethrowsConflict: {
    auto witness = match.Witness;
    auto diag =
        diags.diagnose(witness, diag::protocol_witness_rethrows_conflict);
    auto FD = cast<FuncDecl>(witness);
    diag.fixItReplace(FD->getThrowsLoc(), getTokenText(tok::kw_rethrows));
    break;
  }
  case MatchKind::RethrowsByConformanceConflict: {
    auto witness = match.Witness;
    auto diag =
        diags.diagnose(witness,
                       diag::protocol_witness_rethrows_by_conformance_conflict);
    break;
  }
  case MatchKind::NonObjC:
    diags.diagnose(match.Witness, diag::protocol_witness_not_objc);
    break;
  case MatchKind::MissingDifferentiableAttr: {
    auto *witness = match.Witness;
    // Emit a note and fix-it showing the missing requirement `@differentiable`
    // attribute.
    auto *reqAttr = cast<DifferentiableAttr>(match.UnmetAttribute);
    assert(reqAttr);
    // Omit printing `wrt:` clause if attribute's differentiability
    // parameters match inferred differentiability parameters.
    auto *original = cast<AbstractFunctionDecl>(witness);
    auto *whereClauseGenEnv =
        reqAttr->getDerivativeGenericEnvironment(original);
    auto *inferredParameters = TypeChecker::inferDifferentiabilityParameters(
        original, whereClauseGenEnv);
    bool omitWrtClause = reqAttr->getParameterIndices()->getNumIndices() ==
                         inferredParameters->getNumIndices();
    std::string reqDiffAttrString;
    llvm::raw_string_ostream os(reqDiffAttrString);
    reqAttr->print(os, req, omitWrtClause);
    os.flush();
    diags
        .diagnose(
            witness,
            diag::protocol_witness_missing_differentiable_attr_invalid_context,
            reqDiffAttrString, req, conformance->getType(),
            conformance->getProtocol()->getDeclaredInterfaceType())
        .fixItInsert(match.Witness->getStartLoc(), reqDiffAttrString + ' ');
    break;
  }
  case MatchKind::EnumCaseWithAssociatedValues:
    diags.diagnose(match.Witness, diag::protocol_witness_enum_case_payload);
    break;
  }
}

ConformanceChecker::ConformanceChecker(
    ASTContext &ctx, NormalProtocolConformance *conformance)
    : WitnessChecker(ctx, conformance->getProtocol(), conformance->getType(),
                     conformance->getDeclContext()),
      Conformance(conformance), Loc(conformance->getLoc()) {}

ConformanceChecker::~ConformanceChecker() {}

void ConformanceChecker::recordWitness(ValueDecl *requirement,
                                       const RequirementMatch &match) {
  // If we already recorded this witness, don't do so again.
  if (Conformance->hasWitness(requirement)) {
    assert(Conformance->getWitnessUncached(requirement).getDecl() ==
               match.Witness &&
           "Deduced different witnesses?");
    return;
  }

  // Record this witness in the conformance.
  auto witness = match.getWitness(getASTContext());
  Conformance->setWitness(requirement, witness);
}

void ConformanceChecker::recordOptionalWitness(ValueDecl *requirement) {
  // If we already recorded this witness, don't do so again.
  if (Conformance->hasWitness(requirement)) {
    assert(!Conformance->getWitnessUncached(requirement).getDecl() &&
           "Already have a non-optional witness?");
    return;
  }

  // Record that there is no witness.
  Conformance->setWitness(requirement, Witness());
}

void ConformanceChecker::recordInvalidWitness(ValueDecl *requirement) {
  assert(Conformance->isInvalid());

  // If we already recorded this witness, don't do so again.
  if (Conformance->hasWitness(requirement)) {
    assert(!Conformance->getWitnessUncached(requirement).getDecl() &&
           "Already have a non-optional witness?");
    return;
  }

  // Record that there is no witness.
  Conformance->setWitness(requirement, Witness());
}

/// Returns the location we should use for a primary diagnostic (an error or
/// warning) that concerns \p witness but arose as part of checking
/// \p conformance.
///
/// Ideally we'd like to use the location of \p witness for this, but that
/// could be confusing if the conformance is declared somewhere else. Moreover,
/// if the witness and the conformance declaration are in different files, we
/// could be issuing diagnostics in one file that wouldn't be present if we
/// recompiled just that file. Therefore, we only use the witness's location if
/// it's in the same type or extension that declares the conformance.
static SourceLoc
getLocForDiagnosingWitness(const NormalProtocolConformance *conformance,
                           const ValueDecl *witness) {
  if (witness && witness->getDeclContext() == conformance->getDeclContext()) {
    SourceLoc witnessLoc = witness->getLoc();
    if (witnessLoc.isValid())
      return witnessLoc;
  }
  return conformance->getLoc();
}

/// Emits a "'foo' declared here" note unless \p mainDiagLoc is already the
/// location of \p value.
static void emitDeclaredHereIfNeeded(DiagnosticEngine &diags,
                                     SourceLoc mainDiagLoc,
                                     const ValueDecl *value) {
  if (!value)
    return;
  if (mainDiagLoc == value->getLoc())
    return;
  diags.diagnose(value, diag::decl_declared_here, value);
}

/// Whether this declaration has the 'distributed' modifier on it.
static bool isDistributedDecl(ValueDecl *decl) {
  if (auto func = dyn_cast<AbstractFunctionDecl>(decl))
    return func->isDistributed();
  if (auto var = dyn_cast<VarDecl>(decl))
    return var->isDistributed();
  return false;
}

/// Determine whether there was an explicit global actor attribute on the
/// given declaration.
static bool hasExplicitGlobalActorAttr(ValueDecl *decl) {
  auto globalActorAttr = decl->getGlobalActorAttr();
  if (!globalActorAttr)
    return false;

  return !globalActorAttr->first->isImplicit();
}

std::optional<ActorIsolation>
ConformanceChecker::checkActorIsolation(ValueDecl *requirement,
                                        ValueDecl *witness) {

  // Determine the isolation of the requirement itself.
  auto requirementIsolation = getActorIsolation(requirement);
  if (requirementIsolation.requiresSubstitution()) {
    auto substitutingType = DC->mapTypeIntoContext(Conformance->getType());
    auto subs = SubstitutionMap::getProtocolSubstitutions(
        Proto, substitutingType, ProtocolConformanceRef(Conformance));
    requirementIsolation = requirementIsolation.subst(subs);
  }

  SourceLoc loc = witness->getLoc();
  if (loc.isInvalid())
    loc = Conformance->getLoc();

  auto refResult = ActorReferenceResult::forReference(
      getDeclRefInContext(witness), witness->getLoc(), DC, std::nullopt,
      std::nullopt, std::nullopt, requirementIsolation);
  bool sameConcurrencyDomain = false;
  // If the requirement is isolated (explicitly or implicitly) or
  // explicitly marked as `nonisolated` it means that the protocol
  // has adopted concurrency and `@preconcurrency` doesn't apply.
  bool isPreconcurrency =
      Conformance->isPreconcurrency() &&
      !(requirementIsolation.isActorIsolated() ||
        requirement->getAttrs().hasAttribute<NonisolatedAttr>());

  switch (refResult) {
  case ActorReferenceResult::SameConcurrencyDomain:
    // If the witness has distributed-actor isolation, we have extra
    // checking to do.
    if (refResult.isolation.isDistributedActor()) {
      sameConcurrencyDomain = true;
      break;
    }

    // Witnessing `async` requirement with an isolated synchronous
    // declaration is done via async witness thunk which requires
    // a hop to the expected concurrency domain.
    if (isAsyncDecl(requirement) && !isAsyncDecl(witness))
      return refResult.isolation;

    // Otherwise, we're done.
    return std::nullopt;

  case ActorReferenceResult::ExitsActorToNonisolated:
    diagnoseNonSendableTypesInReference(
        /*base=*/nullptr, getDeclRefInContext(witness),
        DC, loc, SendableCheckReason::Conformance);
    return std::nullopt;
  case ActorReferenceResult::EntersActor:
    // Handled below.
    break;
  }

  // Keep track of what modifiers are missing from the requirement and witness,
  // so we can decide what to diagnose.
  enum class MissingFlags {
    RequirementAsync = 1 << 0,
    RequirementThrows = 1 << 1,
    WitnessDistributed = 1 << 2,
  };
  OptionSet<MissingFlags> missingOptions;

  // If the witness is accessible across actors and the requirement is not
  // async, we need an async requirement.
  // FIXME: feels like ActorReferenceResult should be reporting this back to
  // us somehow.
  // To enter the actor, we always need the requirement to be `async`.
  if (!sameConcurrencyDomain &&
      !isAsyncDecl(requirement) &&
      !isAccessibleAcrossActors(witness, refResult.isolation, DC))
    missingOptions |= MissingFlags::RequirementAsync;

  // If we are entering a distributed actor, the witness must be 'distributed'
  // and we need the requirement to be 'throws'.
  bool isDistributed = refResult.isolation.isDistributedActor() &&
      !witness->getAttrs().hasAttribute<NonisolatedAttr>();
  if (isDistributed) {
    // Check if the protocol where the requirement originates from
    // is a distributed actor constrained one.
    if (cast<ProtocolDecl>(requirement->getDeclContext())->isDistributedActor()) {
      // The requirement was declared in a DistributedActor constrained proto.
      //
      // This means casting up to this `P` won't "strip off" the
      // "distributed-ness" of the type, and all call-sites will be checking
      // distributed isolation.
      //
      // This means that we can actually allow these specific requirements,
      // to be witnessed without the distributed keyword (!), but they won't be
      // possible to be called unless:
      // - from inside the distributed actor (self),
      // - on a known-to-be-local distributed actor reference.
      //
      // This allows us to implement protocols where a local distributed actor
      // registers "call me when something happens", and that call can be
      // expressed as non-distributed function which we are guaranteed to be
      // able to call, since the whenLocal will give us access to this actor as
      // known-to-be-local, so we can invoke this method.

      // If the requirement is distributed, we still need to require it on the witness though.
      // We DO allow a non-distributed requirement to be witnessed here though!
      if (isDistributedDecl(requirement) && !isDistributedDecl(witness))
        missingOptions |= MissingFlags::WitnessDistributed;
    } else {
      // The protocol requirement comes from a normal (non-distributed actor)
      // protocol; so the only witnesses allowed are such that we can witness
      // them using a distributed, or nonisolated functions.

      // If we're coming from a non-distributed requirement,
      // then the requirement must be 'throws' to accommodate failure.
      if (!isThrowsDecl(requirement))
        missingOptions |= MissingFlags::RequirementThrows;

      // If the witness is distributed, it is able to witness a requirement
      // only if the requirement is `async throws`.
      if (!isDistributedDecl(witness) && !missingOptions)
        missingOptions |= MissingFlags::WitnessDistributed;
    }
  }

  // If we aren't missing anything or this is a witness to a `@preconcurrency`
  // conformance, do a Sendable check and move on.
  if (!missingOptions || isPreconcurrency) {
    // FIXME: Disable Sendable checking when the witness is an initializer
    // that is explicitly marked nonisolated.
    if (isa<ConstructorDecl>(witness) &&
        witness->getAttrs().hasAttribute<NonisolatedAttr>())
      return std::nullopt;

    // Check that the results of the witnessing method are sendable
    diagnoseNonSendableTypesInReference(
        /*base=*/nullptr, getDeclRefInContext(witness),
        DC, loc, SendableCheckReason::Conformance,
        getActorIsolation(witness), FunctionCheckKind::Results);

    // If this requirement is a function, check that its parameters are Sendable as well
    if (isa<AbstractFunctionDecl>(requirement)) {
      diagnoseNonSendableTypesInReference(
          /*base=*/nullptr, getDeclRefInContext(requirement),
          requirement->getInnermostDeclContext(), requirement->getLoc(),
          SendableCheckReason::Conformance, getActorIsolation(witness),
          FunctionCheckKind::Params, loc);
    }

    // If the witness is accessible across actors, we don't need to consider it
    // isolated.
    if (isAccessibleAcrossActors(witness, refResult.isolation, DC))
      return std::nullopt;

    if (refResult.isolation.isActorIsolated()) {
      if (isAsyncDecl(requirement) && !isAsyncDecl(witness))
        return refResult.isolation;

      // Always treat `@preconcurrency` witnesses as isolated.
      if (isPreconcurrency &&
          missingOptions.contains(MissingFlags::RequirementAsync))
        return refResult.isolation;
    }

    return std::nullopt;
  }

  // Limit the behavior of the diagnostic based on context.
  // If we're working with requirements imported from Clang, or with global
  // actor isolation in general, use the default diagnostic behavior based
  // on the conformance context.
  DiagnosticBehavior behavior = DiagnosticBehavior::Unspecified;
  if (requirement->hasClangNode() ||
      refResult.isolation.isGlobalActor() ||
      requirementIsolation.isGlobalActor()) {
    // If the witness or requirement has global actor isolation, downgrade
    // based on context. Use the witness itself as the context, because
    // an explicitly isolated witness should not suppress diagnostics.
    behavior = SendableCheckContext(
        witness->getInnermostDeclContext()).defaultDiagnosticBehavior();
  }

  // If the witness is a non-Sendable 'let', compiler versions <= 5.10
  // didn't diagnose this code, so downgrade the error to an warning
  // until Swift 6.
  if (auto *var = dyn_cast<VarDecl>(witness)) {
    ActorReferenceResult::Options options = std::nullopt;
    isLetAccessibleAnywhere(
        witness->getDeclContext()->getParentModule(),
        var, options);
    if (options.contains(ActorReferenceResult::Flags::Preconcurrency)) {
      behavior = DiagnosticBehavior::Warning;
    }
  }

  // Complain that this witness cannot conform to the requirement due to
  // actor isolation.
  witness->diagnose(diag::actor_isolated_witness,
                    isDistributed && !isDistributedDecl(witness),
                    refResult.isolation, witness, requirementIsolation)
    .limitBehaviorUntilSwiftVersion(behavior, 6);

  // If we need 'distributed' on the witness, add it.
  if (missingOptions.contains(MissingFlags::WitnessDistributed)) {
    witness->diagnose(
        diag::note_add_distributed_to_decl, witness)
      .fixItInsert(witness->getAttributeInsertionLoc(true),
                              "distributed ");
    missingOptions -= MissingFlags::WitnessDistributed;
  }

  // One way to address the issue is to make the witness function nonisolated.
  if ((isa<AbstractFunctionDecl>(witness) || isa<SubscriptDecl>(witness)) &&
      !hasExplicitGlobalActorAttr(witness) &&
      !isDistributedDecl(requirement) &&
      !isDistributedDecl(witness)) {
    witness->diagnose(diag::note_add_nonisolated_to_decl, witness)
          .fixItInsert(witness->getAttributeInsertionLoc(true), "nonisolated ");
  }

  // If there are remaining options, they are missing async/throws on the
  // requirement itself. If we have a source location for the requirement,
  // provide those in a note.
  if (missingOptions && requirement->getLoc().isValid() &&
      isa<AbstractFunctionDecl>(requirement)) {
    int suggestAddingModifiers = 0;
    std::string modifiers;
    if (missingOptions.contains(MissingFlags::RequirementAsync)) {
      suggestAddingModifiers += 1;
      modifiers += "async ";
    }

    if (missingOptions.contains(MissingFlags::RequirementThrows)) {
      suggestAddingModifiers += 2;
      modifiers += "throws ";
    }

    auto diag = requirement->diagnose(diag::note_add_async_and_throws_to_decl,
                                      witness, suggestAddingModifiers);

    // Figure out where to insert the modifiers so we can emit a Fix-It.
    SourceLoc insertLoc;
    bool insertAfter = false;
    if (auto requirementAbstractFunc =
            dyn_cast<AbstractFunctionDecl>(requirement)) {
      if (requirementAbstractFunc->getAsyncLoc().isValid()) {
        // Insert before the "async" (we only have throws).
        insertLoc = requirementAbstractFunc->getAsyncLoc();
        insertAfter = true;
        modifiers = " throws";
      } else if (requirementAbstractFunc->getThrowsLoc().isValid()) {
        // Insert before the "throws" (we only have async)".
        insertLoc = requirementAbstractFunc->getThrowsLoc();
      }

      // Insert after the parentheses.
      if (insertLoc.isInvalid()) {
        insertLoc = requirementAbstractFunc->getParameters()->getRParenLoc();
        insertAfter = true;
        modifiers = " " + modifiers.substr(0, modifiers.size() - 1);
      }
    }

    if (insertLoc.isValid()) {
      if (insertAfter)
        diag.fixItInsertAfter(insertLoc, modifiers);
      else
        diag.fixItInsert(insertLoc, modifiers);
    }
  } else {
    requirement->diagnose(diag::decl_declared_here, requirement);
  }

  return std::nullopt;
}

/// Check for ill-formed uses of Objective-C generics in a type witness.
static bool checkObjCTypeErasedGenerics(NormalProtocolConformance *conformance,
                                        AssociatedTypeDecl *assocType,
                                        Type type, TypeDecl *typeDecl) {
  auto *dc = conformance->getDeclContext();
  auto *proto = conformance->getProtocol();

  // Objective-C's type-erased generics don't allow the type arguments
  // to be extracted from an instance (or a metatype), so we cannot refer to
  // the type parameters from an associated type. Check that here.
  auto &ctx = dc->getASTContext();
  if (!ctx.LangOpts.EnableObjCInterop && type->hasError())
    return false;

  auto classDecl = dc->getSelfClassDecl();
  if (!classDecl) return false;

  if (!classDecl->isTypeErasedGenericClass()) return false;

  // Concrete types are okay.
  if (!type->getCanonicalType()->hasTypeParameter()) return false;

  // Find one of the generic parameters named. It doesn't matter
  // which one.
  Type genericParam;
  (void)type.findIf([&](Type type) {
    if (auto gp = type->getAs<GenericTypeParamType>()) {
      genericParam = gp;
      return true;
    }

    return false;
  });

  // Diagnose the problem.
  SourceLoc diagLoc = getLocForDiagnosingWitness(conformance, typeDecl);
  ctx.Diags.diagnose(diagLoc, diag::type_witness_objc_generic_parameter,
                     type, genericParam, !genericParam.isNull(), assocType,
                     proto);
  emitDeclaredHereIfNeeded(ctx.Diags, diagLoc, typeDecl);

  return true;
}

namespace {
/// Helper class for use with ConformanceChecker::diagnoseOrDefer when a witness
/// needs to be marked as '\@usableFromInline'.
class DiagnoseUsableFromInline {
  const ValueDecl *witness;

public:
  explicit DiagnoseUsableFromInline(const ValueDecl *witness)
      : witness(witness) {
    assert(witness);
  }

  void operator()(const NormalProtocolConformance *conformance) {
    auto proto = conformance->getProtocol();
    ASTContext &ctx = proto->getASTContext();

    SourceLoc diagLoc = getLocForDiagnosingWitness(conformance, witness);
    ctx.Diags.diagnose(diagLoc, diag::witness_not_usable_from_inline, witness,
                       proto)
      .warnUntilSwiftVersion(5);
    emitDeclaredHereIfNeeded(ctx.Diags, diagLoc, witness);
  }
};
}

/// Helper function for diagnostics when a witness needs to be seated at a
/// required access level.
static void diagnoseWitnessFixAccessLevel(DiagnosticEngine &diags,
                                          ValueDecl *decl,
                                          AccessLevel requiredAccess,
                                          bool isForSetter = false) {
  bool shouldMoveToAnotherExtension = false;
  bool shouldUseDefaultAccess = false;
  if (auto extDecl = dyn_cast<ExtensionDecl>(decl->getDeclContext())) {
    if (auto attr = extDecl->getAttrs().getAttribute<AccessControlAttr>()) {
      auto extAccess = std::max(attr->getAccess(), AccessLevel::FilePrivate);
      if (extAccess < requiredAccess) {
        shouldMoveToAnotherExtension = true;
      } else if (extAccess == requiredAccess) {
        assert(decl->getFormalAccess() < requiredAccess &&
              "witness is accessible?");
        shouldUseDefaultAccess = true;
      }
    }
  }

  // If decl lives in an extension that forbids the required level, we should
  // move it to another extension where the required level is possible;
  // otherwise, we simply mark decl as the required level.
  if (shouldMoveToAnotherExtension) {
    diags.diagnose(decl, diag::witness_move_to_another_extension,
                   decl->getDescriptiveKind(), requiredAccess);
  } else {
    auto fixItDiag = diags.diagnose(decl, diag::witness_fix_access,
                                    decl->getDescriptiveKind(),
                                    requiredAccess);
    fixItAccess(fixItDiag, decl, requiredAccess, isForSetter,
                shouldUseDefaultAccess);
  }
}

/// Whether this protocol is the Objective-C "NSObject" protocol.
static bool isNSObjectProtocol(ProtocolDecl *proto) {
  if (proto->getNameStr() != "NSObjectProtocol")
    return false;

  return proto->hasClangNode();
}

static Type getTupleConformanceTypeWitness(DeclContext *dc,
                                           AssociatedTypeDecl *assocType) {
  auto genericSig = dc->getGenericSignatureOfContext();
  assert(genericSig.getGenericParams().size() == 1);

  auto paramTy = genericSig.getGenericParams()[0];
  auto elementTy = DependentMemberType::get(paramTy, assocType);
  auto expansionTy = PackExpansionType::get(elementTy, paramTy);

  return TupleType::get(TupleTypeElt(expansionTy), dc->getASTContext());
}

bool swift::
printRequirementStub(ValueDecl *Requirement, DeclContext *Adopter,
                     Type AdopterTy, SourceLoc TypeLoc, raw_ostream &OS) {
  if (isa<ConstructorDecl>(Requirement)) {
    if (auto CD = Adopter->getSelfClassDecl()) {
      if (!CD->isSemanticallyFinal() && isa<ExtensionDecl>(Adopter)) {
        // In this case, user should mark class as 'final' or define
        // 'required' initializer directly in the class definition.
        return false;
      }
    }
  }
  if (auto MissingTypeWitness = dyn_cast<AssociatedTypeDecl>(Requirement)) {
    if (MissingTypeWitness->hasDefaultDefinitionType()) {
      // For type witnesses with default definitions, we don't print the stub.
      return false;
    }
  }
  // FIXME: Infer body indentation from the source rather than hard-coding
  // 4 spaces.
  ASTContext &Ctx = Requirement->getASTContext();
  StringRef ExtraIndent;
  StringRef CurrentIndent =
      Lexer::getIndentationForLine(Ctx.SourceMgr, TypeLoc, &ExtraIndent);
  std::string StubIndent = (CurrentIndent + ExtraIndent).str();

  ExtraIndentStreamPrinter Printer(OS, StubIndent);
  Printer.printNewline();

  AccessLevel Access =
    std::min(
      /* Access of the context */
      Adopter->getSelfNominalTypeDecl()->getFormalAccess(),
      /* Access of the protocol */
      Requirement->getDeclContext()->getSelfProtocolDecl()->
        getFormalAccess());
  if (Access == AccessLevel::Public)
    Printer << "public ";

  if (auto MissingTypeWitness = dyn_cast<AssociatedTypeDecl>(Requirement)) {
    Printer << "typealias " << MissingTypeWitness->getName() << " = ";

    if (isa<BuiltinTupleDecl>(Adopter->getSelfNominalTypeDecl())) {
      auto expectedTy = getTupleConformanceTypeWitness(Adopter, MissingTypeWitness);
      Printer << expectedTy.getString();
    } else {
      Printer << "<#type#>";
    }

    Printer << "\n";
  } else {
    if (isa<ConstructorDecl>(Requirement)) {
      if (auto CD = Adopter->getSelfClassDecl()) {
        if (!CD->isFinal()) {
          Printer << "required ";
        } else if (isa<ExtensionDecl>(Adopter)) {
          Printer << "convenience ";
        }
      }
    }

    PrintOptions Options = PrintOptions::printForDiagnostics(
        AccessLevel::Private, Ctx.TypeCheckerOpts.PrintFullConvention);
    Options.PrintDocumentationComments = false;
    Options.PrintAccess = false;
    Options.SkipAttributes = true;
    Options.FunctionDefinitions = true;
    Options.PrintAccessorBodiesInProtocols = true;
    Options.FullyQualifiedTypesIfAmbiguous = true;

    bool AdopterIsClass = Adopter->getSelfClassDecl() != nullptr;
    // Skip 'mutating' only inside classes: mutating methods usually
    // don't have a sensible non-mutating implementation.
    if (AdopterIsClass)
      Options.ExcludeAttrList.push_back(DeclAttrKind::Mutating);
    // 'nonmutating' is only meaningful on value type member accessors.
    if (AdopterIsClass || !isa<AbstractStorageDecl>(Requirement))
      Options.ExcludeAttrList.push_back(DeclAttrKind::NonMutating);

    // FIXME: Once we support move-only types in generics, remove this if the
    //        conforming type is move-only. Until then, don't suggest printing
    //        ownership modifiers on a protocol requirement.
    Options.ExcludeAttrList.push_back(DeclAttrKind::LegacyConsuming);
    Options.ExcludeAttrList.push_back(DeclAttrKind::Consuming);
    Options.ExcludeAttrList.push_back(DeclAttrKind::Borrowing);

    Options.FunctionBody = [&](const ValueDecl *VD, ASTPrinter &Printer) {
      Printer << " {";
      Printer.printNewline();
      Printer << ExtraIndent << getCodePlaceholder();
      Printer.printNewline();
      Printer << "}";
    };
    Options.setBaseType(AdopterTy);
    Options.CurrentModule = Adopter->getParentModule();
    if (isa<NominalTypeDecl>(Adopter)) {
      // Create a variable declaration instead of a computed property in
      // nominal types...
      Options.PrintPropertyAccessors = false;

      // ...but a non-mutating setter requirement will force us into a
      // computed property in non-class adopters; don't leave the user
      // wondering why a conformance fails.
      if (!AdopterIsClass)
        if (const auto VD = dyn_cast<VarDecl>(Requirement))
          if (const auto Set = VD->getOpaqueAccessor(AccessorKind::Set))
            if (Set->getAttrs().hasAttribute<NonMutatingAttr>())
              Options.PrintPropertyAccessors = true;
    }
    Requirement->print(Printer, Options);
    Printer << "\n";
  }
  return true;
}

/// Print the stubs for an array of witnesses, either type or value, to
/// FixitString. If for a witness we cannot have stub printed, insert it to
/// NoStubRequirements.
static void
printProtocolStubFixitString(SourceLoc TypeLoc, ProtocolConformance *Conf,
                             ArrayRef<ASTContext::MissingWitness> MissingWitnesses,
                             std::string &FixitString,
                             llvm::SetVector<ValueDecl*> &NoStubRequirements) {
  llvm::raw_string_ostream FixitStream(FixitString);
  std::for_each(MissingWitnesses.begin(), MissingWitnesses.end(),
    [&](const ASTContext::MissingWitness &Missing) {
      if (!printRequirementStub(
              Missing.requirement, Conf->getDeclContext(), Conf->getType(),
              TypeLoc, FixitStream)) {
        NoStubRequirements.insert(Missing.requirement);
      }
    });
}

/// Filter the given array of protocol requirements and produce a new vector
/// containing the non-conflicting requirements to be implemented by the given
/// \c Adoptee type.
static llvm::SmallVector<ASTContext::MissingWitness, 4>
filterProtocolRequirements(
    ArrayRef<ASTContext::MissingWitness> MissingWitnesses, Type Adoptee) {
  llvm::SmallVector<ASTContext::MissingWitness, 4> Filtered;
  if (MissingWitnesses.empty()) {
    return Filtered;
  }

  const auto getProtocolSubstitutionMap = [&](ValueDecl *Req) {
    auto *const PD = cast<ProtocolDecl>(Req->getDeclContext());
    return SubstitutionMap::getProtocolSubstitutions(
        PD, Adoptee, ProtocolConformanceRef(PD));
  };

  llvm::SmallDenseMap<DeclName, llvm::SmallVector<ValueDecl *, 2>, 4>
      DeclsByName;
  for (const auto &Missing: MissingWitnesses) {
    auto Req = Missing.requirement;
    if (DeclsByName.find(Req->getName()) == DeclsByName.end()) {
      DeclsByName[Req->getName()] = {Req};
      Filtered.push_back(Missing);
      continue;
    }

    auto OverloadTy = Req->getOverloadSignatureType();
    if (OverloadTy) {
      OverloadTy =
          OverloadTy.subst(getProtocolSubstitutionMap(Req))->getCanonicalType();
    }
    if (llvm::any_of(DeclsByName[Req->getName()], [&](ValueDecl *OtherReq) {
          auto OtherOverloadTy = OtherReq->getOverloadSignatureType();
          if (OtherOverloadTy) {
            OtherOverloadTy =
                OtherOverloadTy.subst(getProtocolSubstitutionMap(OtherReq))
                    ->getCanonicalType();
          }
          return conflicting(Req->getASTContext(), Req->getOverloadSignature(),
                             OverloadTy, OtherReq->getOverloadSignature(),
                             OtherOverloadTy,
                             /*wouldConflictInSwift5*/ nullptr,
                             /*skipProtocolExtensionCheck*/ true);
        })) {
      continue;
    }

    DeclsByName[Req->getName()].push_back(Req);
    Filtered.push_back(Missing);
  }

  return Filtered;
}

/// Sometimes a witness isn't really diagnosed as missing if we have two
/// complementary Objective-C protocol requirements, only one of which must
/// be witnessed.
static bool
hasSatisfiedObjCSiblingRequirement(ProtocolDecl *proto,
                                   NormalProtocolConformance *conformance,
                                   ValueDecl *requirement) {
  assert(proto == requirement->getDeclContext());
  assert(proto == conformance->getProtocol());

  // We only care about functions.
  auto fnRequirement = dyn_cast<AbstractFunctionDecl>(requirement);
  if (fnRequirement == nullptr)
    return false;

  if (!proto->isObjC())
    return false;

  if (getObjCRequirementSibling(
          proto, fnRequirement,
          [proto, conformance](AbstractFunctionDecl *candidate) {
            // FIXME: This performs a recursive lookup in the lazy case, so
            // we have to dodge the cycle.
            auto &ctx = proto->getASTContext();

            // If we've already resolved the sibling candidate to a valid
            // witness, don't record a missing witness.
            if (conformance->getWitnessUncached(candidate))
              return true;

            // If we're currently resolving the sibling candidate, it may
            // be that the sibling is missing also, so we must record a
            // missing witness.
            if (ctx.evaluator.hasActiveRequest(
                   ValueWitnessRequest{conformance, candidate}))
              return false;

            // Otherwise, resolve the sibling cadidate; if its valid, don't
            // record a missing witness.
            return static_cast<bool>(conformance->getWitness(candidate));
          })) {
    return true;
  }

  return false;
}

static void diagnoseProtocolStubFixit(
    NormalProtocolConformance *Conf,
    llvm::SmallVector<ASTContext::MissingWitness, 4> MissingWitnesses) {
  DeclContext *DC = Conf->getDeclContext();

  auto &Ctx = DC->getASTContext();

  SourceLoc ComplainLoc = Conf->getLoc();

  // The location where to insert stubs.
  SourceLoc FixitLocation;

  // The location where the type starts.
  SourceLoc TypeLoc;
  if (auto Extension = dyn_cast<ExtensionDecl>(DC)) {
    FixitLocation = Extension->getBraces().Start;
    TypeLoc = Extension->getStartLoc();
  } else if (auto Nominal = dyn_cast<NominalTypeDecl>(DC)) {
    FixitLocation = Nominal->getBraces().Start;
    TypeLoc = Nominal->getStartLoc();
  } else {
    llvm_unreachable("Unknown adopter kind");
  }
  std::string FixIt;
  llvm::SetVector<ValueDecl*> NoStubRequirements;

  // Print stubs for all known missing witnesses.
  printProtocolStubFixitString(TypeLoc, Conf, MissingWitnesses, FixIt,
                               NoStubRequirements);
  auto &Diags = Ctx.Diags;

  // If we are in editor mode, squash all notes into a single fixit.
  if (Ctx.LangOpts.DiagnosticsEditorMode) {
    if (!FixIt.empty()) {
      Diags.diagnose(ComplainLoc, diag::missing_witnesses_general).
        fixItInsertAfter(FixitLocation, FixIt);
    }
    return;
  }
  auto &SM = Ctx.SourceMgr;
  auto FixitBufferId = SM.findBufferContainingLoc(FixitLocation);
  for (const auto &Missing : MissingWitnesses) {
    auto VD = Missing.requirement;

    // Don't ever emit a diagnostic for a requirement in the NSObject
    // protocol. They're not implementable.
    if (isNSObjectProtocol(VD->getDeclContext()->getSelfProtocolDecl()))
      continue;

    // Whether this VD has a stub printed.
    bool AddFixit = !NoStubRequirements.count(VD);
    bool SameFile = VD->getLoc().isValid() ?
      SM.findBufferContainingLoc(VD->getLoc()) == FixitBufferId : false;

    // Issue diagnostics for witness types.
    if (auto MissingTypeWitness = dyn_cast<AssociatedTypeDecl>(VD)) {
      std::optional<InFlightDiagnostic> diag;
      if (isa<BuiltinTupleDecl>(DC->getSelfNominalTypeDecl())) {
        auto expectedTy = getTupleConformanceTypeWitness(DC, MissingTypeWitness);
        diag.emplace(Diags.diagnose(MissingTypeWitness, diag::no_witnesses_type_tuple,
                                    MissingTypeWitness, expectedTy));
      } else {
        diag.emplace(Diags.diagnose(MissingTypeWitness, diag::no_witnesses_type,
                                    MissingTypeWitness));
      }
      if (SameFile) {
        // If the protocol member decl is in the same file of the stub,
        // we can directly associate the fixit with the note issued to the
        // requirement.
        diag->fixItInsertAfter(FixitLocation, FixIt);
      } else {
        diag.value().flush();

        // Otherwise, we have to issue another note to carry the fixit,
        // because editor may assume the fixit is in the same file with the note.
        if (Ctx.LangOpts.DiagnosticsEditorMode) {
          Diags.diagnose(ComplainLoc, diag::missing_witnesses_general)
            .fixItInsertAfter(FixitLocation, FixIt);
        }
      }
      continue;
    }

    // Issue diagnostics for witness values.
    Type RequirementType =
      getRequirementTypeForDisplay(DC->getParentModule(), Conf, VD);
    if (AddFixit) {
      if (SameFile) {
        // If the protocol member decl is in the same file of the stub,
        // we can directly associate the fixit with the note issued to the
        // requirement.
        Diags
            .diagnose(VD, diag::no_witnesses, getProtocolRequirementKind(VD),
                      VD, RequirementType, true)
            .fixItInsertAfter(FixitLocation, FixIt);
      } else {
        // Otherwise, we have to issue another note to carry the fixit,
        // because editor may assume the fixit is in the same file with the note.
        Diags.diagnose(VD, diag::no_witnesses, getProtocolRequirementKind(VD),
                       VD, RequirementType, false);
        if (Ctx.LangOpts.DiagnosticsEditorMode) {
          Diags.diagnose(ComplainLoc, diag::missing_witnesses_general)
            .fixItInsertAfter(FixitLocation, FixIt);
        }
      }
    } else {
      Diags.diagnose(VD, diag::no_witnesses, getProtocolRequirementKind(VD),
                     VD, RequirementType, true);
    }
  }
}

static void diagnoseProtocolStubFixit(
    ASTContext &ctx,
    NormalProtocolConformance *conformance,
    ArrayRef<ASTContext::MissingWitness> missingWitnesses) {
  auto selfInterfaceType = conformance->getDeclContext()->getSelfInterfaceType();
  const auto filteredWitnesses = filterProtocolRequirements(
      missingWitnesses, selfInterfaceType);
  assert(!filteredWitnesses.empty());

  ctx.addDelayedConformanceDiag(conformance, true,
      [filteredWitnesses](NormalProtocolConformance *conf) {
        diagnoseProtocolStubFixit(conf, filteredWitnesses);
      });
}

/// Whether the given protocol requirement has a "Self ==" constraint.
static bool hasSelfSameTypeConstraint(const ValueDecl *req) {
  const auto *proto = cast<ProtocolDecl>(req->getDeclContext());
  const auto *genCtx = req->getAsGenericContext();
  if (!genCtx)
    return false;

  const auto genericSig = genCtx->getGenericSignature();
  const auto selfTy = proto->getSelfInterfaceType();
  for (const auto &constr : genericSig.getRequirements()) {
    if (constr.getKind() != RequirementKind::SameType)
      continue;

    if (constr.getFirstType()->isEqual(selfTy) ||
        constr.getSecondType()->isEqual(selfTy))
      return true;
  }

  return false;
}

/// Determine the given witness has a same-type constraint constraining the
/// given 'Self' type, and return the requirement that does.
///
/// \returns None if there is no such constraint; a non-empty optional that
/// may have the \c RequirementRepr for the actual constraint.
static std::optional<std::pair<RequirementRepr *, Requirement>>
getAdopteeSelfSameTypeConstraint(ClassDecl *selfClass, ValueDecl *witness) {
  auto genericSig =
    witness->getInnermostDeclContext()->getGenericSignatureOfContext();

  // First, search for any bogus requirements.
  auto it = llvm::find_if(genericSig.getRequirements(),
                          [&selfClass](const auto &req) {
    if (req.getKind() != RequirementKind::SameType)
      return false;

    return req.getFirstType()->getAnyNominal() == selfClass
        || req.getSecondType()->getAnyNominal() == selfClass;
  });
  if (it == genericSig.getRequirements().end()) {
    return std::nullopt;
  }

  // Got it! Now try to find the requirement-as-written.
  TrailingWhereClause *where = nullptr;
  if (auto func = dyn_cast<AbstractFunctionDecl>(witness))
    where = func->getTrailingWhereClause();
  else if (auto subscript = dyn_cast<SubscriptDecl>(witness))
    where = subscript->getTrailingWhereClause();

  // A null repr indicates we don't have a valid location to diagnose. But
  // at least we have a requirement we can signal is bogus.
  std::optional<std::pair<RequirementRepr *, Requirement>> target =
      std::make_pair((RequirementRepr *)nullptr, Requirement(*it));
  if (!where) {
    return target;
  }

  // Resolve and search for a written requirement to match our bogus one.
  WhereClauseOwner(cast<GenericContext>(witness), where)
    .visitRequirements(TypeResolutionStage::Structural,
                       [&](Requirement req, RequirementRepr *repr) {
      if (req.getKind() != RequirementKind::SameType) {
        return false;
      }

      if (req.getFirstType()->getAnyNominal() != selfClass &&
          req.getSecondType()->getAnyNominal() != selfClass) {
        return false;
      }

      target.emplace(repr, req);
      return true;
    });
  return target;
}

static bool allowOptionalWitness(ProtocolDecl *proto,
                                 NormalProtocolConformance *conformance,
                                 ValueDecl *requirement) {
  auto Attrs = requirement->getAttrs();

  // An optional requirement is trivially satisfied with an empty requirement.
  if (Attrs.hasAttribute<OptionalAttr>())
    return true;

  // An 'unavailable' requirement is treated like an optional requirement.
  if (Attrs.isUnavailable(proto->getASTContext()))
    return true;

  // A requirement with a satisfied Obj-C alternative requirement is effectively
  // optional.
  if (hasSatisfiedObjCSiblingRequirement(proto, conformance, requirement))
    return true;

  return false;
}

void ConformanceChecker::checkNonFinalClassWitness(ValueDecl *requirement,
                                                   ValueDecl *witness) {
  auto *classDecl = DC->getSelfClassDecl();

  // If we have an initializer requirement and the conforming type
  // is a non-final class, the witness must be 'required'.
  // We exempt Objective-C initializers from this requirement
  // because there is no equivalent to 'required' in Objective-C.
  if (auto ctor = dyn_cast<ConstructorDecl>(witness)) {
    if (!ctor->isRequired() &&
        !ctor->getDeclContext()->getSelfProtocolDecl() &&
        !ctor->hasClangNode()) {
      // FIXME: We're not recovering (in the AST), so the Fix-It
      // should move.
      getASTContext().addDelayedConformanceDiag(Conformance, false,
        [ctor, requirement](NormalProtocolConformance *conformance) {
          bool inExtension = isa<ExtensionDecl>(ctor->getDeclContext());
          auto &diags = ctor->getASTContext().Diags;
          SourceLoc diagLoc = getLocForDiagnosingWitness(conformance, ctor);
          std::optional<InFlightDiagnostic> fixItDiag =
              diags.diagnose(diagLoc, diag::witness_initializer_not_required,
                             requirement, inExtension, conformance->getType());
          if (diagLoc != ctor->getLoc() && !ctor->isImplicit()) {
            // If the main diagnostic is emitted on the conformance, we want to
            // attach the fix-it to the note that shows where the initializer is
            // defined.
            fixItDiag.value().flush();
            fixItDiag.emplace(diags.diagnose(ctor, diag::decl_declared_here,
                                             ctor));
          }
          if (!inExtension) {
            fixItDiag->fixItInsert(ctor->getAttributeInsertionLoc(true),
                                   "required ");
          }
        });
    }
  }

  // Check whether this requirement uses Self in a way that might
  // prevent conformance from succeeding.
  const auto selfRefInfo = requirement->findExistentialSelfReferences(
      Proto->getDeclaredInterfaceType(),
      /*treatNonResultCovariantSelfAsInvariant=*/true);

  if (selfRefInfo.selfRef == TypePosition::Invariant) {
    // References to Self in a position where subclasses cannot do
    // the right thing. Complain if the adoptee is a non-final
    // class.
    getASTContext().addDelayedConformanceDiag(Conformance, false,
      [witness, requirement](NormalProtocolConformance *conformance) {
        auto proto = conformance->getProtocol();
        auto &diags = proto->getASTContext().Diags;
        SourceLoc diagLoc = getLocForDiagnosingWitness(conformance, witness);
        diags.diagnose(diagLoc, diag::witness_self_non_subtype,
                       proto->getDeclaredInterfaceType(), requirement,
                       conformance->getType());
        emitDeclaredHereIfNeeded(diags, diagLoc, witness);
      });
  } else if (selfRefInfo.hasCovariantSelfResult) {
    // The reference to Self occurs in the result type of a method/subscript
    // or the type of a property. A non-final class can satisfy this requirement
    // by holding onto Self accordingly.
    if (witness->getDeclContext()->getSelfClassDecl()) {
      const bool hasDynamicSelfResult = [&] {
        if (auto func = dyn_cast<AbstractFunctionDecl>(witness)) {
          return func->hasDynamicSelfResult();
        } else if (auto var = dyn_cast<VarDecl>(witness)) {
          return var->getInterfaceType()->hasDynamicSelfType();
        }

        return cast<SubscriptDecl>(witness)
            ->getElementInterfaceType()
            ->hasDynamicSelfType();
      }();

      if (!hasDynamicSelfResult) {
        getASTContext().addDelayedConformanceDiag(Conformance, false,
          [witness, requirement](NormalProtocolConformance *conformance) {
            auto proto = conformance->getProtocol();
            auto &diags = proto->getASTContext().Diags;
            SourceLoc diagLoc = getLocForDiagnosingWitness(conformance,witness);
            diags.diagnose(diagLoc, diag::witness_requires_dynamic_self,
                           getProtocolRequirementKind(requirement),
                           requirement,
                           conformance->getType(),
                           proto->getDeclaredInterfaceType());
            emitDeclaredHereIfNeeded(diags, diagLoc, witness);
          });
      }
    }
  } else if (hasSelfSameTypeConstraint(requirement)) {
    if (auto targetPair = getAdopteeSelfSameTypeConstraint(classDecl,
                                                           witness)) {
      // A "Self ==" constraint works incorrectly with subclasses. Complain.
      auto proto = Conformance->getProtocol();
      auto &diags = proto->getASTContext().Diags;
      SourceLoc diagLoc = getLocForDiagnosingWitness(Conformance, witness);
      diags.diagnose(diagLoc, diag::witness_self_same_type,
                     witness,
                     Conformance->getType(),
                     requirement,
                     proto->getDeclaredInterfaceType());
      emitDeclaredHereIfNeeded(diags, diagLoc, witness);

      if (auto requirementRepr = targetPair->first) {
        diags.diagnose(requirementRepr->getSeparatorLoc(),
                       diag::witness_self_weaken_same_type,
                       targetPair->second.getFirstType(),
                       targetPair->second.getSecondType())
          .fixItReplace(requirementRepr->getSeparatorLoc(), ":");
      }
    }
  }

  // A non-final class can model a protocol requirement with a
  // contravariant Self, because here the witness will always have
  // a more general type than the requirement.

  // If the witness is in a protocol extension, there's an additional
  // constraint that either the requirement not produce 'Self' in a
  // covariant position, or the type of the requirement does not involve
  // associated types.
  if (isa<FuncDecl>(witness) || isa<SubscriptDecl>(witness)) {
    if (witness->getDeclContext()->getExtendedProtocolDecl()) {
      if (selfRefInfo.hasCovariantSelfResult && selfRefInfo.assocTypeRef) {
        getASTContext().addDelayedConformanceDiag(Conformance, false,
          [witness, requirement](NormalProtocolConformance *conformance) {
            auto proto = conformance->getProtocol();
            auto &diags = proto->getASTContext().Diags;
            diags.diagnose(conformance->getLoc(),
                           diag::witness_requires_class_implementation,
                           getProtocolRequirementKind(requirement),
                           requirement, conformance->getType());
            diags.diagnose(witness, diag::decl_declared_here, witness);
          });
      }
    }
  }
}

ResolveWitnessResult
ConformanceChecker::resolveWitnessViaLookup(ValueDecl *requirement) {
  assert(!isa<AssociatedTypeDecl>(requirement) && "Use resolveTypeWitnessVia*");
  auto *nominal = DC->getSelfNominalTypeDecl();

  // Determine whether we can derive a witness for this requirement.
  bool canDerive = false;

  auto *SF = DC->getParentSourceFile();
  if (!(SF == nullptr || SF->Kind == SourceFileKind::Interface)) {
    // Can a witness for this requirement be derived for this nominal type?
    if (auto derivable = DerivedConformance::getDerivableRequirement(
                           nominal,
                           requirement)) {
      if (derivable == requirement) {
        // If it's the same requirement, we can derive it here.
        canDerive = true;
      } else {
        // Otherwise, go satisfy the derivable requirement, which can introduce
        // a member that could in turn satisfy *this* requirement.
        auto derivableProto = cast<ProtocolDecl>(derivable->getDeclContext());
        auto conformance =
            DC->getParentModule()->lookupConformance(Adoptee, derivableProto);
        if (conformance.isConcrete()) {
          (void) conformance.getConcrete()->getWitnessDecl(derivable);
        }
      }
    }
  }

  // Find the best witness for the requirement.
  SmallVector<RequirementMatch, 4> matches;
  unsigned numViable = 0;
  unsigned bestIdx = 0;
  bool doNotDiagnoseMatches = false;
  bool ignoringNames = false;
  bool considerRenames =
      !canDerive && !requirement->getAttrs().hasAttribute<OptionalAttr>() &&
      !requirement->getAttrs().isUnavailable(getASTContext());

  if (findBestWitness(requirement,
                      considerRenames ? &ignoringNames : nullptr,
                      Conformance,
                      /* out parameters: */
                      matches, numViable, bestIdx, doNotDiagnoseMatches)) {
    const auto &best = matches[bestIdx];
    auto witness = best.Witness;

    // If the name didn't actually line up, complain.
    if (ignoringNames &&
        requirement->getName() != best.Witness->getName() &&
        !witnessHasImplementsAttrForRequiredName(best.Witness, requirement)) {

      getASTContext().addDelayedConformanceDiag(Conformance, false,
        [witness, requirement](NormalProtocolConformance *conformance) {
          auto proto = conformance->getProtocol();
          auto &diags = proto->getASTContext().Diags;
          {
            SourceLoc diagLoc = getLocForDiagnosingWitness(conformance,witness);
            auto diag = diags.diagnose(
                diagLoc, diag::witness_argument_name_mismatch, witness,
                proto->getDeclaredInterfaceType(), requirement);
            if (diagLoc == witness->getLoc()) {
              fixDeclarationName(diag, witness, requirement->getName());
            } else {
              diag.flush();
              diags.diagnose(witness, diag::decl_declared_here, witness);
            }
          }

          diags.diagnose(requirement, diag::kind_declname_declared_here,
                         DescriptiveDeclKind::Requirement,
                         requirement->getName());
        });
    }
    if (best.Kind == MatchKind::RequiresNonSendable) {
      SendableCheckContext sendFrom(witness->getDeclContext(),
                                    SendableCheck::Explicit);

      auto *nominal = Conformance->getProtocol();
      auto behavior = sendFrom.diagnosticBehavior(nominal);
      if (behavior != DiagnosticBehavior::Ignore) {
        bool isError = behavior < DiagnosticBehavior::Warning;
        
        // Avoid relying on the lifetime of 'this'.
        const DeclContext *DC = this->DC;
        getASTContext().addDelayedConformanceDiag(Conformance, isError,
                        [DC, requirement, witness, sendFrom, nominal](
                          NormalProtocolConformance *conformance) {
          diagnoseSendabilityErrorBasedOn(conformance->getProtocol(), sendFrom,
                                          [&](DiagnosticBehavior limit) {
            auto &diags = DC->getASTContext().Diags;
            diags.diagnose(getLocForDiagnosingWitness(conformance, witness),
                           diag::witness_not_as_sendable,
                           witness, conformance->getProtocol())
                .limitBehaviorUntilSwiftVersion(limit, 6)
                .limitBehaviorIf(sendFrom.preconcurrencyBehavior(nominal));
            diags.diagnose(requirement, diag::less_sendable_reqt_here);
            return false;
          });
        });
      }
    }

    auto check = checkWitness(requirement, best);

    switch (check.Kind) {
    case CheckKind::Success:
      break;

    case CheckKind::Access:
    case CheckKind::AccessOfSetter: {
      // Swift 4.2 relaxed some rules for protocol witness matching.
      //
      // This meant that it was possible for an optional protocol requirement
      // to have a witness where previously in Swift 4.1 it did not.
      //
      // Since witnesses must be as visible as the protocol, this caused a
      // source compatibility break if the witness was not sufficiently
      // visible.
      //
      // Work around this by discarding the witness if its not sufficiently
      // visible.
      if (!getASTContext().isSwiftVersionAtLeast(5))
        if (requirement->getAttrs().hasAttribute<OptionalAttr>())
          return ResolveWitnessResult::Missing;

      // Avoid relying on the lifetime of 'this'.
      const DeclContext *DC = this->DC;
      getASTContext().addDelayedConformanceDiag(Conformance, false,
        [DC, witness, check, requirement](
          NormalProtocolConformance *conformance) {
        auto requiredAccessScope = check.RequiredAccessScope;
        AccessLevel requiredAccess =
          requiredAccessScope.requiredAccessForDiagnostics();
        auto proto = conformance->getProtocol();
        auto protoAccessScope = proto->getFormalAccessScope(DC);
        bool protoForcesAccess =
          requiredAccessScope.hasEqualDeclContextWith(protoAccessScope);
        auto diagKind = protoForcesAccess
                          ? diag::witness_not_accessible_proto
                          : diag::witness_not_accessible_type;
        bool isSetter = (check.Kind == CheckKind::AccessOfSetter);

        auto &diags = DC->getASTContext().Diags;
        diags.diagnose(getLocForDiagnosingWitness(conformance, witness),
                       diagKind, getProtocolRequirementKind(requirement),
                       witness, isSetter, requiredAccess,
                       protoAccessScope.accessLevelForDiagnostics(),
                       proto);

        auto *decl = dyn_cast<AbstractFunctionDecl>(witness);
        if (decl && decl->isSynthesized())
          return;

        diagnoseWitnessFixAccessLevel(diags, witness, requiredAccess,
                                      isSetter);
      });
      break;
    }

    case CheckKind::UsableFromInline:
      getASTContext().addDelayedConformanceDiag(Conformance, false,
                                                DiagnoseUsableFromInline(witness));
      break;

    case CheckKind::Availability: {
      getASTContext().addDelayedConformanceDiag(Conformance, false,
        [witness, requirement, check](
            NormalProtocolConformance *conformance) {
          // FIXME: The problem may not be the OS version.
          ASTContext &ctx = witness->getASTContext();
          auto &diags = ctx.Diags;
          SourceLoc diagLoc = getLocForDiagnosingWitness(conformance, witness);
          diags.diagnose(
              diagLoc, diag::availability_protocol_requires_version,
              conformance->getProtocol()->getName(),
              witness->getName(),
              prettyPlatformString(targetPlatform(ctx.LangOpts)),
              check.RequiredAvailability.getOSVersion().getLowerEndpoint());
          emitDeclaredHereIfNeeded(diags, diagLoc, witness);
          diags.diagnose(requirement,
                         diag::availability_protocol_requirement_here);
        });
      break;
    }

    case CheckKind::Unavailable: {
      auto *attr = requirement->getAttrs().getUnavailable(getASTContext());
      diagnoseOverrideOfUnavailableDecl(witness, requirement, attr);
      break;
    }

    case CheckKind::OptionalityConflict: {
      auto adjustments = best.OptionalAdjustments;

      getASTContext().addDelayedConformanceDiag(Conformance, false,
        [witness, adjustments, requirement](NormalProtocolConformance *conformance) {
          auto proto = conformance->getProtocol();
          auto &ctx = witness->getASTContext();
          auto &diags = ctx.Diags;
          {
            SourceLoc diagLoc = getLocForDiagnosingWitness(conformance,witness);
            auto issues = static_cast<unsigned>(
                classifyOptionalityIssues(adjustments, requirement));
            auto diag = diags.diagnose(
                diagLoc,
                hasAnyError(adjustments)
                    ? diag::err_protocol_witness_optionality
                    : diag::warn_protocol_witness_optionality,
                issues, witness, proto);
            if (diagLoc == witness->getLoc()) {
              addOptionalityFixIts(adjustments, ctx, witness, diag);
            } else {
              diag.flush();
              diags.diagnose(witness, diag::decl_declared_here, witness);
            }
          }

          diags.diagnose(requirement, diag::kind_declname_declared_here,
                         DescriptiveDeclKind::Requirement,
                         requirement->getName());
      });
      break;
    }

    case CheckKind::ConstructorFailability:
      getASTContext().addDelayedConformanceDiag(Conformance, false,
        [witness, requirement](NormalProtocolConformance *conformance) {
          auto ctor = cast<ConstructorDecl>(requirement);
          auto witnessCtor = cast<ConstructorDecl>(witness);
          auto &diags = witness->getASTContext().Diags;
          SourceLoc diagLoc = getLocForDiagnosingWitness(conformance, witness);
          diags.diagnose(diagLoc, diag::witness_initializer_failability,
                         ctor, witnessCtor->isImplicitlyUnwrappedOptional())
            .highlight(witnessCtor->getFailabilityLoc());
          emitDeclaredHereIfNeeded(diags, diagLoc, witness);
        });

      break;

    case CheckKind::WitnessUnavailable:
      getASTContext().addDelayedConformanceDiag(Conformance, true,
        [witness, requirement](NormalProtocolConformance *conformance) {
          auto &diags = witness->getASTContext().Diags;
          SourceLoc diagLoc = getLocForDiagnosingWitness(conformance, witness);
          auto *attr = AvailableAttr::isUnavailable(witness);
          EncodedDiagnosticMessage EncodedMessage(attr->Message);
          diags.diagnose(diagLoc, diag::witness_unavailable,
                         witness, conformance->getProtocol()->getName(),
                         EncodedMessage.Message);
          emitDeclaredHereIfNeeded(diags, diagLoc, witness);
          diags.diagnose(requirement, diag::kind_declname_declared_here,
                         DescriptiveDeclKind::Requirement,
                         requirement->getName());
        });
      break;
    }

    if (auto *classDecl = DC->getSelfClassDecl()) {
      if (!classDecl->isSemanticallyFinal()) {
        checkNonFinalClassWitness(requirement, witness);
      }
    }

    // Record the match.
    recordWitness(requirement, best);
    return ResolveWitnessResult::Success;

    // We have an ambiguity; diagnose it below.
  }

  // We have either no matches or an ambiguous match.

  // If we can derive a definition for this requirement, just call it missing.
  if (canDerive) {
    return ResolveWitnessResult::Missing;
  }

  // If the requirement is optional, it's okay. We'll satisfy this via
  // our handling of default definitions.
  //
  // FIXME: revisit this once we get default definitions in protocol bodies.
  //
  // Treat 'unavailable' implicitly as if it were 'optional'.
  // The compiler will reject actual uses.
  if (allowOptionalWitness(Proto, Conformance, requirement)) {
    return ResolveWitnessResult::Missing;
  }

  // Diagnose the error.

  // If there was an invalid witness that might have worked, just
  // suppress the diagnostic entirely. This stops the diagnostic cascade.
  // FIXME: We could do something crazy, like try to fix up the witness.
  if (doNotDiagnoseMatches) {
    return ResolveWitnessResult::ExplicitFailed;
  }

  if (!numViable) {
    // Save the missing requirement for later diagnosis.
    getASTContext().addDelayedMissingWitness(Conformance, {requirement, matches});
    return ResolveWitnessResult::Missing;
  }

  getASTContext().addDelayedConformanceDiag(Conformance, true,
    [requirement, matches, ignoringNames](
      NormalProtocolConformance *conformance) {
      auto dc = conformance->getDeclContext();
      // Determine the type that the requirement is expected to have.
      Type reqType = getRequirementTypeForDisplay(dc->getParentModule(),
                                                  conformance, requirement);

      auto &diags = dc->getASTContext().Diags;
      auto diagnosticMessage = diag::ambiguous_witnesses;
      if (ignoringNames) {
        diagnosticMessage = diag::ambiguous_witnesses_wrong_name;
      }
      diags.diagnose(requirement, diagnosticMessage,
                     getProtocolRequirementKind(requirement), requirement,
                     reqType);

      // Diagnose each of the matches.
      for (const auto &match : matches)
        diagnoseMatch(dc->getParentModule(), conformance, requirement, match);
    });

  return ResolveWitnessResult::ExplicitFailed;
}

static ValueDecl *deriveProtocolRequirement(DeclContext *DC,
                                            NominalTypeDecl *TypeDecl,
                                            ValueDecl *Requirement) {
  // Note: whenever you update this function, also update
  // DerivedConformance::getDerivableRequirement.
  const auto protocol = cast<ProtocolDecl>(Requirement->getDeclContext());

  const auto derivableKind = protocol->getKnownDerivableProtocolKind();
  if (!derivableKind)
    return nullptr;

  const auto Decl = DC->getInnermostDeclarationDeclContext();
  if (Decl->isInvalid())
    return nullptr;

  DerivedConformance derived(TypeDecl->getASTContext(), Decl, TypeDecl,
                             protocol);

  switch (*derivableKind) {
  case KnownDerivableProtocolKind::RawRepresentable:
    return derived.deriveRawRepresentable(Requirement);

  case KnownDerivableProtocolKind::CaseIterable:
    return derived.deriveCaseIterable(Requirement);

  case KnownDerivableProtocolKind::Comparable:
    return derived.deriveComparable(Requirement);

  case KnownDerivableProtocolKind::Equatable:
    return derived.deriveEquatable(Requirement);

  case KnownDerivableProtocolKind::Hashable:
    return derived.deriveHashable(Requirement);

  case KnownDerivableProtocolKind::BridgedNSError:
    return derived.deriveBridgedNSError(Requirement);

  case KnownDerivableProtocolKind::CodingKey:
    return derived.deriveCodingKey(Requirement);

  case KnownDerivableProtocolKind::Encodable:
    return derived.deriveEncodable(Requirement);

  case KnownDerivableProtocolKind::Decodable:
    return derived.deriveDecodable(Requirement);

  case KnownDerivableProtocolKind::AdditiveArithmetic:
    return derived.deriveAdditiveArithmetic(Requirement);

  case KnownDerivableProtocolKind::Actor:
    return derived.deriveActor(Requirement);

  case KnownDerivableProtocolKind::Differentiable:
    return derived.deriveDifferentiable(Requirement);

  case KnownDerivableProtocolKind::Identifiable:
    if (derived.Nominal->isDistributedActor()) {
      return derived.deriveDistributedActor(Requirement);
    } else {
      // No synthesis is required for other types; we should only end up
      // attempting synthesis if the nominal was a distributed actor.
      llvm_unreachable("Identifiable is synthesized for distributed actors");
    }

  case KnownDerivableProtocolKind::DistributedActor:
    return derived.deriveDistributedActor(Requirement);

  case KnownDerivableProtocolKind::DistributedActorSystem:
    return derived.deriveDistributedActorSystem(Requirement);

  case KnownDerivableProtocolKind::OptionSet:
      llvm_unreachable(
          "When possible, OptionSet is derived via memberwise init synthesis");
  }
  llvm_unreachable("unknown derivable protocol kind");
}

/// Attempt to resolve a witness via derivation.
ResolveWitnessResult ConformanceChecker::resolveWitnessViaDerivation(
                       ValueDecl *requirement) {
  assert(!isa<AssociatedTypeDecl>(requirement) && "Use resolveTypeWitnessVia*");

  auto *SF = DC->getParentSourceFile();
  if (SF != nullptr && SF->Kind == SourceFileKind::Interface)
    return ResolveWitnessResult::Missing;

  // Find the declaration that derives the protocol conformance.
  NominalTypeDecl *derivingTypeDecl = nullptr;
  auto *nominal = DC->getSelfNominalTypeDecl();
  if (DerivedConformance::derivesProtocolConformance(DC, nominal, Proto))
    derivingTypeDecl = nominal;

  if (!derivingTypeDecl) {
    return ResolveWitnessResult::Missing;
  }

  // Attempt to derive the witness.
  auto derived = deriveProtocolRequirement(DC, derivingTypeDecl, requirement);

  if (!derived) {
    return ResolveWitnessResult::ExplicitFailed;
  }

  // Try to match the derived requirement.
  auto match = matchWitness(ReqEnvironmentCache, Proto, Conformance, DC,
                            requirement, derived);
  if (match.isViable()) {
    recordWitness(requirement, match);
    return ResolveWitnessResult::Success;
  }

  // Derivation failed.
  getASTContext().addDelayedConformanceDiag(Conformance, true,
    [](NormalProtocolConformance *conformance) {
      auto proto = conformance->getProtocol();
      auto &diags = proto->getASTContext().Diags;
      diags.diagnose(conformance->getLoc(), diag::protocol_derivation_is_broken,
                     proto->getDeclaredInterfaceType(),
                     conformance->getType());
    });

  return ResolveWitnessResult::ExplicitFailed;
}

// FIXME: revisit this once we get default implementations in protocol bodies.
ResolveWitnessResult ConformanceChecker::resolveWitnessViaDefault(
                       ValueDecl *requirement) {
  assert(!isa<AssociatedTypeDecl>(requirement) && "Use resolveTypeWitnessVia*");

  if (allowOptionalWitness(Proto, Conformance, requirement)) {
    recordOptionalWitness(requirement);
    return ResolveWitnessResult::Success;
  }

  return ResolveWitnessResult::ExplicitFailed;
}

ResolveWitnessResult
ConformanceChecker::resolveWitnessTryingAllStrategies(ValueDecl *requirement) {
  decltype(&ConformanceChecker::resolveWitnessViaLookup) strategies[] = {
      &ConformanceChecker::resolveWitnessViaLookup,
      &ConformanceChecker::resolveWitnessViaDerivation,
      &ConformanceChecker::resolveWitnessViaDefault};

  for (auto strategy : strategies) {
    ResolveWitnessResult result = (this->*strategy)(requirement);
    switch (result) {
    case ResolveWitnessResult::Success:
    case ResolveWitnessResult::ExplicitFailed:
      return result;
    case ResolveWitnessResult::Missing:
      // Continue trying.
      break;
    }
  }

  return ResolveWitnessResult::Missing;
}

void ConformanceChecker::resolveSingleWitness(ValueDecl *requirement) {
  assert(!isa<AssociatedTypeDecl>(requirement) && "Not a value witness");
  assert(!Conformance->hasWitness(requirement) && "Already resolved");

  // Make sure we've validated the requirement.
  if (requirement->isInvalid()) {
    Conformance->setInvalid();
    return;
  }

  if (!requirement->isProtocolRequirement())
    return;

  // Resolve the type witnesses for all associated types referenced by
  // the requirement. If any are erroneous, don't bother resolving the
  // witness.
  auto referenced = evaluateOrDefault(getASTContext().evaluator,
                                      ReferencedAssociatedTypesRequest{requirement},
                                      TinyPtrVector<AssociatedTypeDecl *>());
  for (auto assocType : referenced) {
    if (Conformance->getTypeWitness(assocType)->hasError()) {
      Conformance->setInvalid();
      return;
    }
  }

  // Try to resolve the witness.
  switch (resolveWitnessTryingAllStrategies(requirement)) {
  case ResolveWitnessResult::Success:
    return;

  case ResolveWitnessResult::ExplicitFailed:
    Conformance->setInvalid();
    recordInvalidWitness(requirement);
    return;

  case ResolveWitnessResult::Missing:
    llvm_unreachable("Should have failed");
  }
}

/// FIXME: It feels like this could be part of findExistentialSelfReferences().
static std::optional<Requirement>
hasInvariantSelfRequirement(const ProtocolDecl *proto,
                            ArrayRef<Requirement> reqSig) {
  auto selfTy = proto->getSelfInterfaceType();

  auto containsInvariantSelf = [&](Type t) -> bool {
    struct Walker : public TypeWalker {
      Type SelfTy;
      bool Found = false;

      Walker(Type selfTy) : SelfTy(selfTy) {}

      Action walkToTypePre(Type ty) override {
        // Check for 'Self'.
        if (ty->isEqual(SelfTy)) {
          Found = true;
          return Action::Stop;
        }

        // 'Self.A' is OK.
        if (ty->is<DependentMemberType>())
          return Action::SkipNode;

        return Action::Continue;
      }
    };

    Walker walker(selfTy);
    t.walk(walker);

    return walker.Found;
  };

  for (auto req : reqSig) {
    switch (req.getKind()) {
    case RequirementKind::SameShape:
      llvm_unreachable("Same-shape requirement not supported here");

    case RequirementKind::SameType:
      if (req.getSecondType()->isTypeParameter()) {
        if (req.getFirstType()->isEqual(selfTy))
          return req;
      } else {
        if (containsInvariantSelf(req.getSecondType()))
          return req;
      }
      continue;
    case RequirementKind::Superclass:
      if (containsInvariantSelf(req.getSecondType()))
        return req;
      continue;
    case RequirementKind::Conformance:
    case RequirementKind::Layout:
      continue;
    }

    llvm_unreachable("Bad requirement kind");
  }

  return std::nullopt;
}

static void diagnoseInvariantSelfRequirement(
    SourceLoc loc, Type adoptee, const ProtocolDecl *proto,
    Requirement req, DiagnosticEngine &diags) {
  Type firstType, secondType;
  unsigned kind = 0;

  switch (req.getKind()) {
  case RequirementKind::SameShape:
    llvm_unreachable("Same-shape requirement not supported here");

  case RequirementKind::SameType:
  if (req.getSecondType()->isTypeParameter()) {
      // eg, 'Self == Self.A.B'
      firstType = req.getSecondType();
      secondType = req.getFirstType();
    } else {
      // eg, 'Self.A.B == G<Self>'
      firstType = req.getFirstType();
      secondType = req.getSecondType();
    }
    kind = 0;
    break;
  case RequirementKind::Superclass:
    // eg, 'Self.A.B : G<Self>'
    firstType = req.getFirstType();
    secondType = req.getSecondType();
    kind = 1;
    break;
  case RequirementKind::Conformance:
  case RequirementKind::Layout:
    llvm_unreachable("Invalid requirement kind");
  }

  diags.diagnose(loc, diag::non_final_class_cannot_conform_to_self_same_type,
                 adoptee, proto->getDeclaredInterfaceType(),
                 firstType, kind, secondType)
      .warnUntilSwiftVersion(6);
}

/// Check whether the type witnesses satisfy the protocol's requirement
/// signature. Also checks access level of type witnesses and availiability
/// of associated conformances.
static void ensureRequirementsAreSatisfied(ASTContext &ctx,
                                           NormalProtocolConformance *conformance) {
  auto *dc = conformance->getDeclContext();
  auto proto = conformance->getProtocol();
  auto &diags = ctx.Diags;

  auto *const module = dc->getParentModule();
  auto substitutingType = dc->mapTypeIntoContext(conformance->getType());
  auto substitutions = SubstitutionMap::getProtocolSubstitutions(
      proto, substitutingType, ProtocolConformanceRef(conformance));

  auto reqSig = proto->getRequirementSignature().getRequirements();

  // Non-final classes should not be able to conform to protocols with a
  // same-type requirement on 'Self', since such a conformance would no
  // longer be covariant. For now, this is a warning. Once this becomes
  // an error, we can handle it as part of the above checkGenericArguments()
  // call by passing in a superclass-bound archetype for the 'self' type
  // instead of the concrete class type itself.
  if (auto *classDecl = dc->getSelfClassDecl()) {
    if (!classDecl->isSemanticallyFinal()) {
      if (auto req = hasInvariantSelfRequirement(proto, reqSig)) {
        diagnoseInvariantSelfRequirement(conformance->getLoc(),
                                         dc->getSelfInterfaceType(),
                                         proto, *req, diags);
      }
    }
  }

  const auto result = TypeChecker::checkGenericArgumentsForDiagnostics(
      module, reqSig, QuerySubstitutionMap{substitutions});
  switch (result.getKind()) {
  case CheckRequirementsResult::Success:
    // Go on to check exportability.
    break;

  case CheckRequirementsResult::RequirementFailure:
  case CheckRequirementsResult::SubstitutionFailure:
    // Diagnose the failure generically.
    // FIXME: Would be nice to give some more context here!
    if (!conformance->isInvalid()) {
      if (result.getKind() == CheckRequirementsResult::RequirementFailure) {
        ctx.addDelayedConformanceDiag(conformance, /*isError=*/true,
          [result, proto, substitutions, module](NormalProtocolConformance *conformance) {
            TypeChecker::diagnoseRequirementFailure(
              result.getRequirementFailureInfo(),
              conformance->getLoc(), conformance->getLoc(),
              proto->getDeclaredInterfaceType(),
              {proto->getSelfInterfaceType()->castTo<GenericTypeParamType>()},
              QuerySubstitutionMap{substitutions}, module);
          });
      }

      conformance->setInvalid();
    }
    return;
  }

  bool isTupleConformance = isa<BuiltinTupleDecl>(dc->getSelfNominalTypeDecl());

  auto where = ExportContext::forConformance(dc, proto);

  conformance->forEachTypeWitness([&](AssociatedTypeDecl *assocType,
                                      Type type, TypeDecl *typeDecl) -> bool {
    checkObjCTypeErasedGenerics(conformance, assocType, type, typeDecl);

    // Tuple conformances can only witness associated types by projecting them
    // element-wise.
    if (isTupleConformance) {
      auto expectedTy = getTupleConformanceTypeWitness(dc, assocType);
      if (!type->hasError() && !expectedTy->isEqual(type)) {
        ctx.addDelayedConformanceDiag(conformance, true,
              [dc, type, typeDecl, expectedTy](NormalProtocolConformance *conformance) {
          dc->getASTContext().Diags.diagnose(
              getLocForDiagnosingWitness(conformance, typeDecl),
              diag::protocol_type_witness_tuple,
              type, expectedTy);
        });
      }
    }

    if (typeDecl && !typeDecl->isImplicit()) {
      auto requiredAccessScope = evaluateOrDefault(
          ctx.evaluator, ConformanceAccessScopeRequest{dc, proto},
          std::make_pair(AccessScope::getPublic(), false));

      // Check access.
      bool isSetter = false;
      if (checkWitnessAccess(dc, assocType, typeDecl, &isSetter)) {
        assert(!isSetter);

        ctx.addDelayedConformanceDiag(conformance, false,
            [dc, requiredAccessScope, typeDecl](
              NormalProtocolConformance *conformance) {
          AccessLevel requiredAccess =
              requiredAccessScope.first.requiredAccessForDiagnostics();
          auto proto = conformance->getProtocol();
          auto protoAccessScope = proto->getFormalAccessScope(dc);
          bool protoForcesAccess =
              requiredAccessScope.first.hasEqualDeclContextWith(protoAccessScope);
          auto diagKind = protoForcesAccess
                            ? diag::type_witness_not_accessible_proto
                            : diag::type_witness_not_accessible_type;
          auto &diags = dc->getASTContext().Diags;
          diags.diagnose(getLocForDiagnosingWitness(conformance, typeDecl),
                         diagKind, typeDecl, requiredAccess, proto);
          diagnoseWitnessFixAccessLevel(diags, typeDecl, requiredAccess);
        });
      }

      if (requiredAccessScope.second) {
        bool witnessIsUsableFromInline = typeDecl->getFormalAccessScope(
            dc, /*usableFromInlineAsPublic*/true).isPublic();
        if (!witnessIsUsableFromInline)
          ctx.addDelayedConformanceDiag(conformance, false,
                                        DiagnoseUsableFromInline(typeDecl));
      }
    }

    // Make sure any associated type witnesses don't make reference to a
    // type we can't emit metadata for, or we're going to have trouble at
    // runtime.
    checkTypeMetadataAvailability(type, typeDecl->getLoc(),
                                  where.getDeclContext());

    return false;
  });

  // Now check that our associated conformances are at least as visible as
  // the conformance itself.
  if (where.isImplicit())
    return;

  conformance->forEachAssociatedConformance(
    [&](Type depTy, ProtocolDecl *proto, unsigned index) {
      auto assocConf = conformance->getAssociatedConformance(depTy, proto);
      if (assocConf.isConcrete()) {
        auto *concrete = assocConf.getConcrete();
        auto replacementTy = dc->mapTypeIntoContext(concrete->getType());
        diagnoseConformanceAvailability(conformance->getLoc(),
                                        assocConf, where,
                                        depTy, replacementTy);
      }

      return false;
    });
}

#pragma mark Protocol conformance checking

/// Determine whether mapping the interface type of the given protocol non-type
/// requirement into the context of the given conformance produces a well-formed
/// type.
static bool
hasInvalidTypeInConformanceContext(const ValueDecl *requirement,
                                   NormalProtocolConformance *conformance) {
  assert(!isa<TypeDecl>(requirement));
  assert(requirement->getDeclContext()->getSelfProtocolDecl() ==
         conformance->getProtocol());

  // FIXME: getInterfaceType() on properties returns contextual types that have
  // been mapped out of context, but mapTypeOutOfContext() does not reconstitute
  // type parameters that were substituted with concrete types. Instead,
  // patterns should be refactored to use interface types, at least if they
  // appear in type contexts.
  auto interfaceTy = requirement->getInterfaceType();

  // Skip the curried 'self' parameter.
  if (requirement->hasCurriedSelf())
    interfaceTy = interfaceTy->castTo<AnyFunctionType>()->getResult();

  // For subscripts, build a regular function type to skip walking generic
  // requirements.
  if (auto *gft = interfaceTy->getAs<GenericFunctionType>()) {
    interfaceTy = FunctionType::get(gft->getParams(), gft->getResult(),
                                    gft->getExtInfo());
  }

  if (!interfaceTy->hasTypeParameter())
    return false;

  const auto subs = SubstitutionMap::getProtocolSubstitutions(
      conformance->getProtocol(),
      conformance->getDeclContext()->mapTypeIntoContext(conformance->getType()),
      ProtocolConformanceRef(conformance));

  class Walker final : public TypeWalker {
    const SubstitutionMap &Subs;
    const ProtocolDecl *Proto;

  public:
    explicit Walker(const SubstitutionMap &Subs, const ProtocolDecl *Proto)
        : Subs(Subs), Proto(Proto) {}

    Action walkToTypePre(Type ty) override {
      if (!ty->hasTypeParameter())
        return Action::SkipNode;

      auto *const dmt = ty->getAs<DependentMemberType>();
      if (!dmt)
        return Action::Continue;

      // We only care about 'Self'-rooted type parameters.
      if (!dmt->getRootGenericParam()->isEqual(Proto->getSelfInterfaceType()))
        return Action::SkipNode;

      if (ty.subst(Subs)->hasError())
        return Action::Stop;
      return Action::SkipNode;
    }
  };

  return interfaceTy.walk(Walker(subs, conformance->getProtocol()));
}

void ConformanceChecker::resolveValueWitnesses() {
  bool usesPreconcurrencyConformance = false;

  for (auto *requirement : Proto->getProtocolRequirements()) {
    // Associated type requirements handled elsewhere.
    if (isa<TypeDecl>(requirement))
      continue;

    /// Local function to finalize the witness.
    auto finalizeWitness = [&] {
      // Find the witness.
      auto witness = Conformance->getWitnessUncached(requirement).getDecl();
      if (!witness) {
        return;
      }

      auto &C = witness->getASTContext();

      // Check actor isolation. If we need to enter into the actor's
      // isolation within the witness thunk, record that.
      if (auto enteringIsolation = checkActorIsolation(requirement, witness)) {
        // Only @preconcurrency conformances allow entering isolation
        // when neither requirement nor witness are async by adding a
        // runtime precondition that witness is always called on the
        // expected executor.
        if (Conformance->isPreconcurrency()) {
          usesPreconcurrencyConformance =
              !isAsyncDecl(requirement) && !isAsyncDecl(witness);
        }

        Conformance->overrideWitness(
            requirement,
            Conformance->getWitnessUncached(requirement)
              .withEnterIsolation(*enteringIsolation));
      }

      // Objective-C checking for @objc requirements.
      if (requirement->isObjC() &&
          requirement->getName() == witness->getName() &&
          !requirement->getAttrs().isUnavailable(getASTContext())) {
        // The witness must also be @objc.
        if (!witness->isObjC()) {
          bool isOptional =
            requirement->getAttrs().hasAttribute<OptionalAttr>();
          SourceLoc diagLoc = getLocForDiagnosingWitness(Conformance, witness);
          if (auto witnessFunc = dyn_cast<AbstractFunctionDecl>(witness)) {
            auto diagInfo = getObjCMethodDiagInfo(witnessFunc);
            std::optional<InFlightDiagnostic> fixItDiag = C.Diags.diagnose(
                diagLoc,
                isOptional ? diag::witness_non_objc_optional
                           : diag::witness_non_objc,
                diagInfo.first, diagInfo.second, Proto->getName());
            if (diagLoc != witness->getLoc()) {
              // If the main diagnostic is emitted on the conformance, we want
              // to attach the fix-it to the note that shows where the
              // witness is defined.
              fixItDiag.value().flush();
              fixItDiag.emplace(witness->diagnose(
                  diag::make_decl_objc, witness->getDescriptiveKind()));
            }
            if (!witness->canInferObjCFromRequirement(requirement)) {
              fixDeclarationObjCName(
                  fixItDiag.value(), witness,
                  witness->getObjCRuntimeName(),
                  requirement->getObjCRuntimeName());
            }
          } else if (isa<VarDecl>(witness)) {
            std::optional<InFlightDiagnostic> fixItDiag = C.Diags.diagnose(
                diagLoc,
                isOptional ? diag::witness_non_objc_storage_optional
                           : diag::witness_non_objc_storage,
                /*isSubscript=*/false, witness->getName(), Proto->getName());
            if (diagLoc != witness->getLoc()) {
              // If the main diagnostic is emitted on the conformance, we want
              // to attach the fix-it to the note that shows where the
              // witness is defined.
              fixItDiag.value().flush();
              fixItDiag.emplace(witness->diagnose(
                  diag::make_decl_objc, witness->getDescriptiveKind()));
            }
            if (!witness->canInferObjCFromRequirement(requirement)) {
              fixDeclarationObjCName(
                  fixItDiag.value(), witness,
                  witness->getObjCRuntimeName(),
                  requirement->getObjCRuntimeName());
            }
          } else if (isa<SubscriptDecl>(witness)) {
            std::optional<InFlightDiagnostic> fixItDiag = C.Diags.diagnose(
                diagLoc,
                isOptional ? diag::witness_non_objc_storage_optional
                           : diag::witness_non_objc_storage,
                /*isSubscript=*/true, witness->getName(), Proto->getName());
            if (diagLoc != witness->getLoc()) {
              // If the main diagnostic is emitted on the conformance, we want
              // to attach the fix-it to the note that shows where the
              // witness is defined.
              fixItDiag.value().flush();
              fixItDiag.emplace(witness->diagnose(
                  diag::make_decl_objc, witness->getDescriptiveKind()));
            }
            fixItDiag->fixItInsert(witness->getAttributeInsertionLoc(false),
                                   "@objc ");
          }

          // If the requirement is optional, @nonobjc suppresses the
          // diagnostic.
          if (isOptional) {
            witness->diagnose(diag::req_near_match_nonobjc, false)
                .fixItInsert(witness->getAttributeInsertionLoc(false),
                             "@nonobjc ");
          }

          requirement->diagnose(diag::kind_declname_declared_here,
                                DescriptiveDeclKind::Requirement,
                                requirement->getName());

          Conformance->setInvalid();
          return;
        }

        // The selectors must coincide.
        if (checkObjCWitnessSelector(requirement, witness)) {
          Conformance->setInvalid();
          return;
        }
      }
    };

    // If we've already determined this witness, skip it.
    if (Conformance->hasWitness(requirement)) {
      finalizeWitness();
      continue;
    }

    // Make sure we've got an interface type.
    if (requirement->isInvalid()) {
      Conformance->setInvalid();
      continue;
    }

    // If this requirement is part of a pair of imported async requirements,
    // where one has already been witnessed, we can skip it.
    //
    // This situation primarily arises when the ClangImporter translates an
    // async-looking ObjC protocol method requirement into two Swift protocol
    // requirements: an async version and a sync version. Exactly one of the two
    // must be witnessed by the conformer.
    if (getObjCRequirementSibling(Proto, requirement,
                                  [this](AbstractFunctionDecl *cand) {
                                    return static_cast<bool>(
                                        this->Conformance->getWitness(cand));
                                  })) {
      recordOptionalWitness(requirement);
      finalizeWitness();
      continue;
    }

    // Try substituting into the requirement's interface type. If we fail,
    // either a generic requirement was not satisfied or we tripped on an
    // invalid type witness, and there's no point in resolving a witness.
    if (hasInvalidTypeInConformanceContext(requirement, Conformance)) {
      continue;
    }

    // Try to resolve the witness.
    switch (resolveWitnessTryingAllStrategies(requirement)) {
    case ResolveWitnessResult::Success:
      finalizeWitness();
      continue;

    case ResolveWitnessResult::ExplicitFailed:
      Conformance->setInvalid();
      continue;

    case ResolveWitnessResult::Missing:
      // Let it get diagnosed later.
      break;
    }
  }

  if (Conformance->isPreconcurrency() && !usesPreconcurrencyConformance) {
    DC->getASTContext().Diags.diagnose(
        Conformance->getLoc(), diag::preconcurrency_conformance_not_used,
        Proto->getDeclaredInterfaceType());
  }

  // Finally, check some ad-hoc protocol requirements.
  //
  // These protocol requirements are not expressible in Swift today, but as
  // the type system gains the required abilities, we should strive to move
  // them to plain-old protocol requirements.
  if (Proto->isSpecificProtocol(KnownProtocolKind::DistributedActorSystem) ||
      Proto->isSpecificProtocol(KnownProtocolKind::DistributedTargetInvocationEncoder) ||
      Proto->isSpecificProtocol(KnownProtocolKind::DistributedTargetInvocationDecoder) ||
      Proto->isSpecificProtocol(KnownProtocolKind::DistributedTargetInvocationResultHandler)) {
    checkDistributedActorSystemAdHocProtocolRequirements(
        Context, Proto, Conformance, Adoptee, /*diagnose=*/true);
  }
}

evaluator::SideEffect
ResolveValueWitnessesRequest::evaluate(Evaluator &evaluator,
                                NormalProtocolConformance *conformance) const {
  auto &ctx = conformance->getDeclContext()->getASTContext();
  ConformanceChecker checker(ctx, conformance);
  checker.resolveValueWitnesses();
  return evaluator::SideEffect();
}

void swift::diagnoseConformanceFailure(Type T,
                                       ProtocolDecl *Proto,
                                       DeclContext *DC,
                                       SourceLoc ComplainLoc) {
  if (T->hasError())
    return;

  ASTContext &ctx = DC->getASTContext();
  auto &diags = ctx.Diags;

  // If we're checking conformance of an existential type to a protocol,
  // do a little bit of extra work to produce a better diagnostic.
  if (T->isExistentialType() &&
      TypeChecker::containsProtocol(T, Proto, DC->getParentModule())) {

    if (!T->isObjCExistentialType()) {
      Type constraintType = T;
      if (auto existential = T->getAs<ExistentialType>())
        constraintType = existential->getConstraintType();
      diags.diagnose(ComplainLoc, diag::type_cannot_conform,
                     T, Proto->getDeclaredInterfaceType());
      diags.diagnose(ComplainLoc,
                     diag::only_concrete_types_conform_to_protocols);
      return;
    }

    diags.diagnose(ComplainLoc, diag::protocol_does_not_conform_static,
                   T, Proto->getDeclaredInterfaceType());
    return;
  }

  // Special case: diagnose conversion to ExpressibleByNilLiteral, since we
  // know this is something involving 'nil'.
  if (Proto->isSpecificProtocol(KnownProtocolKind::ExpressibleByNilLiteral)) {
    diags.diagnose(ComplainLoc, diag::cannot_use_nil_with_this_type, T);
    return;
  }

  // Special case: a distributed actor conformance often can fail because of
  // a missing ActorSystem (or DefaultDistributedActorSystem) typealias.
  // In this case, the "normal" errors are an avalanche of errors related to
  // missing things in the actor that don't help users diagnose the root problem.
  // Instead, we want to suggest adding the typealias.
  if (Proto->isSpecificProtocol(KnownProtocolKind::DistributedActor)) {
    auto nominal = T->getNominalOrBoundGenericNominal();
    if (!nominal)
      return;

    if (isa<ClassDecl>(nominal) &&
        !nominal->isDistributedActor()) {
      if (nominal->isActor()) {
        diags.diagnose(ComplainLoc,
                       diag::actor_cannot_inherit_distributed_actor_protocol,
                       nominal->getName());
      } // else, already diagnosed elsewhere
      return;
    }

    if (nominal->isDistributedActor()) {
      // If it is missing the ActorSystem type, suggest adding it:
      auto systemTy = getDistributedActorSystemType(/*actor=*/nominal);
      if (!systemTy || systemTy->hasError()) {
        diags.diagnose(ComplainLoc,
                       diag::distributed_actor_conformance_missing_system_type,
                       nominal->getName());
        diags.diagnose(nominal->getStartLoc(),
                       diag::note_distributed_actor_system_can_be_defined_using_defaultdistributedactorsystem);
      }
    }

    // For a non-class nominal type, we already diagnose the failure in
    // ensureRequirementsAreSatisfied() when the 'Self: AnyObject' requirement
    // fails.
    if (!isa<ClassDecl>(nominal))
      return;
  }

  // Special case: for enums with a raw type, explain that the failing
  // conformance to RawRepresentable was inferred.
  if (auto enumDecl = T->getEnumOrBoundGenericEnum()) {
    if (Proto->isSpecificProtocol(KnownProtocolKind::RawRepresentable) &&
        enumDecl->hasRawType() &&
        !enumDecl->getRawType()->is<ErrorType>()) {

      auto rawType = enumDecl->getRawType();

      diags.diagnose(enumDecl->getInherited().getStartLoc(),
                     diag::enum_raw_type_nonconforming_and_nonsynthable, T,
                     rawType);

      // If the reason is that the raw type does not conform to
      // Equatable, say so.
      //
      // Map it into context since we want to check conditional requirements.
      rawType = enumDecl->mapTypeIntoContext(rawType);
      if (!TypeChecker::conformsToKnownProtocol(
              rawType, KnownProtocolKind::Equatable, DC->getParentModule())) {
        SourceLoc loc = enumDecl->getInherited().getStartLoc();
        diags.diagnose(loc, diag::enum_raw_type_not_equatable, rawType);
        return;
      }

      return;
    }
  }

  // One cannot meaningfully declare conformance to the NSObject protocol
  // in Swift. Suggest inheritance from NSObject instead.
  if (isNSObjectProtocol(Proto)) {
    if (T->getClassOrBoundGenericClass()) {
      auto diag =
          diags.diagnose(ComplainLoc, diag::type_cannot_conform_to_nsobject,
                         T);

      // Try to suggest inheriting from NSObject instead.
      auto classDecl = dyn_cast<ClassDecl>(DC);
      if (!classDecl)
        return;

      auto inheritedTypes = classDecl->getInherited();
      for (auto i : inheritedTypes.getIndices()) {
        Type inheritedTy = inheritedTypes.getResolvedType(i);

        // If it's a class, we cannot suggest a different class to inherit
        // from.
        if (inheritedTy->getClassOrBoundGenericClass())
          return;

        // Is it the NSObject protocol?
        if (auto protoTy = inheritedTy->getAs<ProtocolType>()) {
          if (isNSObjectProtocol(protoTy->getDecl())) {
            diag.fixItReplace(inheritedTypes.getEntry(i).getSourceRange(),
                              "NSObject");
            return;
          }
        }
      }

      return;
    }
  }

  diags.diagnose(ComplainLoc, diag::type_does_not_conform,
                 T, Proto->getDeclaredInterfaceType());
}

ProtocolConformanceRef
TypeChecker::containsProtocol(Type T, ProtocolDecl *Proto, ModuleDecl *M,
                              bool allowMissing) {
  // Existential types don't need to conform, i.e., they only need to
  // contain the protocol.
  if (T->isExistentialType()) {
    // Handle the special case of the Error protocol, which self-conforms
    // *and* has a witness table.
    auto constraint = T;
    if (auto existential = T->getAs<ExistentialType>())
      constraint = existential->getConstraintType();
    if (constraint->isEqual(Proto->getDeclaredInterfaceType()) &&
        Proto->requiresSelfConformanceWitnessTable()) {
      auto &ctx = M->getASTContext();
      return ProtocolConformanceRef(ctx.getSelfConformance(Proto));
    }

    auto layout = T->getExistentialLayout();

    // First, if we have a superclass constraint, the class may conform
    // concretely.
    //
    // Note that `allowMissing` is not propagated here because it
    // would result in a missing conformance if type is `& Sendable`
    // protocol composition. It's handled for type as a whole below.
    if (auto superclass = layout.getSuperclass()) {
      auto result = M->lookupConformance(superclass, Proto,
                                         /*allowMissing=*/false);
      if (result) {
        return result;
      }
    }

    // Next, check if the existential contains the protocol in question.
    for (auto *PD : layout.getProtocols()) {
      // If we found the protocol we're looking for, return an abstract
      // conformance to it.
      if (PD == Proto)
        return ProtocolConformanceRef(Proto);

      // Now check refined protocols.
      if (PD->inheritsFrom(Proto))
        return ProtocolConformanceRef(Proto);
    }

    return allowMissing ? ProtocolConformanceRef::forMissingOrInvalid(T, Proto)
                        : ProtocolConformanceRef::forInvalid();
  }

  // For non-existential types, this is equivalent to checking conformance.
  return M->lookupConformance(T, Proto, allowMissing);
}

bool TypeChecker::conformsToKnownProtocol(
    Type type, KnownProtocolKind protocol, ModuleDecl *module,
    bool allowMissing) {
  if (auto *proto = module->getASTContext().getProtocol(protocol))
    return (bool) module->checkConformance(type, proto, allowMissing);
  return false;
}

bool
TypeChecker::couldDynamicallyConformToProtocol(Type type, ProtocolDecl *Proto,
                                               ModuleDecl *M) {
  // An existential may have a concrete underlying type with protocol conformances
  // we cannot know statically.
  if (type->isExistentialType())
    return true;

  // The underlying concrete type may have a `Hashable` conformance that is
  // not possible to know statically.
  if (type->isAnyHashable()) {
    return true;
  }

  // A generic archetype may have protocol conformances we cannot know
  // statically.
  if (type->is<ArchetypeType>())
    return true;
  
  // A non-final class might have a subclass that conforms to the protocol.
  if (auto *classDecl = type->getClassOrBoundGenericClass()) {
    if (!classDecl->isSemanticallyFinal())
      return true;
  }

  // For standard library collection types such as Array, Set or Dictionary
  // which have custom casting machinery implemented for situations like:
  //
  //  func encodable(_ value: Encodable) {
  //    _ = value as! [String : Encodable]
  //  }
  // we are skipping checking conditional requirements using lookupConformance,
  // as an intermediate collection cast can dynamically change if the conditions
  // are met or not.
  if (type->isKnownStdlibCollectionType())
    return !M->lookupConformance(type, Proto, /*allowMissing=*/true)
                .isInvalid();
  return !M->checkConformance(type, Proto).isInvalid();
}

/// Determine the score when trying to match two identifiers together.
static unsigned scoreIdentifiers(Identifier lhs, Identifier rhs,
                                 unsigned limit) {
  // Simple case: we have the same identifier.
  if (lhs == rhs) return 0;

  // One of the identifiers is empty. Use the length of the non-empty
  // identifier.
  if (lhs.empty() != rhs.empty())
    return lhs.empty() ? rhs.str().size() : lhs.str().size();

  // Compute the edit distance between the two names.
  return lhs.str().edit_distance(rhs.str(), true, limit);
}

/// Combine the given base name and first argument label into a single
/// name.
static StringRef
combineBaseNameAndFirstArgument(Identifier baseName,
                                Identifier firstArgName,
                                SmallVectorImpl<char> &scratch) {
  // Handle cases where one or the other name is empty.
  if (baseName.empty()) {
    if (firstArgName.empty()) return "";
    return firstArgName.str();
  }

  if (firstArgName.empty())
    return baseName.str();

  // Append the first argument name to the base name.
  scratch.clear();
  scratch.append(baseName.str().begin(), baseName.str().end());
  camel_case::appendSentenceCase(scratch, firstArgName.str());
  return StringRef(scratch.data(), scratch.size());
}

/// Compute the scope between two potentially-matching names, which is
/// effectively the sum of the edit distances between the corresponding
/// argument labels.
static std::optional<unsigned> scorePotentiallyMatchingNames(DeclName lhs,
                                                             DeclName rhs,
                                                             bool isFunc,
                                                             unsigned limit) {
  // If there are a different number of argument labels, we're done.
  if (lhs.getArgumentNames().size() != rhs.getArgumentNames().size())
    return std::nullopt;

  // Score the base name match. If there is a first argument for a
  // function, include its text along with the base name's text.
  unsigned score;
  if (!lhs.isSpecial() && !rhs.isSpecial()) {
    if (lhs.getArgumentNames().empty() || !isFunc) {
      score = scoreIdentifiers(lhs.getBaseIdentifier(), rhs.getBaseIdentifier(),
                               limit);
    } else {
      llvm::SmallString<16> lhsScratch;
      StringRef lhsFirstName =
        combineBaseNameAndFirstArgument(lhs.getBaseIdentifier(),
                                        lhs.getArgumentNames()[0],
                                        lhsScratch);

      llvm::SmallString<16> rhsScratch;
      StringRef rhsFirstName =
        combineBaseNameAndFirstArgument(rhs.getBaseIdentifier(),
                                        rhs.getArgumentNames()[0],
                                        rhsScratch);

      score = lhsFirstName.edit_distance(rhsFirstName.str(), true, limit);
    }
  } else {
    if (lhs.getBaseName().getKind() == rhs.getBaseName().getKind()) {
      score = 0;
    } else {
      return std::nullopt;
    }
  }
  if (score > limit)
    return std::nullopt;

  // Compute the edit distance between matching argument names.
  for (unsigned i = isFunc ? 1 : 0; i < lhs.getArgumentNames().size(); ++i) {
    score += scoreIdentifiers(lhs.getArgumentNames()[i],
                              rhs.getArgumentNames()[i],
                              limit - score);
    if (score > limit)
      return std::nullopt;
  }

  return score;
}

/// Determine the score between two potentially-matching declarations.
static std::optional<unsigned>
scorePotentiallyMatching(ValueDecl *req, ValueDecl *witness, unsigned limit) {
  /// Apply omit-needless-words to the given declaration, if possible.
  auto omitNeedlessWordsIfPossible =
      [](ValueDecl *VD) -> std::optional<DeclName> {
    if (auto func = dyn_cast<AbstractFunctionDecl>(VD))
      return TypeChecker::omitNeedlessWords(func);
    if (auto var = dyn_cast<VarDecl>(VD)) {
      if (auto newName = TypeChecker::omitNeedlessWords(var))
        return DeclName(*newName);
      return std::nullopt;
    }
    return std::nullopt;
  };

  DeclName reqName = req->getName();
  DeclName witnessName = witness->getName();

  // For @objc protocols, apply the omit-needless-words heuristics to
  // both names.
  if (cast<ProtocolDecl>(req->getDeclContext())->isObjC()) {
    if (auto adjustedReqName = omitNeedlessWordsIfPossible(req))
      reqName = *adjustedReqName;
    if (auto adjustedWitnessName = omitNeedlessWordsIfPossible(witness))
      witnessName = *adjustedWitnessName;
  }

  return scorePotentiallyMatchingNames(reqName, witnessName, isa<FuncDecl>(req),
                                       limit);
}

namespace {
  /// Describes actions one could take to suppress a warning about a
  /// nearly-matching witness for an optional requirement.
  enum class PotentialWitnessWarningSuppression {
    MoveToExtension,
    MoveToAnotherExtension
  };
} // end anonymous namespace

/// Determine we can suppress the warning about a potential witness nearly
/// matching an optional requirement by moving the declaration.
std::optional<PotentialWitnessWarningSuppression>
canSuppressPotentialWitnessWarningWithMovement(ValueDecl *requirement,
                                               ValueDecl *witness) {
  // If the witness is within an extension, it can be moved to another
  // extension.
  if (isa<ExtensionDecl>(witness->getDeclContext()))
    return PotentialWitnessWarningSuppression::MoveToAnotherExtension;

  // A stored property cannot be moved to an extension.
  if (auto var = dyn_cast<VarDecl>(witness)) {
    if (var->hasStorage())
      return std::nullopt;
  }

  // If the witness is within a struct or enum, it can be freely moved to
  // another extension.
  if (isa<StructDecl>(witness->getDeclContext()) ||
      isa<EnumDecl>(witness->getDeclContext()))
    return PotentialWitnessWarningSuppression::MoveToExtension;

  // From here on, we only handle members of classes.
  auto classDecl = dyn_cast<ClassDecl>(witness->getDeclContext());
  if (!classDecl)
    return std::nullopt;

  // If the witness is a designated or required initializer, we can't move it
  // to an extension.
  if (auto ctor = dyn_cast<ConstructorDecl>(witness)) {
    if (ctor->isDesignatedInit() || ctor->isRequired())
      return std::nullopt;
  }

  // We can move this entity to an extension.
  return PotentialWitnessWarningSuppression::MoveToExtension;
}

/// Determine we can suppress the warning about a potential witness nearly
/// matching an optional requirement by adding @nonobjc.
static bool
canSuppressPotentialWitnessWarningWithNonObjC(ValueDecl *requirement,
                                              ValueDecl *witness) {
  // The requirement must be @objc.
  if (!requirement->isObjC()) return false;

  // The witness must not have @nonobjc.
  if (witness->getAttrs().hasAttribute<NonObjCAttr>()) return false;

  // The witness must be @objc.
  if (!witness->isObjC()) return false;

  // ... but not explicitly.
  if (auto attr = witness->getAttrs().getAttribute<ObjCAttr>()) {
    if (!attr->isImplicit() || attr->getAddedByAccessNote()) return false;
  }

  // And not because it has to be for overriding.
  if (auto overridden = witness->getOverriddenDecl())
    if (overridden->isObjC()) return false;

  // @nonobjc can be used to silence this warning.
  return true;
}

/// Get the length of the given full name, counting up the base name and all
/// argument labels.
static unsigned getNameLength(DeclName name) {
  unsigned length = 0;
  if (!name.getBaseName().empty() && !name.getBaseName().isSpecial())
    length += name.getBaseIdentifier().str().size();
  for (auto arg : name.getArgumentNames()) {
    if (!arg.empty())
      length += arg.str().size();
  }
  return length;
}

/// Determine whether a particular declaration is generic.
static bool isGeneric(ValueDecl *decl) {
  if (auto func = dyn_cast<AbstractFunctionDecl>(decl))
    return func->isGeneric();
  if (auto subscript = dyn_cast<SubscriptDecl>(decl))
    return subscript->isGeneric();
  return false;
}

/// Determine whether this is an unlabeled initializer or subscript.
static bool isUnlabeledInitializerOrSubscript(ValueDecl *value) {
  ParameterList *paramList = nullptr;
  if (isa<ConstructorDecl>(value) || isa<SubscriptDecl>(value)) {
    paramList = getParameterList(value);
  } else {
    return false;
  }

  for (auto param : *paramList) {
    if (!param->getArgumentName().empty()) return false;
  }

  return true;
}

/// Determine whether this declaration is an initializer
/// Determine whether we should warn about the given witness being a close
/// match for the given optional requirement.
static bool shouldWarnAboutPotentialWitness(
                                      MultiConformanceChecker &groupChecker,
                                      ValueDecl *req,
                                      ValueDecl *witness,
                                      AccessLevel access,
                                      unsigned score) {
  // If the witness is covered, don't warn about it.
  if (groupChecker.isCoveredMember(witness))
    return false;

  // If the kinds of the requirement and witness are different, there's
  // nothing to warn about.
  if (req->getKind() != witness->getKind())
    return false;

  // If the warning couldn't be suppressed, don't warn.
  if (!canSuppressPotentialWitnessWarningWithMovement(req, witness) &&
      !canSuppressPotentialWitnessWarningWithNonObjC(req, witness))
    return false;

  // If the potential witness for an @objc requirement is already
  // marked @nonobjc, don't warn.
  if (req->isObjC() && witness->getAttrs().hasAttribute<NonObjCAttr>())
    return false;

  // If the witness is generic and requirement is not, or vice-versa,
  // don't warn.
  if (isGeneric(req) != isGeneric(witness))
    return false;

  // Don't warn if the potential witness has been explicitly given less
  // visibility than the conformance.
  if (witness->getFormalAccess() < access) {
    if (auto attr = witness->getAttrs().getAttribute<AccessControlAttr>())
      if (!attr->isImplicit()) return false;
  }

  // Don't warn if the requirement or witness is an initializer or subscript
  // with no argument labels.
  if (isUnlabeledInitializerOrSubscript(req) ||
      isUnlabeledInitializerOrSubscript(witness))
    return false;

  // For non-@objc requirements, only warn if the witness comes from an
  // extension.
  if (!req->isObjC() && !isa<ExtensionDecl>(witness->getDeclContext()))
    return false;

  // If the score is relatively high, don't warn: this is probably
  // unrelated.  Allow about one typo for every four properly-typed
  // characters, which prevents completely-wacky suggestions in many
  // cases.
  const unsigned reqNameLen = getNameLength(req->getName());
  const unsigned witnessNameLen = getNameLength(witness->getName());
  if (score > (std::min(reqNameLen, witnessNameLen)) / 4)
    return false;

  return true;
}

/// Diagnose a potential witness.
static void diagnosePotentialWitness(NormalProtocolConformance *conformance,
                                     ValueDecl *req, ValueDecl *witness,
                                     AccessLevel access) {
  auto proto = cast<ProtocolDecl>(req->getDeclContext());

  // Primary warning.
  witness->diagnose(diag::req_near_match, witness,
                    req->getAttrs().hasAttribute<OptionalAttr>(),
                    req->getName(), proto->getName());

  // Describe why the witness didn't satisfy the requirement.
  WitnessChecker::RequirementEnvironmentCache oneUseCache;
  auto dc = conformance->getDeclContext();
  auto match = matchWitness(oneUseCache, conformance->getProtocol(),
                            conformance, dc, req, witness);
  if (match.isWellFormed() &&
      req->isObjC() && !witness->isObjC()) {
    // Special case: note to add @objc.
    auto diag =
        witness->diagnose(diag::optional_req_nonobjc_near_match_add_objc);
    if (!witness->canInferObjCFromRequirement(req))
      fixDeclarationObjCName(diag, witness,
                             witness->getObjCRuntimeName(),
                             req->getObjCRuntimeName());
  } else {
    diagnoseMatch(conformance->getDeclContext()->getParentModule(),
                  conformance, req, match);
  }

  // If moving the declaration can help, suggest that.
  if (auto move
        = canSuppressPotentialWitnessWarningWithMovement(req, witness)) {
    witness->diagnose(diag::req_near_match_move, witness->getName(),
                      static_cast<unsigned>(*move));
  }

  // If adding 'private', 'fileprivate', or 'internal' can help, suggest that.
  if (access > AccessLevel::FilePrivate &&
      !witness->getAttrs().hasAttribute<AccessControlAttr>()) {
    witness
        ->diagnose(diag::req_near_match_access, witness->getName(), access)
        .fixItInsert(witness->getAttributeInsertionLoc(true), "private ");
  }

  // If adding @nonobjc can help, suggest that.
  if (canSuppressPotentialWitnessWarningWithNonObjC(req, witness)) {
    witness->diagnose(diag::req_near_match_nonobjc, false)
        .fixItInsert(witness->getAttributeInsertionLoc(false), "@nonobjc ");
  }

  req->diagnose(diag::kind_declname_declared_here,
                DescriptiveDeclKind::Requirement, req->getName());
}

/// Whether the given protocol is "NSCoding".
static bool isNSCoding(ProtocolDecl *protocol) {
  ASTContext &ctx = protocol->getASTContext();
  return protocol->getModuleContext()->getName() == ctx.Id_Foundation &&
    protocol->getName().str().equals("NSCoding");
}

/// Whether the given class has an explicit '@objc' name.
static bool hasExplicitObjCName(ClassDecl *classDecl) {
  // FIXME: Turn this function into a request instead of computing this
  // as part of the @objc request.
  (void) classDecl->isObjC();

  if (classDecl->getAttrs().hasAttribute<ObjCRuntimeNameAttr>())
    return true;

  auto objcAttr = classDecl->getAttrs().getAttribute<ObjCAttr>();
  if (!objcAttr)
    return false;

  return objcAttr->hasName() && !objcAttr->isNameImplicit();
}

/// Check if the name of a class might be unstable, and if so, emit a
/// diag::nscoding_unstable_mangled_name diagnostic.
static void diagnoseUnstableName(ProtocolConformance *conformance,
                                 ClassDecl *classDecl) {
  // Note: these 'kind' values are synchronized with
  // diag::nscoding_unstable_mangled_name.
  enum class UnstableNameKind : unsigned {
    Private = 0,
    FilePrivate,
    Nested,
    Local,
  };
  std::optional<UnstableNameKind> kind;
  if (!classDecl->getDeclContext()->isModuleScopeContext()) {
    if (classDecl->getDeclContext()->isTypeContext())
      kind = UnstableNameKind::Nested;
    else
      kind = UnstableNameKind::Local;
  } else {
    switch (classDecl->getFormalAccess()) {
    case AccessLevel::FilePrivate:
      kind = UnstableNameKind::FilePrivate;
      break;

    case AccessLevel::Private:
      kind = UnstableNameKind::Private;
      break;

    case AccessLevel::Internal:
    case AccessLevel::Open:
    case AccessLevel::Public:
    case AccessLevel::Package:
      break;
    }
  }

  auto &C = classDecl->getASTContext();
  if (kind && C.LangOpts.EnableNSKeyedArchiverDiagnostics &&
      isa<NormalProtocolConformance>(conformance) &&
      !hasExplicitObjCName(classDecl)) {
    C.Diags.diagnose(cast<NormalProtocolConformance>(conformance)->getLoc(),
                     diag::nscoding_unstable_mangled_name,
                     static_cast<unsigned>(kind.value()),
                     classDecl->getDeclaredInterfaceType());
    auto insertionLoc =
      classDecl->getAttributeInsertionLoc(/*forModifier=*/false);
    // Note: this is intentionally using the Swift 3 mangling,
    // to provide compatibility with archives created in the Swift 3
    // time frame.
    Mangle::ASTMangler mangler;
    std::string mangledName = mangler.mangleObjCRuntimeName(classDecl);
    assert(Lexer::isIdentifier(mangledName) &&
           "mangled name is not an identifier; can't use @objc");
    classDecl->diagnose(diag::unstable_mangled_name_add_objc)
        .fixItInsert(insertionLoc, "@objc(" + mangledName + ")");
    classDecl->diagnose(diag::unstable_mangled_name_add_objc_new)
        .fixItInsert(insertionLoc,
                     "@objc(<#prefixed Objective-C class name#>)");
  }
}

/// Infer the attribute tostatic-initialize the Objective-C metadata for the
/// given class, if needed.
static void inferStaticInitializeObjCMetadata(ClassDecl *classDecl) {
  // If we already have the attribute, there's nothing to do.
  if (classDecl->getAttrs().hasAttribute<StaticInitializeObjCMetadataAttr>())
    return;

  // If the class does not have a custom @objc name and the deployment target
  // supports the objc_getClass() hook, the workaround is unnecessary.
  ASTContext &ctx = classDecl->getASTContext();
  auto deploymentAvailability =
      AvailabilityContext::forDeploymentTarget(ctx);
  if (deploymentAvailability.isContainedIn(
        ctx.getObjCGetClassHookAvailability()) &&
      !hasExplicitObjCName(classDecl))
    return;

  // If we know that the Objective-C metadata will be statically registered,
  // there's nothing to do.
  if (!classDecl->checkAncestry(AncestryFlags::Generic)) {
    return;
  }

  // If this class isn't always available on the deployment target, don't
  // mark it as statically initialized.
  // FIXME: This is a workaround. The proper solution is for IRGen to
  // only statically initialize the Objective-C metadata when running on
  // a new-enough OS.
  if (auto sourceFile = classDecl->getParentSourceFile()) {
    AvailabilityContext safeRangeUnderApprox{
        AvailabilityInference::availableRange(classDecl, ctx)};
    AvailabilityContext runningOSOverApprox =
        AvailabilityContext::forDeploymentTarget(ctx);

    if (!runningOSOverApprox.isContainedIn(safeRangeUnderApprox))
      return;
  }

  // Infer @_staticInitializeObjCMetadata.
  classDecl->getAttrs().add(
            new (ctx) StaticInitializeObjCMetadataAttr(/*implicit=*/true));
}

static void
diagnoseMissingAppendInterpolationMethod(NominalTypeDecl *typeDecl) {
  struct InvalidMethod {
    enum class Reason : unsigned {
      ReturnType,
      AccessControl,
      Static,
    };
    
    FuncDecl *method;
    Reason reason;
    
    InvalidMethod(FuncDecl *method, Reason reason)
      : method(method), reason(reason) {}

    static bool hasValidMethod(NominalTypeDecl *typeDecl,
                               SmallVectorImpl<InvalidMethod> &invalid) {
      NLOptions subOptions = NL_QualifiedDefault;
      subOptions |= NL_ProtocolMembers;

      DeclNameRef baseName(typeDecl->getASTContext().Id_appendInterpolation);

      SmallVector<ValueDecl *, 4> lookupResults;
      typeDecl->lookupQualified(typeDecl, baseName, typeDecl->getLoc(),
                                subOptions, lookupResults);
      for (auto decl : lookupResults) {
        auto method = dyn_cast<FuncDecl>(decl);
        if (!method) continue;

        if (isa<ProtocolDecl>(method->getDeclContext()))
          continue;

        if (method->isStatic()) {
          invalid.emplace_back(method, Reason::Static);
          continue;
        }
        
        if (!method->getResultInterfaceType()->isVoid() &&
            !method->getAttrs().hasAttribute<DiscardableResultAttr>()) {
          invalid.emplace_back(method, Reason::ReturnType);
          continue;
        }
        
        if (method->getFormalAccess() < typeDecl->getFormalAccess()) {
          invalid.emplace_back(method, Reason::AccessControl);
          continue;
        }
        
        return true;
      }

      return false;
    }
  };
  
  SmallVector<InvalidMethod, 4> invalidMethods;

  if (InvalidMethod::hasValidMethod(typeDecl, invalidMethods))
    return;

  typeDecl->diagnose(diag::missing_append_interpolation);

  auto &C = typeDecl->getASTContext();
  for (auto invalidMethod : invalidMethods) {
    switch (invalidMethod.reason) {
      case InvalidMethod::Reason::Static:
        C.Diags
            .diagnose(invalidMethod.method->getStaticLoc(),
                      diag::append_interpolation_static)
            .fixItRemove(invalidMethod.method->getStaticLoc());
        break;
        
      case InvalidMethod::Reason::ReturnType:
        if (auto *const repr = invalidMethod.method->getResultTypeRepr()) {
          C.Diags
              .diagnose(repr->getLoc(),
                        diag::append_interpolation_void_or_discardable)
              .fixItInsert(invalidMethod.method->getStartLoc(),
                           "@discardableResult ");
        }
        break;
        
      case InvalidMethod::Reason::AccessControl:
        C.Diags.diagnose(invalidMethod.method,
                         diag::append_interpolation_access_control,
                         invalidMethod.method->getFormalAccess(),
                         typeDecl->getName(), typeDecl->getFormalAccess());
    }
  }
}

/// Determine whether this conformance is implied by another conformance
/// to a protocol that predated concurrency.
static bool isImpliedByConformancePredatingConcurrency(
    NormalProtocolConformance *conformance) {
  if (conformance->getSourceKind() != ConformanceEntryKind::Implied)
    return false;

  auto implied = conformance->getImplyingConformance();
  if (!implied)
    return false;

  auto impliedProto = implied->getProtocol();
  if (impliedProto->preconcurrency() ||
      impliedProto->isSpecificProtocol(KnownProtocolKind::Error) ||
      impliedProto->isSpecificProtocol(KnownProtocolKind::CodingKey))
    return true;

  // Recurse to look further.
  return isImpliedByConformancePredatingConcurrency(implied);
}

void TypeChecker::checkConformancesInContext(IterableDeclContext *idc) {
  auto *const dc = idc->getAsGenericContext();
  auto *sf = dc->getParentSourceFile();

  assert(sf != nullptr &&
         "checkConformancesInContext() should not be called on imported "
         "or deserialized DeclContexts");

  // Catch invalid extensions.
  const auto *const nominal = dc->getSelfNominalTypeDecl();
  if (!nominal)
    return;

  // Determine the access level of this conformance.
  const auto defaultAccess = nominal->getFormalAccess();

  // Check each of the conformances associated with this context.
  auto conformances = idc->getLocalConformances();

  // The conformance checker bundle that checks all conformances in the context.
  auto &Context = dc->getASTContext();
  MultiConformanceChecker groupChecker(Context);

  ProtocolConformance *SendableConformance = nullptr;
  bool hasDeprecatedUnsafeSendable = false;
  bool sendableConformancePreconcurrency = false;
  bool anyInvalid = false;
  for (auto conformance : conformances) {
    // Check and record normal conformances.
    if (auto normal = dyn_cast<NormalProtocolConformance>(conformance)) {
      groupChecker.addConformance(normal);
    }

    // Diagnose @NSCoding on file/fileprivate/nested/generic classes, which
    // have unstable archival names.
    if (auto classDecl = dc->getSelfClassDecl()) {
      if (Context.LangOpts.EnableObjCInterop &&
          isNSCoding(conformance->getProtocol()) &&
          !classDecl->isGenericContext() &&
          !classDecl->hasClangNode()) {
        diagnoseUnstableName(conformance, classDecl);
        // Infer @_staticInitializeObjCMetadata if needed.
        inferStaticInitializeObjCMetadata(classDecl);
      }
    }

    auto proto = conformance->getProtocol();
    
    if (auto kp = proto->getKnownProtocolKind()) {
      switch (*kp) {
      case KnownProtocolKind::StringInterpolationProtocol: {
        if (auto typeDecl = dc->getSelfNominalTypeDecl()) {
          diagnoseMissingAppendInterpolationMethod(typeDecl);
        }
        break;
      }
      case KnownProtocolKind::Sendable: {
        SendableConformance = conformance;

        if (auto normal = conformance->getRootNormalConformance()) {
          if (isImpliedByConformancePredatingConcurrency(normal))
            sendableConformancePreconcurrency = true;
        }
        break;
      }
      case KnownProtocolKind::DistributedActor: {
        if (auto classDecl = dyn_cast<ClassDecl>(nominal)) {
          if (!classDecl->isDistributedActor()) {
            if (classDecl->isActor()) {
              dc->getSelfNominalTypeDecl()
                  ->diagnose(diag::actor_cannot_inherit_distributed_actor_protocol,
                             dc->getSelfNominalTypeDecl()->getName())
                  .fixItInsert(classDecl->getStartLoc(), "distributed ");
            } else {
              dc->getSelfNominalTypeDecl()
                  ->diagnose(diag::distributed_actor_protocol_illegal_inheritance,
                             dc->getSelfNominalTypeDecl()->getName())
                  .fixItReplace(nominal->getStartLoc(), "distributed actor");
            }
          }
        }
        break;
      }
      case KnownProtocolKind::DistributedActorSystem: {
        checkDistributedActorSystem(nominal);
        break;
      }
      case KnownProtocolKind::Actor: {
        if (auto classDecl = dyn_cast<ClassDecl>(nominal)) {
          if (!classDecl->isExplicitActor()) {
            dc->getSelfNominalTypeDecl()
                ->diagnose(diag::actor_protocol_illegal_inheritance,
                           dc->getSelfNominalTypeDecl()->getName(),
                           proto->getName())
                .fixItReplace(nominal->getStartLoc(), "actor");
          }
        }
        break;
      }
      case KnownProtocolKind::AnyActor: {
        if (auto classDecl = dyn_cast<ClassDecl>(nominal)) {
          if (!classDecl->isExplicitActor() &&
              !classDecl->isExplicitDistributedActor()) {
            dc->getSelfNominalTypeDecl()
                ->diagnose(diag::actor_protocol_illegal_inheritance,
                           dc->getSelfNominalTypeDecl()->getName(),
                           proto->getName())
                .fixItReplace(nominal->getStartLoc(), "actor");
          }
        }
        break;
      }
      case KnownProtocolKind::UnsafeSendable: {
        hasDeprecatedUnsafeSendable = true;
        break;
      }
      case KnownProtocolKind::Executor: {
        tryDiagnoseExecutorConformance(Context, nominal, proto);
        break;
      }
      case KnownProtocolKind::Copyable: {
        checkCopyableConformance(dc, ProtocolConformanceRef(conformance));
        break;
      }
      case KnownProtocolKind::Escapable: {
        checkEscapableConformance(dc, ProtocolConformanceRef(conformance));
        break;
      }
      case KnownProtocolKind::BitwiseCopyable: {
        if (Context.LangOpts.hasFeature(Feature::BitwiseCopyable)) {
          checkBitwiseCopyableConformance(
              conformance, /*isImplicit=*/conformance->getSourceKind() ==
                               ConformanceEntryKind::Synthesized);
        }
        break;
      }
      default:
        break;
      }
    }
  }

  // Check constraints of Sendable.
  if (!hasDeprecatedUnsafeSendable && SendableConformance) {
    SendableCheck check = SendableCheck::Explicit;
    if (sendableConformancePreconcurrency)
      check = SendableCheck::ImpliedByStandardProtocol;
    else if (SendableConformance->getSourceKind() ==
                 ConformanceEntryKind::Synthesized)
      check = SendableCheck::Implicit;
    checkSendableConformance(SendableConformance, check);
  }

  // Check all conformances.
  groupChecker.checkAllConformances();

  // Check actor isolation.
  for (auto *member : idc->getMembers()) {
    if (auto *valueDecl = dyn_cast<ValueDecl>(member)) {
      (void)getActorIsolation(valueDecl);
    }
  }

  if (Context.TypeCheckerOpts.DebugGenericSignatures &&
      !conformances.empty()) {
    // Now that they're filled out, print out information about the conformances
    // here, when requested.
    llvm::errs() << "\n";
    dc->printContext(llvm::errs());
    for (auto conformance : conformances) {
      conformance->dump(llvm::errs());
      llvm::errs() << "\n";
    }
  }

  // Catalog all of members of this declaration context that satisfy
  // requirements of conformances in this context.
  SmallVector<ValueDecl *, 16>
    unsatisfiedReqs(groupChecker.getUnsatisfiedRequirements().begin(),
                    groupChecker.getUnsatisfiedRequirements().end());

  // Diagnose any conflicts attributed to this declaration context.
  for (const auto &diag : idc->takeConformanceDiagnostics()) {
    // Figure out the declaration of the existing conformance.
    Decl *existingDecl = dyn_cast<NominalTypeDecl>(diag.ExistingDC);
    if (!existingDecl)
      existingDecl = cast<ExtensionDecl>(diag.ExistingDC);

    // Complain about the redundant conformance.

    auto currentSig = dc->getGenericSignatureOfContext();
    auto existingSig = diag.ExistingDC->getGenericSignatureOfContext();
    auto differentlyConditional = currentSig && existingSig &&
                                  currentSig.getCanonicalSignature() !=
                                      existingSig.getCanonicalSignature();

    // If we've redundantly stated a conformance for which the original
    // conformance came from the module of the type or the module of the
    // protocol, just warn; we'll pick up the original conformance.
    auto existingModule = diag.ExistingDC->getParentModule();
    auto extendedNominal = diag.ExistingDC->getSelfNominalTypeDecl();
    if (existingModule != dc->getParentModule() &&
        (existingModule->getName() ==
           extendedNominal->getParentModule()->getName() ||
         existingModule == diag.Protocol->getParentModule() ||
         existingModule->getName().is("CoreGraphics"))) {
      // Warn about the conformance.
      auto diagID = differentlyConditional
                        ? diag::redundant_conformance_adhoc_conditional
                        : diag::redundant_conformance_adhoc;
      Context.Diags.diagnose(diag.Loc, diagID, dc->getDeclaredInterfaceType(),
                             diag.Protocol->getName(),
                             existingModule->getName() ==
                                 extendedNominal->getParentModule()->getName(),
                             existingModule->getName());

      // Complain about any declarations in this extension whose names match
      // a requirement in that protocol.
      SmallPtrSet<DeclName, 4> diagnosedNames;
      for (auto decl : idc->getMembers()) {
        if (decl->isImplicit())
          continue;

        auto value = dyn_cast<ValueDecl>(decl);
        if (!value) continue;

        if (!diagnosedNames.insert(value->getName()).second)
          continue;

        bool valueIsType = isa<TypeDecl>(value);
        for (auto requirement
                : diag.Protocol->lookupDirect(value->getName())) {
          if (requirement->getDeclContext() != diag.Protocol)
            continue;

          auto requirementIsType = isa<TypeDecl>(requirement);
          if (valueIsType != requirementIsType)
            continue;

          value->diagnose(diag::redundant_conformance_witness_ignored,
                          value, diag.Protocol->getName());
          break;
        }
      }
    } else {
      auto diagID = differentlyConditional
                        ? diag::redundant_conformance_conditional
                        : diag::redundant_conformance;
      Context.Diags.diagnose(diag.Loc, diagID, dc->getDeclaredInterfaceType(),
                             diag.Protocol->getName());
    }

    // Special case: explain that 'RawRepresentable' conformance
    // is implied for enums which already declare a raw type.
    if (auto enumDecl = dyn_cast<EnumDecl>(existingDecl)) {
      if (diag.Protocol->isSpecificProtocol(
              KnownProtocolKind::RawRepresentable) &&
          DerivedConformance::derivesProtocolConformance(dc, enumDecl,
                                                         diag.Protocol) &&
          enumDecl->hasRawType() &&
          enumDecl->getInherited().getStartLoc().isValid()) {
        auto inheritedLoc = enumDecl->getInherited().getStartLoc();
        Context.Diags.diagnose(
            inheritedLoc, diag::enum_declares_rawrep_with_raw_type,
            dc->getDeclaredInterfaceType(), enumDecl->getRawType());
        continue;
      }
    }

    existingDecl->diagnose(diag::declared_protocol_conformance_here,
                           dc->getDeclaredInterfaceType(),
                           static_cast<unsigned>(diag.ExistingKind),
                           diag.Protocol->getName(),
                           diag.ExistingExplicitProtocol->getName());
  }

  // If there were any unsatisfied requirements, check whether there
  // are any near-matches we should diagnose.
  if (!unsatisfiedReqs.empty() && !anyInvalid) {
    if (sf->Kind != SourceFileKind::Interface) {
      // Find all of the members that aren't used to satisfy
      // requirements, and check whether they are close to an
      // unsatisfied or defaulted requirement.
      for (auto member : idc->getMembers()) {
        // Filter out anything that couldn't satisfy one of the
        // requirements or was used to satisfy a different requirement.
        auto value = dyn_cast<ValueDecl>(member);
        if (!value) continue;
        if (isa<TypeDecl>(value)) continue;
        if (!value->getName()) continue;

        // If this declaration overrides another declaration, the signature is
        // fixed; don't complain about near misses.
        if (value->getOverriddenDecl()) continue;

        // If this member is a witness to any @objc requirement, ignore it.
        if (!findWitnessedObjCRequirements(value, /*anySingleRequirement=*/true)
              .empty())
          continue;

        // Find the unsatisfied requirements with the nearest-matching
        // names.
        SmallVector<ValueDecl *, 4> bestOptionalReqs;
        unsigned bestScore = UINT_MAX;
        for (auto req : unsatisfiedReqs) {
          // Skip unavailable requirements.
          if (req->getAttrs().isUnavailable(Context)) continue;

          // Score this particular optional requirement.
          auto score = scorePotentiallyMatching(req, value, bestScore);
          if (!score) continue;

          // If the score is better than the best we've seen, update the best
          // and clear out the list.
          if (*score < bestScore) {
            bestOptionalReqs.clear();
            bestScore = *score;
          }

          // If this score matches the (possible new) best score, record it.
          if (*score == bestScore)
            bestOptionalReqs.push_back(req);
        }

        // If we found some requirements with nearly-matching names, diagnose
        // the first one.
        if (bestScore < UINT_MAX) {
          bestOptionalReqs.erase(
            std::remove_if(
              bestOptionalReqs.begin(),
              bestOptionalReqs.end(),
              [&](ValueDecl *req) {
                return !shouldWarnAboutPotentialWitness(groupChecker, req,
                                                        value, defaultAccess,
                                                        bestScore);
              }),
            bestOptionalReqs.end());
        }

        // If we have something to complain about, do so.
        if (!bestOptionalReqs.empty()) {
          auto req = bestOptionalReqs[0];
          bool diagnosed = false;
          for (auto conformance : conformances) {
            if (conformance->getProtocol() == req->getDeclContext()) {
              diagnosePotentialWitness(conformance->getRootNormalConformance(),
                                       req, value, defaultAccess);
              diagnosed = true;
              break;
            }
          }
          assert(diagnosed && "Failed to find conformance to diagnose?");
          (void)diagnosed;

          // Remove this requirement from the list. We don't want to
          // complain about it twice.
          unsatisfiedReqs.erase(std::find(unsatisfiedReqs.begin(),
                                          unsatisfiedReqs.end(),
                                          req));
        }
      }
    }

    // For any unsatisfied optional @objc requirements that remain
    // unsatisfied, note them in the AST for @objc selector collision
    // checking.
    for (auto req : unsatisfiedReqs) {
      // Skip non-@objc requirements.
      if (!req->isObjC()) continue;

      // Skip unavailable requirements.
      if (req->getAttrs().isUnavailable(Context)) continue;

      // Record this requirement.
      if (auto funcReq = dyn_cast<AbstractFunctionDecl>(req)) {
        sf->ObjCUnsatisfiedOptReqs.emplace_back(dc, funcReq);
      } else {
        auto storageReq = cast<AbstractStorageDecl>(req);
        if (auto getter = storageReq->getParsedAccessor(AccessorKind::Get))
          sf->ObjCUnsatisfiedOptReqs.emplace_back(dc, getter);
        if (auto setter = storageReq->getParsedAccessor(AccessorKind::Set))
          sf->ObjCUnsatisfiedOptReqs.emplace_back(dc, setter);
      }
    }
  }
}

llvm::TinyPtrVector<ValueDecl *>
swift::findWitnessedObjCRequirements(const ValueDecl *witness,
                                     bool anySingleRequirement) {
  llvm::TinyPtrVector<ValueDecl *> result;

  // Types don't infer @objc this way.
  if (isa<TypeDecl>(witness)) return result;

  auto dc = witness->getDeclContext();
  auto nominal = dc->getSelfNominalTypeDecl();

  if (!nominal) return result;
  if (isa<ProtocolDecl>(nominal)) return result;

  DeclName name = witness->getName();
  std::optional<AccessorKind> accessorKind;
  if (auto *accessor = dyn_cast<AccessorDecl>(witness)) {
    accessorKind = accessor->getAccessorKind();
    switch (*accessorKind) {
    case AccessorKind::Address:
    case AccessorKind::MutableAddress:
    case AccessorKind::Read:
    case AccessorKind::Modify:
    case AccessorKind::Init:
      // These accessors are never exposed to Objective-C.
      return result;
    case AccessorKind::DidSet:
    case AccessorKind::WillSet:
      // These accessors are folded into the setter.
      return result;
    case AccessorKind::Get:
    case AccessorKind::DistributedGet:
    case AccessorKind::Set:
      // These are found relative to the main decl.
      name = accessor->getStorage()->getName();
      break;
    }
  }

  WitnessChecker::RequirementEnvironmentCache reqEnvCache;
  ASTContext &ctx = nominal->getASTContext();
  for (auto proto : nominal->getAllProtocols()) {
    // We only care about Objective-C protocols.
    if (!proto->isObjC()) continue;

    std::optional<ProtocolConformance *> conformance;
    for (auto req : proto->lookupDirect(name)) {
      // Skip anything in a protocol extension.
      if (req->getDeclContext() != proto) continue;

      // Skip types.
      if (isa<TypeDecl>(req)) continue;

      // Skip unavailable requirements.
      if (req->getAttrs().isUnavailable(ctx)) continue;

      // Dig out the conformance.
      if (!conformance.has_value()) {
        SmallVector<ProtocolConformance *, 2> conformances;
        nominal->lookupConformance(proto, conformances);
        if (conformances.size() == 1)
          conformance = conformances.front();
        else
          conformance = nullptr;
      }
      if (!*conformance) continue;

      const Decl *found = (*conformance)->getWitnessDecl(req);

      if (!found) {
        // If we have an optional requirement in an inherited conformance,
        // check whether the potential witness matches the requirement.
        // FIXME: for now, don't even try this with generics involved. We
        // should be tracking how subclasses implement optional requirements,
        // in which case the getWitness() check above would suffice.
        if (!req->getAttrs().hasAttribute<OptionalAttr>() ||
            !isa<InheritedProtocolConformance>(*conformance)) {
          continue;
        }

        auto normal = (*conformance)->getRootNormalConformance();
        auto dc = (*conformance)->getDeclContext();
        if (dc->getGenericSignatureOfContext() ||
            normal->getDeclContext()->getGenericSignatureOfContext()) {
          continue;
        }

        const ValueDecl *witnessToMatch = witness;
        if (accessorKind)
          witnessToMatch = cast<AccessorDecl>(witness)->getStorage();

        if (matchWitness(reqEnvCache, proto, normal,
                         witnessToMatch->getDeclContext(), req,
                         const_cast<ValueDecl *>(witnessToMatch))
              .isWellFormed()) {
          if (accessorKind) {
            auto *storageReq = dyn_cast<AbstractStorageDecl>(req);
            if (!storageReq)
              continue;
            req = storageReq->getOpaqueAccessor(*accessorKind);
            if (!req)
              continue;
          }
          result.push_back(req);
          if (anySingleRequirement) return result;
          continue;
        }

        continue;
      }

      // Dig out the appropriate accessor, if necessary.
      if (accessorKind) {
        auto *storageReq = dyn_cast<AbstractStorageDecl>(req);
        auto *storageFound = dyn_cast_or_null<AbstractStorageDecl>(found);
        if (!storageReq || !storageFound)
          continue;
        req = storageReq->getOpaqueAccessor(*accessorKind);
        if (!req)
          continue;
        found = storageFound->getOpaqueAccessor(*accessorKind);
      }

      // Determine whether the witness for this conformance is in fact
      // our witness.
      if (found == witness) {
        result.push_back(req);
        if (anySingleRequirement) return result;
        continue;
      }
    }
  }

  // Sort the results.
  if (result.size() > 2) {
    std::stable_sort(result.begin(), result.end(),
                     [&](ValueDecl *lhs, ValueDecl *rhs) {
                       ProtocolDecl *lhsProto
                         = cast<ProtocolDecl>(lhs->getDeclContext());
                       ProtocolDecl *rhsProto
                         = cast<ProtocolDecl>(rhs->getDeclContext());
                       return TypeDecl::compare(lhsProto, rhsProto) < 0;
                     });
  }
  return result;
}

Witness
ValueWitnessRequest::evaluate(Evaluator &eval,
                              NormalProtocolConformance *conformance,
                              ValueDecl *requirement) const {
  auto &ctx = requirement->getASTContext();
  ConformanceChecker checker(ctx, conformance);
  checker.resolveSingleWitness(requirement);

  // FIXME: ConformanceChecker and the other associated WitnessCheckers have
  // an extremely convoluted caching scheme that doesn't fit nicely into the
  // evaluator's model. All of this should be refactored away.
  const auto known = conformance->Mapping.find(requirement);
  if (known == conformance->Mapping.end()) {
    assert((!conformance->isComplete() || conformance->isInvalid()) &&
           "Resolver did not resolve requirement");
    return Witness();
  }
  return known->second;
}

namespace {
  class DefaultWitnessChecker : public WitnessChecker {
    
  public:
    DefaultWitnessChecker(ProtocolDecl *proto)
        : WitnessChecker(proto->getASTContext(), proto,
                         proto->getDeclaredInterfaceType(), proto) {}

    ResolveWitnessResult resolveWitnessViaLookup(ValueDecl *requirement);
    void recordWitness(ValueDecl *requirement, const RequirementMatch &match);
  };
} // end anonymous namespace

ResolveWitnessResult
DefaultWitnessChecker::resolveWitnessViaLookup(ValueDecl *requirement) {
  assert(!isa<AssociatedTypeDecl>(requirement) && "Must be a value witness");

  // Find the best default witness for the requirement.
  SmallVector<RequirementMatch, 4> matches;
  unsigned numViable = 0;
  unsigned bestIdx = 0;
  bool doNotDiagnoseMatches = false;

  if (findBestWitness(
                 requirement, nullptr, nullptr,
                 /* out parameters: */
                 matches, numViable, bestIdx, doNotDiagnoseMatches)) {

    auto &best = matches[bestIdx];

    // Perform the same checks as conformance witness matching, but silently
    // ignore the candidate instead of diagnosing anything.
    auto check = checkWitness(requirement, best);
    if (check.Kind != CheckKind::Success)
      return ResolveWitnessResult::ExplicitFailed;

    // Record the match.
    recordWitness(requirement, best);
    return ResolveWitnessResult::Success;
  }

  // We have either no matches or an ambiguous match.
  return ResolveWitnessResult::Missing;
}

void DefaultWitnessChecker::recordWitness(
                                  ValueDecl *requirement,
                                  const RequirementMatch &match) {
  Proto->setDefaultWitness(requirement, match.getWitness(getASTContext()));
}

void TypeChecker::inferDefaultWitnesses(ProtocolDecl *proto) {
  DefaultWitnessChecker checker(proto);

  // Find the default for the given associated type.
  auto findAssociatedTypeDefault = [proto](AssociatedTypeDecl *assocType)
      -> std::pair<Type, AssociatedTypeDecl *> {
    auto defaultedAssocType = findDefaultedAssociatedType(proto, proto, assocType);
    if (!defaultedAssocType)
      return {Type(), nullptr};

    Type defaultType = defaultedAssocType->getDefaultDefinitionType();
    if (!defaultType)
      return {Type(), nullptr};

    return {defaultType, defaultedAssocType};
  };

  for (auto *requirement : proto->getProtocolRequirements()) {
    if (requirement->isInvalid())
      continue;

    if (auto assocType = dyn_cast<AssociatedTypeDecl>(requirement)) {
      if (assocType->getOverriddenDecls().empty()) {
        if (Type defaultType = findAssociatedTypeDefault(assocType).first)
          proto->setDefaultTypeWitness(assocType, defaultType);
      }

      continue;
    }

    assert(!isa<TypeDecl>(requirement));

    ResolveWitnessResult result = checker.resolveWitnessViaLookup(requirement);

    if (result == ResolveWitnessResult::Missing &&
        requirement->isSPI() &&
        !proto->isSPI()) {
      // SPI requirements need a default value, unless the protocol is SPI too.
      requirement->diagnose(diag::spi_attribute_on_protocol_requirement,
                            requirement);
    }
  }

  // Find defaults for any associated conformances rooted on defaulted
  // associated types.
  for (const auto &req : proto->getRequirementSignature().getRequirements()) {
    if (req.getKind() != RequirementKind::Conformance)
      continue;
    if (req.getFirstType()->isEqual(proto->getSelfInterfaceType()))
      continue;

    // Find the innermost dependent member type (e.g., Self.AssocType), so
    // we can look at the associated type.
    auto depMemTy = req.getFirstType()->getAs<DependentMemberType>();
    if (!depMemTy)
      continue;

    while (auto innerDepMemTy =
             depMemTy->getBase()->getAs<DependentMemberType>())
      depMemTy = innerDepMemTy;

    if (!depMemTy->getBase()->isEqual(proto->getSelfInterfaceType()))
      continue;

    auto assocType = depMemTy->getAssocType();
    if (!assocType)
      continue;

    auto *module = proto->getParentModule();

    // Find the associated type nearest our own protocol, which might have
    // a default not available in the associated type referenced by the
    // (canonicalized) requirement.
    if (assocType->getProtocol() != proto) {
      SmallVector<ValueDecl *, 2> found;
      module->lookupQualified(
                           proto, DeclNameRef(assocType->getName()),
                           proto->getLoc(),
                           NL_QualifiedDefault|NL_ProtocolMembers|NL_OnlyTypes,
                           found);
      if (found.size() == 1 && isa<AssociatedTypeDecl>(found[0]))
        assocType = cast<AssociatedTypeDecl>(found[0]);
    }

    // Dig out the default associated type definition.
    AssociatedTypeDecl *defaultedAssocDecl = nullptr;
    Type defaultAssocType;
    std::tie(defaultAssocType, defaultedAssocDecl) =
        findAssociatedTypeDefault(assocType);
    if (!defaultAssocType)
      continue;

    Type defaultAssocTypeInContext =
      proto->mapTypeIntoContext(defaultAssocType);
    auto requirementProto = req.getProtocolDecl();
    auto conformance = module->checkConformance(defaultAssocTypeInContext,
                                                requirementProto);
    if (conformance.isInvalid()) {
      // Diagnose the lack of a conformance. This is potentially an ABI
      // incompatibility.
      proto->diagnose(diag::assoc_type_default_conformance_failed,
                      defaultAssocType, assocType,
                      req.getFirstType(), req.getSecondType());
      defaultedAssocDecl
          ->diagnose(diag::assoc_type_default_here, assocType, defaultAssocType)
          .highlight(defaultedAssocDecl->getDefaultDefinitionTypeRepr()
                         ->getSourceRange());

      continue;
    }

    // Record the default associated conformance.
    proto->setDefaultAssociatedConformanceWitness(
        req.getFirstType()->getCanonicalType(), requirementProto, conformance);
  }
}
