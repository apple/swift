//===--- SILFunctionBuilder.cpp -------------------------------------------===//
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

#include "swift/SIL/SILFunctionBuilder.h"
#include "swift/AST/AttrKind.h"
#include "swift/AST/Availability.h"
#include "swift/AST/DiagnosticsParse.h"
#include "swift/AST/DistributedDecl.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/SemanticAttrs.h"

using namespace swift;

SILFunction *SILFunctionBuilder::getOrCreateFunction(
    SILLocation loc, StringRef name, SILLinkage linkage, CanSILFunctionType type, IsBare_t isBareSILFunction,
    IsTransparent_t isTransparent, IsSerialized_t isSerialized,
    IsDynamicallyReplaceable_t isDynamic, IsDistributed_t isDistributed,
    IsRuntimeAccessible_t isRuntimeAccessible, ProfileCounter entryCount,
    IsThunk_t isThunk, SubclassScope subclassScope) {
  assert(!type->isNoEscape() && "Function decls always have escaping types.");
  if (auto fn = mod.lookUpFunction(name)) {
    assert(fn->getLoweredFunctionType() == type);
    assert(stripExternalFromLinkage(fn->getLinkage()) ==
           stripExternalFromLinkage(linkage));
    return fn;
  }

  auto fn = SILFunction::create(mod, linkage, name, type, nullptr, loc,
                                isBareSILFunction, isTransparent, isSerialized,
                                entryCount, isDynamic, isDistributed,
                                isRuntimeAccessible, IsNotExactSelfClass,
                                isThunk, subclassScope);
  fn->setDebugScope(new (mod) SILDebugScope(loc, fn));
  return fn;
}

