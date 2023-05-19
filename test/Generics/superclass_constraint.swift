// RUN: %target-typecheck-verify-swift -warn-redundant-requirements
// RUN: not %target-swift-frontend -typecheck %s -debug-generic-signatures > %t.dump 2>&1
// RUN: %FileCheck %s < %t.dump

class A {
  func foo() { }
}

class B : A {
  func bar() { }
}

class Other { }

func f1<T : A>(_: T) where T : Other {} // expected-error{{no type for 'T' can satisfy both 'T : Other' and 'T : A'}}

func f2<T : A>(_: T) where T : B {}
// expected-warning@-1{{redundant superclass constraint 'T' : 'A'}}

class GA<T> {}
class GB<T> : GA<T> {}

protocol P {}

func f3<T, U>(_: T, _: U) where U : GA<T> {}
func f4<T, U>(_: T, _: U) where U : GA<T> {}
func f5<T, U : GA<T>>(_: T, _: U) {}
func f6<U : GA<T>, T : P>(_: T, _: U) {}
func f7<U, T>(_: T, _: U) where U : GA<T>, T : P {}

func f8<T : GA<A>>(_: T) where T : GA<B> {} // expected-error{{no type for 'T' can satisfy both 'T : GA<B>' and 'T : GA<A>'}}

func f9<T : GA<A>>(_: T) where T : GB<A> {}
// expected-warning@-1{{redundant superclass constraint 'T' : 'GA<A>'}}

func f10<T : GB<A>>(_: T) where T : GA<A> {}
// expected-warning@-1{{redundant superclass constraint 'T' : 'GA<A>'}}

func f11<T : GA<T>>(_: T) { }
func f12<T : GA<U>, U : GB<T>>(_: T, _: U) { }
func f13<T : U, U : GA<T>>(_: T, _: U) { } // expected-error{{type 'T' constrained to non-protocol, non-class type 'U'}}

// rdar://problem/24730536
// Superclass constraints can be used to resolve nested types to concrete types.

protocol P3 {
  associatedtype T
}

protocol P2 {
  associatedtype T : P3
}

class C : P3 {
  typealias T = Int
}

class S : P2 {
  typealias T = C
}

// CHECK-LABEL: .superclassConformance1(t:)@
// CHECK-NEXT: Generic signature: <T where T : C>
func superclassConformance1<T>(t: T)
  where T : C,
        T : P3 {} // expected-warning{{redundant conformance constraint 'C' : 'P3'}}



// CHECK-LABEL: .superclassConformance2(t:)@
// CHECK-NEXT: Generic signature: <T where T : C>
func superclassConformance2<T>(t: T)
  where T : C,
   T : P3 {} // expected-warning{{redundant conformance constraint 'C' : 'P3'}}

protocol P4 { }

class C2 : C, P4 { }

// CHECK-LABEL: .superclassConformance3(t:)@
// CHECK-NEXT: Generic signature: <T where T : C2>
func superclassConformance3<T>(t: T) where T : C, T : P4, T : C2 {}
// expected-warning@-1{{redundant superclass constraint 'T' : 'C'}}
// expected-warning@-2{{redundant conformance constraint 'T' : 'P4'}}

protocol P5: A { }

protocol P6: A, Other { } // expected-error {{no type for 'Self' can satisfy both 'Self : Other' and 'Self : A'}}
// expected-error@-1{{multiple inheritance from classes 'A' and 'Other'}}

func takeA(_: A) { }
func takeP5<T: P5>(_ t: T) {
	takeA(t) // okay
}

protocol P7 {
// expected-error@-1{{no type for 'Self.Assoc' can satisfy both 'Self.Assoc : Other' and 'Self.Assoc : A'}}
	associatedtype Assoc: A, Other
}

// CHECK-LABEL: .superclassConformance4@
// CHECK-NEXT: Generic signature: <T, U where T : P3, U : P3, T.[P3]T : C, T.[P3]T == U.[P3]T>
func superclassConformance4<T: P3, U: P3>(_: T, _: U)
  where T.T: C, // expected-warning{{redundant superclass constraint 'T.T' : 'C'}}
        U.T: C,
        T.T == U.T { }

