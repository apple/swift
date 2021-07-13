// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import _Differentiation

// Test `Differentiable` protocol conformances for stdlib types.

func assertConformsToDifferentiable<T>(_: T.Type) where T: Differentiable {}

func assertSelfEqualsTangentVector<T>(_: T.Type)
where T: Differentiable, T == T.TangentVector {}

// Test `FloatingPoint` types.
func testFloatingPointDifferentiableConformance() {
  assertSelfEqualsTangentVector(Float.self)
  assertSelfEqualsTangentVector(Double.self)
  #if (arch(i386) || arch(x86_64)) && !(os(Windows) || os(Android) || os(FreeBSD))
  assertSelfEqualsTangentVector(Float80.self)
  #endif
}
