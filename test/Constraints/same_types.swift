// RUN: %target-typecheck-verify-swift -warn-redundant-requirements
// RUN: not %target-swift-frontend -typecheck %s -debug-generic-signatures 2>&1 | %FileCheck %s

protocol Fooable {
  associatedtype Foo // expected-note{{protocol requires nested type 'Foo'; do you want to add it?}}

  var foo: Foo { get }
}

protocol Barrable {
  associatedtype Bar: Fooable
  var bar: Bar { get }
}

struct X {}
struct Y: Fooable {
  typealias Foo = X
  var foo: X { return X() }
}
struct Z: Barrable {
  typealias Bar = Y
  var bar: Y { return Y() }
}

protocol TestSameTypeRequirement {
  func foo<F1: Fooable>(_ f: F1) where F1.Foo == X
}
struct SatisfySameTypeRequirement : TestSameTypeRequirement {
  func foo<F2: Fooable>(_ f: F2) where F2.Foo == X {}
}

func test1<T: Fooable>(_ fooable: T) -> X where T.Foo == X {
  return fooable.foo
}

struct NestedConstraint<T> {
  func tFoo<U: Fooable>(_ fooable: U) -> T where U.Foo == T {
    return fooable.foo
  }
}

// CHECK-LABEL: same_types.(file).test2(_:u:)@
// CHECK-NEXT: Generic signature: <T, U where T : Fooable, U : Fooable, T.[Fooable]Foo == X, U.[Fooable]Foo == X>
func test2<T: Fooable, U: Fooable>(_ t: T, u: U) -> (X, X)
  where T.Foo == X, U.Foo == T.Foo {
  return (t.foo, u.foo)
}

// CHECK-LABEL: same_types.(file).test2a(_:u:)@
// CHECK-NEXT: Generic signature: <T, U where T : Fooable, U : Fooable, T.[Fooable]Foo == X, U.[Fooable]Foo == X>
func test2a<T: Fooable, U: Fooable>(_ t: T, u: U) -> (X, X)
  where T.Foo == X, T.Foo == U.Foo {
  return (t.foo, u.foo)
}

// CHECK-LABEL: same_types.(file).test3(_:u:)@
// CHECK-NEXT: Generic signature: <T, U where T : Fooable, U : Fooable, T.[Fooable]Foo == X, U.[Fooable]Foo == X>
func test3<T: Fooable, U: Fooable>(_ t: T, u: U) -> (X, X)
  where T.Foo == X, U.Foo == X, T.Foo == U.Foo {
	// expected-warning@-1{{redundant same-type constraint 'T.Foo' == 'X'}}
  return (t.foo, u.foo)
}

// CHECK-LABEL: same_types.(file).fail1(_:u:)@
// CHECK-NEXT: Generic signature: <T, U where T : Fooable, U : Fooable, T.[Fooable]Foo == U.[Fooable]Foo>
func fail1< // expected-error{{no type for 'T.Foo' can satisfy both 'T.Foo == X' and 'T.Foo == Y'}}
  T: Fooable, U: Fooable
>(_ t: T, u: U) -> (X, Y)
  where T.Foo == X, U.Foo == Y, T.Foo == U.Foo {
  return (t.foo, u.foo) // expected-error{{cannot convert return expression of type '(T.Foo, T.Foo)' to return type '(X, Y)'}}
}

// CHECK-LABEL: same_types.(file).fail2(_:u:)@
// CHECK-NEXT: Generic signature: <T, U where T : Fooable, U : Fooable, T.[Fooable]Foo == U.[Fooable]Foo>
func fail2< // expected-error{{no type for 'T.Foo' can satisfy both 'T.Foo == Y' and 'T.Foo == X'}}
  T: Fooable, U: Fooable
>(_ t: T, u: U) -> (X, Y)
  where T.Foo == U.Foo, T.Foo == X, U.Foo == Y {
  return (t.foo, u.foo) // expected-error{{cannot convert return expression of type '(T.Foo, T.Foo)' to return type '(X, Y)'}}
}

func test4<T: Barrable>(_ t: T) -> Y where T.Bar == Y {
  return t.bar
}

// CHECK-LABEL: same_types.(file).fail3@
// CHECK-NEXT: Generic signature: <T where T : Barrable>
func fail3<T: Barrable>(_ t: T) -> X // expected-error {{no type for 'T.Bar' can satisfy both 'T.Bar == X' and 'T.Bar : Fooable'}}
  where T.Bar == X {
  return t.bar // expected-error{{cannot convert return expression of type 'T.Bar' }}
}

