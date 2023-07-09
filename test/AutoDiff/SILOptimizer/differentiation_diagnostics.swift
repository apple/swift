// RUN: %target-swift-frontend -emit-sil -verify %s

// Test differentiation transform diagnostics.

import _Differentiation

//===----------------------------------------------------------------------===//
// Basic function
//===----------------------------------------------------------------------===//

@differentiable(reverse)
func basic(_ x: Float) -> Float {
  return x
}

//===----------------------------------------------------------------------===//
// Control flow
//===----------------------------------------------------------------------===//

@differentiable(reverse)
func conditional(_ x: Float, _ flag: Bool) -> Float {
  let y: Float
  if flag {
    y = x
  } else {
    y = x
  }
  return y
}

// TF-433: Test `try_apply` differentiation.

func throwing() throws -> Void {}

@differentiable(reverse)
func try_apply(_ x: Float) -> Float {
  try! throwing()
  return x
}

func rethrowing(_ x: () throws -> Void) rethrows -> Void {}

@differentiable(reverse)
func try_apply_rethrows(_ x: Float) -> Float {
  rethrowing({})
  return x
}

//===----------------------------------------------------------------------===//
// Unreachable
//===----------------------------------------------------------------------===//

// expected-error @+2 {{function is not differentiable}}
// expected-note @+2 {{when differentiating this function definition}}
@differentiable(reverse)
func noReturn(_ x: Float) -> Float {
  let _ = x
  // expected-error @+2 {{missing return in global function expected to return 'Float'}}
  // expected-note @+1 {{missing return for differentiation}}
}

//===----------------------------------------------------------------------===//
// Global variables
//===----------------------------------------------------------------------===//

var global: Float = 3.0

// expected-error @+2 {{function is not differentiable}}
// expected-note @+2 {{when differentiating this function definition}}
@differentiable(reverse, wrt: x)
func testWriteToGlobalVariable(x: Float) -> Float {
  // expected-note @+1 {{cannot differentiate writes to global variables}}
  global = global + x
  return global
}

//===----------------------------------------------------------------------===//
// Class differentiation
//===----------------------------------------------------------------------===//

class Class : Differentiable {
  @differentiable(reverse)
  var stored: Float = 1

  @differentiable(reverse)
  func testRefElementAddr(_ x: Float) -> Float {
    return stored * x
  }

  var nonDifferentiableStored: Float = 1

  @differentiable(reverse)
  func testNonDifferentiableRefElementAddr(_ x: Float) -> Float {
    // expected-error @+2 {{expression is not differentiable}}
    // expected-note @+1 {{member is not differentiable because the corresponding class member is not '@differentiable'}}
    return nonDifferentiableStored * x
  }

  @differentiable(reverse)
  func method(_ x: Float) -> Float { x }

  @differentiable(reverse)
  static func testClassMethod(x: Float) -> Float {
    return Class().method(x)
  }

  func nonDifferentiableMethod(_ x: Float) -> Float { x }

  @differentiable(reverse)
  static func testDifferentiableClassMethod(x: Float) -> Float {
    // expected-error @+2 {{expression is not differentiable}}
    // expected-note @+1 {{member is not differentiable because the corresponding class member is not '@differentiable'}}
    return Class().nonDifferentiableMethod(x)
  }
}

// TF-676: Test differentiation of class method with multiple `@differentiable`
// attributes.
class ClassMethodMultipleDifferentiableAttribute : Differentiable {
  @differentiable(reverse, wrt: (self, x))
  @differentiable(reverse, wrt: x)
  func f(_ x: Float) -> Float { x }
}
func testMultipleDiffAttrsClass<C: ClassMethodMultipleDifferentiableAttribute>(
  _ c: C, _ x: Float
) {
  _ = gradient(at: c, x) { c, x in c.f(x) }
  _ = gradient(at: x) { x in c.f(x) }
}

// TF-1149: Test class with loadable type but address-only `TangentVector` type.
class C<T: Differentiable>: Differentiable {
  @differentiable(reverse)
  var stored: T

  init(_ stored: T) {
    self.stored = stored
  }

