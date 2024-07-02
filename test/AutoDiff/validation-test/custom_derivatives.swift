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

var CustomDerivativesTests = TestSuite("CustomDerivatives")

// Specify non-differentiable functions.
// These will be wrapped in `differentiableFunction` and tested.

func unary(_ x: Tracked<Float>) -> Tracked<Float> {
  var x = x
  x *= 2
  return x
}

func binary(_ x: Tracked<Float>, _ y: Tracked<Float>) -> Tracked<Float> {
  var x = x
  x *= y
  return x
}

CustomDerivativesTests.testWithLeakChecking("SumOfGradPieces") {
  var grad: Tracked<Float> = 0
  func addToGrad(_ x: inout Tracked<Float>) { grad += x }
  _ = gradient(at: 4) { (x: Tracked<Float>) in
    x.withDerivative(addToGrad)
      * x.withDerivative(addToGrad)
        * x.withDerivative(addToGrad)
  }
  expectEqual(48, grad)
}

CustomDerivativesTests.testWithLeakChecking("ModifyGradientOfSum") {
  expectEqual(30, gradient(at: 4) { (x: Tracked<Float>) in
    x.withDerivative { $0 *= 10 } + x.withDerivative { $0 *= 20 }
  })
}

CustomDerivativesTests.testWithLeakChecking("WithoutDerivative") {
  expectEqual(0, gradient(at: Tracked<Float>(4)) { x in
    withoutDerivative(at: x) { x in
      Tracked<Float>(sinf(x.value) + cosf(x.value))
    }
  })
}

runAllTests()
