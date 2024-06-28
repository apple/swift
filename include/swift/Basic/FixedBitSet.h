//===- llvm/ADT/FixedBitSet.h - Fixed-length bitset -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the FixedBitSet template, which is basically
// just std::bitset (a fixed-size inline-allocated bit vector) but with:
//
// - a cleaner interface for using the type to implement a set,
//   especially a set of an enum type
//
// - a more useful set of operations, such as the ability to iterate
//   over the set instead of scanning over all possible elements
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ADT_FIXEDBITSET_H
#define LLVM_ADT_FIXEDBITSET_H

#include <assert.h>
#include <inttypes.h>
#include <initializer_list>
#include "llvm/Support/MathExtras.h"

namespace swift {

namespace detail {
// In principle, long sets would be happier if we chunked
// at the pointer size instead of capping at 32, but we expect this
// to be used with relatively short Sets where larger chunks
// would introduce more padding.
template <size_t numElements,
          bool fitsInUInt8 = (numElements <= 8),
          bool fitsInUInt16 = (numElements <= 16)>
struct FixedBitSetStorageType;

template <size_t numElements>
struct FixedBitSetStorageType<numElements, true, true> {
  using type = uint8_t;
};

template <size_t numElements>
struct FixedBitSetStorageType<numElements, false, true> {
  using type = uint16_t;
};

template <size_t numElements>
struct FixedBitSetStorageType<numElements, false, false> {
  using type = uint32_t;
};

} // end namespace detail

/// A set of integral elements, all of which must be less than
/// numElements.  Iteration produces elements in a sorted
/// (numerically increasing) order.
template <size_t numElements, class ValueType = size_t>
class FixedBitSet {
  static_assert(std::is_integral<ValueType>::value ||
                std::is_enum<ValueType>::value,
                "value type is not an integer or enum type");

  using ChunkType = typename detail::FixedBitSetStorageType<numElements>::type;

  static constexpr size_t chunkSize = CHAR_BIT * sizeof(ChunkType);
  static constexpr size_t numChunks =
    (numElements + chunkSize - 1) / chunkSize;

  /// We represent the elements as an inline array of chunks, with
  /// earlier chunks representing lower indices.  Any padding bits
  /// in the last chunk (if the number of elements isn't an even
  /// multiple of the chunk size) are always clear.
  ChunkType chunks[numChunks] = {};

  static size_t chunkIndex(ValueType i) {
    return size_t(i) / chunkSize;
  }
  static ChunkType chunkMask(ValueType i) {
    return ChunkType(1) << (size_t(i) % chunkSize);
  }

public:
  /// Build an empty set.
  constexpr FixedBitSet() {}

  /// Build a set containing the given elements.
  FixedBitSet(std::initializer_list<ValueType> elements) {
    for (const auto &elt : elements)
      insert(elt);
  }

  /// Build a set filled with all possible elements, i.e., a "full" set.
  static FixedBitSet full() {
    FixedBitSet set;
    set.insertAll();
    return set;
  }

  /// Return true if the set is empty.
  bool empty() const {
    for (auto chunk : chunks)
      if (chunk != 0) return false;
    return true;
  }

  /// Return whether the given element is present in the set.
  bool contains(ValueType i) const {
    assert(size_t(i) < numElements);
    return chunks[chunkIndex(i)] & chunkMask(i);
  }

  /// Either insert or remove the given element.
  void insertOrRemove(ValueType i, bool shouldInsert) {
    if (shouldInsert)
      insert(i);
    else
      remove(i);
  }

  /// Insert the given element.
  void insert(ValueType i) {
    assert(size_t(i) < numElements);
    chunks[chunkIndex(i)] |= chunkMask(i);
  }

  /// Remove the given element from the set.
  void remove(ValueType i) {
    assert(size_t(i) < numElements);
    chunks[chunkIndex(i)] &= ~chunkMask(i);
  }