  @differentiable(reverse)
  func method(_ x: T) -> T {
    stored
  }
}

//===----------------------------------------------------------------------===//
// Enum differentiation
//===----------------------------------------------------------------------===//

// expected-error @+1 {{function is not differentiable}}
@differentiable(reverse)
// expected-note @+1 {{when differentiating this function definition}}
func usesOptionals(_ x: Float) -> Float {
  var maybe: Float? = 10
  // expected-note @+1 {{expression is not differentiable}}
  maybe = x
  return maybe!
}

enum DirectEnum: Differentiable & AdditiveArithmetic {
  case leaf(Float)

  typealias TangentVector = Self

  static var zero: Self { fatalError() }
  static func +(_ lhs: Self, _ rhs: Self) -> Self { fatalError() }
  static func -(_ lhs: Self, _ rhs: Self) -> Self { fatalError() }
}

// expected-error @+1 {{function is not differentiable}}
@differentiable(reverse, wrt: e)
// expected-note @+2 {{when differentiating this function definition}}
// expected-note @+1 {{differentiating enum values is not yet supported}}
func enum_active(_ e: DirectEnum, _ x: Float) -> Float {
  switch e {
  case let .leaf(y): return y
  }
}

// expected-error @+1 {{function is not differentiable}}
@differentiable(reverse, wrt: e)
// expected-note @+2 {{when differentiating this function definition}}
// expected-note @+1 {{differentiating enum values is not yet supported}}
func activeEnumValue(_ e: DirectEnum, _ x: Float) -> Float {
  switch e {
  case let .leaf(y): return y
  }
}

enum IndirectEnum<T: Differentiable>: Differentiable & AdditiveArithmetic {
  case leaf(T)

  typealias TangentVector = Self

  static func ==(_ lhs: Self, _ rhs: Self) -> Bool { fatalError() }
  static var zero: Self { fatalError() }
  static func +(_ lhs: Self, _ rhs: Self) -> Self { fatalError() }
  static func -(_ lhs: Self, _ rhs: Self) -> Self { fatalError() }
}

// expected-error @+1 {{function is not differentiable}}
@differentiable(reverse, wrt: e)
// expected-note @+2 {{when differentiating this function definition}}
// expected-note @+1 {{differentiating enum values is not yet supported}}
func activeEnumAddr(_ e: IndirectEnum<Float>, _ x: Float) -> Float {
  switch e {
  case let .leaf(y): return y
  }
}

//===----------------------------------------------------------------------===//
// Unmet derivative generic requirements
//===----------------------------------------------------------------------===//

@differentiable(reverse)
func generic<T: Differentiable & FloatingPoint>(_ x: T) -> T {
  // expected-error @+2 {{expression is not differentiable}}
  // expected-note @+1 {{member is not differentiable because the corresponding protocol requirement is not '@differentiable'}}
  return x + 1
}

// Test unmet generic requirements.

func weird<T>(_ x: T) -> T {
  return x
}
@derivative(of: weird)
func vjpWeirdExtraRequirements<T : Differentiable & CaseIterable>(_ x: T) -> (
  value: T, pullback: (T.TangentVector) -> T.TangentVector
) where T.AllCases : ExpressibleByStringLiteral {
  return (x, { $0 })
}
@differentiable(reverse)
func weirdWrapper<T : Differentiable>(_ x: T) -> T {
  // expected-error @+2 {{expression is not differentiable}}
  // expected-note @+1 {{function call is not differentiable because generic requirements are not met: 'T : CaseIterable, T.AllCases : ExpressibleByStringLiteral'}}
  return weird(x)
}

@differentiable(reverse)
func direct<T : Differentiable>(_ x: T) -> T {
  return x
}

struct Tensor<Scalar> {
  static func + (_ lhs: Tensor, rhs: Scalar) -> Tensor { return lhs }
}
extension Tensor : Differentiable where Scalar : Differentiable & FloatingPoint {}
extension Tensor where Scalar : BinaryFloatingPoint {
  @differentiable(reverse, wrt: self where Scalar : Differentiable)
  func TF_6(_ x: Float) -> Tensor {
    return self + Scalar(x)
  }
}

