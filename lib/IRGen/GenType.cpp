//===--- GenType.cpp - Swift IR Generation For Types ----------------------===//
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
//  This file implements IR generation for types in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/ABI/MetadataValues.h"
#include "swift/AST/CanTypeVisitor.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/SourceManager.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/SILModule.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Path.h"
#include "clang/CodeGen/SwiftCallingConv.h"

#include "BitPatternBuilder.h"
#include "CallEmission.h"
#include "EnumPayload.h"
#include "LegacyLayoutFormat.h"
#include "LoadableTypeInfo.h"
#include "GenCall.h"
#include "GenMeta.h"
#include "GenPoly.h"
#include "GenProto.h"
#include "GenType.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "Address.h"
#include "Explosion.h"
#include "GenOpaque.h"
#include "HeapTypeInfo.h"
#include "IndirectTypeInfo.h"
#include "Outlining.h"
#include "ProtocolInfo.h"
#include "ReferenceTypeInfo.h"
#include "ScalarPairTypeInfo.h"
#include "NativeConventionSchema.h"
#include "IRGenMangler.h"
#include "NonFixedTypeInfo.h"

using namespace swift;
using namespace irgen;

Alignment IRGenModule::getCappedAlignment(Alignment align) {
  return std::min(align, Alignment(MaximumAlignment));
}

llvm::DenseMap<TypeBase *, const TypeInfo *> &
TypeConverter::Types_t::getCacheFor(bool isDependent, TypeConverter::Mode mode) {
  return (isDependent
          ? DependentCache[unsigned(mode)]
          : IndependentCache[unsigned(mode)]);
}

llvm::DenseMap<TypeBase *, const TypeLayoutEntry *> &
TypeConverter::Types_t::getTypeLayoutCacheFor(bool isDependent,
                                              TypeConverter::Mode mode) {
  return (isDependent ? DependentTypeLayoutCache[unsigned(mode)]
                      : IndependentTypeLayoutCache[unsigned(mode)]);
}

void TypeInfo::assign(IRGenFunction &IGF, Address dest, Address src,
                      IsTake_t isTake, SILType T, bool isOutlined) const {
  if (isTake) {
    assignWithTake(IGF, dest, src, T, isOutlined);
  } else {
    assignWithCopy(IGF, dest, src, T, isOutlined);
  }
}

void TypeInfo::initialize(IRGenFunction &IGF, Address dest, Address src,
                          IsTake_t isTake, SILType T, bool isOutlined) const {
  if (isTake) {
    initializeWithTake(IGF, dest, src, T, isOutlined);
  } else {
    initializeWithCopy(IGF, dest, src, T, isOutlined);
  }
}

bool TypeInfo::isSingleRetainablePointer(ResilienceExpansion expansion,
                                         ReferenceCounting *refcounting) const {
  return false;
}

ExplosionSchema TypeInfo::getSchema() const {
  ExplosionSchema schema;
  getSchema(schema);
  return schema;
}

TypeInfo::~TypeInfo() {
  if (nativeReturnSchema)
    delete nativeReturnSchema;
  if (nativeParameterSchema)
    delete nativeParameterSchema;
}

Address TypeInfo::getAddressForPointer(llvm::Value *ptr) const {
  assert(cast<llvm::PointerType>(ptr->getType())
             ->isOpaqueOrPointeeTypeMatches(getStorageType()));
  return Address(ptr, getStorageType(), getBestKnownAlignment());
}

Address TypeInfo::getUndefAddress() const {
  return Address(llvm::UndefValue::get(getStorageType()->getPointerTo(0)),
                 getStorageType(), getBestKnownAlignment());
}

/// Whether this type is known to be empty.
bool TypeInfo::isKnownEmpty(ResilienceExpansion expansion) const {
  if (auto fixed = dyn_cast<FixedTypeInfo>(this))
    return fixed->isKnownEmpty(expansion);
  return false;
}

const NativeConventionSchema &
TypeInfo::nativeReturnValueSchema(IRGenModule &IGM) const {
  if (nativeReturnSchema == nullptr)
    nativeReturnSchema = new NativeConventionSchema(IGM, this, true);
  return *nativeReturnSchema;
}

const NativeConventionSchema &
TypeInfo::nativeParameterValueSchema(IRGenModule &IGM) const {
  if (nativeParameterSchema == nullptr)
    nativeParameterSchema = new NativeConventionSchema(IGM, this, false);
  return *nativeParameterSchema;
}

/// Copy a value from one object to a new object, directly taking
/// responsibility for anything it might have.  This is like C++
/// move-initialization, except the old object will not be destroyed.
void FixedTypeInfo::initializeWithTake(IRGenFunction &IGF, Address destAddr,
                                       Address srcAddr, SILType T,
                                       bool isOutlined) const {
  assert(isBitwiseTakable(ResilienceExpansion::Maximal)
        && "non-bitwise-takable type must override default initializeWithTake");
  
  // Prefer loads and stores if we won't make a million of them.
  // Maybe this should also require the scalars to have a fixed offset.
  ExplosionSchema schema = getSchema();
  if (!schema.containsAggregate() && schema.size() <= 2) {
    auto &loadableTI = cast<LoadableTypeInfo>(*this);
    Explosion copy;
    loadableTI.loadAsTake(IGF, srcAddr, copy);
    loadableTI.initialize(IGF, copy, destAddr, isOutlined);
    return;
  }

  // Otherwise, use a memcpy.
  IGF.emitMemCpy(destAddr, srcAddr, getFixedSize());
}

/// Copy a value from one object to a new object.  This is just the
/// default implementation.
void LoadableTypeInfo::initializeWithCopy(IRGenFunction &IGF, Address destAddr,
                                          Address srcAddr, SILType T,
                                          bool isOutlined) const {
  // Use memcpy if that's legal.
  if (isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
    return initializeWithTake(IGF, destAddr, srcAddr, T, isOutlined);
  }

  // Otherwise explode and re-implode.
  if (isOutlined) {
    Explosion copy;
    loadAsCopy(IGF, srcAddr, copy);
    initialize(IGF, copy, destAddr, true);
  } else {
    OutliningMetadataCollector collector(IGF);
    // No need to collect anything because we assume loadable types can be
    // loaded without enums.
    collector.emitCallToOutlinedCopy(
        destAddr, srcAddr, T, *this, IsInitialization, IsNotTake);
  }
}

LoadedRef LoadableTypeInfo::loadRefcountedPtr(IRGenFunction &IGF,
                                              SourceLoc loc,
                                              Address addr) const {
  IGF.IGM.error(loc, "Can only load from an address that holds a reference to "
                "a refcounted type or an address of an optional reference.");
  llvm::report_fatal_error("loadRefcountedPtr: Invalid SIL in IRGen");
}

void LoadableTypeInfo::addScalarToAggLowering(IRGenModule &IGM,
                                              SwiftAggLowering &lowering,
                                              llvm::Type *type, Size offset,
                                              Size storageSize) {
  lowering.addTypedData(type, offset.asCharUnits(),
                        offset.asCharUnits() + storageSize.asCharUnits());
}

llvm::Value *FixedTypeInfo::getSize(IRGenFunction &IGF, SILType T) const {
  return FixedTypeInfo::getStaticSize(IGF.IGM);
}
llvm::Constant *FixedTypeInfo::getStaticSize(IRGenModule &IGM) const {
  return IGM.getSize(getFixedSize());
}

llvm::Value *FixedTypeInfo::getAlignmentMask(IRGenFunction &IGF,
                                             SILType T) const {
  return FixedTypeInfo::getStaticAlignmentMask(IGF.IGM);
}
llvm::Constant *FixedTypeInfo::getStaticAlignmentMask(IRGenModule &IGM) const {
  return IGM.getSize(Size(getFixedAlignment().getValue() - 1));
}

llvm::Value *FixedTypeInfo::getStride(IRGenFunction &IGF, SILType T) const {
  return FixedTypeInfo::getStaticStride(IGF.IGM);
}
llvm::Value *FixedTypeInfo::getIsTriviallyDestroyable(IRGenFunction &IGF, SILType T) const {
  return llvm::ConstantInt::get(IGF.IGM.Int1Ty,
                                isTriviallyDestroyable(ResilienceExpansion::Maximal) == IsTriviallyDestroyable);
}
llvm::Value *FixedTypeInfo::getIsBitwiseTakable(IRGenFunction &IGF, SILType T) const {
  return llvm::ConstantInt::get(IGF.IGM.Int1Ty,
                                isBitwiseTakable(ResilienceExpansion::Maximal) == IsBitwiseTakable);
}
llvm::Constant *FixedTypeInfo::getStaticStride(IRGenModule &IGM) const {
  return IGM.getSize(getFixedStride());
}

llvm::Value *FixedTypeInfo::isDynamicallyPackedInline(IRGenFunction &IGF,
                                                      SILType T) const {
  auto packing = getFixedPacking(IGF.IGM);
  assert(packing == FixedPacking::Allocate ||
         packing == FixedPacking::OffsetZero);
  return llvm::ConstantInt::get(IGF.IGM.Int1Ty,
                                packing == FixedPacking::OffsetZero);
}

unsigned FixedTypeInfo::getSpareBitExtraInhabitantCount() const {
  if (SpareBits.none())
    return 0;
  // Make sure the arithmetic below doesn't overflow.
  if (getFixedSize().getValue() >= 4)
    return ValueWitnessFlags::MaxNumExtraInhabitants;
  unsigned spareBitCount = SpareBits.count();
  assert(spareBitCount <= getFixedSize().getValueInBits()
         && "more spare bits than storage bits?!");
  unsigned inhabitedBitCount = getFixedSize().getValueInBits() - spareBitCount;
  unsigned rawCount = ((1U << spareBitCount) - 1U) << inhabitedBitCount;
  return std::min(rawCount,
                  unsigned(ValueWitnessFlags::MaxNumExtraInhabitants));
}

void FixedTypeInfo::applyFixedSpareBitsMask(const IRGenModule &IGM,
                                            SpareBitVector &mask) const {
  auto builder = BitPatternBuilder(IGM.Triple.isLittleEndian());

  // If the mask is no longer than the stored spare bits, we can just
  // apply the stored spare bits.
  if (mask.size() <= SpareBits.size()) {
    // Grow the mask out if necessary; the tail padding is all spare bits.
    builder.append(mask);
    builder.padWithSetBitsTo(SpareBits.size());
    mask = SpareBitVector(builder.build());
    mask &= SpareBits;
    return;
  }

  // Otherwise, we have to grow out the stored spare bits before we
  // can intersect.
  builder.append(SpareBits);
  builder.padWithSetBitsTo(mask.size());
  mask &= builder.build();
  return;
}

APInt
FixedTypeInfo::getSpareBitFixedExtraInhabitantValue(IRGenModule &IGM,
                                                    unsigned bits,
                                                    unsigned index) const {
  // Factor the index into the part that goes in the occupied bits and the
  // part that goes in the spare bits.
  unsigned occupiedIndex, spareIndex = 0;
  
  unsigned spareBitCount = SpareBits.count();
  unsigned occupiedBitCount = SpareBits.size() - spareBitCount;
  
  if (occupiedBitCount >= 31) {
    occupiedIndex = index;
    // The spare bit value is biased by one because all zero spare bits
    // represents a valid value of the type.
    spareIndex = 1;
  } else {
    occupiedIndex = index & ((1 << occupiedBitCount) - 1);
    // The spare bit value is biased by one because all zero spare bits
    // represents a valid value of the type.
    spareIndex = (index >> occupiedBitCount) + 1;
  }

  APInt mask = SpareBits.asAPInt().zextOrTrunc(bits);
  APInt v = scatterBits(mask, spareIndex);
  v |= scatterBits(~mask, occupiedIndex);
  return v;
}

llvm::Value *
FixedTypeInfo::getSpareBitExtraInhabitantIndex(IRGenFunction &IGF,
                                               Address src) const {
  assert(!SpareBits.none() && "no spare bits");
  
  auto &C = IGF.IGM.getLLVMContext();
  
  // Load the value.
  auto payloadTy = llvm::IntegerType::get(C, getFixedSize().getValueInBits());
  src = IGF.Builder.CreateElementBitCast(src, payloadTy);
  auto val = IGF.Builder.CreateLoad(src);
  
  // If the spare bits are all zero, then we have a valid value and not an
  // extra inhabitant.
  auto spareBitsMask
    = llvm::ConstantInt::get(C, SpareBits.asAPInt());
  auto valSpareBits = IGF.Builder.CreateAnd(val, spareBitsMask);
  auto isValid = IGF.Builder.CreateICmpEQ(valSpareBits,
                                          llvm::ConstantInt::get(payloadTy, 0));
  
  auto *origBB = IGF.Builder.GetInsertBlock();
  auto *endBB = llvm::BasicBlock::Create(C);
  auto *spareBB = llvm::BasicBlock::Create(C);
  IGF.Builder.CreateCondBr(isValid, endBB, spareBB);

  IGF.Builder.emitBlock(spareBB);
  ConditionalDominanceScope condition(IGF);
  
  // Gather the occupied bits.
  auto OccupiedBits = SpareBits;
  OccupiedBits.flipAll();
  llvm::Value *idx = emitGatherBits(IGF, OccupiedBits.asAPInt(), val, 0, 31);
  
  // See if spare bits fit into the 31 bits of the index.
  unsigned numSpareBits = SpareBits.count();
  unsigned numOccupiedBits = getFixedSize().getValueInBits() - numSpareBits;
  if (numOccupiedBits < 31) {
    // Gather the spare bits.
    llvm::Value *spareIdx
      = emitGatherBits(IGF, SpareBits.asAPInt(), val, numOccupiedBits, 31);
    // Unbias by subtracting one.

    uint64_t shifted = static_cast<uint64_t>(1) << numOccupiedBits;
    spareIdx = IGF.Builder.CreateSub(spareIdx,
            llvm::ConstantInt::get(spareIdx->getType(), shifted));
    idx = IGF.Builder.CreateOr(idx, spareIdx);
  }
  idx = IGF.Builder.CreateZExt(idx, IGF.IGM.Int32Ty);
  
  IGF.Builder.CreateBr(endBB);
  IGF.Builder.emitBlock(endBB);
  
  // If we had a valid value, return -1. Otherwise, return the index.
  auto phi = IGF.Builder.CreatePHI(IGF.IGM.Int32Ty, 2);
  phi->addIncoming(llvm::ConstantInt::get(IGF.IGM.Int32Ty, -1), origBB);
  phi->addIncoming(idx, spareBB);
  
  return phi;
}