func test5<T: Barrable>(_ t: T) -> X where T.Bar.Foo == X {
  return t.bar.foo
}

func test6<T: Barrable>(_ t: T) -> (Y, X) where T.Bar == Y {
  return (t.bar, t.bar.foo)
}

// CHECK-LABEL: same_types.(file).test7@
// CHECK-NEXT: Generic signature: <T where T : Barrable, T.[Barrable]Bar == Y>
func test7<T: Barrable>(_ t: T) -> (Y, X) where T.Bar == Y, T.Bar.Foo == X {
	// expected-warning@-1{{redundant same-type constraint 'Y.Foo' (aka 'X') == 'X'}}
  return (t.bar, t.bar.foo)
}

// CHECK-LABEL: same_types.(file).fail4@
// CHECK-NEXT: Generic signature: <T where T : Barrable, T.[Barrable]Bar == Y>
func fail4<T: Barrable>(_ t: T) -> (Y, Z)
  where
  T.Bar == Y,
  T.Bar.Foo == Z { // expected-error{{generic signature requires types 'Y.Foo' (aka 'X') and 'Z' to be the same}}
  return (t.bar, t.bar.foo) // expected-error{{cannot convert return expression of type '(Y, X)' to return type '(Y, Z)'}}
}

// CHECK-LABEL: same_types.(file).fail5@
// CHECK-NEXT: Generic signature: <T where T : Barrable, T.[Barrable]Bar == Y>
func fail5<T: Barrable>(_ t: T) -> (Y, Z)
  where
  T.Bar.Foo == Z, // expected-error{{generic signature requires types 'Y.Foo' (aka 'X') and 'Z' to be the same}}
  T.Bar == Y {
  return (t.bar, t.bar.foo) // expected-error{{cannot convert return expression of type '(Y, X)' to return type '(Y, Z)'}}
}

// CHECK-LABEL: same_types.(file).test8@
// CHECK-NEXT: Generic signature: <T where T : Fooable>
func test8<T: Fooable>(_ t: T) // expected-error{{no type for 'T.Foo' can satisfy both 'T.Foo == Y' and 'T.Foo == X'}}
  where T.Foo == X,
  T.Foo == Y {}


func testAssocTypeEquivalence<T: Fooable>(_ fooable: T) -> X.Type
  where T.Foo == X {
  return T.Foo.self
}

func fail6<T>(_ t: T) -> Int where T == Int { // expected-warning{{same-type requirement makes generic parameter 'T' non-generic}}
  return t
}

// CHECK-LABEL: same_types.(file).test8(_:u:)@
// CHECK-NEXT: Generic signature: <T, U where T : Barrable, U : Barrable, T.[Barrable]Bar == Y, U.[Barrable]Bar == Y>
func test8<T: Barrable, U: Barrable>(_ t: T, u: U) -> (Y, Y, X, X)
  where T.Bar == Y,
        U.Bar.Foo == X, T.Bar == U.Bar { // expected-warning{{redundant same-type constraint 'U.Bar.Foo' == 'X'}}
  return (t.bar, u.bar, t.bar.foo, u.bar.foo)
}

// CHECK-LABEL: same_types.(file).test8a(_:u:)@
// CHECK-NEXT: Generic signature: <T, U where T : Barrable, U : Barrable, T.[Barrable]Bar == Y, U.[Barrable]Bar == Y>
func test8a<T: Barrable, U: Barrable>(_ t: T, u: U) -> (Y, Y, X, X)
  where
  T.Bar == Y,
  U.Bar.Foo == X, U.Bar == T.Bar { // expected-warning{{redundant same-type constraint 'U.Bar.Foo' == 'X'}}
  return (t.bar, u.bar, t.bar.foo, u.bar.foo)
}

// CHECK-LABEL: same_types.(file).test8b(_:u:)@
// CHECK-NEXT: Generic signature: <T, U where T : Barrable, U : Barrable, T.[Barrable]Bar == Y, U.[Barrable]Bar == Y>
func test8b<T: Barrable, U: Barrable>(_ t: T, u: U)
  where U.Bar.Foo == X, // expected-warning{{redundant same-type constraint 'U.Bar.Foo' == 'X'}}
        T.Bar == Y,
        T.Bar == U.Bar {
}

// rdar://problem/19137463
func rdar19137463<T>(_ t: T) where T.a == T {} // expected-error{{'a' is not a member type of type 'T'}}
rdar19137463(1)


