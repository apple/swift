// RUN: %target-swift-frontend-typecheck -verify -disable-availability-checking %s
// RUN: %target-swift-frontend-typecheck -enable-testing -verify -disable-availability-checking %s

import _Differentiation

// Dummy `Differentiable`-conforming type.
public struct DummyTangentVector: Differentiable & AdditiveArithmetic {
  public static var zero: Self { Self() }
  public static func + (_: Self, _: Self) -> Self { Self() }
  public static func - (_: Self, _: Self) -> Self { Self() }
  public typealias TangentVector = Self
}

@differentiable(reverse) // expected-error {{'@differentiable' attribute cannot be applied to this declaration}}
let globalConst: Float = 1

@differentiable(reverse) // expected-error {{'@differentiable' attribute cannot be applied to this declaration}}
var globalVar: Float = 1

func testLocalVariables() {
  // expected-error @+1 {{'_' has no parameters to differentiate with respect to}}
  @differentiable(reverse)
  var getter: Float {
    return 1
  }

  // expected-error @+1 {{'_' has no parameters to differentiate with respect to}}
  @differentiable(reverse)
  var getterSetter: Float {
    get { return 1 }
    set {}
  }
}

@differentiable(reverse) // expected-error {{'@differentiable' attribute cannot be applied to this declaration}}
protocol P {}

@differentiable(reverse) // ok!
func no_jvp_or_vjp(_ x: Float) -> Float {
  return x * x
}

// Test duplicate `@differentiable` attributes.

@differentiable(reverse) // expected-error {{duplicate '@differentiable' attribute with same parameters}}
@differentiable(reverse) // expected-note {{other attribute declared here}}
func dupe_attributes(arg: Float) -> Float { return arg }

@differentiable(reverse, wrt: arg1)
@differentiable(reverse, wrt: arg2) // expected-error {{duplicate '@differentiable' attribute with same parameters}}
@differentiable(reverse, wrt: arg2) // expected-note {{other attribute declared here}}
func dupe_attributes(arg1: Float, arg2: Float) -> Float { return arg1 }

struct ComputedPropertyDupeAttributes<T: Differentiable>: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(by _: TangentVector) {}

  var value: T

  @differentiable(reverse) // expected-note {{other attribute declared here}}
  var computed1: T {
    @differentiable(reverse) // expected-error {{duplicate '@differentiable' attribute with same parameters}}
    get { value }
    set { value = newValue }
  }

  // TODO(TF-482): Remove diagnostics when `@differentiable` attributes are
  // also uniqued based on generic requirements.
  @differentiable(reverse where T == Float) // expected-error {{duplicate '@differentiable' attribute with same parameters}}
  @differentiable(reverse where T == Double) // expected-note {{other attribute declared here}}
  var computed2: T {
    get { value }
    set { value = newValue }
  }
}

// Test TF-568.
protocol WrtOnlySelfProtocol: Differentiable {
  @differentiable(reverse)
  var computedProperty: Float { get }

  @differentiable(reverse)
  func method() -> Float
}

class Class: Differentiable {
  typealias TangentVector = DummyTangentVector
  func move(by _: TangentVector) {}
}
@differentiable(reverse, wrt: x)
func invalidDiffWrtClass(_ x: Class) -> Class {
  return x
}

protocol Proto {}
// expected-error @+1 {{can only differentiate functions with results that conform to 'Differentiable', but 'any Proto' does not conform to 'Differentiable'}}
@differentiable(reverse, wrt: x)
func invalidDiffWrtExistential(_ x: Proto) -> Proto {
  return x
}

// expected-error @+1 {{can only differentiate with respect to parameters that conform to 'Differentiable', but '@differentiable(reverse) (Float) -> Float' does not conform to 'Differentiable'}}
@differentiable(reverse, wrt: fn)
func invalidDiffWrtFunction(_ fn: @differentiable(reverse) (Float) -> Float) -> Float {
  return fn(.pi)
}

// expected-error @+1 {{'invalidDiffNoParams()' has no parameters to differentiate with respect to}}
@differentiable(reverse)
func invalidDiffNoParams() -> Float {
  return 1
}

