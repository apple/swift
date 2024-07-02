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

_ = M_PI // expected-warning {{is deprecated}}
_ = M_PI_2 // expected-warning {{is deprecated}}
_ = M_PI_4 // expected-warning {{is deprecated}}

_ = M_SQRT2 // expected-warning {{is deprecated}}
_ = M_SQRT1_2 // expected-warning {{is deprecated}}
