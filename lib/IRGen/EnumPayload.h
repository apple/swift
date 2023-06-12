//===--- EnumPayload.h - Payload management for 'enum' Types ----*- C++ -*-===//
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

#ifndef SWIFT_IRGEN_ENUMPAYLOAD_H
#define SWIFT_IRGEN_ENUMPAYLOAD_H

#include "IRGenModule.h"
#include "Explosion.h"
#include "TypeInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/ADT/PointerEmbeddedInt.h"
#include "llvm/ADT/PointerUnion.h"
#include <utility>

namespace swift {
namespace irgen {
  
/// A description of how to represent an enum payload as a value.
/// A payload can either use a generic word-chunked representation,
/// or attempt to follow the explosion schema of one of its payload
/// types.
/// TODO: the current code only ever uses the generic word-chunked
/// representation, it might be better if it used an appropriate
/// explosion schema instead.
class EnumPayloadSchema {
  // A size in bits less than 0 indicates that the payload size is
  // dynamic.
  const int64_t BitSize;
public:
  /// Create a new schema with a dynamic size.
  EnumPayloadSchema() : BitSize(-1) {}

  /// Create a new schema with the given fixed size in bits.
  explicit EnumPayloadSchema(unsigned bits)
    : BitSize(static_cast<int64_t>(bits)) {}

  /// Report whether the schema has a fixed size.
  explicit operator bool() const {
    return BitSize >= 0;
  }

  /// Invoke a functor for each element type in the schema.
  template<typename TypeFn /* void(llvm::Type *schemaType) */>
  void forEachType(IRGenModule &IGM, TypeFn &&fn) const {
    assert(BitSize >= 0 && "payload size must not be dynamic");

    // Chunk into pointer-sized integer values.
    int64_t bitSize = BitSize;
    int64_t pointerSize = IGM.getPointerSize().getValueInBits();

    while (bitSize >= pointerSize) {
      fn(IGM.SizeTy);
      bitSize -= pointerSize;
    }
    if (bitSize > 0)
      fn(llvm::IntegerType::get(IGM.getLLVMContext(), bitSize));
  }
};

/// Is a switch default destination unreachable?
enum IsUnreachable_t: bool {
  IsNotUnreachable = false,
  IsUnreachable = true,
};

using SwitchDefaultDest
  = llvm::PointerIntPair<llvm::BasicBlock*, 1, IsUnreachable_t>;

/// An enum payload value. The payload is represented as an explosion of
/// integers and pointers that together represent the bit pattern of
/// the payload.
class EnumPayload {
public:
  /// A value, or the type of a zero value in the payload.
  using LazyValue = llvm::PointerUnion<llvm::Value *, llvm::Type *>;
  
  mutable SmallVector<LazyValue, 2> PayloadValues;
  mutable llvm::Type *StorageType = nullptr;
  
  EnumPayload() = default;

  /// Generate a "zero" enum payload.
  static EnumPayload zero(IRGenModule &IGM,
                          EnumPayloadSchema schema);

  /// Generate an enum payload containing the given bit pattern.
  static EnumPayload fromBitPattern(IRGenModule &IGM,
                                    const APInt &bitPattern,
                                    EnumPayloadSchema schema);

  /// Insert a value into the enum payload.
  ///
  /// The current payload value at the given offset is assumed to be zero.
  /// If \p numBitsUsedInValue is non-negative denotes the actual number of bits
  /// that need storing in \p value otherwise the full bit-width of \p value
  /// will be stored.
  void insertValue(IRGenModule &IGM,
                   IRBuilder &builder,
                   llvm::Value *value, unsigned bitOffset);
  
  /// Extract a value from the enum payload.
  llvm::Value *extractValue(IRGenFunction &IGF,
                            llvm::Type *type, unsigned bitOffset) const;
  
  /// Take an enum payload out of an explosion.
  static EnumPayload fromExplosion(IRGenModule &IGM,
                                   Explosion &in,
                                   EnumPayloadSchema schema);
  
  /// Add the payload to an explosion.
  void explode(IRGenModule &IGM, Explosion &out) const;
  
  /// Pack into another enum payload.
  void packIntoEnumPayload(IRGenModule &IGM,
                           IRBuilder &builder,
                           EnumPayload &dest,
                           unsigned bitOffset) const;
  
  /// Unpack from another enum payload.
  static EnumPayload unpackFromEnumPayload(IRGenFunction &IGF,
                                           const EnumPayload &src,
                                           unsigned bitOffset,
                                           EnumPayloadSchema schema);
  
  /// Load an enum payload from memory.
  static EnumPayload load(IRGenFunction &IGF, Address address,
                          EnumPayloadSchema schema);
  
  /// Store an enum payload to memory.
  void store(IRGenFunction &IGF, Address address) const;

  /// Emit a switch over specific bit patterns for the payload.
  /// The value will be tested as if AND-ed against the given mask.
  void emitSwitch(IRGenFunction &IGF,
                  const APInt &mask,
                  ArrayRef<std::pair<APInt, llvm::BasicBlock*>> cases,
                  SwitchDefaultDest dflt) const;
  
  /// Emit an equality comparison operation that payload & mask == value.
  llvm::Value *emitCompare(IRGenFunction &IGF,
                           const APInt &mask,
                           const APInt &value) const;
  
  /// Apply an AND mask to the payload.
  void emitApplyAndMask(IRGenFunction &IGF, const APInt &mask);
  
  /// Apply an OR mask to the payload.
  void emitApplyOrMask(IRGenModule &IGM,
                       IRBuilder &builder, const APInt &mask);
  
  /// Apply an OR mask to the payload.
  void emitApplyOrMask(IRGenFunction &IGF, EnumPayload mask);

  /// Scatter the bits in value to the bit positions indicated by the
  /// mask. The new bits are added using OR, so an emitApplyAndMask
  /// call should be used first if existing bits need to be cleared.
  void emitScatterBits(IRGenModule &IGM,
                       IRBuilder &builder,
                       const APInt &mask,
                       llvm::Value *value);

  /// Gather bits from an enum payload based on a spare bit mask.
  llvm::Value *emitGatherSpareBits(IRGenFunction &IGF,
                                   const SpareBitVector &spareBits,
                                   unsigned firstBitOffset,
                                   unsigned bitWidth) const;

  void print(llvm::raw_ostream &OS);
  void dump();

private:
  /// Calculate the total number of bits this payload requires.
  /// This will always be a multiple of 8.
  unsigned getAllocSizeInBits(const llvm::DataLayout &DL) const;
};
  
}
}

#endif
