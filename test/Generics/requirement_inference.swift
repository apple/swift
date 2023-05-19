// RUN: %target-typecheck-verify-swift -warn-redundant-requirements
// RUN: not %target-swift-frontend -typecheck %s -debug-generic-signatures 2>&1 | %FileCheck %s

protocol P1 { 
  func p1()
}

protocol P2 : P1 { }


struct X1<T : P1> { 
  func getT() -> T { }
}

class X2<T : P1> {
  func getT() -> T { }
}

class X3 { }

struct X4<T : X3> { 
  func getT() -> T { }
}

struct X5<T : P2> { }

// Infer protocol requirements from the parameter type of a generic function.
func inferFromParameterType<T>(_ x: X1<T>) {
  x.getT().p1()
}

// Infer protocol requirements from the return type of a generic function.
func inferFromReturnType<T>(_ x: T) -> X1<T> {
  _ = 0
  x.p1()
}

// Infer protocol requirements from the superclass of a generic parameter.
func inferFromSuperclass<T, U : X2<T>>(_ t: T, u: U) -> T {
  _ = 0
  t.p1()
}


// Infer protocol requirements from the parameter type of a constructor.
struct InferFromConstructor {
  init<T> (x : X1<T>) {
    x.getT().p1()
  }
}


// Don't infer requirements for outer generic parameters.
class Fox : P1 {
  func p1() {}
}

class Box<T : Fox, U> {
  func unpack(_ x: X1<T>) {}
  func unpackFail(_ X: X1<U>) { } // expected-error{{type 'U' does not conform to protocol 'P1'}}
}

// ----------------------------------------------------------------------------
// Superclass requirements
// ----------------------------------------------------------------------------

// Compute meet of two superclass requirements correctly.
class Carnivora {}
class Canidae : Carnivora {}

struct U<T : Carnivora> {}

struct V<T : Canidae> {}

// CHECK-LABEL: .inferSuperclassRequirement1@
// CHECK: Canonical generic signature: <τ_0_0 where τ_0_0 : Canidae>
func inferSuperclassRequirement1<T : Carnivora>( // expected-warning {{redundant superclass constraint 'T' : 'Carnivora'}}
	_ v: V<T>) {}

// CHECK-LABEL: .inferSuperclassRequirement2@
// CHECK: Canonical generic signature: <τ_0_0 where τ_0_0 : Canidae>
func inferSuperclassRequirement2<T : Canidae>(_ v: U<T>) {}

// ----------------------------------------------------------------------------
// Same-type requirements
// ----------------------------------------------------------------------------

protocol P3 {
  associatedtype P3Assoc : P2  // expected-note{{declared here}}
}

protocol P4 {
  associatedtype P4Assoc : P1
}

protocol PCommonAssoc1 {
  associatedtype CommonAssoc
}

protocol PCommonAssoc2 {
  associatedtype CommonAssoc
}

protocol PAssoc {
  associatedtype Assoc
}

struct Model_P3_P4_Eq<T : P3, U : P4> where T.P3Assoc == U.P4Assoc {}

func inferSameType1<T, U>(_ x: Model_P3_P4_Eq<T, U>) {
  let u: U.P4Assoc? = nil
  let _: T.P3Assoc? = u!
}

func inferSameType2<T : P3, U : P4>(_: T, _: U) where U.P4Assoc : P2, T.P3Assoc == U.P4Assoc {}
// expected-warning@-1{{redundant conformance constraint 'U.P4Assoc' : 'P2'}}

func inferSameType3<T : PCommonAssoc1>(_: T) where T.CommonAssoc : P1, T : PCommonAssoc2 {
}

protocol P5 {
  associatedtype Element
}

protocol P6 {
  associatedtype AssocP6 : P5
}

protocol P7 : P6 {
  associatedtype AssocP7: P6
}

// CHECK-LABEL: P7@
// CHECK: Canonical generic signature: <τ_0_0 where τ_0_0 : P7, τ_0_0.[P6]AssocP6.[P5]Element : P6, τ_0_0.[P6]AssocP6.[P5]Element == τ_0_0.[P7]AssocP7.[P6]AssocP6.[P5]Element>
extension P7 where AssocP6.Element : P6, // expected-warning{{redundant conformance constraint 'Self.AssocP6.Element' : 'P6'}}
        AssocP7.AssocP6.Element : P6,
        AssocP6.Element == AssocP7.AssocP6.Element {
  func nestedSameType1() { }
}