static llvm::Value *computeExtraTagBytes(IRGenFunction &IGF, IRBuilder &Builder,
                                         Size fixedSize,
                                         llvm::Value *numEmptyCases) {
  // We can use the payload area with a tag bit set somewhere outside of the
  // payload area to represent cases. See how many bytes we need to cover
  // all the empty cases.

  // Algorithm:
  // unsigned numTags = 1;
  // if (size >= 4)
  //   // Assume that one tag bit is enough if the precise calculation overflows
  //   // an int32.
  //   numTags += 1;
  // else {
  //   unsigned bits = size * 8U;
  //   unsigned casesPerTagBitValue = 1U << bits;
  //   numTags += ((emptyCases + (casesPerTagBitValue - 1U)) >> bits);
  // }
  // return (numTags < 256 ? 1 :
  // 				 numTags < 65536 ? 2 : 4);

  auto &IGM = IGF.IGM;
  auto &Ctx = Builder.getContext();
  auto *int32Ty = IGM.Int32Ty;

  auto *one = llvm::ConstantInt::get(int32Ty, 1U);
  if (fixedSize >= Size(4)) {
    return one;
  }

  auto *entryBB = Builder.GetInsertBlock();
  llvm::Value *size = IGM.getSize(fixedSize);
  auto *returnBB = llvm::BasicBlock::Create(Ctx);
  size = Builder.CreateZExtOrTrunc(size, int32Ty); // We know size < 4.

  auto *two = llvm::ConstantInt::get(int32Ty, 2U);
  auto *four = llvm::ConstantInt::get(int32Ty, 4U);

  auto *bits = Builder.CreateMul(size, llvm::ConstantInt::get(int32Ty, 8U));
  auto *casesPerTagBitValue = Builder.CreateShl(one, bits);

  auto *numTags = Builder.CreateSub(casesPerTagBitValue, one);
  numTags = Builder.CreateAdd(numTags, numEmptyCases);
  numTags = Builder.CreateLShr(numTags, bits);
  numTags = Builder.CreateAdd(numTags, one);

  auto *notLT256BB = llvm::BasicBlock::Create(Ctx);
  auto *isLT256 =
      Builder.CreateICmpULT(numTags, llvm::ConstantInt::get(int32Ty, 256U));
  Builder.CreateCondBr(isLT256, returnBB, notLT256BB);

  Builder.emitBlock(notLT256BB);
  auto *isLT65536 =
      Builder.CreateICmpULT(numTags, llvm::ConstantInt::get(int32Ty, 65536U));
  numTags = Builder.CreateSelect(isLT65536, two, four);
  Builder.CreateBr(returnBB);

  Builder.emitBlock(returnBB);
  auto *phi = Builder.CreatePHI(int32Ty, 3);
  phi->addIncoming(one, entryBB);
  phi->addIncoming(numTags, notLT256BB);
  return phi;
}

/// Emit a specialized memory operation for a \p size of 0, 1, 2 or
/// 4 bytes.
/// \p emitMemOpFn will be called in a basic block where \p size is
/// a known constant value passed as a parameter to the function.
static void emitSpecializedMemOperation(
    IRGenFunction &IGF,
    llvm::function_ref<void(IRBuilder &, Size)> emitMemOpFn,
    llvm::Value *size,
    const std::array<Size, 4> &sizes) {
  auto &Ctx = IGF.IGM.getLLVMContext();
  auto &Builder = IGF.Builder;

  // Block to jump to after successful match.
  auto *returnBB = llvm::BasicBlock::Create(Ctx);

  // Test all the sizes in turn, generating code similar to:
  //
  //   if (size == 0) {
  //     <op>
  //   } else if (size == 1) {
  //     <op>
  //   } else if (size == 2) {
  //     <op>
  //   } else if (size == 4) {
  //     <op>
  //   } else {
  //     <unreachable>
  //   }
  //
  for (const Size &s : sizes) {
    auto *matchBB = llvm::BasicBlock::Create(Ctx);
    auto *nextBB = llvm::BasicBlock::Create(Ctx);

    // Check if size matches.
    auto *imm = Builder.getInt32(s.getValue());
    auto *cmp = Builder.CreateICmpEQ(size, imm);
    Builder.CreateCondBr(cmp, matchBB, nextBB);

    // Size matches: execute sized operation.
    Builder.emitBlock(matchBB);
    emitMemOpFn(Builder, s);
    Builder.CreateBr(returnBB);

    // Size does not match: try next size.
    Builder.emitBlock(nextBB);
  }
  // No size matched. Should never happen.
  Builder.CreateUnreachable();

  // Continue.
  Builder.emitBlock(returnBB);
}

/// Emit a load of the \p size byte integer values zero extended to 4 bytes. \p
/// size maybe any of the four values specified in the \p sizes array.
static llvm::Value *emitLoadBytes(IRGenFunction &IGF, Address from,
                                  llvm::Value *size,
                                  const std::array<Size, 4> &sizes) {
  auto *phi = llvm::PHINode::Create(IGF.IGM.Int32Ty, 4);

  emitSpecializedMemOperation(IGF,
      [=](IRBuilder &B, Size s) {
        if (s == Size(0)) {
          // If the size is 0 bytes return 0.
          phi->addIncoming(B.getInt32(0), B.GetInsertBlock());
          return;
        }
        // Generate a load of size bytes and zero-extend it to 32-bits.
        auto *type = B.getIntNTy(s.getValueInBits());
        Address addr = B.CreateElementBitCast(from, type);
        auto *val = B.CreateZExtOrTrunc(B.CreateLoad(addr), B.getInt32Ty());
        phi->addIncoming(val, B.GetInsertBlock());
      },
      size,
      sizes);
  IGF.Builder.Insert(phi);
  return phi;
}

/// Emit a load of a \p size byte integer value zero extended to 4 bytes \p size
/// may be 1, 2, 3, or 4.
llvm::Value *irgen::emitLoad1to4Bytes(IRGenFunction &IGF, Address from,
                                      llvm::Value *size) {
  // Sizes to try. Tested in order.
  const std::array<Size, 4> sizes = {
    Size(1),
    Size(2),
    Size(3),
    Size(4),
  };
  return emitLoadBytes(IGF, from, size, sizes);
}

/// Emit a load of a \p size byte integer value zero extended to 4 bytes.
/// \p size may be 0, 1, 2 or 4.
llvm::Value *irgen::emitGetTag(IRGenFunction &IGF, Address from,
                               llvm::Value *size) {
  // Sizes to try. Tested in order.
  const std::array<Size, 4> sizes = {
    Size(0),
    Size(1),
    Size(2),
    Size(4),
  };
  return emitLoadBytes(IGF, from, size, sizes);
}

/// Emit a store of \p val truncated to \p size bytes.
/// \p size may be one of the entries in the \p sizes array.
static void emitStoreBytes(IRGenFunction &IGF, Address to, llvm::Value *val,
                           llvm::Value *size,
                           const std::array<Size, 4> &sizes) {
  emitSpecializedMemOperation(IGF,
      [=](IRBuilder &B, Size s) {
        if (s == Size(0)) {
          // Nothing to store.
          return;
        }
        // Store value truncated to size bytes.
        auto *type = B.getIntNTy(s.getValueInBits());
        auto *trunc = B.CreateZExtOrTrunc(val, type);
        Address addr = B.CreateElementBitCast(to, type);
        B.CreateStore(trunc, addr);
      },
      size,
      sizes);
}

/// Emit a store of \p val truncated to \p size bytes.
/// \p size may be  1, 2, 3, or 4.
void irgen::emitStore1to4Bytes(IRGenFunction &IGF, Address to, llvm::Value *val,
                              llvm::Value *size) {
  // Sizes to try. Tested in order.
  const std::array<Size, 4> sizes = {
    Size(1),
    Size(2),
    Size(3),
    Size(4),
  };
  emitStoreBytes(IGF, to, val, size, sizes);
}

/// Emit a store of \p val truncated to \p size bytes.
/// \p size may be 0, 1, 2 or 4.
void irgen::emitSetTag(IRGenFunction &IGF,
                       Address to, llvm::Value *val,
                       llvm::Value *size) {
  // Sizes to try. Tested in order.
  const std::array<Size, 4> sizes = {
    Size(0),
    Size(1),
    Size(2),
    Size(4),
  };
  emitStoreBytes(IGF, to, val, size, sizes);
}

llvm::Value *FixedTypeInfo::getEnumTagSinglePayload(IRGenFunction &IGF,
                                                    llvm::Value *numEmptyCases,
                                                    Address enumAddr,
                                                    SILType T,
                                                    bool isOutlined) const {
  return getFixedTypeEnumTagSinglePayload(IGF, *this, numEmptyCases, enumAddr,
                                          T, isOutlined);
}

llvm::Value *irgen::getFixedTypeEnumTagSinglePayload(IRGenFunction &IGF,
                                                     const FixedTypeInfo &ti,
                                                     llvm::Value *numEmptyCases,
                                                     Address enumAddr,
                                                     SILType T,
                                                     bool isOutlined) {
  auto *size = ti.getSize(IGF, T);
  Size fixedSize = ti.getFixedSize();
  auto fixedExtraInhabitantCount = ti.getFixedExtraInhabitantCount(IGF.IGM);

  return getFixedTypeEnumTagSinglePayload(
      IGF, numEmptyCases, enumAddr, size, fixedSize, fixedExtraInhabitantCount,
      [&](Address addr) -> llvm::Value * {
        return ti.getExtraInhabitantIndex(IGF, addr, T, false);
      },
      isOutlined);
}

llvm::Value *irgen::getFixedTypeEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *numEmptyCases, Address enumAddr,
    llvm::Value *size, Size fixedSize, unsigned fixedExtraInhabitantCount,
    llvm::function_ref<llvm::Value *(Address)> getExtraInhabitantIndex,
    bool isOutlined) {

  auto &IGM = IGF.IGM;
  auto &Ctx = IGF.IGM.getLLVMContext();
  auto &Builder = IGF.Builder;

  auto *numExtraInhabitants =
      llvm::ConstantInt::get(IGM.Int32Ty, fixedExtraInhabitantCount);

  auto *zero = llvm::ConstantInt::get(IGM.Int32Ty, 0U);
  auto *one = llvm::ConstantInt::get(IGM.Int32Ty, 1U);
  auto *four = llvm::ConstantInt::get(IGM.Int32Ty, 4U);
  auto *eight = llvm::ConstantInt::get(IGM.Int32Ty, 8U);

  auto *extraTagBitsBB = llvm::BasicBlock::Create(Ctx);
  auto *noExtraTagBitsBB = llvm::BasicBlock::Create(Ctx);
  auto *hasEmptyCasesBB = llvm::BasicBlock::Create(Ctx);
  auto *singleCaseEnumBB = llvm::BasicBlock::Create(Ctx);

  // No empty cases so we must be the payload.
  auto *hasNoEmptyCases = Builder.CreateICmpEQ(zero, numEmptyCases);
  Builder.CreateCondBr(hasNoEmptyCases, singleCaseEnumBB, hasEmptyCasesBB);

  // Otherwise, check whether we need extra tag bits.
  Builder.emitBlock(hasEmptyCasesBB);
  auto *hasExtraTagBits =
      Builder.CreateICmpUGT(numEmptyCases, numExtraInhabitants);
  Builder.CreateCondBr(hasExtraTagBits, extraTagBitsBB, noExtraTagBitsBB);

  // There are extra tag bits to check.
  Builder.emitBlock(extraTagBitsBB);

  // Compute the number of extra tag bytes.
  auto *emptyCases = Builder.CreateSub(numEmptyCases, numExtraInhabitants);
  auto *numExtraTagBytes =
      computeExtraTagBytes(IGF, Builder, fixedSize, emptyCases);

  // Read the value stored in the extra tag bytes.
  auto *valueAddr =
      Builder.CreateBitOrPointerCast(enumAddr.getAddress(), IGM.Int8PtrTy);
  auto *extraTagBitsAddr =
      Builder.CreateConstInBoundsGEP1_32(IGM.Int8Ty, valueAddr,
          fixedSize.getValue());
  auto *extraTagBits =
      emitGetTag(IGF, Address(extraTagBitsAddr, IGM.Int8Ty, Alignment(1)),
                 numExtraTagBytes);

  extraTagBitsBB = llvm::BasicBlock::Create(Ctx);
  Builder.CreateCondBr(Builder.CreateICmpEQ(extraTagBits, zero),
                       noExtraTagBitsBB, extraTagBitsBB);

  auto *resultBB = llvm::BasicBlock::Create(Ctx);

  Builder.emitBlock(extraTagBitsBB);

  auto *truncSize = Builder.CreateZExtOrTrunc(size, IGM.Int32Ty);

  auto *caseIndexFromExtraTagBits = Builder.CreateSelect(
      Builder.CreateICmpUGE(truncSize, four), zero,
      Builder.CreateShl(Builder.CreateSub(extraTagBits, one),
                        Builder.CreateMul(eight, truncSize)));

  llvm::Value *caseIndexFromValue = zero;
  if (fixedSize > Size(0)) {
    // llvm only supports integer types upto a certain size (i.e selection dag
    // will crash).
    if (fixedSize.getValueInBits() <= llvm::IntegerType::MAX_INT_BITS / 4) {
      // Read up to one pointer-sized 'chunk' of the payload.
      // The size of the chunk does not have to be a power of 2.
      auto *caseIndexType =
          llvm::IntegerType::get(Ctx, fixedSize.getValueInBits());
      auto *caseIndexAddr =
          Builder.CreateBitCast(valueAddr, caseIndexType->getPointerTo());
      caseIndexFromValue = Builder.CreateZExtOrTrunc(
          Builder.CreateLoad(
              Address(caseIndexAddr, caseIndexType, Alignment(1))),
          IGM.Int32Ty);
    } else {
      auto *caseIndexType = llvm::IntegerType::get(Ctx, 32);
      auto *caseIndexAddr =
          Builder.CreateBitCast(valueAddr, caseIndexType->getPointerTo());
      caseIndexFromValue = Builder.CreateZExtOrTrunc(
          Builder.CreateLoad(
              Address(caseIndexAddr, caseIndexType, Alignment(1))),
          IGM.Int32Ty);
    }
  }

  auto *result1 = Builder.CreateAdd(
      numExtraInhabitants,
      Builder.CreateOr(caseIndexFromValue, caseIndexFromExtraTagBits));
  Builder.CreateBr(resultBB);

  // Extra tag bits were considered and zero or there are not extra tag
  // bits.
  Builder.emitBlock(noExtraTagBitsBB);
  // If there are extra inhabitants, see whether the payload is valid.
  llvm::Value *result0;
  if (fixedExtraInhabitantCount > 0) {
    result0 = getExtraInhabitantIndex(enumAddr);
    noExtraTagBitsBB = Builder.GetInsertBlock();
  } else {
    result0 = llvm::ConstantInt::getSigned(IGM.Int32Ty, -1);
  }
  Builder.CreateBr(resultBB);

  Builder.emitBlock(singleCaseEnumBB);
  // Otherwise, we have a valid payload.
  auto *result2 = llvm::ConstantInt::getSigned(IGM.Int32Ty, -1);
  Builder.CreateBr(resultBB);

  Builder.emitBlock(resultBB);
  auto *result = Builder.CreatePHI(IGM.Int32Ty, 3);
  result->addIncoming(result0, noExtraTagBitsBB);
  result->addIncoming(result1, extraTagBitsBB);
  result->addIncoming(result2, singleCaseEnumBB);

  return Builder.CreateAdd(result, llvm::ConstantInt::get(IGM.Int32Ty, 1));
}

void FixedTypeInfo::storeEnumTagSinglePayload(IRGenFunction &IGF,
                                              llvm::Value *whichCase,
                                              llvm::Value *numEmptyCases,
                                              Address enumAddr,
                                              SILType T,
                                              bool isOutlined) const {
  storeFixedTypeEnumTagSinglePayload(IGF, *this, whichCase, numEmptyCases,
                                     enumAddr, T, isOutlined);
}

