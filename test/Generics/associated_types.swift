// RUN: %target-typecheck-verify-swift

// Deduction of associated types.
protocol Fooable {
  associatedtype AssocType
  func foo(_ x : AssocType)
}

struct X : Fooable {
  func foo(_ x: Float) {}
}

struct Y<T> : Fooable {
  func foo(_ x: T) {}
}

struct Z : Fooable {
  func foo(_ x: Float) {}

  func blarg() -> AssocType {}

  func wonka() -> Z.AssocType {}
}

var xa : X.AssocType = Float()
var yf : Y<Float>.AssocType = Float()
var yd : Y<Double>.AssocType = Double()

var f : Float
f = xa
f = yf

var d : Double
d = yd

protocol P1 {
  associatedtype Assoc1
  func foo() -> Assoc1
}

struct S1 : P1 {
  func foo() -> X {}
}

prefix operator %

protocol P2 {
  associatedtype Assoc2
  static prefix func %(target: Self) -> Assoc2
}

prefix func % <P:P1>(target: P) -> P.Assoc1 {
}

extension S1 : P2 {
  typealias Assoc2 = X
}

// <rdar://problem/14418181>
protocol P3 {
  associatedtype Assoc3
  func foo() -> Assoc3
}

protocol P4 : P3 {
  associatedtype Assoc4
  func bar() -> Assoc4
}

func takeP4<T : P4>(_ x: T) { }

struct S4<T> : P3, P4 {
  func foo() -> Int {}
  func bar() -> Double {}
}

takeP4(S4<Int>())

// <rdar://problem/14680393>
infix operator ~>

protocol P5 { }

struct S7a {}

protocol P6 {
  func foo<Target: P5>(_ target: inout Target)
}

protocol P7 : P6 {
  associatedtype Assoc : P6
  static func ~> (x: Self, _: S7a) -> Assoc
}

func ~> <T:P6>(x: T, _: S7a) -> S7b { return S7b() }

struct S7b : P7 {
  typealias Assoc = S7b
  func foo<Target: P5>(_ target: inout Target) {}
}

// <rdar://problem/14685674>
struct zip<A : IteratorProtocol, B : IteratorProtocol>
  : IteratorProtocol, Sequence {

  func next() -> (A.Element, B.Element)? { }

  typealias Generator = zip
  func makeIterator() -> zip { }
}

protocol P8 { }

protocol P9 {
  associatedtype A1 : P8
}

protocol P10 {
  associatedtype A1b : P8
  associatedtype A2 : P9

  func f()
  func g(_ a: A1b)
  func h(_ a: A2)
}

struct X8 : P8 { }

struct Y9 : P9 {
  typealias A1 = X8
}

struct Z10 : P10 {
  func f() { }
  func g(_ a: X8) { }
  func h(_ a: Y9) { }
}


struct W : Fooable {
  func foo(_ x: String) {}
}
struct V<T> : Fooable {
  func foo(_ x: T) {}
}

// FIXME: <rdar://problem/16123805> associated Inferred types can't be used in expression contexts
var w = W.AssocType()
var v = V<String>.AssocType()

//
// SR-427
protocol A {
  func c()
}

protocol B : A {
  associatedtype e : A = C<Self>
}

extension B {
  func c() {
  }
}

struct C<a : B> : B {
}

struct CC : B {
  typealias e = CC
}

C<CC>().c()

// SR-511
protocol sr511 {
  typealias Foo // expected-error {{type alias is missing an assigned type; use 'associatedtype' to define an associated type requirement}}
}

associatedtype Foo = Int // expected-error {{associated types can only be defined in a protocol; define a type or introduce a 'typealias' to satisfy an associated type requirement}}

// rdar://problem/29207581
protocol P {
  associatedtype A
  static var isP : Bool { get }
}

protocol M {
  associatedtype B : P
}

extension M {
  func g<C : P>(in c_: C)
  where Self.B == C.A, C.A.A : P { // *clearly* implies Self.B.A : P
    _ = B.A.isP
  }
}

// SR-6097
protocol sr6097 {
  associatedtype A : AnyObject
  var aProperty: A { get }
}

class C1 {}
class C2 : sr6097 {
  unowned let aProperty: C1 // should deduce A == C1 despite 'unowned'
  init() { fatalError() }
}

protocol sr6097_b {
  associatedtype A : AnyObject
  var aProperty: A? { get }
}
class C3 : sr6097_b {
  weak var aProperty: C1? // and same here, despite 'weak'
  init() { fatalError() }
}
class G<T> : sr6097_b where T : AnyObject {
  weak var aProperty: T?
}


