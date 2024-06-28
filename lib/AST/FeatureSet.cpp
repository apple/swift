//===--- FeatureSet.cpp - Language feature support --------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "FeatureSet.h"

#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "clang/AST/DeclObjC.h"
#include "swift/Basic/Assertions.h"

using namespace swift;

/// Does the interface of this declaration use a type for which the
/// given predicate returns true?
static bool usesTypeMatching(Decl *decl, llvm::function_ref<bool(Type)> fn) {
  if (auto value = dyn_cast<ValueDecl>(decl)) {
    if (Type type = value->getInterfaceType()) {
      return type.findIf(fn);
    }
  }

  return false;
}

// ----------------------------------------------------------------------------
// MARK: - Standard Features
// ----------------------------------------------------------------------------

/// Functions to determine which features a particular declaration uses. The
/// usesFeatureNNN functions correspond to the features in Features.def.

#define BASELINE_LANGUAGE_FEATURE(FeatureName, SENumber, Description)          \
  static bool usesFeature##FeatureName(Decl *decl) { return false; }
#define LANGUAGE_FEATURE(FeatureName, SENumber, Description)
#include "swift/Basic/Features.def"

#define UNINTERESTING_FEATURE(FeatureName)                                     \
  static bool usesFeature##FeatureName(Decl *decl) { return false; }

static bool usesFeatureRethrowsProtocol(Decl *decl,
                                        SmallPtrSet<Decl *, 16> &checked) {
  // Make sure we don't recurse.
  if (!checked.insert(decl).second)
    return false;

  // Check an inheritance clause for a marker protocol.
  auto checkInherited = [&](InheritedTypes inherited) -> bool {
    for (unsigned i : inherited.getIndices()) {
      if (auto inheritedType = inherited.getResolvedType(i)) {
        if (inheritedType->isExistentialType()) {
          auto layout = inheritedType->getExistentialLayout();
          for (ProtocolDecl *proto : layout.getProtocols()) {
            if (usesFeatureRethrowsProtocol(proto, checked))
              return true;
          }
        }
      }
    }

    return false;
  };

  if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
    if (checkInherited(nominal->getInherited()))
      return true;
  }

  if (auto proto = dyn_cast<ProtocolDecl>(decl)) {
    if (proto->getAttrs().hasAttribute<AtRethrowsAttr>())
      return true;
  }

  if (auto ext = dyn_cast<ExtensionDecl>(decl)) {
    if (auto nominal = ext->getSelfNominalTypeDecl())
      if (usesFeatureRethrowsProtocol(nominal, checked))
        return true;

    if (checkInherited(ext->getInherited()))
      return true;
  }

  if (auto genericSig =
          decl->getInnermostDeclContext()->getGenericSignatureOfContext()) {
    for (const auto &req : genericSig.getRequirements()) {
      if (req.getKind() == RequirementKind::Conformance &&
          usesFeatureRethrowsProtocol(req.getProtocolDecl(), checked))
        return true;
    }
  }

  if (auto value = dyn_cast<ValueDecl>(decl)) {
    if (Type type = value->getInterfaceType()) {
      bool hasRethrowsProtocol = type.findIf([&](Type type) {
        if (auto nominal = type->getAnyNominal()) {
          if (usesFeatureRethrowsProtocol(nominal, checked))
            return true;
        }

        return false;
      });

      if (hasRethrowsProtocol)
        return true;
    }
  }

  return false;
}

static bool usesFeatureRethrowsProtocol(Decl *decl) {
  SmallPtrSet<Decl *, 16> checked;
  return usesFeatureRethrowsProtocol(decl, checked);
}

UNINTERESTING_FEATURE(BuiltinBuildTaskExecutorRef)
UNINTERESTING_FEATURE(BuiltinBuildComplexEqualityExecutor)
UNINTERESTING_FEATURE(BuiltinCreateAsyncTaskInGroupWithExecutor)
UNINTERESTING_FEATURE(BuiltinCreateAsyncDiscardingTaskInGroup)
UNINTERESTING_FEATURE(BuiltinCreateAsyncDiscardingTaskInGroupWithExecutor)
UNINTERESTING_FEATURE(BuiltinUnprotectedStackAlloc)
UNINTERESTING_FEATURE(BuiltinAllocVector)
UNINTERESTING_FEATURE(BuiltinCreateTask)

