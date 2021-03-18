// RUN: %target-typecheck-verify-swift

protocol Fooable { func foo() }
protocol Barable { func bar() }

extension Int : Fooable, Barable {
  func foo() {}
  func bar() {}
}

extension Float32 : Barable {
  func bar() {}
}

func f0(_: Barable) {}
func f1(_ x: Fooable & Barable) {}
func f2(_: Float) {}
let nilFunc: Optional<(Barable) -> ()> = nil

func g(_: (Barable & Fooable) -> ()) {}

protocol Classable : AnyObject {}
class SomeArbitraryClass {}

func fc0(_: Classable) {}
func fc1(_: Fooable & Classable) {}
func fc2(_: AnyObject) {}
func fc3(_: SomeArbitraryClass) {}
func gc(_: (Classable & Fooable) -> ()) {}

var i : Int
var f : Float
var b : Barable

//===----------------------------------------------------------------------===//
// Conversion to and among existential types
//===----------------------------------------------------------------------===//

f0(i)
f0(f)
f0(b)
f1(i)

f1(f) // expected-error{{argument type 'Float' does not conform to expected type 'Fooable'}}
f1(b) // expected-error{{argument type 'Barable' does not conform to expected type 'Fooable'}}

//===----------------------------------------------------------------------===//
// Subtyping
//===----------------------------------------------------------------------===//
g(f0) // okay (subtype)
g(f1) // okay (exact match)

g(f2) // expected-error{{cannot convert value of type '(Float) -> ()' to expected argument type '(Barable & Fooable) -> ()'}}
g(nilFunc ?? f0)

gc(fc0) // okay
gc(fc1) // okay
gc(fc2) // okay
gc(fc3) // expected-error{{cannot convert value of type '(SomeArbitraryClass) -> ()' to expected argument type '(Classable & Fooable) -> ()'}}

// rdar://problem/19600325
func getAnyObject() -> AnyObject? {
  return SomeArbitraryClass()
}

func castToClass(_ object: Any) -> SomeArbitraryClass? {
  return object as? SomeArbitraryClass
}

_ = getAnyObject().map(castToClass)


_ = { (_: Any) -> Void in
  return
} as ((Int) -> Void)

let _: (Int) -> Void = {
  (_: Any) -> Void in
  return
}

let _: () -> Any = {
  () -> Int in
  return 0
}

let _: () -> Int = { 
  () -> String in  // expected-error {{declared closure result 'String' is incompatible with contextual type 'Int'}}
  return ""
}

//===----------------------------------------------------------------------===//
// Members of archetypes
//===----------------------------------------------------------------------===//

func id<T>(_ t: T) -> T { return t }

protocol Initable {
  init()
}

protocol P : Initable {
  func bar(_ x: Int)
  mutating func mut(_ x: Int)
  static func tum()
  
  typealias E = Int
  typealias F = Self.E
  typealias G = Array
}

protocol ClassP : class {
  func bas(_ x: Int)
  func quux(_ x: Int)
}

class ClassC : ClassP {
  func bas(_ x: Int) {}
}

extension ClassP {
  func quux(_ x: Int) {}
  func bing(_ x: Int) {}
}

func generic<T: P>(_ t: T) {
  var t = t
  // Instance member of archetype
  let _: (Int) -> () = id(t.bar)
  let _: () = id(t.bar(0))

  // Static member of archetype metatype
  let _: () -> () = id(T.tum)

  // Instance member of archetype metatype
  let _: (T) -> (Int) -> () = id(T.bar)
  let _: (Int) -> () = id(T.bar(t))

  _ = t.mut // expected-error{{partial application of 'mutating' method is not allowed}}
  _ = t.tum // expected-error{{static member 'tum' cannot be used on instance of type 'T'}}
}

func genericClassP<T: ClassP>(_ t: T) {
  // Instance member of archetype)
  let _: (Int) -> () = id(t.bas)
  let _: () = id(t.bas(0))

  // Instance member of archetype metatype)
  let _: (T) -> (Int) -> () = id(T.bas)
  let _: (Int) -> () = id(T.bas(t))
  let _: () = id(T.bas(t)(1))
}

func genericClassC<C : ClassC>(_ c: C) {
  // Make sure that we can find members of protocol extensions
  // on a class-bound archetype
  let _ = c.bas(123)
  let _ = c.quux(123)
  let _ = c.bing(123)
}

//===----------------------------------------------------------------------===//
// Members of existentials
//===----------------------------------------------------------------------===//

func existential(_ p: P) {
  var p = p
  // Fully applied mutating method
  p.mut(1)
  _ = p.mut // expected-error{{partial application of 'mutating' method is not allowed}}

  // Instance member of existential)
  let _: (Int) -> () = id(p.bar)
  let _: () = id(p.bar(0))

  // Static member of existential metatype)
  let _: () -> () = id(type(of: p).tum)
}