protocol P8 {
  associatedtype A
  associatedtype B
}

protocol P9 : P8 {
  associatedtype A
  associatedtype B
}

protocol P10 {
  associatedtype A
  associatedtype C
}

// CHECK-LABEL: sameTypeConcrete1@
// CHECK: Canonical generic signature: <τ_0_0 where τ_0_0 : P10, τ_0_0 : P9, τ_0_0.[P10]A == X3, τ_0_0.[P8]B == Int, τ_0_0.[P10]C == Int>
func sameTypeConcrete1<T : P9 & P10>(_: T) where T.A == X3, T.C == T.B, T.C == Int { }

// CHECK-LABEL: sameTypeConcrete2@
// CHECK: Canonical generic signature: <τ_0_0 where τ_0_0 : P10, τ_0_0 : P9, τ_0_0.[P8]B == X3, τ_0_0.[P10]C == X3>
func sameTypeConcrete2<T : P9 & P10>(_: T) where T.B : X3, T.C == T.B, T.C == X3 { }
// expected-warning@-1{{redundant superclass constraint 'T.B' : 'X3'}}

// Note: a standard-library-based stress test to make sure we don't inject
// any additional requirements.
// CHECK-LABEL: RangeReplaceableCollection
// CHECK: Canonical generic signature: <τ_0_0 where τ_0_0 : MutableCollection, τ_0_0 : RangeReplaceableCollection, τ_0_0.[Collection]SubSequence == Slice<τ_0_0>>
extension RangeReplaceableCollection
  where Self: MutableCollection, Self.SubSequence == Slice<Self>
{
	func f() { }
}

// FIXME(rqm-diagnostics): Bogus warning

// CHECK-LABEL: X14.recursiveConcreteSameType
// CHECK: Generic signature: <T, V where T == Range<Int>>
// CHECK-NEXT: Canonical generic signature: <τ_0_0, τ_1_0 where τ_0_0 == Range<Int>>
struct X14<T> where T.Iterator == IndexingIterator<T> {
	func recursiveConcreteSameType<V>(_: V) where T == Range<Int> { } // expected-warning {{redundant conformance constraint 'Range<Int>' : 'Collection'}}
  // expected-warning@-1 {{redundant conformance constraint 'Int' : 'Strideable'}}
  // expected-warning@-2 {{redundant conformance constraint 'Int' : 'SignedInteger'}}
  // expected-warning@-3 {{redundant same-type constraint 'Range<Int>.Iterator' (aka 'IndexingIterator<Range<Int>>') == 'IndexingIterator<T>'}}
  // expected-warning@-4 {{redundant same-type constraint 'T' == 'Range<Int>'}}
}

// rdar://problem/30478915
protocol P11 {
  associatedtype A
}

protocol P12 {
	associatedtype B: P11
}

struct X6 { }

struct X7 : P11 {
	typealias A = X6
}

struct X8 : P12 {
	typealias B = X7
}

struct X9<T: P12, U: P12> where T.B == U.B {
  // CHECK-LABEL: X9.upperSameTypeConstraint
	// CHECK: Generic signature: <T, U, V where T == X8, U : P12, U.[P12]B == X7>
  // CHECK: Canonical generic signature: <τ_0_0, τ_0_1, τ_1_0 where τ_0_0 == X8, τ_0_1 : P12, τ_0_1.[P12]B == X7>
	func upperSameTypeConstraint<V>(_: V) where T == X8 { } // expected-warning {{redundant conformance constraint 'X8' : 'P12'}}
}

protocol P13 {
	associatedtype C: P11
}

struct X10: P11, P12 {
	typealias A = X10
	typealias B = X10
}

struct X11<T: P12, U: P12> where T.B == U.B.A {
	// CHECK-LABEL: X11.upperSameTypeConstraint
	// CHECK: Generic signature: <T, U, V where T : P12, U == X10, T.[P12]B == X10>
	// CHECK: Canonical generic signature: <τ_0_0, τ_0_1, τ_1_0 where τ_0_0 : P12, τ_0_1 == X10, τ_0_0.[P12]B == X10>
	func upperSameTypeConstraint<V>(_: V) where U == X10 { } // expected-warning {{redundant conformance constraint 'X10' : 'P12'}}
}

protocol P16 {
	associatedtype A
}

struct X15 { }

