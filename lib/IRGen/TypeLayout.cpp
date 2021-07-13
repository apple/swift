//===--- TypeLayout.cpp - TypeLayout --------------------------------------===//
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
#include "TypeLayout.h"
#include "EnumPayload.h"
#include "FixedTypeInfo.h"
#include "GenExistential.h"
#include "GenOpaque.h"
#include "GenType.h"
#include "IRGen.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "SwitchBuilder.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/IR/DerivedTypes.h"

using namespace swift;
using namespace irgen;

static bool isNullableRefCounted(ScalarKind kind) {
  switch (kind) {
  case ScalarKind::ErrorReference:
  case ScalarKind::NativeStrongReference:
  case ScalarKind::NativeUnownedReference:
  case ScalarKind::NativeWeakReference:
  case ScalarKind::UnknownUnownedReference:
  case ScalarKind::UnknownWeakReference:
  case ScalarKind::UnknownReference:
  case ScalarKind::BlockReference:
  case ScalarKind::BridgeReference:
  case ScalarKind::ObjCReference:
    return true;
  default:
    return false;
  }
}

TypeLayoutEntry::~TypeLayoutEntry() {}

void TypeLayoutEntry::computeProperties() {
  // does not add anything.
}

void TypeLayoutEntry::gatherProperties(TypeLayoutEntry *fromEntry) {
  hasArchetypeField |= fromEntry->hasArchetypeField;
  hasResilientField |= fromEntry->hasResilientField;
  hasDependentResilientField |= fromEntry->hasDependentResilientField;

  assert(!(!hasResilientField && hasDependentResilientField));
}

const EnumTypeLayoutEntry *TypeLayoutEntry::getAsEnum() const {
  if (getKind() == TypeLayoutEntryKind::Enum) {
    return static_cast<const EnumTypeLayoutEntry *>(this);
  }
  return nullptr;
}

llvm::Value *TypeLayoutEntry::alignmentMask(IRGenFunction &IGF) const {
  assert(isEmpty());
  return IGF.IGM.getSize(Size(0));
}

llvm::Value *TypeLayoutEntry::size(IRGenFunction &IGF) const {
  assert(isEmpty());
  return IGF.IGM.getSize(Size(0));
}

bool TypeLayoutEntry::isFixedSize(IRGenModule& IGM) const {
  assert(isEmpty() && "Type isn't empty -- perhaps you forgot to override this function?");
  return true;
}

llvm::Optional<Size> TypeLayoutEntry::fixedSize(IRGenModule& IGM) const {
  assert(isEmpty() && "Type isn't empty -- perhaps you forgot to override this function?");
  return Size(0);
}

llvm::Optional<Alignment> TypeLayoutEntry::fixedAlignment(IRGenModule& IGM) const {
  assert(isEmpty() && "Type isn't empty -- perhaps you forgot to override this function?");
  return Alignment();
}

llvm::Optional<uint32_t> TypeLayoutEntry::fixedXICount(IRGenModule& IGM) const {
  assert(isEmpty() && "Type isn't empty -- perhaps you forgot to override this function?");
  return 0;
}

llvm::Value *TypeLayoutEntry::isBitwiseTakable(IRGenFunction &IGF) const {
  assert(isEmpty());
  return llvm::ConstantInt::get(IGF.IGM.Int1Ty, true);
}

llvm::Value *TypeLayoutEntry::extraInhabitantCount(IRGenFunction &IGF) const {
  assert(isEmpty());
  return llvm::ConstantInt::get(IGF.IGM.Int32Ty, 0);
}

void TypeLayoutEntry::destroy(IRGenFunction &IGF, Address addr) const {
  assert(isEmpty());
  // Nothing to destroy.
}

void TypeLayoutEntry::assign(IRGenFunction &IGF, Address dest, Address src,
                             IsTake_t isTake) const {
  if (isTake == IsTake) {
    assignWithTake(IGF, dest, src);
  } else {
    assignWithCopy(IGF, dest, src);
  }
}

void TypeLayoutEntry::initialize(IRGenFunction &IGF, Address dest, Address src,
                                 IsTake_t isTake) const {
  if (isTake == IsTake) {
    initWithTake(IGF, dest, src);
  } else {
    initWithCopy(IGF, dest, src);
  }
}

void TypeLayoutEntry::assignWithCopy(IRGenFunction &IGF, Address dest,
                                     Address src) const {
  assert(isEmpty());
  // Nothing to copy.
}

void TypeLayoutEntry::assignWithTake(IRGenFunction &IGF, Address dest,
                                     Address src) const {
  assert(isEmpty());
  // Nothing to copy.
}

void TypeLayoutEntry::initWithCopy(IRGenFunction &IGF, Address dest,
                                     Address src) const {
  assert(isEmpty());
  // Nothing to copy.
}

void TypeLayoutEntry::initWithTake(IRGenFunction &IGF, Address dest,
                                   Address src) const {
  assert(isEmpty());
  // Nothing to copy.
}

bool TypeLayoutEntry::containsResilientField() const {
  return hasResilientField;
}

bool TypeLayoutEntry::containsArchetypeField() const {
  return hasArchetypeField;
}

bool TypeLayoutEntry::containsDependentResilientField() const {
  return hasDependentResilientField;
}

llvm::Value *TypeLayoutEntry::getEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *numEmptyCases, Address addr) const {
  assert(isEmpty());
  return getFixedTypeEnumTagSinglePayload(
      IGF, numEmptyCases, addr, IGF.IGM.getSize(Size(0)), Size(0), 0,
      [](Address addr) -> llvm::Value * {
        // This function should not be called since the
        // fixedExtraInhabitantCount is zero. We should just store to the extra
        // tag bytes.
        llvm_unreachable("this function should not be called");
        return nullptr;
      },
      true);
}

void TypeLayoutEntry::storeEnumTagSinglePayload(IRGenFunction &IGF,
                                                llvm::Value *tag,
                                                llvm::Value *numEmptyCases,
                                                Address addr) const {
  assert(isEmpty());
  storeFixedTypeEnumTagSinglePayload(
      IGF, tag, numEmptyCases, addr, IGF.IGM.getSize(Size(0)), Size(0), 0,
      [&](llvm::Value *, Address) {
        // This function should not be called since the
        // fixedExtraInhabitantCount is zero. We should just store to the extra
        // tag bytes.
        llvm_unreachable("this function should not be called");
      },
      true);
}

struct EnumTagInfo {
  llvm::Value *numTags;
  llvm::Value *numTagBytes;
};

static EnumTagInfo getEnumTagBytes(IRGenFunction &IGF, llvm::Value *size,
                                   llvm::Value *emptyCases,
                                   llvm::Value *payloadCases) {
  // Implements (compare getEnumTagCounts):
  // unsigned numTags = payloadCases;
  // if (emptyCases > 0) {
  //   if (size >= 4)
  //     numTags += 1;
  //   else {
  //     unsigned bits = size * 8U;
  //     unsigned casesPerTagBitValue = 1U << bits;
  //     numTags += ((emptyCases + (casesPerTagBitValue-1U)) >> bits);
  //   }
  // }
  // unsigned numTagBytes = (numTags <=    1 ? 0 :
  //                         numTags <   256 ? 1 :
  //                         numTags < 65536 ? 2 : 4);
  // return numTagBytes;

  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;
  auto &ctx = IGM.getLLVMContext();

  // unsigned numTags = payloadCases;
  auto numTags = payloadCases;
  auto block1 = Builder.GetInsertBlock();
  auto zero = IGM.getInt32(0);
  auto one = IGM.getInt32(1);

  auto someEmptyCasesBB = llvm::BasicBlock::Create(ctx);
  auto noEmptyCasesBB = llvm::BasicBlock::Create(ctx);
  auto someEmptyCases = Builder.CreateICmpUGT(emptyCases, zero);
  // if (emptyCases > 0) {
  Builder.CreateCondBr(someEmptyCases, someEmptyCasesBB, noEmptyCasesBB);

  Builder.emitBlock(someEmptyCasesBB);
  auto someEmptyCasesMergeBB = llvm::BasicBlock::Create(ctx);
  auto gte4BB = llvm::BasicBlock::Create(ctx);
  auto lt4BB = llvm::BasicBlock::Create(ctx);
  auto sizeGTE4 = Builder.CreateICmpUGE(size, IGM.getInt32(4));
  // if (size >= 4) {
  Builder.CreateCondBr(sizeGTE4, gte4BB, lt4BB);

  Builder.emitBlock(gte4BB);
  //   numTags += 1;
  auto numTagsPlusOne = Builder.CreateAdd(numTags, one);
  Builder.CreateBr(someEmptyCasesMergeBB);

  // } else {
  Builder.emitBlock(lt4BB);
  //   unsigned bits = size * 8U;
  //   unsigned casesPerTagBitValue = 1U << bits;
  //   numTags += ((emptyCases + (casesPerTagBitValue-1U)) >> bits);
  auto *bits = Builder.CreateMul(size, IGM.getInt32(8));
  auto *casesPerTagBitValue = Builder.CreateShl(one, bits);
  auto *numTags2 = Builder.CreateSub(casesPerTagBitValue, one);
  numTags2 = Builder.CreateAdd(numTags2, emptyCases);
  numTags2 = Builder.CreateLShr(numTags2, bits);
  numTags2 = Builder.CreateAdd(numTags2, numTags);
  Builder.CreateBr(someEmptyCasesMergeBB);

  Builder.emitBlock(someEmptyCasesMergeBB);
  auto numTagsSomeEmptyCases = Builder.CreatePHI(IGM.Int32Ty, 2);
  numTagsSomeEmptyCases->setName("num-tags-some-empty-cases");
  numTagsSomeEmptyCases->addIncoming(numTagsPlusOne, gte4BB);
  numTagsSomeEmptyCases->addIncoming(numTags2, lt4BB);
  Builder.CreateBr(noEmptyCasesBB);

  Builder.emitBlock(noEmptyCasesBB);
  auto numTagsPhi = Builder.CreatePHI(IGM.Int32Ty, 2);
  numTagsPhi->setName("num-tags-phi");
  numTagsPhi->addIncoming(numTags, block1);
  numTagsPhi->addIncoming(numTagsSomeEmptyCases, someEmptyCasesMergeBB);

  // unsigned numTagBytes = (numTags <=    1 ? 0 :
  //                         numTags <   256 ? 1 :
  //                         numTags < 65536 ? 2 : 4);
  auto numTagsLTE1 = Builder.CreateICmpULE(numTagsPhi, one);
  auto numTagsLT256 = Builder.CreateICmpULT(numTagsPhi, IGM.getInt32(256));
  auto numTagsLT65536 =
      Builder.CreateICmpULT(numTagsPhi, IGM.getInt32(65536));
  auto useTwoOrFourByte =
      Builder.CreateSelect(numTagsLT65536, IGM.getInt32(2), IGM.getInt32(4));
  auto useOneTwoOrFourByte =
      Builder.CreateSelect(numTagsLT256, one, useTwoOrFourByte);
  auto numTagBytes =
      Builder.CreateSelect(numTagsLTE1, zero, useOneTwoOrFourByte);
  numTagBytes->setName("num-tag-bytes");
  return {numTagsPhi, numTagBytes};
}

struct FixedEnumTagInfo {
  uint32_t numTags;
  uint32_t numTagBytes;
};

static FixedEnumTagInfo getFixedEnumTagBytes(Size size,
                                   uint32_t emptyCases,
                                   uint32_t payloadCases) {
  unsigned numTags = payloadCases;
  if (emptyCases > 0) {
    if (size.getValue() >= 4)
      numTags += 1;
    else {
      unsigned bits = size.getValue() * 8U;
      unsigned casesPerTagBitValue = 1U << bits;
      numTags += ((emptyCases + (casesPerTagBitValue-1U)) >> bits);
    }
  }
  unsigned numTagBytes = (numTags <=    1 ? 0 :
                          numTags <   256 ? 1 :
                          numTags < 65536 ? 2 : 4);
  return {numTags, numTagBytes};
}