static bool usesFeatureNewCxxMethodSafetyHeuristics(Decl *decl) {
  return decl->hasClangNode();
}

static bool usesFeatureSpecializeAttributeWithAvailability(Decl *decl) {
  if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
    for (auto specialize : func->getAttrs().getAttributes<SpecializeAttr>()) {
      if (!specialize->getAvailableAttrs().empty())
        return true;
    }
  }
  return false;
}

static bool usesFeaturePrimaryAssociatedTypes2(Decl *decl) {
  if (auto *protoDecl = dyn_cast<ProtocolDecl>(decl)) {
    if (protoDecl->getPrimaryAssociatedTypes().size() > 0)
      return true;
  }

  return false;
}

static bool usesFeatureAssociatedTypeAvailability(Decl *decl) {
  return isa<AssociatedTypeDecl>(decl) &&
         decl->getAttrs().hasAttribute<AvailableAttr>();
}

static bool isImplicitRethrowsProtocol(const ProtocolDecl *proto) {
  return proto->isSpecificProtocol(KnownProtocolKind::AsyncSequence) ||
         proto->isSpecificProtocol(KnownProtocolKind::AsyncIteratorProtocol);
}

static bool usesFeatureAsyncSequenceFailure(Decl *decl) {
  if (auto proto = dyn_cast<ProtocolDecl>(decl)) {
    return isImplicitRethrowsProtocol(proto);
  }

  return false;
}

static bool usesFeatureMacros(Decl *decl) { return isa<MacroDecl>(decl); }

static bool usesFeatureFreestandingExpressionMacros(Decl *decl) {
  auto macro = dyn_cast<MacroDecl>(decl);
  if (!macro)
    return false;

  return macro->getMacroRoles().contains(MacroRole::Expression);
}

static bool usesFeatureAttachedMacros(Decl *decl) {
  auto macro = dyn_cast<MacroDecl>(decl);
  if (!macro)
    return false;

  return static_cast<bool>(macro->getMacroRoles() & getAttachedMacroRoles());
}

static bool usesFeatureExtensionMacros(Decl *decl) {
  auto macro = dyn_cast<MacroDecl>(decl);
  if (!macro)
    return false;

  return macro->getMacroRoles().contains(MacroRole::Extension);
}

static bool usesFeatureMoveOnly(Decl *decl) {
  if (auto *extension = dyn_cast<ExtensionDecl>(decl)) {
    if (auto *nominal = extension->getExtendedNominal())
      return usesFeatureMoveOnly(nominal);
    return false;
  }

  auto hasInverseInType = [&](Type type) {
    return type.findIf([&](Type type) -> bool {
      if (auto *NTD = type->getAnyNominal()) {
        if (NTD->getAttrs().hasAttribute<MoveOnlyAttr>())
          return true;
      }
      return false;
    });
  };

  if (auto *TD = dyn_cast<TypeDecl>(decl)) {
    if (auto *alias = dyn_cast<TypeAliasDecl>(TD))
      return hasInverseInType(alias->getUnderlyingType());

    if (auto *NTD = dyn_cast<NominalTypeDecl>(TD)) {
      if (NTD->getAttrs().hasAttribute<MoveOnlyAttr>())
        return true;
    }

    return false;
  }

  if (auto *VD = dyn_cast<ValueDecl>(decl)) {
    return hasInverseInType(VD->getInterfaceType());
  }

  return false;
}

static bool usesFeatureMoveOnlyResilientTypes(Decl *decl) {
  if (auto *nomDecl = dyn_cast<NominalTypeDecl>(decl))
    return nomDecl->isResilient() && usesFeatureMoveOnly(decl);
  return false;
}

static bool hasParameterPacks(Decl *decl) {
  if (auto genericContext = decl->getAsGenericContext()) {
    auto sig = genericContext->getGenericSignature();
    if (llvm::any_of(sig.getGenericParams(),
                     [&](const GenericTypeParamType *GP) {
                       return GP->isParameterPack();
                     })) {
      return true;
    }
  }

  return false;
}

