//===--- ExtraInhabitants.cpp - Routines for extra inhabitants ------------===//
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
//  This file implements routines for working with extra inhabitants.
//
//===----------------------------------------------------------------------===//

#include "ExtraInhabitants.h"

#include "BitPatternBuilder.h"
#include "IRGenModule.h"
#include "IRGenFunction.h"
#include "SwiftTargetInfo.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/Basic/Assertions.h"

using namespace swift;
using namespace irgen;

static uint8_t getNumLowObjCReservedBits(const IRGenModule &IGM) {
  if (!IGM.ObjCInterop)
    return 0;

  // Get the index of the first non-reserved bit.
  auto &mask = IGM.TargetInfo.ObjCPointerReservedBits;
  return mask.asAPInt().countTrailingOnes();
}

PointerInfo PointerInfo::forHeapObject(const IRGenModule &IGM) {
  return { Alignment(1), getNumLowObjCReservedBits(IGM), IsNotNullable };
}

PointerInfo PointerInfo::forFunction(const IRGenModule &IGM) {
  return { Alignment(1), 0, IsNotNullable };
}

/*****************************************************************************/

/// Return the number of extra inhabitants for a pointer that reserves
/// the given number of low bits.
unsigned PointerInfo::getExtraInhabitantCount(const IRGenModule &IGM) const {
  // FIXME: We could also make extra inhabitants using spare bits, but we
  // probably don't need to.
  uint64_t rawCount =
    IGM.TargetInfo.LeastValidPointerValue >> NumReservedLowBits;

  if (Nullable) rawCount--;
  
  // The runtime limits the count.
  return std::min(uint64_t(ValueWitnessFlags::MaxNumExtraInhabitants),
                  rawCount);
}

unsigned irgen::getHeapObjectExtraInhabitantCount(const IRGenModule &IGM) {
  // This must be consistent with the extra inhabitant count produced
  // by the runtime's getHeapObjectExtraInhabitantCount function in
  // KnownMetadata.cpp.
  return PointerInfo::forHeapObject(IGM).getExtraInhabitantCount(IGM);
}

/*****************************************************************************/

APInt PointerInfo::getFixedExtraInhabitantValue(const IRGenModule &IGM,
                                                unsigned bits,
                                                unsigned index,
                                                unsigned offset) const {
  unsigned pointerSizeInBits = IGM.getPointerSize().getValueInBits();
  assert(index < getExtraInhabitantCount(IGM) &&
         "pointer extra inhabitant out of bounds");
  assert(bits >= pointerSizeInBits + offset);

  if (Nullable) index++;

  uint64_t value = (uint64_t)index << NumReservedLowBits;

  auto valueBits = BitPatternBuilder(IGM.Triple.isLittleEndian());
  valueBits.appendClearBits(offset);
  valueBits.append(APInt(pointerSizeInBits, value));
  valueBits.padWithClearBitsTo(bits);
  return valueBits.build().value();
}

APInt irgen::getHeapObjectFixedExtraInhabitantValue(const IRGenModule &IGM,
                                                    unsigned bits,
                                                    unsigned index,
                                                    unsigned offset) {
  // This must be consistent with the extra inhabitant calculation implemented
  // in the runtime's storeHeapObjectExtraInhabitant and
  // getHeapObjectExtraInhabitantIndex functions in KnownMetadata.cpp.
  return PointerInfo::forHeapObject(IGM)
           .getFixedExtraInhabitantValue(IGM, bits, index, offset);
}

/*****************************************************************************/

