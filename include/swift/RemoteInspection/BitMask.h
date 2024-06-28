//===--- Bitmask.h - Swift Bitmask type for Reflection ----*- C++ -*-===//
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
// Used by TypeLowering logic to compute masks for in-memory representations
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_REFLECTION_BITMASK_H
#define SWIFT_REFLECTION_BITMASK_H

#include "swift/Remote/MemoryReader.h"
#include <sstream>

namespace swift {
namespace reflection {

// A variable-length bitmap used to track "spare bits" for general multi-payload
// enums.  Note:  These are not arbitrary-sized!  They are always a multiple
// of 8 bits in size, and always aligned on an 8-bit boundary.
class BitMask {
  static constexpr unsigned maxSize = 128 * 1024 * 1024; // 128MB

  unsigned size; // Size of mask _in bytes_
  uint8_t *mask;
public:
  ~BitMask() {
    free(mask);
  }
private:
  // Construct a bitmask of the appropriate number of bytes
  // initialized to all bits set
  BitMask(unsigned sizeInBytes = 0): size(sizeInBytes) {
    assert(size < maxSize && "Trying to build a too-large bitmask");
    if (size > maxSize || size == 0) {
      size = 0;
      mask = nullptr;
      return;
    }

    mask = (uint8_t *)malloc(size);

    if (!mask) {
      // Malloc might fail if size is large due to some bad data. Assert in
      // asserts builds, and fail gracefully in non-asserts builds by
      // constructing an empty BitMask.
      assert(false && "Failed to allocate BitMask");
      size = 0;
      return;
    }

    memset(mask, 0xff, size);
  }

public:
  static BitMask zeroMask(unsigned sizeInBytes) {
    auto mask = BitMask(sizeInBytes);
    mask.makeZero();
    return mask;
  }

  static BitMask oneMask(unsigned sizeInBytes) {
    auto mask = BitMask(sizeInBytes);
    return mask;
  }

  BitMask(unsigned sizeInBytes, uint64_t sourceMask): size(sizeInBytes) {
    mask = (uint8_t *)calloc(1, sizeInBytes);
    if (!mask) {
      assert(false && "Failed to allocate Bitmask");
      size = 0;
      return;
    }
    size_t toCopy = sizeInBytes;
    if (toCopy > sizeof(sourceMask)) {
      toCopy = sizeof(sourceMask);
    }
    memcpy(mask, &sourceMask, toCopy);
  }

  // Construct a bitmask of the appropriate number of bytes
  // initialized with bits from the specified buffer
  BitMask(unsigned sizeInBytes, const uint8_t *initialValue,
          unsigned initialValueBytes, unsigned offset)
      : size(sizeInBytes) {
    // Gracefully fail by constructing an empty mask if we exceed the size
    // limit.
    if (size > maxSize) {
      size = 0;
      mask = nullptr;
      return;
    }

    // Bad data could cause the initial value location to be off the end of our
    // size. If initialValueBytes + offset is beyond sizeInBytes (or overflows),
    // assert in asserts builds, and fail gracefully in non-asserts builds by
    // constructing an empty BitMask.
    bool overflowed = false;
    unsigned initialValueEnd =
        llvm::SaturatingAdd(initialValueBytes, offset, &overflowed);
    if (overflowed) {
      assert(false && "initialValueBytes + offset overflowed");
      size = 0;
      mask = nullptr;
      return;
    }
    assert(initialValueEnd <= sizeInBytes);
    if (initialValueEnd > size) {
      assert(false && "initialValueBytes + offset is greater than size");
      size = 0;
      mask = nullptr;
      return;
    }

    mask = (uint8_t *)calloc(1, size);

    if (!mask) {
      // Malloc might fail if size is large due to some bad data. Assert in
      // asserts builds, and fail gracefully in non-asserts builds by
      // constructing an empty BitMask.
      assert(false && "Failed to allocate BitMask");
      size = 0;
      return;
    }

    memcpy(mask + offset, initialValue, initialValueBytes);
  }
  // Move constructor moves ownership and zeros the src
  BitMask(BitMask&& src) noexcept: size(src.size), mask(std::move(src.mask)) {
    src.size = 0;
    src.mask = nullptr;
  }
  // Copy constructor makes a copy of the mask storage
  BitMask(const BitMask& src) noexcept: size(src.size), mask(nullptr) {
    mask = (uint8_t *)malloc(size);
    memcpy(mask, src.mask, size);
  }

  std::string str() const {
    std::ostringstream buff;
    buff << size << ":0x";
    for (unsigned i = 0; i < size; i++) {
      buff << std::hex << ((mask[i] >> 4) & 0x0f) << (mask[i] & 0x0f);
    }
    return buff.str();
  }

