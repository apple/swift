// RUN: %target-swift-frontend-typecheck -verify -disable-availability-checking %s -package-name myPkg
// RUN: %target-swift-frontend-typecheck -enable-testing -verify -disable-availability-checking %s -package-name myPkg

// Swift.AdditiveArithmetic:3:17: note: cannot yet register derivative default implementation for protocol requirements

import _Differentiation

// Dummy `Differentiable`-conforming type.
struct DummyTangentVector: Differentiable & AdditiveArithmetic {
  static var zero: Self { Self() }
  static func + (_: Self, _: Self) -> Self { Self() }
  static func - (_: Self, _: Self) -> Self { Self() }
  typealias TangentVector = Self
}

// Test top-level functions.

func id(_ x: Float) -> Float {
  return x
}
@derivative(of: id)
func jvpId(x: Float) -> (value: Float, differential: (Float) -> (Float)) {
  return (x, { $0 })
}
@derivative(of: id, wrt: x)
func vjpIdExplicitWrt(x: Float) -> (value: Float, pullback: (Float) -> Float) {
  return (x, { $0 })
}

func generic<T: Differentiable>(_ x: T, _ y: T) -> T {
  return x
}
@derivative(of: generic)
func jvpGeneric<T: Differentiable>(x: T, y: T) -> (
  value: T, differential: (T.TangentVector, T.TangentVector) -> T.TangentVector
) {
  return (x, { $0 + $1 })
}
@derivative(of: generic)
func vjpGenericExtraGenericRequirements<T: Differentiable & FloatingPoint>(
  x: T, y: T
) -> (value: T, pullback: (T) -> (T, T)) where T == T.TangentVector {
  return (x, { ($0, $0) })
}

// Test `wrt` parameter clauses.

func add(x: Float, y: Float) -> Float {
  return x + y
}
@derivative(of: add, wrt: x) // ok
func vjpAddWrtX(x: Float, y: Float) -> (value: Float, pullback: (Float) -> (Float)) {
  return (x + y, { $0 })
}
@derivative(of: add, wrt: (x, y)) // ok
func vjpAddWrtXY(x: Float, y: Float) -> (value: Float, pullback: (Float) -> (Float, Float)) {
  return (x + y, { ($0, $0) })
}

// Test index-based `wrt` parameters.

func subtract(x: Float, y: Float) -> Float {
  return x - y
}
@derivative(of: subtract, wrt: (0, y)) // ok
func vjpSubtractWrt0Y(x: Float, y: Float) -> (value: Float, pullback: (Float) -> (Float, Float)) {
  return (x - y, { ($0, $0) })
}
@derivative(of: subtract, wrt: (1)) // ok
func vjpSubtractWrt1(x: Float, y: Float) -> (value: Float, pullback: (Float) -> Float) {
  return (x - y, { $0 })
}

// Test invalid original function.