void SILFunctionBuilder::addFunctionAttributes(
    SILFunction *F, DeclAttributes &Attrs, SILModule &M,
    llvm::function_ref<SILFunction *(SILLocation loc, SILDeclRef constant)>
        getOrCreateDeclaration,
    SILDeclRef constant) {

  for (auto *A : Attrs.getAttributes<SemanticsAttr>())
    F->addSemanticsAttr(cast<SemanticsAttr>(A)->Value);

  // If we are asked to emit assembly vision remarks for this function, mark the
  // function as force emitting all optremarks including assembly vision
  // remarks. This allows us to emit the assembly vision remarks without needing
  // to change any of the underlying optremark mechanisms.
  if (auto *A = Attrs.getAttribute(DAK_EmitAssemblyVisionRemarks))
    F->addSemanticsAttr(semantics::FORCE_EMIT_OPT_REMARK_PREFIX);

  // Propagate @_specialize.
  for (auto *A : Attrs.getAttributes<SpecializeAttr>()) {
    auto *SA = cast<SpecializeAttr>(A);
    auto kind =
        SA->getSpecializationKind() == SpecializeAttr::SpecializationKind::Full
            ? SILSpecializeAttr::SpecializationKind::Full
            : SILSpecializeAttr::SpecializationKind::Partial;
    assert(!constant.isNull());
    SILFunction *targetFunction = nullptr;
    auto *attributedFuncDecl = constant.getDecl();
    auto *targetFunctionDecl = SA->getTargetFunctionDecl(attributedFuncDecl);
    // Filter out _spi.
    auto spiGroups = SA->getSPIGroups();
    bool hasSPI = !spiGroups.empty();
    if (hasSPI) {
      if (attributedFuncDecl->getModuleContext() != M.getSwiftModule() &&
          !M.getSwiftModule()->isImportedAsSPI(SA, attributedFuncDecl)) {
        continue;
      }
    }
    assert(spiGroups.size() <= 1 && "SIL does not support multiple SPI groups");
    Identifier spiGroupIdent;
    if (hasSPI) {
      spiGroupIdent = spiGroups[0];
    }
    auto availability =
      AvailabilityInference::annotatedAvailableRangeForAttr(SA,
         M.getSwiftModule()->getASTContext());
    if (targetFunctionDecl) {
      SILDeclRef declRef(targetFunctionDecl, constant.kind, false);
      targetFunction = getOrCreateDeclaration(targetFunctionDecl, declRef);
      F->addSpecializeAttr(SILSpecializeAttr::create(
          M, SA->getSpecializedSignature(), SA->getTypeErasedParams(),
          SA->isExported(), kind, targetFunction, spiGroupIdent,
          attributedFuncDecl->getModuleContext(), availability));
    } else {
      F->addSpecializeAttr(SILSpecializeAttr::create(
          M, SA->getSpecializedSignature(), SA->getTypeErasedParams(),
          SA->isExported(), kind, nullptr, spiGroupIdent,
          attributedFuncDecl->getModuleContext(), availability));
    }
  }

  llvm::SmallVector<const EffectsAttr *, 8> customEffects;
  if (constant) {
    for (auto *attr : Attrs.getAttributes<EffectsAttr>()) {
      auto *effectsAttr = cast<EffectsAttr>(attr);
      if (effectsAttr->getKind() == EffectsKind::Custom) {
        customEffects.push_back(effectsAttr);
      } else {
        if (F->getEffectsKind() != EffectsKind::Unspecified &&
            F->getEffectsKind() != effectsAttr->getKind()) {
          mod.getASTContext().Diags.diagnose(effectsAttr->getLocation(),
              diag::warning_in_effects_attribute, "mismatching function effects");
        } else {
          F->setEffectsKind(effectsAttr->getKind());
        }
      }
    }
  }

  if (!customEffects.empty()) {
    llvm::SmallVector<StringRef, 8> paramNames;
    auto *fnDecl = cast<AbstractFunctionDecl>(constant.getDecl());
    if (ParameterList *paramList = fnDecl->getParameters()) {
      for (ParamDecl *pd : *paramList) {
        // Give up on tuples. Their elements are added as individual
        // arguments. It destroys the 1-1 relation ship between parameters
        // and arguments.
        if (isa<TupleType>(CanType(pd->getType())))
          break;
        // First try the "local" parameter name. If there is none, use the
        // API name. E.g. `foo(apiName localName: Type) {}`
        StringRef name = pd->getName().str();
        if (name.empty())
          name = pd->getArgumentName().str();
        if (!name.empty())
          paramNames.push_back(name);
      }
    }
    for (const EffectsAttr *effectsAttr : llvm::reverse(customEffects)) {
      auto error = F->parseArgumentEffectsFromSource(
                                effectsAttr->getCustomString(), paramNames);
      if (error.first) {
        SourceLoc loc = effectsAttr->getCustomStringLocation();
        if (loc.isValid())
          loc = loc.getAdvancedLoc(error.second);
        mod.getASTContext().Diags.diagnose(loc,
                    diag::warning_in_effects_attribute, StringRef(error.first));
      }
    }
  }

  if (auto *OA = Attrs.getAttribute<OptimizeAttr>()) {
    F->setOptimizationMode(OA->getMode());
  }

  // @_silgen_name and @_cdecl functions may be called from C code somewhere.
  if (Attrs.hasAttribute<SILGenNameAttr>() || Attrs.hasAttribute<CDeclAttr>())
    F->setHasCReferences(true);

  if (Attrs.hasAttribute<UsedAttr>())
    F->setMarkedAsUsed(true);

  if (Attrs.hasAttribute<NoLocksAttr>()) {
    F->setPerfConstraints(PerformanceConstraints::NoLocks);
  } else if (Attrs.hasAttribute<NoAllocationAttr>()) {
    F->setPerfConstraints(PerformanceConstraints::NoAllocation);
  }

  if (Attrs.hasAttribute<LexicalLifetimesAttr>()) {
    F->setForceEnableLexicalLifetimes(DoForceEnableLexicalLifetimes);
  }

  // Validate `@differentiable` attributes by calling `getParameterIndices`.
  // This is important for:
  // - Skipping invalid `@differentiable` attributes in non-primary files.
  // - Preventing duplicate SIL differentiability witness creation for
  //   `@differentiable` attributes on `AbstractStorageDecl` declarations.
  //   Such `@differentiable` attributes are deleted and recreated on the getter
  //   `AccessorDecl` of the `AbstractStorageDecl`.
  for (auto *A : Attrs.getAttributes<DifferentiableAttr>())
    (void)A->getParameterIndices();

  // Propagate `@noDerivative` as `[_semantics "autodiff.nonvarying"]`.
  //
  // `@noDerivative` implies non-varying semantics for differentiable activity
  // analysis. SIL values produced from references to `@noDerivative`
  // declarations will not be marked as varying; these values do not need a
  // derivative.
  if (Attrs.hasAttribute<NoDerivativeAttr>())
    F->addSemanticsAttr("autodiff.nonvarying");

  // Propagate @_dynamicReplacement(for:).
  if (constant.isNull())
    return;
  auto *decl = constant.getDecl();

  // Don't add section for addressor functions (where decl is a global)
  if (isa<FuncDecl>(decl)) {
    if (auto *SA = Attrs.getAttribute<SectionAttr>())
      F->setSection(SA->Name);
  }

  // Only emit replacements for the objc entry point of objc methods.
  // There is one exception: @_dynamicReplacement(for:) of @objc methods in
  // generic classes. In this special case we use native replacement instead of
  // @objc categories.
  if (decl->isObjC() && !decl->isNativeMethodReplacement() &&
      F->getLoweredFunctionType()->getExtInfo().getRepresentation() !=
          SILFunctionTypeRepresentation::ObjCMethod)
    return;

  // Only assign replacements when the thing being replaced is function-like and
  // explicitly declared.  
  auto *origDecl = decl->getDynamicallyReplacedDecl();
  if (auto *replacedDecl = dyn_cast_or_null<AbstractFunctionDecl>(origDecl)) {
    // For @objc method replacement we normally use categories to perform the
    // replacement. Except for methods in generic class where we can't. Instead,
    // we special case this and use the native swift replacement mechanism.
    if (decl->isObjC() && !decl->isNativeMethodReplacement()) {
      F->setObjCReplacement(replacedDecl);
      return;
    }

    if (constant.canBeDynamicReplacement()) {
      SILDeclRef declRef(replacedDecl, constant.kind, false);
      auto *replacedFunc = getOrCreateDeclaration(replacedDecl, declRef);

      assert(replacedFunc->getLoweredFunctionType() ==
                 F->getLoweredFunctionType() ||
             replacedFunc->getLoweredFunctionType()->hasOpaqueArchetype());

      F->setDynamicallyReplacedFunction(replacedFunc);
    }
  } else if (constant.isDistributedThunk()) {
    auto decodeFuncDecl =
            getAssociatedDistributedInvocationDecoderDecodeNextArgumentFunction(
                decl);
    assert(decodeFuncDecl && "decodeNextArgument function not found!");

    auto decodeRef = SILDeclRef(decodeFuncDecl);
    auto *adHocFunc = getOrCreateDeclaration(decodeFuncDecl, decodeRef);
    F->setReferencedAdHocRequirementWitnessFunction(adHocFunc);
  }
}