/// A declaration needs the $ParameterPacks feature if it declares a
/// generic parameter pack, or if its type references a generic nominal
/// or type alias which declares a generic parameter pack.
static bool usesFeatureParameterPacks(Decl *decl) {
  if (hasParameterPacks(decl))
    return true;

  if (auto *valueDecl = dyn_cast<ValueDecl>(decl)) {
    if (valueDecl->getInterfaceType().findIf([&](Type t) {
          if (auto *alias = dyn_cast<TypeAliasType>(t.getPointer()))
            return hasParameterPacks(alias->getDecl());
          if (auto *nominal = t->getAnyNominal())
            return hasParameterPacks(nominal);

          return false;
        })) {
      return true;
    }
  }

  return false;
}

static bool usesFeatureLexicalLifetimes(Decl *decl) {
  return decl->getAttrs().hasAttribute<EagerMoveAttr>() ||
         decl->getAttrs().hasAttribute<NoEagerMoveAttr>() ||
         decl->getAttrs().hasAttribute<LexicalLifetimesAttr>();
}

static bool usesFeatureFreestandingMacros(Decl *decl) {
  auto macro = dyn_cast<MacroDecl>(decl);
  if (!macro)
    return false;

  return macro->getMacroRoles().contains(MacroRole::Declaration);
}

static bool usesFeatureRetroactiveAttribute(Decl *decl) {
  auto ext = dyn_cast<ExtensionDecl>(decl);
  if (!ext)
    return false;

  return llvm::any_of(
      ext->getInherited().getEntries(),
      [](const InheritedEntry &entry) { return entry.isRetroactive(); });
}

static bool usesFeatureExtensionMacroAttr(Decl *decl) {
  return usesFeatureExtensionMacros(decl);
}

static bool usesFeatureTypedThrows(Decl *decl) {
  if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
    return usesTypeMatching(decl, [](Type ty) {
      if (auto funcType = ty->getAs<AnyFunctionType>())
        return funcType->hasThrownError();

      return false;
    });
  }

  return false;
}

static bool usesFeatureOptionalIsolatedParameters(Decl *decl) {
  auto *value = dyn_cast<ValueDecl>(decl);
  if (!value)
    return false;

  auto *paramList = getParameterList(value);
  if (!paramList)
    return false;

  for (auto param : *paramList) {
    if (param->isIsolated()) {
      auto paramType = param->getInterfaceType();
      return !paramType->getOptionalObjectType().isNull();
    }
  }

  return false;
}

static bool usesFeatureExtern(Decl *decl) {
  return decl->getAttrs().hasAttribute<ExternAttr>();
}

static bool usesFeatureAssociatedTypeImplements(Decl *decl) {
  return isa<TypeDecl>(decl) && decl->getAttrs().hasAttribute<ImplementsAttr>();
}

static bool usesFeatureExpressionMacroDefaultArguments(Decl *decl) {
  if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
    for (auto param : *func->getParameters()) {
      if (param->getDefaultArgumentKind() ==
          DefaultArgumentKind::ExpressionMacro)
        return true;
    }
  }

  return false;
}

UNINTERESTING_FEATURE(BuiltinStoreRaw)
UNINTERESTING_FEATURE(BuiltinAddressOfRawLayout)

// ----------------------------------------------------------------------------
// MARK: - Upcoming Features
// ----------------------------------------------------------------------------

UNINTERESTING_FEATURE(ConciseMagicFile)
UNINTERESTING_FEATURE(ForwardTrailingClosures)
UNINTERESTING_FEATURE(StrictConcurrency)
UNINTERESTING_FEATURE(BareSlashRegexLiterals)
UNINTERESTING_FEATURE(DeprecateApplicationMain)

static bool usesFeatureImportObjcForwardDeclarations(Decl *decl) {
  ClangNode clangNode = decl->getClangNode();
  if (!clangNode)
    return false;

  const clang::Decl *clangDecl = clangNode.getAsDecl();
  if (!clangDecl)
    return false;

  if (auto objCInterfaceDecl = dyn_cast<clang::ObjCInterfaceDecl>(clangDecl))
    return !objCInterfaceDecl->hasDefinition();

  if (auto objCProtocolDecl = dyn_cast<clang::ObjCProtocolDecl>(clangDecl))
    return !objCProtocolDecl->hasDefinition();

  return false;
}

UNINTERESTING_FEATURE(DisableOutwardActorInference)
UNINTERESTING_FEATURE(InternalImportsByDefault)
UNINTERESTING_FEATURE(IsolatedDefaultValues)
UNINTERESTING_FEATURE(GlobalConcurrency)
UNINTERESTING_FEATURE(FullTypedThrows)
UNINTERESTING_FEATURE(ExistentialAny)
UNINTERESTING_FEATURE(InferSendableFromCaptures)
UNINTERESTING_FEATURE(ImplicitOpenExistentials)

