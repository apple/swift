//===--- GenIntegerLiteral.cpp - IRGen for Builtin.IntegerLiteral ---------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements IR generation for Builtin.IntegerLiteral.
//
//===----------------------------------------------------------------------===//

#include "GenIntegerLiteral.h"

#include "swift/ABI/MetadataValues.h"
#include "swift/Basic/Assertions.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"

#include "BitPatternBuilder.h"
#include "Explosion.h"
#include "ExtraInhabitants.h"
#include "GenType.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "LoadableTypeInfo.h"
#include "ScalarPairTypeInfo.h"

using namespace swift;
using namespace irgen;

namespace {

/// A TypeInfo implementation for Builtin.IntegerLiteral.
class IntegerLiteralTypeInfo :
  public TrivialScalarPairTypeInfo<IntegerLiteralTypeInfo, LoadableTypeInfo> {

public:
  IntegerLiteralTypeInfo(llvm::StructType *storageType,
                         Size size, Alignment align, SpareBitVector &&spareBits)
      : TrivialScalarPairTypeInfo(storageType, size, std::move(spareBits), align,
                            IsTriviallyDestroyable, IsCopyable, IsFixedSize) {}

  static Size getFirstElementSize(IRGenModule &IGM) {
    return IGM.getPointerSize();
  }
  static StringRef getFirstElementLabel() {
    return ".data";
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

  static Size getSecondElementOffset(IRGenModule &IGM) {
    return IGM.getPointerSize();
  }
  static Size getSecondElementSize(IRGenModule &IGM) {
    return IGM.getPointerSize();
  }
  static StringRef getSecondElementLabel() {
    return ".flags";
  }

  // The data pointer isn't a heap object, but it is an aligned pointer.
  bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
    return true;
  }
  unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
    return getHeapObjectExtraInhabitantCount(IGM);
  }
  APInt getFixedExtraInhabitantValue(IRGenModule &IGM,
                                     unsigned bits,
                                     unsigned index) const override {
    return getHeapObjectFixedExtraInhabitantValue(IGM, bits, index, 0);
  }
  llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF, Address src,
                                       SILType T,
                                       bool isOutlined) const override {
    src = projectFirstElement(IGF, src);
    return getHeapObjectExtraInhabitantIndex(IGF, src);
  }
  APInt getFixedExtraInhabitantMask(IRGenModule &IGM) const override {
    auto pointerSize = IGM.getPointerSize();
    auto mask = BitPatternBuilder(IGM.Triple.isLittleEndian());
    mask.appendSetBits(pointerSize.getValueInBits());
    mask.appendClearBits(pointerSize.getValueInBits());
    return mask.build().value();
  }
  void storeExtraInhabitant(IRGenFunction &IGF, llvm::Value *index,
                            Address dest, SILType T,
                            bool isOutlined) const override {
    dest = projectFirstElement(IGF, dest);
    storeHeapObjectExtraInhabitant(IGF, index, dest);
  }
};

}

llvm::StructType *IRGenModule::getIntegerLiteralTy() {
  if (!IntegerLiteralTy) {
    IntegerLiteralTy =
      llvm::StructType::create(getLLVMContext(), {
                                 SizeTy->getPointerTo(),
                                 SizeTy
                               }, "swift.int_literal");
  }
  return IntegerLiteralTy;
}

const LoadableTypeInfo &
TypeConverter::getIntegerLiteralTypeInfo() {
  if (!IntegerLiteralTI) {
    auto ty = IGM.getIntegerLiteralTy();

    SpareBitVector spareBits;
    spareBits.append(IGM.getHeapObjectSpareBits());
    spareBits.appendClearBits(IGM.getPointerSize().getValueInBits());

    IntegerLiteralTI =
      new IntegerLiteralTypeInfo(ty, IGM.getPointerSize() * 2,
                                 IGM.getPointerAlignment(),
                                 std::move(spareBits));
  }
  return *IntegerLiteralTI;
}

ConstantIntegerLiteral
irgen::emitConstantIntegerLiteral(IRGenModule &IGM, IntegerLiteralInst *ILI) {
  return IGM.getConstantIntegerLiteral(ILI->getValue());
}

ConstantIntegerLiteral
IRGenModule::getConstantIntegerLiteral(APInt value) {
  if (!ConstantIntegerLiterals)
    ConstantIntegerLiterals.reset(new ConstantIntegerLiteralMap());

  return ConstantIntegerLiterals->get(*this, std::move(value));
}

