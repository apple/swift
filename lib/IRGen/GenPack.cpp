//===--- GenPack.cpp - Swift IR Generation For Variadic Generics ----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements IR generation for type and value packs in Swift.
//
//===----------------------------------------------------------------------===//

#include "GenPack.h"
#include "GenProto.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/Types.h"
#include "swift/IRGen/GenericRequirement.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "llvm/IR/DerivedTypes.h"

#include "GenTuple.h"
#include "GenType.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "MetadataRequest.h"
#include "ResilientTypeInfo.h"

using namespace swift;
using namespace irgen;

static void cleanupTypeMetadataPackImpl(IRGenFunction &IGF, StackAddress pack,
                                        llvm::Value *shape);
static void cleanupWitnessTablePackImpl(IRGenFunction &IGF, StackAddress pack,
                                        llvm::Value *shape);

static CanPackArchetypeType
getForwardedPackArchetypeType(CanPackType packType) {
  if (auto expansion = packType.unwrapSingletonPackExpansion())
    return dyn_cast<PackArchetypeType>(expansion.getPatternType());
  return CanPackArchetypeType();
}

static MetadataResponse
tryGetLocalPackTypeMetadata(IRGenFunction &IGF, CanPackType packType,
                            DynamicMetadataRequest request) {
  if (auto result = IGF.tryGetLocalTypeMetadata(packType, request))
    return result;

  if (auto packArchetypeType = getForwardedPackArchetypeType(packType)) {
    if (auto result = IGF.tryGetLocalTypeMetadata(packArchetypeType, request))
      return result;
  }

  return MetadataResponse();
}

static llvm::Value *tryGetLocalPackTypeData(IRGenFunction &IGF,
                                            CanPackType packType,
                                            LocalTypeDataKind localDataKind) {
  if (auto *wtable = IGF.tryGetLocalTypeData(packType, localDataKind))
    return wtable;

  if (auto packArchetypeType = getForwardedPackArchetypeType(packType)) {
    // Also unwrap the pack conformance, if there is one.
    if (localDataKind.isPackProtocolConformance()) {
      localDataKind = LocalTypeDataKind::forProtocolWitnessTable(
          localDataKind.getPackProtocolConformance()
              ->getPatternConformances()[0]);
    }

    if (auto *wtable =
            IGF.tryGetLocalTypeData(packArchetypeType, localDataKind))
      return wtable;
  }

  return nullptr;
}

static void accumulateSum(IRGenFunction &IGF, llvm::Value *&result,
                          llvm::Value *value) {
  if (result == nullptr) {
    result = value;
    return;
  }

  result = IGF.Builder.CreateAdd(result, value);
}

llvm::Value *
irgen::emitIndexOfStructuralPackComponent(IRGenFunction &IGF,
                                          CanPackType packType,
                                          unsigned structuralIndex) {
  assert(structuralIndex < packType->getNumElements());
  unsigned numFixedComponents = 0;
  llvm::Value *length = nullptr;
  for (unsigned i = 0; i < structuralIndex; ++i) {
    auto componentType = packType.getElementType(i);
    if (auto expansion = dyn_cast<PackExpansionType>(componentType)) {
      auto countType = expansion.getCountType();
      auto expansionLength = IGF.emitPackShapeExpression(countType);
      accumulateSum(IGF, length, expansionLength);
    } else {
      numFixedComponents++;
    }
  }

  if (numFixedComponents > 0 || !length) {
    auto fixedLength =
      llvm::ConstantInt::get(IGF.IGM.SizeTy, numFixedComponents);
    accumulateSum(IGF, length, fixedLength);
  }

  assert(length);
  return length;
}

using PackExplosionCallback = void (CanType eltTy,
                                    unsigned scalarIndex,
                                    llvm::Value *dynamicIndex,
                                    llvm::Value *dynamicLength);

static std::pair<unsigned, llvm::Value *>
visitPackExplosion(IRGenFunction &IGF, CanPackType type,
                   llvm::function_ref<PackExplosionCallback> callback) {
  llvm::Value *result = nullptr;

  // If shape(T) == t and shape(U) == u, the shape expression for a pack
  // {T..., Int, T..., U..., String} becomes 't + t + u + 2'.
  unsigned scalarElements = 0;

  for (auto elt : type.getElementTypes()) {
    if (auto expansionType = dyn_cast<PackExpansionType>(elt)) {
      auto reducedShape = expansionType.getCountType();
      auto *eltCount = IGF.emitPackShapeExpression(reducedShape);
      callback(elt, scalarElements, result, eltCount);
      accumulateSum(IGF, result, eltCount);
      continue;
    }

    callback(elt, scalarElements, result, nullptr);
    ++scalarElements;
  }

  return std::make_pair(scalarElements, result);
}

llvm::Value *IRGenFunction::emitPackShapeExpression(CanType type) {

  type = type->getReducedShape()->getCanonicalType();

  auto kind = LocalTypeDataKind::forPackShapeExpression();

  llvm::Value *result = tryGetLocalTypeData(type, kind);
  if (result != nullptr)
    return result;

  auto pair = visitPackExplosion(
      *this, cast<PackType>(type),
      [&](CanType, unsigned, llvm::Value *, llvm::Value *) {});

  if (pair.first > 0) {
    auto *constant = llvm::ConstantInt::get(IGM.SizeTy, pair.first);
    accumulateSum(*this, pair.second, constant);
  } else if (pair.second == nullptr) {
    pair.second = llvm::ConstantInt::get(IGM.SizeTy, 0);
  }

  setScopedLocalTypeData(type, kind, pair.second);
  return pair.second;
}

MetadataResponse
irgen::emitPackArchetypeMetadataRef(IRGenFunction &IGF,
                                    CanPackArchetypeType type,
                                    DynamicMetadataRequest request) {
  if (auto result = IGF.tryGetLocalTypeMetadata(type, request))
    return result;

  auto packType = CanPackType::getSingletonPackExpansion(type);
  auto response = emitTypeMetadataPackRef(IGF, packType, request);

  IGF.setScopedLocalTypeMetadata(type, response);
  return response;
}

static Address emitFixedSizeMetadataPackRef(IRGenFunction &IGF,
                                            CanPackType packType,
                                            DynamicMetadataRequest request) {
  assert(!packType->containsPackExpansionType());

  unsigned elementCount = packType->getNumElements();
  auto allocType = llvm::ArrayType::get(
      IGF.IGM.TypeMetadataPtrTy, elementCount);

  auto pack = IGF.createAlloca(allocType, IGF.IGM.getPointerAlignment());
  IGF.Builder.CreateLifetimeStart(pack,
                              IGF.IGM.getPointerSize() * elementCount);

  for (unsigned i : indices(packType->getElementTypes())) {
    Address slot = IGF.Builder.CreateStructGEP(
        pack, i, IGF.IGM.getPointerSize());

    auto metadata = IGF.emitTypeMetadataRef(
        packType.getElementType(i), request).getMetadata();
    IGF.Builder.CreateStore(metadata, slot);
  }

  return pack;
}

