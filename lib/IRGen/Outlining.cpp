//===--- Outlining.cpp - Outlining value operations -----------------------===//
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
//  This file implements IR generation for outlined value operations in Swift.
//
//===----------------------------------------------------------------------===//

#include "Outlining.h"

#include "Explosion.h"
#include "GenOpaque.h"
#include "GenProto.h"
#include "GenericRequirement.h"
#include "IRGenFunction.h"
#include "IRGenMangler.h"
#include "IRGenModule.h"
#include "LoadableTypeInfo.h"
#include "LocalTypeDataKind.h"
#include "MetadataRequest.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/Basic/Assertions.h"
#include "swift/IRGen/GenericRequirement.h"
#include "swift/SIL/SILModule.h"

using namespace swift;
using namespace irgen;

static GenericEnvironment *digOutGenericEnvironment(CanType loweredType) {
  // Find a non-local, non-opaque archetype in the type and pull out
  // its generic environment.
  // TODO: we ought to be able to usefully minimize this

  GenericEnvironment *env = nullptr;
  loweredType.findIf([&env](CanType t) -> bool {
    if (auto arch = dyn_cast<ArchetypeType>(t)) {
      if (!isa<PrimaryArchetypeType>(arch) && !isa<PackArchetypeType>(arch))
        return false;
      env = arch->getGenericEnvironment();
      return true;
    }
    return false;
  });

  return env;
}

OutliningMetadataCollector::OutliningMetadataCollector(
    SILType T, IRGenFunction &IGF, LayoutIsNeeded_t needsLayout,
    DeinitIsNeeded_t needsDeinitTypes)
    : T(T), IGF(IGF), needsLayout(needsLayout), needsDeinit(needsDeinitTypes) {}

void OutliningMetadataCollector::collectTypeMetadata(SILType ty) {
  assert(state != OutliningMetadataCollector::State::Kind::Collected);
  // If the type has no archetypes, we can emit it from scratch in the callee.
  if (!ty.hasArchetype()) {
    return;
  }

  // Substitute opaque types if allowed.
  ty = IGF.IGM.substOpaqueTypesWithUnderlyingTypes(ty, CanGenericSignature());

  collectTypeMetadataForLayout(ty);
  collectTypeMetadataForDeinit(ty);
}

void OutliningMetadataCollector::collectTypeMetadataForLayout(SILType ty) {
  if (!needsLayout)
    return;

  auto astType = ty.getASTType();
  auto &ti = IGF.IGM.getTypeInfoForLowered(astType);

  // We don't need the metadata for fixed size types or types that are not ABI
  // accessible. Outlining will call the value witness of the enclosing type of
  // non ABI accessible field/element types.
  if (isa<FixedTypeInfo>(ti) || !ti.isABIAccessible()) {
    return;
  }

  // If the type is a legal formal type, add it as a formal type.
  // FIXME: does this force us to emit a more expensive metadata than we need
  // to?
  if (astType->isLegalFormalType()) {
    return state.getCollecting().addFormalTypeMetadata(astType);
  }

  state.getCollecting().addRepresentationTypeMetadata(ty);
}

void OutliningMetadataCollector::collectTypeMetadataForDeinit(SILType ty) {
  if (!needsDeinit)
    return;

  auto *nominal = ty.getASTType()->getAnyNominal();
  if (!nominal)
    return;
  if (!nominal->getValueTypeDestructor())
    return;
  assert(ty.isMoveOnly());

  state.getCollecting().addValueTypeWithDeinit(ty);
}

void OutliningMetadataCollector::materializeFormalTypeMetadata(
    CanType ty, State::Collected::Elements &into) {
  // If the type has no archetypes, we can emit it from scratch in the callee.
  assert(ty->hasArchetype());

  auto key = LocalTypeDataKey(ty, LocalTypeDataKind::forFormalTypeMetadata());
  if (into.Values.count(key))
    return;

  auto metadata = IGF.emitTypeMetadataRef(ty);
  into.Values.insert({key, metadata});

  assert(into.Values.count(key));
}