// expected-error @+1 {{cannot differentiate void function 'invalidDiffVoidResult(x:)'}}
@differentiable(reverse)
func invalidDiffVoidResult(x: Float) {}

// Test static methods.
struct StaticMethod {
  // expected-error @+1 {{'invalidDiffNoParams()' has no parameters to differentiate with respect to}}
  @differentiable(reverse)
  static func invalidDiffNoParams() -> Float {
    return 1
  }

  // expected-error @+1 {{cannot differentiate void function 'invalidDiffVoidResult(x:)'}}
  @differentiable(reverse)
  static func invalidDiffVoidResult(x: Float) {}
}

// Test instance methods.
struct InstanceMethod {
  // expected-error @+1 {{'invalidDiffNoParams()' has no parameters to differentiate with respect to}}
  @differentiable(reverse)
  func invalidDiffNoParams() -> Float {
    return 1
  }

  // expected-error @+1 {{cannot differentiate void function 'invalidDiffVoidResult(x:)'}}
  @differentiable(reverse)
  func invalidDiffVoidResult(x: Float) {}
}

// Test instance methods for a `Differentiable` type.
struct DifferentiableInstanceMethod: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(by _: TangentVector) {}

  @differentiable(reverse) // ok
  func noParams() -> Float {
    return 1
  }
}

// Test subscript methods.
struct SubscriptMethod: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(by _: TangentVector) {}

  @differentiable(reverse) // ok
  subscript(implicitGetter x: Float) -> Float {
    return x
  }

  @differentiable(reverse) // ok
  subscript(implicitGetterSetter x: Float) -> Float {
    get { return x }
    set {}
  }

  subscript(explicit x: Float) -> Float {
    @differentiable(reverse) // ok
    get { return x }
    @differentiable(reverse)
    set {}
  }

  subscript(x: Float, y: Float) -> Float {
    @differentiable(reverse) // ok
    get { return x + y }
    @differentiable(reverse)
    set {}
  }
}

// expected-error @+3 {{type 'Scalar' constrained to non-protocol, non-class type 'Float'}}
// expected-error @+2 {{no differentiation parameters could be inferred; must differentiate with respect to at least one parameter conforming to 'Differentiable'}}
// expected-note @+1 {{use 'Scalar == Float' to require 'Scalar' to be 'Float'}}
@differentiable(reverse where Scalar: Float)
func invalidRequirementConformance<Scalar>(x: Scalar) -> Scalar {
  return x
}

// expected-error @+1 {{'@differentiable' attribute does not yet support layout requirements}}
@differentiable(reverse where T: AnyObject)
func invalidAnyObjectRequirement<T: Differentiable>(x: T) -> T {
  return x
}

// expected-error @+1 {{'@differentiable' attribute does not yet support layout requirements}}
@differentiable(reverse where Scalar: _Trivial)
func invalidRequirementLayout<Scalar>(x: Scalar) -> Scalar {
  return x
}

// expected-error @+1 {{no differentiation parameters could be inferred; must differentiate with respect to at least one parameter conforming to 'Differentiable'}}
@differentiable(reverse)
func missingConformance<T>(_ x: T) -> T {
  return x
}

protocol ProtocolRequirements: Differentiable {
  // expected-note @+2 {{protocol requires initializer 'init(x:y:)' with type '(x: Float, y: Float)'}}
  @differentiable(reverse)
  init(x: Float, y: Float)

  // expected-note @+2 {{protocol requires initializer 'init(x:y:)' with type '(x: Float, y: Int)'}}
  @differentiable(reverse, wrt: x)
  init(x: Float, y: Int)

  // expected-note @+2 {{protocol requires function 'amb(x:y:)' with type '(Float, Float) -> Float';}}
  @differentiable(reverse)
  func amb(x: Float, y: Float) -> Float

  // expected-note @+2 {{protocol requires function 'amb(x:y:)' with type '(Float, Int) -> Float';}}
  @differentiable(reverse, wrt: x)
  func amb(x: Float, y: Int) -> Float