llvm::Value *irgen::maskMetadataPackPointer(IRGenFunction &IGF,
                                            llvm::Value *patternPack) {
  // If the pack is on the heap, the LSB is set, so mask it off.
  patternPack =
      IGF.Builder.CreatePtrToInt(patternPack, IGF.IGM.SizeTy);
  patternPack =
      IGF.Builder.CreateAnd(patternPack, llvm::ConstantInt::get(IGF.IGM.SizeTy, -2));
  patternPack =
      IGF.Builder.CreateIntToPtr(patternPack, IGF.IGM.TypeMetadataPtrPtrTy);
  return patternPack;
}

/// Use this to index into packs to correctly handle on-heap packs.
static llvm::Value *loadMetadataAtIndex(IRGenFunction &IGF,
                                        llvm::Value *patternPack,
                                        llvm::Value *index) {
  patternPack = maskMetadataPackPointer(IGF, patternPack);

  Address patternPackAddress(patternPack, IGF.IGM.TypeMetadataPtrTy,
                             IGF.IGM.getPointerAlignment());

  // Load the metadata pack element from the current source index.
  Address fromPtr(
      IGF.Builder.CreateInBoundsGEP(patternPackAddress.getElementType(),
                                    patternPackAddress.getAddress(), index),
      patternPackAddress.getElementType(), patternPackAddress.getAlignment());
  return IGF.Builder.CreateLoad(fromPtr);
}

static llvm::Value *bindMetadataAtIndex(IRGenFunction &IGF,
                                        CanType elementArchetype,
                                        llvm::Value *patternPack,
                                        llvm::Value *index,
                                        DynamicMetadataRequest request) {
  if (auto response = IGF.tryGetLocalTypeMetadata(elementArchetype, request))
    return response.getMetadata();

  llvm::Value *metadata = loadMetadataAtIndex(IGF, patternPack, index);

  // Bind the metadata pack element to the element archetype.
  IGF.setScopedLocalTypeMetadata(elementArchetype,
                                 MetadataResponse::forComplete(metadata));

  return metadata;
}

/// Use this to index into packs to correctly handle on-heap packs.
static llvm::Value *loadWitnessTableAtIndex(IRGenFunction &IGF,
                                            llvm::Value *wtablePack,
                                            llvm::Value *index) {
  // If the pack is on the heap, the LSB is set, so mask it off.
  wtablePack =
      IGF.Builder.CreatePtrToInt(wtablePack, IGF.IGM.SizeTy);
  wtablePack =
      IGF.Builder.CreateAnd(wtablePack, llvm::ConstantInt::get(IGF.IGM.SizeTy, -2));
  wtablePack =
      IGF.Builder.CreateIntToPtr(wtablePack, IGF.IGM.WitnessTablePtrPtrTy);

  Address patternPackAddress(wtablePack, IGF.IGM.WitnessTablePtrTy,
                             IGF.IGM.getPointerAlignment());

  // Load the witness table pack element from the current source index.
  Address fromPtr(
      IGF.Builder.CreateInBoundsGEP(patternPackAddress.getElementType(),
                                    patternPackAddress.getAddress(), index),
      patternPackAddress.getElementType(), patternPackAddress.getAlignment());
  return IGF.Builder.CreateLoad(fromPtr);
}

static llvm::Value *bindWitnessTableAtIndex(IRGenFunction &IGF,
                                            CanType elementArchetype,
                                            ProtocolConformanceRef conf,
                                            llvm::Value *wtablePack,
                                            llvm::Value *index) {
  auto key = LocalTypeDataKind::forProtocolWitnessTable(conf);
  if (auto *wtable = IGF.tryGetLocalTypeData(elementArchetype, key))
    return wtable;

  auto *wtable = loadWitnessTableAtIndex(IGF, wtablePack, index);

  // Bind the witness table pack element to the element archetype.
  IGF.setScopedLocalTypeData(elementArchetype, key, wtable);

  return wtable;
}

/// Find the pack archetype for the given interface type in the given
/// opened element context, which is known to be a forwarding context.
static CanPackArchetypeType
getMappedPackArchetypeType(const OpenedElementContext &context, CanType ty) {
  auto packType = cast<PackType>(
    context.environment->maybeApplyOuterContextSubstitutions(ty)
        ->getCanonicalType());
  auto archetype = getForwardedPackArchetypeType(packType);
  assert(archetype);
  return archetype;
}

static void bindElementSignatureRequirementsAtIndex(
    IRGenFunction &IGF, OpenedElementContext const &context, llvm::Value *index,
    DynamicMetadataRequest request) {
  enumerateGenericSignatureRequirements(
      context.signature, [&](GenericRequirement requirement) {
        switch (requirement.getKind()) {
        case GenericRequirement::Kind::Shape:
        case GenericRequirement::Kind::Metadata:
        case GenericRequirement::Kind::WitnessTable:
          break;
        case GenericRequirement::Kind::MetadataPack: {
          auto ty = requirement.getTypeParameter();
          auto patternPackArchetype = getMappedPackArchetypeType(context, ty);
          auto response =
              IGF.emitTypeMetadataRef(patternPackArchetype, request);
          auto elementArchetype =
              context.environment
                  ->mapContextualPackTypeIntoElementContext(
                      patternPackArchetype)
                  ->getCanonicalType();
          auto *patternPack = response.getMetadata();
          auto elementMetadata = bindMetadataAtIndex(
              IGF, elementArchetype, patternPack, index, request);
          assert(elementMetadata);
          (void)elementMetadata;
          break;
        }
        case GenericRequirement::Kind::WitnessTablePack: {
          auto ty = requirement.getTypeParameter();
          auto proto = requirement.getProtocol();
          auto patternPackArchetype = getMappedPackArchetypeType(context, ty);
          auto elementArchetype =
              context.environment
                  ->mapContextualPackTypeIntoElementContext(
                      patternPackArchetype)
                  ->getCanonicalType();
          llvm::Value *_metadata = nullptr;
          auto packConformance =
              context.signature->lookupConformance(ty, proto);
          auto *wtablePack = emitWitnessTableRef(IGF, patternPackArchetype,
                                                 &_metadata, packConformance);
          auto elementConformance =
              context.signature->lookupConformance(ty, proto);
          auto *wtable = bindWitnessTableAtIndex(
              IGF, elementArchetype, elementConformance, wtablePack, index);
          assert(wtable);
          (void)wtable;
          break;
        }
        }
      });
}

static llvm::Value *emitPackExpansionElementMetadata(
    IRGenFunction &IGF, OpenedElementContext context, CanType patternTy,
    llvm::Value *index, DynamicMetadataRequest request) {
  bindElementSignatureRequirementsAtIndex(IGF, context, index, request);

  // Replace pack archetypes with element archetypes in the pattern type.
  auto instantiatedPatternTy =
      context.environment
          ->mapContextualPackTypeIntoElementContext(patternTy);

  // Emit the element metadata.
  auto element = IGF.emitTypeMetadataRef(instantiatedPatternTy, request)
      .getMetadata();
  return element;
}

