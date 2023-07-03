//===--- UUID.h - UUID generation -------------------------------*- C++ -*-===//
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
// This is an interface over the standard OSF uuid library that gives UUIDs
// sane value semantics and operators.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_UUID_H
#define SWIFT_BASIC_UUID_H

#include "swift/Basic/LLVM.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Optional.h"
#include "llvm/Support/raw_ostream.h"
#include <array>

namespace swift {
  
class UUID {
public:
  enum {
    /// The number of bytes in a UUID's binary representation.
    Size = 16,
    
    /// The number of characters in a UUID's string representation.
    StringSize = 36,
    
    /// The number of bytes necessary to store a null-terminated UUID's string
    /// representation.
    StringBufferSize = StringSize + 1,
  };
  
  unsigned char Value[Size];

private:
  enum FromRandom_t { FromRandom };
  enum FromTime_t { FromTime };

  UUID(FromRandom_t);
  
  UUID(FromTime_t);
  
public:
  /// Default constructor.
  UUID();
  
  UUID(std::array<unsigned char, Size> bytes) {
    memcpy(Value, &bytes, Size);
  }
  
  /// Create a new random UUID from entropy (/dev/random).
  static UUID fromRandom() { return UUID(FromRandom); }
  
  /// Create a new pseudorandom UUID using the time, MAC address, and pid.
  static UUID fromTime() { return UUID(FromTime); }
  
  /// Parse a UUID from a C string.
  static llvm::Optional<UUID> fromString(const char *s);

  /// Convert a UUID to its string representation.
  void toString(llvm::SmallVectorImpl<char> &out) const;
  
  int compare(UUID y) const;
  
#define COMPARE_UUID(op) \
  bool operator op(UUID y) { return compare(y) op 0; }
  
  COMPARE_UUID(==)
  COMPARE_UUID(!=)
  COMPARE_UUID(<)
  COMPARE_UUID(<=)
  COMPARE_UUID(>)
  COMPARE_UUID(>=)
#undef COMPARE_UUID
};
  
llvm::raw_ostream &operator<<(llvm::raw_ostream &os, UUID uuid);
  
}

namespace llvm {
  template<>
  struct DenseMapInfo<swift::UUID> {
    static inline swift::UUID getEmptyKey() {
      return {{{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}}};
    }
    static inline swift::UUID getTombstoneKey() {
      return {{{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE}}};
    }
    
    static unsigned getHashValue(swift::UUID uuid) {
      union {
        swift::UUID uu;
        unsigned words[4];
      } reinterpret = {uuid};
      return reinterpret.words[0] ^ reinterpret.words[1]
           ^ reinterpret.words[2] ^ reinterpret.words[3];
    }
    
    static bool isEqual(swift::UUID a, swift::UUID b) {
      return a == b;
    }
  };
} // end namespace swift

#endif // SWIFT_BASIC_UUID_H