// expected-error @+1 {{cannot find 'nonexistentFunction' in scope}}
@derivative(of: nonexistentFunction)
func vjpOriginalFunctionNotFound(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

struct Q { }

@derivative(of: remainder(_:_:)) // expected-error {{cannot find 'remainder' in scope}}
// expected-error @+1 {{generic parameter 'T' is not used in function signature}}
func _vjpRemainder<T: FloatingPoint>(_ x: Q, _ y: Q) -> (
  value: Q, pullback: (Q) -> (Q, Q)
) {
  fatalError()
}

// Test `@derivative` attribute where `value:` result does not conform to `Differentiable`.
// Invalid original function should be diagnosed first.
// expected-error @+1 {{cannot find 'nonexistentFunction' in scope}}
@derivative(of: nonexistentFunction)
func vjpOriginalFunctionNotFound2(_ x: Float) -> (value: Int, pullback: (Float) -> Float) {
  fatalError()
}

// Test incorrect `@derivative` declaration type.

// expected-note @+2 {{'incorrectDerivativeType' defined here}}
// expected-note @+1 {{candidate global function does not have expected type '(Int) -> Int'}}
func incorrectDerivativeType(_ x: Float) -> Float {
  return x
}

// expected-error @+1 {{'@derivative(of:)' attribute requires function to return a two-element tuple; first element must have label 'value:' and second element must have label 'pullback:' or 'differential:'}}
@derivative(of: incorrectDerivativeType)
func jvpResultIncorrect(x: Float) -> Float {
  return x
}
// expected-error @+1 {{'@derivative(of:)' attribute requires function to return a two-element tuple; first element must have label 'value:'}}
@derivative(of: incorrectDerivativeType)
func vjpResultIncorrectFirstLabel(x: Float) -> (Float, (Float) -> Float) {
  return (x, { $0 })
}
// expected-error @+1 {{'@derivative(of:)' attribute requires function to return a two-element tuple; second element must have label 'pullback:' or 'differential:'}}
@derivative(of: incorrectDerivativeType)
func vjpResultIncorrectSecondLabel(x: Float) -> (value: Float, (Float) -> Float) {
  return (x, { $0 })
}
// expected-error @+1 {{referenced declaration 'incorrectDerivativeType' could not be resolved}}
@derivative(of: incorrectDerivativeType)
func vjpResultNotDifferentiable(x: Int) -> (
  value: Int, pullback: (Int) -> Int
) {
  return (x, { $0 })
}
// expected-error @+2 {{function result's 'pullback' type does not match 'incorrectDerivativeType'}}
// expected-note @+3 {{'pullback' does not have expected type '(Float.TangentVector) -> Float.TangentVector' (aka '(Float) -> Float')}}
@derivative(of: incorrectDerivativeType)
func vjpResultIncorrectPullbackType(x: Float) -> (
  value: Float, pullback: (Double) -> Double
) {
  return (x, { $0 })
}

// Test invalid `wrt:` differentiation parameters.

func invalidWrtParam(_ x: Float, _ y: Float) -> Float {
  return x
}

// expected-error @+1 {{unknown parameter name 'z'}}
@derivative(of: add, wrt: z)
func vjpUnknownParam(x: Float, y: Float) -> (value: Float, pullback: (Float) -> (Float)) {
  return (x + y, { $0 })
}
// expected-error @+1 {{parameters must be specified in original order}}
@derivative(of: invalidWrtParam, wrt: (y, x))
func vjpParamOrderNotIncreasing(x: Float, y: Float) -> (value: Float, pullback: (Float) -> (Float, Float)) {
  return (x + y, { ($0, $0) })
}
// expected-error @+1 {{'self' parameter is only applicable to instance methods}}
@derivative(of: invalidWrtParam, wrt: self)
func vjpInvalidSelfParam(x: Float, y: Float) -> (value: Float, pullback: (Float) -> (Float, Float)) {
  return (x + y, { ($0, $0) })
}
// expected-error @+1 {{parameter index is larger than total number of parameters}}
@derivative(of: invalidWrtParam, wrt: 2)
func vjpSubtractWrt2(x: Float, y: Float) -> (value: Float, pullback: (Float) -> (Float, Float)) {
  return (x - y, { ($0, $0) })
}
// expected-error @+1 {{parameters must be specified in original order}}
@derivative(of: invalidWrtParam, wrt: (1, x))
func vjpSubtractWrt1x(x: Float, y: Float) -> (value: Float, pullback: (Float) -> (Float, Float)) {
  return (x - y, { ($0, $0) })
}
// expected-error @+1 {{parameters must be specified in original order}}
@derivative(of: invalidWrtParam, wrt: (1, 0))
func vjpSubtractWrt10(x: Float, y: Float) -> (value: Float, pullback: (Float) -> (Float, Float)) {
  return (x - y, { ($0, $0) })
}

func noParameters() -> Float {
  return 1
}
// expected-error @+1 {{'vjpNoParameters()' has no parameters to differentiate with respect to}}
@derivative(of: noParameters)
func vjpNoParameters() -> (value: Float, pullback: (Float) -> Float) {
  return (1, { $0 })
}

func noDifferentiableParameters(x: Int) -> Float {
  return 1
}
// expected-error @+1 {{no differentiation parameters could be inferred; must differentiate with respect to at least one parameter conforming to 'Differentiable'}}
@derivative(of: noDifferentiableParameters)
func vjpNoDifferentiableParameters(x: Int) -> (
  value: Float, pullback: (Float) -> Int
) {
  return (1, { _ in 0 })
}

func functionParameter(_ fn: (Float) -> Float) -> Float {
  return fn(1)
}
// expected-error @+1 {{can only differentiate with respect to parameters that conform to 'Differentiable', but '(Float) -> Float' does not conform to 'Differentiable'}}
@derivative(of: functionParameter, wrt: fn)
func vjpFunctionParameter(_ fn: (Float) -> Float) -> (
  value: Float, pullback: (Float) -> Float
) {
  return (functionParameter(fn), { $0 })
}

// Test static methods.

protocol StaticMethod: Differentiable {
  static func foo(_ x: Float) -> Float
  static func generic<T: Differentiable>(_ x: T) -> T
}

extension StaticMethod {
  static func foo(_ x: Float) -> Float { x }
  static func generic<T: Differentiable>(_ x: T) -> T { x }
}

extension StaticMethod {
  @derivative(of: foo)
  static func jvpFoo(x: Float) -> (value: Float, differential: (Float) -> Float)
  {
    return (x, { $0 })
  }

  // Test qualified declaration name.
  @derivative(of: StaticMethod.foo)
  static func vjpFoo(x: Float) -> (value: Float, pullback: (Float) -> Float) {
    return (x, { $0 })
  }

  @derivative(of: generic)
  static func vjpGeneric<T: Differentiable>(_ x: T) -> (
    value: T, pullback: (T.TangentVector) -> (T.TangentVector)
  ) {
    return (x, { $0 })
  }

  // expected-error @+1 {{'self' parameter is only applicable to instance methods}}
  @derivative(of: foo, wrt: (self, x))
  static func vjpFooWrtSelf(x: Float) -> (value: Float, pullback: (Float) -> Float) {
    return (x, { $0 })
  }
}

// Test instance methods.

protocol InstanceMethod: Differentiable {
  func foo(_ x: Self) -> Self
  func generic<T: Differentiable>(_ x: T) -> Self
}

extension InstanceMethod {
  // expected-note @+1 {{'foo' defined here}}
  func foo(_ x: Self) -> Self { x }

  // expected-note @+1 {{'generic' defined here}}
  func generic<T: Differentiable>(_ x: T) -> Self { self }
}

extension InstanceMethod {
  @derivative(of: foo)
  func jvpFoo(x: Self) -> (
    value: Self, differential: (TangentVector, TangentVector) -> (TangentVector)
  ) {
    return (x, { $0 + $1 })
  }

  // Test qualified declaration name.
  @derivative(of: InstanceMethod.foo, wrt: x)
  func jvpFooWrtX(x: Self) -> (
    value: Self, differential: (TangentVector) -> (TangentVector)
  ) {
    return (x, { $0 })
  }

  @derivative(of: generic)
  func vjpGeneric<T: Differentiable>(_ x: T) -> (
    value: Self, pullback: (TangentVector) -> (TangentVector, T.TangentVector)
  ) {
    return (self, { ($0, .zero) })
  }

  @derivative(of: generic, wrt: (self, x))
  func jvpGenericWrt<T: Differentiable>(_ x: T) -> (value: Self, differential: (TangentVector, T.TangentVector) -> TangentVector) {
    return (self, { dself, dx in dself })
  }

  // expected-error @+1 {{'self' parameter must come first in the parameter list}}
  @derivative(of: generic, wrt: (x, self))
  func jvpGenericWrtSelf<T: Differentiable>(_ x: T) -> (value: Self, differential: (TangentVector, T.TangentVector) -> TangentVector) {
    return (self, { dself, dx in dself })
  }
}

extension InstanceMethod {
  // If `Self` conforms to `Differentiable`, then `Self` is inferred to be a differentiation parameter.
  // expected-error @+2 {{function result's 'pullback' type does not match 'foo'}}
  // expected-note @+3 {{'pullback' does not have expected type '(Self.TangentVector) -> (Self.TangentVector, Self.TangentVector)'}}
  @derivative(of: foo)
  func vjpFoo(x: Self) -> (
    value: Self, pullback: (TangentVector) -> TangentVector
  ) {
    return (x, { $0 })
  }

  // If `Self` conforms to `Differentiable`, then `Self` is inferred to be a differentiation parameter.
  // expected-error @+2 {{function result's 'pullback' type does not match 'generic'}}
  // expected-note @+3 {{'pullback' does not have expected type '(Self.TangentVector) -> (Self.TangentVector, T.TangentVector)'}}
  @derivative(of: generic)
  func vjpGeneric<T: Differentiable>(_ x: T) -> (
    value: Self, pullback: (TangentVector) -> T.TangentVector
  ) {
    return (self, { _ in .zero })
  }
}

// Test `@derivative` declaration with more constrained generic signature.

func req1<T>(_ x: T) -> T {
  return x
}
@derivative(of: req1)
func vjpExtraConformanceConstraint<T: Differentiable>(_ x: T) -> (
  value: T, pullback: (T.TangentVector) -> T.TangentVector
) {
  return (x, { $0 })
}

func req2<T, U>(_ x: T, _ y: U) -> T {
  return x
}
@derivative(of: req2)
func vjpExtraConformanceConstraints<T: Differentiable, U: Differentiable>( _ x: T, _ y: U) -> (
  value: T, pullback: (T) -> (T, U)
) where T == T.TangentVector, U == U.TangentVector, T: CustomStringConvertible {
  return (x, { ($0, .zero) })
}

// Test `@derivative` declaration with extra same-type requirements.
func req3<T>(_ x: T) -> T {
  return x
}
@derivative(of: req3)
func vjpSameTypeRequirementsGenericParametersAllConcrete<T>(_ x: T) -> (
  value: T, pullback: (T.TangentVector) -> T.TangentVector
) where T: Differentiable, T.TangentVector == Float {
  return (x, { $0 })
}

struct Wrapper<T: Equatable>: Equatable {
  var x: T
  init(_ x: T) { self.x = x }
}
extension Wrapper: AdditiveArithmetic where T: AdditiveArithmetic {
  static var zero: Self { .init(.zero) }
  static func + (lhs: Self, rhs: Self) -> Self { .init(lhs.x + rhs.x) }
  static func - (lhs: Self, rhs: Self) -> Self { .init(lhs.x - rhs.x) }
}
extension Wrapper: Differentiable where T: Differentiable, T == T.TangentVector {
  typealias TangentVector = Wrapper<T.TangentVector>
}
extension Wrapper where T: Differentiable, T == T.TangentVector {
  @derivative(of: init(_:))
  static func vjpInit(_ x: T) -> (value: Self, pullback: (Wrapper<T>.TangentVector) -> (T)) {
    fatalError()
  }
}

// Test class methods.

class Super {
  @differentiable(reverse)
  // expected-note @+1 {{candidate instance method is not defined in the current type context}}
  func foo(_ x: Float) -> Float {
    return x
  }

  @derivative(of: foo)
  func vjpFoo(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
    return (foo(x), { v in v })
  }
}

class Sub: Super {
  // TODO(TF-649): Enable `@derivative` to override derivatives for original
  // declaration defined in superclass.
  // expected-error @+1 {{referenced declaration 'foo' could not be resolved}}
  @derivative(of: foo)
  override func vjpFoo(_ x: Float) -> (value: Float, pullback: (Float) -> Float)
  {
    return (foo(x), { v in v })
  }
}

// Test non-`func` original declarations.

struct Struct<T> {
  var x: T
}
extension Struct: Equatable where T: Equatable {}
extension Struct: Differentiable & AdditiveArithmetic
where T: Differentiable & AdditiveArithmetic {
  static var zero: Self {
    fatalError()
  }
  static func + (lhs: Self, rhs: Self) -> Self {
    fatalError()
  }
  static func - (lhs: Self, rhs: Self) -> Self {
    fatalError()
  }
  typealias TangentVector = Struct<T.TangentVector>
  mutating func move(by offset: TangentVector) {
    x.move(by: offset.x)
  }
}

class Class<T> {
  var x: T
  init(_ x: T) {
    self.x = x
  }
}
extension Class: Differentiable where T: Differentiable {}

// Test computed properties.

extension Struct {
  var computedProperty: T {
    get { x }
    set { x = newValue }
    _modify { yield &x }
  }
}
extension Struct where T: Differentiable & AdditiveArithmetic {
  @derivative(of: computedProperty)
  func vjpProperty() -> (value: T, pullback: (T.TangentVector) -> TangentVector) {
    return (x, { v in .init(x: v) })
  }
  
  @derivative(of: computedProperty.get)
  func jvpProperty() -> (value: T, differential: (TangentVector) -> T.TangentVector) {
    fatalError()
  }
  
  @derivative(of: computedProperty.set)
  mutating func vjpPropertySetter(_ newValue: T) -> (
    value: (), pullback: (inout TangentVector) -> T.TangentVector
  ) {
    fatalError()
  }

  // expected-error @+1 {{cannot register derivative for _modify accessor}}
  @derivative(of: computedProperty._modify)
  mutating func vjpPropertyModify(_ newValue: T) -> (
    value: (), pullback: (inout TangentVector) -> T.TangentVector
  ) {
    fatalError()
  }
}

// Test initializers.

extension Struct {
  init(_ x: Float) {}
  init(_ x: T, y: Float) {}
}
extension Struct where T: Differentiable & AdditiveArithmetic {
  @derivative(of: init)
  static func vjpInit(_ x: Float) -> (
    value: Struct, pullback: (TangentVector) -> Float
  ) {
    return (.init(x), { _ in .zero })
  }

  @derivative(of: init(_:y:))
  static func vjpInit2(_ x: T, _ y: Float) -> (
    value: Struct, pullback: (TangentVector) -> (T.TangentVector, Float)
  ) {
    return (.init(x, y: y), { _ in (.zero, .zero) })
  }
}

// Test subscripts.

extension Struct {
  subscript() -> Float {
    get { 1 }
    set {}
  }

  subscript(float float: Float) -> Float {
    get { 1 }
    set {}
  }

  // expected-note @+1 {{candidate subscript does not have a setter}}
  subscript<U: Differentiable>(x: U) -> U { x }
}
extension Struct where T: Differentiable & AdditiveArithmetic {
  @derivative(of: subscript.get)
  func vjpSubscriptGetter() -> (value: Float, pullback: (Float) -> TangentVector) {
    return (1, { _ in .zero })
  }

  // expected-error @+2 {{a derivative already exists for getter for 'subscript()'}}
  // expected-note @-6 {{other attribute declared here}}
  @derivative(of: subscript)
  func vjpSubscript() -> (value: Float, pullback: (Float) -> TangentVector) {
    return (1, { _ in .zero })
  }

  @derivative(of: subscript().get)
  func jvpSubscriptGetter() -> (value: Float, differential: (TangentVector) -> Float) {
    return (1, { _ in .zero })
  }

  @derivative(of: subscript(float:).get, wrt: self)
  func vjpSubscriptLabeledGetter(float: Float) -> (value: Float, pullback: (Float) -> TangentVector)  {
    return (1, { _ in .zero })
  }

  // expected-error @+2 {{a derivative already exists for getter for 'subscript(float:)'}}
  // expected-note @-6 {{other attribute declared here}}
  @derivative(of: subscript(float:), wrt: self)
  func vjpSubscriptLabeled(float: Float) -> (value: Float, pullback: (Float) -> TangentVector) {
    return (1, { _ in .zero })
  }

  @derivative(of: subscript(float:).get)
  func jvpSubscriptLabeledGetter(float: Float) -> (value: Float, differential: (TangentVector, Float) -> Float)   {
    return (1, { (_,_) in 1})
  }

  @derivative(of: subscript(_:).get, wrt: self)
  func vjpSubscriptGenericGetter<U: Differentiable>(x: U) -> (value: U, pullback: (U.TangentVector) -> TangentVector)   {
    return (x, { _ in .zero })
  }

  // expected-error @+2 {{a derivative already exists for getter for 'subscript(_:)'}}
  // expected-note @-6 {{other attribute declared here}}
  @derivative(of: subscript(_:), wrt: self)
  func vjpSubscriptGeneric<U: Differentiable>(x: U) -> (value: U, pullback: (U.TangentVector) -> TangentVector)   {
    return (x, { _ in .zero })
  }

  @derivative(of: subscript.set)
  mutating func vjpSubscriptSetter(_ newValue: Float) -> (
    value: (), pullback: (inout TangentVector) -> Float
  ) {
    fatalError()
  }

  @derivative(of: subscript().set)
  mutating func jvpSubscriptSetter(_ newValue: Float) -> (
    value: (), differential: (inout TangentVector, Float) -> ()
  ) {
    fatalError()
  }

  @derivative(of: subscript(float:).set)
  mutating func vjpSubscriptLabeledSetter(float: Float, newValue: Float) -> (
    value: (), pullback: (inout TangentVector) -> (Float, Float)
  ) {
    fatalError()
  }

  @derivative(of: subscript(float:).set)
  mutating func jvpSubscriptLabeledSetter(float: Float, _ newValue: Float) -> (
    value: (), differential: (inout TangentVector, Float, Float) -> Void
  ) {
    fatalError()
  }

  // Error: original subscript has no setter.
  // expected-error @+1 {{referenced declaration 'subscript(_:)' could not be resolved}}
  @derivative(of: subscript(_:).set, wrt: self)
  mutating func vjpSubscriptGeneric_NoSetter<U: Differentiable>(x: U) -> (
    value: U, pullback: (U.TangentVector) -> TangentVector
  ) {
    return (x, { _ in .zero })
  }
}

// https://github.com/apple/swift/issues/57833

struct Struct2<T> {}
extension Struct2: Differentiable where T: Differentiable {}

extension Struct2 {
  // expected-note @+1 {{candidate instance method does not have type equal to or less constrained than '<T where T : Differentiable> (inout Struct2<T>) -> (Int, @differentiable(reverse) (inout T) -> Void) -> Void'}}
  mutating func update<D>(at index: Int, byCalling closure: (inout T, D) -> Void, withArgument: D) {
    fatalError("Stop")
  }
}

extension Struct2 where T: Differentiable {
  // expected-error @+1 {{referenced declaration 'update' could not be resolved}}
  @derivative(of: update)
  mutating func vjp_update(
    at index: Int,
    byCalling closure: @differentiable(reverse) (inout T) -> Void
  ) -> (value: Void, pullback: (inout Self.TangentVector) -> Void) {
    fatalError("Stop")
  }
}

extension Class {
  subscript() -> Float {
    get { 1 }
    // expected-note @+1 {{'subscript()' declared here}}
    set {}
  }
}
extension Class where T: Differentiable {
  @derivative(of: subscript.get)
  func vjpSubscriptGetter() -> (value: Float, pullback: (Float) -> TangentVector) {
    return (1, { _ in .zero })
  }

  // expected-error @+2 {{a derivative already exists for getter for 'subscript()'}}
  // expected-note @-6 {{other attribute declared here}}
  @derivative(of: subscript)
  func vjpSubscript() -> (value: Float, pullback: (Float) -> TangentVector) {
    return (1, { _ in .zero })
  }

  // FIXME: Enable derivative registration for class property/subscript setters (https://github.com/apple/swift/issues/55542).
  //
  // This requires changing derivative type calculation rules for functions with
  // class-typed parameters. We need to assume that all functions taking
  // class-typed operands may mutate those operands.
  // expected-error @+1 {{cannot yet register derivative for class property or subscript setters}}
  @derivative(of: subscript.set)
  func vjpSubscriptSetter(_ newValue: Float) -> (
    value: (), pullback: (inout TangentVector) -> Float
  ) {
    fatalError()
  }
}

// Test duplicate `@derivative` attribute.

func duplicate(_ x: Float) -> Float { x }
// expected-note @+1 {{other attribute declared here}}
@derivative(of: duplicate)
func jvpDuplicate1(_ x: Float) -> (value: Float, differential: (Float) -> Float) {
  return (duplicate(x), { $0 })
}
// expected-error @+1 {{a derivative already exists for 'duplicate'}}
@derivative(of: duplicate)
func jvpDuplicate2(_ x: Float) -> (value: Float, differential: (Float) -> Float) {
  return (duplicate(x), { $0 })
}

// Test invalid original declaration kind.

// expected-note @+1 {{candidate var does not have a getter}}
var globalVariable: Float

// expected-error @+1 {{referenced declaration 'globalVariable' could not be resolved}}
@derivative(of: globalVariable)
func invalidOriginalDeclaration(x: Float) -> (
  value: Float, differential: (Float) -> (Float)
) {
  return (x, { $0 })
}

// Test ambiguous original declaration.

protocol P1 {}
protocol P2 {}
// expected-note @+1 {{candidate global function found here}}
func ambiguous<T: P1>(_ x: T) -> T { x }
// expected-note @+1 {{candidate global function found here}}
func ambiguous<T: P2>(_ x: T) -> T { x }

// expected-error @+1 {{referenced declaration 'ambiguous' is ambiguous}}
@derivative(of: ambiguous)
func jvpAmbiguous<T: P1 & P2 & Differentiable>(x: T)
  -> (value: T, differential: (T.TangentVector) -> (T.TangentVector))
{
  return (x, { $0 })
}

// Test no valid original declaration.
// Original declarations are invalid because they have extra generic
// requirements unsatisfied by the `@derivative` function.

// expected-note @+1 {{candidate global function does not have type equal to or less constrained than '<T where T : Differentiable> (x: T) -> T'}}
func invalid<T: BinaryFloatingPoint>(x: T) -> T { x }
// expected-note @+1 {{candidate global function does not have type equal to or less constrained than '<T where T : Differentiable> (x: T) -> T'}}
func invalid<T: CustomStringConvertible>(x: T) -> T { x }
// expected-note @+1 {{candidate global function does not have type equal to or less constrained than '<T where T : Differentiable> (x: T) -> T'}}
func invalid<T: FloatingPoint>(x: T) -> T { x }

// expected-error @+1 {{referenced declaration 'invalid' could not be resolved}}
@derivative(of: invalid)
func jvpInvalid<T: Differentiable>(x: T) -> (
  value: T, differential: (T.TangentVector) -> T.TangentVector
) {
  return (x, { $0 })
}

// Test stored property original declaration.

struct HasStoredProperty {
  // expected-note @+1 {{'stored' declared here}}
  var stored: Float
}
extension HasStoredProperty: Differentiable & AdditiveArithmetic {
  static var zero: Self {
    fatalError()
  }
  static func + (lhs: Self, rhs: Self) -> Self {
    fatalError()
  }
  static func - (lhs: Self, rhs: Self) -> Self {
    fatalError()
  }
  typealias TangentVector = Self
}
extension HasStoredProperty {
  // expected-error @+1 {{cannot register derivative for stored property 'stored'}}
  @derivative(of: stored)
  func vjpStored() -> (value: Float, pullback: (Float) -> TangentVector) {
    return (stored, { _ in .zero })
  }
}

// Test derivative registration for protocol requirements. Currently unsupported.
// TODO(TF-982): Lift this restriction and add proper support.

protocol ProtocolRequirementDerivative {
  // expected-note @+1 {{cannot yet register derivative default implementation for protocol requirements}}
  func requirement(_ x: Float) -> Float
}
extension ProtocolRequirementDerivative {
  // expected-error @+1 {{referenced declaration 'requirement' could not be resolved}}
  @derivative(of: requirement)
  func vjpRequirement(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
    fatalError()
  }
}

// Test `inout` parameters.

func multipleSemanticResults(_ x: inout Float) -> Float {
  return x
}
@derivative(of: multipleSemanticResults)
func vjpMultipleSemanticResults(x: inout Float) -> (
  value: Float, pullback: (Float, inout Float) -> Void
) { fatalError() }

func inoutNonDifferentiableResult(_ x: inout Float) -> Int {
  return 5
}
// expected-error @+1 {{can only differentiate functions with results that conform to 'Differentiable', but 'Int' does not conform to 'Differentiable'}}
@derivative(of: inoutNonDifferentiableResult)
func vjpInoutNonDifferentiableResult(x: inout Float) -> (
  value: Int, pullback: (inout Float) -> Void
) { fatalError() }

struct InoutParameters: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(by _: TangentVector) {}
}