void irgen::storeFixedTypeEnumTagSinglePayload(IRGenFunction &IGF,
                                               const FixedTypeInfo &ti,
                                               llvm::Value *whichCase,
                                               llvm::Value *numEmptyCases,
                                               Address enumAddr,
                                               SILType T,
                                               bool isOutlined) {
  auto fixedSize = ti.getFixedSize();
  auto *size = ti.getSize(IGF, T);
  auto fixedExtraInhabitantCount = ti.getFixedExtraInhabitantCount(IGF.IGM);
  storeFixedTypeEnumTagSinglePayload(
      IGF, whichCase, numEmptyCases, enumAddr, size, fixedSize,
      fixedExtraInhabitantCount,
      [&](llvm::Value *nonPayloadElementIndex, Address enumAddr) {
        ti.storeExtraInhabitant(IGF, nonPayloadElementIndex, enumAddr, T,
                                /*outlined*/ false);
      },
      isOutlined);
}

void irgen::storeFixedTypeEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *whichCase, llvm::Value *numEmptyCases,
    Address enumAddr, llvm::Value *size, Size fixedSize,
    unsigned fixedExtraInhabitantCount,
    llvm::function_ref<void(llvm::Value *, Address)> storeExtraInhabitant,
    bool isOutlined) {

  auto &IGM = IGF.IGM;
  auto &Ctx = IGF.IGM.getLLVMContext();
  auto &Builder = IGF.Builder;
  auto &int32Ty = IGM.Int32Ty;

  auto *zero = llvm::ConstantInt::get(int32Ty, 0U);
  auto *one = llvm::ConstantInt::get(int32Ty, 1U);
  auto *four = llvm::ConstantInt::get(int32Ty, 4U);
  auto *eight = llvm::ConstantInt::get(int32Ty, 8U);


  Address valueAddr = Builder.CreateElementBitCast(enumAddr, IGM.Int8Ty);
  Address extraTagBitsAddr =
    Builder.CreateConstByteArrayGEP(valueAddr, fixedSize);

  auto *numExtraInhabitants =
      llvm::ConstantInt::get(IGM.Int32Ty, fixedExtraInhabitantCount);

  // Do we need extra tag bytes.
  auto *entryBB = Builder.GetInsertBlock();
  auto *continueBB = llvm::BasicBlock::Create(Ctx);
  auto *computeExtraTagBytesBB = llvm::BasicBlock::Create(Ctx);
  auto *hasExtraTagBits =
      Builder.CreateICmpUGT(numEmptyCases, numExtraInhabitants);
  Builder.CreateCondBr(hasExtraTagBits, computeExtraTagBytesBB, continueBB);

  Builder.emitBlock(computeExtraTagBytesBB);
  // Compute the number of extra tag bytes.
  auto *emptyCases = Builder.CreateSub(numEmptyCases, numExtraInhabitants);
  auto *numExtraTagBytes0 =
      computeExtraTagBytes(IGF, Builder, fixedSize, emptyCases);
  computeExtraTagBytesBB = Builder.GetInsertBlock();
  Builder.CreateBr(continueBB);

  Builder.emitBlock(continueBB);
  auto *numExtraTagBytes = Builder.CreatePHI(int32Ty, 2);
  numExtraTagBytes->addIncoming(zero, entryBB);
  numExtraTagBytes->addIncoming(numExtraTagBytes0, computeExtraTagBytesBB);

  // Check whether we need to set the extra tag bits to non zero.
  auto *isExtraTagBitsCaseBB = llvm::BasicBlock::Create(Ctx);
  auto *isPayloadOrInhabitantCaseBB = llvm::BasicBlock::Create(Ctx);
  auto *isPayloadOrExtraInhabitant =
      Builder.CreateICmpULE(whichCase, numExtraInhabitants);
  Builder.CreateCondBr(isPayloadOrExtraInhabitant, isPayloadOrInhabitantCaseBB,
                       isExtraTagBitsCaseBB);

  // We are the payload or fit within the extra inhabitants.
  Builder.emitBlock(isPayloadOrInhabitantCaseBB);
  // Zero the tag bits.
  emitSetTag(IGF, extraTagBitsAddr, zero, numExtraTagBytes);
  isPayloadOrInhabitantCaseBB = Builder.GetInsertBlock();
  auto *storeInhabitantBB = llvm::BasicBlock::Create(Ctx);
  auto *returnBB = llvm::BasicBlock::Create(Ctx);
  auto *isPayload = Builder.CreateICmpEQ(whichCase, zero);
  Builder.CreateCondBr(isPayload, returnBB, storeInhabitantBB);

  Builder.emitBlock(storeInhabitantBB);
  if (fixedExtraInhabitantCount > 0) {
    // Store an index in the range [0..ElementsWithNoPayload-1].
    auto *nonPayloadElementIndex = Builder.CreateSub(whichCase, one);
    storeExtraInhabitant(nonPayloadElementIndex, enumAddr);
  }
  Builder.CreateBr(returnBB);

  // There are extra tag bits to consider.
  Builder.emitBlock(isExtraTagBitsCaseBB);

  // Write the extra tag bytes. At this point we know we have an no payload case
  // and therefore the index we should store is in the range
  // [0..ElementsWithNoPayload-1].
  auto *nonPayloadElementIndex = Builder.CreateSub(whichCase, one);
  auto *caseIndex =
      Builder.CreateSub(nonPayloadElementIndex, numExtraInhabitants);
  auto *truncSize = Builder.CreateZExtOrTrunc(size, IGM.Int32Ty);
  auto *isFourBytesPayload = Builder.CreateICmpUGE(truncSize, four);
  auto *payloadGE4BB = Builder.GetInsertBlock();
  auto *payloadLT4BB = llvm::BasicBlock::Create(Ctx);
  continueBB = llvm::BasicBlock::Create(Ctx);
  Builder.CreateCondBr(isFourBytesPayload, continueBB, payloadLT4BB);

  Builder.emitBlock(payloadLT4BB);
  auto *payloadBits = Builder.CreateMul(truncSize, eight);
  auto *extraTagIndex0 = Builder.CreateLShr(caseIndex, payloadBits);
  extraTagIndex0 = Builder.CreateAdd(one, extraTagIndex0);
  auto *payloadIndex0 = Builder.CreateShl(one, payloadBits);
  payloadIndex0 = Builder.CreateSub(payloadIndex0, one);
  payloadIndex0 = Builder.CreateAnd(payloadIndex0, caseIndex);
  Builder.CreateBr(continueBB);

  Builder.emitBlock(continueBB);
  auto *extraTagIndex = Builder.CreatePHI(int32Ty, 2);
  extraTagIndex->addIncoming(llvm::ConstantInt::get(int32Ty, 1), payloadGE4BB);
  extraTagIndex->addIncoming(extraTagIndex0, payloadLT4BB);

  auto *payloadIndex = Builder.CreatePHI(int32Ty, 2);
  payloadIndex->addIncoming(caseIndex, payloadGE4BB);
  payloadIndex->addIncoming(payloadIndex0, payloadLT4BB);

  if (fixedSize > Size(0)) {
    if (fixedSize.getValueInBits() <= llvm::IntegerType::MAX_INT_BITS / 4) {
      // Write the value to the payload as a zero extended integer.
      auto *intType = Builder.getIntNTy(fixedSize.getValueInBits());
      Builder.CreateStore(Builder.CreateZExtOrTrunc(payloadIndex, intType),
                          Builder.CreateElementBitCast(valueAddr, intType));
    } else {
      // Write the value to the payload as a zero extended integer.
      Size limit = IGM.getPointerSize();
      auto *intType = Builder.getIntNTy(limit.getValueInBits());
      Builder.CreateStore(Builder.CreateZExtOrTrunc(payloadIndex, intType),
                          Builder.CreateElementBitCast(valueAddr, intType));
      // Zero the remainder of the payload.
      auto zeroAddr = Builder.CreateConstByteArrayGEP(valueAddr, limit);
      auto zeroSize = Builder.CreateSub(
          size, llvm::ConstantInt::get(size->getType(), limit.getValue()));
      Builder.CreateMemSet(zeroAddr, Builder.getInt8(0), zeroSize);
    }
  }
  // Write to the extra tag bytes, if any.
  emitSetTag(IGF, extraTagBitsAddr, extraTagIndex, numExtraTagBytes);
  Builder.CreateBr(returnBB);

  Builder.emitBlock(returnBB);
}

void
FixedTypeInfo::storeSpareBitExtraInhabitant(IRGenFunction &IGF,
                                            llvm::Value *index,
                                            Address dest) const {
  assert(!SpareBits.none() && "no spare bits");
  
  auto &C = IGF.IGM.getLLVMContext();

  auto payloadTy = llvm::IntegerType::get(C, getFixedSize().getValueInBits());

  unsigned spareBitCount = SpareBits.count();
  unsigned occupiedBitCount = SpareBits.size() - spareBitCount;
  llvm::Value *occupiedIndex;
  llvm::Value *spareIndex;
  
  // The spare bit value is biased by one because all zero spare bits
  // represents a valid value of the type.
  auto spareBitBias = llvm::ConstantInt::get(IGF.IGM.Int32Ty, 1U);
  
  // Factor the spare and occupied bit values from the index.
  if (occupiedBitCount >= 31) {
    occupiedIndex = index;
    spareIndex = spareBitBias;
  } else {
    auto occupiedBitMask = APInt::getAllOnesValue(occupiedBitCount);
    occupiedBitMask = occupiedBitMask.zext(32);
    auto occupiedBitMaskValue = llvm::ConstantInt::get(C, occupiedBitMask);
    occupiedIndex = IGF.Builder.CreateAnd(index, occupiedBitMaskValue);
    
    auto occupiedBitCountValue
      = llvm::ConstantInt::get(IGF.IGM.Int32Ty, occupiedBitCount);
    spareIndex = IGF.Builder.CreateLShr(index, occupiedBitCountValue);
    spareIndex = IGF.Builder.CreateAdd(spareIndex, spareBitBias);
  }
  
  // Scatter the occupied bits.
  auto OccupiedBits = ~SpareBits.asAPInt();
  llvm::Value *occupied = emitScatterBits(IGF.IGM, IGF.Builder, OccupiedBits,
                                          occupiedIndex, 0);
  
  // Scatter the spare bits.
  llvm::Value *spare = emitScatterBits(IGF.IGM, IGF.Builder, SpareBits.asAPInt(),
                                       spareIndex, 0);
  
  // Combine the values and store to the destination.
  llvm::Value *inhabitant = IGF.Builder.CreateOr(occupied, spare);

  dest = IGF.Builder.CreateElementBitCast(dest, payloadTy);
  IGF.Builder.CreateStore(inhabitant, dest);
}