// Lookup of superclass associated types from inheritance clause

protocol Elementary {
  associatedtype Element

  func get() -> Element
}

class Classical : Elementary {
  func get() -> Int {
    return 0
  }
}

// CHECK-LABEL: .genericFunc@
// CHECK-NEXT: Generic signature: <T, U where T : Elementary, U : Classical, T.[Elementary]Element == Int>
func genericFunc<T : Elementary, U : Classical>(_: T, _: U) where T.Element == U.Element {}

// Lookup within superclass constraints.
protocol P8 {
  associatedtype B
}

class C8 {
  struct A { }
}

// CHECK-LABEL: .superclassLookup1@
// CHECK-NEXT: Generic signature: <T where T : C8, T : P8, T.[P8]B == C8.A>
func superclassLookup1<T: C8 & P8>(_: T) where T.A == T.B { }

// CHECK-LABEL: .superclassLookup2@
// CHECK-NEXT: Generic signature: <T where T : C8, T : P8, T.[P8]B == C8.A>
func superclassLookup2<T: P8>(_: T) where T.A == T.B, T: C8 { }

// CHECK-LABEL: .superclassLookup3@
// CHECK-NEXT: Generic signature: <T where T : C8, T : P8, T.[P8]B == C8.A>
func superclassLookup3<T>(_: T) where T.A == T.B, T: C8, T: P8 { }

// https://github.com/apple/swift/issues/47741

class C9 {}

protocol P9 {}

class C10 : C9, P9 { }

protocol P10 {
  associatedtype A: C9
}

// CHECK-LABEL: .testP10@
// CHECK-NEXT: Generic signature: <T where T : P10, T.[P10]A : C10>
func testP10<T>(_: T) where T: P10, T.A: C10 { }

// Nested types of generic class-constrained type parameters.
protocol Tail {
  associatedtype E
}

protocol Rump : Tail {
  associatedtype E = Self
}

class Horse<T>: Rump { }

func hasRedundantConformanceConstraint<X : Horse<T>, T>(_: X) where X : Rump {}
// expected-warning@-1 {{redundant conformance constraint 'Horse<T>' : 'Rump'}}

// https://github.com/apple/swift/issues/48432

protocol X {
	associatedtype Y : A
}

// CHECK-LABEL: .noRedundancyWarning@
// CHECK: Generic signature: <C where C : X, C.[X]Y == B>
func noRedundancyWarning<C : X>(_ wrapper: C) where C.Y == B {}

// [qualified lookup] https://github.com/apple/swift/issues/44798

protocol Init {
  init(x: ())
}

class Base {
  required init(y: ()) {}
}

class Derived : Base {}

func g<T : Init & Derived>(_: T.Type) {
  _ = T(x: ())
  _ = T(y: ())
}

// Binding a class-constrained generic parameter to a subclass existential is
// not sound.
struct G<T : Base> {}
// expected-note@-1 2 {{requirement specified as 'T' : 'Base' [with T = any Base & P]}}

_ = G<Base & P>() // expected-error {{'G' requires that 'any Base & P' inherit from 'Base'}}

func badClassConstrainedType(_: G<Base & P>) {}
// expected-error@-1 {{'G' requires that 'any Base & P' inherit from 'Base'}}

// Reduced from CoreStore in source compat suite
public protocol Pony {}

public class Teddy: Pony {}

public struct Paddock<P: Pony> {}

public struct Barn<T: Teddy> {
  // CHECK-LABEL: Barn.foo@
  // CHECK: Generic signature: <T, S where T : Teddy>
  public func foo<S>(_: S, _: Barn<T>, _: Paddock<T>) {}
}


public class Animal { }

@available(*, unavailable, message: "Not a pony")
extension Animal: Pony { }

public struct AnimalWrapper<Friend: Animal> { }

// FIXME: Generic signature: <Friend where Friend : Animal, Friend : Pony>
// Generic signature: <Friend where Friend : Animal>
extension AnimalWrapper: Pony where Friend: Pony { }
// expected-warning@-1{{redundant conformance constraint 'Animal' : 'Pony'}}