func staticExistential(_ p: P.Type, pp: P.Protocol) {
  let _ = p() // expected-error{{initializing from a metatype value must reference 'init' explicitly}}
  let _ = p().bar // expected-error{{initializing from a metatype value must reference 'init' explicitly}}
  let _ = p().bar(1) // expected-error{{initializing from a metatype value must reference 'init' explicitly}}

  let ppp: P = p.init()

  _ = pp() // expected-error{{value of type 'P.Protocol' is a protocol; it cannot be instantiated}}
  _ = pp().bar // expected-error{{value of type 'P.Protocol' is a protocol; it cannot be instantiated}}
  _ = pp().bar(2) // expected-error{{value of type 'P.Protocol' is a protocol; it cannot be instantiated}}

  _ = pp.init() // expected-error{{protocol type 'P' cannot be instantiated}}
  _ = pp.init().bar // expected-error{{protocol type 'P' cannot be instantiated}}
  _ = pp.init().bar(3) // expected-error{{protocol type 'P' cannot be instantiated}}

  _ = P() // expected-error{{protocol type 'P' cannot be instantiated}}
  _ = P().bar // expected-error{{protocol type 'P' cannot be instantiated}}
  _ = P().bar(4) // expected-error{{protocol type 'P' cannot be instantiated}}

  // Instance member of metatype
  let _: (P) -> (Int) -> () = P.bar
  let _: (Int) -> () = P.bar(ppp)
  P.bar(ppp)(5)

  // Instance member of metatype value
  let _: (P) -> (Int) -> () = pp.bar
  let _: (Int) -> () = pp.bar(ppp)
  pp.bar(ppp)(5)

  // Static member of existential metatype value
  let _: () -> () = p.tum

  // Instance member of existential metatype -- not allowed
  _ = p.bar // expected-error{{instance member 'bar' cannot be used on type 'P'}}
  _ = p.mut // expected-error{{instance member 'mut' cannot be used on type 'P'}}
  // expected-error@-1 {{partial application of 'mutating' method is not allowed}}

  // Static member of metatype -- not allowed
  _ = pp.tum // expected-error{{static member 'tum' cannot be used on protocol metatype 'P.Protocol'}}
  _ = P.tum // expected-error{{static member 'tum' cannot be used on protocol metatype 'P.Protocol'}}

  // Access typealias through protocol and existential metatypes
  _ = pp.E.self
  _ = p.E.self

  _ = pp.F.self
  _ = p.F.self

  // Make sure that we open generics
  let _: [Int].Type = p.G.self
}

protocol StaticP {
  static func foo(a: Int)
}
extension StaticP {
  func bar() {
    _ = StaticP.foo(a:) // expected-error{{static member 'foo(a:)' cannot be used on protocol metatype 'StaticP.Protocol'}} {{9-16=Self}}

    func nested() {
      _ = StaticP.foo(a:) // expected-error{{static member 'foo(a:)' cannot be used on protocol metatype 'StaticP.Protocol'}} {{11-18=Self}}
    }
  }
}

func existentialClassP(_ p: ClassP) {
  // Instance member of existential)
  let _: (Int) -> () = id(p.bas)
  let _: () = id(p.bas(0))

  // Instance member of existential metatype)
  let _: (ClassP) -> (Int) -> () = id(ClassP.bas)
  let _: (Int) -> () = id(ClassP.bas(p))
  let _: () = id(ClassP.bas(p)(1))
}

// Partial application of curried protocol methods
protocol Scalar {}
protocol Vector {
  func scale(_ c: Scalar) -> Self
}
protocol Functional {
  func apply(_ v: Vector) -> Scalar
}
protocol Coalgebra {
  func coproduct(_ f: Functional) -> (_ v1: Vector, _ v2: Vector) -> Scalar
}

// Make sure existential is closed early when we partially apply
func wrap<T>(_ t: T) -> T {
  return t
}

func exercise(_ c: Coalgebra, f: Functional, v: Vector) {
  let _: (Vector, Vector) -> Scalar = wrap(c.coproduct(f))
  let _: (Scalar) -> Vector = v.scale
}

// Make sure existential isn't closed too late
protocol Copyable {
  func copy() -> Self
}

func copyTwice(_ c: Copyable) -> Copyable {
  return c.copy().copy()
}

//===----------------------------------------------------------------------===//
// Dynamic self
//===----------------------------------------------------------------------===//
protocol Clonable {
  func maybeClone() -> Self?
  func doubleMaybeClone() -> Self??
  func subdivideClone() -> (Self, Self)
  func metatypeOfClone() -> Self.Type
  func goodClonerFn() -> (() -> Self)
}

extension Clonable {
  func badClonerFn() -> ((Self) -> Self) { }

  func veryBadClonerFn() -> ((inout Self) -> ()) { }

  func extClone() -> Self { }

  func extMaybeClone(_ b: Bool) -> Self? { }

  func extProbablyClone(_ b: Bool) -> Self! { }

  static func returnSelfStatic() -> Self { }

  static func returnSelfOptionalStatic(_ b: Bool) -> Self? { }