namespace {
  /// A TypeInfo implementation for empty types.
  struct EmptyTypeInfo : ScalarTypeInfo<EmptyTypeInfo, LoadableTypeInfo> {
    EmptyTypeInfo(llvm::Type *ty)
      : ScalarTypeInfo(ty, Size(0), SpareBitVector{}, Alignment(1),
                       IsTriviallyDestroyable,
                       IsCopyable,
                       IsFixedSize) {}
    unsigned getExplosionSize() const override { return 0; }
    void getSchema(ExplosionSchema &schema) const override {}
    void addToAggLowering(IRGenModule &IGM, SwiftAggLowering &lowering,
                          Size offset) const override {}
    void loadAsCopy(IRGenFunction &IGF, Address addr,
                    Explosion &e) const override {}
    void loadAsTake(IRGenFunction &IGF, Address addr,
                    Explosion &e) const override {}
    void assign(IRGenFunction &IGF, Explosion &e, Address addr,
                bool isOutlined, SILType T) const override {}
    void initialize(IRGenFunction &IGF, Explosion &e, Address addr,
                    bool isOutlined) const override {}
    void copy(IRGenFunction &IGF, Explosion &src,
              Explosion &dest, Atomicity atomicity) const override {}
    void consume(IRGenFunction &IGF, Explosion &src,
                 Atomicity atomicity, SILType T) const override {}
    void fixLifetime(IRGenFunction &IGF, Explosion &src) const override {}
    void destroy(IRGenFunction &IGF, Address addr, SILType T,
                 bool isOutlined) const override {}
    void packIntoEnumPayload(IRGenModule &IGM,
                             IRBuilder &builder, EnumPayload &payload,
                             Explosion &src, unsigned offset) const override {}
    void unpackFromEnumPayload(IRGenFunction &IGF,
                               const EnumPayload &payload,
                               Explosion &dest,
                               unsigned offset) const override {}

    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType,
                          bool useStructLayouts) const override {
      return IGM.typeLayoutCache.getEmptyEntry();
    }
  };

  /// A TypeInfo implementation for types represented as a single
  /// scalar type.
  class PrimitiveTypeInfo final :
    public PODSingleScalarTypeInfo<PrimitiveTypeInfo, LoadableTypeInfo> {
  public:
    PrimitiveTypeInfo(llvm::Type *storage, Size size,
                      SpareBitVector &&spareBits,
                      Alignment align)
      : PODSingleScalarTypeInfo(storage, size, std::move(spareBits), align) {}
  };

  /// A TypeInfo implementation for pointers that are:
  /// - valid (i.e. non-null, and generally >= LeastValidPointerValue),
  /// - aligned (i.e. have zero low bits up to some bit), and
  /// - trivial (i.e. not reference-counted or otherwise managed).
  ///
  /// These properties make it suitable for unmanaged pointers with special
  /// uses in the ABI.
  class AlignedRawPointerTypeInfo final :
    public PODSingleScalarTypeInfo<AlignedRawPointerTypeInfo,
                                   LoadableTypeInfo> {
    Alignment PointeeAlign;
  public:
    AlignedRawPointerTypeInfo(llvm::Type *storage,
                              Size size, SpareBitVector &&spareBits,
                              Alignment align, Alignment pointeeAlign)
      : PODSingleScalarTypeInfo(storage, size, std::move(spareBits), align),
        PointeeAlign(pointeeAlign) {}

    bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
      return true;
    }

    PointerInfo getPointerInfo() const {
      return PointerInfo::forAligned(PointeeAlign);
    }

    unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
      return getPointerInfo().getExtraInhabitantCount(IGM);
    }

    APInt getFixedExtraInhabitantValue(IRGenModule &IGM, unsigned bits,
                                       unsigned index) const override {
      return getPointerInfo()
               .getFixedExtraInhabitantValue(IGM, bits, index, 0);
    }

    llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF,
                                         Address src,
                                         SILType T,
                                         bool isOutlined) const override {
      return getPointerInfo().getExtraInhabitantIndex(IGF, src);
    }

    void storeExtraInhabitant(IRGenFunction &IGF, llvm::Value *index,
                              Address dest, SILType T,
                              bool isOutlined) const override {
      getPointerInfo().storeExtraInhabitant(IGF, index, dest);
    }
  };

  /// A TypeInfo implementation for Builtin.RawPointer.  We intentionally
  /// do not make any assumptions about values of this type except that
  /// they are not the special "null" extra inhabitant; as a result, an
  /// Optional<Builtin.RawPointer> can reliably carry an arbitrary
  /// bit-pattern of its size without fear of corruption.  Since the
  /// primary uses of Builtin.RawPointer are the unsafe pointer APIs,
  /// that is exactly what we want.  It does mean that Builtin.RawPointer
  /// is usually a suboptimal type for representing known-valid pointers.
  class RawPointerTypeInfo final :
    public PODSingleScalarTypeInfo<RawPointerTypeInfo, LoadableTypeInfo> {
  public:
    RawPointerTypeInfo(llvm::Type *storage, Size size, Alignment align)
      : PODSingleScalarTypeInfo(
          storage, size,
          SpareBitVector::getConstant(size.getValueInBits(), false),
          align) {}

    bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
      return true;
    }

    unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
      return 1;
    }

    APInt getFixedExtraInhabitantValue(IRGenModule &IGM, unsigned bits,
                                       unsigned index) const override {
      assert(index == 0);
      return APInt(bits, 0);
    }

    llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF,
                                         Address src,
                                         SILType T,
                                         bool isOutlined) const override {
      // Copied from BridgeObjectTypeInfo.
      src = IGF.Builder.CreateElementBitCast(src, IGF.IGM.IntPtrTy);
      auto val = IGF.Builder.CreateLoad(src);
      auto zero = llvm::ConstantInt::get(IGF.IGM.IntPtrTy, 0);
      auto isNonzero = IGF.Builder.CreateICmpNE(val, zero);
      // We either have extra inhabitant 0 or no extra inhabitant (-1).
      // Conveniently, this is just a sext i1 -> i32 away.
      return IGF.Builder.CreateSExt(isNonzero, IGF.IGM.Int32Ty);
    }

    void storeExtraInhabitant(IRGenFunction &IGF, llvm::Value *index,
                              Address dest, SILType T,
                              bool isOutlined) const override {
      // Copied from BridgeObjectTypeInfo.
      // There's only one extra inhabitant, 0.
      dest = IGF.Builder.CreateElementBitCast(dest, IGF.IGM.IntPtrTy);
      IGF.Builder.CreateStore(llvm::ConstantInt::get(IGF.IGM.IntPtrTy, 0),dest);
    }
  };

  /// A TypeInfo implementation for opaque storage. Swift will preserve any
  /// data stored into this arbitrarily sized and aligned field, but doesn't
  /// know anything about the data.
  class OpaqueStorageTypeInfo final :
    public ScalarTypeInfo<OpaqueStorageTypeInfo, LoadableTypeInfo>
  {
    std::vector<llvm::IntegerType *> ScalarTypes;
  public:
    OpaqueStorageTypeInfo(llvm::ArrayType *storage,
                          std::vector<llvm::IntegerType *> &&scalarTypes,
                          Size size,
                          SpareBitVector &&spareBits,
                          Alignment align)
      : ScalarTypeInfo(storage, size, std::move(spareBits), align,
                       IsTriviallyDestroyable,
                       IsCopyable,
                       IsFixedSize),
        ScalarTypes(std::move(scalarTypes))
    {}
    
    llvm::ArrayType *getStorageType() const {
      return cast<llvm::ArrayType>(ScalarTypeInfo::getStorageType());
    }

    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType T,
                          bool useStructLayouts) const override {
      if (!useStructLayouts) {
        return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
      }
      return IGM.typeLayoutCache.getOrCreateScalarEntry(*this, T,
                                            ScalarKind::TriviallyDestroyable);
    }

    unsigned getExplosionSize() const override {
      return ScalarTypes.size();
    }
    
    void loadAsCopy(IRGenFunction &IGF, Address addr,
                    Explosion &explosion) const override {
      loadAsTake(IGF, addr, explosion);
    }
    
    void loadAsTake(IRGenFunction &IGF, Address addr,
                    Explosion &explosion) const override {
      auto index = ScalarTypes.size();
      for (auto scalarTy : ScalarTypes) {
        addr = IGF.Builder.CreateElementBitCast(addr, scalarTy);
        explosion.add(IGF.Builder.CreateLoad(addr));
        --index;
        // Advance to next scalar chunk.
        if (index > 0) {
          addr = IGF.Builder.CreateElementBitCast(addr, IGF.IGM.Int8Ty);
          auto currentScalarTypeSize = Size(scalarTy->getIntegerBitWidth()/8);
          addr = IGF.Builder.CreateConstByteArrayGEP(addr, currentScalarTypeSize);
        }
      }
    }

    void assign(IRGenFunction &IGF, Explosion &explosion, Address addr,
                bool isOutlined, SILType T) const override {
      initialize(IGF, explosion, addr, isOutlined);
    }

    void initialize(IRGenFunction &IGF, Explosion &explosion, Address addr,
                    bool isOutlined) const override {
      auto index = ScalarTypes.size();
      for (auto scalarTy : ScalarTypes) {
        addr = IGF.Builder.CreateElementBitCast(addr, scalarTy);
        IGF.Builder.CreateStore(explosion.claimNext(), addr);
        --index;
        // Advance to next scalar chunk.
        if (index > 0) {
          addr = IGF.Builder.CreateElementBitCast(addr, IGF.IGM.Int8Ty);
          auto currentScalarTypeSize = Size(scalarTy->getIntegerBitWidth()/8);
          addr = IGF.Builder.CreateConstByteArrayGEP(addr, currentScalarTypeSize);
        }
      }
    }
    
    void reexplode(Explosion &sourceExplosion,
                   Explosion &targetExplosion) const override {
      for (auto scalarTy : ScalarTypes) {
        (void)scalarTy;
        targetExplosion.add(sourceExplosion.claimNext());
      }
    }
    
    void copy(IRGenFunction &IGF, Explosion &sourceExplosion,
              Explosion &targetExplosion, Atomicity atomicity) const override {
      reexplode(sourceExplosion, targetExplosion);
    }

    void consume(IRGenFunction &IGF, Explosion &explosion,
                 Atomicity atomicity, SILType T) const override {
      for (auto scalarTy: ScalarTypes) {
        (void)scalarTy;
        (void)explosion.claimNext();
      }
    }
    
    void fixLifetime(IRGenFunction &IGF, Explosion &explosion) const override {
      for (auto scalarTy: ScalarTypes) {
        (void)scalarTy;
        (void)explosion.claimNext();
      }
    }

    void destroy(IRGenFunction &IGF, Address address, SILType T,
                 bool isOutlined) const override {
      /* nop */
    }
    
    void getSchema(ExplosionSchema &schema) const override {
      for (auto scalarTy: ScalarTypes) {
        schema.add(ExplosionSchema::Element::forScalar(scalarTy));
      }
    }

    void addToAggLowering(IRGenModule &IGM, SwiftAggLowering &lowering,
                          Size offset) const override {
      lowering.addOpaqueData(offset.asCharUnits(),
                             (offset + getFixedSize()).asCharUnits());
    }
    
    void packIntoEnumPayload(IRGenModule &IGM,
                             IRBuilder &builder,
                             EnumPayload &payload,
                             Explosion &source,
                             unsigned offset) const override {
      for (auto scalarTy: ScalarTypes) {
        payload.insertValue(IGM, builder, source.claimNext(), offset);
        offset += scalarTy->getIntegerBitWidth();
      }
    }
    
    void unpackFromEnumPayload(IRGenFunction &IGF,
                               const EnumPayload &payload,
                               Explosion &target,
                               unsigned offset) const override {
      for (auto scalarTy : ScalarTypes) {
        target.add(payload.extractValue(IGF, scalarTy, offset));
        offset += scalarTy->getIntegerBitWidth();
      }
    }
  };

  template <class Impl, class Base>
  class ImmovableTypeInfoBase : public IndirectTypeInfo<Impl, Base> {
  public:
    template <class... Args>
    ImmovableTypeInfoBase(Args &&...args)
      : IndirectTypeInfo<Impl, Base>(std::forward<Args>(args)...) {}

    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType T,
                          bool useStructLayouts) const override {
      if (!useStructLayouts) {
        return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
      }
      return IGM.typeLayoutCache.getOrCreateScalarEntry(*this, T,
                                                        ScalarKind::Immovable);
    }

    void assignWithCopy(IRGenFunction &IGF, Address dest, Address src,
                        SILType T, bool isOutlined) const override {
      llvm_unreachable("cannot opaquely manipulate immovable types!");
    }

    void assignWithTake(IRGenFunction &IGF, Address dest, Address src,
                        SILType T, bool isOutlined) const override {
      llvm_unreachable("cannot opaquely manipulate immovable types!");
    }

    void initializeWithTake(IRGenFunction &IGF, Address destAddr,
                            Address srcAddr, SILType T,
                            bool isOutlined) const override {
      llvm_unreachable("cannot opaquely manipulate immovable types!");
    }

    void initializeWithCopy(IRGenFunction &IGF, Address destAddr,
                            Address srcAddr, SILType T,
                            bool isOutlined) const override {
      llvm_unreachable("cannot opaquely manipulate immovable types!");
    }

    void destroy(IRGenFunction &IGF, Address address, SILType T,
                 bool isOutlined) const override {
      llvm_unreachable("cannot opaquely manipulate immovable types!");
    }
  };

  /// A TypeInfo implementation for address-only types which can never
  /// be copied.
  class ImmovableTypeInfo :
    public ImmovableTypeInfoBase<ImmovableTypeInfo, FixedTypeInfo> {
  public:
    ImmovableTypeInfo(llvm::Type *storage, Size size,
                      SpareBitVector &&spareBits,
                      Alignment align)
      : ImmovableTypeInfoBase(storage, size, std::move(spareBits), align,
                              IsNotTriviallyDestroyable,
                              IsNotBitwiseTakable,
                              IsNotCopyable,
                              IsFixedSize) {}
  };

  /// A TypeInfo implementation for address-only types which can never
  /// be copied and whose layouts are not even dynamically knowable.
  class OpaqueImmovableTypeInfo :
    public ImmovableTypeInfoBase<OpaqueImmovableTypeInfo, TypeInfo> {
  public:
    OpaqueImmovableTypeInfo(llvm::Type *storage, Alignment minAlign)
      : ImmovableTypeInfoBase(storage, minAlign, IsNotTriviallyDestroyable,
                              IsNotBitwiseTakable,
                              IsNotCopyable,
                              IsNotFixedSize,
                              IsNotABIAccessible,
                              SpecialTypeInfoKind::None) {}

    llvm::Value *getSize(IRGenFunction &IGF, SILType T) const override {
      llvm_unreachable("should not call on an immovable opaque type");
    }
    llvm::Value *getAlignmentMask(IRGenFunction &IGF, SILType T) const override {
      llvm_unreachable("should not call on an immovable opaque type");
    }
    llvm::Value *getStride(IRGenFunction &IGF, SILType T) const override {
      llvm_unreachable("should not call on an immovable opaque type");
    }
    llvm::Value *getIsTriviallyDestroyable(IRGenFunction &IGF, SILType T) const override {
      llvm_unreachable("should not call on an immovable opaque type");
    }
    llvm::Value *getIsBitwiseTakable(IRGenFunction &IGF, SILType T) const override {
      llvm_unreachable("should not call on an immovable opaque type");
    }
    llvm::Value *isDynamicallyPackedInline(IRGenFunction &IGF,
                                          SILType T) const override {
      llvm_unreachable("should not call on an immovable opaque type");
    }
    llvm::Constant *getStaticSize(IRGenModule &IGM) const override {
      return nullptr;
    }
    llvm::Constant *getStaticAlignmentMask(IRGenModule &IGM) const override {
      return nullptr;
    }
    llvm::Constant *getStaticStride(IRGenModule &IGM) const override {
      return nullptr;
    }
    StackAddress allocateStack(IRGenFunction &IGF, SILType T,
                               const llvm::Twine &name) const override {
      llvm_unreachable("should not call on an immovable opaque type");
    }
    void deallocateStack(IRGenFunction &IGF, StackAddress addr,
                         SILType T) const override {
      llvm_unreachable("should not call on an immovable opaque type");
    }
    void destroyStack(IRGenFunction &IGF, StackAddress addr, SILType T,
                      bool isOutlined) const override {
      llvm_unreachable("should not call on an immovable opaque type");
    }
    void initializeFromParams(IRGenFunction &IGF, Explosion &params,
                              Address src, SILType T,
                              bool isOutlined) const override {
      llvm_unreachable("should not call on an immovable opaque type");
    }
    bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
      llvm_unreachable("should not call on an immovable opaque type");
    }
    llvm::Value *getEnumTagSinglePayload(IRGenFunction &IGF,
                                         llvm::Value *numEmptyCases,
                                         Address enumAddr,
                                         SILType T,
                                         bool isOutlined) const override {
      llvm_unreachable("should not call on an immovable opaque type");
    }
    void storeEnumTagSinglePayload(IRGenFunction &IGF,
                                   llvm::Value *whichCase,
                                   llvm::Value *numEmptyCases,
                                   Address enumAddr,
                                   SILType T,
                                   bool isOutlined) const override {
      llvm_unreachable("should not call on an immovable opaque type");
    }
  };
} // end anonymous namespace

/// Constructs a type info which performs simple loads and stores of
/// the given IR type.
const LoadableTypeInfo *
TypeConverter::createPrimitive(llvm::Type *type, Size size, Alignment align) {
  return new PrimitiveTypeInfo(type, size, IGM.getSpareBitsForType(type, size),
                               align);
}

static SpareBitVector getSpareBitsForAlignedPointer(IRGenModule &IGM,
                                                    Alignment pointeeAlign) {
  // FIXME: this is little-endian
  SpareBitVector spareBits = IGM.TargetInfo.PointerSpareBits;
  for (unsigned bit = 0; Alignment(1ull << bit) != pointeeAlign; ++bit) {
    spareBits.setBit(bit);
  }
  return spareBits;
}

static LoadableTypeInfo *createAlignedPointerTypeInfo(IRGenModule &IGM,
                                                      llvm::Type *ty,
                                                      Alignment pointeeAlign) {
  return new AlignedRawPointerTypeInfo(ty, IGM.getPointerSize(),
                              getSpareBitsForAlignedPointer(IGM, pointeeAlign),
                                       IGM.getPointerAlignment(),
                                       pointeeAlign);
}

/// Constructs a fixed-size type info which asserts if you try to copy
/// or destroy it.
const FixedTypeInfo *
TypeConverter::createImmovable(llvm::Type *type, Size size, Alignment align) {
  auto spareBits = SpareBitVector::getConstant(size.getValueInBits(), false);
  return new ImmovableTypeInfo(type, size, std::move(spareBits), align);
}

const TypeInfo *
TypeConverter::createOpaqueImmovable(llvm::Type *type, Alignment align) {
  return new OpaqueImmovableTypeInfo(type, align);
}

static TypeInfo *invalidTypeInfo() { return (TypeInfo*) 1; }

bool TypeConverter::readLegacyTypeInfo(llvm::vfs::FileSystem &fs,
                                       StringRef path) {
  auto fileOrErr = fs.getBufferForFile(path);
  if (!fileOrErr)
    return true;

  auto file = std::move(fileOrErr.get());

  llvm::yaml::Input yin(file->getBuffer());

  // Read the document list.
  std::vector<YAMLModuleNode> modules;
  yin >> modules;

  if (yin.error())
    return true;

  for (auto &module : modules) {
    for (auto &decl : module.Decls) {
      auto result = LegacyTypeInfos.insert(std::make_pair(
                                             decl.Name,
                                             decl));
      assert(result.second);
      (void) result;
    }
  }

  return false;
}