protocol TF8_Proto : Differentiable {
  associatedtype Scalar
  @differentiable(reverse, wrt: (self, input))
  func applied(to input: Float) -> Float
}

struct TF8_Struct<Scalar> : TF8_Proto where Scalar : FloatingPoint & Differentiable {
  @noDerivative let bar: Scalar

  @differentiable(reverse, wrt: (self, input))
  func applied(to input: Float) -> Float {
    return input
  }
}

//===----------------------------------------------------------------------===//
// `Differentiable` conformance requirement inference
//===----------------------------------------------------------------------===//

func id<T>(_ x: T) -> T { x }
let _: @differentiable(reverse) (Float) -> Float = { x in id(x) }

struct TF_691<Scalar> {
  var x: Scalar
  init(_ x: Scalar) {
    self.x = x
  }
}
extension TF_691: Differentiable where Scalar: Differentiable {}

func identity<T>(_ x: TF_691<T>) -> TF_691<T> { x }
let _: @differentiable(reverse) (Float) -> TF_691<Float> = { x in identity(TF_691(x)) }
let _: @differentiable(reverse) (Float) -> TF_691<Float> = { x in id(TF_691(x)) }

//===----------------------------------------------------------------------===//
// Non-differentiable arguments and results
//===----------------------------------------------------------------------===//

struct TF_687<T> : Differentiable {
  @noDerivative var indirectDummy: T
  var base: Float

  init(_ base: Float, dummy: T) {
    self.base = base
    self.indirectDummy = dummy
  }
}
// expected-error @+2 {{function is not differentiable}}
// expected-note @+1 {{cannot differentiate through a non-differentiable argument; do you want to use 'withoutDerivative(at:)'?}} {{87-87=withoutDerivative(at: }} {{88-88=)}}
let _: @differentiable(reverse) (Float) -> TF_687<Any> = { x in TF_687<Any>(x, dummy: x) }

// expected-error @+1 {{function is not differentiable}}
@differentiable(reverse)
// expected-note @+1 {{when differentiating this function definition}}
func roundingGivesError(x: Float) -> Float {
  // expected-note @+1 {{cannot differentiate through a non-differentiable result; do you want to use 'withoutDerivative(at:)'?}} {{16-16=withoutDerivative(at: }} {{22-22=)}}
  return Float(Int(x))
}

//===----------------------------------------------------------------------===//
// Non-varied results
//===----------------------------------------------------------------------===//

@differentiable(reverse)
func nonVariedResult(_ x: Float) -> Float {
  // TODO(TF-788): Re-enable non-varied result warning.
  // xpected-warning @+1 {{result does not depend on differentiation arguments and will always have a zero derivative; do you want to use 'withoutDerivative(at:)'?}} {{10-10=withoutDerivative(at:}} {{15-15=)}}
  return 0
}

// Check that `withoutDerivative(at:)` silences the non-varied result warning.

struct TF_775: Differentiable {
  @differentiable(reverse, wrt: (self))
  func nonVariedResult(_ input: Float) -> Float {
    return withoutDerivative(at: input)
  }
}

//===----------------------------------------------------------------------===//
// Multiple results
//===----------------------------------------------------------------------===//

func multipleResults(_ x: Float) -> (Float, Float) {
  return (x, x)
}

@differentiable(reverse)
func usesMultipleResults(_ x: Float) -> Float {
  let tuple = multipleResults(x)
  return tuple.0 + tuple.1
}

//===----------------------------------------------------------------------===//
// `inout` parameter differentiation
//===----------------------------------------------------------------------===//

@differentiable(reverse)
func activeInoutParam(_ x: Float) -> Float {
  var result = x
  result += x
  return result
}

@differentiable(reverse)
func activeInoutParamNonactiveInitialResult(_ x: Float) -> Float {
  var result: Float = 1
  result += x
  return result
}

@differentiable(reverse)
func activeInoutParamTuple(_ x: Float) -> Float {
  var tuple = (x, x)
  tuple.0 *= x
  return x * tuple.0
}

