// RUN: %target-swift-emit-sil -verify %s -warn-redundant-requirements | %FileCheck %s -check-prefix=CHECK-SIL

import _Differentiation

@_silgen_name("identity")
func identity<T : Differentiable>(_ x: T) -> T {
  return x
}
_ = gradient(at: Float(1), of: { x in identity(x) })

// Test PullbackCloner local buffer allocation.
// Verify that local buffers are immediately set to zero.

// CHECK-SIL-LABEL: sil private @identity16_Differentiation14DifferentiableRzlTJpSpSr
// CHECK-SIL:      [[ORIG_COTAN:%.*]] = alloc_stack $τ_0_0.TangentVector
// CHECK-SIL-NEXT: [[ZERO_WITNESS:%.*]] = witness_method $τ_0_0.TangentVector, #AdditiveArithmetic.zero!getter
// CHECK-SIL-NEXT: [[ORIG_COTAN_METATYPE:%.*]] = metatype $@thick τ_0_0.TangentVector.Type
// CHECK-SIL-NEXT: [[EMIT_ZERO_INDIRECT:%.*]] = apply [[ZERO_WITNESS]]<τ_0_0.TangentVector>([[ORIG_COTAN]], [[ORIG_COTAN_METATYPE]])
// CHECK-SIL: }

// Test TF-201: differentiate direct references to generic function.
// This involves reabstraction thunk differentiation.

_ = gradient(at: Float(1), of: identity)

protocol DifferentiableAdditiveArithmetic: Differentiable & AdditiveArithmetic {
  @differentiable(reverse)
  static func + (lhs: Self, rhs: Self) -> Self
}
extension Float: DifferentiableAdditiveArithmetic {}
func generic<T: DifferentiableAdditiveArithmetic>(_ x: T) -> T {
  x + x + x
}
_ = gradient(at: Float(10), of: generic)

struct Wrapper<Scalar : Differentiable> : Differentiable {
  var value: Scalar
  init(_ value: Scalar) { self.value = value }
}
func generic<T>(_ x: Wrapper<T>) -> T {
  return x.value
}
_ = gradient(at: Wrapper<Float>(1), of: generic)

func generic2<T: Differentiable, U: Differentiable>(_ x: T, _ y: Float, _ z: U) -> T {
  return x
}
func foo<T>(_ x: Wrapper<T>) {
  _ = gradient(at: Float(1), 2, x, of: generic2)
}

// Test case where associated derivative function's requirements are met.
extension Wrapper where Scalar : Numeric {
  @differentiable(reverse, wrt: self where Scalar : Differentiable & FloatingPoint) // expected-warning {{redundant conformance constraint 'Scalar' : 'Differentiable'}}
  // expected-warning@-1 {{redundant conformance constraint 'Scalar' : 'Numeric'}}
  func mean() -> Wrapper {
    return self
  }

  @differentiable(reverse, wrt: self where Scalar : Differentiable & FloatingPoint) // expected-warning {{redundant conformance constraint 'Scalar' : 'Differentiable'}}
  // expected-warning@-1 {{redundant conformance constraint 'Scalar' : 'Numeric'}}
  func variance() -> Wrapper {
    return mean() // ok
  }
}
_ = pullback(at: Wrapper<Float>(1), of: { $0.variance() })

// Tests TF-277.
protocol Layer : Differentiable {
  associatedtype Output : Differentiable
}
struct SupervisedTrainer<Model : Layer> {
  var model: Model
  var lossFunction: @differentiable(reverse) (Model.Output, Model.Output) -> Float
  func fit(y: Model.Output) {
    _ = gradient(at: y) { y in return self.lossFunction(y, y) }
  }
}

// Tests TF-440.
struct TF_440_Input<Input: Differentiable, State: Differentiable>
  : Differentiable {
  var input: Input
  var state: State
}
struct TF_440<T : Differentiable> {
  @differentiable(reverse)
  func applied(to input: TF_440_Input<Float, Float>) -> Float {
    return input.state
  }

  @differentiable(reverse)
  func applied(to input: TF_440_Input<T, Float>) -> Float {
    return input.state
  }

  @differentiable(reverse)
  func applied(to input: TF_440_Input<T, Float>) -> T {
    return input.input
  }
}

// Tests TF-508: differentiation requirements with dependent member types.
protocol TF_508_Proto {
  associatedtype Scalar
}
extension TF_508_Proto where Scalar : FloatingPoint {
  @differentiable(reverse
    where Self : Differentiable, Scalar : Differentiable,
          // Conformance requirement with dependent member type.
          Self.TangentVector : TF_508_Proto
  )
  static func +(lhs: Self, rhs: Self) -> Self {
    return lhs
  }