void OutliningMetadataCollector::materializeRepresentationTypeMetadata(
    SILType ty, State::Collected::Elements &into) {
  auto key = LocalTypeDataKey(
      ty.getASTType(), LocalTypeDataKind::forRepresentationTypeMetadata());
  if (into.Values.count(key))
    return;

  auto metadata = IGF.emitTypeMetadataRefForLayout(ty);
  into.Values.insert({key, metadata});
}

void OutliningMetadataCollector::materialize() {
  if (state == State::Kind::Collected)
    return;

  auto collection = std::move(state.getCollecting());
  switch (collection) {
  case State::CollectionKind::Elements: {
    auto &elements = collection.getElements();
    auto &collected = state.setCollectedElements();
    for (auto &element : elements.elements) {
      switch (element) {
      case State::ElementKind::MetadataForFormal: {
        auto ty = element.getFormalType();
        materializeFormalTypeMetadata(ty, /*into=*/collected);
        break;
      }
      case State::ElementKind::MetadataForRepresentation: {
        auto ty = element.getRepresentationType();
        materializeRepresentationTypeMetadata(ty, /*into=*/collected);
        break;
      }
      }
    }
    return;
  }
  case State::CollectionKind::Environment: {
    auto pair = getTypeAndGenericSignatureForManglingOutlineFunction(T);
    auto sig = pair.second;
    auto subs = digOutGenericEnvironment(T.getASTType())
                    ->getForwardingSubstitutionMap();
    auto &collected = state.setCollectedEnvironment(subs);

    GenericTypeRequirements requirements(IGF.IGM, sig);
    for (auto requirement : requirements.getRequirements()) {
      auto *value = emitGenericRequirementFromSubstitutions(
          IGF, requirement, MetadataState::Complete, subs);
      collected.Requirements.insert({requirement, value});
    }
    return;
  }
  }
}

void OutliningMetadataCollector::addPolymorphicArguments(
    SmallVectorImpl<llvm::Value *> &args) const {
  assert(hasFinished());
  if (state == State::Kind::Empty)
    return;
  auto &collected = state.getCollected();
  switch (collected) {
  case State::CollectionKind::Elements: {
    for (auto &pair : collected.getElements().Values) {
      auto metadata = pair.second;
      assert(metadata->getType() == IGF.IGM.TypeMetadataPtrTy);
      args.push_back(metadata);
    }
    return;
  }
  case State::CollectionKind::Environment: {
    for (auto &pair : collected.getEnvironment().Requirements) {
      auto *value = pair.second;
      args.push_back(value);
    }
    return;
  }
  }
}

void OutliningMetadataCollector::addPolymorphicParameterTypes(
    SmallVectorImpl<llvm::Type *> &paramTys) const {
  assert(hasFinished());
  if (state == State::Kind::Empty)
    return;
  auto &collected = state.getCollected();
  switch (collected) {
  case State::CollectionKind::Elements: {
    for (auto &pair : collected.getElements().Values) {
      auto *metadata = pair.second;
      paramTys.push_back(metadata->getType());
    }
    return;
  }
  case State::CollectionKind::Environment: {
    for (auto &pair : collected.getEnvironment().Requirements) {
      auto *value = pair.second;
      paramTys.push_back(value->getType());
    }
    return;
  }
  }
}

void OutliningMetadataCollector::bindPolymorphicParameters(
    IRGenFunction &IGF, Explosion &params) const {
  assert(hasFinished());
  if (state == State::Kind::Empty)
    return;
  auto &collected = state.getCollected();
  switch (collected) {
  case State::CollectionKind::Elements: {
    // Note that our parameter IGF intentionally shadows the IGF that this
    // collector was built with.
    for (auto &pair : collected.getElements().Values) {
      llvm::Value *arg = params.claimNext();

      auto key = pair.first;
      assert(key.Kind.isAnyTypeMetadata());
      setTypeMetadataName(IGF.IGM, arg, key.Type);
      IGF.setUnscopedLocalTypeData(key, MetadataResponse::forComplete(arg));
    }
    return;
  }
  case State::CollectionKind::Environment: {
    auto &environment = collected.getEnvironment();
    for (auto &pair : environment.Requirements) {
      bindGenericRequirement(IGF, pair.first, params.claimNext(),
                             MetadataState::Complete, environment.Subs);
    }
    return;
  }
  }
}

