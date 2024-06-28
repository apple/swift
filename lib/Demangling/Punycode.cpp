//===--- Punycode.cpp - Unicode to Punycode transcoding -------------------===//
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

#include "swift/Basic/Assertions.h"
#include "swift/Demangling/Punycode.h"
#include "swift/Demangling/ManglingUtils.h"
#include <limits>
#include <vector>
#include <cstdint>

using namespace swift;
using namespace Punycode;

// RFC 3492
// Section 5: Parameter values for Punycode

static const int base         = 36;
static const int tmin         = 1;
static const int tmax         = 26;
static const int skew         = 38;
static const int damp         = 700;
static const int initial_bias = 72;
static const uint32_t initial_n = 128;

static const char delimiter = '_';

static char digit_value(int digit) {
  assert(digit < base && "invalid punycode digit");
  if (digit < 26)
    return 'a' + digit;
  return 'A' - 26 + digit;
}

static int digit_index(char value) {
  if (value >= 'a' && value <= 'z')
    return value - 'a';
  if (value >= 'A' && value <= 'J')
    return value - 'A' + 26;
  return -1;
}

static bool isValidUnicodeScalar(uint32_t S) {
  // Also accept the range of 0xD800 - 0xD880, which is used for non-symbol
  // ASCII characters.
  return (S < 0xD880) || (S >= 0xE000 && S <= 0x10FFFF);
}

// Section 6.1: Bias adaptation function

static int adapt(int delta, int numpoints, bool firsttime) {
  if (firsttime)
    delta = delta / damp;
  else
    delta = delta / 2;
  
  delta += delta / numpoints;
  int k = 0;
  while (delta > ((base - tmin) * tmax) / 2) {
    delta /= base - tmin;
    k += base;
  }
  return k + (((base - tmin + 1) * delta) / (delta + skew));
}

// Section 6.2: Decoding procedure

bool Punycode::decodePunycode(StringRef InputPunycode,
                              std::vector<uint32_t> &OutCodePoints) {
  OutCodePoints.clear();
  OutCodePoints.reserve(InputPunycode.size());

  // -- Build the decoded string as UTF32 first because we need random access.
  uint32_t n = initial_n;
  int i = 0;
  int bias = initial_bias;
  /// let output = an empty string indexed from 0
  // consume all code points before the last delimiter (if there is one)
  //  and copy them to output,
  size_t lastDelimiter = InputPunycode.find_last_of(delimiter);
  if (lastDelimiter != StringRef::npos) {
    for (char c : InputPunycode.slice(0, lastDelimiter)) {
      // fail on any non-basic code point
      if (static_cast<unsigned char>(c) > 0x7f)
        return false;
      OutCodePoints.push_back(c);
    }
    // if more than zero code points were consumed then consume one more
    //  (which will be the last delimiter)
    InputPunycode =
        InputPunycode.slice(lastDelimiter + 1, InputPunycode.size());
  }
  
  while (!InputPunycode.empty()) {
    int oldi = i;
    int w = 1;
    for (int k = base; ; k += base) {
      // consume a code point, or fail if there was none to consume
      if (InputPunycode.empty())
        return false;
      char codePoint = InputPunycode.front();
      InputPunycode = InputPunycode.slice(1, InputPunycode.size());
      // let digit = the code point's digit-value, fail if it has none
      int digit = digit_index(codePoint);
      if (digit < 0)
        return false;
      
      // Fail if i + (digit * w) would overflow
      if (digit > (std::numeric_limits<int>::max() - i) / w)
        return false;
      i = i + digit * w;
      int t = k <= bias ? tmin
            : k >= bias + tmax ? tmax
            : k - bias;
      if (digit < t)
        break;
      // Fail if w * (base - t) would overflow
      if (w > std::numeric_limits<int>::max() / (base - t))
        return false;
      w = w * (base - t);
    }
    bias = adapt(i - oldi, OutCodePoints.size() + 1, oldi == 0);
    // Fail if n + i / (OutCodePoints.size() + 1) would overflow
    if (i / (OutCodePoints.size() + 1) > std::numeric_limits<int>::max() - n)
      return false;
    n = n + i / (OutCodePoints.size() + 1);
    i = i % (OutCodePoints.size() + 1);
    // if n is a basic code point then fail
    if (n < 0x80)
      return false;
    // insert n into output at position i
    OutCodePoints.insert(OutCodePoints.begin() + i, n);
    ++i;
  }
  
  return true;
}

