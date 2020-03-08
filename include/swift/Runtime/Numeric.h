//===--- Numeric.h - Swift Language ABI numerics support --------*- C++ -*-===//
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
// Swift runtime support for numeric operations.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_RUNTIME_NUMERIC_H
#define SWIFT_RUNTIME_NUMERIC_H

#include "swift/ABI/MetadataValues.h"
#include "swift/Runtime/Config.h"
#include "swift/Basic/LLVM.h"
#include "llvm/ADT/ArrayRef.h"

namespace swift {

/// A constant integer literal.  The format is designed to optimize the
/// checked-truncation operation typically performed by conformances to the
/// ExpressibleByBuiltinIntegerLiteral protocol.
class IntegerLiteral {
public:
  using SignedChunk = intptr_t;
  using UnsignedChunk = uintptr_t;
  enum : size_t { BitsPerChunk = sizeof(SignedChunk) * 8 };

private:
  const UnsignedChunk *Data;
  IntegerLiteralFlags Flags;

public:
  constexpr IntegerLiteral(const UnsignedChunk *data, IntegerLiteralFlags flags)
    : Data(data), Flags(flags) {}

  /// Return the chunks of data making up this value, arranged starting from
  /// the least-significant chunk.  The value is sign-extended to fill the
  /// final chunk.
  ArrayRef<UnsignedChunk> getData() const {
    return ArrayRef<UnsignedChunk>(Data,
                      (Flags.getBitWidth() + BitsPerChunk - 1) / BitsPerChunk);
  }

  /// The flags for this value.
  IntegerLiteralFlags getFlags() const { return Flags; }

  /// Whether this value is negative.
  bool isNegative() const { return Flags.isNegative(); }

  /// The minimum number of bits necessary to store this value.
  /// Because this always includes the sign bit, it is never zero.
  size_t getBitWidth() const { return Flags.getBitWidth(); }
};

SWIFT_RUNTIME_EXPORT SWIFT_CC
float swift_intToFloat32(IntegerLiteral value);

SWIFT_RUNTIME_EXPORT SWIFT_CC
double swift_intToFloat64(IntegerLiteral value);

// TODO: Float16 instead of just truncating from float?
// TODO: Float80 on x86?
// TODO: Float128 on targets that provide it?

}

#endif