static std::string mangleTypeAsContext(const NominalTypeDecl *decl) {
  Mangle::ASTMangler Mangler;
  return Mangler.mangleTypeAsContextUSR(decl);
}

llvm::Optional<YAMLTypeInfoNode>
TypeConverter::getLegacyTypeInfo(NominalTypeDecl *decl) const {
  auto &mangledName = const_cast<TypeConverter *>(this)->DeclMangledNames[decl];
  if (mangledName.empty())
    mangledName = mangleTypeAsContext(decl);
  assert(!mangledName.empty());

  auto found = LegacyTypeInfos.find(mangledName);
  if (found == LegacyTypeInfos.end())
    return llvm::None;

  return found->second;
}

// The following Apple platforms support backward deployment of Swift
// code built with Swift 5.0 running on an old Objective-C runtime
// that does not support the class metadata update hook.
//
// We ship a YAML legacy type info file for these platforms as part
// of the toolchain.
static llvm::StringLiteral platformsWithLegacyLayouts[][2] = {
  {"appletvos", "arm64"},
  {"appletvsimulator", "x86_64"},
  {"iphoneos", "armv7"},
  {"iphoneos", "armv7s"},
  {"iphoneos", "arm64"},
  {"iphonesimulator", "i386"},
  {"iphonesimulator", "x86_64"},
  {"macosx", "x86_64"},
  {"watchos", "armv7k"},
  {"watchos", "arm64_32"},
  {"watchsimulator", "i386"}
};

static bool doesPlatformUseLegacyLayouts(StringRef platformName,
                                         StringRef archName) {
  for (auto platformArchPair : platformsWithLegacyLayouts) {
    if (platformName == platformArchPair[0] &&
        archName == platformArchPair[1]) {
      return true;
    }
  }

  return false;
}

TypeConverter::TypeConverter(IRGenModule &IGM)
  : IGM(IGM),
    FirstType(invalidTypeInfo()) {
  // Whether the Objective-C runtime is guaranteed to invoke the class
  // metadata update callback when realizing a Swift class referenced from
  // Objective-C.
  auto deploymentAvailability =
    AvailabilityContext::forDeploymentTarget(IGM.Context);
  bool supportsObjCMetadataUpdateCallback =
    deploymentAvailability.isContainedIn(
        IGM.Context.getObjCMetadataUpdateCallbackAvailability());

  // If our deployment target allows us to rely on the metadata update
  // callback being called, we don't have to emit a legacy layout for a
  // class with resiliently-sized fields.
  if (supportsObjCMetadataUpdateCallback)
    return;

  // We have a bunch of -parse-stdlib tests that pass a -target in the test
  // suite. To prevent these from failing when the user hasn't build the
  // standard library for that target, we pass -disable-legacy-type-info to
  // disable trying to load the legacy type info.
  if (IGM.IRGen.Opts.DisableLegacyTypeInfo)
    return;

  llvm::SmallString<128> defaultPath;

  StringRef path = IGM.IRGen.Opts.ReadLegacyTypeInfoPath;
  auto fs =
      IGM.getSwiftModule()->getASTContext().SourceMgr.getFileSystem();
  if (path.empty()) {
    const auto &Triple = IGM.Context.LangOpts.Target;

    // If the flag was not explicitly specified, look for a file in a
    // platform-specific location, if this platform is known to require
    // one.
    auto platformName = getPlatformNameForTriple(Triple);
    auto archName = swift::getMajorArchitectureName(Triple);

    if (!doesPlatformUseLegacyLayouts(platformName, archName))
      return;

    defaultPath = IGM.Context.SearchPathOpts.RuntimeLibraryPaths[0];
    llvm::sys::path::append(defaultPath, "layouts-");
    defaultPath.append(archName);
    defaultPath.append(".yaml");

    path = defaultPath;
  }

  bool error = readLegacyTypeInfo(*fs, path);
  if (error) {
    IGM.error(SourceLoc(), "Cannot read legacy layout file at '" + path + "'");
  }
}

TypeConverter::~TypeConverter() {
  // Delete all the converted type infos.
  for (const TypeInfo *I = FirstType; I != invalidTypeInfo(); ) {
    const TypeInfo *Cur = I;
    I = Cur->NextConverted;
    delete Cur;
  }
}

void TypeConverter::setGenericContext(CanGenericSignature signature) {
  CurGenericSignature = signature;

  // Clear the dependent type info cache since we have a new active signature
  // now.
  Types.getCacheFor(/*isDependent*/ true, Mode::Normal).clear();
  Types.getCacheFor(/*isDependent*/ true, Mode::Legacy).clear();
  Types.getCacheFor(/*isDependent*/ true, Mode::CompletelyFragile).clear();

  Types.getTypeLayoutCacheFor(/*isDependent*/ true, Mode::Normal).clear();
  Types.getTypeLayoutCacheFor(/*isDependent*/ true, Mode::Legacy).clear();
  Types.getTypeLayoutCacheFor(/*isDependent*/ true, Mode::CompletelyFragile)
      .clear();
}

CanGenericSignature IRGenModule::getCurGenericContext() {
  return Types.getCurGenericContext();
}

GenericEnvironment *TypeConverter::getGenericEnvironment() {
  return CurGenericSignature.getGenericEnvironment();
}

GenericEnvironment *IRGenModule::getGenericEnvironment() {
  return Types.getGenericEnvironment();
}

/// Add a temporary forward declaration for a type.  This will live
/// only until a proper mapping is added.
void TypeConverter::addForwardDecl(TypeBase *key) {
  assert(key->isCanonical());
  assert(!key->hasTypeParameter());
  auto &Cache = Types.getCacheFor(/*isDependent*/ false, LoweringMode);
  auto result = Cache.insert(std::make_pair(key, nullptr));
  assert(result.second && "entry already exists for type!");
  (void) result;
}

const TypeInfo &IRGenModule::getWitnessTablePtrTypeInfo() {
  return Types.getWitnessTablePtrTypeInfo();
}

const LoadableTypeInfo &TypeConverter::getWitnessTablePtrTypeInfo() {
  if (WitnessTablePtrTI) return *WitnessTablePtrTI;

  auto spareBits =
    getSpareBitsForAlignedPointer(IGM, IGM.getWitnessTableAlignment());

  // This is sub-optimal because it doesn't consider that there are
  // also potential extra inhabitants in witness table pointers, but
  // it's what we're currently doing, so we might be stuck.
  // TODO: it's likely that this never matters in the current ABI,
  // so we can just switch to using AlignedRawPointerTypeInfo; but
  // we need to check that first.
  WitnessTablePtrTI = new PrimitiveTypeInfo(IGM.WitnessTablePtrTy,
                                            IGM.getPointerSize(),
                                            std::move(spareBits),
                                            IGM.getPointerAlignment());
  WitnessTablePtrTI->NextConverted = FirstType;
  FirstType = WitnessTablePtrTI;
  return *WitnessTablePtrTI;
}

const SpareBitVector &IRGenModule::getWitnessTablePtrSpareBits() const {
  // Witness tables are pointers and have pointer spare bits.
  // FIXME: this is not what we use in getWitnessTablePtrTypeInfo()
  return TargetInfo.PointerSpareBits;
}

const TypeInfo &IRGenModule::getTypeMetadataPtrTypeInfo() {
  return Types.getTypeMetadataPtrTypeInfo();
}

const TypeInfo &TypeConverter::getTypeMetadataPtrTypeInfo() {
  if (TypeMetadataPtrTI) return *TypeMetadataPtrTI;
  TypeMetadataPtrTI = createUnmanagedStorageType(IGM.TypeMetadataPtrTy,
                                                 ReferenceCounting::Unknown,
                                                 /*isOptional*/false);
  TypeMetadataPtrTI->NextConverted = FirstType;
  FirstType = TypeMetadataPtrTI;
  return *TypeMetadataPtrTI;
}

const TypeInfo &IRGenModule::getSwiftContextPtrTypeInfo() {
  return Types.getSwiftContextPtrTypeInfo();
}

const TypeInfo &TypeConverter::getSwiftContextPtrTypeInfo() {
  if (SwiftContextPtrTI) return *SwiftContextPtrTI;
  SwiftContextPtrTI = createUnmanagedStorageType(IGM.SwiftContextPtrTy,
                                                 ReferenceCounting::Unknown,
                                                 /*isOptional*/false);
  SwiftContextPtrTI->NextConverted = FirstType;
  FirstType = SwiftContextPtrTI;
  return *SwiftContextPtrTI;
}

const TypeInfo &IRGenModule::getTaskContinuationFunctionPtrTypeInfo() {
  return Types.getTaskContinuationFunctionPtrTypeInfo();
}

const TypeInfo &TypeConverter::getTaskContinuationFunctionPtrTypeInfo() {
  if (TaskContinuationFunctionPtrTI) return *TaskContinuationFunctionPtrTI;
  TaskContinuationFunctionPtrTI = createUnmanagedStorageType(
      IGM.TaskContinuationFunctionPtrTy, ReferenceCounting::Unknown,
      /*isOptional*/ false);
  TaskContinuationFunctionPtrTI->NextConverted = FirstType;
  FirstType = TaskContinuationFunctionPtrTI;
  return *TaskContinuationFunctionPtrTI;
}

const LoadableTypeInfo &
IRGenModule::getReferenceObjectTypeInfo(ReferenceCounting refcounting) {
  switch (refcounting) {
  case ReferenceCounting::Native:
    return getNativeObjectTypeInfo();
  case ReferenceCounting::Unknown:
    return getUnknownObjectTypeInfo();
  case ReferenceCounting::Bridge:
    return getBridgeObjectTypeInfo();
  case ReferenceCounting::Block:
  case ReferenceCounting::Error:
  case ReferenceCounting::ObjC:
  case ReferenceCounting::Custom:
  case ReferenceCounting::None:
    llvm_unreachable("not implemented");
  }

  llvm_unreachable("Not a valid ReferenceCounting.");
}

const LoadableTypeInfo &IRGenModule::getNativeObjectTypeInfo() {
  return Types.getNativeObjectTypeInfo();
}

const LoadableTypeInfo &TypeConverter::getNativeObjectTypeInfo() {
  if (NativeObjectTI) return *NativeObjectTI;
  NativeObjectTI = convertBuiltinNativeObject();
  NativeObjectTI->NextConverted = FirstType;
  FirstType = NativeObjectTI;
  return *NativeObjectTI;
}

const LoadableTypeInfo &IRGenModule::getUnknownObjectTypeInfo() {
  return Types.getUnknownObjectTypeInfo();
}

const LoadableTypeInfo &TypeConverter::getUnknownObjectTypeInfo() {
  if (UnknownObjectTI) return *UnknownObjectTI;
  UnknownObjectTI = convertBuiltinUnknownObject();
  UnknownObjectTI->NextConverted = FirstType;
  FirstType = UnknownObjectTI;
  return *UnknownObjectTI;
}

const LoadableTypeInfo &IRGenModule::getBridgeObjectTypeInfo() {
  return Types.getBridgeObjectTypeInfo();
}

const LoadableTypeInfo &TypeConverter::getBridgeObjectTypeInfo() {
  if (BridgeObjectTI) return *BridgeObjectTI;
  BridgeObjectTI = convertBuiltinBridgeObject();
  BridgeObjectTI->NextConverted = FirstType;
  FirstType = BridgeObjectTI;
  return *BridgeObjectTI;
}

const LoadableTypeInfo &IRGenModule::getRawPointerTypeInfo() {
  return Types.getRawPointerTypeInfo();
}

const LoadableTypeInfo &TypeConverter::getRawPointerTypeInfo() {
  if (RawPointerTI) return *RawPointerTI;
  RawPointerTI = new RawPointerTypeInfo(IGM.Int8PtrTy,
                                        IGM.getPointerSize(),
                                        IGM.getPointerAlignment());
  RawPointerTI->NextConverted = FirstType;
  FirstType = RawPointerTI;
  return *RawPointerTI;
}

const LoadableTypeInfo &IRGenModule::getRawUnsafeContinuationTypeInfo() {
  return Types.getRawUnsafeContinuationTypeInfo();
}

const LoadableTypeInfo &TypeConverter::getRawUnsafeContinuationTypeInfo() {
  if (RawUnsafeContinuationTI) return *RawUnsafeContinuationTI;

  // A Builtin.RawUnsafeContinuation is an AsyncTask*, which is a heap
  // object aligned to 2*alignof(void*).  Incomplete tasks are
  // self-owning, which is to say that pointers to them can be held
  // reliably without retaining or releasing until the task starts
  // running again.
  //
  // TODO: It is possible to retain and release task pointers, which means
  // they can be used directly as Swift function contexts.  Preserve this
  // information to optimize closure-creation (partial apply).
  auto ty = IGM.Int8PtrTy;
  auto pointeeAlign = Alignment(2 * IGM.getPointerAlignment().getValue());
  RawUnsafeContinuationTI =
    createAlignedPointerTypeInfo(IGM, ty, pointeeAlign);
  RawUnsafeContinuationTI->NextConverted = FirstType;
  FirstType = RawUnsafeContinuationTI;
  return *RawUnsafeContinuationTI;
}

const LoadableTypeInfo &TypeConverter::getJobTypeInfo() {
  if (JobTI) return *JobTI;

  // A Builtin.Job is a Job*, which is an arbitrary pointer aligned to
  // 2*alignof(void*).  Jobs are self-owning, which is to say that
  // they're valid until they are scheduled, and then they're responsible
  // for destroying themselves.  (Jobs are often interior pointers into
  // an AsyncTask*, but that's not guaranteed.)
  auto ty = IGM.SwiftJobPtrTy;
  auto pointeeAlign = Alignment(2 * IGM.getPointerAlignment().getValue());
  JobTI = createAlignedPointerTypeInfo(IGM, ty, pointeeAlign);
  JobTI->NextConverted = FirstType;
  FirstType = JobTI;
  return *JobTI;
}

const LoadableTypeInfo &TypeConverter::getEmptyTypeInfo() {
  if (EmptyTI) return *EmptyTI;
  EmptyTI = new EmptyTypeInfo(IGM.Int8Ty);
  EmptyTI->NextConverted = FirstType;
  FirstType = EmptyTI;
  return *EmptyTI;
}

const TypeInfo &
TypeConverter::getDynamicTupleTypeInfo(IsCopyable_t isCopyable) {
  auto &cache = DynamicTupleTI[(unsigned)isCopyable];
  if (cache) return *cache;
  cache = convertDynamicTupleType(isCopyable);
  cache->NextConverted = FirstType;
  FirstType = cache;
  return *cache;
}

const TypeInfo &
TypeConverter::getResilientStructTypeInfo(IsCopyable_t isCopyable,
                                          IsABIAccessible_t isAccessible) {
  auto &cache = ResilientStructTI[(unsigned)isCopyable][(unsigned)isAccessible];
  if (cache) return *cache;
  cache = convertResilientStruct(isCopyable, isAccessible);
  cache->NextConverted = FirstType;
  FirstType = cache;
  return *cache;
}

/// Get the fragile type information for the given type, which may not
/// have yet undergone SIL type lowering.  The type can serve as its own
/// abstraction pattern.
const TypeInfo &IRGenFunction::getTypeInfoForUnlowered(Type subst) {
  return IGM.getTypeInfoForUnlowered(subst);
}