  @differentiable(reverse
    where Self : Differentiable, Scalar : Differentiable,
          // Same-type requirement with dependent member type.
          Self.TangentVector == Float
  )
  static func -(lhs: Self, rhs: Self) -> Self {
    return lhs
  }
}
extension TF_508_Proto where Self : Differentiable,
                             Scalar : FloatingPoint & Differentiable,
                             Self.TangentVector : TF_508_Proto {
  @derivative(of: +)
  static func vjpAdd(lhs: Self, rhs: Self)
      -> (value: Self, pullback: (TangentVector) -> (TangentVector, TangentVector)) {
    return (lhs, { v in (v, v) })
  }
}
extension TF_508_Proto where Self : Differentiable,
                             Scalar : FloatingPoint & Differentiable,
                             Self.TangentVector == Float {
  @derivative(of: -)
  static func vjpSubtract(lhs: Self, rhs: Self)
      -> (value: Self, pullback: (TangentVector) -> (TangentVector, TangentVector)) {
    return (lhs, { v in (v, v) })
  }
}

struct TF_508_Struct<Scalar : AdditiveArithmetic>
  : TF_508_Proto, AdditiveArithmetic {}
extension TF_508_Struct : Differentiable where Scalar : Differentiable {
  typealias TangentVector = TF_508_Struct
}

func TF_508() {
  let x = TF_508_Struct<Float>()
  // Test conformance requirement with dependent member type.
  _ = pullback(at: x, of: { (x: TF_508_Struct<Float>) -> TF_508_Struct<Float> in
    return x + x
  })
  // Test same-type requirement with dependent member type.
  _ = pullback(at: x, of: { (x: TF_508_Struct<Float>) -> TF_508_Struct<Float> in
    return x - x
  })
}

// TF-523
struct TF_523_Struct : Differentiable & AdditiveArithmetic {
  var a: Float = 1
  typealias TangentVector = TF_523_Struct
}

@differentiable(reverse)
func TF_523_f(_ x: TF_523_Struct) -> Float {
  return x.a * 2
}

// TF-534: Thunk substitution map remapping.
protocol TF_534_Layer : Differentiable {
  associatedtype Input : Differentiable
  associatedtype Output : Differentiable

  @differentiable(reverse)
  func callAsFunction(_ input: Input) -> Output
}
struct TF_534_Tensor<Scalar> : Differentiable {}

func TF_534<Model: TF_534_Layer>(
  _ model: inout Model, inputs: Model.Input
) -> TF_534_Tensor<Float> where Model.Output == TF_534_Tensor<Float> {
  return valueWithPullback(at: model) { model -> Model.Output in
    return model(inputs)
  }.0
}

// TF-546: Test that SILGen linear map thunk performs correct reabstraction.
struct TF_546<T: FloatingPoint>: AdditiveArithmetic {
  var real: T
  var imaginary: T

  @differentiable(reverse where T: Differentiable, T == T.TangentVector)
  init(real: T = 0, imaginary: T = 0) {
    self.real = real
    self.imaginary = imaginary
  }
}
extension TF_546: Differentiable where T: Differentiable {
  typealias TangentVector = TF_546
}
extension TF_546 where T: Differentiable, T == T.TangentVector {
  @derivative(of: init)
  static func _vjpInit(real: T, imaginary: T) -> (value: TF_546, pullback: (TF_546) -> (T, T)) {
    return (TF_546(real: real, imaginary: imaginary), { ($0.real, $0.imaginary) })
  }
}
let _: @differentiable(reverse) (Float, Float) -> TF_546<Float> = { r, i in
  TF_546(real: r, imaginary: i)
}

// TF-652: Test VJPCloner substitution map generic signature.
// The substitution map should have the VJP's generic signature, not the
// original function's.
struct TF_652<Scalar> {}
extension TF_652 : Differentiable where Scalar : FloatingPoint {}

@differentiable(reverse, wrt: x where Scalar: FloatingPoint) // expected-warning {{redundant conformance constraint 'Scalar' : 'Numeric'}}
func test<Scalar: Numeric>(x: TF_652<Scalar>) -> TF_652<Scalar> {
  for _ in 0..<10 {
    let _ = x
  }
  return x
}

// TF-682: Test that SILGen linear map thunk performs correct reabstraction.
protocol TF_682_Proto {
  associatedtype Scalar
}
extension TF_682_Proto where Scalar : FloatingPoint {
  @differentiable(reverse
    where Self : Differentiable, Scalar : Differentiable,
          // Same-type requirement with dependent member type.
          Self.TangentVector == Float
  )
  func foo(lhs: Self) -> Self {
    return lhs
  }
}
extension TF_682_Proto where Self : Differentiable,
                             Scalar : FloatingPoint & Differentiable,
                             Self.TangentVector == Float {
  @derivative(of: foo)
  func vjpFoo(lhs: Self) -> (
    value: Self, pullback: (TangentVector) -> (TangentVector, TangentVector)
  ) {
    return (lhs, { v in (v, v) })
  }
}