@differentiable(reverse)
func activeInoutParamControlFlow(_ array: [Float]) -> Float {
  var result: Float = 1
  for i in withoutDerivative(at: array).indices {
    result += array[i]
  }
  return result
}

@differentiable(reverse)
func activeInoutParamControlFlowComplex(_ array: [Float], _ bool: Bool) -> Float {
  var result: Float = 1
  if bool {
    if bool {}
    for i in withoutDerivative(at: array).indices {
      switch i % 2 {
      case 0: continue
      case 1: break
      default: break
      }
      result = result + 1
      result += array[i]
    }
  }
  return result
}

struct Mut: Differentiable {}
extension Mut {
  @differentiable(reverse, wrt: x)
  mutating func mutatingMethod(_ x: Mut) {}
}

@differentiable(reverse, wrt: x)
func nonActiveInoutParam(_ nonactive: inout Mut, _ x: Mut) {
  nonactive.mutatingMethod(x)
}

@differentiable(reverse, wrt: x)
func activeInoutParamMutatingMethod(_ x: Mut) -> Mut {
  var result = x
  result.mutatingMethod(result)
  return result
}

@differentiable(reverse, wrt: x)
func activeInoutParamMutatingMethodVar(_ nonactive: inout Mut, _ x: Mut) {
  var result = nonactive
  result.mutatingMethod(x)
  nonactive = result
}

@differentiable(reverse, wrt: x)
func activeInoutParamMutatingMethodTuple(_ nonactive: inout Mut, _ x: Mut) {
  var result = (nonactive, x)
  result.0.mutatingMethod(result.0)
  nonactive = result.0
}

func twoInoutParameters(_ x: inout Float, _ y: inout Float) {}
@differentiable(reverse)
func testTwoInoutParameters(_ x: Float, _ y: Float) -> Float {
  var x = x
  var y = y
  twoInoutParameters(&x, &y)
  return x
}

func inoutParameterAndFormalResult(_ x: inout Float) -> Float { x }
@differentiable(reverse)
func testInoutParameterAndFormalResult(_ x: Float) -> Float {
  var x = x
  return inoutParameterAndFormalResult(&x)
}

//===----------------------------------------------------------------------===//
// Stored property access differentiation
//===----------------------------------------------------------------------===//

// Test differentiation of invalid stored property access instructions:
// `struct_extract`, `struct_element_addr`, `ref_element_addr`.

struct StructTangentVectorNotStruct: Differentiable {
  var x: Float

  enum TangentVector: Differentiable, AdditiveArithmetic {
    case x(Float)
    typealias TangentVector = Self
    static func ==(_: Self, _: Self) -> Bool { fatalError() }
    static var zero: Self { fatalError() }
    static func +(_: Self, _: Self) -> Self { fatalError() }
    static func -(_: Self, _: Self) -> Self { fatalError() }
  }
  mutating func move(by offset: TangentVector) {}
}

// expected-error @+2 {{function is not differentiable}}
// expected-note @+3 {{when differentiating this function definition}}
@differentiable(reverse)
@_silgen_name("test_struct_tangent_vector_not_struct")
func testStructTangentVectorNotStruct(_ s: StructTangentVectorNotStruct) -> Float {
  // expected-note @+1 {{cannot differentiate access to property 'StructTangentVectorNotStruct.x' because 'StructTangentVectorNotStruct.TangentVector' is not a struct}}
  return s.x
}

// CHECK-LABEL: sil {{.*}} @test_struct_tangent_vector_not_struct
// CHECK: struct_extract {{%.*}} : $StructTangentVectorNotStruct, #StructTangentVectorNotStruct.x

struct StructOriginalPropertyNotDifferentiable: Differentiable {
  struct Nondiff {
    var x: Float
  }
  var nondiff: Nondiff

  struct TangentVector: Differentiable & AdditiveArithmetic {
    var nondiff: Float
  }
  mutating func move(by offset: TangentVector) {}
}

