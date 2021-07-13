// RUN: %target-run-simple-swift
// NOTE: Verify whether forward-mode differentiation crashes. It currently does.
// RUN: not --crash %target-swift-frontend -enable-experimental-forward-mode-differentiation -emit-sil %s
// REQUIRES: executable_test

import StdlibUnittest
import DifferentiationUnittest

var ClassTests = TestSuite("ClassDifferentiation")

ClassTests.test("TrivialMember") {
  final class C: Differentiable {
    @differentiable(reverse)
    var float: Float

    @noDerivative
    final var noDerivative: Float = 1

    @differentiable(reverse)
    init(_ float: Float) {
      self.float = float
    }

    @differentiable(reverse)
    convenience init(convenience x: Float) {
      self.init(x)
    }

    @differentiable(reverse)
    func method(_ x: Float) -> Float {
      x * float
    }

    @differentiable(reverse)
    func testNoDerivative() -> Float {
      noDerivative
    }

    @differentiable(reverse)
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
  expectEqual(10, pullback(at: 3, of: { C($0) })(.init(float: 10)))
  expectEqual(10, pullback(at: 3, of: { C(convenience: $0) })(.init(float: 10)))
  // Test class method differentiation.
  expectEqual((.init(float: 3), 10), gradient(at: C(10), 3, of: { c, x in c.method(x) }))
  expectEqual(.init(float: 0), gradient(at: C(10), of: { c in c.testNoDerivative() }))
  expectEqual((.init(float: 20), .init(float: 10)),
              gradient(at: C(10), C(20), of: { c1, c2 in C.controlFlow(c1, c2, true) }))
}

ClassTests.test("NontrivialMember") {
  final class C: Differentiable {
    @differentiable(reverse)
    var float: Tracked<Float>

    @differentiable(reverse)
    init(_ float: Tracked<Float>) {
      self.float = float
    }

    @differentiable(reverse)
    func method(_ x: Tracked<Float>) -> Tracked<Float> {
      x * float
    }

    @differentiable(reverse)
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
  expectEqual(10, pullback(at: 3, of: { C($0) })(.init(float: 10)))
  // Test class method differentiation.
  expectEqual((.init(float: 3), 10), gradient(at: C(10), 3, of: { c, x in c.method(x) }))
  expectEqual((.init(float: 20), .init(float: 10)),
              gradient(at: C(10), C(20), of: { c1, c2 in C.controlFlow(c1, c2, true) }))
}

ClassTests.test("GenericNontrivialMember") {
  final class C<T: Differentiable>: Differentiable where T == T.TangentVector {
    @differentiable(reverse)
    var x: Tracked<T>

    @differentiable(reverse)
    init(_ x: T) {
      self.x = Tracked(x)
    }

    @differentiable(reverse)
    convenience init(convenience x: T) {
      self.init(x)
    }
  }
  // Test class initializer differentiation.
  expectEqual(10, pullback(at: 3, of: { C<Float>($0) })(.init(x: 10)))
  expectEqual(10, pullback(at: 3, of: { C<Float>(convenience: $0) })(.init(x: 10)))
}

// TF-1149: Test class with loadable type but address-only `TangentVector` type.
ClassTests.test("AddressOnlyTangentVector") {
  final class C<T: Differentiable>: Differentiable {
    @differentiable(reverse)
    var stored: T

    @differentiable(reverse)
    init(_ stored: T) {
      self.stored = stored
    }

    @differentiable(reverse)
    func method(_ x: T) -> T {
      stored
    }
  }
  // Test class initializer differentiation.
  expectEqual(10, pullback(at: 3, of: { C<Float>($0) })(.init(stored: 10)))
  // Test class method differentiation.
  expectEqual((.init(stored: Float(1)), 0),
              gradient(at: C<Float>(3), 3, of: { c, x in c.method(x) }))
}

// TF-1175: Test whether class-typed arguments are not marked active.
ClassTests.test("ClassArgumentActivity") {
  class C: Differentiable {
    @differentiable(reverse)
    var x: Float

    init(_ x: Float) {
      self.x = x
    }

    // Note: this method mutates `self`. However, since `C` is a class, the
    // method type does not involve `inout` arguments: `(C) -> () -> ()`.
    func square() {
      x *= x
    }
  }

  // Returns `x * x`.
  func squared(_ x: Float) -> Float {
    var c = C(x)
    c.square() // FIXME(TF-1175): doesn't get differentiated!
    return c.x
  }
  // FIXME(TF-1175): Find a robust solution so that derivatives are correct.
  // expectEqual((100, 20), valueWithGradient(at: 10, of: squared))
  expectEqual((100, 1), valueWithGradient(at: 10, of: squared))
}

ClassTests.test("FinalClassMethods") {
  final class Final: Differentiable {
    func method(_ x: Tracked<Float>) -> Tracked<Float> {
      return x * x
    }
  }

  for i in -5...5 {
  expectEqual(Tracked<Float>(Float(i * 2)),
              gradient(at: Tracked<Float>(Float(i))) {
                x in Final().method(x)
              })
  }
}

ClassTests.test("ClassMethods") {
  class Super {
    @differentiable(reverse, wrt: x)
    func f(_ x: Tracked<Float>) -> Tracked<Float> {
      return 2 * x
    }

    @derivative(of: f)
    final func jvpf(_ x: Tracked<Float>) -> (
      value: Tracked<Float>, differential: (Tracked<Float>) -> Tracked<Float>
    ) {
      return (f(x), { v in 2 * v })
    }

    @derivative(of: f)
    final func vjpf(_ x: Tracked<Float>) -> (
      value: Tracked<Float>, pullback: (Tracked<Float>) -> Tracked<Float>
    ) {
      return (f(x), { v in 2 * v })
    }
  }

  class SubOverride: Super {
    @differentiable(reverse, wrt: x)
    override func f(_ x: Tracked<Float>) -> Tracked<Float> {
      return 3 * x
    }
  }

  class SubOverrideCustomDerivatives: Super {
    @differentiable(reverse, wrt: x)
    override func f(_ x: Tracked<Float>) -> Tracked<Float> {
      return 3 * x
    }

    @derivative(of: f)
    final func jvpf2(_ x: Tracked<Float>) -> (
      value: Tracked<Float>, differential: (Tracked<Float>) -> Tracked<Float>
    ) {
      return (f(x), { v in 3 * v })
    }

    @derivative(of: f)
    final func vjpf2(_ x: Tracked<Float>) -> (
      value: Tracked<Float>, pullback: (Tracked<Float>) -> Tracked<Float>
    ) {
      return (f(x), { v in 3 * v })
    }

  }

  func classValueWithGradient(_ c: Super) -> (Tracked<Float>, Tracked<Float>) {
    return valueWithGradient(at: 1) { c.f($0) }
  }
  expectEqual((2, 2), classValueWithGradient(Super()))
  expectEqual((3, 3), classValueWithGradient(SubOverride()))
  expectEqual((3, 3), classValueWithGradient(SubOverrideCustomDerivatives()))
}

ClassTests.test("ClassMethods - wrt self") {
  class Super: Differentiable {
    var base: Tracked<Float>

    init(base: Tracked<Float>) {
      self.base = base
    }

    @differentiable(reverse, wrt: (self, x))
    func f(_ x: Tracked<Float>) -> Tracked<Float> {
      return base * x
    }

    @derivative(of: f)
    final func jvpf(
      _ x: Tracked<Float>
    ) -> (value: Tracked<Float>, differential: (TangentVector, Tracked<Float>) -> Tracked<Float>) {
      return (f(x), { (dself, dx) in dself.base * dx })
    }

    @derivative(of: f)
    final func vjpf(
      _ x: Tracked<Float>
    ) -> (value: Tracked<Float>, pullback: (Tracked<Float>) -> (TangentVector, Tracked<Float>)) {
      let base = self.base
      return (f(x), { v in
        (TangentVector(base: v * x), base * v)
      })
    }
  }

  final class SubOverride: Super {
    @differentiable(reverse)
    override init(base: Tracked<Float>) {
      super.init(base: base)
    }

    @differentiable(reverse, wrt: (self, x))
    override func f(_ x: Tracked<Float>) -> Tracked<Float> {
      return 3 * x
    }
  }

  final class SubOverrideCustomDerivatives: Super {
    @differentiable(reverse)
    override init(base: Tracked<Float>) {
      super.init(base: base)
    }

    @derivative(of: init)
    static func vjpInit(base: Tracked<Float>) -> (
      value: SubOverrideCustomDerivatives, pullback: (Super.TangentVector) -> Tracked<Float>
    ) {
      return (SubOverrideCustomDerivatives(base: base), { x in x.base * 2 })
    }

    @differentiable(reverse, wrt: (self, x))
    @differentiable(reverse, wrt: x)
    override func f(_ x: Tracked<Float>) -> Tracked<Float> {
      return 3 * x
    }

    @derivative(of: f, wrt: x)
    final func jvpf2(_ x: Tracked<Float>) -> (
      value: Tracked<Float>, differential: (Tracked<Float>) -> Tracked<Float>
    ) {
      return (f(x), { v in 3 * v })
    }

    @derivative(of: f, wrt: x)
    final func vjpf2(_ x: Tracked<Float>) -> (
      value: Tracked<Float>, pullback: (Tracked<Float>) -> Tracked<Float>
    ) {
      return (f(x), { v in 3 * v })
    }
  }

  let v = Super.TangentVector(base: 100)
  expectEqual(100, pullback(at: 1337) { x in Super(base: x) }(v))
  expectEqual(100, pullback(at: 1337) { x in SubOverride(base: x) }(v))
  expectEqual(200, pullback(at: 1337) { x in SubOverrideCustomDerivatives(base: x) }(v))

  // `valueWithGradient` is not used because nested tuples cannot be compared
  // with `expectEqual`.
  func classGradient(_ c: Super) -> (Super.TangentVector, Tracked<Float>) {
    return gradient(at: c, 10) { c, x in c.f(x) }
  }
  expectEqual((Super.TangentVector(base: 10), 2),
              classGradient(Super(base: 2)))
  expectEqual((Super.TangentVector(base: 0), 3),
              classGradient(SubOverride(base: 2)))
  expectEqual((Super.TangentVector(base: 0), 3),
              classGradient(SubOverrideCustomDerivatives(base: 2)))
}

ClassTests.test("ClassMethods - generic") {
  class Super<T: Differentiable & FloatingPoint> where T == T.TangentVector {
    @differentiable(reverse, wrt: x)
    func f(_ x: Tracked<T>) -> Tracked<T> {
      return Tracked<T>(2) * x
    }

    @derivative(of: f)
    final func jvpf(
      _ x: Tracked<T>
    ) -> (value: Tracked<T>, differential: (Tracked<T>.TangentVector) -> Tracked<T>.TangentVector) {
      return (f(x), { v in Tracked<T>(2) * v })
    }

    @derivative(of: f)
    final func vjpf(
      _ x: Tracked<T>
    ) -> (value: Tracked<T>, pullback: (Tracked<T>.TangentVector) -> Tracked<T>.TangentVector) {
      return (f(x), { v in Tracked<T>(2) * v })
    }
  }

  class SubOverride<T: Differentiable & FloatingPoint>: Super<T> where T == T.TangentVector {
    @differentiable(reverse, wrt: x)
    override func f(_ x: Tracked<T>) -> Tracked<T> {
      return x
    }
  }

  class SubSpecializeOverride: Super<Float> {
    @differentiable(reverse, wrt: x)
    override func f(_ x: Tracked<Float>) -> Tracked<Float> {
      return 3 * x
    }
  }

  class SubOverrideCustomDerivatives<T: Differentiable & FloatingPoint>: Super<T>
  where T == T.TangentVector {
    @differentiable(reverse, wrt: x)
    override func f(_ x: Tracked<T>) -> Tracked<T> {
      return Tracked<T>(3) * x
    }

    @derivative(of: f)
    final func jvpf2(
      _ x: Tracked<T>
    ) -> (value: Tracked<T>, differential: (Tracked<T>.TangentVector) -> Tracked<T>.TangentVector) {
      return (f(x), { v in Tracked<T>(3) * v })
    }

    @derivative(of: f)
    final func vjpf2(
      _ x: Tracked<T>
    ) -> (value: Tracked<T>, pullback: (Tracked<T>.TangentVector) -> Tracked<T>.TangentVector) {
      return (f(x), { v in Tracked<T>(3) * v })
    }
  }

#if !(os(Windows) || os(Android) || os(FreeBSD)) && (arch(i386) || arch(x86_64))
  class SubSpecializeOverrideCustomDerivatives: Super<Float80> {
    @differentiable(reverse, wrt: x)
    override func f(_ x: Tracked<Float80>) -> Tracked<Float80> {
      return 3 * x
    }
    @derivative(of: f)
    final func jvpf2(
      _ x: Tracked<Float80>
    ) -> (value: Tracked<Float80>, differential: (Tracked<Float80>) -> Tracked<Float80>) {
      return (f(x), { v in 3 * v })
    }
    @derivative(of: f)
    final func vjpf2(
      _ x: Tracked<Float80>
    ) -> (value: Tracked<Float80>, pullback: (Tracked<Float80>) -> Tracked<Float80>) {
      return (f(x), { v in 3 * v })
    }
  }
#endif

  func classValueWithGradient<T: Differentiable & FloatingPoint>(
    _ c: Super<T>
  ) -> (T, T) where T == T.TangentVector {
    let (x,y) =  valueWithGradient(at: Tracked<T>(1), of: {
        c.f($0) })
    return (x.value, y.value)
  }
  expectEqual((2, 2), classValueWithGradient(Super<Float>()))
  expectEqual((1, 1), classValueWithGradient(SubOverride<Float>()))
  expectEqual((3, 3), classValueWithGradient(SubSpecializeOverride()))
  expectEqual((3, 3), classValueWithGradient(SubOverrideCustomDerivatives<Float>()))
#if !(os(Windows) || os(Android) || os(FreeBSD)) && (arch(i386) || arch(x86_64))
  expectEqual((3, 3), classValueWithGradient(SubSpecializeOverrideCustomDerivatives()))
#endif
}

ClassTests.test("ClassMethods - closure captures") {
  class Multiplier {
    var coefficient: Tracked<Float>
    init(_ coefficient: Tracked<Float>) {
      self.coefficient = coefficient
    }

    // Case 1: generated VJP.
    @differentiable(reverse)
    func apply1(to x: Tracked<Float>) -> Tracked<Float> {
      return coefficient * x
    }

    // Case 2: custom VJP capturing `self`.
    @differentiable(reverse, wrt: (x))
    func apply2(to x: Tracked<Float>) -> Tracked<Float> {
      return coefficient * x
    }

    @derivative(of: apply2)
    final func vjpApply2(
      to x: Tracked<Float>
    ) -> (value: Tracked<Float>, pullback: (Tracked<Float>) -> Tracked<Float>) {
      return (coefficient * x, { v in self.coefficient * v })
    }

    // Case 3: custom VJP capturing `self.coefficient`.
    @differentiable(reverse, wrt: x)
    func apply3(to x: Tracked<Float>) -> Tracked<Float> {
      return coefficient * x
    }

    @derivative(of: apply3)
    final func vjpApply3(
      to x: Tracked<Float>
    ) -> (value: Tracked<Float>, pullback: (Tracked<Float>) -> Tracked<Float>) {
      let coefficient = self.coefficient
      return (coefficient * x, { v in coefficient * v })
    }
  }

  func f1(_ x: Tracked<Float>) -> Tracked<Float> {
    let m = Multiplier(10)
    let result = m.apply1(to: x)
    m.coefficient += 1
    return result
  }
  expectEqual(10, gradient(at: 1, of: f1))

  func f2(_ x: Tracked<Float>) -> Tracked<Float> {
    let m = Multiplier(10)
    let result = m.apply2(to: x)
    m.coefficient += 1
    return result
  }
  expectEqual(11, gradient(at: 1, of: f2))

  func f3(_ x: Tracked<Float>) -> Tracked<Float> {
    let m = Multiplier(10)
    let result = m.apply3(to: x)
    m.coefficient += 1
    return result
  }
  expectEqual(10, gradient(at: 1, of: f3))
}

ClassTests.test("ClassProperties") {
  class Super: Differentiable {
    @differentiable(reverse)
    var base: Tracked<Float>

    init(base: Tracked<Float>) { self.base = base }

    @differentiable(reverse)
    var squared: Tracked<Float> { base * base }

    @derivative(of: squared)
    final func vjpSquared() -> (value: Tracked<Float>, pullback: (Tracked<Float>) -> TangentVector) {
      let base = self.base
      return (base * base, { v in TangentVector(base: 2 * base * v) })
    }
  }

  class Sub1: Super {
    @differentiable(reverse)
    override var squared: Tracked<Float> { base * base }
  }

  func classValueWithGradient(_ c: Super) -> (Tracked<Float>, Super.TangentVector) {
    return valueWithGradient(at: c) { c in c.squared }
  }

  expectEqual(4, gradient(at: 2) { x in Super(base: x).squared })
  expectEqual(Super.TangentVector(base: 4),
              gradient(at: Super(base: 2)) { foo in foo.squared })
}

ClassTests.test("LetProperties") {
  final class Foo: Differentiable {
    var x: Tracked<Float>
    init(x: Tracked<Float>) { self.x = x }
  }
  final class Bar: Differentiable {
    let x = Foo(x: 2)
  }
  let bar = Bar()
  let grad = gradient(at: bar) { bar in (bar.x.x * bar.x.x).value }
  expectEqual(Bar.TangentVector(x: .init(x: 6.0)), grad)
  bar.move(by: grad)
  expectEqual(8.0, bar.x.x)
}

runAllTests()