// NOTE(TF-1208): Differentiation regression due to changes in curry thunk generation.
/*
// TF-688: Test generic curry thunk cloning.
public struct TF_688_Struct<Scalar> {
  var x: Scalar
}
extension TF_688_Struct: Differentiable where Scalar: Differentiable {
  @differentiable(reverse)
  public static func id(x: Self) -> Self {
    return x
  }
}
@differentiable(reverse, wrt: x)
public func TF_688<Scalar: Differentiable>(
  _ x: TF_688_Struct<Scalar>,
  reduction: @differentiable(reverse) (TF_688_Struct<Scalar>) -> TF_688_Struct<Scalar> = TF_688_Struct.id
) -> TF_688_Struct<Scalar> {
  reduction(x)
}
*/

// TF-697: Test generic requirements of generated derivative function.
protocol TF_697_Module: Differentiable {
    associatedtype Input
    associatedtype Output: Differentiable

    @differentiable(reverse, wrt: self)
    func callModule(_ input: Input) -> Output
}
protocol TF_697_Layer: TF_697_Module where Input: Differentiable {
    @differentiable(reverse)
    func callLayer(_ input: Input) -> Output
}
struct TF_697_Sequential<Layer1: TF_697_Module, Layer2: TF_697_Layer>: TF_697_Module
    where Layer1.Output == Layer2.Input {
    var layer1: Layer1
    var layer2: Layer2

    @differentiable(reverse, wrt: self)
    func callModule(_ input: Layer1.Input) -> Layer2.Output {
        layer2.callLayer(layer1.callModule(input))
    }
}
extension TF_697_Sequential: TF_697_Layer where Layer1: TF_697_Layer { // expected-warning {{redundant conformance constraint 'Layer1' : 'TF_697_Module'}}
    @differentiable(reverse)
    func callLayer(_ input: Layer1.Input) -> Layer2.Output {
        layer2.callLayer(layer1.callLayer(input))
    }
}

// TF-817: Test remapping `apply` callee types in derivative function context.
struct TF_817<T> {
  func foo(_ index: Int) -> T {
    fatalError()
  }
}
extension TF_817: Differentiable where T: Differentiable {
  @derivative(of: foo)
  func vjpFoo(index: Int) -> (value: T, pullback: (T.TangentVector) -> (TangentVector)) {
    fatalError()
  }
}
extension TF_817 {
  @differentiable(reverse, wrt: self where T: Differentiable)
  public func test(index: Int) -> T {
    return self.foo(0) // crash happened here
  }
}

// TF-886: Test `partial_apply` of linear map subset parameters thunk.
@differentiable(reverse)
func TF_886_foo<T, U: Differentiable>(_: Float, _: T, _: U) -> Float {
  return 0
}
@differentiable(reverse)
func TF_886_bar<T>(x: Float, y: T) -> Float {
  return TF_886_foo(x, y, 0)
}

// Test layout requirements.

// The layout requirement is "contextual": the requirement is not on `T`, the
// differentiable function parameter/result type.
struct ContextualLayoutRequirement<T: Differentiable, U: AnyObject> {
  var stored: T
}
extension ContextualLayoutRequirement {
  func test(_ x: T) {
    let _: @differentiable(reverse) (T) -> T = { _ in self.stored }
    let _: @differentiable(reverse) (T) -> T = { $0 }
  }
}
// The layout requirement directly involves `T`, the differentiable function
// parameter/result type.
// TODO(TF-851): Uncomment the tests below after `@differentiable` function
// SILGen thunking is fixed.
/*
struct LayoutRequirement<T: AnyObject & Differentiable> {
  var stored: T
}
extension LayoutRequirement {
  func test(_ x: T) {
    let _: @differentiable(reverse) (T) -> T = { _ in self.stored }
    let _: @differentiable(reverse) (T) -> T = { $0 }
  }
}
*/

// Test superclass requirements.

class Super: Differentiable {}

// The superclass requirement is "contextual": the requirement is not on `T`,
// the differentiable function parameter/result type.
struct ContextualSuperclassRequirement<T: Differentiable, U: Super> {
  var stored: T
}
extension ContextualSuperclassRequirement {
  func test(_ x: T) {
    let _: @differentiable(reverse) (T) -> T = { _ in self.stored }
    let _: @differentiable(reverse) (T) -> T = { $0 }
  }
}
// The superclass requirement directly involves `T`, the differentiable
// function parameter/result type.
// TODO(TF-851): Uncomment the tests below after `@differentiable` function
// SILGen thunking is fixed.
/*
struct SuperclassRequirement<T: Super & Differentiable> {
  var stored: T
}
extension SuperclassRequirement {
  func test(_ x: T) {
    let _: @differentiable(reverse) (T) -> T = { _ in self.stored }
    let _: @differentiable(reverse) (T) -> T = { $0 }
  }
}
*/