// Section 6.3: Encoding procedure

bool Punycode::encodePunycode(const std::vector<uint32_t> &InputCodePoints,
                              std::string &OutPunycode) {
  OutPunycode.clear();

  uint32_t n = initial_n;
  int delta = 0;
  int bias = initial_bias;

  // let h = b = the number of basic code points in the input
  // copy them to the output in order...
  size_t h = 0;
  for (auto C : InputCodePoints) {
    if (C < 0x80) {
      ++h;
      OutPunycode.push_back(C);
    }
    if (!isValidUnicodeScalar(C)) {
      OutPunycode.clear();
      return false;
    }
  }
  size_t b = h;
  // ...followed by a delimiter if b > 0
  if (b > 0)
    OutPunycode.push_back(delimiter);
  
  while (h < InputCodePoints.size()) {
    // let m = the minimum code point >= n in the input
    uint32_t m = 0x10FFFF;
    for (auto codePoint : InputCodePoints) {
      if (codePoint >= n && codePoint < m)
        m = codePoint;
    }

    if ((m - n) > (std::numeric_limits<int>::max() - delta) / (h + 1))
      return false;
    delta = delta + (m - n) * (h + 1);
    n = m;
    for (auto c : InputCodePoints) {
      if (c < n) {
        if (delta == std::numeric_limits<int>::max())
          return false;
        ++delta;
      }
      if (c == n) {
        int q = delta;
        for (int k = base; ; k += base) {
          int t = k <= bias ? tmin
                : k >= bias + tmax ? tmax
                : k - bias;
          
          if (q < t) break;
          OutPunycode.push_back(digit_value(t + ((q - t) % (base - t))));
          q = (q - t) / (base - t);
        }
        OutPunycode.push_back(digit_value(q));
        bias = adapt(delta, h + 1, h == b);
        delta = 0;
        ++h;
      }
    }
    ++delta; ++n;
  }
  return true;
}

static bool encodeToUTF8(const std::vector<uint32_t> &Scalars,
                         std::string &OutUTF8) {
  for (auto S : Scalars) {
    if (!isValidUnicodeScalar(S)) {
      OutUTF8.clear();
      return false;
    }
    if (S >= 0xD800 && S < 0xD880)
      S -= 0xD800;

    unsigned Bytes = 0;
    if (S < 0x80)
      Bytes = 1;
    else if (S < 0x800)
      Bytes = 2;
    else if (S < 0x10000)
      Bytes = 3;
    else
      Bytes = 4;

    switch (Bytes) {
    case 1:
      OutUTF8.push_back(S);
      break;
    case 2: {
      uint8_t Byte2 = (S | 0x80) & 0xBF;
      S >>= 6;
      uint8_t Byte1 = S | 0xC0;
      OutUTF8.push_back(Byte1);
      OutUTF8.push_back(Byte2);
      break;
    }
    case 3: {
      uint8_t Byte3 = (S | 0x80) & 0xBF;
      S >>= 6;
      uint8_t Byte2 = (S | 0x80) & 0xBF;
      S >>= 6;
      uint8_t Byte1 = S | 0xE0;
      OutUTF8.push_back(Byte1);
      OutUTF8.push_back(Byte2);
      OutUTF8.push_back(Byte3);
      break;
    }
    case 4: {
      uint8_t Byte4 = (S | 0x80) & 0xBF;
      S >>= 6;
      uint8_t Byte3 = (S | 0x80) & 0xBF;
      S >>= 6;
      uint8_t Byte2 = (S | 0x80) & 0xBF;
      S >>= 6;
      uint8_t Byte1 = S | 0xF0;
      OutUTF8.push_back(Byte1);
      OutUTF8.push_back(Byte2);
      OutUTF8.push_back(Byte3);
      OutUTF8.push_back(Byte4);
      break;
    }
    }
  }
  return true;
}

