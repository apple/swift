// RUN: %target-typecheck-verify-swift

// Tests for typealias inside protocols

protocol Bad {
  associatedtype X<T>  // expected-error {{associated types must not have a generic parameter list}}
  typealias Y<T>       // expected-error {{expected '=' in type alias declaration}}
}

protocol Col {
  associatedtype Elem
  typealias E = Elem?
  var elem: Elem { get }
}

protocol CB {
  associatedtype C : Col

  // Self at top level
  typealias S = Self
  func cloneDefinitely() -> S

  // Self in bound generic position
  typealias OS = Self?
  func cloneMaybe() -> OS

  // Associated type of archetype
  typealias E = C.Elem
  func setIt(_ element: E)

  // Typealias of archetype
  typealias EE = C.E
  func setItSometimes(_ element: EE)

  // Associated type in bound generic position
  typealias OE = C.Elem?
  func setItMaybe(_ element: OE)

  // Complex type expression
  typealias FE = (C) -> (C.Elem) -> Self
  func teleport(_ fn: FE)
}

// Generic signature requirement involving protocol typealias
func go1<T : CB, U : Col>(_ col: U, builder: T) where U.Elem == T.E { // OK
  builder.setIt(col.elem)
}
func go2<T : CB, U : Col>(_ col: U, builder: T) where U.Elem == T.C.Elem { // OK
  builder.setIt(col.elem)
}

// Test for same type requirement with typealias == concrete
func go3<T : CB>(_ builder: T) where T.E == Int {
  builder.setIt(1)
}

// Test for conformance to protocol with associatedtype and another with
// typealias with same name
protocol MyIterator {
  associatedtype Elem

  func next() -> Elem?
}

protocol MySeq {
  associatedtype I : MyIterator
  typealias Elem = Self.I.Elem

  func makeIterator() -> I
  func getIndex(_ i: Int) -> Elem
  func first() -> Elem
}

extension MySeq where Self : MyIterator {
  func makeIterator() -> Self {
    return self
  }
}

func plusOne<S: MySeq>(_ s: S, i: Int) -> Int where S.Elem == Int {
  return s.getIndex(i) + 1
}

struct OneIntSeq: MySeq, MyIterator {
  let e : Float

  func next() -> Float? {
    return e
  }

  func getIndex(_ i: Int) -> Float {
    return e
  }
}

protocol MyIntIterator {
  typealias Elem = Int
}

struct MyIntSeq : MyIterator, MyIntIterator {
  func next() -> Elem? {
    return 0
  }
}

protocol MyIntIterator2 {}

extension MyIntIterator2 {
  typealias Elem = Int
}

struct MyIntSeq2 : MyIterator, MyIntIterator2 {
  func next() -> Elem? {
    return 0
  }
}

// test for conformance correctness using typealias in extension
extension MySeq {
  func first() -> Elem {
    return getIndex(0)
  }
}

// Typealiases whose underlying type is a structural type written in terms of
// associated types
protocol P1 {
    associatedtype A
    typealias F = (A) -> ()
}

protocol P2 {
    associatedtype B
}

func go3<T : P1, U : P2>(_ x: T) -> U where T.F == U.B {
}

// Specific diagnosis for things that look like Swift 2.x typealiases
protocol P3 {
  typealias T // expected-error {{type alias is missing an assigned type; use 'associatedtype' to define an associated type requirement}}
  typealias U : P2 // expected-error {{type alias is missing an assigned type; use 'associatedtype' to define an associated type requirement}}
}

// Test for not crashing on recursive aliases
protocol Circular {
  typealias Y = Self.Y // expected-error {{type alias 'Y' references itself}} expected-note {{while resolving type 'Self.Y'}}

  typealias Y2 = Y2 // expected-error {{type alias 'Y2' references itself}} expected-note {{while resolving type 'Y2'}}
  // expected-error@-1 {{circular reference}}

  typealias Y3 = Y4 // expected-error {{type alias 'Y3' references itself}} expected-note {{while resolving type 'Y4'}}
  // expected-error@-1 {{circular reference}}

  typealias Y4 = Y3 // expected-note 2{{through reference here}} expected-note {{while resolving type 'Y3'}}
}

