//===--- BitPatternBuilder.h - Create masks for composite types -----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#pragma once

#include "swift/Basic/ClusteredBitVector.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {
namespace irgen {

/// BitPatternBuilder is a class to help with the construction of
/// bit masks for composite types. The class should be constructed
/// using the byte order of the target platform. Elements added
/// to the class must mask an entire element in a composite type.
/// These elements must be byte aligned. For example, if you want
/// to add a 64-bit integer to the bit pattern with the low 32
/// bits set to 1 and the high 32 bits set to 0 then you must create
/// a 64-bit APInt and append it in its entirety rather than calling
/// the appendSetBits and appendClearBits helper functions which
/// would not be portable across architectures using different byte
/// orders.
///
/// Example construction of a mask for a struct:
///
///   // Type T that we are generating a mask for:
///   struct T {
///     uint8_t a;
///     uint8_t b;
///     uint16_t c;
///   };
///
///   // Code to generate the mask:
///   auto mask = BitPatternBuilder(isLittleEndian());
///   mask.appendSetBits(8);             // mask T.a with 0xff
///   mask.appendClearBits(8);           // mask T.b with 0x00
///   mask.append(APInt(16, 0x1177ULL)); // mask T.c with 0x1177
///
///   // Little-endian result:
///   mask.build(); // 0x117700ff [ ff 00 77 11 ]
///
///   // Big-endian result:
///   mask.build(); // 0xff001177 [ ff 00 11 77 ]
///
class BitPatternBuilder {
  using APInt = llvm::APInt;

  // An array of masks that, when combined, will form the mask for a
  // composite value. Generally these correspond to elements in a
  // struct (or class, tuple etc.).
  llvm::SmallVector<APInt, 8> Elements;

  // Little-endian byte order implies that elements should be
  // appended to the most significant bit. If this flag is false
  // then elements should be appended to the least signficant
  // bit (big-endian byte order).
  bool LittleEndian;

  // The combined size of the elements added so far in bits.
  unsigned Size = 0;
public:
  /// Create a new BitPatternBuilder with either a little-endian
  /// (true) or big-endian (false) byte order.
  BitPatternBuilder(bool littleEndian) : LittleEndian(littleEndian) {}

  /// Append the given mask to the bit pattern. The mask should mask
  /// an entire element type and be byte aligned.
  void append(const APInt &value) {
    assert(value.getBitWidth() % 8 == 0);
    Size += value.getBitWidth();
    Elements.push_back(value);
  }

  /// Append the given mask to the bit pattern. The mask should mask
  /// an entire element type and be byte aligned.
  void append(APInt &&value) {
    assert(value.getBitWidth() % 8 == 0);
    Size += value.getBitWidth();
    Elements.push_back(std::move(value));
  }

  /// Append the given mask to the bit pattern. The mask should mask
  /// an entire element type and be byte aligned.
  void append(const ClusteredBitVector &value) {
    assert(value.size() % 8 == 0);
    if (!value.empty()) {
      Size += value.size();
      Elements.push_back(value.asAPInt());
    }
  }

  /// Append the given number of set (1) bits to the bit pattern. The
  /// number of bits must be a multiple of 8 and mask a whole number
  /// of element types.
  void appendSetBits(unsigned numBits) {
    assert(numBits % 8 == 0);
    if (numBits) {
      Size += numBits;
      Elements.push_back(APInt::getAllOnes(numBits));
    }
  }

  /// Append the given number of clear (0) bits to the bit pattern. The
  /// number of bits must be a multiple of 8 and mask a whole number
  /// of element types.
  void appendClearBits(unsigned numBits) {
    assert(numBits % 8 == 0);
    if (numBits) {
      Size += numBits;
      Elements.push_back(APInt::getZero(numBits));
    }
  }

  /// Append set (1) bits to the bit pattern until it reaches the
  /// given size in bits. The total number of bits must be a
  /// multiple of 8.
  void padWithSetBitsTo(unsigned totalSizeInBits) {
    assert(totalSizeInBits % 8 == 0);
    assert(totalSizeInBits >= Size);
    appendSetBits(totalSizeInBits - Size);
  }

  /// Append clear (0) bits to the bit pattern until it reaches the
  /// given size in bits. The total number of bits must be a
  /// multiple of 8.
  void padWithClearBitsTo(unsigned totalSizeInBits) {
    assert(totalSizeInBits % 8 == 0);
    assert(totalSizeInBits >= Size);
    appendClearBits(totalSizeInBits - Size);
  }

  /// Build the complete mask for the composite type. If the mask has a
  /// length of 0 then the optional will not contain a value. Otherwise
  /// the option will contain a value that is the combined length of
  /// the elements appended to the builder. The mask represents is an
  /// integer in the target byte order.
  llvm::Optional<APInt> build() const {
    if (Size == 0) {
      return llvm::Optional<APInt>();
    }
    auto result = APInt::getZero(Size);
    unsigned offset = 0;
    for (const auto &e : Elements) {
      unsigned index = offset;
      if (!LittleEndian) {
        index = Size - offset - e.getBitWidth();
      }
      result.insertBits(e, index);
      offset += e.getBitWidth();
    }
    assert(offset == Size);
    return result;
  }
};

} // end namespace irgen
} // end namespace swift