  /// Add every element in the range to this set.
  void insertAll() {
    // Our invariant is that any padding bits are clear, so
    // we need to set bits in the most significant chunk only
    // for the bits that are set.
    constexpr size_t partialBits = (numElements % chunkSize);
    constexpr size_t firstIncompleteChunk =
      partialBits == 0 ? numChunks : numChunks - 1;

    for (size_t i = 0; i != firstIncompleteChunk; ++i)
      chunks[i] = ~ChunkType(0);

    if (partialBits != 0)
      chunks[numChunks - 1] = (ChunkType(1) << partialBits) - 1;
  }

  /// Remove all of the elements in this set.
  void removeAll() {
    for (size_t i = 0; i != numChunks; ++i)
      chunks[i] = 0;
  }

  /// Add all of the elements in the given set.
  void insertAll(const FixedBitSet &other) {
    for (size_t i = 0; i != numChunks; ++i) {
      chunks[i] |= other.chunks[i];
    }
  }

  /// Remove all of the elements that aren't in the given set.
  void removeAllExcept(const FixedBitSet &other) {
    for (size_t i = 0; i != numChunks; ++i) {
      chunks[i] &= other.chunks[i];
    }
  }

  /// Remove all of the elements that are also in the given set.
  void removeAll(const FixedBitSet &other) {
    for (size_t i = 0; i != numChunks; ++i) {
      chunks[i] &= ~other.chunks[i];
    }
  }

  class iterator {
    const ChunkType *chunks;
    size_t chunkIndex;

    /// Our possibly-edited copy of the current chunk.  As we iterate
    /// past elements, we clear the corresponding bit here and then find
    /// the next chunk that has a bit set.  The invariant is that either
    /// this is non-zero or chunkIndex == numChunks.
    size_t remainingChunk;

    friend class FixedBitSet;

    // Constructor for begin().
    iterator(const ChunkType *chunks, size_t chunkIndex,
             size_t remainingChunk)
      : chunks(chunks), chunkIndex(chunkIndex),
        remainingChunk(remainingChunk) {
      advance();
    }

    /// Constructor for end().
    iterator(const ChunkType *chunks)
      : chunks(chunks), chunkIndex(numChunks), remainingChunk(0) {}

    /// Find the next element, if any, or else set chunkIndex to numChunks.
    void advance() {
      while (!remainingChunk) {
        assert(chunkIndex < numChunks);
        if (++chunkIndex == numChunks) break;
        remainingChunk = chunks[chunkIndex];
      }
    }

  public:
    iterator &operator++() {
      assert(remainingChunk && "incrementing a completed iterator");
      //  rc     = aaaaaaaa100
      //  rc - 1 = aaaaaaaa011
      remainingChunk &= (remainingChunk - 1);
      advance();
      return *this;
    }
    iterator operator++(int) {
      iterator copy = *this;
      ++*this;
      return copy;
    }

    ValueType operator*() const {
      assert(remainingChunk && "dereferencing a completed iterator");
      return ValueType(chunkIndex * chunkSize +
                       llvm::countr_zero(remainingChunk));
    }

    bool operator==(const iterator &other) const {
      assert(chunks == other.chunks &&
             "comparing iterators from different bit Sets");
      return chunkIndex == other.chunkIndex
          && remainingChunk == other.remainingChunk;
    }
    bool operator!=(const iterator &other) const {
      return !(*this == other);
    }
  };

  iterator begin() const {
    return iterator(chunks, 0, chunks[0]);
  }
  iterator end() const {
    return iterator(chunks);
  }

  bool operator==(const FixedBitSet &other) const {
    for (size_t i = 0; i != numChunks; ++i)
      if (chunks[i] != other.chunks[i])
        return false;
    return true;
  }
  bool operator!=(const FixedBitSet &other) const {
    return !(*this == other);
  }
};

}  // end namespace swift

#endif