llvm::Value *TypeLayoutEntry::getEnumTagSinglePayloadGeneric(
    IRGenFunction &IGF, Address addr, llvm::Value *numEmptyCases,
    llvm::function_ref<llvm::Value *(Address addr)> getExtraInhabitantIndexFun)
    const {
  auto &IGM = IGF.IGM;
  auto &Ctx = IGF.IGM.getLLVMContext();
  auto &Builder = IGF.Builder;

  auto numExtraInhabitants = this->extraInhabitantCount(IGF);
  auto size = this->size(IGF);

  auto *zero = llvm::ConstantInt::get(IGM.Int32Ty, 0U);
  auto *one = llvm::ConstantInt::get(IGM.Int32Ty, 1U);
  auto *four = llvm::ConstantInt::get(IGM.Int32Ty, 4U);
  auto *eight = llvm::ConstantInt::get(IGM.Int32Ty, 8U);

  auto *extraTagBitsBB = llvm::BasicBlock::Create(Ctx);
  auto *noExtraTagBitsBB = llvm::BasicBlock::Create(Ctx);
  auto *hasEmptyCasesBB = llvm::BasicBlock::Create(Ctx);
  auto *singleCaseEnumBB = llvm::BasicBlock::Create(Ctx);

  auto *truncSize = Builder.CreateZExtOrTrunc(size, IGM.Int32Ty);

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
      getEnumTagBytes(IGF, truncSize, emptyCases, IGM.getInt32(1)).numTagBytes;

  // Read the value stored in the extra tag bytes.
  auto *valueAddr =
      Builder.CreateBitOrPointerCast(addr.getAddress(), IGM.Int8PtrTy);
  auto *extraTagBitsAddr = Builder.CreateInBoundsGEP(valueAddr, size);
  auto *extraTagBits = emitGetTag(IGF, Address(extraTagBitsAddr, Alignment(1)),
                                  numExtraTagBytes);

  extraTagBitsBB = llvm::BasicBlock::Create(Ctx);
  Builder.CreateCondBr(Builder.CreateICmpEQ(extraTagBits, zero),
                       noExtraTagBitsBB, extraTagBitsBB);

  auto *resultBB = llvm::BasicBlock::Create(Ctx);

  Builder.emitBlock(extraTagBitsBB);

  auto sizeGTE4 = Builder.CreateICmpUGE(truncSize, four);
  auto *caseIndexFromExtraTagBits = Builder.CreateSelect(
      sizeGTE4, zero,
      Builder.CreateShl(Builder.CreateSub(extraTagBits, one),
                        Builder.CreateMul(eight, truncSize)));

  auto caseIndexFromValue = llvm::PHINode::Create(IGM.Int32Ty, 2);
  caseIndexFromValue->setName("case-index-from-value");
  auto contBB = IGF.createBasicBlock("");
  auto nonZeroSizeBB = IGF.createBasicBlock("");
  auto isNonZero = Builder.CreateICmpNE(truncSize, zero);
  caseIndexFromValue->addIncoming(zero, Builder.GetInsertBlock());
  Builder.CreateCondBr(isNonZero, nonZeroSizeBB, contBB);
  {
    // Read up to one pointer-sized 'chunk' of the payload.
    // The size of the chunk does not have to be a power of 2.
    Builder.emitBlock(nonZeroSizeBB);
    auto sizeClampedTo4 = Builder.CreateSelect(sizeGTE4, four, truncSize);
    auto loadPayloadChunk = emitLoad1to4Bytes(IGF, addr, sizeClampedTo4);
    caseIndexFromValue->addIncoming(loadPayloadChunk, Builder.GetInsertBlock());
    Builder.CreateBr(contBB);
  }
  Builder.emitBlock(contBB);
  Builder.Insert(caseIndexFromValue);

  auto *result1 = Builder.CreateAdd(
      numExtraInhabitants,
      Builder.CreateOr(caseIndexFromValue, caseIndexFromExtraTagBits));
  result1 = Builder.CreateAdd(result1, one);
  auto *result1BB = Builder.GetInsertBlock();
  Builder.CreateBr(resultBB);

  // Extra tag bits were considered and zero or there are not extra tag
  // bits.
  Builder.emitBlock(noExtraTagBitsBB);

  // If there are extra inhabitants, see whether the payload is valid.
  auto result0 = llvm::PHINode::Create(IGM.Int32Ty, 2);
  result0->setName("get-payload-tag-phi");
  auto contBB2 = IGF.createBasicBlock("");
  auto hasXIBB = IGF.createBasicBlock("");
  auto isNonZeroXI = Builder.CreateICmpNE(numExtraInhabitants, zero);
  result0->addIncoming(zero, Builder.GetInsertBlock());
  Builder.CreateCondBr(isNonZeroXI, hasXIBB, contBB2);
  {
    Builder.emitBlock(hasXIBB);
    ConditionalDominanceScope scope(IGF);
    // Get tag in payload.
    auto tagInPayload = getExtraInhabitantIndexFun(addr);
    result0->addIncoming(tagInPayload, Builder.GetInsertBlock());
    Builder.CreateBr(contBB2);
  }
  Builder.emitBlock(contBB2);
  Builder.Insert(result0);
  auto result0BB = Builder.GetInsertBlock();
  Builder.CreateBr(resultBB);

  Builder.emitBlock(singleCaseEnumBB);
  // Otherwise, we have a valid payload.
  auto *result2 = zero;
  Builder.CreateBr(resultBB);

  Builder.emitBlock(resultBB);
  auto *result = Builder.CreatePHI(IGM.Int32Ty, 3);
  result->addIncoming(result0, result0BB);
  result->addIncoming(result1, result1BB);
  result->addIncoming(result2, singleCaseEnumBB);
  return result;
}

void TypeLayoutEntry::storeEnumTagSinglePayloadGeneric(
    IRGenFunction &IGF, llvm::Value *tag, llvm::Value *numEmptyCases,
    Address addr,
    llvm::function_ref<void(Address addr, llvm::Value *tag)>
        storeExtraInhabitantIndexFun) const {
  auto &IGM = IGF.IGM;
  auto &Ctx = IGF.IGM.getLLVMContext();
  auto &Builder = IGF.Builder;

  auto numExtraInhabitants = this->extraInhabitantCount(IGF);
  auto size = this->size(IGF);

  auto *truncSize = Builder.CreateZExtOrTrunc(size, IGM.Int32Ty);
  auto &int32Ty = IGM.Int32Ty;
  auto *zero = llvm::ConstantInt::get(int32Ty, 0U);
  auto *one = llvm::ConstantInt::get(int32Ty, 1U);
  auto *four = llvm::ConstantInt::get(int32Ty, 4U);
  auto *eight = llvm::ConstantInt::get(int32Ty, 8U);

  auto *valueAddr =
      Builder.CreateBitOrPointerCast(addr.getAddress(), IGM.Int8PtrTy);
  auto extraTagBitsAddr =
      Address(Builder.CreateInBoundsGEP(valueAddr, size), Alignment(1));

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
      getEnumTagBytes(IGF, truncSize, emptyCases, IGM.getInt32(1)).numTagBytes;
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
      Builder.CreateICmpULE(tag, numExtraInhabitants);
  Builder.CreateCondBr(isPayloadOrExtraInhabitant, isPayloadOrInhabitantCaseBB,
                       isExtraTagBitsCaseBB);

  // We are the payload or fit within the extra inhabitants.
  Builder.emitBlock(isPayloadOrInhabitantCaseBB);
  // Zero the tag bits.
  emitSetTag(IGF, extraTagBitsAddr, zero, numExtraTagBytes);
  isPayloadOrInhabitantCaseBB = Builder.GetInsertBlock();
  auto *storeInhabitantBB = llvm::BasicBlock::Create(Ctx);
  auto *returnBB = llvm::BasicBlock::Create(Ctx);
  auto *isPayload = Builder.CreateICmpEQ(tag, zero);
  Builder.CreateCondBr(isPayload, returnBB, storeInhabitantBB);

  Builder.emitBlock(storeInhabitantBB);
  auto contBB2 = IGF.createBasicBlock("");
  auto hasXIBB = IGF.createBasicBlock("");
  auto isNonZeroXI = Builder.CreateICmpNE(numExtraInhabitants, zero);
  Builder.CreateCondBr(isNonZeroXI, hasXIBB, contBB2);
  {
    Builder.emitBlock(hasXIBB);
    // Store the extra inhabitant.
    storeExtraInhabitantIndexFun(addr, tag);
    Builder.CreateBr(contBB2);
  }
  Builder.emitBlock(contBB2);
  Builder.CreateBr(returnBB);

  // There are extra tag bits to consider.
  Builder.emitBlock(isExtraTagBitsCaseBB);

  // Write the extra tag bytes. At this point we know we have an no payload case
  // and therefore the index we should store is in the range
  // [0..ElementsWithNoPayload-1].
  auto *nonPayloadElementIndex = Builder.CreateSub(tag, one);
  auto *caseIndex =
      Builder.CreateSub(nonPayloadElementIndex, numExtraInhabitants);
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

  auto contBB = IGF.createBasicBlock("");
  auto nonZeroSizeBB = IGF.createBasicBlock("");
  auto isNonZero = Builder.CreateICmpNE(truncSize, zero);
  Builder.CreateCondBr(isNonZero, nonZeroSizeBB, contBB);
  {
    Builder.emitBlock(nonZeroSizeBB);
    auto *truncSize = Builder.CreateZExtOrTrunc(size, IGM.Int32Ty);
    auto sizeGTE4 = Builder.CreateICmpUGE(truncSize, four);
    auto sizeClampedTo4 = Builder.CreateSelect(sizeGTE4, four, truncSize);
    // Zero out the payload.
    Builder.CreateMemSet(addr, llvm::ConstantInt::get(IGF.IGM.Int8Ty, 0),
                         truncSize);
    // Store tag into the payload.
    emitStore1to4Bytes(IGF, addr, payloadIndex, sizeClampedTo4);
    Builder.CreateBr(contBB);
  }
  Builder.emitBlock(contBB);

  // Write to the extra tag bytes, if any.
  emitSetTag(IGF, extraTagBitsAddr, extraTagIndex, numExtraTagBytes);
  Builder.CreateBr(returnBB);

  Builder.emitBlock(returnBB);
}

static llvm::Value *projectOutlineBuffer(IRGenFunction &IGF, Address buffer,
                                         llvm::Value *alignmentMask) {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  Address boxAddress(Builder.CreateBitCast(buffer.getAddress(),
                                           IGM.RefCountedPtrTy->getPointerTo()),
                     buffer.getAlignment());
  auto *boxStart = Builder.CreateLoad(boxAddress);
  auto *heapHeaderSize =
      llvm::ConstantInt::get(IGM.SizeTy, IGM.RefCountedStructSize.getValue());
  auto *startOffset =
      Builder.CreateAnd(Builder.CreateAdd(heapHeaderSize, alignmentMask),
                        Builder.CreateNot(alignmentMask));
  auto *addressInBox =
      IGF.emitByteOffsetGEP(boxStart, startOffset, IGM.OpaqueTy);

  addressInBox = Builder.CreateBitCast(addressInBox, IGM.OpaquePtrTy);
  return addressInBox;
}

llvm::Value *TypeLayoutEntry::initBufferWithCopyOfBuffer(IRGenFunction &IGF,
                                                         Address dest,
                                                         Address src) const {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;

  auto size = this->size(IGF);
  auto alignMask = this->alignmentMask(IGF);
  auto isBitwiseTakable = this->isBitwiseTakable(IGF);

  auto bufferSize = IGM.getSize(getFixedBufferSize(IGM));
  auto bufferAlign = IGM.getSize(Size(getFixedBufferAlignment(IGM).getValue()));
  auto bufferAlignMask = Builder.CreateSub(bufferAlign, IGM.getSize(Size(1)));

  auto bufferAlignFits = Builder.CreateICmpUGE(bufferAlignMask, alignMask);
  auto bufferFits = Builder.CreateICmpUGE(bufferSize, size);

  auto canUseInline = Builder.CreateAnd(isBitwiseTakable, bufferFits);
  canUseInline = Builder.CreateAnd(canUseInline, bufferAlignFits);

  auto inlineBB = IGF.createBasicBlock("inlineBB");
  auto allocateBB = IGF.createBasicBlock("allocateBB");
  auto finishedBB = IGF.createBasicBlock("");
  auto pointerToObject = llvm::PHINode::Create(IGM.OpaquePtrTy, 2);
  Builder.CreateCondBr(canUseInline, inlineBB, allocateBB);

  Builder.emitBlock(inlineBB);
  {
    // Inline of the buffer.
    this->initWithCopy(IGF, dest, src);
    pointerToObject->addIncoming(
        Builder.CreateBitCast(dest, IGM.OpaquePtrTy).getAddress(),
        Builder.GetInsertBlock());
    Builder.CreateBr(finishedBB);
  }

  Builder.emitBlock(allocateBB);
  {
    // The buffer stores a reference to a copy-on-write managed heap buffer.
    auto *destReferenceAddr = Builder.CreateBitCast(
        dest.getAddress(), IGM.RefCountedPtrTy->getPointerTo());
    auto *srcReferenceAddr = Builder.CreateBitCast(
        src.getAddress(), IGM.RefCountedPtrTy->getPointerTo());
    auto *srcReference =
        Builder.CreateLoad(srcReferenceAddr, src.getAlignment());
    IGF.emitNativeStrongRetain(srcReference, IGF.getDefaultAtomicity());
    Builder.CreateStore(srcReference,
                        Address(destReferenceAddr, dest.getAlignment()));

    pointerToObject->addIncoming(projectOutlineBuffer(IGF, dest, alignMask),
                                 Builder.GetInsertBlock());
    Builder.CreateBr(finishedBB);
  }

  Builder.emitBlock(finishedBB);
  Builder.Insert(pointerToObject);

  return pointerToObject;
}

void ScalarTypeLayoutEntry::computeProperties() {
  // does not add anything.
}

void ScalarTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id) const {
  ScalarTypeLayoutEntry::Profile(id, typeInfo, representative);
}

void ScalarTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id,
                                    const TypeInfo &ti,
                                    SILType ty) {
  id.AddPointer(&ti);
  id.AddPointer(ty.getASTType().getPointer());
}

ScalarTypeLayoutEntry::~ScalarTypeLayoutEntry() {}

llvm::Value *ScalarTypeLayoutEntry::alignmentMask(IRGenFunction  &IGF) const {
  return typeInfo.getAlignmentMask(IGF, representative);
}

llvm::Value *ScalarTypeLayoutEntry::size(IRGenFunction &IGF) const {
  return typeInfo.getSize(IGF, representative);
}

bool ScalarTypeLayoutEntry::isFixedSize(IRGenModule& IGM) const {
  return true;
}

llvm::Optional<Size> ScalarTypeLayoutEntry::fixedSize(IRGenModule& IGM) const {
  return typeInfo.getFixedSize();
}

llvm::Optional<Alignment> ScalarTypeLayoutEntry::fixedAlignment(IRGenModule& IGM) const {
  return typeInfo.getFixedAlignment();
}

llvm::Optional<uint32_t> ScalarTypeLayoutEntry::fixedXICount(IRGenModule& IGM) const {
  return typeInfo.getFixedExtraInhabitantCount(IGM);
}

llvm::Value *
ScalarTypeLayoutEntry::extraInhabitantCount(IRGenFunction &IGF) const {
  auto &IGM = IGF.IGM;
  auto fixedXICount =
      typeInfo.getFixedExtraInhabitantCount(IGM);
  return llvm::ConstantInt::get(IGM.Int32Ty, fixedXICount);
}

llvm::Value *ScalarTypeLayoutEntry::isBitwiseTakable(IRGenFunction &IGF) const {
  return llvm::ConstantInt::get(IGF.IGM.Int1Ty,
                                typeInfo.isBitwiseTakable(
                                    ResilienceExpansion::Maximal));
}

