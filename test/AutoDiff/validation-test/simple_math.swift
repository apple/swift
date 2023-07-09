// RUN: %target-run-simple-swift

// NOTE(TF-813): verify that enabling forward-mode does not affect reverse-mode.
// Temporarily disabled because forward-mode is not at feature parity with reverse-mode.
// UN: %target-run-simple-swift(-Xfrontend -enable-experimental-forward-mode-differentiation)

// RUN: %target-swift-frontend -Xllvm -sil-print-after=differentiation %s -emit-sil -o /dev/null -module-name null 2>&1 | %FileCheck %s

// REQUIRES: executable_test

import StdlibUnittest
import DifferentiationUnittest

var SimpleMathTests = TestSuite("SimpleMath")

SimpleMathTests.test("Arithmetics") {
  func foo1(x: Float, y: Float) -> Float {
    return x * y
  }
  expectEqual((4, 3), gradient(at: 3, 4, of: foo1))
  func foo2(x: Float, y: Float) -> Float {
    return -x * y
  }
  expectEqual((-4, -3), gradient(at: 3, 4, of: foo2))
  func foo3(x: Float, y: Float) -> Float {
    return -x + y
  }
  expectEqual((-1, 1), gradient(at: 3, 4, of: foo3))
}

SimpleMathTests.test("Fanout") {
  func foo1(x: Float) -> Float {
     x - x
  }
  expectEqual(0, gradient(at: 100, of: foo1))
  func foo2(x: Float) -> Float {
     x + x
  }
  expectEqual(2, gradient(at: 100, of: foo2))
  func foo3(x: Float, y: Float) -> Float {
    x + x + x * y
  }
  expectEqual((4, 3), gradient(at: 3, 2, of: foo3))
}

SimpleMathTests.test("FunctionCall") {
  func foo(_ x: Float, _ y: Float) -> Float {
    return 3 * x + { $0 * 3 }(3) * y
  }
  expectEqual((3, 9), gradient(at: 3, 4, of: foo))
  expectEqual(3, gradient(at: 3) { x in foo(x, 4) })
}

SimpleMathTests.test("ResultSelection") {
  func tuple(_ x: Float, _ y: Float) -> (Float, Float) {
    return (x + 1, y + 2)
  }
  expectEqual((1, 0), gradient(at: 3, 3, of: { x, y in tuple(x, y).0 }))
  expectEqual((0, 1), gradient(at: 3, 3, of: { x, y in tuple(x, y).1 }))

  func tupleGeneric<T>(_ x: T, _ y: T) -> (T, T) {
    return (x, y)
  }
  func tupleGenericFirst<T>(_ x: T, _ y: T) -> T { tupleGeneric(x, y).0 }
  func tupleGenericSecond<T>(_ x: T, _ y: T) -> T { tupleGeneric(x, y).1 }
  expectEqual((1, 0), gradient(at: 3, 3, of: tupleGenericFirst))
  expectEqual((0, 1), gradient(at: 3, 3, of: tupleGenericSecond))
}