std::pair<CanType, CanGenericSignature>
irgen::getTypeAndGenericSignatureForManglingOutlineFunction(SILType type) {
  auto loweredType = type.getASTType();
  if (!loweredType->hasArchetype()) return {loweredType, nullptr};

  GenericEnvironment *env = digOutGenericEnvironment(loweredType);

  assert(env && "has archetype but no archetype?!");
  return {loweredType->mapTypeOutOfContext()->getCanonicalType(),
          env->getGenericSignature().getCanonicalSignature()};
}

bool TypeInfo::withWitnessableMetadataCollector(
    IRGenFunction &IGF, SILType T, LayoutIsNeeded_t mayNeedLayout,
    DeinitIsNeeded_t needsDeinit,
    llvm::function_ref<void(OutliningMetadataCollector &)> invocation) const {
  bool needsCollector = false;
  LayoutIsNeeded_t needsLayout = LayoutIsNotNeeded;
  if (!T.hasLocalArchetype() &&
      !IGF.outliningCanCallValueWitnesses()) {
    needsCollector = true;
    if (T.hasArchetype()) {
      needsLayout = LayoutIsNeeded;
    }
  } else if (!T.hasArchetype()) {
    needsCollector = true;
    // The implementation will call vwt in this case.
    needsLayout = LayoutIsNotNeeded;
  }

  if (needsCollector) {
    OutliningMetadataCollector collector(T, IGF, needsLayout, needsDeinit);
    if (needsDeinit || needsLayout) {
      // Only collect if anything would be collected.
      collectMetadataForOutlining(collector, T);
    }
    collector.materialize();
    invocation(collector);
    return true;
  }

  return false;
}

void TypeInfo::callOutlinedCopy(IRGenFunction &IGF, Address dest, Address src,
                                SILType T, IsInitialization_t isInit,
                                IsTake_t isTake) const {
  if (withWitnessableMetadataCollector(
          IGF, T, LayoutIsNeeded, DeinitIsNotNeeded, [&](auto collector) {
            collector.emitCallToOutlinedCopy(dest, src, T, *this, isInit,
                                             isTake);
          })) {
    return;
  }

  if (isInit == IsInitialization && isTake == IsTake) {
    return emitInitializeWithTakeCall(IGF, T, dest, src);
  } else if (isInit == IsInitialization && isTake == IsNotTake) {
    return emitInitializeWithCopyCall(IGF, T, dest, src);
  } else if (isInit == IsNotInitialization && isTake == IsTake) {
    return emitAssignWithTakeCall(IGF, T, dest, src);
  } else if (isInit == IsNotInitialization && isTake == IsNotTake) {
    return emitAssignWithCopyCall(IGF, T, dest, src);
  }
  llvm_unreachable("unknown case");
}

void OutliningMetadataCollector::emitCallToOutlinedCopy(
    Address dest, Address src, SILType T, const TypeInfo &ti,
    IsInitialization_t isInit, IsTake_t isTake) const {
  assert(hasFinished());
  assert(!needsDeinit);
  llvm::SmallVector<llvm::Value *, 4> args;
  args.push_back(IGF.Builder.CreateElementBitCast(src, ti.getStorageType())
                            .getAddress());
  args.push_back(IGF.Builder.CreateElementBitCast(dest, ti.getStorageType())
                            .getAddress());
  addPolymorphicArguments(args);

  llvm::Constant *outlinedFn;
  if (isInit && isTake) {
    outlinedFn =
      IGF.IGM.getOrCreateOutlinedInitializeWithTakeFunction(T, ti, *this);
  } else if (isInit) {
    outlinedFn =
      IGF.IGM.getOrCreateOutlinedInitializeWithCopyFunction(T, ti, *this);
  } else if (isTake) {
    outlinedFn =
      IGF.IGM.getOrCreateOutlinedAssignWithTakeFunction(T, ti, *this);
  } else {
    outlinedFn =
      IGF.IGM.getOrCreateOutlinedAssignWithCopyFunction(T, ti, *this);
  }

  llvm::CallInst *call = IGF.Builder.CreateCall(
      cast<llvm::Function>(outlinedFn)->getFunctionType(), outlinedFn, args);
  call->setCallingConv(IGF.IGM.DefaultCC);
}