void ScalarTypeLayoutEntry::destroy(IRGenFunction &IGF, Address addr) const {
  switch (scalarKind) {
  case ScalarKind::POD:
    return;
  case ScalarKind::Immovable:
    llvm_unreachable("cannot opaquely manipulate immovable types!");
  case ScalarKind::NativeStrongReference: {
    auto alignment = typeInfo.getFixedAlignment();
    auto addressType = typeInfo.getStorageType()->getPointerTo();
    auto castAddr = IGF.Builder.CreateBitCast(addr.getAddress(), addressType);
    llvm::Value *val = IGF.Builder.CreateLoad(castAddr, alignment);
    IGF.emitNativeStrongRelease(val, IGF.getDefaultAtomicity());
    return;
  }
  case ScalarKind::ErrorReference: {
    auto alignment = typeInfo.getFixedAlignment();
    auto addressType = typeInfo.getStorageType()->getPointerTo();
    auto castAddr = IGF.Builder.CreateBitCast(addr.getAddress(), addressType);
    llvm::Value *val = IGF.Builder.CreateLoad(castAddr, alignment);
    IGF.emitErrorStrongRelease(val);
    return;
  }
  case ScalarKind::NativeWeakReference: {
    IGF.emitNativeWeakDestroy(addr);
    return;
  }
  case ScalarKind::NativeUnownedReference: {
    IGF.emitNativeUnownedDestroy(addr);
    return;
  }
  case ScalarKind::BlockReference: {
    IGF.emitBlockRelease(addr.getAddress());
    return;
  }
  case ScalarKind::UnknownReference: {
    auto alignment = typeInfo.getFixedAlignment();
    auto addressType = typeInfo.getStorageType()->getPointerTo();
    auto castAddr = IGF.Builder.CreateBitCast(addr.getAddress(), addressType);
    llvm::Value *val = IGF.Builder.CreateLoad(castAddr, alignment);
    IGF.emitUnknownStrongRelease(val, IGF.getDefaultAtomicity());
    return;
  }
  case ScalarKind::UnknownUnownedReference: {
    IGF.emitUnknownUnownedDestroy(addr);
    return;
  }
  case ScalarKind::UnknownWeakReference: {
    IGF.emitUnknownWeakDestroy(addr);
    return;
  }
  case ScalarKind::BridgeReference: {
    auto alignment = typeInfo.getFixedAlignment();
    auto addressType = typeInfo.getStorageType()->getPointerTo();
    auto castAddr = IGF.Builder.CreateBitCast(addr.getAddress(), addressType);
    llvm::Value *val = IGF.Builder.CreateLoad(castAddr, alignment);
    IGF.emitBridgeStrongRelease(val, IGF.getDefaultAtomicity());
    return;
  }
  case ScalarKind::ObjCReference: {
    auto alignment = typeInfo.getFixedAlignment();
    auto addressType = typeInfo.getStorageType()->getPointerTo();
    auto &Builder = IGF.Builder;
    addr = Address(Builder.CreateBitCast(addr.getAddress(), addressType),
                   alignment);
    llvm::Value *val = IGF.Builder.CreateLoad(addr.getAddress(), alignment);
    IGF.emitObjCStrongRelease(val);
    break;
  }
  case ScalarKind::BlockStorage: {
    // The frontend will currently never emit copy_addr or destroy_addr for
    // block storage.
    IGF.unimplemented(SourceLoc(), "destroying @block_storage");
    return;
  }
  case ScalarKind::ThickFunc: {
    // A thick function is a pair of a function pointer and an optional
    // ref counted opaque pointer. We don't need to do anything for the
    // function pointer, but we need to release the pointer.
    auto alignment = typeInfo.getFixedAlignment();
    auto addressType = typeInfo.getStorageType()->getPointerTo();
    auto castAddr = IGF.Builder.CreateBitCast(addr.getAddress(), addressType);
    auto secondElement = IGF.Builder.CreateStructGEP(
        Address(castAddr, alignment), 1, IGF.IGM.getPointerSize(),
        addr->getName() + ".data");
    auto pointer = IGF.Builder.CreateLoad(secondElement);
    IGF.emitNativeStrongRelease(pointer, IGF.getDefaultAtomicity());
    return;
  }
  case ScalarKind::ExistentialReference: {
    emitDestroyBoxedOpaqueExistentialBuffer(IGF, representative, addr);
    return;
  }
  }
}

void ScalarTypeLayoutEntry::assignWithCopy(IRGenFunction &IGF, Address dest,
                                           Address src) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  dest =
      Address(Builder.CreateBitCast(dest.getAddress(), addressType), alignment);
  src =
      Address(Builder.CreateBitCast(src.getAddress(), addressType), alignment);
  typeInfo.assignWithCopy(IGF, dest, src, representative, true);
}

void ScalarTypeLayoutEntry::assignWithTake(IRGenFunction &IGF, Address dest,
                                           Address src) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  dest =
      Address(Builder.CreateBitCast(dest.getAddress(), addressType), alignment);
  src =
      Address(Builder.CreateBitCast(src.getAddress(), addressType), alignment);
  typeInfo.assignWithTake(IGF, dest, src, representative, true);
}

void ScalarTypeLayoutEntry::initWithCopy(IRGenFunction &IGF, Address dest,
                                         Address src) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  dest =
      Address(Builder.CreateBitCast(dest.getAddress(), addressType), alignment);
  src =
      Address(Builder.CreateBitCast(src.getAddress(), addressType), alignment);
  typeInfo.initializeWithCopy(IGF, dest, src, representative, true);
}

void ScalarTypeLayoutEntry::initWithTake(IRGenFunction &IGF, Address dest,
                                         Address src) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  dest =
      Address(Builder.CreateBitCast(dest.getAddress(), addressType), alignment);
  src =
      Address(Builder.CreateBitCast(src.getAddress(), addressType), alignment);
  typeInfo.initializeWithTake(IGF, dest, src, representative, true);
}

llvm::Value *ScalarTypeLayoutEntry::getEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *numEmptyCases, Address value) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  value = Address(Builder.CreateBitCast(value.getAddress(), addressType),
                  alignment);;
  return typeInfo.getEnumTagSinglePayload(IGF, numEmptyCases, value,
                                          representative, true);
}

void ScalarTypeLayoutEntry::storeEnumTagSinglePayload(IRGenFunction &IGF,
                                                llvm::Value *tag,
                                                llvm::Value *numEmptyCases,
                                                Address addr) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  addr =
      Address(Builder.CreateBitCast(addr.getAddress(), addressType), alignment);
  typeInfo.storeEnumTagSinglePayload(IGF, tag, numEmptyCases, addr,
                                     representative, true);
}

bool ScalarTypeLayoutEntry::classof(const TypeLayoutEntry *entry) {
  return entry->kind == TypeLayoutEntryKind::Scalar;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void ScalarTypeLayoutEntry::dump() const {
  if (typeInfo.isFixedSize())
    llvm::dbgs() << "{ scalar isFixedSize:" << (bool)typeInfo.isFixedSize()
                 << " isLoadable: " << (bool)typeInfo.isLoadable() << " size: "
                 << cast<FixedTypeInfo>(typeInfo).getFixedSize().getValue()
                 << " alignment: "
                 << cast<FixedTypeInfo>(typeInfo).getFixedAlignment().getValue()
                 << " rep: " << representative << " id: " << this << " }\n";
  if (!typeInfo.isFixedSize()) {
    llvm::dbgs() << "{ scalar non-fixed rep: " << representative
                 << " id: " << this << " }\n";
  }
}
#endif

void AlignedGroupEntry::computeProperties() {
  for (auto *entry : entries) {
    gatherProperties(entry);
  }
}

void AlignedGroupEntry::Profile(llvm::FoldingSetNodeID &id) const {
  AlignedGroupEntry::Profile(id, entries, minimumAlignment);
}

void AlignedGroupEntry::Profile(llvm::FoldingSetNodeID &id,
                                const std::vector<TypeLayoutEntry *> &entries,
                                Alignment::int_type minimumAlignment) {
  for (auto *entry : entries)
    id.AddPointer(entry);
  id.AddInteger(minimumAlignment);
}

AlignedGroupEntry::~AlignedGroupEntry() {}

llvm::Value *AlignedGroupEntry::alignmentMask(IRGenFunction &IGF) const {
  auto &IGM = IGF.IGM;
  auto minimumAlignmentMask = IGM.getSize(Size(minimumAlignment - 1));
  if (isFixedSize(IGF.IGM))
    return minimumAlignmentMask;

  // Non fixed layouts should have a minimumAlignment of 1.
  assert(minimumAlignment == 1);
  auto &Builder = IGF.Builder;
  llvm::Value *currentMaxAlignment = minimumAlignmentMask;
  for(auto *entry : entries) {
    auto entryAlignmentMask = entry->alignmentMask(IGF);
    currentMaxAlignment =
        Builder.CreateOr(entryAlignmentMask, currentMaxAlignment);
  }
  currentMaxAlignment->setName("alignment-mask");
  return currentMaxAlignment;
}

llvm::Value *AlignedGroupEntry::size(IRGenFunction &IGF) const {
  llvm::Value *currentSize = nullptr;
  auto &Builder = IGF.Builder;
  for(auto *entry : entries) {
    if (!currentSize) {
      currentSize = entry->size(IGF);
      continue;
    }
    // alignupto(currentSize, entry.alignment) + entry.size
    auto entryAlignMask = entry->alignmentMask(IGF);
    auto invertedMask = Builder.CreateNot(entryAlignMask);
    currentSize = Builder.CreateAdd(currentSize, entryAlignMask);
    currentSize = Builder.CreateAnd(currentSize, invertedMask);
    currentSize = Builder.CreateAdd(currentSize, entry->size(IGF));
  }
  currentSize->setName("size");
  return currentSize;
}

bool AlignedGroupEntry::isFixedSize(IRGenModule& IGM) const {
  for(auto *entry : entries) {
    if (!entry->isFixedSize(IGM)) {
      return false;
    }
  }
  return true;
}

llvm::Optional<Size> AlignedGroupEntry::fixedSize(IRGenModule& IGM) const {
  Size currentSize(0);
  for(auto *entry : entries) {
    if (!entry->fixedSize(IGM) || !entry->fixedAlignment(IGM)) {
      return llvm::NoneType::None;
    }
    Size entrySize = *entry->fixedSize(IGM);
    currentSize =
        currentSize.roundUpToAlignment(*entry->fixedAlignment(IGM)) + entrySize;
  }
  return currentSize;
}

llvm::Optional<Alignment> AlignedGroupEntry::fixedAlignment(IRGenModule& IGM) const {
  Alignment currentAlignment = Alignment(1);
  for(auto *entry : entries) {
    if (!entry->fixedAlignment(IGM)) {
      return llvm::NoneType::None;
    }
    currentAlignment = std::max(currentAlignment, *entry->fixedAlignment(IGM));
  }
  return currentAlignment;
}

llvm::Optional<uint32_t> AlignedGroupEntry::fixedXICount(IRGenModule& IGM) const {
  uint32_t currentMaxXICount = 0;
  // Choose the the field with the max xi count.
  for (auto *entry : entries) {
    auto entryXICount = entry->fixedXICount(IGM);
    if (!entryXICount) {
      return llvm::NoneType::None;
    }
    currentMaxXICount = std::max(*entryXICount, currentMaxXICount);
  }
  return currentMaxXICount;
}

llvm::Value *AlignedGroupEntry::extraInhabitantCount(IRGenFunction &IGF) const {
  llvm::Value *currentMaxXICount = IGF.IGM.getInt32(0);
  auto &Builder = IGF.Builder;
  // Choose the the field with the max xi count.
  for (auto *entry : entries) {
    auto entryXICount = entry->extraInhabitantCount(IGF);
    auto entryXICountGT =
        Builder.CreateICmpUGT(entryXICount, currentMaxXICount);
    currentMaxXICount =
        Builder.CreateSelect(entryXICountGT, entryXICount, currentMaxXICount);
  }
  currentMaxXICount->setName("num-extra-inhabitants");
  return currentMaxXICount;
}

llvm::Value *AlignedGroupEntry::isBitwiseTakable(IRGenFunction &IGF) const {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  llvm::Value *isBitwiseTakable = llvm::ConstantInt::get(IGM.Int1Ty, true);
  for(auto *entry : entries) {
    isBitwiseTakable =
        Builder.CreateAnd(isBitwiseTakable, entry->isBitwiseTakable(IGF));
  }
  return isBitwiseTakable;
}

static Address alignAddress(IRGenFunction &IGF, Address addr,
                            llvm::Value *alignMask) {
  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;
  auto ptr = Builder.CreatePtrToInt(addr.getAddress(), IGM.SizeTy);
  ptr = Builder.CreateAdd(ptr, alignMask);
  ptr = Builder.CreateAnd(ptr, Builder.CreateNot(alignMask));
  ptr = Builder.CreateIntToPtr(ptr, IGM.OpaquePtrTy);
  return Address(ptr, Alignment(1));
}

static Address addOffset(IRGenFunction &IGF, Address addr,
                         llvm::Value *offset) {
  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;
  auto ptr = Builder.CreatePtrToInt(addr.getAddress(), IGM.SizeTy);
  ptr = Builder.CreateAdd(ptr, offset);
  ptr = Builder.CreateIntToPtr(ptr, IGM.OpaquePtrTy);
  return Address(ptr, Alignment(1));
}

void AlignedGroupEntry::destroy(IRGenFunction &IGF, Address addr) const {
  if (isFixedSize(IGF.IGM)) {
    Size offset(0);
    for (auto entry: entries) {
      uint64_t aligned = offset.getValue();
      // Align the offset
      aligned += entry->fixedAlignment(IGF.IGM)->getMaskValue();
      aligned &= ~(entry->fixedAlignment(IGF.IGM)->getMaskValue());
      offset = Size(aligned);
      auto projected = IGF.emitByteOffsetGEP(
          addr.getAddress(),
          llvm::ConstantInt::get(IGF.IGM.Int32Ty, offset.getValue()),
          IGF.IGM.OpaquePtrTy);
      entry->destroy(IGF, Address(projected, *entry->fixedAlignment(IGF.IGM)));
      offset += *entry->fixedSize(IGF.IGM);
    }
  } else {
    Address currentDest = addr;
    auto remainingEntries = entries.size();
    for (auto *entry : entries) {
      if (currentDest.getAddress() != addr.getAddress()) {
        // Align upto the current entry's requirement.
        auto entryMask = entry->alignmentMask(IGF);
        currentDest = alignAddress(IGF, currentDest, entryMask);
      }

      entry->destroy(IGF, currentDest);

      --remainingEntries;
      if (remainingEntries == 0)
        continue;

      auto entrySize = entry->size(IGF);
      currentDest = addOffset(IGF, currentDest, entrySize);
    }
  }
}

void AlignedGroupEntry::withEachEntry(
    IRGenFunction &IGF, Address dest, Address src,
    llvm::function_ref<void(TypeLayoutEntry *entry, Address entryDest,
                            Address entrySrc)>
        entryFun) const {
  if (isFixedSize(IGF.IGM)) {
    Size offset(0);
    for (auto entry: entries) {
      uint64_t aligned = offset.getValue();
      // Align the offset
      aligned += entry->fixedAlignment(IGF.IGM)->getMaskValue();
      aligned &= ~(entry->fixedAlignment(IGF.IGM)->getMaskValue());
      offset = Size(aligned);
      auto projectedSrc = IGF.emitByteOffsetGEP(
          src.getAddress(),
          llvm::ConstantInt::get(IGF.IGM.Int32Ty, offset.getValue()),
          IGF.IGM.OpaquePtrTy);
      auto projectedDest = IGF.emitByteOffsetGEP(
          dest.getAddress(),
          llvm::ConstantInt::get(IGF.IGM.Int32Ty, offset.getValue()),
          IGF.IGM.OpaquePtrTy);
      entryFun(entry, Address(projectedDest, *entry->fixedAlignment(IGF.IGM)),
               Address(projectedSrc, *entry->fixedAlignment(IGF.IGM)));
      offset += *entry->fixedSize(IGF.IGM);
    }
  } else {
    Address currentDest = dest;
    Address currentSrc = src;
    auto remainingEntries = entries.size();
    for (auto *entry : entries) {
      if (currentDest.getAddress() != dest.getAddress()) {
        // Align upto the current entry's requirement.
        auto entryMask = entry->alignmentMask(IGF);
        currentDest = alignAddress(IGF, currentDest, entryMask);
        currentSrc = alignAddress(IGF, currentSrc, entryMask);
      }

      entryFun(entry, currentDest, currentSrc);

      --remainingEntries;
      if (remainingEntries == 0)
        continue;

      auto entrySize = entry->size(IGF);
      currentDest = addOffset(IGF, currentDest, entrySize);
      currentSrc = addOffset(IGF, currentSrc, entrySize);
    }
  }
}

void AlignedGroupEntry::assignWithCopy(IRGenFunction &IGF, Address dest,
                                       Address src) const {
  withEachEntry(
      IGF, dest, src,
      [&](TypeLayoutEntry *entry, Address entryDest, Address entrySrc) {
        entry->assignWithCopy(IGF, entryDest, entrySrc);
      });
}

void AlignedGroupEntry::assignWithTake(IRGenFunction &IGF, Address dest,
                                       Address src) const {
  withEachEntry(
      IGF, dest, src,
      [&](TypeLayoutEntry *entry, Address entryDest, Address entrySrc) {
        entry->assignWithTake(IGF, entryDest, entrySrc);
      });
}

void AlignedGroupEntry::initWithCopy(IRGenFunction &IGF, Address dest,
                                     Address src) const {
  withEachEntry(
      IGF, dest, src,
      [&](TypeLayoutEntry *entry, Address entryDest, Address entrySrc) {
        entry->initWithCopy(IGF, entryDest, entrySrc);
      });
}

void AlignedGroupEntry::initWithTake(IRGenFunction &IGF, Address dest,
                                     Address src) const {
  withEachEntry(
      IGF, dest, src,
      [&](TypeLayoutEntry *entry, Address entryDest, Address entrySrc) {
        entry->initWithTake(IGF, entryDest, entrySrc);
      });
}

llvm::Value *AlignedGroupEntry::withExtraInhabitantProvidingEntry(
    IRGenFunction &IGF, Address addr, llvm::Type *returnType,
    llvm::function_ref<llvm::Value *(TypeLayoutEntry *, Address, llvm::Value *)>
        entryFun) const {
  if (isFixedSize(IGF.IGM)) {
    // First compute the max xi count.
    auto maxXICount = fixedXICount(IGF.IGM);
    Size offset(0);
    for (auto entry : entries) {
      uint64_t aligned = offset.getValue();
      // Align the offset
      aligned += entry->fixedAlignment(IGF.IGM)->getMaskValue();
      aligned &= ~(entry->fixedAlignment(IGF.IGM)->getMaskValue());
      offset = Size(aligned);

      auto xiCount = entry->fixedXICount(IGF.IGM);
      if (xiCount == maxXICount) {
        auto projected = IGF.emitByteOffsetGEP(
            addr.getAddress(),
            llvm::ConstantInt::get(IGF.IGM.Int32Ty, offset.getValue()),
            IGF.IGM.OpaquePtrTy);
        return entryFun(entry, Address(projected, *entry->fixedAlignment(IGF.IGM)),
                 llvm::ConstantInt::get(IGF.IGM.Int32Ty, *xiCount));
      }
      offset += *entry->fixedSize(IGF.IGM);
    }
    llvm_unreachable("Failed to find entry that contains the xis");
  } else {
    // First compute the max xi count.
    auto maxXICount = extraInhabitantCount(IGF);

    Address currentAddr = addr;
    auto &IGM = IGF.IGM;
    auto &Builder = IGF.Builder;
    auto remainingEntries = entries.size();
    // Select the first field that matches the max xi count.
    auto mergeBB = IGF.createBasicBlock("found_max_xi");
    llvm::PHINode *mergePHI = nullptr;
    if (returnType != IGM.VoidTy)
      mergePHI = llvm::PHINode::Create(IGM.Int32Ty, remainingEntries);
    for (auto *entry : entries) {
      if (currentAddr.getAddress() != addr.getAddress()) {
        // Align upto the current entry's requirement.
        auto entryMask = entry->alignmentMask(IGF);
        currentAddr = alignAddress(IGF, currentAddr, entryMask);
      }
      auto xiCount = entry->extraInhabitantCount(IGF);

      auto isMaxXICount = Builder.CreateICmpEQ(xiCount, maxXICount);
      auto trueBB = IGF.createBasicBlock("");
      auto falseBB = IGF.createBasicBlock("");

      Builder.CreateCondBr(isMaxXICount, trueBB, falseBB);
      ConditionalDominanceScope scope(IGF);
      Builder.emitBlock(trueBB);

      auto tag = entryFun(entry, currentAddr, xiCount);
      if (mergePHI)
        mergePHI->addIncoming(tag, Builder.GetInsertBlock());
      Builder.CreateBr(mergeBB);

      Builder.emitBlock(falseBB);
      --remainingEntries;
      if (remainingEntries != 0) {
        auto entrySize = entry->size(IGF);
        currentAddr = addOffset(IGF, currentAddr, entrySize);
      }
    }
    // We should have found an entry with max xi count.
    Builder.CreateUnreachable();
    Builder.emitBlock(mergeBB);
    if (mergePHI)
      Builder.Insert(mergePHI);
    return mergePHI;
  }
}

llvm::Value *AlignedGroupEntry::getEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *numEmptyCases, Address addr) const {
  return getEnumTagSinglePayloadGeneric(
      IGF, addr, numEmptyCases, [&](Address addr) -> llvm::Value * {
        // Get tag in payload.
        auto tagInPayload = withExtraInhabitantProvidingEntry(
            IGF, addr, IGF.IGM.Int32Ty,
            [&](TypeLayoutEntry *entry, Address entryAddress,
                llvm::Value *entryXICount) -> llvm::Value * {
              return entry->getEnumTagSinglePayload(IGF, entryXICount,
                                                    entryAddress);
            });
        return tagInPayload;
      });
}