SimpleMathTests.test("MultipleResults") {
  // Test function returning a tuple of active results.
  func tuple(_ x: Float, _ y: Float) -> (Float, Float) {
    return (x, y)
  }
  func multiply(_ x: Float, _ y: Float) -> Float {
    let z = tuple(x, y)
    // Note: both results (tuple elements) are active.
    return z.0 * z.1
  }
  expectEqual((4, 3), gradient(at: 3, 4, of: multiply))
  expectEqual((10, 5), gradient(at: 5, 10, of: multiply))

  // Test function with multiple `inout` parameters.
  func swap(_ x: inout Float, _ y: inout Float) {
    let tmp = x; x = y; y = tmp
  }
  func multiply_swap(_ x: Float, _ y: Float) -> Float {
    var tuple = (x, y)
    swap(&tuple.0, &tuple.1)
    return tuple.0 * tuple.1
  }
  expectEqual((4, 3), gradient(at: 3, 4, of: multiply_swap))
  expectEqual((10, 5), gradient(at: 5, 10, of: multiply_swap))

  // Test function with multiple `inout` parameters.
  func swapGeneric<T>(_ x: inout T, _ y: inout T) {
    let tmp = x; x = y; y = tmp
  }
  func multiply_swapGeneric(_ x: Float, _ y: Float) -> Float {
    var tuple = (x, y)
    swapGeneric(&tuple.0, &tuple.1)
    return tuple.0 * tuple.1
  }
  expectEqual((4, 3), gradient(at: 3, 4, of: multiply_swapGeneric))
  expectEqual((10, 5), gradient(at: 5, 10, of: multiply_swapGeneric))

  // Test function with multiple `inout` parameters and a formal result.
  func swapAndReturnProduct(_ x: inout Float, _ y: inout Float) -> Float {
    let tmp = x
    x = y
    y = tmp
    return x * y
  }
  func multiply_swapAndReturnProduct(_ x: Float, _ y: Float) -> Float {
    var x2 = x
    var y2 = y
    let result = swapAndReturnProduct(&x2, &y2)
    return result
  }
  expectEqual((4, 3), gradient(at: 3, 4, of: multiply_swapAndReturnProduct))
  expectEqual((4, 3), gradient(at: 3, 4, of: multiply_swapAndReturnProduct))
}

// Test function with multiple `inout` parameters and a custom pullback.
@differentiable(reverse)
func swapCustom(_ x: inout Float, _ y: inout Float) {
  let tmp = x; x = y; y = tmp
}
@derivative(of: swapCustom)
func vjpSwapCustom(_ x: inout Float, _ y: inout Float) -> (
  value: Void, pullback: (inout Float, inout Float) -> Void
) {
  swapCustom(&x, &y)
  return ((), {v1, v2 in
    let tmp = v1; v1 = v2; v2 = tmp
  })
}

SimpleMathTests.test("MultipleResultsWithCustomPullback") {
  func multiply_swapCustom(_ x: Float, _ y: Float) -> Float {
    var tuple = (x, y)
    swapCustom(&tuple.0, &tuple.1)
    return tuple.0 * tuple.1
  }

  expectEqual((4, 3), gradient(at: 3, 4, of: multiply_swapCustom))
  expectEqual((10, 5), gradient(at: 5, 10, of: multiply_swapCustom))
}

// Test functions returning tuples.
@differentiable(reverse)
func swapTuple(_ x: Float, _ y: Float) -> (Float, Float) {
  return (y, x)
}

@differentiable(reverse)
func swapTupleCustom(_ x: Float, _ y: Float) -> (Float, Float) {
  return (y, x)
}
@derivative(of: swapTupleCustom)
func vjpSwapTupleCustom(_ x: Float, _ y: Float) -> (
  value: (Float, Float), pullback: (Float, Float) -> (Float, Float)
) {
  return (swapTupleCustom(x, y), {v1, v2 in
    return (v2, v1)
  })
}

SimpleMathTests.test("ReturningTuples") {
  func multiply_swapTuple(_ x: Float, _ y: Float) -> Float {
    let result = swapTuple(x, y)
    return result.0 * result.1
  }

  expectEqual((4, 3), gradient(at: 3, 4, of: multiply_swapTuple))
  expectEqual((10, 5), gradient(at: 5, 10, of: multiply_swapTuple))

  func multiply_swapTupleCustom(_ x: Float, _ y: Float) -> Float {
    let result = swapTupleCustom(x, y)
    return result.0 * result.1
  }

  expectEqual((4, 3), gradient(at: 3, 4, of: multiply_swapTupleCustom))
  expectEqual((10, 5), gradient(at: 5, 10, of: multiply_swapTupleCustom))
}

SimpleMathTests.test("CaptureLocal") {
  let z: Float = 10
  func foo(_ x: Float) -> Float {
    return z * x
  }
  expectEqual(10, gradient(at: 0, of: foo))
}

var globalVar: Float = 10
SimpleMathTests.test("CaptureGlobal") {
  func foo(x: Float) -> Float {
    globalVar += 20
    return globalVar * x
  }
  expectEqual(30, gradient(at: 0, of: foo))
}

