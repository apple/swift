// RUN: %target-typecheck-verify-swift -swift-version 5 -enable-objc-interop

// ----------------------------------------------------------------------------
// DynamicSelf is only allowed on the return type of class and
// protocol methods.
func global() -> Self { } // expected-error{{global function cannot return 'Self'}}

func inFunction() {
  func local() -> Self { } // expected-error{{local function cannot return 'Self'}}
}

struct S0 {
  func f() -> Self { }

  func g(_ ds: Self) { }
}

enum E0 {
  func f() -> Self { }

  func g(_ ds: Self) { }
}

class C0 {
  func f() -> Self { } // okay

  func g(_ ds: Self) { } // okay

  func h(_ ds: Self) -> Self { } // okay
}

protocol P0 {
  func f() -> Self // okay

  func g(_ ds: Self) // okay
}

extension P0 {
  func h() -> Self { // okay
    func g(_ t : Self) -> Self { // okay
      return t
    }
    return g(self)
  }
}

protocol P1: class {
  func f() -> Self // okay

  func g(_ ds: Self) // okay
}

extension P1 {
  func h() -> Self { // okay
    func g(_ t : Self) -> Self { // okay
      return t
    }
    return g(self)
  }
}

// ----------------------------------------------------------------------------
// The 'self' type of a Self method is based on Self
class C1 {
  required init(int i: Int) {}

  // Instance methods have a self of type Self.
  func f(_ b: Bool) -> Self {
    // FIXME: below diagnostic should complain about C1 -> Self conversion
    if b { return C1(int: 5) } // expected-error{{cannot convert return expression of type 'C1' to return type 'Self'}}

    // One can use `type(of:)` to attempt to construct an object of type Self.
    if !b { return type(of: self).init(int: 5) }

    // Can't utter Self within the body of a method.
    var _: Self = self

    // Okay to return 'self', because it has the appropriate type.
    return self // okay
  }

  // Type methods have a self of type Self.Type.
  class func factory(_ b: Bool) -> Self {
    // Check directly.
    var x: Int = self // expected-error{{cannot convert value of type 'Self.Type' to specified type 'Int'}}

    // Can't utter Self within the body of a method.
    var c1 = C1(int: 5) as Self // expected-error{{'C1' is not convertible to 'Self'; did you mean to use 'as!' to force downcast?}}

    if b { return self.init(int: 5) }

    return Self() // expected-error{{non-nominal type 'Self' does not support explicit initialization}}
  }

  // This used to crash because metatype construction went down a
  // different code path that didn't handle DynamicSelfType.
  class func badFactory() -> Self {
    return self(int: 0)
    // expected-error@-1 {{initializing from a metatype value must reference 'init' explicitly}}
  }
}

// ----------------------------------------------------------------------------
// Using a method with a Self result carries the receiver type.
class X {
  func instance() -> Self {
  }

  class func factory() -> Self {
  }

  func produceX() -> X { }
}

class Y : X { 
  func produceY() -> Y { }
}

func testInvokeInstanceMethodSelf() {
  // Trivial case: invoking on the declaring class.
  var x = X()
  var x2 = x.instance()
  x = x2 // at least an X
  x2 = x // no more than an X

  // Invoking on a subclass.
  var y = Y()
  var y2 = y.instance();
  y = y2 // at least a Y
  y2 = y // no more than a Y
}

func testInvokeTypeMethodSelf() {
  // Trivial case: invoking on the declaring class.
  var x = X()
  var x2 = X.factory()
  x = x2 // at least an X
  x2 = x // no more than an X

  // Invoking on a subclass.
  var y = Y()
  var y2 = Y.factory()
  y = y2 // at least a Y
  y2 = y // no more than a Y
}

func testCurryInstanceMethodSelf() {
  // Trivial case: currying on the declaring class.
  var produceX = X.produceX
  var produceX2 = X.instance
  produceX = produceX2
  produceX2 = produceX

  // Currying on a subclass.
  var produceY = Y.produceY
  var produceY2 = Y.instance
  produceY = produceY2
  produceY2 = produceY
}

class GX<T> {
  func instance() -> Self {
  }

  class func factory() -> Self {
  }

  func produceGX() -> GX { }
}

class GY<T> : GX<[T]> { 
  func produceGY() -> GY { }
}

func testInvokeInstanceMethodSelfGeneric() {
  // Trivial case: invoking on the declaring class.
  var x = GX<Int>()
  var x2 = x.instance()
  x = x2 // at least an GX<Int>
  x2 = x // no more than an GX<Int>

  // Invoking on a subclass.
  var y = GY<Int>()
  var y2 = y.instance();
  y = y2 // at least a GY<Int>
  y2 = y // no more than a GY<Int>
}

func testInvokeTypeMethodSelfGeneric() {
  // Trivial case: invoking on the declaring class.
  var x = GX<Int>()
  var x2 = GX<Int>.factory()
  x = x2 // at least an GX<Int>
  x2 = x // no more than an GX<Int>

  // Invoking on a subclass.
  var y = GY<Int>()
  var y2 = GY<Int>.factory();
  y = y2 // at least a GY<Int>
  y2 = y // no more than a GY<Int>
}