// Qualified and unqualified references to protocol typealiases from concrete type
protocol P5 {
  associatedtype A
  typealias X = Self
  typealias T1 = Int
  typealias T2 = A
  var a: T2 { get }
}

protocol P6 {
  typealias A = Int
  typealias B = Self
}

struct T5 : P5 {
  // This is OK -- the typealias is fully concrete
  var a: P5.T1 // OK

  // Invalid -- cannot represent associated type of existential
  var v2: P5.T2 // expected-error {{type alias 'T2' can only be used with a concrete type or generic parameter base}}
  var v3: P5.X // expected-error {{type alias 'X' can only be used with a concrete type or generic parameter base}}

  // Unqualified reference to typealias from a protocol conformance
  var v4: T1 // OK
  var v5: T2 // OK

  // Qualified reference
  var v6: T5.T1 // OK
  var v7: T5.T2 // OK

  var v8 = P6.A.self
  var v9 = P6.B.self // expected-error {{type alias 'B' can only be used with a concrete type or generic parameter base}}
}

// Unqualified lookup finds typealiases in protocol extensions
protocol P7 {
  associatedtype A
  typealias Z = A
}

extension P7 {
  typealias Y = A
}

struct S7 : P7 {
  typealias A = Int

  func inTypeContext(y: Y) { }

  func inExpressionContext() {
    _ = Y.self
    _ = Z.self
    _ = T5.T1.self
    _ = T5.T2.self
  }
}

protocol P8 {
  associatedtype B

  @available(*, unavailable, renamed: "B")
  typealias A = B // expected-note{{'A' has been explicitly marked unavailable here}}
}

func testP8<T: P8>(_: T) where T.A == Int { } // expected-error{{'A' has been renamed to 'B'}}{{34-35=B}}

// Associated type resolution via lookup should find typealiases in protocol extensions
protocol Edible {
  associatedtype Snack
}

protocol CandyWrapper {
  associatedtype Wrapped
}

extension CandyWrapper where Wrapped : Edible {
  typealias Snack = Wrapped.Snack
}

struct Candy {}

struct CandyBar : CandyWrapper {
  typealias Wrapped = CandyEdible
}

struct CandyEdible : Edible {
  typealias Snack = Candy
}

// Edible.Snack is witnessed by 'typealias Snack' inside the
// constrained extension of CandyWrapper above
extension CandyBar : Edible {}

protocol P9 {
  typealias A = Int
}

func testT9a<T: P9, U>(_: T, _: U) where T.A == U { } // expected-error {{same-type requirement makes generic parameter 'U' non-generic}}
func testT9b<T: P9>(_: T) where T.A == Float { } // expected-error{{generic signature requires types 'T.A' (aka 'Int') and 'Float' to be the same}}


struct X<T> { }

protocol P10 {
  associatedtype A
  typealias T = Int

  @available(*, deprecated, message: "just use Int, silly")
  typealias V = Int
}

extension P10 {
  typealias U = Float
}

extension P10 where T == Int { } // expected-warning{{neither type in same-type constraint ('Self.T' (aka 'Int') or 'Int') refers to a generic parameter or associated type}}

extension P10 where A == X<T> { }

extension P10 where A == X<U> { }

extension P10 where A == X<Self.U> { }

extension P10 where V == Int { } // expected-warning {{'V' is deprecated: just use Int, silly}}
// expected-warning@-1{{neither type in same-type constraint ('Self.V' (aka 'Int') or 'Int') refers to a generic parameter or associated type}}

// rdar://problem/36003312
protocol P11 {
  typealias A = Y11
}

struct X11<T: P11> { }
struct Y11: P11 { }

extension P11 {
  func foo(_: X11<Self.A>) { }
}

// Ordering issue
struct SomeConformingType : UnboundGenericAliasProto {
  func f(_: G<Int>) {}
}

protocol UnboundGenericAliasProto {
  typealias G = X
}

// If pre-checking cannot resolve a member type due to ambiguity,
// we go down the usual member access path. Make sure its correct
// for protocol typealiases.
protocol Amb1 {
  typealias T = Int
}

protocol Amb2 {
  typealias T = String
}

typealias Amb = Amb1 & Amb2

let _: Int.Type = Amb.T.self
let _: String.Type = Amb.T.self