// ----------------------------------------------------------------------------
// MARK: - Experimental Features
// ----------------------------------------------------------------------------

UNINTERESTING_FEATURE(StaticAssert)
UNINTERESTING_FEATURE(NamedOpaqueTypes)
UNINTERESTING_FEATURE(FlowSensitiveConcurrencyCaptures)

static bool usesFeatureCodeItemMacros(Decl *decl) {
  auto macro = dyn_cast<MacroDecl>(decl);
  if (!macro)
    return false;

  return macro->getMacroRoles().contains(MacroRole::CodeItem);
}

UNINTERESTING_FEATURE(BodyMacros)
UNINTERESTING_FEATURE(PreambleMacros)
UNINTERESTING_FEATURE(TupleConformances)

static bool usesFeatureSymbolLinkageMarkers(Decl *decl) {
  auto &attrs = decl->getAttrs();
  return std::any_of(attrs.begin(), attrs.end(), [](auto *attr) {
    if (isa<UsedAttr>(attr))
      return true;
    if (isa<SectionAttr>(attr))
      return true;
    return false;
  });
}

UNINTERESTING_FEATURE(LazyImmediate)

static bool usesFeatureMoveOnlyClasses(Decl *decl) {
  return isa<ClassDecl>(decl) && usesFeatureMoveOnly(decl);
}

static bool usesFeatureNoImplicitCopy(Decl *decl) {
  return decl->isNoImplicitCopy();
}

UNINTERESTING_FEATURE(OldOwnershipOperatorSpellings)

static bool usesFeatureMoveOnlyEnumDeinits(Decl *decl) {
  if (auto *ei = dyn_cast<EnumDecl>(decl)) {
    return usesFeatureMoveOnly(ei) && ei->getValueTypeDestructor();
  }
  return false;
}

UNINTERESTING_FEATURE(MoveOnlyTuples)

// Partial consumption does not affect declarations directly.
UNINTERESTING_FEATURE(MoveOnlyPartialConsumption)

UNINTERESTING_FEATURE(MoveOnlyPartialReinitialization)

UNINTERESTING_FEATURE(OneWayClosureParameters)

static bool usesFeatureLayoutPrespecialization(Decl *decl) {
  auto &attrs = decl->getAttrs();
  return std::any_of(attrs.begin(), attrs.end(), [](auto *attr) {
    if (auto *specialize = dyn_cast<SpecializeAttr>(attr)) {
      return !specialize->getTypeErasedParams().empty();
    }
    return false;
  });
}

UNINTERESTING_FEATURE(AccessLevelOnImport)
UNINTERESTING_FEATURE(AllowNonResilientAccessInPackage)
UNINTERESTING_FEATURE(ClientBypassResilientAccessInPackage)
UNINTERESTING_FEATURE(LayoutStringValueWitnesses)
UNINTERESTING_FEATURE(LayoutStringValueWitnessesInstantiation)
UNINTERESTING_FEATURE(DifferentiableProgramming)
UNINTERESTING_FEATURE(ForwardModeDifferentiation)
UNINTERESTING_FEATURE(AdditiveArithmeticDerivedConformances)
UNINTERESTING_FEATURE(SendableCompletionHandlers)
UNINTERESTING_FEATURE(OpaqueTypeErasure)
UNINTERESTING_FEATURE(PackageCMO)
UNINTERESTING_FEATURE(ParserRoundTrip)
UNINTERESTING_FEATURE(ParserValidation)
UNINTERESTING_FEATURE(ParserDiagnostics)
UNINTERESTING_FEATURE(ImplicitSome)
UNINTERESTING_FEATURE(ParserASTGen)
UNINTERESTING_FEATURE(BuiltinMacros)
UNINTERESTING_FEATURE(ImportSymbolicCXXDecls)
UNINTERESTING_FEATURE(GenerateBindingsForThrowingFunctionsInCXX)

static bool usesFeatureReferenceBindings(Decl *decl) {
  auto *vd = dyn_cast<VarDecl>(decl);
  return vd && vd->getIntroducer() == VarDecl::Introducer::InOut;
}

