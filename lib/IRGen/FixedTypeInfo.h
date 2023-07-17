//===--- FixedTypeInfo.h - Supplement for fixed-layout types ----*- C++ -*-===//
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
// This file defines FixedTypeInfo, which supplements the TypeInfo
// interface for classes with (at least locally) fixed-layout type
// implementations.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_FIXEDTYPEINFO_H
#define SWIFT_IRGEN_FIXEDTYPEINFO_H

#include "Address.h"
#include "TypeInfo.h"
#include "swift/Basic/ClusteredBitVector.h"
#include "swift/SIL/SILType.h"

namespace llvm {
  class ConstantInt;
}

namespace swift {
namespace irgen {

/// FixedTypeInfo - An abstract class designed for use when
/// implementing a type that has a statically known layout.
class FixedTypeInfo : public TypeInfo {
private:
  /// The spare bit mask for this type. SpareBits[0] is the LSB of the first
  /// byte. This may be empty if the type has no spare bits.
  SpareBitVector SpareBits;
  
protected:
  FixedTypeInfo(llvm::Type *type, Size size,
                const SpareBitVector &spareBits,
                Alignment align, IsTriviallyDestroyable_t pod,
                IsBitwiseTakable_t bt,
                IsCopyable_t copy,
                IsFixedSize_t alwaysFixedSize,
                SpecialTypeInfoKind stik = SpecialTypeInfoKind::Fixed)
      : TypeInfo(type, align, pod, bt, copy, alwaysFixedSize, IsABIAccessible, stik),
        SpareBits(spareBits) {
    assert(SpareBits.size() == size.getValueInBits());
    assert(isFixedSize());
    Bits.FixedTypeInfo.Size = size.getValue();
    assert(Bits.FixedTypeInfo.Size == size.getValue() && "truncation");
  }

  FixedTypeInfo(llvm::Type *type, Size size,
                SpareBitVector &&spareBits,
                Alignment align, IsTriviallyDestroyable_t pod,
                IsBitwiseTakable_t bt,
                IsCopyable_t copy,
                IsFixedSize_t alwaysFixedSize,
                SpecialTypeInfoKind stik = SpecialTypeInfoKind::Fixed)
      : TypeInfo(type, align, pod, bt, copy, alwaysFixedSize, IsABIAccessible, stik),
        SpareBits(std::move(spareBits)) {
    assert(SpareBits.size() == size.getValueInBits());
    assert(isFixedSize());
    Bits.FixedTypeInfo.Size = size.getValue();
    assert(Bits.FixedTypeInfo.Size == size.getValue() && "truncation");
  }

public:
  // This is useful for metaprogramming.
  static bool isFixed() { return true; }
  static IsABIAccessible_t isABIAccessible() { return IsABIAccessible; }

  /// Whether this type is known to be empty.
  bool isKnownEmpty(ResilienceExpansion expansion) const {
    return (isFixedSize(expansion) && getFixedSize().isZero());
  }

  StackAddress allocateStack(IRGenFunction &IGF, SILType T,
                             const llvm::Twine &name) const override;
  void deallocateStack(IRGenFunction &IGF, StackAddress addr, SILType T) const override;
  void destroyStack(IRGenFunction &IGF, StackAddress addr, SILType T,
                    bool isOutlined) const override;

  // We can give these reasonable default implementations.

  void initializeWithTake(IRGenFunction &IGF, Address destAddr, Address srcAddr,
                          SILType T, bool isOutlined) const override;

  llvm::Value *getSize(IRGenFunction &IGF, SILType T) const override;
  llvm::Value *getAlignmentMask(IRGenFunction &IGF, SILType T) const override;
  llvm::Value *getStride(IRGenFunction &IGF, SILType T) const override;
  llvm::Value *getIsTriviallyDestroyable(IRGenFunction &IGF, SILType T) const override;
  llvm::Value *getIsBitwiseTakable(IRGenFunction &IGF, SILType T) const override;
  llvm::Value *isDynamicallyPackedInline(IRGenFunction &IGF,
                                         SILType T) const override;

  llvm::Constant *getStaticSize(IRGenModule &IGM) const override;
  llvm::Constant *getStaticAlignmentMask(IRGenModule &IGM) const override;
  llvm::Constant *getStaticStride(IRGenModule &IGM) const override;

  void completeFixed(Size size, Alignment alignment) {
    Bits.FixedTypeInfo.Size = size.getValue();
    assert(Bits.FixedTypeInfo.Size == size.getValue() && "truncation");
    setStorageAlignment(alignment);
  }

  /// Returns the known, fixed alignment of a stored value of this type.
  Alignment getFixedAlignment() const {
    return getBestKnownAlignment();
  }

  /// Returns the known, fixed size required to store a value of this type.
  Size getFixedSize() const {
    return Size(Bits.FixedTypeInfo.Size);
  }

  /// Returns the (assumed fixed) stride of the storage for this
  /// object.  The stride is the storage size rounded up to the
  /// alignment; its practical use is that, in an array, it is the
  /// offset from the size of one element to the offset of the next.
  /// The stride is at least one, even for zero-sized types, like the empty
  /// tuple.
  Size getFixedStride() const {
    Size s = getFixedSize().roundUpToAlignment(getFixedAlignment());
    if (s.isZero())
      s = Size(1);
    return s;
  }
  
  /// Returns the fixed number of "extra inhabitants" (that is, bit
  /// patterns that don't represent valid values of the type) in the type
  /// representation.
  virtual unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const {
    return getSpareBitExtraInhabitantCount();
  }

  /// Returns the number of extra inhabitants available by exercising spare
  /// bits.
  unsigned getSpareBitExtraInhabitantCount() const;

