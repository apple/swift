//===--- ClusteredBitVector.h - A size-optimized bit vector -----*- C++ -*-===//
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
// This file defines the ClusteredBitVector class, a bitset data
// structure.
//
// For example, this would be reasonable to use to describe the
// unoccupied bits in a memory layout.
//
// Primary mutators:
//   - appending another vector to this vector
//   - appending a constant vector (<0,0,...,0> or <1,1,...,1>) to this vector
//
// Primary observers:
//   - testing a specific bit
//   - converting to llvm::APInt
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_CLUSTEREDBITVECTOR_H
#define SWIFT_BASIC_CLUSTEREDBITVECTOR_H

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/Optional.h"
#include "swift/Basic/Debug.h"
#include <cassert>

namespace swift {

/// A vector of bits.
class ClusteredBitVector {
  using APInt = llvm::APInt;

  /// Represents the bit vector as an integer.
  /// The least-significant bit of the integer corresponds to the bit
  /// at index 0. If the optional does not have a value then the bit
  /// vector has a length of 0 bits.
  llvm::Optional<APInt> Bits;

  /// Copy constructor from APInt.
  ClusteredBitVector(const APInt &bits) : Bits(bits) {}

  /// Move constructor from APInt.
  ClusteredBitVector(APInt &&bits) : Bits(std::move(bits)) {}
public:
  /// Create a new bit vector of zero length.  This does not perform
  /// any allocations.
  ClusteredBitVector() = default;

  ClusteredBitVector(const ClusteredBitVector &other)
    : Bits(other.Bits) {}

  ClusteredBitVector(ClusteredBitVector &&other)
    : Bits(std::move(other.Bits)) {}

  /// Create a new ClusteredBitVector from the provided APInt,
  /// with a size of 0 if the optional does not have a value.
  ClusteredBitVector(const llvm::Optional<APInt> &bits)
    : Bits(bits) {}

  /// Create a new ClusteredBitVector from the provided APInt,
  /// with a size of 0 if the optional does not have a value.
  ClusteredBitVector(llvm::Optional<APInt> &&bits)
    : Bits(std::move(bits)) {}

  ClusteredBitVector &operator=(const ClusteredBitVector &other) {
    this->Bits = other.Bits;
    return *this;
  }

  ClusteredBitVector &operator=(ClusteredBitVector &&other) {
    this->Bits = std::move(other.Bits);
    return *this;
  }

  ~ClusteredBitVector() = default;

  /// Return true if this vector is zero-length (*not* if it does not
  /// contain any set bits).
  bool empty() const {
    return !Bits.has_value();
  }

  /// Return the length of this bit-vector.
  size_t size() const {
    return Bits ? Bits.value().getBitWidth() : 0;
  }

  /// Append the bits from the given vector to this one.
  void append(const ClusteredBitVector &other) {
    // Nothing to do if the other vector is empty.
    if (!other.Bits) {
      return;
    }
    if (!Bits) {
      Bits = other.Bits;
      return;
    }
    APInt &v = Bits.value();
    unsigned w = v.getBitWidth();
    v = v.zext(w + other.Bits.value().getBitWidth());
    v.insertBits(other.Bits.value(), w);
    return;
  }

  /// Add the low N bits from the given value to the vector.
  void add(size_t numBits, uint64_t value) {
    append(fromAPInt(APInt(numBits, value)));
  }

  /// Append a number of clear bits to this vector.
  void appendClearBits(size_t numBits) {
    if (numBits == 0) {
      return;
    }
    if (Bits) {
      APInt &v = Bits.value();
      v = v.zext(v.getBitWidth() + numBits);
      return;
    }
    Bits = APInt::getZero(numBits);
  }

  /// Extend the vector out to the given length with clear bits.
  void extendWithClearBits(size_t newSize) {
    assert(newSize >= size());
    appendClearBits(newSize - size());
  }