// expected-error @+2 {{function is not differentiable}}
// expected-note @+3 {{when differentiating this function definition}}
@differentiable(reverse)
@_silgen_name("test_struct_original_property_not_differentiable")
func testStructOriginalPropertyNotDifferentiable(_ s: StructOriginalPropertyNotDifferentiable) -> Float {
  // expected-note @+1 {{cannot differentiate access to property 'StructOriginalPropertyNotDifferentiable.nondiff' because property type 'StructOriginalPropertyNotDifferentiable.Nondiff' does not conform to 'Differentiable'}}
  return s.nondiff.x
}

// CHECK-LABEL: sil {{.*}} @test_struct_original_property_not_differentiable
// CHECK: struct_extract {{%.*}} : $StructOriginalPropertyNotDifferentiable, #StructOriginalPropertyNotDifferentiable.nondiff

struct StructTangentVectorPropertyNotFound: Differentiable {
  var x: Float

  struct TangentVector: Differentiable, AdditiveArithmetic {
    var y: Float
  }
  mutating func move(by offset: TangentVector) {}
}

// expected-error @+2 {{function is not differentiable}}
// expected-note @+3 {{when differentiating this function definition}}
@differentiable(reverse)
@_silgen_name("test_struct_tangent_property_not_found")
func testStructTangentPropertyNotFound(_ s: StructTangentVectorPropertyNotFound) -> Float {
  // expected-warning @+1 {{variable 'tmp' was never mutated}}
  var tmp = s
  // expected-note @+1 {{cannot differentiate access to property 'StructTangentVectorPropertyNotFound.x' because 'StructTangentVectorPropertyNotFound.TangentVector' does not have a stored property named 'x'}}
  return tmp.x
}

// CHECK-LABEL: sil {{.*}} @test_struct_tangent_property_not_found
// CHECK: struct_element_addr {{%.*}} : $*StructTangentVectorPropertyNotFound, #StructTangentVectorPropertyNotFound.x

struct StructTangentPropertyWrongType: Differentiable {
  var x: Float

  struct TangentVector: Differentiable, AdditiveArithmetic {
    var x: Double
  }
  mutating func move(by offset: TangentVector) {}
}

// expected-error @+2 {{function is not differentiable}}
// expected-note @+3 {{when differentiating this function definition}}
@differentiable(reverse)
@_silgen_name("test_struct_tangent_property_wrong_type")
func testStructTangentPropertyWrongType(_ s: StructTangentPropertyWrongType) -> Float {
  // expected-warning @+1 {{variable 'tmp' was never mutated}}
  var tmp = s
  // expected-note @+1 {{cannot differentiate access to property 'StructTangentPropertyWrongType.x' because 'StructTangentPropertyWrongType.TangentVector.x' does not have expected type 'Float.TangentVector' (aka 'Float')}}
  return tmp.x
}

// CHECK-LABEL: sil {{.*}} @test_struct_tangent_property_wrong_type
// CHECK: struct_element_addr {{%.*}} : $*StructTangentPropertyWrongType, #StructTangentPropertyWrongType.x

final class ClassTangentPropertyWrongType: Differentiable {
  var x: Float = 0

  struct TangentVector: Differentiable, AdditiveArithmetic {
    var x: Double
  }
  func move(by offset: TangentVector) {}
}

// expected-error @+2 {{function is not differentiable}}
// expected-note @+3 {{when differentiating this function definition}}
@differentiable(reverse)
@_silgen_name("test_class_tangent_property_wrong_type")
func testClassTangentPropertyWrongType(_ c: ClassTangentPropertyWrongType) -> Float {
  // expected-warning @+1 {{variable 'tmp' was never mutated}}
  var tmp = c
  // expected-note @+1 {{cannot differentiate access to property 'ClassTangentPropertyWrongType.x' because 'ClassTangentPropertyWrongType.TangentVector.x' does not have expected type 'Float.TangentVector' (aka 'Float')}}
  return tmp.x
}

// CHECK-LABEL: sil {{.*}} @test_class_tangent_property_wrong_type
// CHECK: ref_element_addr {{%.*}} : $ClassTangentPropertyWrongType, #ClassTangentPropertyWrongType.x

struct StructTangentPropertyNotStored: Differentiable {
  var x: Float

  struct TangentVector: Differentiable, AdditiveArithmetic {
    var x: Float { 0 }
  }
  mutating func move(by offset: TangentVector) {}
}