bool Punycode::decodePunycodeUTF8(StringRef InputPunycode,
                                  std::string &OutUTF8) {
  std::vector<uint32_t> OutCodePoints;
  if (!decodePunycode(InputPunycode, OutCodePoints))
    return false;

  return encodeToUTF8(OutCodePoints, OutUTF8);
}

static bool isContinuationByte(uint8_t unit) {
  return (unit & 0xC0) == 0x80;
}

/// Reencode well-formed UTF-8 as UTF-32.
///
/// This entry point is only called from compiler-internal entry points, so does
/// only minimal validation. In particular, it does *not* check for overlong
/// encodings.
/// If \p mapNonSymbolChars is true, non-symbol ASCII characters (characters
/// except [$_a-zA-Z0-9]) are also encoded like non-ASCII unicode characters.
/// Returns false if \p InputUTF8 contains surrogate code points.
static bool convertUTF8toUTF32(llvm::StringRef InputUTF8,
                               std::vector<uint32_t> &OutUTF32,
                               bool mapNonSymbolChars) {
  auto ptr = InputUTF8.begin();
  auto end = InputUTF8.end();
  while (ptr < end) {
    uint8_t first = *ptr++;
    uint32_t code_point = 0;
    if (first < 0x80) {
      if (Mangle::isValidSymbolChar(first) || !mapNonSymbolChars) {
        code_point = first;
      } else {
        code_point = (uint32_t)first + 0xD800;
      }
    } else if (first < 0xC0) {
      // Invalid continuation byte.
      return false;
    } else if (first < 0xE0) {
      // Two-byte sequence.
      if (ptr == end)
        return false;
      uint8_t second = *ptr++;
      if (!isContinuationByte(second))
        return false;
      code_point = ((first & 0x1F) << 6) | (second & 0x3F);
    } else if (first < 0xF0) {
      // Three-byte sequence.
      if (end - ptr < 2)
        return false;
      uint8_t second = *ptr++;
      uint8_t third = *ptr++;
      if (!isContinuationByte(second) || !isContinuationByte(third))
        return false;
      code_point = ((first & 0xF) << 12)
                 | ((second & 0x3F) << 6)
                 | ( third  & 0x3F      );
    } else if (first < 0xF8) {
      // Four-byte sequence.
      if (end - ptr < 3)
        return false;
      uint8_t second = *ptr++;
      uint8_t third = *ptr++;
      uint8_t fourth = *ptr++;
      if (!isContinuationByte(second) || !isContinuationByte(third)
          || !isContinuationByte(fourth))
        return false;
      code_point = ((first & 0x7) << 18)
                  | ((second & 0x3F) << 12)
                  | ((third  & 0x3F) <<  6)
                  | ( fourth & 0x3F       );
    } else {
      // Unused sequence length.
      return false;
    }
    if (!isValidUnicodeScalar(code_point))
      return false;
    OutUTF32.push_back(code_point);
  }
  return true;
}

bool Punycode::encodePunycodeUTF8(StringRef InputUTF8,
                                  std::string &OutPunycode,
                                  bool mapNonSymbolChars) {
  std::vector<uint32_t> InputCodePoints;
  InputCodePoints.reserve(InputUTF8.size());

  if (!convertUTF8toUTF32(InputUTF8, InputCodePoints, mapNonSymbolChars))
    return false;

  return encodePunycode(InputCodePoints, OutPunycode);
}