/// Store the values corresponding to the specified pack expansion \p
/// expansionTy for each index in its range [dynamicIndex, dynamicIndex +
/// dynamicLength) produced by the provided function \p elementForIndex into
/// the indicated buffer \p pack.
static void emitPackExpansionPack(
    IRGenFunction &IGF, Address pack, CanPackExpansionType expansionTy,
    llvm::Value *dynamicIndex, llvm::Value *dynamicLength,
    function_ref<llvm::Value *(llvm::Value *)> elementForIndex) {
  auto *prev = IGF.Builder.GetInsertBlock();
  auto *check = IGF.createBasicBlock("pack-expansion-check");
  auto *loop = IGF.createBasicBlock("pack-expansion-loop");
  auto *rest = IGF.createBasicBlock("pack-expansion-rest");

  IGF.Builder.CreateBr(check);
  IGF.Builder.emitBlock(check);

  // An index into the source metadata pack.
  auto *phi = IGF.Builder.CreatePHI(IGF.IGM.SizeTy, 2);
  phi->addIncoming(llvm::ConstantInt::get(IGF.IGM.SizeTy, 0), prev);

  // If we reach the end, jump to the continuation block.
  auto *cond = IGF.Builder.CreateICmpULT(phi, dynamicLength);
  IGF.Builder.CreateCondBr(cond, loop, rest);

  IGF.Builder.emitBlock(loop);
  ConditionalDominanceScope condition(IGF);

  auto *element = elementForIndex(phi);

  // Store the element metadata into to the current destination index.
  auto *eltIndex = IGF.Builder.CreateAdd(dynamicIndex, phi);
  Address eltPtr(
      IGF.Builder.CreateInBoundsGEP(pack.getElementType(),
                                    pack.getAddress(),
                                    eltIndex),
      pack.getElementType(),
      pack.getAlignment());

  IGF.Builder.CreateStore(element, eltPtr);

  // Increment our counter.
  auto *next = IGF.Builder.CreateAdd(phi,
                                     llvm::ConstantInt::get(IGF.IGM.SizeTy, 1));

  phi->addIncoming(next, loop);

  // Repeat the loop.
  IGF.Builder.CreateBr(check);

  // Fall through.
  IGF.Builder.emitBlock(rest);
}

static void emitPackExpansionMetadataPack(IRGenFunction &IGF, Address pack,
                                          CanPackExpansionType expansionTy,
                                          llvm::Value *dynamicIndex,
                                          llvm::Value *dynamicLength,
                                          DynamicMetadataRequest request) {
  emitPackExpansionPack(
      IGF, pack, expansionTy, dynamicIndex, dynamicLength, [&](auto *index) {
        auto context =
            OpenedElementContext::createForContextualExpansion(IGF.IGM.Context, expansionTy);
        auto patternTy = expansionTy.getPatternType();
        return emitPackExpansionElementMetadata(IGF, context, patternTy, index,
                                                request);
      });
}

std::pair<StackAddress, llvm::Value *>
irgen::emitTypeMetadataPack(IRGenFunction &IGF, CanPackType packType,
                            DynamicMetadataRequest request) {
  auto *shape = IGF.emitPackShapeExpression(packType);

  if (auto *constantInt = dyn_cast<llvm::ConstantInt>(shape)) {
    assert(packType->getNumElements() == constantInt->getValue());
    auto pack =
        StackAddress(emitFixedSizeMetadataPackRef(IGF, packType, request));
    IGF.recordStackPackMetadataAlloc(pack, constantInt);
    return {pack, constantInt};
  }

  assert(packType->containsPackExpansionType());
  auto pack = IGF.emitDynamicAlloca(IGF.IGM.TypeMetadataPtrTy, shape,
                                    IGF.IGM.getPointerAlignment(),
                                    /*allowTaskAlloc=*/true);

  auto visitFn =
    [&](CanType eltTy, unsigned staticIndex,
        llvm::Value *dynamicIndex,
        llvm::Value *dynamicLength) {
      if (staticIndex != 0 || dynamicIndex == nullptr) {
        auto *constant = llvm::ConstantInt::get(IGF.IGM.SizeTy, staticIndex);
        accumulateSum(IGF, dynamicIndex, constant);
      }

      if (auto expansionTy = dyn_cast<PackExpansionType>(eltTy)) {
        emitPackExpansionMetadataPack(IGF, pack.getAddress(), expansionTy,
                                      dynamicIndex, dynamicLength, request);
      } else {
        Address eltPtr(
          IGF.Builder.CreateInBoundsGEP(pack.getAddress().getElementType(),
                                        pack.getAddressPointer(),
                                        dynamicIndex),
          pack.getAddress().getElementType(),
          pack.getAlignment());

        auto metadata = IGF.emitTypeMetadataRef(eltTy, request).getMetadata();
        IGF.Builder.CreateStore(metadata, eltPtr);
      }
    };

  visitPackExplosion(IGF, packType, visitFn);
  IGF.recordStackPackMetadataAlloc(pack, shape);

  return {pack, shape};
}

static llvm::Optional<unsigned> countForShape(llvm::Value *shape) {
  if (auto *constant = dyn_cast<llvm::ConstantInt>(shape))
    return constant->getValue().getZExtValue();
  return llvm::None;
}

MetadataResponse
irgen::emitTypeMetadataPackRef(IRGenFunction &IGF, CanPackType packType,
                               DynamicMetadataRequest request) {
  if (auto result = tryGetLocalPackTypeMetadata(IGF, packType, request))
    return result;

  StackAddress pack;
  llvm::Value *shape;
  std::tie(pack, shape) = emitTypeMetadataPack(IGF, packType, request);

  auto *metadata = pack.getAddress().getAddress();
  metadata = IGF.Builder.CreatePointerCast(
      metadata, IGF.IGM.TypeMetadataPtrTy->getPointerTo());

  if (!IGF.canStackPromotePackMetadata()) {
    metadata = IGF.Builder.CreateCall(
        IGF.IGM.getAllocateMetadataPackFunctionPointer(), {metadata, shape});

    cleanupTypeMetadataPack(IGF, pack, shape);
  }

  auto response = MetadataResponse::forComplete(metadata);
  IGF.setScopedLocalTypeMetadata(packType, response);

  return response;
}

static Address emitFixedSizeWitnessTablePack(IRGenFunction &IGF,
                                             CanPackType packType,
                                             PackConformance *packConformance) {
  assert(!packType->containsPackExpansionType());

  unsigned elementCount = packType->getNumElements();
  auto allocType =
      llvm::ArrayType::get(IGF.IGM.WitnessTablePtrTy, elementCount);

  auto pack = IGF.createAlloca(allocType, IGF.IGM.getPointerAlignment());
  IGF.Builder.CreateLifetimeStart(pack,
                                  IGF.IGM.getPointerSize() * elementCount);

  for (unsigned i : indices(packType->getElementTypes())) {
    Address slot =
        IGF.Builder.CreateStructGEP(pack, i, IGF.IGM.getPointerSize());

    auto conformance = packConformance->getPatternConformances()[i];
    llvm::Value *_metadata = nullptr;
    auto *wtable =
        emitWitnessTableRef(IGF, packType.getElementType(i),
                            /*srcMetadataCache=*/&_metadata, conformance);

    IGF.Builder.CreateStore(wtable, slot);
  }

  return pack;
}

