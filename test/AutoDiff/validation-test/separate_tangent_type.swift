// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import StdlibUnittest
#if canImport(Darwin)
  import Darwin.C
#elseif canImport(Glibc)
  import Glibc
#elseif canImport(Android)
  import Android
#elseif os(Windows)
  import CRT
#else
#error("Unsupported platform")
#endif

import DifferentiationUnittest

var SeparateTangentTypeTests = TestSuite("SeparateTangentType")

struct DifferentiableSubset : Differentiable {
  @differentiable(reverse, wrt: self)
  var w: Tracked<Float>
  @differentiable(reverse, wrt: self)
  var b: Tracked<Float>
  @noDerivative var flag: Bool

  struct TangentVector : Differentiable, AdditiveArithmetic {
    typealias TangentVector = DifferentiableSubset.TangentVector
    var w: Tracked<Float>
    var b: Tracked<Float>
  }
  mutating func move(by v: TangentVector) {
    w.move(by: v.w)
    b.move(by: v.b)
  }
}

SeparateTangentTypeTests.testWithLeakChecking("Trivial") {
  let x = DifferentiableSubset(w: 0, b: 1, flag: false)
  let pb = pullback(at: x) { x in x }
  expectEqual(pb(DifferentiableSubset.TangentVector.zero), DifferentiableSubset.TangentVector.zero)
}

SeparateTangentTypeTests.testWithLeakChecking("Initialization") {
  let x = DifferentiableSubset(w: 0, b: 1, flag: false)
  let pb = pullback(at: x) { x in DifferentiableSubset(w: 1, b: 2, flag: true) }
  expectEqual(pb(DifferentiableSubset.TangentVector.zero), DifferentiableSubset.TangentVector.zero)
}

SeparateTangentTypeTests.testWithLeakChecking("SomeArithmetics") {
  let x = DifferentiableSubset(w: 0, b: 1, flag: false)
  let pb = pullback(at: x) { x in DifferentiableSubset(w: x.w * x.w, b: x.b * x.b, flag: true) }
  expectEqual(pb(DifferentiableSubset.TangentVector.zero), DifferentiableSubset.TangentVector.zero)
}

runAllTests()