ConstantIntegerLiteral
ConstantIntegerLiteralMap::get(IRGenModule &IGM, APInt &&value) {
  auto &entry = map[value];
  if (entry.Data) return entry;

  assert(value.getSignificantBits() == value.getBitWidth() &&
         "expected IntegerLiteral value to be maximally compact");

  // We're going to break the value down into pointer-sized chunks.
  uint64_t chunkSizeInBits = IGM.getPointerSize().getValueInBits();

  // Count how many bits are needed to store the value, including the sign bit.
  uint64_t minWidthInBits = value.getBitWidth();

  // Round up to the nearest multiple of the chunk size.
  uint64_t storageWidthInBits = (minWidthInBits + chunkSizeInBits - 1)
                                & ~(chunkSizeInBits - 1);

  // Extend the value to that width.  We guarantee that extra bits in the
  // chunks will be appropriately sign-extended.
  value = value.sextOrTrunc(storageWidthInBits);

  // Extract the individual chunks from the extended value.
  uint64_t numChunks = storageWidthInBits / chunkSizeInBits;
  SmallVector<llvm::Constant *, 4> chunks;
  chunks.reserve(numChunks);
  for (uint64_t i = 0; i != numChunks; ++i) {
    auto chunk = value.extractBits(chunkSizeInBits, i * chunkSizeInBits);
    chunks.push_back(llvm::ConstantInt::get(IGM.SizeTy, std::move(chunk)));
  }

  // Build a global to hold the chunks.
  // TODO: make this shared within the image
  auto arrayTy = llvm::ArrayType::get(IGM.SizeTy, numChunks);
  auto initV = llvm::ConstantArray::get(arrayTy, chunks);
  auto globalArray = new llvm::GlobalVariable(
      *IGM.getModule(), arrayTy, /*constant*/ true,
      llvm::GlobalVariable::PrivateLinkage, initV,
      IGM.EnableValueNames
          ? Twine("intliteral.") + llvm::toString(value, 10, true)
          : "");
  globalArray->setUnnamedAddr(llvm::GlobalVariable::UnnamedAddr::Global);

  // Various clients expect this to be a i64*, not an [N x i64]*, so cast down.
  auto zero = llvm::ConstantInt::get(IGM.Int32Ty, 0);
  llvm::Constant *indices[] = { zero, zero };
  auto data = llvm::ConstantExpr::getInBoundsGetElementPtr(arrayTy, globalArray,
                                                           indices);

  // Build the flags word.
  auto flags = IntegerLiteralFlags(minWidthInBits, value.isNegative());
  auto flagsV = llvm::ConstantInt::get(IGM.SizeTy, flags.getOpaqueValue());

  // Cache the global.
  entry.Data = data;
  entry.Flags = flagsV;
  return entry;
}

