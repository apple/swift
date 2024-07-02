// RUN: %target-run-simple-swift

// REQUIRES: executable_test

// https://github.com/apple/swift/issues/56600
// Error: symbol 'powTJfSSpSr' (powTJfSSpSr) is in generated IR file, but not
// in TBD file

import _Differentiation

#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#elseif canImport(Android)
  import Android
#elseif os(Windows)
  import CRT
#else
#error("Unsupported platform")
#endif

@inlinable
@derivative(of: pow)
func powVJP(
  _ base: Double, _ exponent: Double
) -> (value: Double, pullback: (Double) -> (Double, Double)) {
  fatalError()
}