extension InoutParameters {
  // expected-note @+2 {{'staticMethod(_:rhs:)' defined here}}
  // expected-note @+1 {{'staticMethod(_:rhs:)' defined here}}
  static func staticMethod(_ lhs: inout Self, rhs: Self) {}

  // Test wrt `inout` parameter.

  @derivative(of: staticMethod)
  static func vjpWrtInout(_ lhs: inout Self, _ rhs: Self) -> (
    value: Void, pullback: (inout TangentVector) -> TangentVector
  ) { fatalError() }

  // expected-error @+1 {{function result's 'pullback' type does not match 'staticMethod(_:rhs:)'}}
  @derivative(of: staticMethod)
  static func vjpWrtInoutMismatch(_ lhs: inout Self, _ rhs: Self) -> (
    // expected-note @+1 {{'pullback' does not have expected type '(inout InoutParameters.TangentVector) -> InoutParameters.TangentVector' (aka '(inout DummyTangentVector) -> DummyTangentVector')}}
    value: Void, pullback: (TangentVector) -> TangentVector
  ) { fatalError() }

  @derivative(of: staticMethod)
  static func jvpWrtInout(_ lhs: inout Self, _ rhs: Self) -> (
    value: Void, differential: (inout TangentVector, TangentVector) -> Void
  ) { fatalError() }