// expected-error @+2 {{function is not differentiable}}
// expected-note @+3 {{when differentiating this function definition}}
@differentiable(reverse)
@_silgen_name("test_struct_tangent_property_not_stored")
func testStructTangentPropertyNotStored(_ s: StructTangentPropertyNotStored) -> Float {
  // expected-warning @+1 {{variable 'tmp' was never mutated}}
  var tmp = s
  // expected-note @+1 {{cannot differentiate access to property 'StructTangentPropertyNotStored.x' because 'StructTangentPropertyNotStored.TangentVector.x' is not a stored property}}
  return tmp.x
}

// CHECK-LABEL: sil {{.*}} @test_struct_tangent_property_not_stored
// CHECK: struct_element_addr {{%.*}} : $*StructTangentPropertyNotStored, #StructTangentPropertyNotStored.x

final class ClassTangentPropertyNotStored: Differentiable {
  var x: Float = 0

  struct TangentVector: Differentiable, AdditiveArithmetic {
    var x: Float { 0 }
  }
  func move(by offset: TangentVector) {}
}

// expected-error @+2 {{function is not differentiable}}
// expected-note @+3 {{when differentiating this function definition}}
@differentiable(reverse)
@_silgen_name("test_class_tangent_property_not_stored")
func testClassTangentPropertyNotStored(_ c: ClassTangentPropertyNotStored) -> Float {
  // expected-warning @+1 {{variable 'tmp' was never mutated}}
  var tmp = c
  // expected-note @+1 {{cannot differentiate access to property 'ClassTangentPropertyNotStored.x' because 'ClassTangentPropertyNotStored.TangentVector.x' is not a stored property}}
  return tmp.x
}

// CHECK-LABEL: sil {{.*}} @test_class_tangent_property_not_stored
// CHECK: ref_element_addr {{%.*}} : $ClassTangentPropertyNotStored, #ClassTangentPropertyNotStored.x

// https://github.com/apple/swift/issues/55580
// Test stored property access with conditionally `Differentiable` base type.

struct Complex<T: FloatingPoint> {
  var real: T
  var imaginary: T
}
extension Complex: Differentiable where T: Differentiable {
  typealias TangentVector = Complex
}
extension Complex: AdditiveArithmetic {}

@differentiable(reverse)
func f_55580(lhs: Complex<Float>, rhs: Complex<Float>) -> Float {
  return lhs.real + rhs.real
}

//===----------------------------------------------------------------------===//
// Wrapped property differentiation
//===----------------------------------------------------------------------===//

@propertyWrapper
struct Wrapper<Value> {
  private var value: Value
  var wrappedValue: Value {
    get { value }
    set { value = newValue }
  }
  var projectedValue: Self { self }

  init(wrappedValue: Value) {
    self.value = wrappedValue
  }
}

@propertyWrapper
struct DifferentiableWrapper<Value> {
  private var value: Value
  var wrappedValue: Value {
    get { value }
    set { value = newValue }
  }
  var projectedValue: Self { self }

  init(wrappedValue: Value) {
    self.value = wrappedValue
  }
}
extension DifferentiableWrapper: Differentiable where Value: Differentiable {}
// Note: property wrapped value differentiation works even if wrapper types do
// not conform to `Differentiable`. The conformance here tests projected value
// accesses.

struct Struct: Differentiable {
  // expected-error @+2 {{expression is not differentiable}}
  // expected-note @+1 {{cannot differentiate access to property 'Struct._x' because 'Struct.TangentVector' does not have a stored property named '_x'}}
  @DifferentiableWrapper @DifferentiableWrapper var x: Float = 10

  @Wrapper var y: Float = 20
  var z: Float = 30
}

@differentiable(reverse)
func differentiableProjectedValueAccess(_ s: Struct) -> Float {
  s.$x.wrappedValue.wrappedValue
}

// expected-error @+2 {{function is not differentiable}}
// expected-note @+2 {{when differentiating this function definition}}
@differentiable(reverse)
func projectedValueAccess(_ s: Struct) -> Float {
  // expected-note @+1 {{cannot differentiate through a non-differentiable result; do you want to use 'withoutDerivative(at:)'?}} {{3-3=withoutDerivative(at: }} {{7-7=)}}
  s.$y.wrappedValue
}