void AlignedGroupEntry::storeEnumTagSinglePayload(IRGenFunction &IGF,
                                                  llvm::Value *tag,
                                                  llvm::Value *numEmptyCases,
                                                  Address addr) const {
  storeEnumTagSinglePayloadGeneric(
      IGF, tag, numEmptyCases, addr, [&](Address addr, llvm::Value *tag) {
        // Store the extra inhabitant.
        withExtraInhabitantProvidingEntry(
            IGF, addr, IGF.IGM.VoidTy,
            [&](TypeLayoutEntry *entry, Address entryAddress,
                llvm::Value *entryXICount) -> llvm::Value * {
              entry->storeEnumTagSinglePayload(IGF, tag, entryXICount,
                                               entryAddress);
              return nullptr;
            });
      });
}

bool AlignedGroupEntry::classof(const TypeLayoutEntry *entry) {
  return entry->kind == TypeLayoutEntryKind::AlignedGroup;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void AlignedGroupEntry::dump() const {
    llvm::dbgs() << "{ aligned group:\n";
    llvm::dbgs() << "  alignment: " << minimumAlignment << "\n";;
    for (auto *entry : entries) {
      entry->dump();
    }
    llvm::dbgs() << "  id: " << this  << "}\n";
}
#endif

void ArchetypeLayoutEntry::computeProperties() { hasArchetypeField = true; }

void ArchetypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id) const {
  ArchetypeLayoutEntry::Profile(id, archetype);
}

void ArchetypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id,
                                   SILType archetype) {
  id.AddPointer(archetype.getASTType().getPointer());
}

ArchetypeLayoutEntry::~ArchetypeLayoutEntry() {}

llvm::Value *ArchetypeLayoutEntry::alignmentMask(IRGenFunction &IGF) const {
  return emitLoadOfAlignmentMask(IGF, archetype);
}

llvm::Value *ArchetypeLayoutEntry::size(IRGenFunction &IGF) const {
  return emitLoadOfSize(IGF, archetype);
}

bool ArchetypeLayoutEntry::isFixedSize(IRGenModule& IGM) const {
  return false;
}

llvm::Optional<Size> ArchetypeLayoutEntry::fixedSize(IRGenModule& IGM) const {
  return llvm::NoneType::None;
}

llvm::Optional<Alignment> ArchetypeLayoutEntry::fixedAlignment(IRGenModule& IGM) const {
  return llvm::NoneType::None;
}

llvm::Optional<uint32_t> ArchetypeLayoutEntry::fixedXICount(IRGenModule& IGM) const {
  return llvm::NoneType::None;
}

llvm::Value *
ArchetypeLayoutEntry::extraInhabitantCount(IRGenFunction &IGF) const {
  return emitLoadOfExtraInhabitantCount(IGF, archetype);
}

llvm::Value *
ArchetypeLayoutEntry::isBitwiseTakable(IRGenFunction &IGF) const {
  return emitLoadOfIsBitwiseTakable(IGF, archetype);
}

void ArchetypeLayoutEntry::destroy(IRGenFunction &IGF, Address addr) const {
  emitDestroyCall(IGF, archetype, addr);
}

void ArchetypeLayoutEntry::assignWithCopy(IRGenFunction &IGF, Address dest,
                                          Address src) const {
  emitAssignWithCopyCall(IGF, archetype, dest, src);
}

void ArchetypeLayoutEntry::assignWithTake(IRGenFunction &IGF, Address dest,
                                          Address src) const {
  emitAssignWithTakeCall(IGF, archetype, dest, src);
}

void ArchetypeLayoutEntry::initWithCopy(IRGenFunction &IGF, Address dest,
                                        Address src) const {
  emitInitializeWithCopyCall(IGF, archetype, dest, src);
}

void ArchetypeLayoutEntry::initWithTake(IRGenFunction &IGF, Address dest,
                                        Address src) const {
  emitInitializeWithTakeCall(IGF, archetype, dest, src);
}

llvm::Value *ArchetypeLayoutEntry::getEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *numEmptyCases, Address value) const {
  value = Address(
      IGF.Builder.CreateBitCast(value.getAddress(), IGF.IGM.OpaquePtrTy),
      value.getAlignment());

  return emitGetEnumTagSinglePayloadCall(IGF, archetype, numEmptyCases, value);
}

void ArchetypeLayoutEntry::storeEnumTagSinglePayload(IRGenFunction &IGF,
                                                     llvm::Value *tag,
                                                     llvm::Value *numEmptyCases,
                                                     Address addr) const {
  addr =
      Address(IGF.Builder.CreateBitCast(addr.getAddress(), IGF.IGM.OpaquePtrTy),
              addr.getAlignment());

  emitStoreEnumTagSinglePayloadCall(IGF, archetype, tag, numEmptyCases, addr);
}

bool ArchetypeLayoutEntry::classof(const TypeLayoutEntry *entry) {
  return entry->kind == TypeLayoutEntryKind::Archetype;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void ArchetypeLayoutEntry::dump() const {
  llvm::dbgs() << "{ archetype: " << archetype << " id: " << this << " }\n";
}
#endif

void EnumTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id) const {
  EnumTypeLayoutEntry::Profile(id, numEmptyCases, cases);
}

void EnumTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id,
                                  unsigned numEmptyCases,
                                  const std::vector<TypeLayoutEntry *> &cases) {
  id.AddInteger(numEmptyCases);
  for (auto *layout : cases)
    id.AddPointer(layout);
}

EnumTypeLayoutEntry::~EnumTypeLayoutEntry() {}

llvm::Value *EnumTypeLayoutEntry::alignmentMask(IRGenFunction &IGF) const {
  assert(!cases.empty());
  auto &IGM = IGF.IGM;
  auto minimumAlignmentMask = IGM.getSize(Size(minimumAlignment - 1));

  // Non fixed layouts should have a minimumAlignment of 1.
  assert(minimumAlignment == 1);
  auto &Builder = IGF.Builder;
  llvm::Value *currentMaxAlignment = minimumAlignmentMask;
  for(auto *entry : cases) {
    auto entryAlignmentMask = entry->alignmentMask(IGF);
    currentMaxAlignment =
        Builder.CreateOr(entryAlignmentMask, currentMaxAlignment);
  }
  return currentMaxAlignment;
}

llvm::Value *EnumTypeLayoutEntry::maxPayloadSize(IRGenFunction &IGF) const {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  llvm::Value *payloadSize = IGM.getSize(Size(0));
  for (auto &entry: cases) {
    auto entrySize = entry->size(IGF);
    auto gt = Builder.CreateICmpUGT(entrySize, payloadSize);
    payloadSize = Builder.CreateSelect(gt, entrySize, payloadSize);
    payloadSize->setName("payload-size");
  }
  return payloadSize;
}

llvm::Value *EnumTypeLayoutEntry::isBitwiseTakable(IRGenFunction &IGF) const {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  llvm::Value *isBitwiseTakable = llvm::ConstantInt::get(IGM.Int1Ty, true);
  for (auto &entry: cases) {
    isBitwiseTakable =
        Builder.CreateAnd(isBitwiseTakable, entry->isBitwiseTakable(IGF));
  }
  return isBitwiseTakable;
}