static llvm::Value *emitPackExpansionElementWitnessTable(
    IRGenFunction &IGF, OpenedElementContext context, CanType patternTy,
    ProtocolConformanceRef conformance, llvm::Value **srcMetadataCache,
    llvm::Value *index) {
  bindElementSignatureRequirementsAtIndex(IGF, context, index,
                                          MetadataState::Complete);

  // Replace pack archetypes with element archetypes in the pattern type.
  auto instantiatedPatternTy =
      context.environment->mapContextualPackTypeIntoElementContext(patternTy);
  auto instantiatedConformance =
      context.environment->getGenericSignature()->lookupConformance(
          instantiatedPatternTy, conformance.getRequirement());

  // Emit the element witness table.
  auto *wtable = emitWitnessTableRef(IGF, instantiatedPatternTy,
                                     srcMetadataCache, instantiatedConformance);
  return wtable;
}

static void emitPackExpansionWitnessTablePack(
    IRGenFunction &IGF, Address pack, CanPackExpansionType expansionTy,
    ProtocolConformanceRef conformance, llvm::Value *dynamicIndex,
    llvm::Value *dynamicLength) {
  emitPackExpansionPack(
      IGF, pack, expansionTy, dynamicIndex, dynamicLength, [&](auto *index) {
        llvm::Value *_metadata = nullptr;
        auto context =
            OpenedElementContext::createForContextualExpansion(IGF.IGM.Context, expansionTy);
        auto patternTy = expansionTy.getPatternType();
        return emitPackExpansionElementWitnessTable(
            IGF, context, patternTy, conformance,
            /*srcMetadataCache=*/&_metadata, index);
      });
}

std::pair<StackAddress, llvm::Value *>
irgen::emitWitnessTablePack(IRGenFunction &IGF, CanPackType packType,
                            PackConformance *packConformance) {
  auto *shape = IGF.emitPackShapeExpression(packType);

  if (auto *constantInt = dyn_cast<llvm::ConstantInt>(shape)) {
    assert(packType->getNumElements() == constantInt->getValue());
    auto pack = StackAddress(
        emitFixedSizeWitnessTablePack(IGF, packType, packConformance));
    IGF.recordStackPackWitnessTableAlloc(pack, constantInt);
    return {pack, constantInt};
  }

  assert(packType->containsPackExpansionType());
  auto pack = IGF.emitDynamicAlloca(IGF.IGM.WitnessTablePtrTy, shape,
                                    IGF.IGM.getPointerAlignment(),
                                    /*allowTaskAlloc=*/true);

  auto index = 0;
  auto visitFn = [&](CanType eltTy, unsigned staticIndex,
                     llvm::Value *dynamicIndex, llvm::Value *dynamicLength) {
    if (staticIndex != 0 || dynamicIndex == nullptr) {
      auto *constant = llvm::ConstantInt::get(IGF.IGM.SizeTy, staticIndex);
      accumulateSum(IGF, dynamicIndex, constant);
    }

    auto conformance = packConformance->getPatternConformances()[index];
    if (auto expansionTy = dyn_cast<PackExpansionType>(eltTy)) {
      emitPackExpansionWitnessTablePack(IGF, pack.getAddress(), expansionTy,
                                        conformance, dynamicIndex,
                                        dynamicLength);
    } else {
      Address eltPtr(
          IGF.Builder.CreateInBoundsGEP(pack.getAddress().getElementType(),
                                        pack.getAddressPointer(), dynamicIndex),
          pack.getAddress().getElementType(), pack.getAlignment());

      llvm::Value *_metadata = nullptr;
      auto *wtable = emitWitnessTableRef(
          IGF, eltTy, /*srcMetadataCache=*/&_metadata, conformance);
      IGF.Builder.CreateStore(wtable, eltPtr);
    }
    ++index;
  };

  visitPackExplosion(IGF, packType, visitFn);
  IGF.recordStackPackWitnessTableAlloc(pack, shape);

  return {pack, shape};
}

static void cleanupWitnessTablePackImpl(IRGenFunction &IGF, StackAddress pack,
                                        llvm::Value *shape) {

  if (pack.getExtraInfo()) {
    IGF.emitDeallocateDynamicAlloca(pack);
  } else if (auto count = countForShape(shape)) {
    IGF.Builder.CreateLifetimeEnd(pack.getAddress(),
                                  IGF.IGM.getPointerSize() * (count.value()));
  }
}

void irgen::cleanupWitnessTablePack(IRGenFunction &IGF, StackAddress pack,
                                    llvm::Value *shape) {
  cleanupWitnessTablePackImpl(IGF, pack, shape);
  IGF.eraseStackPackWitnessTableAlloc(pack, shape);
}

void irgen::cleanupStackAllocPacks(IRGenFunction &IGF,
                                   ArrayRef<StackPackAlloc> allocs) {
  for (auto alloc : llvm::reverse(allocs)) {
    StackAddress addr;
    uint8_t kind;
    llvm::Value *shape;
    std::tie(addr, shape, kind) = alloc;

    switch ((GenericRequirement::Kind)kind) {
    case GenericRequirement::Kind::MetadataPack:
      cleanupTypeMetadataPackImpl(IGF, addr, shape);
      break;
    case GenericRequirement::Kind::WitnessTablePack:
      cleanupWitnessTablePackImpl(IGF, addr, shape);
      break;
    default:
      llvm_unreachable("bad requirement in stack pack alloc");
    }
  }
}

void IRGenFunction::recordStackPackMetadataAlloc(StackAddress addr,
                                                 llvm::Value *shape) {
  OutstandingStackPackAllocs.insert(
      {addr, shape, (uint8_t)GenericRequirement::Kind::MetadataPack});
}

void IRGenFunction::eraseStackPackMetadataAlloc(StackAddress addr,
                                                llvm::Value *shape) {
  auto removed = OutstandingStackPackAllocs.remove(
      {addr, shape, (uint8_t)GenericRequirement::Kind::MetadataPack});
  assert(removed && "erased stack pack metadata addr that wasn't recorded!?");
  (void)removed;
}

void IRGenFunction::recordStackPackWitnessTableAlloc(StackAddress addr,
                                                     llvm::Value *shape) {
  OutstandingStackPackAllocs.insert(
      {addr, shape, (uint8_t)GenericRequirement::Kind::WitnessTablePack});
}

void IRGenFunction::eraseStackPackWitnessTableAlloc(StackAddress addr,
                                                    llvm::Value *shape) {
  auto removed = OutstandingStackPackAllocs.remove(
      {addr, shape, (uint8_t)GenericRequirement::Kind::WitnessTablePack});
  assert(removed && "erased stack pack metadata addr that wasn't recorded!?");
  (void)removed;
}