// https://github.com/apple/swift/issues/55084
// Test `wrapperValue.modify` differentiation.

// expected-error @+2 {{function is not differentiable}}
// expected-note @+2 {{when differentiating this function definition}}
@differentiable(reverse)
func modify(_ s: Struct, _ x: Float) -> Float {
  var s = s
  // expected-note @+1 {{differentiation of coroutine calls is not yet supported}}
  s.x *= x * s.z
  return s.x
}

//===----------------------------------------------------------------------===//
// Array literal initialization
//===----------------------------------------------------------------------===//

// expected-error @+1 {{function is not differentiable}}
@differentiable(reverse)
// expected-note @+1 {{when differentiating this function definition}}
func tupleArrayLiteralInitialization(_ x: Float, _ y: Float) -> Float {
  // `Array<(Float, Float)>` does not conform to `Differentiable`.
  let array = [(x * y, x * y)]
  // expected-note @-1 {{cannot differentiate through a non-differentiable argument; do you want to use 'withoutDerivative(at:)'?}} {{15-15=withoutDerivative(at: }} {{31-31=)}}
  return array[0].0
}

//===----------------------------------------------------------------------===//
// Subset parameter differentiation thunks
//===----------------------------------------------------------------------===//

// FIXME: Non-differentiability diagnostic crash due to invalid source location (https://github.com/apple/swift/issues/55492).
/*
func testNoDerivativeParameter(_ f: @differentiable(reverse) (Float, @noDerivative Float) -> Float) -> Float {
  return gradient(at: 2) { x in f(x * x, x) }
}
*/

// Test parameter subset thunk + partially-applied original function.
struct TF_675 : Differentiable {
  @differentiable(reverse)
  func method(_ x: Float) -> Float {
    return x
  }
}
let _: @differentiable(reverse) (Float) -> Float = TF_675().method

// TF-918: Test parameter subset thunk + partially-applied original function.
let _: @differentiable(reverse) (Float, @noDerivative Float) -> Float = (+) as @differentiable(reverse) (Float, Float) -> Float
let _: @differentiable(reverse) (Float, @noDerivative Float) -> Float = (+) as @differentiable(reverse) (Float, @noDerivative Float) -> Float

//===----------------------------------------------------------------------===//
// Differentiation in fragile functions
//===----------------------------------------------------------------------===//

public func implicitlyDifferentiableFromFragile(_ x: Float) -> Float { x }

public func hasImplicitlyDifferentiatedTopLevelDefaultArgument(
// expected-error @+2 {{function is not differentiable}}
// expected-note @+1 {{differentiated functions in default arguments must be marked '@differentiable' or have a public '@derivative'}}
  _ f: @differentiable(reverse) (Float) -> Float = implicitlyDifferentiableFromFragile
) {}

// TODO(TF-1030): This will eventually not be an error.
// expected-error @+2 {{function is not differentiable}}
// expected-note @+1 {{differentiated functions in default arguments must be marked '@differentiable' or have a public '@derivative'; this is not possible with a closure, make a top-level function instead}}
public func hasImplicitlyDifferentiatedClosureDefaultArgument(_ f: @differentiable(reverse) (Float) -> Float = { $0 }) {}

@inlinable
public func fragileFuncWithGradient() {
  // expected-error @+2 {{function is not differentiable}}
  // expected-note @+1 {{differentiated functions in '@inlinable' functions must be marked '@differentiable' or have a public '@derivative'}}
  _ = gradient(at: 0, of: implicitlyDifferentiableFromFragile)
}

@inlinable
@differentiable(reverse)
public func fragileDifferentiable(_ x: Float) -> Float {
  // expected-error @+2 {{expression is not differentiable}}
  // expected-note @+1 {{differentiated functions in '@inlinable' functions must be marked '@differentiable' or have a public '@derivative'}}
  implicitlyDifferentiableFromFragile(x)
}


