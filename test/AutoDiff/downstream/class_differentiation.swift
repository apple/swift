// RUN: %target-run-simple-swift
// NOTE: Verify whether forward-mode differentiation crashes. It currently does.
// RUN: not --crash %target-swift-frontend -enable-experimental-forward-mode-differentiation -emit-sil %s
// REQUIRES: executable_test

import StdlibUnittest
import DifferentiationUnittest

var ClassTests = TestSuite("ClassDifferentiation")

ClassTests.test("TrivialMember") {
  class C: Differentiable {
    @differentiable
    var float: Float

    @noDerivative
    final var noDerivative: Float = 1

    init(_ float: Float) {
      self.float = float
    }

    @differentiable
    func method(_ x: Float) -> Float {
      x * float
    }

    @differentiable
    func testNoDerivative() -> Float {
      noDerivative
    }

    @differentiable
    static func controlFlow(_ c1: C, _ c2: C, _ flag: Bool) -> Float {
      var result: Float = 0
      if flag {
        var c3 = C(c1.float * c2.float)
        result = c3.float
      } else {
        result = c2.float * c1.float
      }
      return result
    }
  }
  // Test class initializer differentiation.
  expectEqual(10, pullback(at: 3, in: { C($0) })(.init(float: 10)))
  // Test class method differentiation.
  expectEqual((.init(float: 3), 10), gradient(at: C(10), 3, in: { c, x in c.method(x) }))
  expectEqual(.init(float: 0), gradient(at: C(10), in: { c in c.testNoDerivative() }))
  expectEqual((.init(float: 20), .init(float: 10)),
              gradient(at: C(10), C(20), in: { c1, c2 in C.controlFlow(c1, c2, true) }))
}

ClassTests.test("NontrivialMember") {
  class C: Differentiable {
    @differentiable
    var float: Tracked<Float>

    init(_ float: Tracked<Float>) {
      self.float = float
    }

    @differentiable
    func method(_ x: Tracked<Float>) -> Tracked<Float> {
      x * float
    }

    @differentiable
    static func controlFlow(_ c1: C, _ c2: C, _ flag: Bool) -> Tracked<Float> {
      var result: Tracked<Float> = 0
      if flag {
        result = c1.float * c2.float
      } else {
        result = c2.float * c1.float
      }
      return result
    }
  }
  // Test class initializer differentiation.
  expectEqual(10, pullback(at: 3, in: { C($0) })(.init(float: 10)))
  // Test class method differentiation.
  expectEqual((.init(float: 3), 10), gradient(at: C(10), 3, in: { c, x in c.method(x) }))
  expectEqual((.init(float: 20), .init(float: 10)),
              gradient(at: C(10), C(20), in: { c1, c2 in C.controlFlow(c1, c2, true) }))
}

// TF-1149: Test class with loadable type but address-only `TangentVector` type.
// TODO(TF-1149): Uncomment when supported.
/*
ClassTests.test("AddressOnlyTangentVector") {
  class C<T: Differentiable>: Differentiable {
    @differentiable
    var stored: T

    @differentiable
    init(_ stored: T) {
      self.stored = stored
    }

    @differentiable
    func method(_ x: T) -> T {
      stored
    }
  }
  // Test class initializer differentiation.
  expectEqual(10, pullback(at: 3, in: { C<Float>($0) })(.init(float: 10)))
  // Test class method differentiation.
  expectEqual((.init(stored: Float(3)), 10),
              gradient(at: C<Float>(3), 3, in: { c, x in c.method(x) }))
}
*/

runAllTests()
