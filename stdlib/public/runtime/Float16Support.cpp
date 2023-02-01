//===------------- Float16Support.cpp - Swift Float16 Support -------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Implementations of:
//
// __gnu_h2f_ieee
// __gnu_f2h_ieee
// __truncdfhf2
// __extendhfxf2
//
// On Darwin platforms, these are provided by the host compiler-rt, but we
// can't depend on that everywhere, so we have to provide them in the Swift
// runtime. Calls to these symbols are automatically generated by LLVM when
// operating on Float16, so they are used *even though they appear to have
// no call sites anywhere in Swift*.
//
// These may require different naming or mangling on other targets; what I've
// setup here is correct for Linux/x86.
//
//===----------------------------------------------------------------------===//

// Android NDK <r21 do not provide `__aeabi_d2h` in the compiler runtime,
// provide shims in that case.
#if (defined(__ANDROID__) && defined(__ARM_ARCH_7A__) && defined(__ARM_EABI__)) || \
    (!defined(__APPLE__) && (defined(__i386__) || defined(__x86_64__)))

#include "swift/shims/Visibility.h"

#include <stdint.h>

static unsigned toEncoding(float f) {
  unsigned e;
  static_assert(sizeof e == sizeof f, "float and int must have the same size");
  __builtin_memcpy(&e, &f, sizeof f);
  return e;
}

static float fromEncoding(unsigned int e) {
  float f;
  static_assert(sizeof f == sizeof e, "float and int must have the same size");
  __builtin_memcpy(&f, &e, sizeof f);
  return f;
}

#if (defined(__i386__) || defined(__x86_64__)) &&                               \
    !(defined(__ANDROID__) || defined(__APPLE__) || defined(_WIN32))
static long double fromEncoding(__uint128_t hf) {
  long double ld;
  __builtin_memcpy(&ld, &hf, sizeof ld);
  return ld;
}
#endif

#if defined(__x86_64__) && defined(__F16C__)

// If we're compiling the runtime for a target that has the conversion
// instruction, we might as well just use those. In theory, we'd also be
// compiling Swift for that target and not need these builtins at all,
// but who knows what could go wrong, and they're tiny functions.
# include <immintrin.h>

SWIFT_RUNTIME_EXPORT float __gnu_h2f_ieee(short h) {
  return _mm_cvtss_f32(_mm_cvtph_ps(_mm_set_epi64x(0,h)));
}

SWIFT_RUNTIME_EXPORT short __gnu_f2h_ieee(float f) {
  return (unsigned short)_mm_cvtsi128_si32(
    _mm_cvtps_ph(_mm_set_ss(f), _MM_FROUND_CUR_DIRECTION)
  );
}

#else

// Input in di, result in xmm0. We can get that calling convention in C++
// by taking a int16 arg instead of Float16, which we don't have (or else
// we wouldn't need this function).
SWIFT_RUNTIME_EXPORT float __gnu_h2f_ieee(unsigned short h) {
  // We need to have two cases; subnormals and zeros, and everything else.
  // We are in the first case if the exponent field (bits 14:10) is zero:
  if ((h & 0x7c00) == 0) {
    // Sign-extend and mask so that we get a subnormal or zero in f32
    // with the appropriate sign, then multiply by the appropriate scale
    // factor to produce the f32 result.
    return 0x1.0p125f * fromEncoding((int)(short)h & 0x80007fffU);
  }
  // We have either a normal number of an infinity or NaN. All of these
  // can be handled by shifting the significand into the correct position,
  // extending the exponent, and then multiplying by the correct scale.
  return 0x1.0p-112f * fromEncoding((int)(short)h << 13 | 0x70000000U);
}