  // expected-error @+1 {{function result's 'differential' type does not match 'staticMethod(_:rhs:)'}}
  @derivative(of: staticMethod)
  static func jvpWrtInoutMismatch(_ lhs: inout Self, _ rhs: Self) -> (
    // expected-note @+1 {{'differential' does not have expected type '(inout InoutParameters.TangentVector, InoutParameters.TangentVector) -> ()' (aka '(inout DummyTangentVector, DummyTangentVector) -> ()')}}
    value: Void, differential: (TangentVector, TangentVector) -> Void
  ) { fatalError() }

  // Test non-wrt `inout` parameter.

  // expected-error @+1 {{cannot differentiate void function 'staticMethod(_:rhs:)'}}
  @derivative(of: staticMethod, wrt: rhs)
  static func vjpNotWrtInout(_ lhs: inout Self, _ rhs: Self) -> (
    value: Void, pullback: (TangentVector) -> TangentVector
  ) { fatalError() }

  // expected-error @+1 {{cannot differentiate void function 'staticMethod(_:rhs:)'}}
  @derivative(of: staticMethod, wrt: rhs)
  static func vjpNotWrtInoutMismatch(_ lhs: inout Self, _ rhs: Self) -> (
    value: Void, pullback: (inout TangentVector) -> TangentVector
  ) { fatalError() }