SimpleMathTests.test("Mutation") {
  func fourthPower(x: Float) -> Float {
    var a = x
    a = a * x
    a = a * x
    return a * x
  }
  expectEqual(4 * 27, gradient(at: 3, of: fourthPower))
}

SimpleMathTests.test("Tuple") {
  // TF-945: Nested tuple projections.
  func nested(_ x: Float) -> Float {
    var tuple = (1, 1, ((x, 1), 1))
    return tuple.2.0.0
  }
  expectEqual(1, gradient(at: 3, of: nested))
}

SimpleMathTests.test("TupleMutation") {
  func foo(_ x: Float) -> Float {
    var tuple = (x, x)
    tuple.0 = tuple.0 * x
    return x * tuple.0
  }
  expectEqual(27, gradient(at: 3, of: foo))

  func fifthPower(_ x: Float) -> Float {
    var tuple = (x, x)
    tuple.0 = tuple.0 * x
    tuple.1 = tuple.0 * x
    return tuple.0 * tuple.1
  }
  expectEqual(405, gradient(at: 3, of: fifthPower))

  func nested(_ x: Float) -> Float {
    var tuple = ((x, x), x)
    tuple.0.0 = tuple.0.0 * x
    tuple.0.1 = tuple.0.0 * x
    return tuple.0.0 * tuple.0.1
  }
  expectEqual(405, gradient(at: 3, of: nested))

  func generic<T: Differentiable & AdditiveArithmetic>(_ x: T) -> T {
    var tuple = (x, x)
    return tuple.0
  }
  expectEqual(1, gradient(at: 3.0, of: generic))

  // FIXME(TF-1033): Fix forward-mode ownership error for tuple with non-active
  // initial values.
  /*
  func genericInitialNonactive<T: Differentiable & AdditiveArithmetic>(
    _ x: T
  ) -> T {
    var tuple = (T.zero, T.zero)
    tuple.0 = x
    tuple.1 = x
    return tuple.0
  }
  expectEqual(1, gradient(at: 3.0, of: genericInitialNonactive))
  */
}

// Tests TF-321.
SimpleMathTests.test("TupleNonDifferentiableElements") {
  // TF-964: Test tuple with non-tuple-typed adjoint value.
  func tupleLet(_ x: Tracked<Float>) -> Tracked<Float> {
    let tuple = (2 * x, 1)
    return tuple.0
  }
  expectEqual((8, 2), valueWithGradient(at: 4, of: tupleLet))

  func tupleVar(_ x: Tracked<Float>) -> Tracked<Float> {
    var tuple = (x, 1)
    tuple.0 = x
    tuple.1 = 1
    return tuple.0
  }
  expectEqual((3, 1), valueWithGradient(at: 3, of: tupleVar))

  func nested(_ x: Tracked<Float>) -> Tracked<Float> {
    // Convoluted function computing `x * x`.
    var tuple: (Int, (Int, Tracked<Float>), Tracked<Float>) = (1, (1, 0), 0)
    tuple.0 = 1
    tuple.1.0 = 1
    tuple.1.1 = x
    tuple.2 = x
    return tuple.1.1 * tuple.2
  }
  expectEqual((16, 8), valueWithGradient(at: 4, of: nested))

  struct Wrapper<T> {
    @differentiable(reverse where T : Differentiable)
    func baz(_ x: T) -> T {
      var tuple = (1, 1, x, 1)
      tuple.0 = 1
      tuple.2 = x
      tuple.3 = 1
      return tuple.2
    }
  }
  func wrapper(_ x: Tracked<Float>) -> Tracked<Float> {
    let w = Wrapper<Tracked<Float>>()
    return w.baz(x)
  }
  expectEqual((3, 1), valueWithGradient(at: 3, of: wrapper))
}