  static func returnSelfIUOStatic(_ b: Bool) -> Self! { }
}

func testClonableArchetype<T : Clonable>(_ t: T) {
  // Instance member of extension returning Self)
  let _: (T) -> () -> T = id(T.extClone)
  let _: () -> T = id(T.extClone(t))
  let _: T = id(T.extClone(t)())

  let _: () -> T = id(t.extClone)
  let _: T = id(t.extClone())

  let _: (T) -> (Bool) -> T? = id(T.extMaybeClone)
  let _: (Bool) -> T? = id(T.extMaybeClone(t))
  let _: T? = id(T.extMaybeClone(t)(false))

  let _: (Bool) -> T? = id(t.extMaybeClone)
  let _: T? = id(t.extMaybeClone(true))

  let _: (T) -> (Bool) -> T? = id(T.extProbablyClone as (T) -> (Bool) -> T?)
  let _: (Bool) -> T? = id(T.extProbablyClone(t) as (Bool) -> T?)
  let _: T! = id(T.extProbablyClone(t)(true))

  let _: (Bool) -> T? = id(t.extProbablyClone as (Bool) -> T?)
  let _: T! = id(t.extProbablyClone(true))

  // Static member of extension returning Self)
  let _: () -> T = id(T.returnSelfStatic)
  let _: T = id(T.returnSelfStatic())

  let _: (Bool) -> T? = id(T.returnSelfOptionalStatic)
  let _: T? = id(T.returnSelfOptionalStatic(false))

  let _: (Bool) -> T? = id(T.returnSelfIUOStatic as (Bool) -> T?)
  let _: T! = id(T.returnSelfIUOStatic(true))
}

func testClonableExistential(_ v: Clonable, _ vv: Clonable.Type) {
  let _: Clonable? = v.maybeClone()
  let _: Clonable?? = v.doubleMaybeClone()
  let _: (Clonable, Clonable) = v.subdivideClone()
  let _: Clonable.Type = v.metatypeOfClone()
  let _: () -> Clonable = v.goodClonerFn()

  // Instance member of extension returning Self
  let _: () -> Clonable = id(v.extClone)
  let _: Clonable = id(v.extClone())
  let _: Clonable? = id(v.extMaybeClone(true))
  let _: Clonable! = id(v.extProbablyClone(true))

  // Static member of extension returning Self)
  let _: () -> Clonable = id(vv.returnSelfStatic)
  let _: Clonable = id(vv.returnSelfStatic())

  let _: (Bool) -> Clonable? = id(vv.returnSelfOptionalStatic)
  let _: Clonable? = id(vv.returnSelfOptionalStatic(false))

  let _: (Bool) -> Clonable? = id(vv.returnSelfIUOStatic as (Bool) -> Clonable?)
  let _: Clonable! = id(vv.returnSelfIUOStatic(true))

  let _ = v.badClonerFn() // expected-error {{member 'badClonerFn' cannot be used on value of protocol type 'Clonable'; use a conformance constraint instead}}
  let _ = v.veryBadClonerFn() // expected-error {{member 'veryBadClonerFn' cannot be used on value of protocol type 'Clonable'; use a conformance constraint instead}}

}


// rdar://problem/50099849

protocol Trivial {
  associatedtype T
}

func rdar_50099849() {
  struct A : Trivial {
    typealias T = A
  }

  struct B<C : Trivial> : Trivial { // expected-note {{'C' declared as parameter to type 'B'}}
    typealias T = C.T
  }

  struct C<W: Trivial, Z: Trivial> : Trivial where W.T == Z.T {
    typealias T = W.T
  }

  let _ = C<A, B>() // expected-error {{generic parameter 'C' could not be inferred}}
  // expected-note@-1 {{explicitly specify the generic arguments to fix this issue}} {{17-17=<<#C: Trivial#>>}}
}

// rdar://problem/50512161 - improve diagnostic when generic parameter cannot be deduced
func rdar_50512161() {
  struct Item {}

  struct TrivialItem : Trivial {
    typealias T = Item?
  }

  func foo<I>(_: I.Type = I.self, item: I.T) where I : Trivial { // expected-note {{in call to function 'foo(_:item:)'}}
    fatalError()
  }

  func bar(_ item: Item) {
    foo(item: item) // expected-error {{generic parameter 'I' could not be inferred}}
  }
}

// SR-11609: Compiler crash on missing conformance for default param
func test_sr_11609() {
  func foo<T : Initable>(_ x: T = .init()) -> T { x } // expected-note {{where 'T' = 'String'}}
  let _: String = foo()
  // expected-error@-1 {{local function 'foo' requires that 'String' conform to 'Initable'}}
}

// rdar://70814576 -- failed to produce a diagnostic when implicit value-to-optional conversion is involved.
func rdar70814576() {
  struct S {}

  func test(_: Fooable?) {
  }

  test(S()) // expected-error {{argument type 'S' does not conform to expected type 'Fooable'}}
}