void EnumTypeLayoutEntry::computeProperties() {
  for (auto c: cases) {
    gatherProperties(c);
  }
}

llvm::Value *EnumTypeLayoutEntry::size(IRGenFunction &IGF) const {
  assert(!cases.empty());
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  auto &ctx = IGM.getLLVMContext();

  auto emptyCaseCount = IGM.getInt32(numEmptyCases);
  if (cases.size() == 1) {
    // Single payload enum.
    // // If there are enough extra inhabitants for all of the cases, then the
    // // size of the enum is the same as its payload.
    //
    // size_t size;
    // if (payloadNumExtraInhabitants >= emptyCases) {
    //   size = payloadSize;
    // } else {
    //   size = payloadSize + getEnumTagCounts(payloadSize,
    //                                     emptyCases -
    //                                     payloadNumExtraInhabitants,
    //                                       1 /*payload case*/).numTagBytes;
    // }
    auto payloadXIs = cases[0]->extraInhabitantCount(IGF);
    auto payloadSize = cases[0]->size(IGF);
    auto truncPayloadSize =
        Builder.CreateZExtOrTrunc(cases[0]->size(IGF), IGM.Int32Ty);
    auto enoughXIs = Builder.CreateICmpUGE(payloadXIs, emptyCaseCount);
    auto branchBB = Builder.GetInsertBlock();
    auto resumeBB = llvm::BasicBlock::Create(ctx);
    auto computeTagBB = llvm::BasicBlock::Create(ctx);
    Builder.CreateCondBr(enoughXIs, resumeBB, computeTagBB);

    Builder.emitBlock(computeTagBB);
    auto extraEmptyCases = Builder.CreateSub(emptyCaseCount, payloadXIs);
    auto enumTagBytes =
        getEnumTagBytes(IGF, truncPayloadSize, extraEmptyCases, IGM.getInt32(1))
            .numTagBytes;
    auto payloadPlusEnumTagBytes = Builder.CreateAdd(
        payloadSize, Builder.CreateZExtOrTrunc(enumTagBytes, IGM.SizeTy));
    computeTagBB = Builder.GetInsertBlock();
    Builder.CreateBr(resumeBB);

    Builder.emitBlock(resumeBB);
    auto size = Builder.CreatePHI(IGM.SizeTy, 2);
    size->setName("size");
    size->addIncoming(payloadSize, branchBB);
    size->addIncoming(payloadPlusEnumTagBytes, computeTagBB);
    return size;
  }

  assert(cases.size() > 1);
  auto payloadSize = maxPayloadSize(IGF);
  auto truncPayloadSize =
      Builder.CreateZExtOrTrunc(payloadSize, IGM.Int32Ty);
  auto numPayloads = IGM.getInt32(cases.size());
  auto extraTagBytes =
    Builder.CreateZExtOrTrunc(
      getEnumTagBytes(IGF, truncPayloadSize, emptyCaseCount, numPayloads)
          .numTagBytes, IGM.SizeTy);
  return Builder.CreateAdd(payloadSize, extraTagBytes);
}

bool EnumTypeLayoutEntry::isFixedSize(IRGenModule& IGM) const {
  return fixedSize(IGM).hasValue();
}

llvm::Optional<Size> EnumTypeLayoutEntry::fixedSize(IRGenModule& IGM) const {
  assert(!cases.empty());
  if (cases.size() == 1) {
    // Single payload enum.
    //
    // If there are enough extra inhabitants for all of the cases, then the
    // size of the enum is the same as its payload.
    //
     Size size;
     auto payloadNumExtraInhabitants = cases[0]->fixedXICount(IGM);
     auto payloadSize= cases[0]->fixedSize(IGM);
     if (!payloadNumExtraInhabitants || !payloadSize) {
       return llvm::NoneType::None;
     }
     if (*payloadNumExtraInhabitants >= numEmptyCases) {
       size = *payloadSize;
     } else {
       size = Size((*payloadSize).getValue() + getFixedEnumTagBytes(
                                   *payloadSize,
                                   numEmptyCases - *payloadNumExtraInhabitants,
                                   1 /*payload case*/)
                                   .numTagBytes);
     }
     return size;
  }

  assert(cases.size() > 1);
  Size maxPayloadSize(0);
  for (auto enum_case : cases) {
    auto caseSize = enum_case->fixedSize(IGM);
    if (!caseSize) {
      return llvm::NoneType::None;
    }
    maxPayloadSize = std::max(*caseSize, maxPayloadSize);
  }
  uint32_t truncPayloadSize = maxPayloadSize.getValue();
  auto extraTagBytes =
      getFixedEnumTagBytes(Size(truncPayloadSize), numEmptyCases, cases.size())
          .numTagBytes;
  return maxPayloadSize + Size(extraTagBytes);
}

llvm::Optional<Alignment> EnumTypeLayoutEntry::fixedAlignment(IRGenModule& IGM) const {
  Alignment maxAlign(1);
  for (auto payload: cases) {
    auto caseAlign = payload->fixedAlignment(IGM);
    if (!caseAlign) {
      return llvm::NoneType::None;
    }
    maxAlign = std::max(*caseAlign, maxAlign);
  }
  return maxAlign;
}

llvm::Optional<uint32_t> EnumTypeLayoutEntry::fixedXICount(IRGenModule& IGM) const {
  assert(!cases.empty());

  if (cases.size() == 1) {
    // Single payload enum.
    // unsigned unusedExtraInhabitants =
    //   payloadNumExtraInhabitants >= emptyCases ?
    //     payloadNumExtraInhabitants - emptyCases : 0;
    auto payloadXIs = cases[0]->fixedXICount(IGM);
    if (!payloadXIs) { return llvm::NoneType::None; }
    return payloadXIs >= numEmptyCases ? *payloadXIs - numEmptyCases : 0;
  }

  // See whether there are extra inhabitants in the tag.
  // unsigned numExtraInhabitants = tagCounts.numTagBytes == 4
  //   ? INT_MAX
  //   : (1 << (tagCounts.numTagBytes * 8)) - tagCounts.numTags;
  // numExtraInhabitants = std::min(numExtraInhabitants,
  //                       unsigned(ValueWitnessFlags::MaxNumExtraInhabitants));
  Size maxPayloadSize(0);
  for (auto enum_case : cases) {
    auto caseSize = enum_case->fixedSize(IGM);
    if (!caseSize) {
      return llvm::NoneType::None;
    }
    maxPayloadSize = std::max(*caseSize, maxPayloadSize);
  }
  auto extraTagInfo = getFixedEnumTagBytes(maxPayloadSize, numEmptyCases, cases.size());
  uint32_t extraTagBytes = extraTagInfo.numTagBytes;
  uint32_t numTags = extraTagInfo.numTags;
  if (extraTagBytes == 4) {
    return ValueWitnessFlags::MaxNumExtraInhabitants;
  } else {
    auto numXIs = (1 << extraTagBytes * 8) - numTags;
    return std::min(ValueWitnessFlags::MaxNumExtraInhabitants, numXIs);
  }
}

llvm::Value *EnumTypeLayoutEntry::extraInhabitantCount(IRGenFunction &IGF) const {
  assert(!cases.empty());
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;

  if (cases.size() == 1) {
    // Single payload enum.
    // unsigned unusedExtraInhabitants =
    //   payloadNumExtraInhabitants >= emptyCases ?
    //     payloadNumExtraInhabitants - emptyCases : 0;
    auto emptyCaseCount = llvm::ConstantInt::get(IGM.Int32Ty, numEmptyCases);
    auto payloadXIs = cases[0]->extraInhabitantCount(IGF);
    auto enoughXIs = Builder.CreateICmpUGE(payloadXIs, emptyCaseCount);
    auto remainingXIs = Builder.CreateSub(payloadXIs, emptyCaseCount);
    auto zero = IGM.getInt32(0);
    auto unusedXIs = Builder.CreateSelect(enoughXIs, remainingXIs, zero);
    unusedXIs->setName("num-extra-inhabitants");
    return unusedXIs;
  }

  assert(cases.size() > 1);
  auto one = IGM.getInt32(1);
  auto four = IGM.getInt32(4);
  auto eight = IGM.getInt32(8);
  auto intMax = IGM.getInt32(INT_MAX);

  // See whether there are extra inhabitants in the tag.
  // unsigned numExtraInhabitants = tagCounts.numTagBytes == 4
  //   ? INT_MAX
  //   : (1 << (tagCounts.numTagBytes * 8)) - tagCounts.numTags;
  // numExtraInhabitants = std::min(numExtraInhabitants,
  //                       unsigned(ValueWitnessFlags::MaxNumExtraInhabitants));
  auto payloadSize =
      Builder.CreateZExtOrTrunc(maxPayloadSize(IGF), IGM.Int32Ty);
  auto numPayloads = IGM.getInt32(cases.size());
  auto emptyCaseCount = IGM.getInt32(numEmptyCases);
  auto extraTagInfo =
      getEnumTagBytes(IGF, payloadSize, emptyCaseCount, numPayloads);
  auto maxNumXIs = llvm::ConstantInt::get(
      IGM.Int32Ty, ValueWitnessFlags::MaxNumExtraInhabitants);
  auto extraTagBytes =
      Builder.CreateZExtOrTrunc(extraTagInfo.numTagBytes, IGM.Int32Ty);
  auto numXIs = Builder.CreateMul(extraTagBytes, eight);
  numXIs = Builder.CreateShl(one, numXIs);
  auto numTags = Builder.CreateZExtOrTrunc(extraTagInfo.numTags, IGM.Int32Ty);
  numXIs = Builder.CreateSub(numXIs, numTags);
  auto fourTagBytes = Builder.CreateICmpEQ(extraTagBytes, four);
  numXIs = Builder.CreateSelect(fourTagBytes, intMax, numXIs);
  auto lte = Builder.CreateICmpULE(numXIs, maxNumXIs);
  numXIs = Builder.CreateSelect(lte, numXIs, maxNumXIs);
  numXIs->setName("num-extra-inhabitants");
  return numXIs;
}

static void emitMemCpy(IRGenFunction &IGF, Address dest, Address src,
                       llvm::Value *size) {
  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;

  // If the layout is fixed, the size will be a constant.
  // Otherwise, do a memcpy of the dynamic size of the type.
  auto byteDestAddr = Builder.CreateBitOrPointerCast(dest.getAddress(), IGM.Int8PtrTy);
  auto byteSrcAddr =
      Builder.CreateBitOrPointerCast(src.getAddress(), IGM.Int8PtrTy);
  Builder.CreateMemCpy(byteDestAddr, llvm::MaybeAlign(dest.getAlignment()),
                       byteSrcAddr, llvm::MaybeAlign(src.getAlignment()), size);
}

llvm::BasicBlock *
EnumTypeLayoutEntry::testSinglePayloadEnumContainsPayload(IRGenFunction &IGF,
                                                          Address addr) const {
  assert(cases.size() == 1);
  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;

  auto emptyCases = IGM.getInt32(numEmptyCases);
  auto tag = cases[0]->getEnumTagSinglePayload(IGF, emptyCases, addr);
  auto payloadBB = IGF.createBasicBlock("payloadBlock");
  auto noPayloadBB = IGF.createBasicBlock("noPayloadBlock");
  auto hasPaylaod = Builder.CreateICmpEQ(tag, IGM.getInt32(0));
  Builder.CreateCondBr(hasPaylaod, payloadBB, noPayloadBB);

  Builder.emitBlock(payloadBB);
  return noPayloadBB;
}

void EnumTypeLayoutEntry::initializeSinglePayloadEnum(IRGenFunction &IGF,
                                                      Address dest, Address src,
                                                      IsTake_t isTake) const {
  assert(cases.size() == 1);

  Address destData = dest;
  Address srcData = src;
  auto payload = cases[0];
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;

  auto endBB = IGF.createBasicBlock("");

  // See whether the source value has a payload.
  auto noSrcPayloadBB = testSinglePayloadEnumContainsPayload(IGF, src);

  {
    ConditionalDominanceScope condition(IGF);

    // Here, the source value has a payload. Initialize the destination
    // with it, and set the extra tag if any to zero.
    payload->initialize(IGF, destData, srcData, isTake);
    // Potentially initialize extra tag bytes.
    payload->storeEnumTagSinglePayload(IGF, IGM.getInt32(0),
                                       IGM.getInt32(numEmptyCases), dest);
    Builder.CreateBr(endBB);
  }

  // If the source value has no payload, we can primitive-store the
  // empty-case value.
  Builder.emitBlock(noSrcPayloadBB);
  {
    ConditionalDominanceScope condition(IGF);
    emitMemCpy(IGF, dest, src, size(IGF));
    Builder.CreateBr(endBB);
  }

  IGF.Builder.emitBlock(endBB);
}

void EnumTypeLayoutEntry::assignSinglePayloadEnum(IRGenFunction &IGF,
                                                  Address dest, Address src,
                                                  IsTake_t isTake) const {
  assert(cases.size() == 1);
  Address destData = dest;
  Address srcData = src;

  auto payload = cases[0];
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;

  auto endBB = IGF.createBasicBlock("");

  // See whether the current value at the destination has a payload.
  auto *noDestPayloadBB = testSinglePayloadEnumContainsPayload(IGF, dest);
  {
    ConditionalDominanceScope destCondition(IGF);

    // Here, the destination has a payload. Now see if the source also
    // has one.
    auto destNoSrcPayloadBB = testSinglePayloadEnumContainsPayload(IGF, src);
    {
      ConditionalDominanceScope destSrcCondition(IGF);

      // Here, both source and destination have payloads. Do the
      // reassignment of the payload in-place.
      payload->assign(IGF, destData, srcData, isTake);
      Builder.CreateBr(endBB);
    }

    // If the destination has a payload but the source doesn't, we can
    // destroy the payload and primitive-store the new no-payload value.
    Builder.emitBlock(destNoSrcPayloadBB);
    {
      ConditionalDominanceScope destNoSrcCondition(IGF);
      payload->destroy(IGF, destData);
      emitMemCpy(IGF, dest, src, this->size(IGF));
      Builder.CreateBr(endBB);
    }
  }

  // Now, if the destination has no payload, check if the source has one.
  Builder.emitBlock(noDestPayloadBB);
  {
    ConditionalDominanceScope noDestCondition(IGF);
    auto noDestNoSrcPayloadBB = testSinglePayloadEnumContainsPayload(IGF, src);
    {
      ConditionalDominanceScope noDestSrcCondition(IGF);

      // Here, the source has a payload but the destination doesn't.
      // We can copy-initialize the source over the destination, then
      // primitive-store the zero extra tag (if any).
      payload->initialize(IGF, destData, srcData, isTake);
      // Potentially initialize extra tag bytes.
      payload->storeEnumTagSinglePayload(IGF, IGM.getInt32(0),
                                         IGM.getInt32(numEmptyCases), dest);
      Builder.CreateBr(endBB);
    }

    // If neither destination nor source have payloads, we can just
    // primitive-store the new empty-case value.
    Builder.emitBlock(noDestNoSrcPayloadBB);
    {
      ConditionalDominanceScope noDestNoSrcCondition(IGF);
      emitMemCpy(IGF, dest, src, this->size(IGF));
      Builder.CreateBr(endBB);
    }
  }

  Builder.emitBlock(endBB);
}