struct X16<X, Y> : P16 {
	typealias A = (X, Y)
}

// CHECK-LABEL: .X17.bar@
// CHECK: Generic signature: <S, T, U, V where S == X16<X3, X15>, T == X3, U == X15>
struct X17<S: P16, T, U> where S.A == (T, U) {
	func bar<V>(_: V) where S == X16<X3, X15> { } // expected-warning {{redundant conformance constraint 'X16<X3, X15>' : 'P16'}}
  // expected-warning@-1 {{redundant same-type constraint 'X16<X3, X15>.A' (aka '(X3, X15)') == '(T, U)'}}
}

// Same-type constraints that are self-derived via a parent need to be
// suppressed in the resulting signature.
protocol P17 { }

protocol P18 {
  associatedtype A: P17
}

struct X18: P18, P17 {
  typealias A = X18
}

// FIXME(rqm-diagnostics): Bogus warning

// CHECK-LABEL: .X19.foo@
// CHECK: Generic signature: <T, U where T == X18>
struct X19<T: P18> where T == T.A {
  func foo<U>(_: U) where T == X18 { } // expected-warning {{redundant conformance constraint 'X18' : 'P18'}}
  // expected-warning@-1 {{redundant same-type constraint 'T' == 'X18'}}
}

// rdar://problem/31520386
protocol P20 { }

struct X20<T: P20> { }

// CHECK-LABEL: .X21.f@
// CHECK: Generic signature: <T, U, V where T : P20, U == X20<T>>
// CHECK: Canonical generic signature: <τ_0_0, τ_0_1, τ_1_0 where τ_0_0 : P20, τ_0_1 == X20<τ_0_0>>
struct X21<T, U> {
  func f<V>(_: V) where U == X20<T> { }
}

struct X22<T, U> {
  func g<V>(_: V) where T: P20,
                  U == X20<T> { }
}

// CHECK: Generic signature: <Self where Self : P22>
// CHECK: Canonical generic signature: <τ_0_0 where τ_0_0 : P22>
// CHECK: Protocol requirement signature:
// CHECK: .P22@
// CHECK-NEXT: Requirement signature: <Self where Self.[P22]A == X20<Self.[P22]B>, Self.[P22]B : P20>
// CHECK-NEXT: Canonical requirement signature: <τ_0_0 where τ_0_0.[P22]A == X20<τ_0_0.[P22]B>, τ_0_0.[P22]B : P20>
protocol P22 {
  associatedtype A
  associatedtype B: P20 where A == X20<B>
}

// CHECK: Generic signature: <Self where Self : P23>
// CHECK: Canonical generic signature: <τ_0_0 where τ_0_0 : P23>
// CHECK: Protocol requirement signature:
// CHECK: .P23@
// CHECK-NEXT: Requirement signature: <Self where Self.[P23]A == X20<Self.[P23]B>, Self.[P23]B : P20>
// CHECK-NEXT: Canonical requirement signature: <τ_0_0 where τ_0_0.[P23]A == X20<τ_0_0.[P23]B>, τ_0_0.[P23]B : P20>
protocol P23 {
  associatedtype A
  associatedtype B: P20
    where A == X20<B>
}

protocol P24 {
  associatedtype C: P20
}

struct X24<T: P20> : P24 {
  typealias C = T
}

protocol P25c {
  associatedtype A: P24
  associatedtype B where A == X<B> // expected-error{{cannot find type 'X' in scope}}
}

protocol P25d {
  associatedtype A
  associatedtype B where A == X24<B> // expected-error{{type 'Self.B' does not conform to protocol 'P20'}}
}

// ----------------------------------------------------------------------------
// Inference of associated type relationships within a protocol hierarchy
// ----------------------------------------------------------------------------

struct X28 : P2 {
  func p1() { }
}

// CHECK-LABEL: .P28@
// CHECK-NEXT: Requirement signature: <Self where Self : P3, Self.[P3]P3Assoc == X28>
// CHECK-NEXT: Canonical requirement signature: <τ_0_0 where τ_0_0 : P3, τ_0_0.[P3]P3Assoc == X28>
protocol P28: P3 {
  typealias P3Assoc = X28   // expected-warning{{typealias overriding associated type}}
}

// ----------------------------------------------------------------------------
// Inference of associated types by name match
// ----------------------------------------------------------------------------
protocol P29 {
  associatedtype X
}

protocol P30 {
  associatedtype X
}

protocol P31 { }