static bool needsSpecialOwnershipHandling(SILType t) {
  auto astType = t.getASTType();
  auto ref = dyn_cast<ReferenceStorageType>(astType);
  if (!ref) {
    return false;
  }
  return ref->getOwnership() != ReferenceOwnership::Strong;
}

static bool canUseValueWitnessForValueOp(IRGenModule &IGM, SILType T) {
  if (!IGM.getSILModule().isTypeMetadataForLayoutAccessible(T))
    return false;

  // No value witness tables in embedded Swift.
  if (IGM.Context.LangOpts.hasFeature(Feature::Embedded))
    return false;

  // It is not a good code size trade-off to instantiate a metatype for
  // existentials, and also does not back-deploy gracefully in the case of
  // constrained protocols.
  if (T.getASTType()->isExistentialType())
    return false;

  if (needsSpecialOwnershipHandling(T))
    return false;
  if (T.getASTType()->hasDynamicSelfType())
    return false;
  return true;
}

llvm::Constant *IRGenModule::getOrCreateOutlinedInitializeWithTakeFunction(
                              SILType T, const TypeInfo &ti,
                              const OutliningMetadataCollector &collector) {
  auto manglingBits = getTypeAndGenericSignatureForManglingOutlineFunction(T);
  auto funcName =
    IRGenMangler().mangleOutlinedInitializeWithTakeFunction(manglingBits.first,
        manglingBits.second, collector.IGF.isPerformanceConstraint);

  return getOrCreateOutlinedCopyAddrHelperFunction(
      T, ti, collector, funcName,
      [this](IRGenFunction &IGF, Address dest, Address src, SILType T,
         const TypeInfo &ti) {
        if (!IGF.outliningCanCallValueWitnesses() ||
            T.hasArchetype() || !canUseValueWitnessForValueOp(*this, T)) {
          ti.initializeWithTake(IGF, dest, src, T, true, /*zeroizeIfSensitive=*/ true);
        } else {
          emitInitializeWithTakeCall(IGF, T, dest, src);
        }
      });
}

llvm::Constant *IRGenModule::getOrCreateOutlinedInitializeWithCopyFunction(
                              SILType T, const TypeInfo &ti,
                              const OutliningMetadataCollector &collector) {
  auto manglingBits = getTypeAndGenericSignatureForManglingOutlineFunction(T);
  auto funcName =
    IRGenMangler().mangleOutlinedInitializeWithCopyFunction(manglingBits.first,
        manglingBits.second, collector.IGF.isPerformanceConstraint);

  return getOrCreateOutlinedCopyAddrHelperFunction(
      T, ti, collector, funcName,
      [this](IRGenFunction &IGF, Address dest, Address src, SILType T,
         const TypeInfo &ti) {
        if (!IGF.outliningCanCallValueWitnesses() ||
            T.hasArchetype() || !canUseValueWitnessForValueOp(*this, T)) {
          ti.initializeWithCopy(IGF, dest, src, T, true);
        } else {
          emitInitializeWithCopyCall(IGF, T, dest, src);
        }
      });
}

llvm::Constant *IRGenModule::getOrCreateOutlinedAssignWithTakeFunction(
                              SILType T, const TypeInfo &ti,
                              const OutliningMetadataCollector &collector) {
  auto manglingBits = getTypeAndGenericSignatureForManglingOutlineFunction(T);
  auto funcName =
    IRGenMangler().mangleOutlinedAssignWithTakeFunction(manglingBits.first,
        manglingBits.second, collector.IGF.isPerformanceConstraint);

  return getOrCreateOutlinedCopyAddrHelperFunction(
      T, ti, collector, funcName,
      [this](IRGenFunction &IGF, Address dest, Address src, SILType T,
         const TypeInfo &ti) {
        if (!IGF.outliningCanCallValueWitnesses() ||
            T.hasArchetype() || !canUseValueWitnessForValueOp(*this, T)) {
          ti.assignWithTake(IGF, dest, src, T, true);
        } else {
          emitAssignWithTakeCall(IGF, T, dest, src);
        }
      });
}