UNINTERESTING_FEATURE(BuiltinModule)
UNINTERESTING_FEATURE(RegionBasedIsolation)
UNINTERESTING_FEATURE(PlaygroundExtendedCallbacks)
UNINTERESTING_FEATURE(ThenStatements)
UNINTERESTING_FEATURE(DoExpressions)
UNINTERESTING_FEATURE(ImplicitLastExprResults)

static bool usesFeatureRawLayout(Decl *decl) {
  return decl->getAttrs().hasAttribute<RawLayoutAttr>();
}

UNINTERESTING_FEATURE(Embedded)
UNINTERESTING_FEATURE(Volatile)
UNINTERESTING_FEATURE(SuppressedAssociatedTypes)

static bool disallowFeatureSuppression(StringRef featureName, Decl *decl);

static bool allSubstTypesAreCopyable(Type type, DeclContext *context) {
  assert(type->getAnyNominal());
  auto bgt = type->getAs<BoundGenericType>();
  if (!bgt)
    return false;  // nothing is bound.

  for (auto argInterfaceTy : bgt->getGenericArgs())
    if (context->mapTypeIntoContext(argInterfaceTy)->isNoncopyable())
      return false;

  return true;
}

static bool usesFeatureNoncopyableGenerics(Decl *decl) {
  if (decl->getAttrs().hasAttribute<PreInverseGenericsAttr>())
    return true;

  if (auto *valueDecl = dyn_cast<ValueDecl>(decl)) {
    if (auto proto = dyn_cast<ProtocolDecl>(decl)) {
      auto reqSig = proto->getRequirementSignature();

      SmallVector<Requirement, 2> reqs;
      SmallVector<InverseRequirement, 2> inverses;
      reqSig.getRequirementsWithInverses(proto, reqs, inverses);
      if (!inverses.empty())
        return true;
    }

    if (isa<AbstractFunctionDecl>(valueDecl) ||
        isa<AbstractStorageDecl>(valueDecl)) {
      auto *context = decl->getInnermostDeclContext();
      auto usesFeature = valueDecl->getInterfaceType().findIf(
          [&](Type type) -> bool {
        auto *nominalDecl = type->getAnyNominal();
        if (!nominalDecl || !isa<StructDecl, EnumDecl, ClassDecl>(nominalDecl))
          return false;

        if (!usesFeatureNoncopyableGenerics(nominalDecl))
          return false;

        // If we only _refer_ to a TypeDecl that uses NoncopyableGenerics,
        // and a suppressed version of that decl is in the interface, and
        // if we only substitute Copyable types for the generic parameters,
        // then we can say this decl is not "using" the feature such that
        // a feature guard is required. In other words, this reference to the
        // type will always be valid, regardless of whether the feature is
        // enabled or not. (rdar://127389991)
        if (!disallowFeatureSuppression("NoncopyableGenerics", nominalDecl)
            && allSubstTypesAreCopyable(type, context)) {
          return false;
        }

        return true;
      });
      if (usesFeature)
        return true;
    }
  }

  if (auto *ext = dyn_cast<ExtensionDecl>(decl)) {
    if (auto *nominal = ext->getExtendedNominal())
      if (usesFeatureNoncopyableGenerics(nominal))
        return true;
  }

  SmallVector<Requirement, 2> reqs;
  SmallVector<InverseRequirement, 2> inverseReqs;

  if (auto *proto = dyn_cast<ProtocolDecl>(decl)) {

    // We have baked-in support for Sendable not needing to state inverses
    // in its inheritance clause within interface files. So, it technically is
    // not using the feature with respect to the concerns of an interface file.
    if (proto->isSpecificProtocol(KnownProtocolKind::Sendable))
      return false;

    proto->getRequirementSignature().getRequirementsWithInverses(
        proto, reqs, inverseReqs);
  } else if (auto *genCtx = decl->getAsGenericContext()) {
    if (auto genericSig = genCtx->getGenericSignature())
      genericSig->getRequirementsWithInverses(reqs, inverseReqs);
  }

  return !inverseReqs.empty();
}

UNINTERESTING_FEATURE(NoncopyableGenerics2)

static bool usesFeatureStructLetDestructuring(Decl *decl) {
  auto sd = dyn_cast<StructDecl>(decl);
  if (!sd)
    return false;

  for (auto member : sd->getStoredProperties()) {
    if (!member->isLet())
      continue;

    auto init = member->getParentPattern();
    if (!init)
      continue;

    if (!init->getSingleVar())
      return true;
  }

  return false;
}