// CHECK-LABEL: .sameTypeNameMatch1@
// CHECK: Generic signature: <T where T : P29, T : P30, T.[P29]X : P31>
func sameTypeNameMatch1<T: P29 & P30>(_: T) where T.X: P31 { }

// ----------------------------------------------------------------------------
// Infer requirements from conditional conformances
// ----------------------------------------------------------------------------

protocol P32 {}
protocol P33 {
  associatedtype A: P32
}
protocol P34 {}
struct Foo<T> {}
extension Foo: P32 where T: P34 {}

// Inference chain: U.A: P32 => Foo<V>: P32 => V: P34

// CHECK-LABEL: conditionalConformance1@
// CHECK: Generic signature: <U, V where U : P33, V : P34, U.[P33]A == Foo<V>>
// CHECK: Canonical generic signature: <τ_0_0, τ_0_1 where τ_0_0 : P33, τ_0_1 : P34, τ_0_0.[P33]A == Foo<τ_0_1>>
func conditionalConformance1<U: P33, V>(_: U) where U.A == Foo<V> {}

struct Bar<U: P32> {}
// CHECK-LABEL: conditionalConformance2@
// CHECK: Generic signature: <V where V : P34>
// CHECK: Canonical generic signature: <τ_0_0 where τ_0_0 : P34>
func conditionalConformance2<V>(_: Bar<Foo<V>>) {}

// https://github.com/apple/swift/issues/49399
// Mentioning a nested type that is conditional should infer that requirement.

protocol P35 {}
protocol P36 {
    func foo()
}

struct ConditionalNested<T> {}

extension ConditionalNested where T: P35 {
    struct Inner {}
}

// CHECK: Generic signature: <T where T : P35, T : P36>
// CHECK: Canonical generic signature: <τ_0_0 where τ_0_0 : P35, τ_0_0 : P36>
extension ConditionalNested.Inner: P36 where T: P36 {
    func foo() {}

    struct Inner2 {}
}

// CHECK-LABEL: conditionalNested1@
// CHECK: Generic signature: <U where U : P35>
// CHECK: Canonical generic signature: <τ_0_0 where τ_0_0 : P35>
func conditionalNested1<U>(_: [ConditionalNested<U>.Inner?]) {}

// CHECK-LABEL: conditionalNested2@
// CHECK: Generic signature: <U where U : P35, U : P36>
// CHECK: Canonical generic signature: <τ_0_0 where τ_0_0 : P35, τ_0_0 : P36>
func conditionalNested2<U>(_: [ConditionalNested<U>.Inner.Inner2?]) {}

//
// Generate typalias adds requirements that can be inferred
//
typealias X1WithP2<T: P2> = X1<T>

// Inferred requirement T: P2 from the typealias
func testX1WithP2<T>(_: X1WithP2<T>) {
  _ = X5<T>() // requires P2
}

// Overload based on the inferred requirement.
func testX1WithP2Overloading<T>(_: X1<T>) {
  _ = X5<T>() // expected-error{{type 'T' does not conform to protocol 'P2'}}
}

func testX1WithP2Overloading<T>(_: X1WithP2<T>) {
  _ = X5<T>() // requires P2
}

// Extend using the inferred requirement.
extension X1WithP2 {
  func f() {
    _ = X5<T>() // okay: inferred T: P2 from generic typealias
  }
}

extension X1: P1 {
  func p1() { }
}

typealias X1WithP2Changed<T: P2> = X1<X1<T>>
typealias X1WithP2MoreArgs<T: P2, U> = X1<T>

extension X1WithP2Changed {
  func bad1() {
    _ = X5<T>() // expected-error{{type 'T' does not conform to protocol 'P2'}}
  }
}

extension X1WithP2MoreArgs {
  func bad2() {
    _ = X5<T>() // expected-error{{type 'T' does not conform to protocol 'P2'}}
  }
}

// Inference from protocol inheritance clauses is not allowed.
typealias ExistentialP4WithP2Assoc<T: P4> = P4 where T.P4Assoc : P2

protocol P37 : ExistentialP4WithP2Assoc<Self> { }
// expected-error@-1 {{type 'Self.P4Assoc' does not conform to protocol 'P2'}}

extension P37 {
  func f() {
    _ = X5<P4Assoc>()
    // expected-error@-1 {{type 'Self.P4Assoc' does not conform to protocol 'P2'}}
  }
}