struct Brunch<U : Fooable> where U.Foo == X {}

struct BadFooable : Fooable { // expected-error{{type 'BadFooable' does not conform to protocol 'Fooable'}}
  typealias Foo = DoesNotExist // expected-error{{cannot find type 'DoesNotExist' in scope}}
  var foo: Foo { while true {} }
}

func bogusInOutError(d: inout Brunch<BadFooable>) {}

// Some interesting invalid cases that used to crash
protocol P {
  associatedtype A
  associatedtype B
}

struct Q : P {
  typealias A = Int
  typealias B = Int
}

struct S1<T : P> {
  // CHECK-LABEL: same_types.(file).S1.foo(x:y:)@
  // CHECK-NEXT: Generic signature: <T, X, Y where T : P, X == T.[P]A, Y == T.[P]B>
  func foo<X, Y>(x: X, y: Y) where X == T.A, Y == T.B {
    print(X.self)
    print(Y.self)
    print(x)
    print(y)
  }
}
S1<Q>().foo(x: 1, y: 2)

struct S2<T : P> where T.A == T.B {
  // CHECK-LABEL: same_types.(file).S2.foo(x:y:)@
  // CHECK-NEXT: <T, X, Y where T : P, X == Y, Y == T.[P]A, T.[P]A == T.[P]B>
  func foo<X, Y>(x: X, y: Y) where X == T.A, Y == T.B {  // expected-warning{{same-type requirement makes generic parameters 'Y' and 'X' equivalent}}
  // expected-warning@-1 {{redundant same-type constraint 'X' == 'T.A'}}
    print(X.self)
    print(Y.self)
    print(x)
    print(y)
  }
}
S2<Q>().foo(x: 1, y: 2)

struct S3<T : P> {
  // CHECK-LABEL: same_types.(file).S3.foo(x:y:)@
  // CHECK-NEXT: <T, X, Y where T : P, X == Y, Y == T.[P]A>
  func foo<X, Y>(x: X, y: Y) where X == T.A, Y == T.A {} // expected-warning{{same-type requirement makes generic parameters 'Y' and 'X' equivalent}}
}
S3<Q>().foo(x: 1, y: 2)

// Secondaries can be equated OK, even if we're imposing
// new conformances onto an outer secondary

protocol PPP {}

protocol PP {
  associatedtype A : PPP
}

struct SSS : PPP {}
struct SS : PP { typealias A = SSS }

struct QQ : P {
  typealias A = SSS
  typealias B = Int
}

struct S4<T : P> {
  // CHECK-LABEL: same_types.(file).S4.foo(x:)@
  // CHECK-NEXT: Generic signature: <T, X where T : P, X : PP, T.[P]A == X.[PP]A>
  func foo<X : PP>(x: X) where X.A == T.A {
    print(x)
    print(X.self)
  }
}

S4<QQ>().foo(x: SS())

// rdar://problem/29333056 - allow deeper matching of same-type constraints.
struct X1<A, B> { }

protocol P1 {
  associatedtype Assoc
}

// CHECK-LABEL: same_types.(file).structuralSameType1@
// CHECK-NEXT: Generic signature: <A, B, T, U, V, W where A : P1, B : P1, T == V, U == W, A.[P1]Assoc == X1<T, U>, B.[P1]Assoc == X1<T, U>>
func structuralSameType1<A: P1, B: P1, T, U, V, W>(_: A, _: B, _: T, _: U, _: V, _: W)
  where A.Assoc == X1<T, U>, B.Assoc == X1<V, W>, A.Assoc == B.Assoc { }
// expected-warning@-2{{same-type requirement makes generic parameters 'V' and 'T' equivalent}}
// expected-warning@-3{{same-type requirement makes generic parameters 'W' and 'U' equivalent}}

typealias Tuple2<T, U> = (T, U)

// CHECK-LABEL: same_types.(file).structuralSameType2@
// CHECK-NEXT: Generic signature: <A, B, T, U, V, W where A : P1, B : P1, T == V, U == W, A.[P1]Assoc == (T, U), B.[P1]Assoc == (T, U)>
func structuralSameType2<A: P1, B: P1, T, U, V, W>(_: A, _: B, _: T, _: U, _: V, _: W)
  where A.Assoc == Tuple2<T, U>, B.Assoc == Tuple2<V, W>, A.Assoc == B.Assoc { }