static bool usesFeatureNonescapableTypes(Decl *decl) {
  if (decl->getAttrs().hasAttribute<NonEscapableAttr>() ||
      decl->getAttrs().hasAttribute<UnsafeNonEscapableResultAttr>()) {
    return true;
  }
  return false;
}

static bool usesFeatureStaticExclusiveOnly(Decl *decl) {
  return decl->getAttrs().hasAttribute<StaticExclusiveOnlyAttr>();
}

static bool usesFeatureExtractConstantsFromMembers(Decl *decl) {
  return decl->getAttrs().hasAttribute<ExtractConstantsFromMembersAttr>();
}

UNINTERESTING_FEATURE(BitwiseCopyable)
UNINTERESTING_FEATURE(FixedArrays)
UNINTERESTING_FEATURE(GroupActorErrors)

UNINTERESTING_FEATURE(TransferringArgsAndResults)
static bool usesFeatureSendingArgsAndResults(Decl *decl) {
  auto isFunctionTypeWithSending = [](Type type) {
      auto fnType = type->getAs<AnyFunctionType>();
      if (!fnType)
        return false;

      if (fnType->hasExtInfo() && fnType->hasSendingResult())
        return true;

      return llvm::any_of(fnType->getParams(),
                          [](AnyFunctionType::Param param) {
                            return param.getParameterFlags().isSending();
                          });
  };
  auto declUsesFunctionTypesThatUseSending = [&](Decl *decl) {
    return usesTypeMatching(decl, isFunctionTypeWithSending);
  };

  if (auto *pd = dyn_cast<ParamDecl>(decl)) {
    if (pd->isSending()) {
      return true;
    }

    if (declUsesFunctionTypesThatUseSending(pd))
      return true;
  }

  if (auto *fDecl = dyn_cast<AbstractFunctionDecl>(decl)) {
    // First check for param decl results.
    if (llvm::any_of(fDecl->getParameters()->getArray(), [](ParamDecl *pd) {
          return usesFeatureSendingArgsAndResults(pd);
        }))
      return true;
    if (declUsesFunctionTypesThatUseSending(decl))
      return true;
  }

  // Check if we have a pattern binding decl for a function that has sending
  // parameters and results.
  if (auto *pbd = dyn_cast<PatternBindingDecl>(decl)) {
    for (auto index : range(pbd->getNumPatternEntries())) {
      auto *pattern = pbd->getPattern(index);
      if (pattern->hasType() && isFunctionTypeWithSending(pattern->getType()))
        return true;
    }
  }

  return false;
}

UNINTERESTING_FEATURE(DynamicActorIsolation)

UNINTERESTING_FEATURE(NonfrozenEnumExhaustivity)

UNINTERESTING_FEATURE(BorrowingSwitch)

UNINTERESTING_FEATURE(ClosureIsolation)

static bool usesFeatureConformanceSuppression(Decl *decl) {
  auto *nominal = dyn_cast<NominalTypeDecl>(decl);
  if (!nominal)
    return false;

  auto inherited = InheritedTypes(nominal);
  for (auto index : indices(inherited.getEntries())) {
    // Ensure that InheritedTypeRequest has set the isSuppressed bit if
    // appropriate.
    auto resolvedTy = inherited.getResolvedType(index);
    (void)resolvedTy;

    auto entry = inherited.getEntry(index);

    if (!entry.isSuppressed())
      continue;

    auto ty = entry.getType();

    if (!ty)
      continue;

    auto kp = ty->getKnownProtocol();
    if (!kp)
      continue;

    auto rpk = getRepressibleProtocolKind(*kp);
    if (!rpk)
      continue;

    return true;
  }

  return false;
}

static bool usesFeatureBitwiseCopyable2(Decl *decl) {
  if (!decl->getModuleContext()->isStdlibModule()) {
    return false;
  }
  if (auto *proto = dyn_cast<ProtocolDecl>(decl)) {
    return proto->getNameStr() == "BitwiseCopyable";
  }
  if (auto *typealias = dyn_cast<TypeAliasDecl>(decl)) {
    return typealias->getNameStr() == "_BitwiseCopyable";
  }
  return false;
}