  /// We can statically determine the presence of extra inhabitants for fixed
  /// types.
  bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
    return getFixedExtraInhabitantCount(IGM) > 0;
  }

  /// Get the bit mask that must be applied before testing an extra inhabitant.
  virtual APInt getFixedExtraInhabitantMask(IRGenModule &IGM) const {
    return APInt::getAllOnes(getFixedSize().getValueInBits());
  }

  /// Create a constant of the given bit width holding one of the extra
  /// inhabitants of the type.
  /// The index must be less than the value returned by
  /// getFixedExtraInhabitantCount().
  virtual APInt getFixedExtraInhabitantValue(IRGenModule &IGM,
                                             unsigned bits,
                                             unsigned index) const {
    return getSpareBitFixedExtraInhabitantValue(IGM, bits, index);
  }
  
  /// Create an extra inhabitant constant using the spare bits of the type.
  APInt getSpareBitFixedExtraInhabitantValue(IRGenModule &IGM,
                                             unsigned bits,
                                             unsigned index) const;
  
  /// Map an extra inhabitant representation in memory to a unique 31-bit
  /// identifier, and map a valid representation of the type to -1.
  virtual llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF,
                                               Address src, SILType T,
                                               bool isOutlined) const {
    return getSpareBitExtraInhabitantIndex(IGF, src);
  }
  
  /// Map an extra inhabitant representation derived from spare bits to an
  /// index.
  llvm::Value *getSpareBitExtraInhabitantIndex(IRGenFunction &IGF,
                                               Address src) const;
  
  /// Store the extra inhabitant representation indexed by a 31-bit identifier
  /// to memory.
  virtual void storeExtraInhabitant(IRGenFunction &IGF,
                                    llvm::Value *index,
                                    Address dest, SILType T,
                                    bool isOutlined) const {
    storeSpareBitExtraInhabitant(IGF, index, dest);
  }
  
  /// Store the indexed spare-bit-derived extra inhabitant to memory.
  void storeSpareBitExtraInhabitant(IRGenFunction &IGF,
                                    llvm::Value *index,
                                    Address dest) const;
  
  /// Get the spare bit mask for the type.
  const SpareBitVector &getSpareBits() const { return SpareBits; }
  
  /// True if the type representation has statically "spare" unused bits.
  bool hasFixedSpareBits() const {
    return SpareBits.any();
  }
  
  /// Applies the fixed spare bits mask for this type to the given BitVector,
  /// clearing any bits used by valid representations of the type.
  ///
  /// If the bitvector is empty or smaller than this type, it is grown and
  /// filled with bits direct from the spare bits mask. If the bitvector is
  /// larger than this type, the trailing bits are untouched.
  ///
  /// The intent is that, for all the data types of an enum, you should be able
  /// to do this:
  ///
  ///   SpareBitVector spareBits;
  ///   for (EnumElementDecl *elt : u->getAllElements())
  ///     getFragileTypeInfo(elt->getArgumentType())
  ///       .applyFixedSpareBitsMask(IGM, spareBits);
  ///
  /// and end up with a spare bits mask for the entire enum.
  void applyFixedSpareBitsMask(const IRGenModule &IGM,
                               SpareBitVector &mask) const;

  void collectMetadataForOutlining(OutliningMetadataCollector &collector,
                                   SILType T) const override {
    // We assume that fixed type infos generally do not require type
    // metadata in order to perform value operations.
  }

  llvm::Value *getEnumTagSinglePayload(IRGenFunction &IGF,
                                       llvm::Value *numEmptyCases,
                                       Address enumAddr,
                                       SILType T,
                                       bool isOutlined) const override;

  void storeEnumTagSinglePayload(IRGenFunction &IGF, llvm::Value *whichCase,
                                 llvm::Value *numEmptyCases, Address enumAddr,
                                 SILType T, bool isOutlined) const override;

  static bool classof(const FixedTypeInfo *type) { return true; }
  static bool classof(const TypeInfo *type) { return type->isFixedSize(); }
};

llvm::Value *getFixedTypeEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *numEmptyCases, Address enumAddr,
    llvm::Value *size, Size fixedSize, unsigned fixedExtraInhabitantCount,
    llvm::function_ref<llvm::Value *(Address)> getExtraInhabitantIndex,
    bool isOutlined);

llvm::Value *getFixedTypeEnumTagSinglePayload(IRGenFunction &IGF,
                                              const FixedTypeInfo &fixedTI,
                                              llvm::Value *numEmptyCases,
                                              Address enumAddr,
                                              SILType T, bool isOutlined);
void storeFixedTypeEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *whichCase, llvm::Value *numEmptyCases,
    Address enumAddr, llvm::Value *size, Size fixedSize,
    unsigned fixedExtraInhabitantCount,
    llvm::function_ref<void(llvm::Value *, Address)> storeExtraInhabitant,
    bool isOutlined);

llvm::Value *emitLoad1to4Bytes(IRGenFunction &IGF, Address from,
                               llvm::Value *size);
void emitStore1to4Bytes(IRGenFunction &IGF, Address to, llvm::Value *val,
                        llvm::Value *size);

llvm::Value *emitGetTag(IRGenFunction &IGF, Address from, llvm::Value *size);
void emitSetTag(IRGenFunction &IGF, Address to, llvm::Value *val,
                llvm::Value *size);

void storeFixedTypeEnumTagSinglePayload(IRGenFunction &IGF,
                                        const FixedTypeInfo &fixedTI,
                                        llvm::Value *index,
                                        llvm::Value *numEmptyCases,
                                        Address enumAddr,
                                        SILType T, bool isOutlined);

}
}

#endif