// FIXME: Differentiable curry thunk RequirementMachine error (rdar://87429620, https://github.com/apple/swift/issues/54819).
#if false
// TF-1208: Test curry thunk differentiation regression.
public struct Struct_54819<Scalar> {
  var x: Scalar
}
extension Struct_54819: Differentiable where Scalar: Differentiable {
  @differentiable(reverse)
  public static func id(x: Self) -> Self {
    return x
  }
}
@differentiable(reverse, wrt: x)
public func f_54819<Scalar: Differentiable>(
  _ x: Struct_54819<Scalar>,
  // NOTE(TF-1208): This diagnostic is unexpected because `Struct_54819.id` is marked `@differentiable`.
  // xpected-error @+3 2 {{function is not differentiable}}
  // xpected-note @+2 {{differentiated functions in '@inlinable' functions must be marked '@differentiable' or have a public '@derivative'; this is not possible with a closure, make a top-level function instead}}
  // xpected-note @+1 {{opaque non-'@differentiable' function is not differentiable}}
  reduction: @differentiable(reverse) (Struct_54819<Scalar>) -> Struct_54819<Scalar> = Struct_54819.id
) -> Struct_54819<Scalar> {
  reduction(x)
}
#endif

//===----------------------------------------------------------------------===//
// Coroutines (SIL function yields, `begin_apply`) (not yet supported)
//===----------------------------------------------------------------------===//

struct HasCoroutineAccessors: Differentiable {
  var stored: Float
  var computed: Float {
    // `_read` is a coroutine: `(Self) -> () -> ()`.
    _read { yield stored }
    // `_modify` is a coroutine: `(inout Self) -> () -> ()`.
    _modify { yield &stored }
  }
}
// expected-error @+2 {{function is not differentiable}}
// expected-note @+2 {{when differentiating this function definition}}
@differentiable(reverse)
func testAccessorCoroutines(_ x: HasCoroutineAccessors) -> HasCoroutineAccessors {
  var x = x
  // expected-note @+1 {{differentiation of coroutine calls is not yet supported}}
  x.computed = x.computed
  return x
}

// TF-1078: Diagnose `_modify` accessor application with active `inout` argument.
// expected-error @+2 {{function is not differentiable}}
// expected-note @+2 {{when differentiating this function definition}}
@differentiable(reverse)
func TF_1078(array: [Float], x: Float) -> Float {
  var array = array
  // Array subscript assignment below calls `Array.subscript.modify`.
  // expected-note @+1 {{differentiation of coroutine calls is not yet supported}}
  array[0] = x
  return array[0]
}

// TF-1115: Diagnose `_modify` accessor application with initially non-active `inout` argument.
// expected-error @+2 {{function is not differentiable}}
// expected-note @+2 {{when differentiating this function definition}}
@differentiable(reverse)
func TF_1115(_ x: Float) -> Float {
  var array: [Float] = [0]
  // Array subscript assignment below calls `Array.subscript.modify`.
  // expected-note @+1 {{differentiation of coroutine calls is not yet supported}}
  array[0] = x
  return array[0]
}

// TF-1115: Test `_modify` accessor application with initially non-active `inout` argument,
// where the yielded value is not a projection from `self`.
extension Float {
  static var staticProperty: Float = 1

  var projection: Float {
    get { self }
    // This `modify` accessor yields a static variable, not a projection from `self`.
    // Diagnosing active applications is nonetheless a safe over-approximation.
    _modify { yield &Float.staticProperty }
  }
}

// expected-error @+2 {{function is not differentiable}}
// expected-note @+2 {{when differentiating this function definition}}
@differentiable(reverse)
func TF_1115_modifyNonSelfProjection(x: Float) -> Float {
  var result: Float = 0
  // Assignment below calls `Float.projection.modify`.
  // expected-note @+1 {{differentiation of coroutine calls is not yet supported}}
  result.projection = x
  return result
}

//===----------------------------------------------------------------------===//
// Conversion to `@differentiable(_linear)` (not yet supported)
//===----------------------------------------------------------------------===//

// expected-error @+1 {{conversion to '@differentiable(_linear)' function type is not yet supported}}
let _: @differentiable(_linear) (Float) -> Float = { x in x }