llvm::Value *irgen::emitWitnessTablePackRef(IRGenFunction &IGF,
                                            CanPackType packType,
                                            PackConformance *conformance) {
  assert(Lowering::TypeConverter::protocolRequiresWitnessTable(
             conformance->getProtocol()) &&
         "looking up witness table for protocol that doesn't have one");

  if (auto *wtable = tryGetLocalPackTypeData(
          IGF, packType,
          LocalTypeDataKind::forAbstractProtocolWitnessTable(
              conformance->getProtocol())))
    return wtable;

  auto localDataKind =
      LocalTypeDataKind::forProtocolWitnessTablePack(conformance);

  if (auto *wtable = tryGetLocalPackTypeData(IGF, packType, localDataKind))
    return wtable;

  StackAddress pack;
  llvm::Value *shape;
  std::tie(pack, shape) = emitWitnessTablePack(IGF, packType, conformance);

  auto *result = pack.getAddress().getAddress();
  result = IGF.Builder.CreatePointerCast(
      result, IGF.IGM.WitnessTablePtrTy->getPointerTo());

  if (!IGF.canStackPromotePackMetadata()) {
    result = IGF.Builder.CreateCall(
        IGF.IGM.getAllocateWitnessTablePackFunctionPointer(), {result, shape});

    cleanupWitnessTablePack(IGF, pack, shape);
  }

  IGF.setScopedLocalTypeData(packType, localDataKind, result);

  return result;
}