  // expected-note @+3 {{protocol requires function 'f1'}}
  // expected-note @+2 {{overridden declaration is here}}
  @differentiable(reverse, wrt: (self, x))
  func f1(_ x: Float) -> Float

  // expected-note @+2 {{protocol requires function 'f2'}}
  @differentiable(reverse, wrt: (self, x, y))
  func f2(_ x: Float, _ y: Float) -> Float
}

protocol ProtocolRequirementsRefined: ProtocolRequirements {
  // expected-error @+1 {{overriding declaration is missing attribute '@differentiable(reverse)'}}
  func f1(_ x: Float) -> Float
}

// Test missing `@differentiable` attribute for internal protocol witnesses.
// No errors expected; internal `@differentiable` attributes are created.

struct InternalDiffAttrConformance: ProtocolRequirements {
  typealias TangentVector = DummyTangentVector
  mutating func move(by _: TangentVector) {}

  var x: Float
  var y: Float

  init(x: Float, y: Float) {
    self.x = x
    self.y = y
  }

  init(x: Float, y: Int) {
    self.x = x
    self.y = Float(y)
  }

  func amb(x: Float, y: Float) -> Float {
    return x
  }

  func amb(x: Float, y: Int) -> Float {
    return x
  }

  func f1(_ x: Float) -> Float {
    return x
  }

  @differentiable(reverse, wrt: (self, x))
  func f2(_ x: Float, _ y: Float) -> Float {
    return x + y
  }
}

// Test missing `@differentiable` attribute for public protocol witnesses. Errors expected.

// expected-error @+1 {{does not conform to protocol 'ProtocolRequirements'}}
public struct PublicDiffAttrConformance: ProtocolRequirements {
  public typealias TangentVector = DummyTangentVector
  public mutating func move(by _: TangentVector) {}

  var x: Float
  var y: Float

  // FIXME(TF-284): Fix unexpected diagnostic.
  // expected-note @+2 {{candidate is missing explicit '@differentiable(reverse)' attribute to satisfy requirement}} {{10-10=@differentiable(reverse) }}
  // expected-note @+1 {{candidate has non-matching type '(x: Float, y: Float)'}}
  public init(x: Float, y: Float) {
    self.x = x
    self.y = y
  }

  // FIXME(TF-284): Fix unexpected diagnostic.
  // expected-note @+2 {{candidate is missing explicit '@differentiable(reverse)' attribute to satisfy requirement}} {{10-10=@differentiable(reverse) }}
  // expected-note @+1 {{candidate has non-matching type '(x: Float, y: Int)'}}
  public init(x: Float, y: Int) {
    self.x = x
    self.y = Float(y)
  }

  // expected-note @+2 {{candidate is missing explicit '@differentiable(reverse)' attribute to satisfy requirement}} {{10-10=@differentiable(reverse) }}
  // expected-note @+1 {{candidate has non-matching type '(Float, Float) -> Float'}}
  public func amb(x: Float, y: Float) -> Float {
    return x
  }

  // expected-note @+2 {{candidate is missing explicit '@differentiable(reverse, wrt: x)' attribute to satisfy requirement}} {{10-10=@differentiable(reverse, wrt: x) }}
  // expected-note @+1 {{candidate has non-matching type '(Float, Int) -> Float'}}
  public func amb(x: Float, y: Int) -> Float {
    return x
  }

  // expected-note @+1 {{candidate is missing explicit '@differentiable(reverse)' attribute to satisfy requirement}}
  public func f1(_ x: Float) -> Float {
    return x
  }

  // expected-note @+2 {{candidate is missing explicit '@differentiable(reverse)' attribute to satisfy requirement}}
  @differentiable(reverse, wrt: (self, x))
  public func f2(_ x: Float, _ y: Float) -> Float {
    return x + y
  }
}

protocol ProtocolRequirementsWithDefault_NoConformingTypes {
  @differentiable(reverse)
  func f1(_ x: Float) -> Float
}
extension ProtocolRequirementsWithDefault_NoConformingTypes {
  // TODO(TF-650): It would be nice to diagnose protocol default implementation
  // with missing `@differentiable` attribute.
  func f1(_ x: Float) -> Float { x }
}

