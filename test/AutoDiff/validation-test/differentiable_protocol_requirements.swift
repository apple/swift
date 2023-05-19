// RUN: %target-run-simple-swift
// REQUIRES: executable_test

// Test is unexpectedly passing on no_assert config on Linux
// REQUIRES: rdar89860761

// FIXME: Disabled due to test failure with `-O` (https://github.com/apple/swift/issues/55690).
// XFAIL: swift_test_mode_optimize
// XFAIL: swift_test_mode_optimize_size
// XFAIL: swift_test_mode_optimize_unchecked

import StdlibUnittest
import DifferentiationUnittest

// Test end-to-end differentiation of `@differentiable` protocol requirements.

var ProtocolRequirementAutodiffTests = TestSuite("ProtocolRequirementDifferentiation")

// MARK: - Method requirements.

protocol DiffReq: Differentiable {
  @differentiable(reverse, wrt: (self, x))
  func f(_ x: Tracked<Float>) -> Tracked<Float>
}

extension DiffReq where TangentVector: AdditiveArithmetic {
  @inline(never)  // Prevent specialization, to test all witness code.
  func gradF(at x: Tracked<Float>) -> (Self.TangentVector, Tracked<Float>) {
    return (valueWithPullback(at: self, x) { s, x in s.f(x) }).1(1)
  }
}

struct Quadratic: DiffReq, AdditiveArithmetic {
  typealias TangentVector = Quadratic

  @differentiable(reverse)
  let a: Tracked<Float>

  @differentiable(reverse)
  let b: Tracked<Float>

  @differentiable(reverse)
  let c: Tracked<Float>

  init(_ a: Tracked<Float>, _ b: Tracked<Float>, _ c: Tracked<Float>) {
    self.a = a
    self.b = b
    self.c = c
  }

  @differentiable(reverse, wrt: (self, x))
  func f(_ x: Tracked<Float>) -> Tracked<Float> {
    return a * x * x + b * x + c
  }
}

ProtocolRequirementAutodiffTests.testWithLeakChecking("func") {
  expectEqual((Quadratic(0, 0, 1), 12), Quadratic(11, 12, 13).gradF(at: 0))
  expectEqual((Quadratic(1, 1, 1), 2 * 11 + 12),
              Quadratic(11, 12, 13).gradF(at: 1))
  expectEqual((Quadratic(4, 2, 1), 2 * 11 * 2 + 12),
              Quadratic(11, 12, 13).gradF(at: 2))
}

// MARK: - Constructor, accessor, and subscript requirements.

protocol FunctionsOfX: Differentiable {
  @differentiable(reverse)
  init(x: Tracked<Float>)

  @differentiable(reverse)
  var x: Tracked<Float> { get }

  @differentiable(reverse)
  var y: Tracked<Float> { get }

  @differentiable(reverse)
  var z: Tracked<Float> { get }

  @differentiable(reverse)
  subscript() -> Tracked<Float> { get }
}

struct TestFunctionsOfX: FunctionsOfX {
  @differentiable(reverse)
  init(x: Tracked<Float>) {
    self.x = x
    self.y = x * x
  }

  /// x = x
  var x: Tracked<Float>

  /// y = x * x
  var y: Tracked<Float>

  /// z = x * x + x
  var z: Tracked<Float> {
    return y + x
  }

  @differentiable(reverse)
  subscript() -> Tracked<Float> {
    return z
  }
}

@inline(never)  // Prevent specialization, to test all witness code.
func derivatives<F: FunctionsOfX>(at x: Tracked<Float>, of: F.Type)
  -> (Tracked<Float>, Tracked<Float>, Tracked<Float>, Tracked<Float>)
{
  let dxdx = gradient(at: x) { x in F(x: x).x }
  let dydx = gradient(at: x) { x in F(x: x).y }
  let dzdx = gradient(at: x) { x in F(x: x).z }
  let dsubscriptdx = gradient(at: x) { x in F(x: x)[] }
  return (dxdx, dydx, dzdx, dsubscriptdx)
}

ProtocolRequirementAutodiffTests.testWithLeakChecking("constructor, accessor, subscript") {
  expectEqual(
    (1.0, 4.0, 5.0, 5.0),
    derivatives(at: 2.0, of: TestFunctionsOfX.self))
}

// MARK: - Test witness method SIL type computation.

protocol P: Differentiable {
  @differentiable(reverse, wrt: (x, y))
  func foo(_ x: Tracked<Float>, _ y: Double) -> Tracked<Float>
}
struct S: P {
  @differentiable(reverse, wrt: (x, y))
  func foo(_ x: Tracked<Float>, _ y: Double) -> Tracked<Float> {
    return x
  }
}

// MARK: - Overriding protocol method adding `@differentiable` attribute.

public protocol Distribution {
  associatedtype Value
  func logProbability(of value: Value) -> Tracked<Float>
}

public protocol DifferentiableDistribution: Differentiable, Distribution {
  @differentiable(reverse, wrt: self)
  func logProbability(of value: Value) -> Tracked<Float>
}

struct Foo: DifferentiableDistribution {
  @differentiable(reverse, wrt: self)
  func logProbability(of value: Tracked<Float>) -> Tracked<Float> {
    .zero
  }
}

@differentiable(reverse)
func blah<T: DifferentiableDistribution>(_ x: T) -> Tracked<Float>
where T.Value: AdditiveArithmetic {
  x.logProbability(of: .zero)
}

// Adding a more general `@differentiable` attribute.

public protocol DoubleDifferentiableDistribution: DifferentiableDistribution
  where Value: Differentiable {
  @differentiable(reverse, wrt: self)
  @differentiable(reverse, wrt: (self, value))
  func logProbability(of value: Value) -> Tracked<Float>
}

@differentiable(reverse)
func blah2<T: DoubleDifferentiableDistribution>(_ x: T, _ value: T.Value) -> Tracked<Float>
where T.Value: AdditiveArithmetic {
  x.logProbability(of: value)
}

// Satisfying the requirement with more wrt parameter indices than are necessary.

protocol DifferentiableFoo {
  associatedtype T: Differentiable
  @differentiable(reverse, wrt: x)
  func foo(_ x: T) -> Tracked<Float>
}

protocol MoreDifferentiableFoo: Differentiable, DifferentiableFoo {
  @differentiable(reverse, wrt: (self, x))
  func foo(_ x: T) -> Tracked<Float>
}

struct MoreDifferentiableFooStruct: MoreDifferentiableFoo {
  @differentiable(reverse, wrt: (self, x))
  func foo(_ x: Tracked<Float>) -> Tracked<Float> {
    x
  }
}

// Satisfying the requirement with a less-constrained derivative than is necessary.

protocol ExtraDerivativeConstraint {}

protocol HasExtraConstrainedDerivative {
  @differentiable(reverse)
  func requirement<T: Differentiable & ExtraDerivativeConstraint>(_ x: T) -> T
}

struct SatisfiesDerivativeWithLessConstraint: HasExtraConstrainedDerivative {
  @differentiable(reverse)
  func requirement<T: Differentiable>(_ x: T) -> T { x }
}

runAllTests()