llvm::Value *irgen::emitTypeMetadataPackElementRef(
    IRGenFunction &IGF, CanPackType packType,
    ArrayRef<ProtocolConformanceRef> conformances, llvm::Value *index,
    DynamicMetadataRequest request,
    llvm::SmallVectorImpl<llvm::Value *> &wtables) {
  // If the packs have already been materialized, just gep into them.
  auto materializedMetadataPack =
      tryGetLocalPackTypeMetadata(IGF, packType, request);
  llvm::SmallVector<llvm::Value *> materializedWtablePacks;
  for (auto conformance : conformances) {
    auto *wtablePack = tryGetLocalPackTypeData(
        IGF, packType,
        LocalTypeDataKind::forProtocolWitnessTable(conformance));
    materializedWtablePacks.push_back(wtablePack);
  }
  if (materializedMetadataPack &&
      llvm::all_of(materializedWtablePacks,
                   [](auto *wtablePack) { return wtablePack; })) {
    auto *metadataPack = materializedMetadataPack.getMetadata();
    auto *metadata = loadMetadataAtIndex(IGF, metadataPack, index);
    for (auto *wtablePack : materializedWtablePacks) {
      auto *wtable = loadWitnessTableAtIndex(IGF, wtablePack, index);
      wtables.push_back(wtable);
    }
    return metadata;
  }

  // Otherwise, in general, there's no already available array of metadata
  // which can be indexed into.
  auto *shape = IGF.emitPackShapeExpression(packType);

  // If the shape and the index are both constant, the type for which metadata
  // will be emitted is statically available.
  auto *constantShape = dyn_cast<llvm::ConstantInt>(shape);
  auto *constantIndex = dyn_cast<llvm::ConstantInt>(index);
  if (constantShape && constantIndex) {
    assert(packType->getNumElements() == constantShape->getValue());
    auto index = constantIndex->getValue().getZExtValue();
    assert(packType->getNumElements() > index);
    auto ty = packType.getElementType(index);
    auto response = IGF.emitTypeMetadataRef(ty, request);
    auto *metadata = response.getMetadata();
    for (auto conformance : conformances) {
      auto patternConformance = conformance.getPack()
          ->getPatternConformances()[index];
      auto *wtable =
          emitWitnessTableRef(IGF, ty, /*srcMetadataCache=*/&metadata,
                              patternConformance);
      wtables.push_back(wtable);
    }
    return metadata;
  }

  // A pack consists of types and pack expansion types.  An example:
  //   {repeat each T, Int, repeat each T, repeat each U, String},
  // The above type has length 5.  The type "repeat each U" is at index 3.
  //
  // A pack _explosion_ is notionally obtained by flat-mapping the pack by the
  // the operation of "listing elements" in pack expansion types.
  //
  // The explosion of the example pack looks like
  //   {T_0, T_1, ..., Int, T_0, T_1, ..., U_0, U_1, ..., String}
  //    ^^^^^^^^^^^^^
  //    the runtime components of "each T"
  //
  // We have an index into the explosion,
  //
  //  {T_0, T_1, ..., Int, T_0, T_1, ..., U_0, U_1, ... String}
  //   ------------%index------------>
  //
  // and we need to obtain the element in the explosion corresponding to it.
  //
  //  {T_0, T_1, ..., Int, T_0, T_1, ..., T_k, ..., U_0, U_1, ... String}
  //   ------------%index---------------> ^^^
  //
  // Unfortunately, the explosion has not (the first check in this function)
  // been materialized--and doing so is likely wasteful--so we can't simply
  // index into some array.
  //
  // Instead, _notionally_, we will "compute"
  // (1) the index into the _pack_ and
  //     {repeat each T, Int, repeat each T, repeat each U, String}
  //      ------%outer------> ^^^^^^^^^^^^^
  // (2) the index within the elements of the pack expansion type
  //     {T_0, T_2, ..., T_k, ...}
  //      ----%inner---> ^^^
  //
  // In fact, we won't ever materialize %outer into any register.  Instead, we
  // can just brach to materializing the metadata (and witness tables) once
  // we've determined which outer element's range contains %index.
  //
  // As for %inner, it will only be materialized in those blocks corresponding
  // to pack expansions.
  //
  // Create the following control flow:
  //
  // +-------+      t_0 is not             t_N _is_ an
  // |entry: |      an expansion           expansion
  // |...    |      +----------+           +----------+    +----------+
  // |...    |  --> |check_0:  | -> ... -> |check_N:  | -> |trap:     |
  // |       |      | %i == %u0|           | %i < %uN |    | llvm.trap|
  // +-------+      +----------+           +----------+    +----------+
  //                %outer = 0             %outer = N
  //                    |                      |
  //                    V                      V
  //               +----------+           +-----------------------+
  //               |emit_1:   |           |emit_N:                |
  //               | %inner=0 |           | %inner = %index - %lN |
  //               | %m_1 =   |           | %m_N =                |
  //               | %wt_1_1= |           | %wt_1_N =             |
  //               | %wt_k_1= |           | %wt_k_N =             |
  //               +----------+           +-----------------------+
  //                    |                      |
  //                    V                      V
  //               +-------------------------------------------
  //               |exit:
  //               | %m = phi [ %m_1, %emit_1 ],
  //               |                 ...
  //               |          [ %m_N, %emit_N ]
  //               | %wt_1 = phi [ %wt_1_1, %emit_1 ],
  //               |                 ...
  //               |             [ %m_1_N, %emit_N ]
  //               | ...
  //               | %wt_k = phi [ %wt_k_1, %emit_1 ],
  //               |                 ...
  //               |             [ %m_k_N, %emit_N ]
  auto *current = IGF.Builder.GetInsertBlock();

  // Terminate the block that branches to continue checking or metadata/wtable
  // emission depending on whether the index is in the pack expansion's bounds.
  auto emitCheckBranch = [&IGF](llvm::Value *condition,
                                llvm::BasicBlock *inBounds,
                                llvm::BasicBlock *outOfBounds) {
    if (condition) {
      IGF.Builder.CreateCondBr(condition, inBounds, outOfBounds);
    } else {
      assert(!inBounds &&
             "no condition to check but a materialization block!?");
      IGF.Builder.CreateBr(outOfBounds);
    }
  };

  // The block which emission will continue in after we finish emitting
  // metadata/wtables for this element.
  auto *exit = IGF.createBasicBlock("pack-index-element-exit");
  IGF.Builder.emitBlock(exit);
  auto *metadataPhi = IGF.Builder.CreatePHI(IGF.IGM.TypeMetadataPtrTy,
                                            packType.getElementTypes().size());
  llvm::SmallVector<llvm::PHINode *, 2> wtablePhis;
  wtablePhis.reserve(conformances.size());
  for (auto idx : indices(conformances)) {
    (void)idx;
    wtablePhis.push_back(IGF.Builder.CreatePHI(
        IGF.IGM.WitnessTablePtrTy, packType.getElementTypes().size()));
  }

  IGF.Builder.SetInsertPoint(current);
  // The previous checkBounds' block's comparision of %index.  Use it to emit a
  // branch to the current block or the previous block's metadata/wtable
  // emission block.
  llvm::Value *previousCondition = nullptr;
  // The previous type's materialize block.  Use it as the inBounds target when
  // branching from the previous block.
  llvm::BasicBlock *previousInBounds = nullptr;
  // The lower bound of indices for the current pack expansion.  Inclusive.
  llvm::Value *lowerBound = llvm::ConstantInt::get(IGF.IGM.SizeTy, 0);
  for (unsigned i = 0, e = packType->getNumElements(); i < e; ++i) {
    auto elementTy = packType.getElementType(i);

    // The block within which it will be checked whether %index corresponds to
    // an element of the pack expansion elementTy.
    auto *checkBounds = IGF.createBasicBlock("pack-index-element-bounds");
    // Finish emitting the previous block, either entry or check_i-1.
    //
    // Branch from the previous bounds-check block either to this bounds-check
    // block or to the previous metadata/wtable emission block.
    emitCheckBranch(previousCondition, previousInBounds, checkBounds);

    // (1) Emit check_i {{
    IGF.Builder.emitBlock(checkBounds);

    ConditionalDominanceScope dominanceScope(IGF);

    // The upper bound for the current pack expansion.  Exclusive.
    llvm::Value *upperBound = nullptr;
    llvm::Value *condition = nullptr;
    if (auto expansionTy = dyn_cast<PackExpansionType>(elementTy)) {
      auto reducedShape = expansionTy.getCountType();
      auto *length = IGF.emitPackShapeExpression(reducedShape);
      upperBound = IGF.Builder.CreateAdd(lowerBound, length);
      // %index < %upperBound
      //
      // It's not necessary to check that %index >= %lowerBound.  Either
      // elementTy is the first element type in packType or we branched here
      // from some series of checkBounds blocks in each of which it was
      // determined that %index is greater than the indices of the
      // corresponding element type.
      condition = IGF.Builder.CreateICmpULT(index, upperBound);
    } else {
      upperBound = IGF.Builder.CreateAdd(
          lowerBound, llvm::ConstantInt::get(IGF.IGM.SizeTy, 1));
      // %index == %lowerBound
      condition = IGF.Builder.CreateICmpEQ(lowerBound, index);
    }
    // }} Finished emitting check_i, except for the terminator which will be
    //    emitted in the next iteration once the new outOfBounds block is
    //    available.

    // (2) Emit emit_i {{
    // The block within which the metadata/wtables corresponding to %inner will
    // be materialized.
    auto *materialize = IGF.createBasicBlock("pack-index-element-metadata");
    IGF.Builder.emitBlock(materialize);

    llvm::Value *metadata = nullptr;
    llvm::SmallVector<llvm::Value *, 2> wtables;
    wtables.reserve(conformances.size());
    if (auto expansionTy = dyn_cast<PackExpansionType>(elementTy)) {
      // Actually materialize %inner.  Then use it to get the metadata from the
      // pack expansion at that index.
      auto *relativeIndex = IGF.Builder.CreateSub(index, lowerBound);
      auto context =
          OpenedElementContext::createForContextualExpansion(IGF.IGM.Context, expansionTy);
      auto patternTy = expansionTy.getPatternType();
      metadata = emitPackExpansionElementMetadata(IGF, context, patternTy,
                                                  relativeIndex, request);
      for (auto conformance : conformances) {
        auto patternConformance = conformance.getPack()->getPatternConformances()[i];
        auto *wtable = emitPackExpansionElementWitnessTable(
            IGF, context, patternTy, patternConformance,
            &metadata, relativeIndex);
        wtables.push_back(wtable);
      }
    } else {
      metadata = IGF.emitTypeMetadataRef(elementTy, request).getMetadata();
      for (auto conformance : conformances) {
        auto patternConformance = conformance.getPack()->getPatternConformances()[i];
        llvm::Value *_metadata = nullptr;
        auto *wtable =
            emitWitnessTableRef(IGF, elementTy, /*srcMetadataCache=*/&_metadata,
                                patternConformance);
        wtables.push_back(wtable);
      }
    }
    metadataPhi->addIncoming(metadata, IGF.Builder.GetInsertBlock());
    for (auto i : indices(wtables)) {
      auto *wtable = wtables[i];
      auto *wtablePhi = wtablePhis[i];
      wtablePhi->addIncoming(wtable, IGF.Builder.GetInsertBlock());
    }
    IGF.Builder.CreateBr(exit);
    // }} Finished emitting emit_i.

    // Switch back to emitting check_i.  The next iteration will emit its
    // terminator.
    IGF.Builder.SetInsertPoint(checkBounds);

    // Set up the values for the next iteration.
    previousInBounds = materialize;
    previousCondition = condition;
    lowerBound = upperBound;
  }
  auto *trap = IGF.createBasicBlock("pack-index-element-trap");
  emitCheckBranch(previousCondition, previousInBounds, trap);

  IGF.Builder.emitBlock(trap);
  IGF.emitTrap("Variadic generic index out of bounds",
               /*EmitUnreachable=*/true);

  IGF.Builder.SetInsertPoint(exit);
  for (auto *wtablePhi : wtablePhis) {
    wtables.push_back(wtablePhi);
  }
  return metadataPhi;
}