func testCurryInstanceMethodSelfGeneric() {
  // Trivial case: currying on the declaring class.
  var produceGX = GX<Int>.produceGX
  var produceGX2 = GX<Int>.instance
  produceGX = produceGX2
  produceGX2 = produceGX

  // Currying on a subclass.
  var produceGY = GY<Int>.produceGY
  var produceGY2 = GY<Int>.instance
  produceGY = produceGY2
  produceGY2 = produceGY
}

// ----------------------------------------------------------------------------
// Overriding a method with a Self
class Z : Y { 
  override func instance() -> Self {
  }

  override class func factory() -> Self {
  }
}

func testOverriddenMethodSelfGeneric() {
  var z = Z()

  var z2 = z.instance();
  z = z2
  z2 = z

  var z3 = Z.factory()
  z = z3
  z3 = z
}

// ----------------------------------------------------------------------------
// Generic uses of Self methods.
protocol P {
  func f() -> Self
}

func testGenericCall<T: P>(_ t: T) {
  var t = t
  var t2 = t.f()
  t2 = t
  t = t2
}

// ----------------------------------------------------------------------------
// Existential uses of Self methods.
func testExistentialCall(_ p: P) {
  _ = p.f()
}

// ----------------------------------------------------------------------------
// Dynamic lookup of Self methods.
@objc class SomeClass {
  @objc func method() -> Self { return self }
}

func testAnyObject(_ ao: AnyObject) {
  var ao = ao
  var result : AnyObject = ao.method!()
  result = ao
  ao = result
}

// ----------------------------------------------------------------------------
// Name lookup on Self values
extension Y {
  func testInstance() -> Self {
    if false { return self.instance() }
    return instance()
  }

  class func testFactory() -> Self {
    if false { return self.factory() }
    return factory()
  }
}

// ----------------------------------------------------------------------------
// Optional Self returns

extension X {
  func tryToClone() -> Self? { return nil }
  func tryHarderToClone() -> Self! { return nil }
  func cloneOrFail() -> Self { return self }
  func cloneAsObjectSlice() -> X? { return self }
}
extension Y {
  func operationThatOnlyExistsOnY() {}
}
func testOptionalSelf(_ y : Y) {
  if let clone = y.tryToClone() {
    clone.operationThatOnlyExistsOnY()
  }

  // Sanity-checking to make sure that the above succeeding
  // isn't coincidental.
  if let clone = y.cloneOrFail() { // expected-error {{initializer for conditional binding must have Optional type, not 'Y'}}
    clone.operationThatOnlyExistsOnY()
  }

  // Sanity-checking to make sure that the above succeeding
  // isn't coincidental.
  if let clone = y.cloneAsObjectSlice() {
    clone.operationThatOnlyExistsOnY() // expected-error {{value of type 'X' has no member 'operationThatOnlyExistsOnY'}}
  }

  if let clone = y.tryHarderToClone().tryToClone() {
    clone.operationThatOnlyExistsOnY();
  }
}

// ----------------------------------------------------------------------------
// Conformance lookup on Self

protocol Runcible {
}

extension Runcible {
  func runce() {}
}

func wantsRuncible<T : Runcible>(_: T) {}

class Runce : Runcible {
  func getRunced() -> Self {
    runce()
    wantsRuncible(self)
    return self
  }
}

// ----------------------------------------------------------------------------
// Forming a type with 'Self' in invariant position

struct Generic<T> { init(_: T) {} }

class InvariantSelf {
  func me() -> Self {
    let a = Generic(self)
    let _: Generic<InvariantSelf> = a
    // expected-error@-1 {{cannot convert value of type 'Generic<Self>' to specified type 'Generic<InvariantSelf>'}}

    return self
  }
}

// FIXME: This should be allowed

final class FinalInvariantSelf {
  func me() -> Self {
    let a = Generic(self)
    let _: Generic<FinalInvariantSelf> = a
    // expected-error@-1 {{cannot convert value of type 'Generic<Self>' to specified type 'Generic<FinalInvariantSelf>'}}

    return self
  }
}

// ----------------------------------------------------------------------------
// Semi-bogus factory init pattern

protocol FactoryPattern {
  init(factory: @autoclosure () -> Self)
}

extension  FactoryPattern {
  init(factory: @autoclosure () -> Self) { self = factory() }
}

class Factory : FactoryPattern {
  init(_string: String) {}

  convenience init(string: String) {
    self.init(factory: Factory(_string: string))
    // expected-error@-1 {{incorrect argument label in call (have 'factory:', expected '_string:')}}
    // FIXME: Bogus diagnostic
  }
}

// Final classes are OK

final class FinalFactory : FactoryPattern {
  init(_string: String) {}

  convenience init(string: String) {
    self.init(factory: FinalFactory(_string: string))
  }
}

// Operators returning Self

class SelfOperator {
  required init() {}

  static func +(lhs: SelfOperator, rhs: SelfOperator) -> Self {
    return self.init()
  }

  func double() -> Self {
    // FIXME: Should this work?
    return self + self // expected-error {{cannot convert return expression of type 'SelfOperator' to return type 'Self'}}
  }
}

func useSelfOperator() {
  let s = SelfOperator()
  _ = s + s
}