  // expected-error @+1 {{cannot differentiate void function 'staticMethod(_:rhs:)'}}
  @derivative(of: staticMethod, wrt: rhs)
  static func jvpNotWrtInout(_ lhs: inout Self, _ rhs: Self) -> (
    value: Void, differential: (TangentVector) -> TangentVector
  ) { fatalError() }

  // expected-error @+1 {{cannot differentiate void function 'staticMethod(_:rhs:)'}}
  @derivative(of: staticMethod, wrt: rhs)
  static func jvpNotWrtInout(_ lhs: inout Self, _ rhs: Self) -> (
    value: Void, differential: (inout TangentVector) -> TangentVector
  ) { fatalError() }
}

extension InoutParameters {
  // expected-note @+2 {{'mutatingMethod' defined here}}
  // expected-note @+1 {{'mutatingMethod' defined here}}  
  mutating func mutatingMethod(_ other: Self) {}

  // Test wrt `inout` `self` parameter.

  @derivative(of: mutatingMethod)
  mutating func vjpWrtInout(_ other: Self) -> (
    value: Void, pullback: (inout TangentVector) -> TangentVector
  ) { fatalError() }

  // expected-error @+1 {{function result's 'pullback' type does not match 'mutatingMethod'}}
  @derivative(of: mutatingMethod)
  mutating func vjpWrtInoutMismatch(_ other: Self) -> (
    // expected-note @+1 {{'pullback' does not have expected type '(inout InoutParameters.TangentVector) -> InoutParameters.TangentVector' (aka '(inout DummyTangentVector) -> DummyTangentVector')}}
    value: Void, pullback: (TangentVector) -> TangentVector
  ) { fatalError() }