void irgen::bindOpenedElementArchetypesAtIndex(IRGenFunction &IGF,
                                               GenericEnvironment *environment,
                                               llvm::Value *index) {
  assert(environment->getKind() == GenericEnvironment::Kind::OpenedElement);

  // Record the generic type parameters of interest.
  llvm::SmallPtrSet<CanType, 2> openablePackParams;
  environment->forEachPackElementGenericTypeParam([&](auto *genericParam) {
    openablePackParams.insert(genericParam->getCanonicalType());
  });

  auto subs = environment->getPackElementContextSubstitutions();

  // Find the archetypes and conformances which must be bound.
  llvm::SmallSetVector<CanType, 2> types;
  llvm::DenseMap<CanType, llvm::SmallVector<ProtocolConformanceRef, 2>>
      conformancesForType;
  auto isDerivedFromPackElementGenericTypeParam = [&](CanType ty) -> bool {
    // Is this type itself an openable pack parameter OR a dependent type of
    // one?
    return openablePackParams.contains(
        ty->getRootGenericParam()->getCanonicalType());
  };

  enumerateGenericSignatureRequirements(
      environment->getGenericSignature().getCanonicalSignature(),
      [&](GenericRequirement requirement) {
        switch (requirement.getKind()) {
        case GenericRequirement::Kind::MetadataPack: {
          auto ty = requirement.getTypeParameter();
          if (!isDerivedFromPackElementGenericTypeParam(ty))
            return;
          types.insert(ty);
          conformancesForType.insert({ty, {}});
          break;
        }
        case GenericRequirement::Kind::WitnessTablePack: {
          auto ty = requirement.getTypeParameter();
          if (!isDerivedFromPackElementGenericTypeParam(ty))
            return;
          types.insert(ty);
          auto iterator = conformancesForType.insert({ty, {}}).first;

          auto conformance = subs.lookupConformance(
              ty, requirement.getProtocol());
          iterator->getSecond().push_back(conformance);
          break;
        }
        case GenericRequirement::Kind::Shape:
        case GenericRequirement::Kind::Metadata:
        case GenericRequirement::Kind::WitnessTable:
          break;
        }
      });

  // For each archetype to be bound, find the corresponding conformances and
  // bind the metadata and wtables.
  for (auto ty : types) {
    auto conformances = conformancesForType.find(ty)->getSecond();
    auto archetype = cast<ElementArchetypeType>(
        environment->mapPackTypeIntoElementContext(ty)->getCanonicalType());
    auto packType = cast<PackType>(ty.subst(subs)->getCanonicalType());

    llvm::SmallVector<llvm::Value *, 2> wtables;
    auto *metadata = emitTypeMetadataPackElementRef(
        IGF, packType, conformances, index, MetadataState::Complete, wtables);

    auto reqt = GenericRequirement::forMetadata(archetype);
    bindGenericRequirement(IGF, reqt, metadata, MetadataState::Complete,
                           SubstitutionMap());

    assert(conformances.size() == wtables.size());
    for (unsigned i : indices(wtables)) {
      auto reqt = GenericRequirement::forWitnessTable(
          archetype, conformances[i].getRequirement());
      bindGenericRequirement(IGF, reqt, wtables[i], MetadataState::Complete,
                             SubstitutionMap());
    }
  }
}

static void cleanupTypeMetadataPackImpl(IRGenFunction &IGF, StackAddress pack,
                                        llvm::Value *shape) {
  if (pack.getExtraInfo()) {
    IGF.emitDeallocateDynamicAlloca(pack);
  } else if (auto count = countForShape(shape)) {
    IGF.Builder.CreateLifetimeEnd(pack.getAddress(),
                                  IGF.IGM.getPointerSize() * (*count));
  }
}

void irgen::cleanupTypeMetadataPack(IRGenFunction &IGF, StackAddress pack,
                                    llvm::Value *shape) {
  cleanupTypeMetadataPackImpl(IGF, pack, shape);
  IGF.eraseStackPackMetadataAlloc(pack, shape);
}

Address irgen::emitStorageAddressOfPackElement(IRGenFunction &IGF, Address pack,
                                               llvm::Value *index,
                                               SILType elementType,
                                               CanSILPackType packType) {
  // When we have an indirect pack, the elements are pointers, so we can
  // simply index into that flat array.
  assert(elementType.isAddress() && "direct packs not currently supported");
  auto elementSize = getPackElementSize(IGF.IGM, packType);
  auto elementAddress = IGF.Builder.CreateArrayGEP(pack, index, elementSize);
  return IGF.Builder.CreateElementBitCast(elementAddress,
                                 IGF.IGM.getStoragePointerType(elementType));
}

Size irgen::getPackElementSize(IRGenModule &IGM, CanSILPackType ty) {
  assert(ty->isElementAddress() && "not implemented for direct packs");
  return IGM.getPointerSize();
}

StackAddress irgen::allocatePack(IRGenFunction &IGF, CanSILPackType packType) {
  auto *shape = IGF.emitPackShapeExpression(packType);

  auto elementSize = getPackElementSize(IGF.IGM, packType);

  if (auto *constantInt = dyn_cast<llvm::ConstantInt>(shape)) {
    assert(packType->getNumElements() == constantInt->getValue());
    (void)constantInt;
    assert(!packType->containsPackExpansionType());
    unsigned elementCount = packType->getNumElements();
    auto allocType = llvm::ArrayType::get(
        IGF.IGM.OpaquePtrTy, elementCount);

    auto addr = IGF.createAlloca(allocType, IGF.IGM.getPointerAlignment());
    IGF.Builder.CreateLifetimeStart(addr, elementSize * elementCount);

    // We have an [N x opaque*]*; we need an opaque**.
    addr = IGF.Builder.CreateElementBitCast(addr, IGF.IGM.OpaquePtrTy);
    return addr;
  }

  assert(packType->containsPackExpansionType());
  auto addr = IGF.emitDynamicAlloca(IGF.IGM.OpaquePtrTy, shape,
                                    IGF.IGM.getPointerAlignment(),
                                    /*allowTaskAlloc=*/true);

  return addr;
}

void irgen::deallocatePack(IRGenFunction &IGF, StackAddress addr, CanSILPackType packType) {
  if (packType->containsPackExpansionType()) {
    IGF.emitDeallocateDynamicAlloca(addr);
    return;
  } 

  auto elementSize = getPackElementSize(IGF.IGM, packType);
  auto elementCount = packType->getNumElements();
  IGF.Builder.CreateLifetimeEnd(addr.getAddress(),
                                elementSize * elementCount);
}

static unsigned getConstantLabelsLength(CanTupleType type) {
  unsigned total = 0;

  for (auto elt : type->getElements()) {
    if (elt.getType()->is<PackExpansionType>()) {
      assert(!elt.hasName());
      continue;
    }

    if (elt.hasName()) {
      assert(!elt.getType()->is<PackExpansionType>());
      total += elt.getName().getLength();
    }

    ++total;
  }

  return total;
}