void EnumTypeLayoutEntry::multiPayloadEnumForPayloadAndEmptyCases(
    IRGenFunction &IGF, Address addr,
    llvm::function_ref<void(TypeLayoutEntry *payload, llvm::Value *tagIndex)>
        payloadFunction,
    llvm::function_ref<void()> noPayloadFunction) const {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  auto &ctxt = IGM.getLLVMContext();

  auto tag = getEnumTagMultipayload(IGF, addr);
  auto *endBB = llvm::BasicBlock::Create(ctxt);

  auto *trivialBB = llvm::BasicBlock::Create(ctxt);
  bool anyTrivial = numEmptyCases != 0;
  unsigned numPayloads = cases.size();

  auto switchBuilder = SwitchBuilder::create(
      IGF, tag,
      SwitchDefaultDest(trivialBB,
                        anyTrivial ? IsNotUnreachable : IsUnreachable),
      numPayloads);

  unsigned tagIndex = 0;
  // Payload cases start at 0.
  for (auto &payload : cases) {
    auto *caseBB = llvm::BasicBlock::Create(ctxt);
    auto *tag = IGM.getInt32(tagIndex);
    switchBuilder->addCase(tag, caseBB);
    Builder.emitBlock(caseBB);
    {
      ConditionalDominanceScope scope(IGF);
      payloadFunction(payload, tag);
    }
    Builder.CreateBr(endBB);
    ++tagIndex;
  }

  if (anyTrivial) {
    Builder.emitBlock(trivialBB);
    {
      ConditionalDominanceScope scope(IGF);
      noPayloadFunction();
    }
    Builder.CreateBr(endBB);
  } else {
    // If there are no trivial cases to handle, this is unreachable.
    if (trivialBB->use_empty()) {
      delete trivialBB;
    } else {
      Builder.emitBlock(trivialBB);
      Builder.CreateUnreachable();
    }
  }

  Builder.emitBlock(endBB);
}

void EnumTypeLayoutEntry::destroyMultiPayloadEnum(IRGenFunction &IGF,
                                                  Address addr) const {
  multiPayloadEnumForPayloadAndEmptyCases(
      IGF, addr,
      [&](TypeLayoutEntry *payload, llvm::Value *) {
        payload->destroy(IGF, addr);
      },
      []() { /* nothing to do */ });
}

void EnumTypeLayoutEntry::assignMultiPayloadEnum(IRGenFunction &IGF,
                                                 Address dest, Address src,
                                                 IsTake_t isTake) const {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  auto &ctxt = IGM.getLLVMContext();
  auto *endBB = llvm::BasicBlock::Create(ctxt);

  // Check whether the source and destination alias.
  llvm::Value *alias =
      Builder.CreateICmpEQ(dest.getAddress(), src.getAddress());
  auto *noAliasBB = llvm::BasicBlock::Create(ctxt);
  Builder.CreateCondBr(alias, endBB, noAliasBB);
  Builder.emitBlock(noAliasBB);
  {
    ConditionalDominanceScope condition(IGF);

    // Destroy the old value.
    destroyMultiPayloadEnum(IGF, dest);

    // Reinitialize with the new value.
    initializeMultiPayloadEnum(IGF, dest, src, isTake);

    IGF.Builder.CreateBr(endBB);
  }
  IGF.Builder.emitBlock(endBB);
}

void EnumTypeLayoutEntry::initializeMultiPayloadEnum(IRGenFunction &IGF,
                                                     Address dest, Address src,
                                                     IsTake_t isTake) const {
  multiPayloadEnumForPayloadAndEmptyCases(
      IGF, src,
      [&](TypeLayoutEntry *payload, llvm::Value *tagIndex) {
        if (isTake)
          payload->initWithTake(IGF, dest, src);
        else
          payload->initWithCopy(IGF, dest, src);
        storeMultiPayloadTag(IGF, tagIndex, dest);
      },
      [&]() { emitMemCpy(IGF, dest, src, this->size(IGF)); });
}

void EnumTypeLayoutEntry::destroySinglePayloadEnum(IRGenFunction &IGF,
                                                   Address addr) const {
  auto payload = cases[0];
  auto scalarLayout = llvm::dyn_cast<ScalarTypeLayoutEntry>(payload);
  if (scalarLayout && scalarLayout->scalarKind == ScalarKind::POD) {
    // Do Nothing
  }
  // If the payload is a single refcounted pointer and we have a single
  // empty case, then the layout will be a nullable pointer, and we can
  // pass enum values directly into swift_retain/swift_release as-is.
  else if (scalarLayout && isNullableRefCounted(scalarLayout->scalarKind) && numEmptyCases == 1) {
    // Since release can handle null pointers, we don't need to emit a test, we
    // can just release
    payload->destroy(IGF, addr);
  } else {
    // Check that there is a payload at the address.
    llvm::BasicBlock *endBB = testSinglePayloadEnumContainsPayload(IGF, addr);
    {
      ConditionalDominanceScope condition(IGF);

      // If there is, destroy it.
      payload->destroy(IGF, addr);

      IGF.Builder.CreateBr(endBB);
    }
    IGF.Builder.emitBlock(endBB);
  }
}

void EnumTypeLayoutEntry::destroy(IRGenFunction &IGF, Address addr) const {
  assert(!cases.empty());

  if (cases.size() == 1) {
    destroySinglePayloadEnum(IGF, addr);
    return;
  }

  destroyMultiPayloadEnum(IGF, addr);
}

void EnumTypeLayoutEntry::assignWithCopy(IRGenFunction &IGF, Address dest,
                                         Address src) const {
  assert(!cases.empty());
  if (cases.size() == 1) {
    return assignSinglePayloadEnum(IGF, dest, src, IsNotTake);
  }
  assert(cases.size() > 1);
  assignMultiPayloadEnum(IGF, dest, src, IsNotTake);
}

void EnumTypeLayoutEntry::assignWithTake(IRGenFunction &IGF, Address dest,
                                         Address src) const {
  assert(!cases.empty());
  if (cases.size() == 1) {
    return assignSinglePayloadEnum(IGF, dest, src, IsTake);
  }

  assert(cases.size() > 1);
  assignMultiPayloadEnum(IGF, dest, src, IsTake);
}

void EnumTypeLayoutEntry::initWithCopy(IRGenFunction &IGF, Address dest,
                                       Address src) const {
  assert(!cases.empty());
  if (cases.size() == 1) {
    return initializeSinglePayloadEnum(IGF, dest, src, IsNotTake);
  }

  assert(cases.size() > 1);
  initializeMultiPayloadEnum(IGF, dest, src, IsNotTake);
}

void EnumTypeLayoutEntry::initWithTake(IRGenFunction &IGF, Address dest,
                                       Address src) const {
  assert(!cases.empty());
  if (cases.size() == 1) {
    return initializeSinglePayloadEnum(IGF, dest, src, IsTake);
  }

  assert(cases.size() > 1);
  initializeMultiPayloadEnum(IGF, dest, src, IsTake);
}

std::pair<Address, llvm::Value *>
EnumTypeLayoutEntry::getMultiPalyloadEnumTagByteAddrAndNumBytes(
    IRGenFunction &IGF, Address addr) const {
  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;
  auto payloadSize = maxPayloadSize(IGF);
  auto *valueAddr =
      Builder.CreateBitOrPointerCast(addr.getAddress(), IGM.Int8PtrTy);
  auto extraTagBytesAddr =
      Address(Builder.CreateInBoundsGEP(valueAddr, payloadSize), Alignment(1));
  auto numPayloads = IGM.getInt32(cases.size());
  auto emptyCaseCount = IGM.getInt32(numEmptyCases);

  auto truncPayloadSize = Builder.CreateZExtOrTrunc(payloadSize, IGM.Int32Ty);
  auto extraTagInfo =
      getEnumTagBytes(IGF, truncPayloadSize, emptyCaseCount, numPayloads);

  return std::make_pair(extraTagBytesAddr, extraTagInfo.numTagBytes);
}

llvm::Value *EnumTypeLayoutEntry::getEnumTagSinglePayloadForMultiPayloadEnum(
    IRGenFunction &IGF, Address addr, llvm::Value *emptyCases) const {
  // We don't handle (create enum type layout entries) multi payload enums that
  // have known spare bits (enums that are always fixed size) . Therefore the
  // only place to store extra inhabitants is in available bits in the extra tag
  // bytes.
  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;
  return getEnumTagSinglePayloadGeneric(
      IGF, addr, emptyCases, [&](Address addr) -> llvm::Value * {
        // Compute the address and number of the tag bytes.
        Address extraTagBitsAddr;
        llvm::Value *numTagBytes;
        std::tie(extraTagBitsAddr, numTagBytes) =
            getMultiPalyloadEnumTagByteAddrAndNumBytes(IGF, addr);

        // Compute the tag.
        // unsigned tag;
        // if (numTagBytes == 4)
        //   tag = loadedTag;
        // else
        //   tag = loadedTag | (~0u & (~0u << (numTagBytes * 8)));
        auto loadedTag = emitLoad1to4Bytes(IGF, extraTagBitsAddr, numTagBytes);
        auto tag = llvm::PHINode::Create(IGM.Int32Ty, 2);
        auto four = IGM.getInt32(4);
        auto hasFourTagBytes = Builder.CreateICmpEQ(numTagBytes, four);
        tag->addIncoming(loadedTag, Builder.GetInsertBlock());
        auto tagComputedBB = IGF.createBasicBlock("");
        auto lessThanFourBytesBB = IGF.createBasicBlock("");
        Builder.CreateCondBr(hasFourTagBytes, tagComputedBB, lessThanFourBytesBB);

        Builder.emitBlock(lessThanFourBytesBB);
        auto baseValue = IGM.getInt32(~0u);
        auto shifted = Builder.CreateShl(
            baseValue, Builder.CreateMul(numTagBytes, IGM.getInt32(8)));
        auto newTag =
            Builder.CreateOr(loadedTag, Builder.CreateAnd(baseValue, shifted));
        tag->addIncoming(newTag, Builder.GetInsertBlock());
        Builder.CreateBr(tagComputedBB);

        Builder.emitBlock(tagComputedBB);
        Builder.Insert(tag);

        // index = ~tag;
        // if (index >= extraInhabitantCount)
        //   index = 0;
        // else
        //   index = index + 1;
        auto index = Builder.CreateNot(tag);

        auto result = llvm::PHINode::Create(IGM.Int32Ty, 2);
        auto resultBB = IGF.createBasicBlock("");
        auto addOneBB = IGF.createBasicBlock("");
        auto indexGEQXICount =
            Builder.CreateICmpUGE(index, extraInhabitantCount(IGF));
        result->addIncoming(IGM.getInt32(0), Builder.GetInsertBlock());
        Builder.CreateCondBr(indexGEQXICount, resultBB, addOneBB);

        Builder.emitBlock(addOneBB);
        result->addIncoming(Builder.CreateAdd(index, IGM.getInt32(1)),
                            Builder.GetInsertBlock());
        Builder.CreateBr(resultBB);

        Builder.emitBlock(resultBB);
        Builder.Insert(result);
        return result;
      });
}

llvm::Value *EnumTypeLayoutEntry::getEnumTagSinglePayloadForSinglePayloadEnum(
    IRGenFunction &IGF, Address addr, llvm::Value *emptyCases) const {
  assert(cases.size() == 1);
  return getEnumTagSinglePayloadGeneric(
      IGF, addr, emptyCases, [&](Address addr) -> llvm::Value * {
        auto payloadEntry = cases[0];
        auto maxNumXIPayload = payloadEntry->extraInhabitantCount(IGF);
        // Read the tag from the payload and adjust it by the number
        // cases of this enum.
        auto tag =
            payloadEntry->getEnumTagSinglePayload(IGF, maxNumXIPayload, addr);
        auto numEnumCases = IGF.IGM.getInt32(numEmptyCases);
        auto adjustedTag = IGF.Builder.CreateSub(tag, numEnumCases);
        auto isEnumValue = IGF.Builder.CreateICmpULE(tag, numEnumCases);
        adjustedTag = IGF.Builder.CreateSelect(isEnumValue, IGF.IGM.getInt32(0),
                                               adjustedTag);
        return adjustedTag;
      });
}

llvm::Value *EnumTypeLayoutEntry::getEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *emptyCases, Address addr) const {
  assert(!cases.empty());
  if (cases.size() == 1) {
    return getEnumTagSinglePayloadForSinglePayloadEnum(IGF, addr, emptyCases);
  }
  return getEnumTagSinglePayloadForMultiPayloadEnum(IGF, addr, emptyCases);
}

void EnumTypeLayoutEntry::storeEnumTagSinglePayloadForSinglePayloadEnum(
    IRGenFunction &IGF, llvm::Value *tag, llvm::Value *emptyCases,
    Address addr) const {
  assert(cases.size() == 1);
  storeEnumTagSinglePayloadGeneric(
      IGF, tag, emptyCases, addr, [&](Address addr, llvm::Value *tag) {
        auto payloadEntry = cases[0];
        auto maxNumXIPayload = payloadEntry->extraInhabitantCount(IGF);
        auto numExtraCases = IGF.IGM.getInt32(numEmptyCases);
        // Adjust the tag.
        llvm::Value *adjustedTag = IGF.Builder.CreateAdd(tag, numExtraCases);

        // Preserve the zero tag so that we don't pass down a meaningless XI
        // value that the payload will waste time installing before we
        // immediately overwrite it.
        auto isEnumValue = IGF.Builder.CreateIsNull(tag);
        adjustedTag = IGF.Builder.CreateSelect(isEnumValue, IGF.IGM.getInt32(0),
                                               adjustedTag);
        payloadEntry->storeEnumTagSinglePayload(IGF, adjustedTag,
                                                maxNumXIPayload, addr);

      });
}