  /// Append a number of set bits to this vector.
  void appendSetBits(size_t numBits) {
    if (numBits == 0) {
      return;
    }
    if (Bits) {
      APInt &v = Bits.value();
      unsigned w = v.getBitWidth();
      v = v.zext(w + numBits);
      v.setBitsFrom(w);
      return;
    }
    Bits = APInt::getAllOnes(numBits);
    return;
  }

  /// Extend the vector out to the given length with set bits.
  void extendWithSetBits(size_t newSize) {
    assert(newSize >= size());
    appendSetBits(newSize - size());
  }

  /// Test whether a particular bit is set.
  bool operator[](size_t i) const {
    assert(i < size());
    return Bits.value()[i];
  }

  /// Intersect a bit-vector of the same size into this vector.
  ClusteredBitVector &operator&=(const ClusteredBitVector &other) {
    assert(size() == other.size());
    if (Bits) {
      APInt &v = Bits.value();
      v &= other.Bits.value();
    }
    return *this;
  }

  /// Union a bit-vector of the same size into this vector.
  ClusteredBitVector &operator|=(const ClusteredBitVector &other) {
    assert(size() == other.size());
    if (Bits) {
      APInt &v = Bits.value();
      v |= other.Bits.value();
    }
    return *this;
  }

  /// Set bit i.
  void setBit(size_t i) {
    assert(i < size());
    Bits.value().setBit(i);
  }

  /// Clear bit i.
  void clearBit(size_t i) {
    assert(i < size());
    Bits.value().clearBit(i);
  }

  /// Toggle bit i.
  void flipBit(size_t i) {
    assert(i < size());
    Bits.value().flipBit(i);
  }

  /// Toggle all the bits in this vector.
  void flipAll() {
    if (Bits) {
      Bits.value().flipAllBits();
    }
  }

  /// Set the length of this vector to zero.
  void clear() {
    Bits.reset();
  }

  /// Count the number of set bits in this vector.
  size_t count() const {
    return Bits ? Bits.value().countPopulation() : 0;
  }

  /// Determine if there are any bits set in this vector.
  bool any() const {
    return Bits && Bits.value() != 0;
  }

  /// Determine if there are no bits set in this vector.
  ///
  /// \return \c !any()
  bool none() const {
    return !any();
  }

  friend bool operator==(const ClusteredBitVector &lhs,
                         const ClusteredBitVector &rhs) {
    if (lhs.size() != rhs.size()) {
      return false;
    }
    if (lhs.size() == 0) {
      return true;
    }
    return lhs.Bits.value() == rhs.Bits.value();
  }
  friend bool operator!=(const ClusteredBitVector &lhs,
                         const ClusteredBitVector &rhs) {
    return !(lhs == rhs);
  }

  /// Return this bit-vector as an APInt, with low indices becoming
  /// the least significant bits of the number.
  APInt asAPInt() const {
    // Return 1-bit wide zero APInt for a 0-bit vector.
    return Bits ? Bits.value() : APInt();
  }

  /// Construct a bit-vector from an APInt.
  static ClusteredBitVector fromAPInt(const APInt &value) {
    return ClusteredBitVector(value);
  }

  /// Construct a bit-vector from an APInt.
  static ClusteredBitVector fromAPInt(APInt &&value) {
    return ClusteredBitVector(std::move(value));
  }

  /// Return a constant bit-vector of the given size.
  static ClusteredBitVector getConstant(size_t numBits, bool value) {
    if (numBits == 0) {
      return ClusteredBitVector();
    }
    auto vec = APInt::getZero(numBits);
    if (value) {
      vec.flipAllBits();
    }
    return ClusteredBitVector(vec);
  }

  /// Pretty-print the vector.
  void print(llvm::raw_ostream &out) const;
  SWIFT_DEBUG_DUMP;
};

} // end namespace swift

#endif // SWIFT_BASIC_CLUSTEREDBITVECTOR_H