protocol ProtocolRequirementsWithDefault {
  @differentiable(reverse)
  func f1(_ x: Float) -> Float
}
extension ProtocolRequirementsWithDefault {
  func f1(_ x: Float) -> Float { x }
}
struct DiffAttrConformanceErrors2: ProtocolRequirementsWithDefault {
  func f1(_ x: Float) -> Float { x }
}

protocol NotRefiningDiffable {
  @differentiable(reverse, wrt: x)
  func a(_ x: Float) -> Float
}

struct CertainlyNotDiffableWrtSelf: NotRefiningDiffable {
  func a(_ x: Float) -> Float { return x * 5.0 }
}

protocol TF285: Differentiable {
  @differentiable(reverse, wrt: (x, y))
  @differentiable(reverse, wrt: x)
  func foo(x: Float, y: Float) -> Float
}

struct TF285MissingOneDiffAttr: TF285 {
  typealias TangentVector = DummyTangentVector
  mutating func move(by _: TangentVector) {}

  // Requirement is missing the required `@differentiable(reverse, wrt: (x, y))` attribute.
  // Since `TF285MissingOneDiffAttr.foo` is internal, the attribute is implicitly created.
  @differentiable(reverse, wrt: x)
  func foo(x: Float, y: Float) -> Float {
    return x
  }
}

// TF-521: Test invalid `@differentiable` attribute due to invalid
// `Differentiable` conformance (`TangentVector` does not conform to
// `AdditiveArithmetic`).
struct TF_521<T: FloatingPoint> {
  var real: T
  var imaginary: T

  // expected-error @+1 {{can only differentiate functions with results that conform to 'Differentiable', but 'TF_521<T>' does not conform to 'Differentiable'}}
  @differentiable(reverse where T: Differentiable, T == T.TangentVector)
  init(real: T = 0, imaginary: T = 0) {
    self.real = real
    self.imaginary = imaginary
  }
}
// expected-error @+1 {{type 'TF_521<T>' does not conform to protocol 'Differentiable'}}
extension TF_521: Differentiable where T: Differentiable {
  // expected-note @+1 {{possibly intended match 'TF_521<T>.TangentVector' (aka 'TF_521<T>') does not conform to 'AdditiveArithmetic'}}
  typealias TangentVector = TF_521
}
// expected-error @+1 {{result type 'TF_521<Float>' does not conform to 'Differentiable', but the enclosing function type is '@differentiable'}}
let _: @differentiable(reverse) (Float, Float) -> TF_521<Float> = { r, i in
  TF_521(real: r, imaginary: i)
}

// expected-error @+1 {{result type 'TF_521<Float>' does not conform to 'Differentiable', but the enclosing function type is '@differentiable'}}
let _: @differentiable(reverse) (_, _) -> TF_521<Float> = { (r: Float, i: Float) in
  TF_521(real: r, imaginary: i)
}

// expected-error @+1 {{result type 'TF_521<Float>' does not conform to 'Differentiable', but the enclosing function type is '@differentiable'}}
let _: @differentiable(reverse) (Float, Float) -> _ = { r, i in
  TF_521(real: r, imaginary: i)
}

// TF-296: Infer `@differentiable` wrt parameters to be to all parameters that conform to `Differentiable`.

@differentiable(reverse)
func infer1(_ a: Float, _ b: Int) -> Float {
  return a + Float(b)
}

@differentiable(reverse)
func infer2(_ fn: @differentiable(reverse) (Float) -> Float, x: Float) -> Float {
  return fn(x)
}

struct DiffableStruct: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(by _: TangentVector) {}

  var a: Float

  @differentiable(reverse)
  func fn(_ b: Float, _ c: Int) -> Float {
    return a + b + Float(c)
  }
}

struct NonDiffableStruct {
  var a: Float

  @differentiable(reverse)
  func fn(_ b: Float) -> Float {
    return a + b
  }
}

// Index based 'wrt:'