llvm::Constant *IRGenModule::getOrCreateOutlinedAssignWithCopyFunction(
                              SILType T, const TypeInfo &ti,
                              const OutliningMetadataCollector &collector) {
  auto manglingBits = getTypeAndGenericSignatureForManglingOutlineFunction(T);
  auto funcName =
    IRGenMangler().mangleOutlinedAssignWithCopyFunction(manglingBits.first,
        manglingBits.second, collector.IGF.isPerformanceConstraint);

  return getOrCreateOutlinedCopyAddrHelperFunction(
      T, ti, collector, funcName,
      [this](IRGenFunction &IGF, Address dest, Address src, SILType T,
             const TypeInfo &ti) {
        if (!IGF.outliningCanCallValueWitnesses() ||
            T.hasArchetype() || !canUseValueWitnessForValueOp(*this, T)) {
          ti.assignWithCopy(IGF, dest, src, T, true);
        } else {
          emitAssignWithCopyCall(IGF, T, dest, src);
        }
      });
}

llvm::Constant *IRGenModule::getOrCreateOutlinedCopyAddrHelperFunction(
                              SILType T, const TypeInfo &ti,
                              const OutliningMetadataCollector &collector,
                              StringRef funcName,
                              CopyAddrHelperGenerator generator) {
  assert(collector.hasFinished());
  auto ptrTy = ti.getStorageType()->getPointerTo();

  llvm::SmallVector<llvm::Type *, 4> paramTys;
  paramTys.push_back(ptrTy);
  paramTys.push_back(ptrTy);
  collector.addPolymorphicParameterTypes(paramTys);

  return getOrCreateHelperFunction(funcName, ptrTy, paramTys,
      [&](IRGenFunction &IGF) {
        auto params = IGF.collectParameters();
        Address src = ti.getAddressForPointer(params.claimNext());
        Address dest = ti.getAddressForPointer(params.claimNext());
        collector.bindPolymorphicParameters(IGF, params);
        generator(IGF, dest, src, T, ti);
        IGF.Builder.CreateRet(dest.getAddress());
      },
      true /*setIsNoInline*/,
      false /*forPrologue*/,
      collector.IGF.isPerformanceConstraint);
}

void TypeInfo::callOutlinedDestroy(IRGenFunction &IGF,
                                   Address addr, SILType T) const {
  // Short-cut destruction of trivial values.
  if (IGF.IGM.getTypeLowering(T).isTrivial())
    return;

  if (withWitnessableMetadataCollector(
          IGF, T, LayoutIsNeeded, DeinitIsNeeded, [&](auto collector) {
            collector.emitCallToOutlinedDestroy(addr, T, *this);
          })) {
    return;
  }

  return emitDestroyCall(IGF, T, addr);
}

void OutliningMetadataCollector::emitCallToOutlinedDestroy(
    Address addr, SILType T, const TypeInfo &ti) const {
  assert(hasFinished());
  assert(needsDeinit);
  llvm::SmallVector<llvm::Value *, 4> args;
  args.push_back(IGF.Builder.CreateElementBitCast(addr, ti.getStorageType())
                            .getAddress());
  addPolymorphicArguments(args);

  auto outlinedFn =
    IGF.IGM.getOrCreateOutlinedDestroyFunction(T, ti, *this);

  llvm::CallInst *call = IGF.Builder.CreateCall(
      cast<llvm::Function>(outlinedFn)->getFunctionType(), outlinedFn, args);
  call->setCallingConv(IGF.IGM.DefaultCC);
}

llvm::Constant *IRGenModule::getOrCreateOutlinedDestroyFunction(
                              SILType T, const TypeInfo &ti,
                              const OutliningMetadataCollector &collector) {
  IRGenMangler mangler;
  auto manglingBits = getTypeAndGenericSignatureForManglingOutlineFunction(T);
  auto funcName = mangler.mangleOutlinedDestroyFunction(manglingBits.first,
                     manglingBits.second, collector.IGF.isPerformanceConstraint);

  auto ptrTy = ti.getStorageType()->getPointerTo();
  llvm::SmallVector<llvm::Type *, 4> paramTys;
  paramTys.push_back(ptrTy);
  collector.addPolymorphicParameterTypes(paramTys);

  return getOrCreateHelperFunction(funcName, ptrTy, paramTys,
      [&](IRGenFunction &IGF) {
        Explosion params = IGF.collectParameters();
        Address addr = ti.getAddressForPointer(params.claimNext());
        collector.bindPolymorphicParameters(IGF, params);
        if (!IGF.outliningCanCallValueWitnesses() ||
            T.hasArchetype() || !canUseValueWitnessForValueOp(*this, T)) {
          ti.destroy(IGF, addr, T, true);
        } else {
          emitDestroyCall(IGF, T, addr);
        }

        IGF.Builder.CreateRet(addr.getAddress());
      },
      true /*setIsNoInline*/,
      false /*forPrologue*/,
      collector.IGF.isPerformanceConstraint);
}