/// Get the fragile type information for the given type, which may not
/// have yet undergone SIL type lowering.
const TypeInfo &
IRGenFunction::getTypeInfoForUnlowered(AbstractionPattern orig, Type subst) {
  return IGM.getTypeInfoForUnlowered(orig, subst);
}

/// Get the fragile type information for the given type, which may not
/// have yet undergone SIL type lowering.
const TypeInfo &
IRGenFunction::getTypeInfoForUnlowered(AbstractionPattern orig, CanType subst) {
  return IGM.getTypeInfoForUnlowered(orig, subst);
}

/// Get the fragile type information for the given type, which is known
/// to have undergone SIL type lowering (or be one of the types for
/// which that lowering is the identity function).
const TypeInfo &IRGenFunction::getTypeInfoForLowered(CanType T) {
  return IGM.getTypeInfoForLowered(T);
}

/// Get the fragile type information for the given type.
const TypeInfo &IRGenFunction::getTypeInfo(SILType T) {
  return IGM.getTypeInfo(T);
}

/// Return the SIL-lowering of the given type.
SILType IRGenModule::getLoweredType(AbstractionPattern orig, Type subst) const {
  return getSILTypes().getLoweredType(
      orig, subst, TypeExpansionContext::maximalResilienceExpansionOnly());
}

/// Return the SIL-lowering of the given type.
SILType IRGenModule::getLoweredType(Type subst) const {
  return getSILTypes().getLoweredType(
      subst, TypeExpansionContext::maximalResilienceExpansionOnly());
}

/// Return the SIL-lowering of the given type.
const Lowering::TypeLowering &IRGenModule::getTypeLowering(SILType type) const {
  return getSILTypes().getTypeLowering(
      type, TypeExpansionContext::maximalResilienceExpansionOnly());
}

bool IRGenModule::isTypeABIAccessible(SILType type) const {
  return getSILModule().isTypeABIAccessible(
      type, TypeExpansionContext::maximalResilienceExpansionOnly());
}

/// Get a pointer to the storage type for the given type.  Note that,
/// unlike fetching the type info and asking it for the storage type,
/// this operation will succeed for forward-declarations.
llvm::PointerType *IRGenModule::getStoragePointerType(SILType T) {
  return getStoragePointerTypeForLowered(T.getASTType());
}
llvm::PointerType *IRGenModule::getStoragePointerTypeForUnlowered(Type T) {
  return getStorageTypeForUnlowered(T)->getPointerTo();
}
llvm::PointerType *IRGenModule::getStoragePointerTypeForLowered(CanType T) {
  return getStorageTypeForLowered(T)->getPointerTo();
}

llvm::Type *IRGenModule::getStorageTypeForUnlowered(Type subst) {
  return getStorageType(getLoweredType(subst));
}

llvm::Type *IRGenModule::getStorageType(SILType T) {
  return getStorageTypeForLowered(T.getASTType());
}

/// Get the storage type for the given type.
llvm::Type *IRGenModule::getStorageTypeForLowered(CanType T) {
  return Types.getTypeEntry(T)->getStorageType();
}

/// Get the type information for the given type, which may not have
/// yet undergone SIL type lowering.  The type can serve as its own
/// abstraction pattern.
const TypeInfo &IRGenModule::getTypeInfoForUnlowered(Type subst) {
  return getTypeInfoForUnlowered(AbstractionPattern(subst), subst);
}

/// Get the type information for the given type, which may not
/// have yet undergone SIL type lowering.
const TypeInfo &
IRGenModule::getTypeInfoForUnlowered(AbstractionPattern orig, Type subst) {
  return getTypeInfoForUnlowered(orig, subst->getCanonicalType());
}

/// Get the type information for the given type, which may not
/// have yet undergone SIL type lowering.
const TypeInfo &
IRGenModule::getTypeInfoForUnlowered(AbstractionPattern orig, CanType subst) {
  return getTypeInfo(getLoweredType(orig, subst));
}

/// Get the fragile type information for the given type, which is known
/// to have undergone SIL type lowering (or be one of the types for
/// which that lowering is the identity function).
const TypeInfo &IRGenModule::getTypeInfo(SILType T) {
  return getTypeInfoForLowered(T.getASTType());
}

/// Get the fragile type information for the given type.
const TypeInfo &IRGenModule::getTypeInfoForLowered(CanType T) {
  return Types.getCompleteTypeInfo(T);
}

/// 
const TypeInfo &TypeConverter::getCompleteTypeInfo(CanType T) {
  return *getTypeEntry(T);
}

ArchetypeType *TypeConverter::getExemplarArchetype(ArchetypeType *t) {
  if (isa<LocalArchetypeType>(t) || isa<OpaqueTypeArchetypeType>(t))
    return t;

  assert(isa<PrimaryArchetypeType>(t) || isa<PackArchetypeType>(t));

  // Get the root archetype.
  auto root = t->getRoot();

  // Retrieve the generic environment of the archetype.
  auto genericEnv = root->getGenericEnvironment();

  // Dig out the canonical generic environment.
  auto genericSig = genericEnv->getGenericSignature();
  auto canGenericSig = genericSig.getCanonicalSignature();
  auto canGenericEnv = canGenericSig.getGenericEnvironment();
  if (canGenericEnv == genericEnv) return t;

  // Map the archetype out of its own generic environment and into the
  // canonical generic environment.
  auto interfaceType = t->getInterfaceType();
  auto exemplar = canGenericEnv->mapTypeIntoContext(interfaceType)
                    ->castTo<ArchetypeType>();
  assert(isExemplarArchetype(exemplar));
  return exemplar;
}

/// Fold archetypes to unique exemplars. Any archetype with the same
/// constraints is equivalent for type lowering purposes.
CanType TypeConverter::getExemplarType(CanType contextTy) {
  // FIXME: A generic SILFunctionType should not contain any nondependent
  // archetypes.
  if (isa<SILFunctionType>(contextTy)
      && cast<SILFunctionType>(contextTy)->isPolymorphic()) {
    return contextTy;
  } else {
    auto exemplified = contextTy.subst(
      [&](SubstitutableType *type) -> Type {
        if (auto arch = dyn_cast<ArchetypeType>(type))
          return getExemplarArchetype(arch);
        return type;
      },
      MakeAbstractConformanceForGenericType(),
      SubstFlags::AllowLoweredTypes |
      SubstFlags::PreservePackExpansionLevel);
    return CanType(exemplified);
  }
}
const TypeLayoutEntry
&TypeConverter::getTypeLayoutEntry(SILType T, bool useStructLayouts) {
  auto astTy = T.getASTType();
  auto cache =
      Types.getTypeLayoutCacheFor(astTy->hasTypeParameter(), LoweringMode);
  auto it = cache.find(astTy.getPointer());
  if (it != cache.end()) {
    return *it->second;
  }
  auto *ti = getTypeEntry(T.getASTType());
  auto *entry = ti->buildTypeLayoutEntry(IGM, T, useStructLayouts);
  cache[astTy.getPointer()] = entry;
  return *entry;
}

const TypeInfo *TypeConverter::getTypeEntry(CanType canonicalTy) {
  // Cache this entry in the dependent or independent cache appropriate to it.
  auto &Cache = Types.getCacheFor(canonicalTy->hasTypeParameter(),
                                  LoweringMode);

  {
    auto it = Cache.find(canonicalTy.getPointer());
    if (it != Cache.end()) {
      return it->second;
    }
  }

  // If the type is dependent, substitute it into our current context.
  auto contextTy = canonicalTy;
  if (contextTy->hasTypeParameter()) {
    // The type we got should be lowered, so lower it like a SILType.
    contextTy = getGenericEnvironment()->mapTypeIntoContext(
                  IGM.getSILModule(),
                  SILType::getPrimitiveAddressType(contextTy)).getASTType();
  }
  
  // Fold archetypes to unique exemplars. Any archetype with the same
  // constraints is equivalent for type lowering purposes.
  CanType exemplarTy = getExemplarType(contextTy);
  assert(!exemplarTy->hasTypeParameter());
  
  // See whether we lowered a type equivalent to this one.
  if (exemplarTy != canonicalTy) {
    auto &Cache = Types.getCacheFor(/*isDependent*/ false, LoweringMode);
    auto it = Cache.find(exemplarTy.getPointer());
    if (it != Cache.end()) {
      // Record the object under the original type.
      auto result = it->second;
      Cache[canonicalTy.getPointer()] = result;
      return result;
    }
  }

  // Convert the type.
  auto *convertedTI = convertType(exemplarTy);

  // Cache the entry under the original type and the exemplar type, so that
  // we can avoid relowering equivalent types.
  auto insertEntry = [&](const TypeInfo *&entry) {
    assert(entry == nullptr);
    entry = convertedTI;
  };
  insertEntry(Cache[canonicalTy.getPointer()]);
  if (canonicalTy != exemplarTy) {
    auto &IndependentCache = Types.getCacheFor(/*isDependent*/ false,
                                               LoweringMode);
    insertEntry(IndependentCache[exemplarTy.getPointer()]);
  }
  
  // If the type info hasn't been added to the list of types, do so.
  if (!convertedTI->NextConverted) {
    convertedTI->NextConverted = FirstType;
    FirstType = convertedTI;
  }

  return convertedTI;
}

/// Return a TypeInfo the represents opaque storage for a loadable POD value
/// with the given storage size.
///
/// The formal alignment of the opaque storage will be 1.
///
/// The TypeInfo will completely ignore any type passed to its
/// implementation methods; it is safe to pass a null type.
const LoadableTypeInfo &
IRGenModule::getOpaqueStorageTypeInfo(Size size, Alignment align) {
  return Types.getOpaqueStorageTypeInfo(size, align);
}

const LoadableTypeInfo &
TypeConverter::getOpaqueStorageTypeInfo(Size size, Alignment align) {
  assert(!size.isZero());
  std::pair<unsigned, unsigned> key = {size.getValue(), align.getValue()};
  auto existing = OpaqueStorageTypes.find(key);
  if (existing != OpaqueStorageTypes.end())
    return *existing->second;

  // Use an [N x i8] array for storage, but load and store as a single iNNN
  // scalar.
  auto storageType = llvm::ArrayType::get(IGM.Int8Ty, size.getValue());

  // Create chunks of MAX_INT_BITS integer scalar types if necessary.
  std::vector<llvm::IntegerType*> scalarTypes;
  Size chunkSize = size;
  auto maxChunkSize = Size(llvm::IntegerType::MAX_INT_BITS/8);
  while (chunkSize) {
    if (chunkSize > maxChunkSize) {
      auto intType = llvm::IntegerType::get(IGM.getLLVMContext(),
                                            maxChunkSize.getValueInBits());
      scalarTypes.push_back(intType);
      chunkSize -= maxChunkSize;
      continue;
    }
    auto intType = llvm::IntegerType::get(IGM.getLLVMContext(),
                                          chunkSize.getValueInBits());
    scalarTypes.push_back(intType);
    chunkSize = Size(0);
  }

  // There are no spare bits in an opaque storage type.
  auto type = new OpaqueStorageTypeInfo(storageType, std::move(scalarTypes),
                    size,
                    SpareBitVector::getConstant(size.getValueInBits(), false),
                    align);
  
  type->NextConverted = FirstType;
  FirstType = type;

  OpaqueStorageTypes[key] = type;

  return *type;
}

/// Convert a primitive builtin type to its LLVM type, size, and
/// alignment.
static std::tuple<llvm::Type *, Size, Alignment>
convertPrimitiveBuiltin(IRGenModule &IGM, CanType canTy) {
  using RetTy = std::tuple<llvm::Type *, Size, Alignment>;
  llvm::LLVMContext &ctx = IGM.getLLVMContext();
  TypeBase *ty = canTy.getPointer();
  switch (ty->getKind()) {
  case TypeKind::BuiltinRawPointer:
    return RetTy{ IGM.Int8PtrTy, IGM.getPointerSize(),
                  IGM.getPointerAlignment() };
  case TypeKind::BuiltinFloat:
    switch (cast<BuiltinFloatType>(ty)->getFPKind()) {
    case BuiltinFloatType::IEEE16:
      return RetTy{ llvm::Type::getHalfTy(ctx), Size(2), Alignment(2) };
    case BuiltinFloatType::IEEE32:
      return RetTy{ llvm::Type::getFloatTy(ctx), Size(4), Alignment(4) };
    case BuiltinFloatType::IEEE64:
      return RetTy{ llvm::Type::getDoubleTy(ctx), Size(8), Alignment(8) };
    case BuiltinFloatType::IEEE80: {
      llvm::Type *floatTy = llvm::Type::getX86_FP80Ty(ctx);
      uint64_t ByteSize = IGM.DataLayout.getTypeAllocSize(floatTy);
      unsigned align = IGM.DataLayout.getABITypeAlignment(floatTy);
      return RetTy{ floatTy, Size(ByteSize), Alignment(align) };
    }
    case BuiltinFloatType::IEEE128:
      return RetTy{ llvm::Type::getFP128Ty(ctx), Size(16), Alignment(16) };
    case BuiltinFloatType::PPC128:
      return RetTy{ llvm::Type::getPPC_FP128Ty(ctx),Size(16), Alignment(16) };
    }
    llvm_unreachable("bad builtin floating-point type kind");
  case TypeKind::BuiltinInteger: {
    auto intTy = cast<BuiltinIntegerType>(ty);
    unsigned BitWidth = IGM.getBuiltinIntegerWidth(intTy);
    unsigned ByteSize = (BitWidth+7U)/8U;
    // Round up the memory size and alignment to a power of 2.
    if (!llvm::isPowerOf2_32(ByteSize))
      ByteSize = llvm::NextPowerOf2(ByteSize);

    return RetTy{ llvm::IntegerType::get(ctx, BitWidth), Size(ByteSize),
             Alignment(ByteSize) };
  }
  case TypeKind::BuiltinVector: {
    auto vecTy = ty->castTo<BuiltinVectorType>();
    llvm::Type *elementTy;
    Size size;
    Alignment align;
    std::tie(elementTy, size, align)
      = convertPrimitiveBuiltin(IGM,
                                vecTy->getElementType()->getCanonicalType());

    auto llvmVecTy =
        llvm::FixedVectorType::get(elementTy, vecTy->getNumElements());

    unsigned bitSize = size.getValue() * vecTy->getNumElements() * 8;
    if (!llvm::isPowerOf2_32(bitSize))
      bitSize = llvm::NextPowerOf2(bitSize);

    return RetTy{ llvmVecTy, Size(bitSize / 8), align };
  }
  default:
    llvm_unreachable("Not a primitive builtin type");
  }
}

