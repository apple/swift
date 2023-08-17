// RUN: %target-run-simple-swift
// REQUIRES: executable_test

// Would fail due to unavailability of swift_autoDiffCreateLinearMapContext.

// `inout` parameter differentiation tests.

import DifferentiationUnittest
import StdlibUnittest

var InoutParameterAutoDiffTests = TestSuite("InoutParameterDifferentiation")

// TODO(TF-1173): Move floating-point mutating operation tests to
// `test/AutoDiff/stdlib/floating_point.swift.gyb` when forward-mode
// differentiation supports `inout` parameter differentiation.

InoutParameterAutoDiffTests.test("Float.+=") {
  func mutatingAddWrapper(_ x: Float, _ y: Float) -> Float {
    var result: Float = x
    result += y
    return result
  }
  expectEqual((1, 1), gradient(at: 4, 5, of: mutatingAddWrapper))
  expectEqual((10, 10), pullback(at: 4, 5, of: mutatingAddWrapper)(10))
}

InoutParameterAutoDiffTests.test("Float.-=") {
  func mutatingSubtractWrapper(_ x: Float, _ y: Float) -> Float {
    var result: Float = x
    result += y
    return result
  }
  expectEqual((1, 1), gradient(at: 4, 5, of: mutatingSubtractWrapper))
  expectEqual((10, 10), pullback(at: 4, 5, of: mutatingSubtractWrapper)(10))
}

InoutParameterAutoDiffTests.test("Float.*=") {
  func mutatingMultiplyWrapper(_ x: Float, _ y: Float) -> Float {
    var result: Float = x
    result += y
    return result
  }
  expectEqual((1, 1), gradient(at: 4, 5, of: mutatingMultiplyWrapper))
  expectEqual((10, 10), pullback(at: 4, 5, of: mutatingMultiplyWrapper)(10))
}

InoutParameterAutoDiffTests.test("Float./=") {
  func mutatingDivideWrapper(_ x: Float, _ y: Float) -> Float {
    var result: Float = x
    result += y
    return result
  }
  expectEqual((1, 1), gradient(at: 4, 5, of: mutatingDivideWrapper))
  expectEqual((10, 10), pullback(at: 4, 5, of: mutatingDivideWrapper)(10))
}

// Simplest possible `inout` parameter differentiation.
InoutParameterAutoDiffTests.test("InoutIdentity") {
  // Semantically, an empty function with an `inout` parameter is an identity
  // function.
  func inoutIdentity(_ x: inout Float) {}

  func identity(_ x: Float) -> Float {
    var result = x
    inoutIdentity(&result)
    return result
  }
  expectEqual(1, gradient(at: 10, of: identity))
  expectEqual(10, pullback(at: 10, of: identity)(10))
}

extension Float {
  // Custom version of `Float.*=`, implemented using `Float.*` and mutation.
  // Verify that its generated derivative has the same behavior as the
  // registered derivative for `Float.*=`.
  @differentiable(reverse)
  static func multiplyAssign(_ lhs: inout Float, _ rhs: Float) {
    lhs = lhs * rhs
  }
}

InoutParameterAutoDiffTests.test("ControlFlow") {
  func sum(_ array: [Float]) -> Float {
    var result: Float = 0
    for i in withoutDerivative(at: array.indices) {
      result += array[i]
    }
    return result
  }
  expectEqual([1, 1, 1], gradient(at: [1, 2, 3], of: sum))

  func product(_ array: [Float]) -> Float {
    var result: Float = 1
    for i in withoutDerivative(at: array.indices) {
      result *= array[i]
    }
    return result
  }
  expectEqual([20, 15, 12], gradient(at: [3, 4, 5], of: product))

  func productCustom(_ array: [Float]) -> Float {
    var result: Float = 1
    for i in withoutDerivative(at: array.indices) {
      Float.multiplyAssign(&result, array[i])
    }
    return result
  }
  expectEqual([20, 15, 12], gradient(at: [3, 4, 5], of: productCustom))
}