// expected-warning@-2{{same-type requirement makes generic parameters 'V' and 'T' equivalent}}
// expected-warning@-3{{same-type requirement makes generic parameters 'W' and 'U' equivalent}}

// CHECK-LABEL: same_types.(file).structuralSameType3@
// CHECK-NEXT: Generic signature: <T, U, V, W where T == V, U == W>
func structuralSameType3<T, U, V, W>(_: T, _: U, _: V, _: W)
  where X1<T, U> == X1<V, W> { }
// expected-warning@-2{{same-type requirement makes generic parameters 'V' and 'T' equivalent}}
// expected-warning@-3{{same-type requirement makes generic parameters 'W' and 'U' equivalent}}
// expected-warning@-3{{redundant same-type constraint 'X1<T, U>' == 'X1<V, W>'}}

protocol P2 {
  associatedtype Assoc1
  associatedtype Assoc2
}

// CHECK-LABEL: same_types.(file).structuralSameTypeRecursive1@
// CHECK-NEXT: Generic signature: <T, U>

// expected-error@+2 {{cannot build rewrite system for generic signature; concrete nesting limit exceeded}}
// expected-note@+1 {{failed rewrite rule is τ_0_0.[P2:Assoc1].[concrete: ((((((((((((((((((((((((((((((τ_0_0.[P2:Assoc1], τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1), τ_0_1)] => τ_0_0.[P2:Assoc1]}}
func structuralSameTypeRecursive1<T: P2, U>(_: T, _: U)
  where T.Assoc1 == Tuple2<T.Assoc1, U>
  // expected-error@-1 {{'Assoc1' is not a member type of type 'T'}}
  // expected-error@-2 {{'Assoc1' is not a member type of type 'T'}}
{ }

protocol P3 {
}

protocol P4 {
  associatedtype A
}

// CHECK-LABEL: same_types.(file).test9@
// CHECK-NEXT: Generic signature: <T where T : P4>
func test9<T>(_: T) where T.A == X, T: P4, T.A: P3 { } // expected-error{{no type for 'T.A' can satisfy both 'T.A == X' and 'T.A : P3'}}

// Same-type constraint conflict through protocol where clauses.
protocol P5 where Foo1 == Foo2 {
  associatedtype Foo1
  associatedtype Foo2
}

protocol P6 {
  associatedtype Bar: P5
}

struct X5a {}

struct X5b { }

// CHECK-LABEL: same_types.(file).test9(_:u:)@
// CHECK-NEXT: Generic signature: <T, U where T : P6, U : P6, T.[P6]Bar == U.[P6]Bar>
func test9<T: P6, U: P6>(_ t: T, u: U) // expected-error{{no type for 'T.Bar.Foo1' can satisfy both 'T.Bar.Foo1 == X5a' and 'T.Bar.Foo1 == X5b'}}
  where T.Bar.Foo1 == X5a,
        U.Bar.Foo2 == X5b,
        T.Bar == U.Bar {
}

// FIXME: Remove -verify-ignore-unknown.
// <unknown>:0: error: unexpected error produced: generic parameter τ_0_0.Bar.Foo cannot be equal to both 'Y.Foo' (aka 'X') and 'Z'

// CHECK-LABEL: same_types.(file).testMetatypeSameType@
// CHECK-NEXT: Generic signature: <T, U where T == U>
func testMetatypeSameType<T, U>(_ t: T, _ u: U)
  where T.Type == U.Type { }
// expected-warning@-2{{same-type requirement makes generic parameters 'U' and 'T' equivalent}}
// expected-warning@-2{{redundant same-type constraint 'T.Type' == 'U.Type'}}

// CHECK-LABEL: same_types.(file).testSameTypeCommutativity1@
// CHECK-NEXT: Generic signature: <U, T where U == T.Type>
func testSameTypeCommutativity1<U, T>(_ t: T, _ u: U)
  where T.Type == U { } // Equivalent to U == T.Type
// expected-warning@-2{{same-type requirement makes generic parameter 'U' non-generic}}

// CHECK-LABEL: same_types.(file).testSameTypeCommutativity2@
// CHECK-NEXT: Generic signature: <U, T where T : P1, T.[P1]Assoc == U?>
func testSameTypeCommutativity2<U, T: P1>(_ t: T, _ u: U)
  where U? == T.Assoc { } // Ok, equivalent to T.Assoc == U?

