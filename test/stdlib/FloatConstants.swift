// RUN: %target-typecheck-verify-swift

#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#elseif os(WASI)
  import WASILibc
#elseif canImport(Android)
  import Android
#elseif os(Windows)
  import CRT
#else
#error("Unsupported platform")
#endif

_ = FLT_RADIX // expected-warning {{is deprecated}}

_ = FLT_MANT_DIG // expected-warning {{is deprecated}}
_ = FLT_MIN_EXP // expected-warning {{is deprecated}}
_ = FLT_MAX_EXP // expected-warning {{is deprecated}}
_ = FLT_MAX // expected-warning {{is deprecated}}
_ = FLT_EPSILON // expected-warning {{is deprecated}}
_ = FLT_MIN // expected-warning {{is deprecated}}
_ = FLT_TRUE_MIN // expected-warning {{is deprecated}}

_ = DBL_MANT_DIG // expected-warning {{is deprecated}}
_ = DBL_MIN_EXP // expected-warning {{is deprecated}}
_ = DBL_MAX_EXP // expected-warning {{is deprecated}}
_ = DBL_MAX // expected-warning {{is deprecated}}
_ = DBL_EPSILON // expected-warning {{is deprecated}}
_ = DBL_MIN // expected-warning {{is deprecated}}
_ = DBL_TRUE_MIN // expected-warning {{is deprecated}}