static bool usesFeatureIsolatedAny(Decl *decl) {
  return usesTypeMatching(decl, [](Type type) {
    if (auto fnType = type->getAs<AnyFunctionType>()) {
      return fnType->getIsolation().isErased();
    }
    return false;
  });
}

UNINTERESTING_FEATURE(MemberImportVisibility)
UNINTERESTING_FEATURE(IsolatedAny2)

static bool usesFeatureGlobalActorIsolatedTypesUsability(Decl *decl) {
  return false;
}

UNINTERESTING_FEATURE(ObjCImplementation)
UNINTERESTING_FEATURE(ObjCImplementationWithResilientStorage)
UNINTERESTING_FEATURE(CImplementation)

static bool usesFeatureSensitive(Decl *decl) {
  return decl->getAttrs().hasAttribute<SensitiveAttr>();
}

UNINTERESTING_FEATURE(DebugDescriptionMacro)
UNINTERESTING_FEATURE(ReinitializeConsumeInMultiBlockDefer)
UNINTERESTING_FEATURE(SE427NoInferenceOnExtension)

// ----------------------------------------------------------------------------
// MARK: - FeatureSet
// ----------------------------------------------------------------------------

void FeatureSet::collectRequiredFeature(Feature feature,
                                        InsertOrRemove operation) {
  required.insertOrRemove(feature, operation == Insert);
}

void FeatureSet::collectSuppressibleFeature(Feature feature,
                                            InsertOrRemove operation) {
  suppressible.insertOrRemove(numFeatures() - size_t(feature),
                              operation == Insert);
}

static bool hasFeatureSuppressionAttribute(Decl *decl, StringRef featureName,
                                           bool inverted) {
  auto attr = decl->getAttrs().getAttribute<AllowFeatureSuppressionAttr>();
  if (!attr)
    return false;

  if (attr->getInverted() != inverted)
    return false;

  for (auto suppressedFeature : attr->getSuppressedFeatures()) {
    if (suppressedFeature.is(featureName))
      return true;
  }

  return false;
}

static bool disallowFeatureSuppression(StringRef featureName, Decl *decl) {
  return hasFeatureSuppressionAttribute(decl, featureName, true);
}

static bool allowFeatureSuppression(StringRef featureName, Decl *decl) {
  return hasFeatureSuppressionAttribute(decl, featureName, false);
}

/// Go through all the features used by the given declaration and
/// either add or remove them to this set.
void FeatureSet::collectFeaturesUsed(Decl *decl, InsertOrRemove operation) {
  // Go through each of the features, checking whether the
  // declaration uses that feature.
#define LANGUAGE_FEATURE(FeatureName, SENumber, Description)                   \
  if (usesFeature##FeatureName(decl))                                          \
    collectRequiredFeature(Feature::FeatureName, operation);
#define SUPPRESSIBLE_LANGUAGE_FEATURE(FeatureName, SENumber, Description)      \
  if (usesFeature##FeatureName(decl)) {                                        \
    if (disallowFeatureSuppression(#FeatureName, decl))                        \
      collectRequiredFeature(Feature::FeatureName, operation);                 \
    else                                                                       \
      collectSuppressibleFeature(Feature::FeatureName, operation);             \
  }
#define CONDITIONALLY_SUPPRESSIBLE_LANGUAGE_FEATURE(FeatureName, SENumber, Description)      \
  if (usesFeature##FeatureName(decl)) {                                        \
    if (allowFeatureSuppression(#FeatureName, decl))                           \
      collectSuppressibleFeature(Feature::FeatureName, operation);             \
    else                                                                       \
      collectRequiredFeature(Feature::FeatureName, operation);                 \
  }
#include "swift/Basic/Features.def"
}

FeatureSet swift::getUniqueFeaturesUsed(Decl *decl) {
  // Add all the features used by this declaration.
  FeatureSet features;
  features.collectFeaturesUsed(decl, FeatureSet::Insert);

  // Remove all the features used by all enclosing declarations.
  Decl *enclosingDecl = decl;
  while (!features.empty()) {
    // Find the next outermost enclosing declaration.
    if (auto accessor = dyn_cast<AccessorDecl>(enclosingDecl))
      enclosingDecl = accessor->getStorage();
    else
      enclosingDecl = enclosingDecl->getDeclContext()->getAsDecl();
    if (!enclosingDecl)
      break;

    features.collectFeaturesUsed(enclosingDecl, FeatureSet::Remove);
  }

  return features;
}