void EnumTypeLayoutEntry::storeMultiPayloadTag(IRGenFunction &IGF,
                                               llvm::Value *value,
                                               Address enumAddr) const {
  // Compute the address and number of the tag bytes.
  Address extraTagBytesAddr;
  llvm::Value *numTagBytes;
  std::tie(extraTagBytesAddr, numTagBytes) =
      getMultiPalyloadEnumTagByteAddrAndNumBytes(IGF, enumAddr);

  emitStore1to4Bytes(IGF, extraTagBytesAddr, value, numTagBytes);
}

void EnumTypeLayoutEntry::storeMultiPayloadValue(IRGenFunction &IGF,
                                                 llvm::Value *value,
                                                 Address enumAddr) const {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  auto truncSize = Builder.CreateZExtOrTrunc(maxPayloadSize(IGF), IGM.Int32Ty);
  auto four = IGM.getInt32(4);
  auto sizeGTE4 = Builder.CreateICmpUGE(truncSize, four);
  auto sizeClampedTo4 = Builder.CreateSelect(sizeGTE4, four, truncSize);
  Builder.CreateMemSet(enumAddr, llvm::ConstantInt::get(IGF.IGM.Int8Ty, 0),
                       truncSize);
  emitStore1to4Bytes(IGF, enumAddr, value, sizeClampedTo4);
}

void EnumTypeLayoutEntry::storeEnumTagSinglePayloadForMultiPayloadEnum(
    IRGenFunction &IGF, llvm::Value *tag, llvm::Value *emptyCases,
    Address addr) const {
  // We don't handle (create enum type layout entries) multi payload enums that
  // have known spare bits (enums that are always fixed size). Therefore the
  // only place to store extra inhabitants is in available bits in the extra tag
  // bytes.
  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;
  storeEnumTagSinglePayloadGeneric(
      IGF, tag, emptyCases, addr, [&](Address addr, llvm::Value *tag) {
        ConditionalDominanceScope scope(IGF);
        auto invertedTag =
            Builder.CreateNot(Builder.CreateSub(tag, IGM.getInt32(1)));
        storeMultiPayloadTag(IGF, invertedTag, addr);
      });
}

void EnumTypeLayoutEntry::storeEnumTagSinglePayload(IRGenFunction &IGF,
                                                    llvm::Value *tag,
                                                    llvm::Value *emptyCases,
                                                    Address addr) const {
  assert(!cases.empty());
  if (cases.size() == 1) {
    storeEnumTagSinglePayloadForSinglePayloadEnum(IGF, tag, emptyCases, addr);
    return;
  }

  storeEnumTagSinglePayloadForMultiPayloadEnum(IGF, tag, emptyCases, addr);
}

bool EnumTypeLayoutEntry::isMultiPayloadEnum() const {
  return cases.size() > 1;
}

llvm::Value *
EnumTypeLayoutEntry::getEnumTagMultipayload(IRGenFunction &IGF,
                                            Address enumAddr) const {
  auto &IGM = IGF.IGM;
  // unsigned tag = loadMultiPayloadTag(value, layout);
  // if (tag < numPayloads) {
  //   // If the tag indicates a payload, then we're done.
  //   return tag;
  // } else {
  //   // Otherwise, the other part of the discriminator is in the payload.
  //   unsigned payloadValue = loadMultiPayloadValue(value, layout);
  //   if (layout.payloadSize >= 4) {
  //     return numPayloads + payloadValue;
  //   } else {
  //     unsigned numPayloadBits = layout.payloadSize * CHAR_BIT;
  //     return (payloadValue | (tag - numPayloads) << numPayloadBits)
  //            + numPayloads;
  //   }
  // }
  auto &Builder = IGF.Builder;
  Address extraTagBitsAddr;
  llvm::Value *numTagBytes;
  std::tie(extraTagBitsAddr, numTagBytes) =
      getMultiPalyloadEnumTagByteAddrAndNumBytes(IGF, enumAddr);
  auto loadedTag = emitLoad1to4Bytes(IGF, extraTagBitsAddr, numTagBytes);
  auto resultBB = IGF.createBasicBlock("result");
  auto usePayloadBB = IGF.createBasicBlock("use-payload-for-tag");
  auto numPayloads = IGM.getInt32(cases.size());
  auto usePayloadValue = Builder.CreateICmpUGE(loadedTag, numPayloads);
  auto tagValue = llvm::PHINode::Create(IGM.Int32Ty, 3);
  tagValue->addIncoming(loadedTag, Builder.GetInsertBlock());
  Builder.CreateCondBr(usePayloadValue, usePayloadBB, resultBB);

  Builder.emitBlock(usePayloadBB);
  auto four = IGM.getInt32(4);
  auto truncSize = Builder.CreateZExtOrTrunc(maxPayloadSize(IGF), IGM.Int32Ty);
  auto sizeGTE4 = Builder.CreateICmpUGE(truncSize, four);
  auto sizeClampedTo4 = Builder.CreateSelect(sizeGTE4, four, truncSize);
  auto payloadValue = emitLoad1to4Bytes(IGF, enumAddr, sizeClampedTo4);
  auto payloadGTE4BB = IGF.createBasicBlock("");
  auto payloadLT4BB = IGF.createBasicBlock("");
  auto isPayloadGTE4 = Builder.CreateICmpUGE(truncSize, four);
  Builder.CreateCondBr(isPayloadGTE4, payloadGTE4BB, payloadLT4BB);

  Builder.emitBlock(payloadGTE4BB);
  auto result2 = Builder.CreateAdd(numPayloads, payloadValue);
  tagValue->addIncoming(result2, Builder.GetInsertBlock());
  Builder.CreateBr(resultBB);

  Builder.emitBlock(payloadLT4BB);
  auto numPayloadBits = Builder.CreateMul(truncSize, IGM.getInt32(CHAR_BIT));
  auto tmp = Builder.CreateSub(loadedTag, numPayloads);
  auto tmp2 = Builder.CreateShl(tmp, numPayloadBits);
  auto tmp3 = Builder.CreateOr(payloadValue, tmp2);
  auto result3 = Builder.CreateAdd(tmp3, numPayloads);
  tagValue->addIncoming(result3, Builder.GetInsertBlock());
  Builder.CreateBr(resultBB);

  Builder.emitBlock(resultBB);
  Builder.Insert(tagValue);
  return tagValue;
}

llvm::Value *EnumTypeLayoutEntry::getEnumTag(IRGenFunction &IGF,
                                             Address enumAddr) const {
  assert(!cases.empty());

  if (cases.size() == 1) {
    // Single payload enum.
    auto &IGM = IGF.IGM;
    auto payload = cases[0];
    auto emptyCases = IGM.getInt32(numEmptyCases);
    return payload->getEnumTagSinglePayload(IGF, emptyCases, enumAddr);
  }

  return getEnumTagMultipayload(IGF, enumAddr);
}

void EnumTypeLayoutEntry::destructiveProjectEnumData(IRGenFunction &IGF,
                                                     Address enumAddr) const {

  if (cases.size() == 1) {
    // Nothing to do because single payload enums don't interleave tag bits.
    return;
  }
  // Nothing to do here either because we don't handle fixed size enums which
  // would be the only ones to use spare bits in the payload.
}

void EnumTypeLayoutEntry::storeEnumTagMultipayload(IRGenFunction &IGF,
                                                   llvm::Value *tag,
                                                   Address enumAddr) const {
  // if (whichCase < numPayloads) {
  //   // For a payload case, store the tag after the payload area.
  //   storeMultiPayloadTag(value, layout, whichCase);
  // } else {
  //   // For an empty case, factor out the parts that go in the payload and
  //   // tag areas.
  //   unsigned whichEmptyCase = whichCase - numPayloads;
  //   unsigned whichTag, whichPayloadValue;
  //   if (layout.payloadSize >= 4) {
  //     whichTag = numPayloads;
  //     whichPayloadValue = whichEmptyCase;
  //   } else {
  //     unsigned numPayloadBits = layout.payloadSize * CHAR_BIT;
  //     whichTag = numPayloads + (whichEmptyCase >> numPayloadBits);
  //     whichPayloadValue = whichEmptyCase & ((1U << numPayloads) - 1U);
  //   }
  //   storeMultiPayloadTag(value, layout, whichTag);
  //   storeMultiPayloadValue(value, layout, whichPayloadValue);
  // }
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;

  auto numPayloads = IGM.getInt32(cases.size());
  auto shouldStoreOnlyTag = Builder.CreateICmpULT(tag, numPayloads);
  auto tagOnlyBB = IGF.createBasicBlock("tag-only");
  auto bothBB = IGF.createBasicBlock("tag-and-payload");
  auto finishedBB = IGF.createBasicBlock("");
  Builder.CreateCondBr(shouldStoreOnlyTag, tagOnlyBB, bothBB);

  Builder.emitBlock(tagOnlyBB);
  {
    ConditionalDominanceScope scope(IGF);
    storeMultiPayloadTag(IGF, tag, enumAddr);
  }
  Builder.CreateBr(finishedBB);

  Builder.emitBlock(bothBB);
  {
    ConditionalDominanceScope scope(IGF);
    auto payloadSize =
        Builder.CreateZExtOrTrunc(maxPayloadSize(IGF), IGM.Int32Ty);
    auto four = IGM.getInt32(4);
    auto payloadSizeGTE4 = Builder.CreateICmpUGE(payloadSize, four);
    auto whichTag = llvm::PHINode::Create(IGM.Int32Ty, 2);
    auto whichPayloadValue = llvm::PHINode::Create(IGM.Int32Ty, 2);
    auto whichEmptyCase = Builder.CreateSub(tag, numPayloads);
    auto payloadLT4BB = IGF.createBasicBlock("");
    auto storeBB = IGF.createBasicBlock("");
    whichTag->addIncoming(numPayloads, Builder.GetInsertBlock());
    whichPayloadValue->addIncoming(whichEmptyCase, Builder.GetInsertBlock());
    Builder.CreateCondBr(payloadSizeGTE4, storeBB, payloadLT4BB);

    Builder.emitBlock(payloadLT4BB);
    auto numPayloadBits =
        Builder.CreateMul(payloadSize, IGM.getInt32(CHAR_BIT));
    auto tmp = Builder.CreateLShr(whichEmptyCase, numPayloadBits);
    auto tmp2 = Builder.CreateAdd(numPayloads, tmp);
    whichTag->addIncoming(tmp2, Builder.GetInsertBlock());

    auto tmp3 = Builder.CreateSub(
        Builder.CreateShl(IGM.getInt32(1), numPayloads), IGM.getInt32(1));
    auto tmp4 = Builder.CreateAnd(whichEmptyCase, tmp3);
    whichPayloadValue->addIncoming(tmp4, Builder.GetInsertBlock());
    Builder.CreateBr(storeBB);

    Builder.emitBlock(storeBB);
    Builder.Insert(whichTag);
    Builder.Insert(whichPayloadValue);
    storeMultiPayloadTag(IGF, whichTag, enumAddr);
    storeMultiPayloadValue(IGF, whichPayloadValue, enumAddr);
  }
  Builder.CreateBr(finishedBB);

  Builder.emitBlock(finishedBB);
}

void EnumTypeLayoutEntry::destructiveInjectEnumTag(IRGenFunction &IGF,
                                                   llvm::Value *tag,
                                                   Address enumAddr) const {
  if (cases.size() == 1) {
    auto payload = cases[0];
    auto emptyCases = IGF.IGM.getInt32(numEmptyCases);
    payload->storeEnumTagSinglePayload(IGF, tag, emptyCases, enumAddr);
    return;
  }

  storeEnumTagMultipayload(IGF, tag, enumAddr);
}

bool EnumTypeLayoutEntry::classof(const TypeLayoutEntry *entry) {
  return entry->kind == TypeLayoutEntryKind::Enum;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void EnumTypeLayoutEntry::dump() const {
    llvm::dbgs() << "{ enum emptycases: " << numEmptyCases << "\n";
    for (auto *c : cases) {
      c->dump();
    }
    llvm::dbgs() << " id: " << this << " }\n";
}
#endif

void ResilientTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id) const {
  ResilientTypeLayoutEntry::Profile(id, ty);
}

void ResilientTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id, SILType ty) {
  id.AddPointer(ty.getASTType().getPointer());
}

ResilientTypeLayoutEntry::~ResilientTypeLayoutEntry() {}

llvm::Value *ResilientTypeLayoutEntry::alignmentMask(IRGenFunction &IGF) const {
  return emitLoadOfAlignmentMask(IGF, ty);
}

llvm::Value *ResilientTypeLayoutEntry::size(IRGenFunction &IGF) const {
  return emitLoadOfSize(IGF, ty);
}

llvm::Optional<Size> ResilientTypeLayoutEntry::fixedSize(IRGenModule& IGM) const {
  return llvm::NoneType::None;
}

bool ResilientTypeLayoutEntry::isFixedSize(IRGenModule& IGM) const {
  return false;
}

llvm::Optional<Alignment> ResilientTypeLayoutEntry::fixedAlignment(IRGenModule& IGM) const {
  return llvm::NoneType::None;
}

llvm::Optional<uint32_t> ResilientTypeLayoutEntry::fixedXICount(IRGenModule& IGM) const {
  return llvm::NoneType::None;
}

llvm::Value *
ResilientTypeLayoutEntry::extraInhabitantCount(IRGenFunction &IGF) const {
  return emitLoadOfExtraInhabitantCount(IGF, ty);
}

llvm::Value *
ResilientTypeLayoutEntry::isBitwiseTakable(IRGenFunction &IGF) const {
  return emitLoadOfIsBitwiseTakable(IGF, ty);
}

void ResilientTypeLayoutEntry::computeProperties() {
  hasResilientField = true;
  if (ty.getASTType()->hasArchetype())
    hasDependentResilientField = true;
}