// Input in xmm0, result in di. We can get that calling convention in C++
// by returning int16 instead of Float16, which we don't have (or else
// we wouldn't need this function).
SWIFT_RUNTIME_EXPORT unsigned short __gnu_f2h_ieee(float f) {
  unsigned signbit = toEncoding(f) & 0x80000000U;
  // Construct a "magic" rounding constant for f; this is a value that
  // we will add and subtract from f to force rounding to occur in the
  // correct position for half-precision. Half has 10 significand bits,
  // float has 23, so we need to add 2**(e+13) to get the desired rounding.
  float magic;
  unsigned exponent = toEncoding(f) & 0x7f800000;
  // Subnormals all round in the same place as the minimum normal binade,
  // so treat anything below 0x1.0p-14 as 0x1.0p-14.
  if (exponent < 0x38800000) exponent = 0x38800000;
  // In the overflow, inf, and NaN cases, magic doesn't contribute, so we
  // just use zero for anything bigger than 0x1.0p16.
  if (exponent > 0x47000000) magic = fromEncoding(signbit);
  else magic = fromEncoding(signbit | exponent + 0x06800000);
  // Map anything with an exponent larger than 15 to infinity; this will
  // avoid special-casing overflow later on.
  f = 0x1.0p112f*f;
  f = 0x1.0p-112f*f + magic;
  f -= magic;
  // We've now rounded in the correct place. One more scaling and we have
  // all the bits we need (this multiply does not change anything for
  // normal results, but denormalizes tiny results exactly as needed).
  f *= 0x1.0p-112f;
  short magnitude = toEncoding(f) >> 13 & 0x7fff;
  return (int)signbit >> 16 | magnitude;
}

#endif

// Input in xmm0, result in di. We can get that calling convention in C++
// by returning uint16 instead of Float16, which we don't have (or else
// we wouldn't need this function).
//
// Note that F16C doesn't provide this operation, so we still need a software
// implementation on those cores.
SWIFT_RUNTIME_EXPORT unsigned short __truncdfhf2(double d) {
  // You can't just do (half)(float)x, because that makes the result
  // susceptible to double-rounding. Instead we need to make the first
  // rounding use round-to-odd, but that doesn't exist on x86, so we have
  // to fake it.
  float f = (float)d;
  // Double-rounding can only occur if the result of rounding to float is
  // an exact-halfway case for the subsequent rounding to float16. We
  // can check for that significand bit pattern quickly (though we need
  // to be careful about values that will result in a subnormal float16,
  // as those will round in a different position):
  unsigned e = toEncoding(f);
  bool exactHalfway = (e & 0x1fff) == 0x1000;
  double fabs = __builtin_fabsf(f);
  if (exactHalfway || __builtin_fabsf(f) < 0x1.0p-14f) {
    // We might be in a double-rounding case, so simulate sticky-rounding
    // by comparing f and x and adjusting as needed.
    double dabs = __builtin_fabs(d);
    if (fabs > dabs) e -= ~e & 1;
    if (fabs < dabs) e |= 1;
    f = fromEncoding(e);
  }
  return __gnu_f2h_ieee(f);
}

// F16C does not cover FP80 conversions, so we still need an implementation
// here.
#if (defined(__i386__) || defined(__x86_64__)) &&                               \
    !(defined(__ANDROID__) || defined(__APPLE__) || defined(_WIN32))

SWIFT_RUNTIME_EXPORT long double __extendhfxf2(uint16_t h) {
  __uint128_t concat(uint64_t hi, uint64_t lo) {
    return (((__uint128_t)hi) << 64) | lo;
  }

  // We need to have two cases; subnormals and zeros, and everything else.
  // We are in the first case if the exponent field (bits 14:10) is zero:
  if ((h & 0x7c00) == 0) {
    // Sign-extend and mask so that we get a subnormal or zero in f32
    // with the appropriate sign, then multiply by the appropriate scale
    // factor to produce the f32 result.
    const __uint128_t mask = concat(0x8000, 0xffffffffffffffff);
    return 0x1.0p16368L * fromEncoding((__int128_t)h << 53 & mask);
  }
  // We have either a normal number of an infinity or NaN. All of these
  // can be handled by shifting the significand into the correct position,
  // extending the exponent, and then multiplying by the correct scale.
  const __uint128_t value = concat(0x7fe0, 0x8000000000000000);
  return 0x1.0p-16368L * fromEncoding(((__int128_t)(h & 0xfc00) << 54) |
                                      ((__int128_t)(h & 0x3ff) << 53) | value);
}

#endif // (defined(__i386__) || defined(__x86_64__)) &&
       // !(defined(__ANDROID__) || defined(__APPLE__) || defined(_WIN32))

#if defined(__ARM_EABI__)
SWIFT_RUNTIME_EXPORT unsigned short __aeabi_d2h(double d) {
  return __truncdfhf2(d);
}
#endif

#endif // (defined(__ANDROID__) && defined(__ARM_ARCH_7A__) && defined(__ARM_EABI__)) ||
       // (!defined(__APPLE__) && (defined(__i386__) || defined(__x86_64__)))