/// Emit the dynamic label string for a tuple type containing pack
/// expansions.
///
/// The basic idea is that the static label string is "stretched out".
/// Pack expansion elements are unlabeled, so they appear as a single
/// blank space in the static label string. We replace this with the
/// appropriate number of blank spaces, given the dynamic length of
/// the pack.
llvm::Optional<StackAddress>
irgen::emitDynamicTupleTypeLabels(IRGenFunction &IGF,
                                  CanTupleType type,
                                  CanPackType packType,
                                  llvm::Value *shapeExpression) {
  bool hasLabels = false;
  for (auto elt : type->getElements()) {
    hasLabels |= elt.hasName();
  }

  if (!hasLabels)
    return llvm::None;

  // Elements of pack expansion type are unlabeled, so the length of
  // the label string is the number of elements in the pack, plus the
  // sum of the lengths of the labels.
  llvm::Value *labelLength = llvm::ConstantInt::get(
      IGF.IGM.SizeTy, getConstantLabelsLength(type));
  labelLength = IGF.Builder.CreateAdd(shapeExpression, labelLength);

  // Leave root for a null byte at the end.
  labelLength = IGF.Builder.CreateAdd(labelLength,
      llvm::ConstantInt::get(IGF.IGM.SizeTy, 1));

  // Allocate space for the label string; we fill it in below.
  StackAddress labelString = IGF.emitDynamicAlloca(
                                IGF.IGM.Int8Ty, labelLength,
                                IGF.IGM.getPointerAlignment(),
                                /*allowTaskAlloc=*/true);

  // Get the static label string, where each pack expansion is one element.
  auto *staticLabelString = getTupleLabelsString(IGF.IGM, type);

  // The position in the static label string for to the current element.
  unsigned staticPosition = 0;

  // The position in the dynamic label string for to the current element.
  llvm::Value *dynamicPosition = llvm::ConstantInt::get(IGF.IGM.SizeTy, 0);

  // Number of expansions we've seen so far.
  unsigned numExpansions = 0;

  // Was there at least one label?
  bool sawLabel = false;

  auto visitFn = [&](CanType eltTy,
                     unsigned scalarIndex,
                     llvm::Value *dynamicIndex,
                     llvm::Value *dynamicLength) {
    auto elt = type->getElements()[scalarIndex + numExpansions];
    assert(eltTy == CanType(elt.getType()));

    // The destination address, where we put the current element's label.
    auto eltAddr = IGF.Builder.CreateArrayGEP(labelString.getAddress(),
                                              dynamicPosition, Size(1));

    // If we're looking at a pack expansion, insert the appropriate
    // number of blank spaces in the dynamic label string.
    if (isa<PackExpansionType>(eltTy)) {
      assert(!elt.hasName() && "Pack expansions cannot have labels");
      // Fill the dynamic label string with a blank label for each
      // dynamic element.
      IGF.Builder.CreateMemSet(
          eltAddr, llvm::ConstantInt::get(IGF.IGM.Int8Ty, ' '),
          dynamicLength);

      // We consumed one static label.
      staticPosition += 1;

      // We produced some number of dynamic labels.
      dynamicPosition = IGF.Builder.CreateAdd(dynamicPosition, dynamicLength);

      // We consumed an expansion.
      numExpansions += 1;

      return;
    }

    // Otherwise, we have a single scalar element, which deposits a single
    // label in the dynamic label string.
    unsigned length = 0;

    // Scalar elements may have labels.
    if (elt.hasName()) {
      // Index into the static label string.
      llvm::Constant *indices[] = {
        llvm::ConstantInt::get(IGF.IGM.SizeTy, staticPosition)
      };

      // The source address in the static label string.
      Address srcAddr(
          llvm::ConstantExpr::getInBoundsGetElementPtr(
              IGF.IGM.Int8Ty, staticLabelString,
              indices),
          IGF.IGM.Int8Ty, Alignment(1));

      // The number of bytes to copy; add one for the space at the end.
      length = elt.getName().getLength() + 1;

      // Desposit the label for this element in the dynamic label string.
      IGF.Builder.CreateMemCpy(eltAddr, srcAddr, Size(length));

      sawLabel = true;
    } else {
      length = 1;

      // There is no label. The static label string stores a blank space,
      // and we need to update the dynamic string for the same.
      IGF.Builder.CreateStore(
          llvm::ConstantInt::get(IGF.IGM.Int8Ty, ' '),
          eltAddr);
    }

    // We consumed one static label.
    staticPosition += length;

    // We produced one dynamic label.
    auto *constant = llvm::ConstantInt::get(IGF.IGM.SizeTy, length);
    accumulateSum(IGF, dynamicPosition, constant);
  };

  (void) visitPackExplosion(IGF, packType, visitFn);

  // Null-terminate the dynamic label string.
  auto eltAddr = IGF.Builder.CreateArrayGEP(labelString.getAddress(),
                                            dynamicPosition, Size(1));
  IGF.Builder.CreateStore(
          llvm::ConstantInt::get(IGF.IGM.Int8Ty, '\0'),
          eltAddr);

  assert(sawLabel);
  (void) sawLabel;

  return labelString;
}

StackAddress
irgen::emitDynamicFunctionParameterFlags(IRGenFunction &IGF,
                                         AnyFunctionType::CanParamArrayRef params,
                                         CanPackType packType,
                                         llvm::Value *shapeExpression) {
  auto array =
      IGF.emitDynamicAlloca(IGF.IGM.Int32Ty, shapeExpression,
                            Alignment(4), /*allowTaskAlloc=*/true);

  unsigned numExpansions = 0;

  auto visitFn = [&](CanType eltTy,
                     unsigned scalarIndex,
                     llvm::Value *dynamicIndex,
                     llvm::Value *dynamicLength) {
    if (scalarIndex != 0 || dynamicIndex == nullptr) {
      auto *constant = llvm::ConstantInt::get(IGF.IGM.SizeTy, scalarIndex);
      accumulateSum(IGF, dynamicIndex, constant);
    }

    auto elt = params[scalarIndex + numExpansions];
    auto flags = getABIParameterFlags(elt.getParameterFlags());
    auto flagsVal = llvm::ConstantInt::get(
        IGF.IGM.Int32Ty, flags.getIntValue());

    assert(eltTy == elt.getPlainType());

    // If we're looking at a pack expansion, insert the appropriate
    // number of flags fields.
    if (auto expansionTy = dyn_cast<PackExpansionType>(eltTy)) {
      emitPackExpansionPack(IGF, array.getAddress(), expansionTy,
                            dynamicIndex, dynamicLength,
                            [&](llvm::Value *) -> llvm::Value * {
                              return flagsVal;
                            });

      // We consumed an expansion.
      numExpansions += 1;

      return;
    }

    // The destination address, where we put the current element's flags field.
    Address eltAddr(
        IGF.Builder.CreateInBoundsGEP(array.getAddress().getElementType(),
                                      array.getAddressPointer(),
                                      dynamicIndex),
        array.getAddress().getElementType(),
        array.getAlignment());

    // Otherwise, we have a single scalar element, which deposits a single
    // flags field.
    IGF.Builder.CreateStore(flagsVal, eltAddr);
  };

  (void) visitPackExplosion(IGF, packType, visitFn);

  return array;
}