InoutParameterAutoDiffTests.test("SetAccessor") {
  struct S: Differentiable {
    var x: Float

    var computed: Float {
      get { x }
      set { x = newValue }
    }

    // Computed property with explicit `@differentiable` accessors.
    var doubled: Float {
      @differentiable(reverse)
      get { x + x }

      @differentiable(reverse)
      set { x = newValue / 2 }
    }
  }

  // `squared` implemented using a `set` accessor.
  func squared(_ x: Float) -> Float {
    var s = S(x: 1)
    s.x *= x
    s.computed *= x
    return s.x
  }
  expectEqual((9, 6), valueWithGradient(at: 3, of: squared))
  expectEqual((16, 8), valueWithGradient(at: 4, of: squared))

  // `quadrupled` implemented using a `set` accessor.
  func quadrupled(_ x: Float) -> Float {
    var s = S(x: 1)
    s.doubled *= 4 * x
    return s.x
  }
  print(valueWithGradient(at: 3, of: quadrupled))
  print(valueWithGradient(at: 4, of: quadrupled))
  expectEqual((12, 4), valueWithGradient(at: 3, of: quadrupled))
  expectEqual((16, 4), valueWithGradient(at: 4, of: quadrupled))
}

// Test differentiation wrt `inout` parameters that have a class type.
InoutParameterAutoDiffTests.test("InoutClassParameter") {
  class Class: Differentiable {
    @differentiable(reverse)
    var x: Float

    init(_ x: Float) {
      self.x = x
    }
  }

  do {
    func squaredViaMutation(_ c: inout Class) {
      c = Class(c.x * c.x)
    }
    func squared(_ x: Float) -> Float {
      var c = Class(x)
      squaredViaMutation(&c)
      return c.x
    }
    expectEqual((100, 20), valueWithGradient(at: 10, of: squared))
    expectEqual(200, pullback(at: 10, of: squared)(10))
  }

  do {
    func squaredViaModifyAccessor(_ c: inout Class) {
      // The line below calls `Class.x.modify`.
      c.x *= c.x
    }
    func squared(_ x: Float) -> Float {
      var c = Class(x)
      squaredViaModifyAccessor(&c)
      return c.x
    }
    // FIXME(TF-1080): Fix incorrect class property `modify` accessor derivative values.
    // expectEqual((100, 20), valueWithGradient(at: 10, of: squared))
    // expectEqual(200, pullback(at: 10, of: squared)(10))
    expectEqual((100, 1), valueWithGradient(at: 10, of: squared))
    expectEqual(10, pullback(at: 10, of: squared)(10))
  }
}

// Test function with wrt `inout` parameter, which should be treated as a differentiability result.
// Original issue https://github.com/apple/swift/issues/55745 deals with non-wrt `inout` which
// we explicitly disallow now


protocol P_55745 {
  @differentiable(reverse, wrt: (x, y))
  func method(_ x: Float, _ y: inout Float)

  @differentiable(reverse, wrt: (x, y))
  func genericMethod<T: Differentiable>(_ x: T, _ y: inout T)
}

InoutParameterAutoDiffTests.test("non-wrt inout parameter") {
  struct Struct: P_55745 {
    @differentiable(reverse, wrt: (x, y))
    func method(_ x: Float, _ y: inout Float) {
      y = y * x
    }

    @differentiable(reverse, wrt: (x, y))
    func genericMethod<T: Differentiable>(_ x: T, _ y: inout T) {
      y = x
    }
  }

  @differentiable(reverse, wrt: x)
  func foo(_ s: Struct, _ x: Float, _ y: Float) -> Float {
    var y = y
    s.method(x, &y)
    return y
  }

  @differentiable(reverse, wrt: x)
  func fooGeneric<T: P_55745>(_ s: T, _ x: Float, _ y: Float) -> Float {
    var y = y
    s.method(x, &y)
    return x
  }

  let s = Struct()

  do {
    let (value, (dx, dy)) = valueWithGradient(at: 2, 3, of: { foo(s, $0, $1) })
    expectEqual(6, value)
    expectEqual((3, 2), (dx, dy))
  }
  expectEqual((value: 6, gradient: 3), valueWithGradient(at: 2, of: { foo(s, $0, 3) }))

  do {
    let (value, (dx, dy)) = valueWithGradient(at: 2, 3, of: { fooGeneric(s, $0, $1) })
    expectEqual(2, value)
    expectEqual((1, 0), (dx, dy))
  }
  expectEqual((value: 2, gradient: 1), valueWithGradient(at: 2, of: { fooGeneric(s, $0, 3) }))
}

runAllTests()