void irgen::emitIntegerLiteralCheckedTrunc(IRGenFunction &IGF, Explosion &in,
                                           llvm::Type *FromTy,
                                           llvm::IntegerType *resultTy,
                                           bool resultIsSigned,
                                           Explosion &out) {
  Address data(in.claimNext(), FromTy, IGF.IGM.getPointerAlignment());
  auto flags = in.claimNext();

  size_t chunkWidth = IGF.IGM.getPointerSize().getValueInBits();
  size_t resultWidth = resultTy->getBitWidth();

  // The number of bits required to express the value, including the sign bit.
  auto valueWidth = IGF.Builder.CreateLShr(flags,
                    IGF.IGM.getSize(Size(IntegerLiteralFlags::BitWidthShift)));

  // The maximum number of chunks that we need to read in order to fill the
  // result type: ceil(resultWidth / chunkWidth).
  // Note that we won't actually end up reading the final chunk if we're
  // building an unsigned value that requires e.g. 65 bits to express:
  // there's only one meaningful bit there, and we know it's zero from the
  // isNegative check.
  size_t maxNumChunks = (resultWidth + chunkWidth - 1) / chunkWidth;

  // One branch from invalidBB, one branch at each intermediate point in the
  // do-we-have-more-chunks chain, and one branch at the end.
  auto numPHIEntries = maxNumChunks + /*overflow*/ 1;

  auto boolTy = IGF.IGM.Int1Ty;
  auto doneBB = IGF.createBasicBlock("intliteral.trunc.done");
  auto resultPHI = llvm::PHINode::Create(resultTy, numPHIEntries, "", doneBB);
  auto overflowPHI = llvm::PHINode::Create(boolTy, numPHIEntries, "", doneBB);
  out.add(resultPHI);
  out.add(overflowPHI);

  auto validBB = IGF.createBasicBlock("intliteral.trunc.valid");
  auto invalidBB = IGF.createBasicBlock("intliteral.trunc.invalid");

  // Check whether the value fits in the result type.
  // If the result is signed, then we need valueWidth <= resultWidth.
  // Otherwise we need valueWidth <= resultWidth + 1 && !isNegative.
  {
    llvm::Value *hasOverflow;
    if (resultIsSigned) {
      hasOverflow = IGF.Builder.CreateICmpUGT(valueWidth,
                                        IGF.IGM.getSize(Size(resultWidth)));
    } else {
      static_assert(IntegerLiteralFlags::IsNegativeFlag == 1,
                    "hardcoded in this truncation");
      auto isNegative = IGF.Builder.CreateTrunc(flags, boolTy);
      auto tooBig = IGF.Builder.CreateICmpUGT(valueWidth,
                                        IGF.IGM.getSize(Size(resultWidth + 1)));
      hasOverflow = IGF.Builder.CreateOr(isNegative, tooBig);
    }
    IGF.Builder.CreateCondBr(hasOverflow, invalidBB, validBB);
  }

  // In the invalid block, we just need to construct the result.  This block
  // only exists to split the otherwise-critical edge.
  IGF.Builder.emitBlock(invalidBB);
  {
    resultPHI->addIncoming(llvm::ConstantInt::get(resultTy, 0), invalidBB);
    overflowPHI->addIncoming(llvm::ConstantInt::get(boolTy, 1), invalidBB);
    IGF.Builder.CreateBr(doneBB);
  }

  // Okay, the value fits in the result type, so overflow is off the table
  // and we just need to assemble a value of resultTy.  But we might not have
  // the full complement of chunks.
  IGF.Builder.emitBlock(validBB);
  {
    auto firstChunk = IGF.Builder.CreateLoad(data);

    // The easy case is if resultWidth <= chunkWidth, in which case knowing
    // that we haven't overflowed is sufficient to say that we can just
    // use the first chunk.
    if (resultWidth <= chunkWidth) {
      auto result = IGF.Builder.CreateTrunc(firstChunk, resultTy);
      resultPHI->addIncoming(result, validBB);
      overflowPHI->addIncoming(llvm::ConstantInt::get(boolTy, 0), validBB);
      IGF.Builder.CreateBr(doneBB);

    // Otherwise, we're going to have to test dynamically how many chunks
    // we need to read.
    } else {
      assert(maxNumChunks >= 2);
      llvm::Value *cur = firstChunk;
      for (size_t i = 1; i != maxNumChunks; ++i) {
        auto extendBB = IGF.createBasicBlock("intliteral.trunc.finish");
        auto nextBB = IGF.createBasicBlock("intliteral.trunc.next");

        // If the result is signed, then we're done if:
        //   valueWidth <= bitsInChunksReadSoFar
        // If the result is unsigned, then we're done if:
        //   valueWidth <= bitsInChunksReadSoFar + 1
        // (because we know the next bit will be zero)
        auto limit = i * chunkWidth + size_t(!resultIsSigned);
        auto isComplete =
          IGF.Builder.CreateICmpULE(valueWidth, IGF.IGM.getSize(Size(limit)));
        IGF.Builder.CreateCondBr(isComplete, extendBB, nextBB);

        // If we're done, extend the current value to the result type and
        // then branch out.
        IGF.Builder.emitBlock(extendBB);
        {
          auto extendedResult =
            resultIsSigned ? IGF.Builder.CreateSExt(cur, resultTy)
                           : IGF.Builder.CreateZExt(cur, resultTy);
          resultPHI->addIncoming(extendedResult, extendBB);
          overflowPHI->addIncoming(llvm::ConstantInt::get(boolTy, 0), extendBB);
          IGF.Builder.CreateBr(doneBB);
        }

        // Otherwise, load the next chunk.
        IGF.Builder.emitBlock(nextBB);
        auto nextChunkAddr =
          IGF.Builder.CreateConstArrayGEP(data, i, IGF.IGM.getPointerSize());
        auto nextChunk = IGF.Builder.CreateLoad(nextChunkAddr);

        // Zero-extend the current value and the chunk and then shift the
        // chunk into place.  If this is the last iteration, we should use
        // the final result type; the shift might then drop bits, but they
        // should just be sign-extension bits.
        auto nextTy = (i + 1 == maxNumChunks
                         ? resultTy
                         : llvm::IntegerType::get(IGF.IGM.getLLVMContext(),
                                                  (i + 1) * chunkWidth));
        cur = IGF.Builder.CreateZExt(cur, nextTy);
        auto shiftedNextChunk =
          IGF.Builder.CreateShl(IGF.Builder.CreateZExt(nextChunk, nextTy),
                                i * chunkWidth);
        cur = IGF.Builder.CreateAdd(cur, shiftedNextChunk);
      }

      // Given the overflow check before, we know we don't need to look at
      // any more chunks.
      assert(cur->getType() == resultTy);
      auto curBB = IGF.Builder.GetInsertBlock();
      resultPHI->addIncoming(cur, curBB);
      overflowPHI->addIncoming(llvm::ConstantInt::get(boolTy, 0), curBB);
      IGF.Builder.CreateBr(doneBB);
    }
  }

  // Emit the continuation block.  We've already set up the PHIs here and
  // add them to `out`, so there's nothing else to do.
  IGF.Builder.emitBlock(doneBB);
}