llvm::Value *PointerInfo::getExtraInhabitantIndex(IRGenFunction &IGF,
                                                  Address src) const {
  llvm::BasicBlock *contBB = IGF.createBasicBlock("is-valid-pointer");
  SmallVector<std::pair<llvm::BasicBlock*, llvm::Value*>, 3> phiValues;
  auto invalidIndex = llvm::ConstantInt::getSigned(IGF.IGM.Int32Ty, -1);

  src = IGF.Builder.CreateElementBitCast(src, IGF.IGM.SizeTy);

  // Check if the inhabitant is below the least valid pointer value.
  llvm::Value *val = IGF.Builder.CreateLoad(src);
  {
    llvm::Value *leastValid = llvm::ConstantInt::get(IGF.IGM.SizeTy,
                                     IGF.IGM.TargetInfo.LeastValidPointerValue);
    llvm::Value *isValid = IGF.Builder.CreateICmpUGE(val, leastValid);

    phiValues.push_back({IGF.Builder.GetInsertBlock(), invalidIndex});
    llvm::BasicBlock *invalidBB = IGF.createBasicBlock("is-invalid-pointer");
    IGF.Builder.CreateCondBr(isValid, contBB, invalidBB);
    IGF.Builder.emitBlock(invalidBB);
  }

  // If null is not an extra inhabitant, check if the inhabitant is null.
  if (Nullable) {
    auto null = llvm::ConstantInt::get(IGF.IGM.SizeTy, 0);
    auto isNonNull = IGF.Builder.CreateICmpNE(val, null);
    phiValues.push_back({IGF.Builder.GetInsertBlock(), invalidIndex});
    llvm::BasicBlock *nonnullBB = IGF.createBasicBlock("is-nonnull-pointer");
    IGF.Builder.CreateCondBr(isNonNull, nonnullBB, contBB);
    IGF.Builder.emitBlock(nonnullBB);
  }

  // Check if the inhabitant has any reserved low bits set.
  // FIXME: This check is unneeded if the type is known to be pure Swift.
  if (NumReservedLowBits) {
    auto objcMask =
      llvm::ConstantInt::get(IGF.IGM.SizeTy, (1 << NumReservedLowBits) - 1);
    llvm::Value *masked = IGF.Builder.CreateAnd(val, objcMask);
    llvm::Value *maskedZero = IGF.Builder.CreateICmpEQ(masked,
                                     llvm::ConstantInt::get(IGF.IGM.SizeTy, 0));

    phiValues.push_back({IGF.Builder.GetInsertBlock(), invalidIndex});
    llvm::BasicBlock *untaggedBB = IGF.createBasicBlock("is-untagged-pointer");
    IGF.Builder.CreateCondBr(maskedZero, untaggedBB, contBB);
    IGF.Builder.emitBlock(untaggedBB);
  }

  // The inhabitant is an invalid pointer. Derive its extra inhabitant index.
  {
    llvm::Value *index = val;

    // Shift away the reserved bits.
    if (NumReservedLowBits) {
      index = IGF.Builder.CreateLShr(index,
                                  IGF.IGM.getSize(Size(NumReservedLowBits)));
    }

    // Subtract one if we have a nullable type.
    if (Nullable) {
      index = IGF.Builder.CreateSub(index, IGF.IGM.getSize(Size(1)));
    }

    // Truncate down to i32 if necessary.
    if (index->getType() != IGF.IGM.Int32Ty) {
      index = IGF.Builder.CreateTrunc(index, IGF.IGM.Int32Ty);
    }

    phiValues.push_back({IGF.Builder.GetInsertBlock(), index});
    IGF.Builder.CreateBr(contBB);
    IGF.Builder.emitBlock(contBB);
  }

  // Build the result phi.
  auto phi = IGF.Builder.CreatePHI(IGF.IGM.Int32Ty, phiValues.size());
  for (auto &entry : phiValues) {
    phi->addIncoming(entry.second, entry.first);
  }  
  return phi;
}

llvm::Value *irgen::getHeapObjectExtraInhabitantIndex(IRGenFunction &IGF,
                                                      Address src) {
  // This must be consistent with the extra inhabitant calculation implemented
  // in the runtime's getHeapObjectExtraInhabitantIndex function in
  // KnownMetadata.cpp.
  return PointerInfo::forHeapObject(IGF.IGM).getExtraInhabitantIndex(IGF, src);
}

/*****************************************************************************/

void PointerInfo::storeExtraInhabitant(IRGenFunction &IGF,
                                       llvm::Value *index,
                                       Address dest) const {
  if (index->getType() != IGF.IGM.SizeTy) {
    index = IGF.Builder.CreateZExt(index, IGF.IGM.SizeTy);
  }

  if (Nullable) {
    index = IGF.Builder.CreateAdd(index, IGF.IGM.getSize(Size(1)));
  }

  if (NumReservedLowBits) {
    index = IGF.Builder.CreateShl(index,
                  llvm::ConstantInt::get(IGF.IGM.SizeTy, NumReservedLowBits));
  }

  dest = IGF.Builder.CreateElementBitCast(dest, IGF.IGM.SizeTy);
  IGF.Builder.CreateStore(index, dest);
}

void irgen::storeHeapObjectExtraInhabitant(IRGenFunction &IGF,
                                           llvm::Value *index,
                                           Address dest) {
  // This must be consistent with the extra inhabitant calculation implemented
  // in the runtime's storeHeapObjectExtraInhabitant function in
  // KnownMetadata.cpp.
  PointerInfo::forHeapObject(IGF.IGM)
    .storeExtraInhabitant(IGF, index, dest);
}