SILFunction *SILFunctionBuilder::getOrCreateFunction(
    SILLocation loc, SILDeclRef constant, ForDefinition_t forDefinition,
    llvm::function_ref<SILFunction *(SILLocation loc, SILDeclRef constant)>
        getOrCreateDeclaration,
    ProfileCounter entryCount) {
  auto nameTmp = constant.mangle();
  auto constantType = mod.Types.getConstantFunctionType(
      TypeExpansionContext::minimal(), constant);
  SILLinkage linkage = constant.getLinkage(forDefinition);

  if (auto fn = mod.lookUpFunction(nameTmp)) {
    // During SILGen (where the module's SIL stage is Raw), there might be
    // mismatches between the type or linkage. This can happen, when two
    // functions are mistakenly mapped to the same name (e.g. with @_cdecl).
    // We want to issue a regular error in this case and not crash with an
    // assert.
    assert(mod.getStage() == SILStage::Raw ||
           fn->getLoweredFunctionType() == constantType);
    auto linkageForDef = constant.getLinkage(ForDefinition_t::ForDefinition);
    auto fnLinkage = fn->getLinkage();
    assert(mod.getStage() == SILStage::Raw || fn->getLinkage() == linkage ||
           (forDefinition == ForDefinition_t::NotForDefinition &&
            (fnLinkage == linkageForDef ||
             (linkageForDef == SILLinkage::PublicNonABI &&
              fnLinkage == SILLinkage::Shared))));
    if (forDefinition) {
      // In all the cases where getConstantLinkage returns something
      // different for ForDefinition, it returns an available-externally
      // linkage.
      if (isAvailableExternally(fn->getLinkage())) {
        fn->setLinkage(constant.getLinkage(ForDefinition));
      }
    }
    return fn;
  }

  IsTransparent_t IsTrans =
      constant.isTransparent() ? IsTransparent : IsNotTransparent;

  IsSerialized_t IsSer = constant.isSerialized();
  // Don't create a [serialized] function after serialization has happened.
  if (IsSer == IsSerialized && mod.isSerialized())
    IsSer = IsNotSerialized;

  Inline_t inlineStrategy = InlineDefault;
  if (constant.isNoinline())
    inlineStrategy = NoInline;
  else if (constant.isAlwaysInline())
    inlineStrategy = AlwaysInline;

  StringRef name = mod.allocateCopy(nameTmp);
  IsDynamicallyReplaceable_t IsDyn = IsNotDynamic;
  if (constant.isDynamicallyReplaceable()) {
    IsDyn = IsDynamic;
    IsTrans = IsNotTransparent;
  }

  IsDistributed_t IsDistributed = IsDistributed_t::IsNotDistributed;
  // Mark both distributed thunks and methods as distributed.
  if (constant.hasFuncDecl() && constant.getFuncDecl()->isDistributed()) {
    IsDistributed = IsDistributed_t::IsDistributed;
  }

  IsRuntimeAccessible_t isRuntimeAccessible = IsNotRuntimeAccessible;
  if (constant.isRuntimeAccessibleFunction())
    isRuntimeAccessible = IsRuntimeAccessible;

  auto *F = SILFunction::create(mod, linkage, name, constantType, nullptr, None,
                                IsNotBare, IsTrans, IsSer, entryCount, IsDyn,
                                IsDistributed, isRuntimeAccessible,
                                IsNotExactSelfClass, IsNotThunk,
                                constant.getSubclassScope(), inlineStrategy);
  F->setDebugScope(new (mod) SILDebugScope(loc, F));

  if (constant.isGlobal())
    F->setSpecialPurpose(SILFunction::Purpose::GlobalInit);

  if (constant.hasDecl()) {
    auto decl = constant.getDecl();

    if (constant.isForeign && decl->hasClangNode() &&
        !decl->getObjCImplementationDecl())
      F->setClangNodeOwner(decl);

    if (auto availability = constant.getAvailabilityForLinkage())
      F->setAvailabilityForLinkage(*availability);

    F->setIsAlwaysWeakImported(decl->isAlwaysWeakImported());

    if (auto *accessor = dyn_cast<AccessorDecl>(decl)) {
      auto *storage = accessor->getStorage();
      // Add attributes for e.g. computed properties.
      addFunctionAttributes(F, storage->getAttrs(), mod,
                            getOrCreateDeclaration);
                            
      auto *varDecl = dyn_cast<VarDecl>(storage);
      if (varDecl && varDecl->getAttrs().hasAttribute<LazyAttr>() &&
          accessor->getAccessorKind() == AccessorKind::Get) {
        F->setSpecialPurpose(SILFunction::Purpose::LazyPropertyGetter);
        
        // Lazy property getters should not get inlined because they are usually
        // non-trivial functions (otherwise the user would not implement it as
        // lazy property). Inlining such getters would most likely not benefit
        // other optimizations because the top-level switch_enum cannot be
        // constant folded in most cases.
        // Also, not inlining lazy property getters enables optimizing them in
        // CSE.
        F->setInlineStrategy(NoInline);
      }
    }
    addFunctionAttributes(F, decl->getAttrs(), mod, getOrCreateDeclaration,
                          constant);
  }

  return F;
}