static llvm::Value *emitIntegerLiteralToFloatCall(IRGenFunction &IGF,
                                                  llvm::Value *data,
                                                  llvm::Value *flags,
                                                  unsigned bitWidth) {
  assert(bitWidth == 32 || bitWidth == 64);
  auto fn = bitWidth == 32 ? IGF.IGM.getIntToFloat32FunctionPointer()
                           : IGF.IGM.getIntToFloat64FunctionPointer();
  auto call = IGF.Builder.CreateCall(fn, {data, flags});
  call->setCallingConv(IGF.IGM.SwiftCC);
  call->setDoesNotThrow();
  call->setOnlyReadsMemory();
  call->setOnlyAccessesArgMemory();

  return call;
}

llvm::Value *irgen::emitIntegerLiteralToFP(IRGenFunction &IGF,
                                           Explosion &in,
                                           llvm::Type *toType) {
  auto data = in.claimNext();
  auto flags = in.claimNext();

  assert(toType->isFloatingPointTy());
  switch (toType->getTypeID()) {
  case llvm::Type::HalfTyID: {
    auto flt = emitIntegerLiteralToFloatCall(IGF, data, flags, 32);
    return IGF.Builder.CreateFPTrunc(flt, toType);
  }

  case llvm::Type::FloatTyID:
    return emitIntegerLiteralToFloatCall(IGF, data, flags, 32);

  case llvm::Type::DoubleTyID:
    return emitIntegerLiteralToFloatCall(IGF, data, flags, 64);

  // TODO: add runtime functions for some of these?
  case llvm::Type::X86_FP80TyID:
  case llvm::Type::FP128TyID:
  case llvm::Type::PPC_FP128TyID: {
    auto dbl = emitIntegerLiteralToFloatCall(IGF, data, flags, 64);
    return IGF.Builder.CreateFPExt(dbl, toType);
  }

  default:
    llvm_unreachable("not a floating-point type");
  }
}

llvm::Value *irgen::emitIntLiteralBitWidth(
  IRGenFunction &IGF,
  Explosion &in
) {
  auto data = in.claimNext();
  auto flags = in.claimNext();
  (void)data; // [[maybe_unused]]
  return IGF.Builder.CreateLShr(
    flags,
    IGF.IGM.getSize(Size(IntegerLiteralFlags::BitWidthShift))
  );
}

llvm::Value *irgen::emitIntLiteralIsNegative(
  IRGenFunction &IGF,
  Explosion &in
) {
  auto data = in.claimNext();
  auto flags = in.claimNext();
  (void)data; // [[maybe_unused]]
  static_assert(
    IntegerLiteralFlags::IsNegativeFlag == 1,
    "hardcoded in this truncation"
  );
  return IGF.Builder.CreateTrunc(flags, IGF.IGM.Int1Ty);
}

llvm::Value *irgen::emitIntLiteralWordAtIndex(
  IRGenFunction &IGF,
  Explosion &in
) {
  auto data = in.claimNext();
  auto flags = in.claimNext();
  auto index = in.claimNext();
  (void)flags; // [[maybe_unused]]
  return IGF.Builder.CreateLoad(
    IGF.Builder.CreateInBoundsGEP(IGF.IGM.SizeTy, data, index),
    IGF.IGM.SizeTy,
    IGF.IGM.getPointerAlignment()
  );
}