  @derivative(of: mutatingMethod)
  mutating func jvpWrtInout(_ other: Self) -> (
    value: Void, differential: (inout TangentVector, TangentVector) -> Void
  ) { fatalError() }

  // expected-error @+1 {{function result's 'differential' type does not match 'mutatingMethod'}}
  @derivative(of: mutatingMethod)
  mutating func jvpWrtInoutMismatch(_ other: Self) -> (
    // expected-note @+1 {{'differential' does not have expected type '(inout InoutParameters.TangentVector, InoutParameters.TangentVector) -> ()' (aka '(inout DummyTangentVector, DummyTangentVector) -> ()')}}
    value: Void, differential: (TangentVector, TangentVector) -> Void
  ) { fatalError() }

  // Test non-wrt `inout` `self` parameter.

  // expected-error @+1 {{cannot differentiate void function 'mutatingMethod'}}
  @derivative(of: mutatingMethod, wrt: other)
  mutating func vjpNotWrtInout(_ other: Self) -> (
    value: Void, pullback: (TangentVector) -> TangentVector
  ) { fatalError() }

  // expected-error @+1 {{cannot differentiate void function 'mutatingMethod'}}
  @derivative(of: mutatingMethod, wrt: other)
  mutating func vjpNotWrtInoutMismatch(_ other: Self) -> (
    value: Void, pullback: (inout TangentVector) -> TangentVector
  ) { fatalError() }

  // expected-error @+1 {{cannot differentiate void function 'mutatingMethod'}}
  @derivative(of: mutatingMethod, wrt: other)
  mutating func jvpNotWrtInout(_ other: Self) -> (
    value: Void, differential: (TangentVector) -> TangentVector
  ) { fatalError() }

  // expected-error @+1 {{cannot differentiate void function 'mutatingMethod'}}
  @derivative(of: mutatingMethod, wrt: other)
  mutating func jvpNotWrtInoutMismatch(_ other: Self) -> (
    value: Void, differential: (TangentVector, TangentVector) -> Void
  ) { fatalError() }
}

// Test no semantic results.

func noSemanticResults(_ x: Float) {}

// expected-error @+1 {{cannot differentiate void function 'noSemanticResults'}}
@derivative(of: noSemanticResults)
func vjpNoSemanticResults(_ x: Float) -> (value: Void, pullback: Void) {}

// Test multiple semantic results.

extension InoutParameters {
  func multipleSemanticResults(_ x: inout Float) -> Float { x }
  @derivative(of: multipleSemanticResults, wrt: x)
  func vjpMultipleSemanticResults(_ x: inout Float) -> (
    value: Float, pullback: (Float, inout Float) -> Void
  ) { fatalError() }

  func inoutVoid(_ x: Float, _ void: inout Void) -> Float {}
  @derivative(of: inoutVoid, wrt: (x, void))
  func vjpInoutVoidParameter(_ x: Float, _ void: inout Void) -> (
    value: Float, pullback: (Float) -> Float
  ) { fatalError() }
}

// Test tuple results.

extension InoutParameters {
  func tupleResults(_ x: Float) -> (Float, Float) { (x, x) }
  @derivative(of: tupleResults, wrt: x)
  func vjpTupleResults(_ x: Float) -> (
    value: (Float, Float), pullback: (Float, Float) -> Float
  ) { fatalError() }

  func tupleResultsInt(_ x: Float) -> (Int, Float) { (1, x) }
  // expected-error @+1 {{can only differentiate functions with results that conform to 'Differentiable', but 'Int' does not conform to 'Differentiable'}}
  @derivative(of: tupleResultsInt, wrt: x)
  func vjpTupleResults(_ x: Float) -> (
    value: (Int, Float), pullback: (Float) -> Float
  ) { fatalError() }
}

// Test original/derivative function `inout` parameter mismatches.

extension InoutParameters {
  // expected-note @+1 {{candidate instance method does not have expected type '(InoutParameters) -> (inout Float) -> Void'}}
  func inoutParameterMismatch(_ x: Float) {}

  // expected-error @+1 {{referenced declaration 'inoutParameterMismatch' could not be resolved}}
  @derivative(of: inoutParameterMismatch)
  func vjpInoutParameterMismatch(_ x: inout Float) -> (value: Void, pullback: (inout Float) -> Void) {
    fatalError()
  }

  // expected-note @+1 {{candidate instance method does not have expected type '(inout InoutParameters) -> (Float) -> Void'}}
  func mutatingMismatch(_ x: Float) {}

  // expected-error @+1 {{referenced declaration 'mutatingMismatch' could not be resolved}}
  @derivative(of: mutatingMismatch)
  mutating func vjpMutatingMismatch(_ x: Float) -> (value: Void, pullback: (inout Float) -> Void) {
    fatalError()
  }
}

// Test cross-file derivative registration.

extension FloatingPoint where Self: Differentiable {
  @usableFromInline
  @derivative(of: rounded)
  func vjpRounded() -> (
    value: Self,
    pullback: (Self.TangentVector) -> (Self.TangentVector)
  ) {
    fatalError()
  }
}