  bool operator==(const BitMask& rhs) const {
    // The two masks may be of different sizes.
    // The common prefix must be identical.
    size_t common = std::min(size, rhs.size);
    if (memcmp(mask, rhs.mask, common) != 0)
      return false;
    // The remainder of the longer mask must be
    // all zero bits.
    unsigned mustBeZeroSize = std::max(size, rhs.size) - common;
    uint8_t *mustBeZero;
    if (size < rhs.size) {
      mustBeZero = rhs.mask + size;
    } else if (size > rhs.size) {
      mustBeZero = mask + rhs.size;
    }
    for (unsigned i = 0; i < mustBeZeroSize; ++i) {
      if (mustBeZero[i] != 0) {
        return false;
      }
    }
    return true;
  }

  bool operator!=(const BitMask& rhs) const {
    return !(*this == rhs);
  }

  bool isNonZero() const { return !isZero(); }

  bool isZero() const {
    for (unsigned i = 0; i < size; ++i) {
      if (mask[i] != 0) {
        return false;
      }
    }
    return true;
  }

  void makeZero() {
    memset(mask, 0, size * sizeof(mask[0]));
  }

  void complement() {
    for (unsigned i = 0; i < size; ++i) {
      mask[i] = ~mask[i];
    }
  }

  int countSetBits() const {
    static const int counter[] =
      {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    int bits = 0;
    for (unsigned i = 0; i < size; ++i) {
      bits += counter[mask[i] >> 4] + counter[mask[i] & 15];
    }
    return bits;
  }

  int countZeroBits() const {
    return (size * 8) - countSetBits();
  }

  // Treat the provided value as a mask, `and` it with
  // the part of the mask at the provided byte offset.
  // Bits outside the specified area are unchanged.
  template<typename IntegerType>
  void andMask(IntegerType value, unsigned byteOffset) {
    andMask((void *)&value, sizeof(value), byteOffset);
  }

  // As above, but using the provided bitmask instead
  // of an integer.
  void andMask(BitMask mask, unsigned offset) {
    andMask(mask.mask, mask.size, offset);
  }

  // As above, but using the complement of the
  // provided mask.
  void andNotMask(BitMask mask, unsigned offset) {
    if (offset < size) {
      andNotMask(mask.mask, mask.size, offset);
    }
  }

  // Zero all bits except for the `n` most significant ones.
  void keepOnlyMostSignificantBits(unsigned n) {
    if (size < 1) {
      return;
    }
#if defined(__BIG_ENDIAN__)
    assert(false && "Big endian not supported for readMaskedInteger");
#else
    unsigned count = 0;
    unsigned i = size;
    while (i > 0) {
      i -= 1;
      if (count < n) {
        for (int b = 128; b > 0; b >>= 1) {
          if (count >= n) {
            mask[i] &= ~b;
          } else if ((mask[i] & b) != 0) {
            ++count;
          }
        }
      } else {
        mask[i] = 0;
      }
    }
#endif
  }

  void keepOnlyLeastSignificantBytes(unsigned n) {
    if (size > n) {
      size = n;
    }
  }

  unsigned numBits() const {
    return size * 8;
  }

  unsigned numSetBits() const {
    unsigned count = 0;
    for (unsigned i = 0; i < size; ++i) {
      if (mask[i] != 0) {
        for (unsigned b = 1; b < 256; b <<= 1) {
          if ((mask[i] & b) != 0) {
            ++count;
          }
        }
      }
    }
    return count;
  }

  // Read a mask-sized area from the target and collect
  // the masked bits into a single integer.
  template<typename IntegerType>
   bool readMaskedInteger(remote::MemoryReader &reader,
                         remote::RemoteAddress address,
                         IntegerType *dest) const {
    auto data = reader.readBytes(address, size);
    if (!data) {
      return false;
    }
#if defined(__BIG_ENDIAN__)
    assert(false && "Big endian not supported for readMaskedInteger");
#else
    IntegerType result = 0;
    IntegerType resultBit = 1; // Start from least-significant bit
    auto bytes = static_cast<const uint8_t *>(data.get());
    for (unsigned i = 0; i < size; ++i) {
      for (unsigned b = 1; b < 256; b <<= 1) {
        if ((mask[i] & b) != 0) {
          if ((bytes[i] & b) != 0) {
            result |= resultBit;
          }
          resultBit <<= 1;
        }
      }
    }
    *dest = result;
    return true;
#endif
  }

private:
  void andMask(void *maskData, unsigned len, unsigned offset) {
    if (offset < size) {
      unsigned common = std::min(len, size - offset);
      uint8_t *maskBytes = (uint8_t *)maskData;
      for (unsigned i = 0; i < common; ++i) {
        mask[i + offset] &= maskBytes[i];
      }
    }
  }

  void andNotMask(void *maskData, unsigned len, unsigned offset) {
    assert(offset < size);
    if (offset < size) {
      unsigned common = std::min(len, size - offset);
      uint8_t *maskBytes = (uint8_t *)maskData;
      for (unsigned i = 0; i < common; ++i) {
        mask[i + offset] &= ~maskBytes[i];
      }
    }
  }
};

} // namespace reflection
} // namespace swift

#endif