// CHECK-LABEL: same_types.(file).testSameTypeCommutativity3@
// CHECK-NEXT: Generic signature: <U, T where T : P1, T.[P1]Assoc == (U) -> ()>
func testSameTypeCommutativity3<U, T: P1>(_ t: T, _ u: U)
  where (U) -> () == T.Assoc { } // Ok, equivalent to T.Assoc == (U) -> ()

// CHECK-LABEL: same_types.(file).testSameTypeCommutativity4@
// CHECK-NEXT: Generic signature: <U, T where T == (U) -> ()>
func testSameTypeCommutativity4<U, T>(_ t: T, _ u: U)
  where (U) -> () == T { } // Equivalent to T == (U) -> ()
// expected-warning@-2{{same-type requirement makes generic parameter 'T' non-generic}}

// CHECK-LABEL: same_types.(file).testSameTypeCommutativity5@
// CHECK-NEXT: Generic signature: <U, T where T : P1, T.[P1]Assoc == any P3 & PPP>
func testSameTypeCommutativity5<U, T: P1>(_ t: T, _ u: U)
  where PPP & P3 == T.Assoc { } // Ok, equivalent to T.Assoc == PPP & P3

// CHECK-LABEL: same_types.(file).testSameTypeCommutativity6@
// CHECK-NEXT: Generic signature: <U, T where T : P1, T.[P1]Assoc == <<error type>>>
func testSameTypeCommutativity6<U, T: P1>(_ t: T, _ u: U)
  where U & P3 == T.Assoc { } // Equivalent to T.Assoc == U & P3
// expected-error@-1 {{non-protocol, non-class type 'U' cannot be used within a protocol-constrained type}}

// rdar://problem/46848889

// CHECK-LABEL: same_types.(file).Foo@
// CHECK-NEXT: Generic signature: <A, B, C where A : P1, B : P1, C : P1, A.[P1]Assoc == B.[P1]Assoc, B.[P1]Assoc == C.[P1]Assoc>
struct Foo<A: P1, B: P1, C: P1> where A.Assoc == B.Assoc, A.Assoc == C.Assoc {}

// CHECK-LABEL: same_types.(file).Bar@
// CHECK-NEXT: Generic signature: <A, B where A : P1, B : P1, A.[P1]Assoc == B.[P1]Assoc>
struct Bar<A: P1, B: P1> where A.Assoc == B.Assoc {
  // CHECK-LABEL: same_types.(file).Bar.f(with:)@
  // CHECK-NEXT: Generic signature: <A, B, C where A : P1, B : P1, C : P1, A.[P1]Assoc == B.[P1]Assoc, B.[P1]Assoc == C.[P1]Assoc>
  func f<C: P1>(with other: C) -> Foo<A, B, C> where A.Assoc == C.Assoc {
    fatalError()
  }
}

protocol P7 {
  associatedtype A
  static func fn(args: A)
}

class R<T>: P7 where T: P7, T.A == T.Type { // expected-note {{'T' declared as parameter to type 'R'}}
  typealias A = T.Type
  static func fn(args: T.Type) {}
}

R.fn(args: R.self) // expected-error {{generic parameter 'T' could not be inferred}}
// expected-note@-1 {{explicitly specify the generic arguments to fix this issue}}

// rdar://problem/58607155
protocol AssocType1 { associatedtype A }
protocol AssocType2 { associatedtype A }

func rdar58607155() {
  func f<T1: AssocType1, T2: AssocType2>(t1: T1, t2: T2) where T1.A == T2.A {}
  // expected-note@-1 2 {{where 'T2' = 'MissingConformance'}}
  // expected-note@-2 2 {{where 'T1' = 'MissingConformance'}}

  class Conformance: AssocType1, AssocType2 { typealias A = Int }
  class MissingConformance {}

  // One generic argument has a conformance failure
  f(t1: MissingConformance(), t2: Conformance()) // expected-error {{local function 'f(t1:t2:)' requires that 'MissingConformance' conform to 'AssocType1'}}
  f(t1: Conformance(), t2: MissingConformance()) // expected-error {{local function 'f(t1:t2:)' requires that 'MissingConformance' conform to 'AssocType2'}}

  // Both generic arguments have a conformance failure
  f(t1: MissingConformance(), t2: MissingConformance())
  // expected-error@-1 {{local function 'f(t1:t2:)' requires that 'MissingConformance' conform to 'AssocType1'}}
  // expected-error@-2 {{local function 'f(t1:t2:)' requires that 'MissingConformance' conform to 'AssocType2'}}
}