void ResilientTypeLayoutEntry::destroy(IRGenFunction &IGF, Address addr) const {
  emitDestroyCall(IGF, ty, addr);
}

void ResilientTypeLayoutEntry::assignWithCopy(IRGenFunction &IGF, Address dest,
                                           Address src) const {
  emitAssignWithCopyCall(IGF, ty, dest, src);
}

void ResilientTypeLayoutEntry::assignWithTake(IRGenFunction &IGF, Address dest,
                                              Address src) const {
  emitAssignWithTakeCall(IGF, ty, dest, src);
}

void ResilientTypeLayoutEntry::initWithCopy(IRGenFunction &IGF, Address dest,
                                            Address src) const {
  emitInitializeWithCopyCall(IGF, ty, dest, src);
}

void ResilientTypeLayoutEntry::initWithTake(IRGenFunction &IGF, Address dest,
                                            Address src) const {
  emitInitializeWithTakeCall(IGF, ty, dest, src);
}

llvm::Value *ResilientTypeLayoutEntry::getEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *numEmptyCases, Address value) const {

  value = Address(
      IGF.Builder.CreateBitCast(value.getAddress(), IGF.IGM.OpaquePtrTy),
      value.getAlignment());

  return emitGetEnumTagSinglePayloadCall(IGF, ty, numEmptyCases, value);
}

void ResilientTypeLayoutEntry::storeEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *tag, llvm::Value *numEmptyCases,
    Address addr) const {
  addr =
      Address(IGF.Builder.CreateBitCast(addr.getAddress(), IGF.IGM.OpaquePtrTy),
              addr.getAlignment());

  emitStoreEnumTagSinglePayloadCall(IGF, ty, tag, numEmptyCases, addr);
}

bool ResilientTypeLayoutEntry::classof(const TypeLayoutEntry *entry) {
  return entry->kind == TypeLayoutEntryKind::Resilient;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void ResilientTypeLayoutEntry::dump() const {
  llvm::dbgs() << "{ resilient type: " << ty << " id: " << this << " }\n";
}
#endif

void TypeInfoBasedTypeLayoutEntry::computeProperties() {
  // does not add anything.
}

void TypeInfoBasedTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id) const {
  ScalarTypeLayoutEntry::Profile(id, typeInfo, representative);
}

void TypeInfoBasedTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id,
                                           const TypeInfo &ti, SILType ty) {
  id.AddPointer(&ti);
  id.AddPointer(ty.getASTType().getPointer());
}

TypeInfoBasedTypeLayoutEntry::~TypeInfoBasedTypeLayoutEntry() {}

llvm::Value *
TypeInfoBasedTypeLayoutEntry::alignmentMask(IRGenFunction &IGF) const {
  assert(typeInfo.isFixedSize());
  return typeInfo.getAlignmentMask(IGF, representative);
}

llvm::Value *TypeInfoBasedTypeLayoutEntry::size(IRGenFunction &IGF) const {
  assert(typeInfo.isFixedSize());
  return typeInfo.getSize(IGF, representative);
}

bool TypeInfoBasedTypeLayoutEntry::isFixedSize(IRGenModule& IGM) const {
  return true;
}

llvm::Optional<Size> TypeInfoBasedTypeLayoutEntry::fixedSize(IRGenModule& IGM) const {
  return typeInfo.getFixedSize();
}

llvm::Optional<Alignment> TypeInfoBasedTypeLayoutEntry::fixedAlignment(IRGenModule& IGM) const {
  return typeInfo.getFixedAlignment();
}

llvm::Optional<uint32_t> TypeInfoBasedTypeLayoutEntry::fixedXICount(IRGenModule& IGM) const {
  return typeInfo.getFixedExtraInhabitantCount(IGM);
}

llvm::Value *
TypeInfoBasedTypeLayoutEntry::extraInhabitantCount(IRGenFunction &IGF) const {
  assert(typeInfo.isFixedSize());
  auto &IGM = IGF.IGM;
  auto fixedXICount =
      cast<FixedTypeInfo>(typeInfo).getFixedExtraInhabitantCount(IGM);
  return llvm::ConstantInt::get(IGM.Int32Ty, fixedXICount);
}

llvm::Value *
TypeInfoBasedTypeLayoutEntry::isBitwiseTakable(IRGenFunction &IGF) const {
  assert(typeInfo.isFixedSize());
  return llvm::ConstantInt::get(IGF.IGM.Int1Ty,
                                cast<FixedTypeInfo>(typeInfo).isBitwiseTakable(
                                    ResilienceExpansion::Maximal));
}

void TypeInfoBasedTypeLayoutEntry::destroy(IRGenFunction &IGF,
                                           Address addr) const {
  auto alignment = cast<FixedTypeInfo>(typeInfo).getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  addr =
      Address(Builder.CreateBitCast(addr.getAddress(), addressType), alignment);
  typeInfo.destroy(IGF, addr, representative, true);
}

void TypeInfoBasedTypeLayoutEntry::assignWithCopy(IRGenFunction &IGF,
                                                  Address dest,
                                                  Address src) const {
  auto alignment = cast<FixedTypeInfo>(typeInfo).getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  dest =
      Address(Builder.CreateBitCast(dest.getAddress(), addressType), alignment);
  src =
      Address(Builder.CreateBitCast(src.getAddress(), addressType), alignment);
  typeInfo.assignWithCopy(IGF, dest, src, representative, true);
}

void TypeInfoBasedTypeLayoutEntry::assignWithTake(IRGenFunction &IGF,
                                                  Address dest,
                                                  Address src) const {
  auto alignment = cast<FixedTypeInfo>(typeInfo).getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  dest =
      Address(Builder.CreateBitCast(dest.getAddress(), addressType), alignment);
  src =
      Address(Builder.CreateBitCast(src.getAddress(), addressType), alignment);
  typeInfo.assignWithTake(IGF, dest, src, representative, true);
}

void TypeInfoBasedTypeLayoutEntry::initWithCopy(IRGenFunction &IGF,
                                                Address dest,
                                                Address src) const {
  auto alignment = cast<FixedTypeInfo>(typeInfo).getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  dest =
      Address(Builder.CreateBitCast(dest.getAddress(), addressType), alignment);
  src =
      Address(Builder.CreateBitCast(src.getAddress(), addressType), alignment);
  typeInfo.initializeWithCopy(IGF, dest, src, representative, true);
}

void TypeInfoBasedTypeLayoutEntry::initWithTake(IRGenFunction &IGF,
                                                Address dest,
                                                Address src) const {
  auto alignment = cast<FixedTypeInfo>(typeInfo).getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  dest =
      Address(Builder.CreateBitCast(dest.getAddress(), addressType), alignment);
  src =
      Address(Builder.CreateBitCast(src.getAddress(), addressType), alignment);
  typeInfo.initializeWithTake(IGF, dest, src, representative, true);
}

llvm::Value *TypeInfoBasedTypeLayoutEntry::getEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *numEmptyCases, Address value) const {
  auto alignment = cast<FixedTypeInfo>(typeInfo).getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  value = Address(Builder.CreateBitCast(value.getAddress(), addressType),
                  alignment);
  ;
  return typeInfo.getEnumTagSinglePayload(IGF, numEmptyCases, value,
                                          representative, true);
}

void TypeInfoBasedTypeLayoutEntry::storeEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *tag, llvm::Value *numEmptyCases,
    Address addr) const {
  auto alignment = cast<FixedTypeInfo>(typeInfo).getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  addr =
      Address(Builder.CreateBitCast(addr.getAddress(), addressType), alignment);
  typeInfo.storeEnumTagSinglePayload(IGF, tag, numEmptyCases, addr,
                                     representative, true);
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void TypeInfoBasedTypeLayoutEntry::dump() const {
  if (typeInfo.isFixedSize())
    llvm::dbgs() << "{ scalar isFixedSize:" << (bool)typeInfo.isFixedSize()
                 << " isLoadable: " << (bool)typeInfo.isLoadable() << " size: "
                 << cast<FixedTypeInfo>(typeInfo).getFixedSize().getValue()
                 << " alignment: "
                 << cast<FixedTypeInfo>(typeInfo).getFixedAlignment().getValue()
                 << " rep: " << representative << " id: " << this << " }\n";
  if (!typeInfo.isFixedSize()) {
    llvm::dbgs() << "{ scalar non-fixed rep: " << representative
                 << " id: " << this << " }\n";
  }
}
#endif

ScalarTypeLayoutEntry *TypeLayoutCache::getOrCreateScalarEntry(
    const TypeInfo &ti, SILType representative, ScalarKind kind) {
  assert(ti.isFixedSize());
  llvm::FoldingSetNodeID id;
  ScalarTypeLayoutEntry::Profile(id, cast<FixedTypeInfo>(ti), representative);
  // Do we already have an entry.
  void *insertPos;
  if (auto *entry = scalarEntries.FindNodeOrInsertPos(id, insertPos)) {
    return entry;
  }
  // Otherwise, create a new one.
  auto bytes = sizeof(ScalarTypeLayoutEntry);
  auto mem = bumpAllocator.Allocate(bytes, alignof(ScalarTypeLayoutEntry));
  auto newEntry = new (mem) ScalarTypeLayoutEntry(cast<FixedTypeInfo>(ti), representative, kind);
  scalarEntries.InsertNode(newEntry, insertPos);
  newEntry->computeProperties();
  return newEntry;
}

ArchetypeLayoutEntry *
TypeLayoutCache::getOrCreateArchetypeEntry(SILType archetype) {
  llvm::FoldingSetNodeID id;
  ArchetypeLayoutEntry::Profile(id, archetype);
  void *insertPos;
  if (auto *entry = archetypeEntries.FindNodeOrInsertPos(id, insertPos)) {
    return entry;
  }
  auto bytes = sizeof(ArchetypeLayoutEntry);
  auto mem = bumpAllocator.Allocate(bytes, alignof(ArchetypeLayoutEntry));
  auto newEntry = new (mem) ArchetypeLayoutEntry(archetype);
  archetypeEntries.InsertNode(newEntry, insertPos);
  newEntry->computeProperties();
  return newEntry;
}

AlignedGroupEntry *TypeLayoutCache::getOrCreateAlignedGroupEntry(
    const std::vector<TypeLayoutEntry *> &entries,
    Alignment::int_type minimumAlignment) {
  llvm::FoldingSetNodeID id;
  AlignedGroupEntry::Profile(id, entries, minimumAlignment);
  void *insertPos;
  if (auto *entry = alignedGroupEntries.FindNodeOrInsertPos(id, insertPos)) {
    return entry;
  }
  auto bytes = sizeof(AlignedGroupEntry);
  auto mem = bumpAllocator.Allocate(bytes, alignof(AlignedGroupEntry));
  auto newEntry =
      new (mem) AlignedGroupEntry(entries, minimumAlignment);
  alignedGroupEntries.InsertNode(newEntry, insertPos);
  newEntry->computeProperties();
  return newEntry;
}

TypeLayoutEntry *TypeLayoutCache::getEmptyEntry() { return &emptyEntry; }

EnumTypeLayoutEntry *TypeLayoutCache::getOrCreateEnumEntry(
    unsigned numEmptyCases,
    const std::vector<TypeLayoutEntry *> &nonEmptyCases) {

  llvm::FoldingSetNodeID id;
  EnumTypeLayoutEntry::Profile(id, numEmptyCases, nonEmptyCases);
  void *insertPos;
  if (auto *entry = enumEntries.FindNodeOrInsertPos(id, insertPos)) {
    return entry;
  }
  auto bytes = sizeof(EnumTypeLayoutEntry);
  auto mem = bumpAllocator.Allocate(bytes, alignof(EnumTypeLayoutEntry));
  auto newEntry = new (mem) EnumTypeLayoutEntry(numEmptyCases, nonEmptyCases);
  enumEntries.InsertNode(newEntry, insertPos);
  newEntry->computeProperties();
  return newEntry;
}

ResilientTypeLayoutEntry *
TypeLayoutCache::getOrCreateResilientEntry(SILType ty) {
  llvm::FoldingSetNodeID id;
  ResilientTypeLayoutEntry::Profile(id, ty);
  void *insertPos;
  if (auto *entry = resilientEntries.FindNodeOrInsertPos(id, insertPos)) {
    return entry;
  }
  auto bytes = sizeof(ResilientTypeLayoutEntry);
  auto mem = bumpAllocator.Allocate(bytes, alignof(ResilientTypeLayoutEntry));
  auto newEntry = new (mem) ResilientTypeLayoutEntry(ty);
  resilientEntries.InsertNode(newEntry, insertPos);
  newEntry->computeProperties();
  return newEntry;
}

TypeInfoBasedTypeLayoutEntry *
TypeLayoutCache::getOrCreateTypeInfoBasedEntry(const TypeInfo &ti,
                                               SILType representative) {
  assert(ti.isFixedSize());
  llvm::FoldingSetNodeID id;
  TypeInfoBasedTypeLayoutEntry::Profile(id, ti, representative);
  // Do we already have an entry.
  void *insertPos;
  if (auto *entry = typeInfoBasedEntries.FindNodeOrInsertPos(id, insertPos)) {
    return entry;
  }
  // Otherwise, create a new one.
  auto bytes = sizeof(ScalarTypeLayoutEntry);
  auto mem = bumpAllocator.Allocate(bytes, alignof(ScalarTypeLayoutEntry));
  auto newEntry = new (mem) TypeInfoBasedTypeLayoutEntry(cast<FixedTypeInfo>(ti), representative);
  typeInfoBasedEntries.InsertNode(newEntry, insertPos);
  newEntry->computeProperties();
  return newEntry;
}

TypeLayoutCache::~TypeLayoutCache() {
  for (auto &entry : scalarEntries) {
    entry.~ScalarTypeLayoutEntry();
  }
  for (auto &entry : archetypeEntries) {
    entry.~ArchetypeLayoutEntry();
  }
  for (auto &entry : alignedGroupEntries) {
    entry.~AlignedGroupEntry();
  }
  for (auto &entry : enumEntries) {
    entry.~EnumTypeLayoutEntry();
  }
  for (auto &entry : resilientEntries) {
    entry.~ResilientTypeLayoutEntry();
  }
  for (auto &entry : typeInfoBasedEntries) {
    entry.~TypeInfoBasedTypeLayoutEntry();
  }
}