struct NumberWrtStruct: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(by _: TangentVector) {}

  var a, b: Float

  @differentiable(reverse, wrt: 0) // ok
  @differentiable(reverse, wrt: 1) // ok
  func foo1(_ x: Float, _ y: Float) -> Float {
    return a*x + b*y
  }

  @differentiable(reverse, wrt: -1) // expected-error {{expected a parameter, which can be a function parameter name, parameter index, or 'self'}}
  @differentiable(reverse, wrt: (1, x)) // expected-error {{parameters must be specified in original order}}
  func foo2(_ x: Float, _ y: Float) -> Float {
    return a*x + b*y
  }

  @differentiable(reverse, wrt: (x, 1)) // ok
  @differentiable(reverse, wrt: (0)) // ok
  static func staticFoo1(_ x: Float, _ y: Float) -> Float {
    return x + y
  }

  @differentiable(reverse, wrt: (1, 1)) // expected-error {{parameters must be specified in original order}}
  @differentiable(reverse, wrt: (2)) // expected-error {{parameter index is larger than total number of parameters}}
  static func staticFoo2(_ x: Float, _ y: Float) -> Float {
    return x + y
  }
}

@differentiable(reverse, wrt: y) // ok
func two1(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(reverse, wrt: (x, y)) // ok
func two2(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(reverse, wrt: (0, y)) // ok
func two3(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(reverse, wrt: (x, 1)) // ok
func two4(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(reverse, wrt: (0, 1)) // ok
func two5(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(reverse, wrt: 2) // expected-error {{parameter index is larger than total number of parameters}}
func two6(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(reverse, wrt: (1, 0)) // expected-error {{parameters must be specified in original order}}
func two7(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(reverse, wrt: (1, x)) // expected-error {{parameters must be specified in original order}}
func two8(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(reverse, wrt: (y, 0)) // expected-error {{parameters must be specified in original order}}
func two9(x: Float, y: Float) -> Float {
  return x + y
}

// Inout 'wrt:' arguments.

@differentiable(reverse, wrt: y)
func inout1(x: Float, y: inout Float) -> Void {
  let _ = x + y
}
@differentiable(reverse, wrt: y)
func inout2(x: Float, y: inout Float) -> Float {
  let _ = x + y
}

// Test refining protocol requirements with `@differentiable` attribute.

public protocol Distribution {
  associatedtype Value
  func logProbability(of value: Value) -> Float
}

public protocol DifferentiableDistribution: Differentiable, Distribution {
  // expected-note @+2 {{overridden declaration is here}}
  @differentiable(reverse, wrt: self)
  func logProbability(of value: Value) -> Float
}

// Adding a more general `@differentiable` attribute.
public protocol DoubleDifferentiableDistribution: DifferentiableDistribution
  where Value: Differentiable {
  // expected-error @+1 {{overriding declaration is missing attribute '@differentiable(reverse, wrt: self)'}} {{3-3=@differentiable(reverse, wrt: self) }}
  func logProbability(of value: Value) -> Float
}

// Test failure to satisfy protocol requirement's `@differentiable` attribute.

public protocol HasRequirement {
  @differentiable(reverse)
  // expected-note @+1 {{protocol requires function 'requirement' with type '<T> (T, T) -> T'; do you want to add a stub?}}
  func requirement<T: Differentiable>(_ x: T, _ y: T) -> T
}

// expected-error @+1 {{type 'AttemptsToSatisfyRequirement' does not conform to protocol 'HasRequirement'}}
public struct AttemptsToSatisfyRequirement: HasRequirement {
  // This `@differentiable` attribute does not satisfy the requirement because
  // it is more constrained than the requirement's `@differentiable` attribute.
  @differentiable(reverse where T: CustomStringConvertible)
  // expected-note @+1 {{candidate is missing explicit '@differentiable(reverse, wrt: (x, y))' attribute to satisfy requirement}}
  public func requirement<T: Differentiable>(_ x: T, _ y: T) -> T { x }
}

// Test protocol requirement `@differentiable` attribute unsupported features.

protocol ProtocolRequirementUnsupported: Differentiable {
  associatedtype Scalar

  // expected-error @+1 {{'@differentiable' attribute on protocol requirement cannot specify 'where' clause}}
  @differentiable(reverse where Scalar: Differentiable)
  func unsupportedWhereClause(value: Scalar) -> Float
}
extension ProtocolRequirementUnsupported {
  func dfoo(_ x: Float) -> (Float, (Float) -> Float) {
    (x, { $0 })
  }
}

// Classes.

class Super: Differentiable {
  typealias TangentVector = DummyTangentVector
  func move(by _: TangentVector) {}

  var base: Float

  // expected-error @+1 {{'@differentiable' attribute cannot be declared on 'init' in a non-final class; consider making 'Super' final}}
  @differentiable(reverse)
  init(base: Float) {
    self.base = base
  }

  // NOTE(TF-1040): `@differentiable` attribute on class methods currently
  // does two orthogonal things:
  // - Requests derivative generation for the class method.
  // - Adds JVP/VJP vtable entries for the class method.
  // There's currently no way using `@differentiable` to do only one of the
  // above.
  @differentiable(reverse)
  func testClassMethod(_ x: Float) -> Float { x }

  @differentiable(reverse)
  final func testFinalMethod(_ x: Float) -> Float { x }

  @differentiable(reverse)
  static func testStaticMethod(_ x: Float) -> Float { x }

  @differentiable(reverse, wrt: (self, x))
  @differentiable(reverse, wrt: x)
  // expected-note @+1 2 {{overridden declaration is here}}
  func testMissingAttributes(_ x: Float) -> Float { x }

  @differentiable(reverse, wrt: x)
  func testSuperclassDerivatives(_ x: Float) -> Float { x }

  // Test duplicate attributes with different derivative generic signatures.
  // expected-error @+1 {{duplicate '@differentiable' attribute with same parameters}}
  @differentiable(reverse, wrt: x where T: Differentiable)
  // expected-note @+1 {{other attribute declared here}}
  @differentiable(reverse, wrt: x)
  func instanceMethod<T>(_ x: Float, y: T) -> Float { x }

  // expected-error @+1 {{'@differentiable' attribute cannot be declared on class members returning 'Self'}}
  @differentiable(reverse)
  func dynamicSelfResult() -> Self { self }

  // expected-error @+1 {{'@differentiable' attribute cannot be declared on class members returning 'Self'}}
  @differentiable(reverse)
  var testDynamicSelfProperty: Self { self }

  // TODO(TF-632): Fix "'TangentVector' is not a member type of 'Self'" diagnostic.
  // The underlying error should appear instead:
  // "covariant 'Self' can only appear at the top level of method result type".
  // expected-error @+1 2 {{'TangentVector' is not a member type of type 'Self'}}
  func vjpDynamicSelfResult() -> (Self, (Self.TangentVector) -> Self.TangentVector) {
    return (self, { $0 })
  }
}

class Sub: Super {
  // expected-error @+2 {{overriding declaration is missing attribute '@differentiable(reverse, wrt: x)'}}
  // expected-error @+1 {{overriding declaration is missing attribute '@differentiable(reverse)'}}
  override func testMissingAttributes(_ x: Float) -> Float { x }
}

final class FinalClass: Differentiable {
  typealias TangentVector = DummyTangentVector
  func move(by _: TangentVector) {}

  var base: Float

  @differentiable(reverse)
  init(base: Float) {
    self.base = base
  }
}

// Test `inout` parameters.

@differentiable(reverse, wrt: y)
func inoutVoid(x: Float, y: inout Float) {}

@differentiable(reverse)
func multipleSemanticResults(_ x: inout Float) -> Float { x }

@differentiable(reverse, wrt: y)
func swap(x: inout Float, y: inout Float) {}

struct InoutParameters: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(by _: TangentVector) {}
}

extension NonDiffableStruct {
  // expected-error @+1 {{can only differentiate functions with results that conform to 'Differentiable', but 'NonDiffableStruct' does not conform to 'Differentiable'}}
  @differentiable(reverse)
  static func nondiffResult(x: Int, y: inout NonDiffableStruct, z: Float) {}

  @differentiable(reverse)
  static func diffResult(x: Int, y: inout NonDiffableStruct, z: Float) -> Float {}

  // expected-error @+1 {{can only differentiate functions with results that conform to 'Differentiable', but 'NonDiffableStruct' does not conform to 'Differentiable'}}
  @differentiable(reverse, wrt: (y, z))
  static func diffResult2(x: Int, y: inout NonDiffableStruct, z: Float) -> Float {}
}

extension InoutParameters {
  @differentiable(reverse)
  static func staticMethod(_ lhs: inout Self, rhs: Self) {}

  @differentiable(reverse)
  static func multipleSemanticResults(_ lhs: inout Self, rhs: Self) -> Self {}
}

extension InoutParameters {
  @differentiable(reverse)
  mutating func mutatingMethod(_ other: Self) {}

  @differentiable(reverse)
  mutating func mutatingMethod(_ other: Self) -> Self {}
}

// Test tuple results.

extension InoutParameters {
  @differentiable(reverse)
  static func tupleResults(_ x: Self) -> (Self, Self) {}

  // Int does not conform to Differentiable
  // expected-error @+1 {{can only differentiate functions with results that conform to 'Differentiable', but 'Int' does not conform to 'Differentiable'}}
  @differentiable(reverse)
  static func tupleResultsInt(_ x: Self) -> (Int, Self) {}

  // expected-error @+1 {{can only differentiate functions with results that conform to 'Differentiable', but 'Int' does not conform to 'Differentiable'}}
  @differentiable(reverse)
  static func tupleResultsInt2(_ x: Self) -> (Self, Int) {}

  @differentiable(reverse)
  static func tupleResultsFloat(_ x: Self) -> (Float, Self) {}

  @differentiable(reverse)
  static func tupleResultsFloat2(_ x: Self) -> (Self, Float) {}
}

// Test accessors: `set`, `_read`, `_modify`.

struct Accessors: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(by _: TangentVector) {}

  var stored: Float
  var computed: Float {
    @differentiable(reverse)
    set { stored = newValue }

    // `_read` is a coroutine: `(Self) -> () -> ()`.
    // expected-error @+1 {{'@differentiable' attribute cannot be applied to this declaration}}
    @differentiable(reverse)
    _read { yield stored }

    // `_modify` is a coroutine: `(inout Self) -> () -> ()`.
    // expected-error @+1 {{'@differentiable' attribute cannot be applied to this declaration}}
    @differentiable(reverse)
    _modify { yield &stored }
  }
}

// expected-error @+1 {{cannot differentiate functions returning opaque result types}}
@differentiable(reverse)
func opaqueResult(_ x: Float) -> some Differentiable { x }

// Test the function tupling conversion with @differentiable.
func tuplify<Ts, U>(_ fn: @escaping (Ts) -> U) -> (Ts) -> U { fn }
func tuplifyDifferentiable<Ts : Differentiable, U>(_ fn: @escaping @differentiable(reverse) (Ts) -> U) -> @differentiable(reverse) (Ts) -> U { fn }

func testTupling(withoutNoDerivative: @escaping @differentiable(reverse) (Float, Float) -> Float,
                 withNoDerivative: @escaping @differentiable(reverse) (Float, @noDerivative Float) -> Float) {
  // We support tupling of differentiable functions as long as they drop @differentiable.
  let _: ((Float, Float)) -> Float = tuplify(withoutNoDerivative)
  let fn1 = tuplify(withoutNoDerivative)
  _ = fn1((0, 0))

  // In this case we also drop @noDerivative.
  let _: ((Float, Float)) -> Float = tuplify(withNoDerivative)
  let fn2 = tuplify(withNoDerivative)
  _ = fn2((0, 0))

  // We do not support tupling into an @differentiable function.
  let _ = tuplifyDifferentiable(withoutNoDerivative) // expected-error {{cannot convert value of type '@differentiable(reverse) (Float, Float) -> Float' to expected argument type '@differentiable(reverse) (Float) -> Float'}}
  let _ = tuplifyDifferentiable(withNoDerivative) // expected-error {{cannot convert value of type '@differentiable(reverse) (Float, @noDerivative Float) -> Float' to expected argument type '@differentiable(reverse) (Float) -> Float'}}
}