SILFunction *SILFunctionBuilder::getOrCreateSharedFunction(
    SILLocation loc, StringRef name, CanSILFunctionType type,
    IsBare_t isBareSILFunction, IsTransparent_t isTransparent,
    IsSerialized_t isSerialized, ProfileCounter entryCount, IsThunk_t isThunk,
    IsDynamicallyReplaceable_t isDynamic, IsDistributed_t isDistributed,
    IsRuntimeAccessible_t isRuntimeAccessible) {
  return getOrCreateFunction(loc, name, SILLinkage::Shared, type,
                             isBareSILFunction, isTransparent, isSerialized,
                             isDynamic, isDistributed, isRuntimeAccessible,
                             entryCount, isThunk, SubclassScope::NotApplicable);
}

SILFunction *SILFunctionBuilder::createFunction(
    SILLinkage linkage, StringRef name, CanSILFunctionType loweredType,
    GenericEnvironment *genericEnv, Optional<SILLocation> loc,
    IsBare_t isBareSILFunction, IsTransparent_t isTrans,
    IsSerialized_t isSerialized, IsDynamicallyReplaceable_t isDynamic,
    IsDistributed_t isDistributed, IsRuntimeAccessible_t isRuntimeAccessible,
    ProfileCounter entryCount, IsThunk_t isThunk, SubclassScope subclassScope,
    Inline_t inlineStrategy, EffectsKind EK, SILFunction *InsertBefore,
    const SILDebugScope *DebugScope) {
  return SILFunction::create(mod, linkage, name, loweredType, genericEnv, loc,
                             isBareSILFunction, isTrans, isSerialized,
                             entryCount, isDynamic, isDistributed,
                             isRuntimeAccessible, IsNotExactSelfClass, isThunk,
                             subclassScope, inlineStrategy, EK, InsertBefore,
                             DebugScope);
}