const TypeInfo *TypeConverter::convertType(CanType ty) {
  PrettyStackTraceType stackTrace(IGM.Context, "converting", ty);

  switch (ty->getKind()) {
  case TypeKind::Error: {
    // We might see error types if type checking has failed.
    // Try to do something graceful and return an zero sized type.
    auto &ctx = IGM.Context;
    return convertTupleType(cast<TupleType>(ctx.TheEmptyTupleType));
  }
#define UNCHECKED_TYPE(id, parent) \
  case TypeKind::id: \
    llvm_unreachable("found a " #id "Type in IR-gen");
#define SUGARED_TYPE(id, parent) \
  case TypeKind::id: \
    llvm_unreachable("converting a " #id "Type after canonicalization");
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
  case TypeKind::LValue:
    llvm_unreachable("@lvalue type made it to IRGen");
  case TypeKind::BuiltinTuple:
    llvm_unreachable("BuiltinTupleType made it to IRGen");
  case TypeKind::ExistentialMetatype:
    return convertExistentialMetatypeType(cast<ExistentialMetatypeType>(ty));
  case TypeKind::Metatype:
    return convertMetatypeType(cast<MetatypeType>(ty));
  case TypeKind::Module:
    return convertModuleType(cast<ModuleType>(ty));
  case TypeKind::DynamicSelf: {
    // DynamicSelf has the same representation as its superclass type.
    auto dynamicSelf = cast<DynamicSelfType>(ty);
    auto nominal = dynamicSelf->getSelfType()->getAnyNominal();
    return convertAnyNominalType(ty, nominal);
  }
  case TypeKind::BuiltinNativeObject:
    return &getNativeObjectTypeInfo();
  case TypeKind::BuiltinBridgeObject:
    return &getBridgeObjectTypeInfo();
  case TypeKind::BuiltinUnsafeValueBuffer:
    return createImmovable(IGM.getFixedBufferTy(),
                           getFixedBufferSize(IGM),
                           getFixedBufferAlignment(IGM));
  case TypeKind::BuiltinRawPointer:
    return &getRawPointerTypeInfo();
  case TypeKind::BuiltinRawUnsafeContinuation:
    return &getRawUnsafeContinuationTypeInfo();
  case TypeKind::BuiltinJob:
    return &getJobTypeInfo();
  case TypeKind::BuiltinExecutor:
    return &getExecutorTypeInfo();
  case TypeKind::BuiltinIntegerLiteral:
    return &getIntegerLiteralTypeInfo();
  case TypeKind::BuiltinPackIndex:
    return createPrimitive(IGM.SizeTy, IGM.getPointerSize(),
                           IGM.getCappedAlignment(IGM.getPointerAlignment()));
  case TypeKind::BuiltinFloat:
  case TypeKind::BuiltinInteger:
  case TypeKind::BuiltinVector: {
    llvm::Type *llvmTy;
    Size size;
    Alignment align;
    std::tie(llvmTy, size, align) = convertPrimitiveBuiltin(IGM, ty);
    align = IGM.getCappedAlignment(align);
    return createPrimitive(llvmTy, size, align);
  }
  case TypeKind::BuiltinDefaultActorStorage: {
    // Builtin.DefaultActorStorage represents the extra storage
    // (beyond the heap header) of a default actor.  It is
    // fixed-size and totally opaque.
    auto numWords = NumWords_DefaultActor;

    auto ty = llvm::StructType::create(IGM.getLLVMContext(),
                                 llvm::ArrayType::get(IGM.Int8PtrTy, numWords),
                                       "swift.defaultactor");
    auto size = IGM.getPointerSize() * numWords;
    auto align = Alignment(2 * IGM.getPointerAlignment().getValue());
    auto spareBits = SpareBitVector::getConstant(size.getValueInBits(), false);
    return new PrimitiveTypeInfo(ty, size, std::move(spareBits), align);
  }
  case TypeKind::BuiltinNonDefaultDistributedActorStorage: {
    // Builtin.NonDefaultDistributedActorStorage represents the extra storage
    // (beyond the heap header) of a distributed actor that is not a default actor.
    // It is fixed-size and totally opaque.
    auto numWords = NumWords_NonDefaultDistributedActor;

    auto ty = llvm::StructType::create(IGM.getLLVMContext(),
                                 llvm::ArrayType::get(IGM.Int8PtrTy, numWords),
                                       "swift.nondefaultdistributedactor");
    auto size = IGM.getPointerSize() * numWords;
    auto align = Alignment(2 * IGM.getPointerAlignment().getValue());
    auto spareBits = SpareBitVector::getConstant(size.getValueInBits(), false);
    return new PrimitiveTypeInfo(ty, size, std::move(spareBits), align);
  }

  case TypeKind::PrimaryArchetype:
  case TypeKind::OpenedArchetype:
  case TypeKind::OpaqueTypeArchetype:
  case TypeKind::ElementArchetype:
    return convertArchetypeType(cast<ArchetypeType>(ty));
  case TypeKind::Class:
  case TypeKind::Enum:
  case TypeKind::Struct:
    return convertAnyNominalType(ty, cast<NominalType>(ty)->getDecl());
  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericEnum:
  case TypeKind::BoundGenericStruct:
    return convertAnyNominalType(ty, cast<BoundGenericType>(ty)->getDecl());
  case TypeKind::SILMoveOnlyWrapped:
    llvm_unreachable("implement this");
  case TypeKind::InOut:
    return convertInOutType(cast<InOutType>(ty));
  case TypeKind::Tuple:
    return convertTupleType(cast<TupleType>(ty));
  case TypeKind::Function:
  case TypeKind::GenericFunction:
    llvm_unreachable("AST FunctionTypes should be lowered by SILGen");
  case TypeKind::SILFunction:
    return convertFunctionType(cast<SILFunctionType>(ty));
  case TypeKind::Protocol:
    return convertProtocolType(cast<ProtocolType>(ty));
  case TypeKind::ProtocolComposition:
    return convertProtocolCompositionType(cast<ProtocolCompositionType>(ty));
  case TypeKind::ParameterizedProtocol:
    return convertParameterizedProtocolType(cast<ParameterizedProtocolType>(ty));
  case TypeKind::Existential:
    return convertExistentialType(cast<ExistentialType>(ty));
  case TypeKind::GenericTypeParam:
  case TypeKind::DependentMember:
    llvm_unreachable("can't convert dependent type");
#define REF_STORAGE(Name, ...) \
  case TypeKind::Name##Storage: \
    return convert##Name##StorageType(cast<Name##StorageType>(ty));
#include "swift/AST/ReferenceStorage.def"
  case TypeKind::SILBlockStorage: {
    return convertBlockStorageType(cast<SILBlockStorageType>(ty));
  case TypeKind::SILBox:
    return convertBoxType(cast<SILBoxType>(ty));
  case TypeKind::Pack:
    llvm_unreachable("AST pack types should be lowered by SILGen");
  case TypeKind::SILPack:
    return convertPackType(cast<SILPackType>(ty));
  case TypeKind::PackArchetype:
  case TypeKind::PackExpansion:
  case TypeKind::PackElement:
    llvm_unreachable("pack archetypes and expansions should not be seen in "
                     " arbitrary type positions");
  case TypeKind::SILToken:
    llvm_unreachable("should not be asking for representation of a SILToken");
  }
  }
  llvm_unreachable("bad type kind");
}

/// Convert an inout type.  This is always just a bare pointer.
const TypeInfo *TypeConverter::convertInOutType(InOutType *T) {
  auto referenceType =
    IGM.getStoragePointerTypeForUnlowered(CanType(T->getObjectType()));
  
  // Just use the reference type as a primitive pointer.
  return createPrimitive(referenceType, IGM.getPointerSize(),
                         IGM.getPointerAlignment());
}

const TypeInfo *TypeConverter::convertPackType(SILPackType *pack) {
  if (pack->isElementAddress())
    return createOpaqueImmovable(IGM.OpaquePtrTy, IGM.getPointerAlignment());
  return createOpaqueImmovable(IGM.OpaqueTy, Alignment(1));
}

/// Convert a reference storage type. The implementation here depends on the
/// underlying reference type. The type may be optional.
#define REF_STORAGE(Name, ...) \
const TypeInfo * \
TypeConverter::convert##Name##StorageType(Name##StorageType *refType) { \
  CanType referent(refType->getReferentType()); \
  bool isOptional = false; \
  if (auto referentObj = referent.getOptionalObjectType()) { \
    referent = referentObj; \
    isOptional = true; \
  } \
  assert(referent->allowsOwnership()); \
  auto &referentTI = cast<ReferenceTypeInfo>(getCompleteTypeInfo(referent)); \
  return referentTI.create##Name##StorageType(*this, isOptional); \
}
#include "swift/AST/ReferenceStorage.def"

static void overwriteForwardDecl(llvm::DenseMap<TypeBase *, const TypeInfo *> &cache,
                                 TypeBase *key, const TypeInfo *result) {
  assert(cache.count(key) && "no forward declaration?");
  assert(cache[key] == nullptr && "overwriting real entry!");
  cache[key] = result;
}

namespace {
  /// Is IR-gen type-dependent for the given type?  Specifically, will
  /// basic operations on the type misbehave (e.g. by having an IR
  /// type mismatch) if an aggregate type containing a value of this
  /// type is generated generically rather than independently for
  /// different specializations?
  class IsIRTypeDependent : public CanTypeVisitor<IsIRTypeDependent, bool> {
    IRGenModule &IGM;
  public:
    IsIRTypeDependent(IRGenModule &IGM) : IGM(IGM) {}

    // If the type isn't actually dependent, we're okay.
    bool visit(CanType type) {
      if (!type->hasArchetype() && !type->hasTypeParameter())
        return false;
      return CanTypeVisitor::visit(type);
    }

    // Dependent struct types need their own implementation if any
    // field type might need its own implementation.
    bool visitStructType(CanStructType type) {
      return visitStructDecl(type->getDecl());
    }
    bool visitBoundGenericStructType(CanBoundGenericStructType type) {
      return visitStructDecl(type->getDecl());
    }
    bool visitStructDecl(StructDecl *decl) {
      if (IGM.isResilient(decl, ResilienceExpansion::Maximal))
        return true;

      for (auto field : decl->getStoredProperties()) {
        if (visit(field->getInterfaceType()->getCanonicalType()))
          return true;
      }
      return false;
    }

    // Dependent enum types need their own implementation if any
    // element payload type might need its own implementation.
    bool visitEnumType(CanEnumType type) {
      return visitEnumDecl(type->getDecl());
    }
    bool visitBoundGenericEnumType(CanBoundGenericEnumType type) {
      return visitEnumDecl(type->getDecl());
    }
    bool visitEnumDecl(EnumDecl *decl) {
      if (IGM.isResilient(decl, ResilienceExpansion::Maximal))
        return true;
      if (decl->isIndirect())
        return false;

      for (auto elt : decl->getAllElements()) {
        if (!elt->hasAssociatedValues() || elt->isIndirect())
          continue;

        if (visit(elt->getArgumentInterfaceType()->getCanonicalType()))
          return true;
      }
      return false;
    }

    // Conservatively assume classes need unique implementations.
    bool visitClassType(CanClassType type) {
      return visitClassDecl(type->getDecl());
     }
    bool visitBoundGenericClassType(CanBoundGenericClassType type) {
      return visitClassDecl(type->getDecl());
    }
    bool visitClassDecl(ClassDecl *theClass) {
      return true;
    }

    // Reference storage types propagate the decision.
    bool visitReferenceStorageType(CanReferenceStorageType type) {
      return visit(type.getReferentType());
    }

    // The IR-generation for function types is specifically not
    // type-dependent.
    bool visitAnyFunctionType(CanAnyFunctionType type) {
      return false;
    }

    // The safe default for a dependent type is to assume that it
    // needs its own implementation.
    bool visitType(CanType type) {
      return true;
    }
  };
} // end anonymous namespace

static bool isIRTypeDependent(IRGenModule &IGM, NominalTypeDecl *decl) {
  assert(!isa<ProtocolDecl>(decl));
  if (auto classDecl = dyn_cast<ClassDecl>(decl)) {
    return IsIRTypeDependent(IGM).visitClassDecl(classDecl);
  } else if (auto structDecl = dyn_cast<StructDecl>(decl)) {
    return IsIRTypeDependent(IGM).visitStructDecl(structDecl);
  } else {
    auto enumDecl = cast<EnumDecl>(decl);
    return IsIRTypeDependent(IGM).visitEnumDecl(enumDecl);
  }
}

namespace {

class LegacyTypeInfo : public FixedTypeInfo {
  unsigned NumExtraInhabitants;

public:
  LegacyTypeInfo(llvm::Type *type, const SpareBitVector &spareBits,
                 const YAMLTypeInfoNode &node)
    : FixedTypeInfo(type,
                    Size(node.Size),
                    spareBits,
                    Alignment(node.Alignment),
                    IsNotTriviallyDestroyable, /* irrelevant */
                    IsNotBitwiseTakable, /* irrelevant */
                    IsCopyable, /* irrelevant */
                    IsFixedSize /* irrelevant */),
      NumExtraInhabitants(node.NumExtraInhabitants) {}

  TypeLayoutEntry
  *buildTypeLayoutEntry(IRGenModule &IGM,
                        SILType T,
                        bool useStructLayouts) const override {
    llvm_unreachable("Cannot construct type layout for legacy types");
  }

  virtual unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
    return NumExtraInhabitants;
  }

  virtual APInt getFixedExtraInhabitantMask(IRGenModule &IGM) const override {
    llvm_unreachable("TypeConverter::Mode::Legacy is not for real values");
  }

  virtual APInt getFixedExtraInhabitantValue(IRGenModule &IGM,
                                             unsigned bits,
                                             unsigned index) const override {
    llvm_unreachable("TypeConverter::Mode::Legacy is not for real values");
  }

  virtual void getSchema(ExplosionSchema &schema) const override {
    llvm_unreachable("TypeConverter::Mode::Legacy is not for real values");
  }

  virtual void assignWithCopy(IRGenFunction &IGF, Address dest, Address src,
                              SILType T, bool isOutlined) const override {
    llvm_unreachable("TypeConverter::Mode::Legacy is not for real values");
  }

  virtual void assignWithTake(IRGenFunction &IGF, Address dest, Address src,
                              SILType T, bool isOutlined) const override {
    llvm_unreachable("TypeConverter::Mode::Legacy is not for real values");
  }

  virtual void initializeWithCopy(IRGenFunction &IGF, Address destAddr,
                                  Address srcAddr, SILType T,
                                  bool isOutlined) const override {
    llvm_unreachable("TypeConverter::Mode::Legacy is not for real values");
  }

  virtual void initializeFromParams(IRGenFunction &IGF, Explosion &params,
                                    Address src, SILType T,
                                    bool isOutlined) const override {
    llvm_unreachable("TypeConverter::Mode::Legacy is not for real values");
  }

  virtual void destroy(IRGenFunction &IGF, Address address, SILType T,
                       bool isOutlined) const override {
    llvm_unreachable("TypeConverter::Mode::Legacy is not for real values");
  }
};

} // namespace