llvm::Constant *IRGenModule::getOrCreateRetainFunction(const TypeInfo &ti,
                                                       SILType t,
                                                       llvm::Type *llvmType,
                                                       Atomicity atomicity) {
  auto *loadableTI = cast<LoadableTypeInfo>(&ti);
  IRGenMangler mangler;
  auto manglingBits =
    getTypeAndGenericSignatureForManglingOutlineFunction(t);
  auto funcName = mangler.mangleOutlinedRetainFunction(manglingBits.first,
                                                       manglingBits.second);
  llvm::Type *argTys[] = {llvmType};
  return getOrCreateHelperFunction(
      funcName, llvmType, argTys,
      [&](IRGenFunction &IGF) {
        auto it = IGF.CurFn->arg_begin();
        Address addr(&*it++, loadableTI->getStorageType(),
                     loadableTI->getFixedAlignment());
        Explosion loaded;
        loadableTI->loadAsTake(IGF, addr, loaded);
        Explosion out;
        loadableTI->copy(IGF, loaded, out, atomicity);
        (void)out.claimAll();
        IGF.Builder.CreateRet(addr.getAddress());
      },
      true /*setIsNoInline*/);
}

void TypeInfo::callOutlinedRelease(IRGenFunction &IGF, Address addr, SILType T,
                                   Atomicity atomicity) const {
  OutliningMetadataCollector collector(T, IGF, LayoutIsNotNeeded,
                                       DeinitIsNeeded);
  collectMetadataForOutlining(collector, T);
  collector.materialize();
  collector.emitCallToOutlinedRelease(addr, T, *this, atomicity);
}

void OutliningMetadataCollector::emitCallToOutlinedRelease(
    Address addr, SILType T, const TypeInfo &ti, Atomicity atomicity) const {
  assert(hasFinished());
  assert(!needsLayout);
  assert(needsDeinit);
  llvm::SmallVector<llvm::Value *, 4> args;
  args.push_back(addr.getAddress());
  addPolymorphicArguments(args);
  auto *outlinedF = cast<llvm::Function>(IGF.IGM.getOrCreateReleaseFunction(
      ti, T, addr.getAddress()->getType(), atomicity, *this));
  llvm::CallInst *call =
      IGF.Builder.CreateCall(outlinedF->getFunctionType(), outlinedF, args);
  call->setCallingConv(IGF.IGM.DefaultCC);
}

llvm::Constant *IRGenModule::getOrCreateReleaseFunction(
    const TypeInfo &ti, SILType t, llvm::Type *ptrTy, Atomicity atomicity,
    const OutliningMetadataCollector &collector) {
  auto *loadableTI = cast<LoadableTypeInfo>(&ti);
  IRGenMangler mangler;
  auto manglingBits =
    getTypeAndGenericSignatureForManglingOutlineFunction(t);
  auto funcName = mangler.mangleOutlinedReleaseFunction(manglingBits.first,
                                                        manglingBits.second);
  llvm::SmallVector<llvm::Type *, 4> argTys;
  argTys.push_back(ptrTy);
  collector.addPolymorphicParameterTypes(argTys);
  return getOrCreateHelperFunction(
      funcName, ptrTy, argTys,
      [&](IRGenFunction &IGF) {
        Explosion params = IGF.collectParameters();
        Address addr(params.claimNext(), loadableTI->getStorageType(),
                     loadableTI->getFixedAlignment());
        collector.bindPolymorphicParameters(IGF, params);
        Explosion loaded;
        loadableTI->loadAsTake(IGF, addr, loaded);
        loadableTI->consume(IGF, loaded, atomicity, t);
        IGF.Builder.CreateRet(addr.getAddress());
      },
      true /*setIsNoInline*/);
}