// Tests TF-21.
SimpleMathTests.test("StructMemberwiseInitializer") {
  struct Foo : AdditiveArithmetic, Differentiable {
    var stored: Float
    var computed: Float {
      return stored * stored
    }
  }

  // Test direct `init` reference.
  expectEqual(10, pullback(at: 4, of: Foo.init)(.init(stored: 10)))

  let 𝛁foo = pullback(at: Float(4), of: { input -> Foo in
    let foo = Foo(stored: input)
    let foo2 = foo + foo
    return Foo(stored: foo2.stored)
  })(Foo.TangentVector(stored: 1))
  expectEqual(2, 𝛁foo)

  let 𝛁computed = gradient(at: Float(4)) { input -> Float in
    let foo = Foo(stored: input)
    return foo.computed
  }
  expectEqual(8, 𝛁computed)

  let 𝛁product = gradient(at: Float(4)) { input -> Float in
    let foo = Foo(stored: input)
    return foo.computed * foo.stored
  }
  expectEqual(48, 𝛁product)

  struct Custom : AdditiveArithmetic, Differentiable {
    var x: Float

    // Custom initializer with `@differentiable`.
    @differentiable(reverse)
    init(x: Float) {
      self.x = x
    }
  }

  let 𝛁custom = pullback(at: Float(4), of: { input -> Custom in
    let foo = Custom(x: input)
    return foo + foo
  })(Custom.TangentVector(x: 1))
  expectEqual(2, 𝛁custom)
}

// Tests TF-319: struct with non-differentiable constant stored property.
SimpleMathTests.test("StructConstantStoredProperty") {
  struct TF_319 : Differentiable {
    var x: Float
    @noDerivative let constant = Float(2)

    @differentiable(reverse)
    init(x: Float) {
      self.x = x
    }

    @differentiable(reverse, wrt: (self, input))
    func applied(to input: Float) -> Float {
      return x * constant * input
    }
  }
  func testStructInit(to input: Float) -> Float {
    let model = TF_319(x: 10)
    return model.applied(to: input)
  }
  expectEqual(TF_319.TangentVector(x: 6),
              gradient(at: TF_319(x: 10), of: { $0.applied(to: 3) }))
  expectEqual(20, gradient(at: 3, of: testStructInit))
}

SimpleMathTests.test("StructMutation") {
  struct Point : AdditiveArithmetic, Differentiable {
    var x: Float
    var y: Float
    var z: Float
  }

  func double(_ input: Float) -> Point {
    let point = Point(x: input, y: input, z: input)
    return point + point
  }
  expectEqual(6, pullback(at: 4, of: double)(Point(x: 1, y: 1, z: 1)))

  func fifthPower(_ input: Float) -> Float {
    var point = Point(x: input, y: input, z: input)
    point.x = point.x * input
    point.y = point.x * input
    return point.x * point.y
  }
  expectEqual(405, gradient(at: 3, of: fifthPower))

  func mix(_ input: Float) -> Float {
    var tuple = (point: Point(x: input, y: input, z: input), float: input)
    tuple.point.x = tuple.point.x * tuple.float
    tuple.point.y = tuple.point.x * input
    return tuple.point.x * tuple.point.y
  }
  expectEqual(405, gradient(at: 3, of: mix))

  // Test TF-282.
  struct Add : Differentiable {
    var bias: Float
    func applied(to input: Float) -> Float {
      var tmp = input
      tmp = tmp + bias
      return tmp
    }
  }
  let model = Add(bias: 1)
  expectEqual(Add.TangentVector(bias: 1),
              gradient(at: model) { m in m.applied(to: 1) })
}

SimpleMathTests.test("StructGeneric") {
  struct Generic<T : AdditiveArithmetic & Differentiable> : AdditiveArithmetic, Differentiable {
    var x: T
    var y: T
    var z: T
  }

  let 𝛁generic = pullback(at: Float(3), of: { input -> Generic<Float> in
    var generic = Generic(x: input, y: input, z: input)
    return generic
  })(Generic<Float>.TangentVector(x: 1, y: 1, z: 1))
  expectEqual(3, 𝛁generic)

  func fifthPower(_ input: Float) -> Float {
    var generic = Generic(x: input, y: input, z: input)
    generic.x = generic.x * input
    generic.y = generic.x * input
    return generic.x * generic.y
  }
  expectEqual(405, gradient(at: 3, of: fifthPower))
}