const TypeInfo *TypeConverter::convertAnyNominalType(CanType type,
                                                     NominalTypeDecl *decl) {
  // By "any", we don't mean existentials.
  assert(!isa<ProtocolDecl>(decl));

  // If we're producing a legacy type layout, and we have a serialized
  // record for this type, produce it now.
  if (LoweringMode == Mode::Legacy) {
    auto node = getLegacyTypeInfo(decl);

    if (node) {
      Size size(node->Size);

      auto ty = IGM.createNominalType(type);
      ty->setBody(llvm::ArrayType::get(IGM.Int8Ty, size.getValue()));

      SpareBitVector spareBits;
      spareBits.appendClearBits(size.getValueInBits());

      return new LegacyTypeInfo(ty, spareBits, *node);
    }
  }

  // We need to give generic specializations distinct TypeInfo objects
  // if their IR-gen might be different, e.g. if they use different IR
  // types or if type-specific operations like projections might need
  // to be handled differently.
  if (!decl->isGenericContext() || isIRTypeDependent(IGM, decl)) {
    switch (decl->getKind()) {
#define NOMINAL_TYPE_DECL(ID, PARENT)
#define DECL(ID, PARENT) \
    case DeclKind::ID:
#include "swift/AST/DeclNodes.def"
      llvm_unreachable("not a nominal type declaration");
    case DeclKind::Protocol:
      llvm_unreachable("protocol types shouldn't be handled here");

    case DeclKind::Class:
      return convertClassType(type, cast<ClassDecl>(decl));
    case DeclKind::Enum:
      return convertEnumType(type.getPointer(), type, cast<EnumDecl>(decl));
    case DeclKind::Struct:
      return convertStructType(type.getPointer(), type, cast<StructDecl>(decl));

    case DeclKind::BuiltinTuple:
      llvm_unreachable("BuiltinTupleType should not show up here");
    }
    llvm_unreachable("bad declaration kind");
  }

  assert(decl->isGenericContext());

  // Look to see if we've already emitted this type under a different
  // set of arguments.  We cache under the unbound type, which should
  // never collide with anything.
  //
  // FIXME: this isn't really inherently good; we might want to use
  // different type implementations for different applications.
  assert(decl->getDeclaredType()->isCanonical());
  assert(decl->getDeclaredType()->hasUnboundGenericType());
  TypeBase *key = decl->getDeclaredType().getPointer();
  auto &Cache = Types.getCacheFor(/*isDependent*/ false, LoweringMode);
  auto entry = Cache.find(key);
  if (entry != Cache.end())
    return entry->second;

  switch (decl->getKind()) {
#define NOMINAL_TYPE_DECL(ID, PARENT)
#define DECL(ID, PARENT) \
  case DeclKind::ID:
#include "swift/AST/DeclNodes.def"
    llvm_unreachable("not a nominal type declaration");

  case DeclKind::Protocol:
    llvm_unreachable("protocol types don't take generic parameters");

  case DeclKind::Class:
    llvm_unreachable("classes are always considered dependent for now");

  case DeclKind::BuiltinTuple:
    llvm_unreachable("BuiltinTupleType should not show up here");

  case DeclKind::Enum: {
    auto type = decl->getDeclaredTypeInContext()->getCanonicalType();
    auto result = convertEnumType(key, type, cast<EnumDecl>(decl));
    overwriteForwardDecl(Cache, key, result);
    return result;
  }

  case DeclKind::Struct: {
    auto type = decl->getDeclaredTypeInContext()->getCanonicalType();
    auto result = convertStructType(key, type, cast<StructDecl>(decl));
    overwriteForwardDecl(Cache, key, result);
    return result;
  }
  }
  llvm_unreachable("bad declaration kind");
}

const TypeInfo *TypeConverter::convertModuleType(ModuleType *T) {
  return new EmptyTypeInfo(IGM.Int8Ty);
}

const TypeInfo *TypeConverter::convertMetatypeType(MetatypeType *T) {
  assert(T->hasRepresentation() &&
         "metatype should have been assigned a representation by SIL");

  return &getMetatypeTypeInfo(T->getRepresentation());
}

const TypeInfo &
TypeConverter::getMetatypeTypeInfo(MetatypeRepresentation representation) {
  switch (representation) {
  case MetatypeRepresentation::Thin:
    // Thin metatypes are empty.
    return getEmptyTypeInfo();

  case MetatypeRepresentation::Thick:
    // Thick metatypes are represented with a metadata pointer.
    return getTypeMetadataPtrTypeInfo();

  case MetatypeRepresentation::ObjC:
    // ObjC metatypes are represented with an objc_class pointer.
    return getObjCClassPtrTypeInfo();
  }
  llvm_unreachable("bad representation");
}

/// createNominalType - Create a new nominal type.
llvm::StructType *IRGenModule::createNominalType(CanType type) {
  assert(type.getNominalOrBoundGenericNominal());

  // We share type infos for different instantiations of a generic type
  // when the archetypes have the same exemplars.  We cannot mangle
  // archetypes, and the mangling does not have to be unique, so we just
  // mangle the unbound generic form of the type.
  if (type->hasArchetype())
    type = type.getNominalOrBoundGenericNominal()->getDeclaredType()
                                                 ->getCanonicalType();

  IRGenMangler Mangler;
  std::string typeName = Mangler.mangleTypeForLLVMTypeName(type);
  return llvm::StructType::create(getLLVMContext(), StringRef(typeName));
}

/// createNominalType - Create a new nominal LLVM type for the given
/// protocol composition type.  Protocol composition types are
/// structural in the swift type system, but LLVM's type system
/// doesn't really care about this distinction, and it's nice to
/// distinguish different cases.
llvm::StructType *
IRGenModule::createNominalType(ProtocolCompositionType *type) {
  IRGenMangler Mangler;
  std::string typeName = Mangler.mangleProtocolForLLVMTypeName(type);
  return llvm::StructType::create(getLLVMContext(), StringRef(typeName));
}

SpareBitVector IRGenModule::getSpareBitsForType(llvm::Type *scalarTy, Size size) {
  auto it = SpareBitsForTypes.find(scalarTy);
  if (it != SpareBitsForTypes.end())
    return it->second;

  assert(!isa<llvm::StructType>(scalarTy));

  unsigned allocBits = size.getValueInBits();
  assert(allocBits >= DataLayout.getTypeAllocSizeInBits(scalarTy) &&
         "using a size that's smaller than LLVM's alloc size?");

  // Allocate a new cache entry.
  SpareBitVector &result = SpareBitsForTypes[scalarTy];

  // FIXME: Currently we only implement spare bits for primitive integer
  // types.
  if (auto *intTy = dyn_cast<llvm::IntegerType>(scalarTy)) {
    // Pad integers with spare bits up to their allocation size.
    auto v = llvm::APInt::getBitsSetFrom(allocBits, intTy->getBitWidth());
    // FIXME: byte swap v on big-endian platforms.
    result = SpareBitVector::fromAPInt(v);
    return result;
  }

  // No spare bits.
  result = SpareBitVector::getConstant(allocBits, false);
  return result;
}

unsigned IRGenModule::getBuiltinIntegerWidth(BuiltinIntegerType *t) {
  return getBuiltinIntegerWidth(t->getWidth());
}

unsigned IRGenModule::getBuiltinIntegerWidth(BuiltinIntegerWidth w) {
  if (w.isFixedWidth())
    return w.getFixedWidth();
  if (w.isPointerWidth())
    return getPointerSize().getValueInBits();
  llvm_unreachable("impossible width value");
}

void IRGenFunction::setDynamicSelfMetadata(CanType selfClass,
                                           bool isExactSelfClass,
                                           llvm::Value *value,
                                           IRGenFunction::DynamicSelfKind kind) {
  assert(!SelfValue && "already have local self metadata");
  SelfValue = value;
  assert(selfClass->getClassOrBoundGenericClass()
         && "self type not a class?");
  SelfTypeIsExact = isExactSelfClass;
  SelfType = selfClass;
  SelfKind = kind;
}

#ifndef NDEBUG
bool TypeConverter::isExemplarArchetype(ArchetypeType *arch) const {
  auto primary = arch->getRoot();
  if (!isa<PrimaryArchetypeType>(primary) &&
      !isa<PackArchetypeType>(primary))
    return true;
  auto genericEnv = primary->getGenericEnvironment();

  // Dig out the canonical generic environment.
  auto genericSig = genericEnv->getGenericSignature();
  auto canGenericSig = genericSig.getCanonicalSignature();
  auto canGenericEnv = canGenericSig.getGenericEnvironment();

  // If this archetype is in the canonical generic environment, it's an
  // exemplar archetype.
  return canGenericEnv == genericEnv;
}
#endif

SILType irgen::getSingletonAggregateFieldType(IRGenModule &IGM, SILType t,
                                              ResilienceExpansion expansion) {
  if (auto tuple = t.getAs<TupleType>())
    if (tuple->getNumElements() == 1)
      return t.getTupleElementType(0);

  if (auto structDecl = t.getStructOrBoundGenericStruct()) {
    // If the struct has to be accessed resiliently from this resilience domain,
    // we can't assume anything about its layout.
    if (IGM.isResilient(structDecl, expansion))
      return SILType();

    // C ABI wackiness may cause a single-field struct to have different layout
    // from its field.
    if (structDecl->hasUnreferenceableStorage()
        || structDecl->hasClangNode())
      return SILType();

    // A single-field struct with custom alignment has different layout from its
    // field.
    if (structDecl->getAttrs().hasAttribute<AlignmentAttr>())
      return SILType();

    // If there's only one stored property, we have the layout of its field.
    auto allFields = structDecl->getStoredProperties();
    
    if (allFields.size() == 1) {
      auto fieldTy = t.getFieldType(
          allFields[0], IGM.getSILModule(),
          TypeExpansionContext(expansion, IGM.getSwiftModule(),
                               IGM.getSILModule().isWholeModule()));
      if (!IGM.isTypeABIAccessible(fieldTy))
        return SILType();
      return fieldTy;
    }

    return SILType();
  }

  if (auto enumDecl = t.getEnumOrBoundGenericEnum()) {
    // If the enum has to be accessed resiliently from this resilience domain,
    // we can't assume anything about its layout.
    if (IGM.isResilient(enumDecl, expansion))
      return SILType();

    auto allCases = enumDecl->getAllElements();
    
    auto theCase = allCases.begin();
    if (!allCases.empty() && std::next(theCase) == allCases.end()
        && (*theCase)->hasAssociatedValues()) {
      auto enumEltTy = t.getEnumElementType(
          *theCase, IGM.getSILModule(),
          TypeExpansionContext(expansion, IGM.getSwiftModule(),
                               IGM.getSILModule().isWholeModule()));
      if (!IGM.isTypeABIAccessible(enumEltTy))
        return SILType();
      return enumEltTy;
    }

    return SILType();
  }

  return SILType();
}

void TypeInfo::verify(IRGenTypeVerifierFunction &IGF,
                      llvm::Value *typeMetadata,
                      SILType T) const {
  // By default, no type-specific verifier behavior.
}

static bool tryEmitDeinitCall(IRGenFunction &IGF,
                          SILType T,
                          llvm::function_ref<void (Explosion &)> direct,
                          llvm::function_ref<Address ()> indirect,
                          llvm::function_ref<void ()> indirectCleanup) {
  auto ty = T.getASTType();
  auto nominal = ty->getAnyNominal();

  // We are only concerned with move-only type deinits here.
  if (!nominal || !nominal->getValueTypeDestructor()) {
    return false;
  }

  auto deinitTable = IGF.getSILModule().lookUpMoveOnlyDeinit(
      nominal, false /*deserialize lazily*/);

  // If we do not have a deinit table already deserialized, call the value
  // witness instead.
  if (!deinitTable) {
    irgen::emitDestroyCall(IGF, T, indirect());
    indirectCleanup();
    return true;
  }

  // The deinit should take a single value parameter of the nominal type, either
  // by @owned or indirect @in convention.
  auto deinitFn = IGF.IGM.getAddrOfSILFunction(deinitTable->getImplementation(),
                                               NotForDefinition);
  auto deinitTy = deinitTable->getImplementation()->getLoweredFunctionType();
  auto deinitFP = FunctionPointer::forDirect(IGF.IGM, deinitFn,
                                             nullptr, deinitTy);
  assert(deinitTy->getNumParameters() == 1
         && deinitTy->getNumResults() == 0
         && !deinitTy->hasError()
         && "deinit should have only one parameter");

  auto substitutions = ty->getContextSubstitutionMap(IGF.getSwiftModule(),
                                                     nominal);
                                                     
  CalleeInfo info(deinitTy,
                  deinitTy->substGenericArgs(IGF.getSILModule(),
                                     substitutions,
                                     IGF.IGM.getMaximalTypeExpansionContext()),
                  substitutions);
                  
  bool isIndirect;
  Address indirectArg;
  Explosion directArg;
  switch (deinitTy->getParameters()[0].getConvention()) {
  case ParameterConvention::Direct_Owned:
    isIndirect = false;
    direct(directArg);
    break;
  case ParameterConvention::Indirect_In:
    isIndirect = true;
    indirectArg = indirect();
    break;
  default:
    llvm_unreachable("move-only deinit should only have consuming parameter convention");
  }
                  
  // If the deinit's convention has a special `self` parameter, then the
  // (pointer to) the value being destroyed is that parameter.
  llvm::Value *self = nullptr;
  if (hasSelfContextParameter(deinitTy)) {
    self = isIndirect ? indirectArg.getAddress() : directArg.claimNext();
    assert(directArg.empty()
           && "direct param (if any) should be a single pointer if "
              "it's the swiftself param");
  }
   
  GenericContextScope scope(IGF.IGM,
                        nominal->getGenericSignature().getCanonicalSignature());

  Callee deinitCallee(std::move(info), deinitFP, self);
  auto callEmission = getCallEmission(IGF, self, std::move(deinitCallee));
  callEmission->begin();
  // Pass the parameter if it wasn't already the by-convention self parameter.
  if (!self) {
    if (isIndirect) {
      directArg.add(indirectArg.getAddress());
    }
  }
  if (hasPolymorphicParameters(deinitTy)) {
    emitPolymorphicArguments(IGF, deinitTy, substitutions, nullptr,
                             directArg);
  }
  callEmission->setArgs(directArg, /*outlined*/ false, /*witness*/nullptr);
  Explosion nothing;
  callEmission->emitToExplosion(nothing, /*isOutlined*/ false);
  callEmission->end();
  if (isIndirect) {
    indirectCleanup();
  }
  return true;
}

bool irgen::tryEmitConsumeUsingDeinit(IRGenFunction &IGF, Explosion &explosion,
                                      SILType T) {
  const LoadableTypeInfo *ti = cast<LoadableTypeInfo>(&IGF.getTypeInfo(T));
  StackAddress temporary;
  return tryEmitDeinitCall(IGF, T,
    // Direct parameter case
    [&](Explosion &arg) {
      ti->reexplode(explosion, arg);
    },
    // Indirect parameter setup
    [&]() -> Address {
      // Allocate stack space to store the indirect argument, and forward our
      // value into it. The deinit will consume the value in memory.
      temporary = ti->allocateStack(IGF, T, "deinit.arg");
      ti->initialize(IGF, explosion, temporary.getAddress(), /*outlined*/false);
      return temporary.getAddress();
    },
    // Indirect parameter teardown
    [&]{
      // End the lifetime of the stack allocation.
      ti->deallocateStack(IGF, temporary, T);
    });
}

bool irgen::tryEmitDestroyUsingDeinit(IRGenFunction &IGF, Address address,
                                      SILType T) {
  return tryEmitDeinitCall(IGF, T,
    // Direct parameter case
    [&](Explosion &arg) {
      // Load the value from the address.
      auto *ti = cast<LoadableTypeInfo>(&IGF.getTypeInfo(T));
      ti->loadAsTake(IGF, address, arg);
    },
    // Indirect parameter setup
    [&]() -> Address {
      return address;
    },
    // Indirect parameter teardown
    [&]{ /* nothing to do */ });
}
