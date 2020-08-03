//===--- Numeric.cpp - Swift Language ABI numerics support ----------------===//
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
// Implementations of the numeric-support ABI functions.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Numeric.h"

using namespace swift;

/// Convert an integer literal to the floating-point type T.
template <class T>
static T convert(IntegerLiteral value) {
  using SignedChunk = IntegerLiteral::SignedChunk;
  using UnsignedChunk = IntegerLiteral::UnsignedChunk;

  auto data = value.getData();
  assert(!data.empty() && "always require at least one chunk");

  // The single-word case is the easiest.
  if (data.size() == 1) {
    return T(SignedChunk(data[0]));
  }

  // In two's complement, only the topmost chunk is really signed;
  // everything else is added to that as an unsigned value.
  static_assert(IntegerLiteral::BitsPerChunk == 32 ||
                IntegerLiteral::BitsPerChunk == 64,
                "expected either 32-bit or 64-bit chunking");
  T chunkFactor = (IntegerLiteral::BitsPerChunk == 32 ? 0x1p32 : 0x1p64);

  T result = UnsignedChunk(data[0]);
  T scale = chunkFactor;
  for (size_t i = 1, e = data.size() - 1; i != e; ++i) {
    result += UnsignedChunk(data[i]) * scale;
    scale *= chunkFactor;
  }
  result += SignedChunk(data.back()) * scale;

  return result;
}

SWIFT_CC(swift)
float swift::swift_intToFloat32(IntegerLiteral value) {
  return convert<float>(value);
}

SWIFT_CC(swift)
double swift::swift_intToFloat64(IntegerLiteral value) {
  return convert<double>(value);
}