SimpleMathTests.test("StructWithNoDerivativeProperty") {
  struct NoDerivativeProperty : Differentiable {
    var x: Float
    @noDerivative var y: Float
  }
  expectEqual(
    NoDerivativeProperty.TangentVector(x: 1),
    gradient(at: NoDerivativeProperty(x: 1, y: 1)) { s -> Float in
      var tmp = s
      tmp.y = tmp.x
      return tmp.x
    }
  )
}

SimpleMathTests.test("SubsetIndices") {
  func grad(_ lossFunction: @differentiable(reverse) (Float, Float) -> Float) -> Float {
    return gradient(at: 1) { x in lossFunction(x * x, 10.0) }
  }
  expectEqual(2, grad { x, y in x + y })

  func gradWRTNonDiff(_ lossFunction: @differentiable(reverse) (Float, @noDerivative Int) -> Float) -> Float {
    return gradient(at: 2) { x in lossFunction(x * x, 10) }
  }
  expectEqual(4, gradWRTNonDiff { x, y in x + Float(y) })
}

SimpleMathTests.test("ForceUnwrapping") {
  func forceUnwrap<T: Differentiable & FloatingPoint>(_ t: T)
    -> (T, Float) where T == T.TangentVector {
    gradient(at: t, Float(1)) { (x, y) in
      (x as! Float) * y
    }
  }
  expectEqual((1, 2), forceUnwrap(Float(2)))
}

SimpleMathTests.test("Adjoint value accumulation for aggregate lhs and concrete rhs") {
  // TF-943: Test adjoint value accumulation for aggregate lhs and concrete rhs.
  struct SmallTestModel : Differentiable {
    public var stored: Float = 3.0
    @differentiable(reverse) public func callAsFunction() -> Float { return stored }
  }

  func doubled(_ model: SmallTestModel) -> Float{
    return model() + model.stored
  }
  let grads = gradient(at: SmallTestModel(), of: doubled)
  expectEqual(2.0, grads.stored)
}

// CHECK-LABEL: sil private [ossa] @${{.*}}doubled{{.*}}TJp{{.*}} : $@convention(thin) (Float, @owned {{.*}}) -> SmallTestModel.TangentVector {
// CHECK: bb0([[DX:%.*]] : $Float, [[PB0:%.*]] : {{.*}}, [[PB1:%.*]] : {{.*}}):
// CHECK:   [[ADJ_TUPLE:%.*]] = apply [[PB1]]([[DX]]) : $@callee_guaranteed (Float) -> (Float, Float)
// CHECK:   ([[TMP0:%.*]], [[ADJ_CONCRETE:%.*]]) = destructure_tuple [[ADJ_TUPLE]] : $(Float, Float)
// CHECK:   [[TMP1:%.*]] = apply [[PB0]]([[TMP0]]) : $@callee_guaranteed (Float) -> SmallTestModel.TangentVector
// CHECK:   [[ADJ_STRUCT_FIELD:%.*]] = destructure_struct [[TMP1]] : $SmallTestModel.TangentVector
// CHECK:   [[TMP_RES:%.*]] = alloc_stack $Float
// CHECK:   [[TMP_ADJ_STRUCT_FIELD:%.*]] = alloc_stack $Float
// CHECK:   [[TMP_ADJ_CONCRETE:%.*]] = alloc_stack $Float
// CHECK:   store [[ADJ_STRUCT_FIELD]] to [trivial] [[TMP_ADJ_STRUCT_FIELD]] : $*Float
// CHECK:   store [[ADJ_CONCRETE]] to [trivial] [[TMP_ADJ_CONCRETE]] : $*Float
// CHECK:   [[PLUS_EQUAL:%.*]] = witness_method $Float, #AdditiveArithmetic."+"
// CHECK:   %{{.*}} = apply [[PLUS_EQUAL]]<Float>([[TMP_RES]], [[TMP_ADJ_CONCRETE]], [[TMP_ADJ_STRUCT_FIELD]], {{.*}})
// CHECK:   [[RES:%.*]] = load [trivial] [[TMP_RES]] : $*Float
// CHECK:   [[RES_STRUCT:%.*]] = struct $SmallTestModel.TangentVector ([[RES]] : $Float)
// CHECK:   return [[RES_STRUCT]] : $SmallTestModel.TangentVector
// CHECK: }

runAllTests()
