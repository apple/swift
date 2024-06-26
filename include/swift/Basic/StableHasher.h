//===--- StableHasher.h - Stable Hashing ------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// An implementation of a stable hashing for Swift.
//
// Derived from the reference implementation for SipHash 2-4:
//   https://github.com/veorq/SipHash
//
// With inline buffering derived from the hash implementation in the Swift
// Standard Library.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_STABLEHASHER_H
#define SWIFT_BASIC_STABLEHASHER_H

#include "llvm/Support/Endian.h"
#include "llvm/ADT/StringRef.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <tuple>
#include <utility>
#include <iterator>

namespace swift {

/// A \c StableHasher is an implementation of a 128-bit stable hash - built for
/// speed.
///
/// A "stable" hash in this context refers to the idea that the output of this
/// hasher is deterministic across instantiations of the compiler. In order to
/// support this goal, this hasher disallows run-dependent or otherwise
/// unstable values from entering into the hash-combiner. For example, this
/// hasher will statically reject attempts to hash-combine pointers and
/// aggregates of pointers. Note that this relies on user cooperation as well.
/// The order of hash-combines is pivotal, thus e.g. collection values should
/// have a guaranteed order or be sorted before being hash-combined.
///
/// Stable hash values must also be independent of the host architecture. For
/// integral types and enumerations, the default hash-combiner will
/// automatically byte-swap to a common little-endian format.
///
/// This hasher also allows for extending the hash-combiner to user-defined
/// types. To do so, define a (partial) specialization of
/// \c swift::StableHasher::Combiner<T>
///
///    template <typename T>
///    struct swift::StableHasher::Combiner<std::optional<T>> {
///      static void combine(StableHasher &hasher, const std::optional<T> &O) {
///        if (!O.has_value()) {
///          hasher.combine(0);
///        } else {
///          hasher.combine(1);
///          swift::StableHasher::Combiner<T>::combine(hasher, O.value());
///        }
///      }
///    };
///
/// The current implementation is the 128-bit (extended) SipHash 2-4, which
/// has been empirically demonstrated to have the best throughput relative to
/// the other SipHash tunings.
class StableHasher final {
private:
  struct State {
    uint64_t v0 = 0x736F6D6570736575;
    uint64_t v1 = 0x646F72616E646f6D;
    uint64_t v2 = 0x6C7967656E657261;
    uint64_t v3 = 0x7465646279746573;
  } state;

  // A buffer of up to 8 items that this hasher uses to amortize the cost
  // of the hashing function for hash-combines shorter than 64-bits.
  uint8_t byteBuffer[8] = {0};
  // msb                                                             lsb
  // +---------+-------+-------+-------+-------+-------+-------+-------+
  // |byteCount|               length (<= 56 bits)                     |
  // +---------+-------+-------+-------+-------+-------+-------+-------+
  uint64_t lengthAndByteCount = 0;

public:
  static StableHasher defaultHasher() { StableHasher hasher{0, 0}; return hasher; }

  explicit StableHasher(uint64_t leftSeed, uint64_t rightSeed) {
    state.v3 ^= rightSeed;
    state.v2 ^= leftSeed;
    state.v1 ^= rightSeed;
    state.v0 ^= leftSeed;

    state.v1 ^= 0xEE;
  }

public:
  template <typename T> struct Combiner {
    // static void combine(StableHasher &hasher, const T &Val);
  };

public:
  /// Consume this stable hasher and compute the final 128-bit stable hash value.
  std::pair<uint64_t, uint64_t> finalize() &&;