extension Differentiable where Self: AdditiveArithmetic {
  // expected-error @+1 {{referenced declaration '+' could not be resolved}}
  @derivative(of: +)
  static func vjpPlus(x: Self, y: Self) -> (
    value: Self,
    pullback: (Self.TangentVector) -> (Self.TangentVector, Self.TangentVector)
  ) {
    return (x + y, { v in (v, v) })
  }
}

extension AdditiveArithmetic
where Self: Differentiable, Self == Self.TangentVector {
  // expected-error @+1 {{referenced declaration '+' could not be resolved}}
  @derivative(of: +)
  func vjpPlusInstanceMethod(x: Self, y: Self) -> (
    value: Self, pullback: (Self) -> (Self, Self)
  ) {
    return (x + y, { v in (v, v) })
  }
}

// Test derivatives of default implementations.
protocol HasADefaultImplementation {
  func req(_ x: Float) -> Float
}
extension HasADefaultImplementation {
  func req(_ x: Float) -> Float { x }
  // ok
  @derivative(of: req)
  func req(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
    (x, { 10 * $0 })
  }
}

// Test default derivatives of requirements.
protocol HasADefaultDerivative {
  // expected-note @+1 {{cannot yet register derivative default implementation for protocol requirements}}
  func req(_ x: Float) -> Float
}
extension HasADefaultDerivative {
  // TODO(TF-982): Support default derivatives for protocol requirements.
  // expected-error @+1 {{referenced declaration 'req' could not be resolved}}
  @derivative(of: req)
  func vjpReq(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
    (x, { 10 * $0 })
  }
}

// MARK: - Original function visibility = derivative function visibility

public func public_original_public_derivative(_ x: Float) -> Float { x }
@derivative(of: public_original_public_derivative)
public func _public_original_public_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

public func public_original_usablefrominline_derivative(_ x: Float) -> Float { x }
@usableFromInline
@derivative(of: public_original_usablefrominline_derivative)
func _public_original_usablefrominline_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

package func package_original_package_derivative(_ x: Float) -> Float { x }
@derivative(of: package_original_package_derivative)
package func _package_original_package_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

func internal_original_internal_derivative(_ x: Float) -> Float { x }
@derivative(of: internal_original_internal_derivative)
func _internal_original_internal_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

private func private_original_private_derivative(_ x: Float) -> Float { x }
@derivative(of: private_original_private_derivative)
private func _private_original_private_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

fileprivate func fileprivate_original_fileprivate_derivative(_ x: Float) -> Float { x }
@derivative(of: fileprivate_original_fileprivate_derivative)
fileprivate func _fileprivate_original_fileprivate_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

func internal_original_usablefrominline_derivative(_ x: Float) -> Float { x }
@usableFromInline
@derivative(of: internal_original_usablefrominline_derivative)
func _internal_original_usablefrominline_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

func internal_original_inlinable_derivative(_ x: Float) -> Float { x }
@inlinable
@derivative(of: internal_original_inlinable_derivative)
func _internal_original_inlinable_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

func internal_original_alwaysemitintoclient_derivative(_ x: Float) -> Float { x }
@_alwaysEmitIntoClient
@derivative(of: internal_original_alwaysemitintoclient_derivative)
func _internal_original_alwaysemitintoclient_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}


package func package_original_usablefrominline_derivative(_ x: Float) -> Float { x }
@usableFromInline
@derivative(of: package_original_usablefrominline_derivative)
package func _package_original_usablefrominline_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

package func package_original_inlinable_derivative(_ x: Float) -> Float { x }
@inlinable
@derivative(of: package_original_inlinable_derivative)
package func _package_original_inlinable_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

package func package_original_alwaysemitintoclient_derivative(_ x: Float) -> Float { x }
@_alwaysEmitIntoClient
@derivative(of: package_original_alwaysemitintoclient_derivative)
package func _package_original_alwaysemitintoclient_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

// MARK: - Original function visibility < derivative function visibility

@usableFromInline
func usablefrominline_original_public_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_usablefrominline_original_public_derivative' is public, but original function 'usablefrominline_original_public_derivative' is internal}}
@derivative(of: usablefrominline_original_public_derivative)
// expected-note @+1 {{mark the derivative function as 'internal' to match the original function}} {{1-7=internal}}
public func _usablefrominline_original_public_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

@usableFromInline
package func package_usablefrominline_original_public_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_package__usablefrominline_original_public_derivative' is public, but original function 'package_usablefrominline_original_public_derivative' is package}}
@derivative(of: package_usablefrominline_original_public_derivative)
// expected-note @+1 {{mark the derivative function as 'package' to match the original function}} {{1-7=package}}
public func _package__usablefrominline_original_public_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

package func package_original_public_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_package_original_public_derivative' is public, but original function 'package_original_public_derivative' is package}}
@derivative(of: package_original_public_derivative)
// expected-note @+1 {{mark the derivative function as 'package' to match the original function}} {{1-7=package}}
public func _package_original_public_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

func internal_original_public_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_internal_original_public_derivative' is public, but original function 'internal_original_public_derivative' is internal}}
@derivative(of: internal_original_public_derivative)
// expected-note @+1 {{mark the derivative function as 'internal' to match the original function}} {{1-7=internal}}
public func _internal_original_public_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

func internal_original_package_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_internal_original_package_derivative' is package, but original function 'internal_original_package_derivative' is internal}}
@derivative(of: internal_original_package_derivative)
// expected-note @+1 {{mark the derivative function as 'internal' to match the original function}} {{1-8=internal}}
package func _internal_original_package_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

private func private_original_usablefrominline_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_private_original_usablefrominline_derivative' is internal, but original function 'private_original_usablefrominline_derivative' is private}}
@derivative(of: private_original_usablefrominline_derivative)
@usableFromInline
// expected-note @+1 {{mark the derivative function as 'private' to match the original function}} {{1-1=private }}
func _private_original_usablefrominline_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

private func private_original_public_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_private_original_public_derivative' is public, but original function 'private_original_public_derivative' is private}}
@derivative(of: private_original_public_derivative)
// expected-note @+1 {{mark the derivative function as 'private' to match the original function}} {{1-7=private}}
public func _private_original_public_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

private func private_original_package_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_private_original_package_derivative' is package, but original function 'private_original_package_derivative' is private}}
@derivative(of: private_original_package_derivative)
// expected-note @+1 {{mark the derivative function as 'private' to match the original function}}
package func _private_original_package_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

private func private_original_internal_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_private_original_internal_derivative' is internal, but original function 'private_original_internal_derivative' is private}}
@derivative(of: private_original_internal_derivative)
// expected-note @+1 {{mark the derivative function as 'private' to match the original function}}
func _private_original_internal_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

fileprivate func fileprivate_original_private_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_fileprivate_original_private_derivative' is private, but original function 'fileprivate_original_private_derivative' is fileprivate}}
@derivative(of: fileprivate_original_private_derivative)
// expected-note @+1 {{mark the derivative function as 'fileprivate' to match the original function}} {{1-8=fileprivate}}
private func _fileprivate_original_private_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

private func private_original_fileprivate_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_private_original_fileprivate_derivative' is fileprivate, but original function 'private_original_fileprivate_derivative' is private}}
@derivative(of: private_original_fileprivate_derivative)
// expected-note @+1 {{mark the derivative function as 'private' to match the original function}} {{1-12=private}}
fileprivate func _private_original_fileprivate_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

// MARK: - Original function visibility > derivative function visibility

public func public_original_private_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_public_original_private_derivative' is fileprivate, but original function 'public_original_private_derivative' is public}}
@derivative(of: public_original_private_derivative)
fileprivate func _public_original_private_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
// expected-note @-1 {{mark the derivative function as '@usableFromInline' to match the original function}} {{-1:1-1=@usableFromInline }}
  fatalError()
}

public func public_original_package_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_public_original_package_derivative' is package, but original function 'public_original_package_derivative' is public}}
@derivative(of: public_original_package_derivative)
package func _public_original_package_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  // expected-note @-1 {{mark the derivative function as '@usableFromInline' to match the original function}} {{-1:1-1=@usableFromInline }}
  fatalError()
}

public func public_original_internal_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_public_original_internal_derivative' is internal, but original function 'public_original_internal_derivative' is public}}
@derivative(of: public_original_internal_derivative)
func _public_original_internal_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
// expected-note @-1 {{mark the derivative function as '@usableFromInline' to match the original function}} {{-1:1-1=@usableFromInline }}
  fatalError()
}

package func package_original_internal_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_package_original_internal_derivative' is internal, but original function 'package_original_internal_derivative' is package}}
@derivative(of: package_original_internal_derivative)
// expected-note @+1 {{mark the derivative function as 'package' to match the original function}} {{1-1=package }}
func _package_original_internal_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

func internal_original_fileprivate_derivative(_ x: Float) -> Float { x }
// expected-error @+1 {{derivative function must have same access level as original function; derivative function '_internal_original_fileprivate_derivative' is fileprivate, but original function 'internal_original_fileprivate_derivative' is internal}}
@derivative(of: internal_original_fileprivate_derivative)
// expected-note @+1 {{mark the derivative function as 'internal' to match the original function}} {{1-12=internal}}
fileprivate func _internal_original_fileprivate_derivative(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

// Test invalid reference to an accessor of a non-storage declaration.

// expected-note @+1 {{candidate global function does not have a getter}}
func function(_ x: Float) -> Float {
  x
}

// expected-error @+1 {{referenced declaration 'function' could not be resolved}}
@derivative(of: function(_:).get)
func vjpFunction(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

// Test ambiguity that exists when Type function name is the same
// as an accessor label.

extension Float {
  // Original function name conflicts with an accessor name ("set").
  func set() -> Float {
    self
  }

  // Original function name does not conflict with an accessor name.
  func method() -> Float {
    self
  }

  // Test ambiguous parse.
  // Expected:
  // - Base type: `Float`
  // - Declaration name: `set`
  // - Accessor kind: <none>
  // Actual:
  // - Base type: <none>
  // - Declaration name: `Float`
  // - Accessor kind: `set`
  // expected-error @+1 {{cannot find 'Float' in scope}}
  @derivative(of: Float.set)
  func jvpSet() -> (value: Float, differential: (Float) -> Float) {
    fatalError()
  }

  @derivative(of: Float.method)
  func jvpMethod() -> (value: Float, differential: (Float) -> Float) {
    fatalError()
  }
}

// Test original function with opaque result type.

// expected-note @+1 {{candidate global function does not have expected type '(Float) -> Float'}}
func opaqueResult(_ x: Float) -> some Differentiable { x }

// expected-error @+1 {{referenced declaration 'opaqueResult' could not be resolved}}
@derivative(of: opaqueResult)
func vjpOpaqueResult(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
  fatalError()
}

// Test instance vs static method mismatch.

struct StaticMismatch<T: Differentiable> {
  // expected-note @+1 {{original function 'init(_:)' is a 'static' method}}
  init(_ x: T) {}
  // expected-note @+1 {{original function 'instanceMethod' is an instance method}}
  func instanceMethod(_ x: T) -> T { x }
  // expected-note @+1 {{original function 'staticMethod' is a 'static' method}}
  static func staticMethod(_ x: T) -> T { x }

  // expected-error @+1 {{unexpected derivative function declaration; 'init(_:)' requires the derivative function 'vjpInit' to be a 'static' method}}
  @derivative(of: init)
  // expected-note @+1 {{make derivative function 'vjpInit' a 'static' method}}{{3-3=static }}
  func vjpInit(_ x: T) -> (value: Self, pullback: (T.TangentVector) -> T.TangentVector) {
    fatalError()
  }

  // expected-error @+1 {{unexpected derivative function declaration; 'instanceMethod' requires the derivative function 'jvpInstance' to be an instance method}}
  @derivative(of: instanceMethod)
  // expected-note @+1 {{make derivative function 'jvpInstance' an instance method}}{{3-10=}}
  static func jvpInstance(_ x: T) -> (
    value: T, differential: (T.TangentVector) -> (T.TangentVector)
  ) {
    return (x, { $0 })
  }

  // expected-error @+1 {{unexpected derivative function declaration; 'staticMethod' requires the derivative function 'jvpStatic' to be a 'static' method}}
  @derivative(of: staticMethod)
  // expected-note @+1 {{make derivative function 'jvpStatic' a 'static' method}}{{3-3=static }}
  func jvpStatic(_ x: T) -> (
    value: T, differential: (T.TangentVector) -> (T.TangentVector)
  ) {
    return (x, { $0 })
  }
}