  template <uint64_t N> void combine(uint8_t (&bits)[N]) {
    static_assert(N > 0, "Cannot append VLA");
    static_assert(N <= 8, "Can only append up to 64 bits at a time");

    lengthAndByteCount += N;

    const uint64_t bufLen = getBufferLength();
    const uint64_t available = sizeof(byteBuffer) - bufLen;

    // Cram as many bytes into the buffer as we can.
    const uint64_t nhead = std::min(N, available);
    if (nhead == sizeof(byteBuffer)) {
      // We have headroom available for all 64 bits. Eagerly compress the
      // now-full buffer into our state.
      std::copy_n(bits, sizeof(byteBuffer), byteBuffer);
    } else if (N >= available) {
      // There was some excess - append as many bytes as we can hold and
      // compress the buffer into our state.
      std::copy_n(bits, nhead, byteBuffer + bufLen);
    } else {
      // We have headroom available for these bits.
      std::copy_n(bits, N, byteBuffer + bufLen);
      return setBufferLength(bufLen + N);
    }

    constexpr auto endian = llvm::support::endianness::little;
    compress(llvm::support::endian::read<uint64_t>(byteBuffer, endian));

    // Now reseed the buffer with the remaining bytes.
    const uint64_t remainder = N - available;
    std::copy_n(bits + available, remainder, byteBuffer);
    return setBufferLength(remainder);
  }

  template <
      typename T,
      typename std::enable_if<std::is_integral<T>::value>::type * = nullptr>
  void combine(T bits) {
    constexpr auto endian = llvm::support::endianness::little;
    uint8_t buf[sizeof(T)] = {0};
    bits = llvm::support::endian::byte_swap<T>(bits, endian);
    std::memcpy(buf, &bits, sizeof(T));
    combine<sizeof(T)>(buf);
  }

  template <
      typename EnumType,
      typename std::enable_if<std::is_enum<EnumType>::value>::type * = nullptr>
  void combine(EnumType value) {
    using Underlying = typename std::underlying_type<EnumType>::type;
    return this->template combine<Underlying>(static_cast<Underlying>(value));
  }

  template <typename T>
  auto combine(const T *ptr) -> decltype("Cannot hash-combine pointers!"){};

  template <typename T, typename... Ts>
  void combine(const T &arg, const Ts &... args) {
    return combine_many(arg, args...);
  }

  template <typename T, typename U> void combine(const std::pair<T, U> &arg) {
    return combine_many(arg.first, arg.second);
  }

  void combine(const std::basic_string<char> &arg) {
    return combine_range(arg.begin(), arg.end());
  }

  void combine(const std::basic_string<wchar_t> &arg) {
    return combine_range(arg.begin(), arg.end());
  }

  void combine(llvm::StringRef arg) {
    return combine_range(arg.begin(), arg.end());
  }

  template <typename T,
            decltype(StableHasher::Combiner<T>::combine) * = nullptr>
  void combine(const T &val) {
    return StableHasher::Combiner<T>::combine(*this, val);
  }

  template <typename ValueT> void combine_range(ValueT first, ValueT last) {
    combine(std::distance(first, last));
    while (first != last) {
      combine(*first++);
    }
  }

  template <typename... Ts> void combine(const std::tuple<Ts...> &arg) {
    return combine_tuple(arg, typename std::index_sequence_for<Ts...>{});
  }

private:
  template <typename... Ts, unsigned... Indices>
  void combine_tuple(const std::tuple<Ts...> &arg,
                     std::index_sequence<Indices...> indices) {
    return combine_many(hash_value(std::get<Indices>(arg))...);
  }

  // base case.
  void combine_many() {}

  // recursive case
  template <typename T, typename... Ts>
  void combine_many(const T &arg, const Ts &... args) {
    combine(arg);
    return combine_many(args...);
  }

private:
  /// Return the number of bytes in the inline buffer.
  uint64_t getBufferLength() const { return lengthAndByteCount >> 56; }
  /// Set the number of bytes in the inline buffer.
  void setBufferLength(uint64_t newLen) {
    lengthAndByteCount = getDigestLength() | (newLen << 56);
  }

  /// Return the number of bytes that have been hash-combined so far.
  uint64_t getDigestLength() const {
    return lengthAndByteCount & ~(uint64_t(0xFF) << 56);
  }

  void compress(uint64_t value);
};

} // namespace swift

#endif // SWIFT_BASIC_STABLEHASHER_H
