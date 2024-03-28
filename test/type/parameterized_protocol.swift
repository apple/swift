// RUN: %target-typecheck-verify-swift -disable-availability-checking

// RUN: not %target-swift-frontend -typecheck %s -debug-generic-signatures -disable-availability-checking 2>&1 | %FileCheck %s


/// Test some invalid syntax first

protocol Invalid1<> {}
// expected-error@-1 {{expected an identifier to name primary associated type}}

protocol Invalid2<A B> {}
// expected-error@-1 {{expected '>' to complete primary associated type list}}
// expected-note@-2 {{to match this opening '<'}}
// expected-error@-3 {{an associated type named 'A' must be declared in the protocol 'Invalid2' or a protocol it inherits}}

protocol Invalid3<Element, +> {}
// expected-error@-1 {{expected an identifier to name primary associated type}}
// expected-error@-2 {{expected '>' to complete primary associated type list}}
// expected-note@-3 {{to match this opening '<'}}
// expected-error@-4 {{an associated type named 'Element' must be declared in the protocol 'Invalid3' or a protocol it inherits}}

protocol Invalid4<Element> {}
// expected-error@-1 {{an associated type named 'Element' must be declared in the protocol 'Invalid4' or a protocol it inherits}}

protocol Invalid5<Element, Element> {
// expected-error@-1 {{duplicate primary associated type name 'Element'}}
  associatedtype Element
}

/// Test semantics

protocol Sequence<Element> {
  associatedtype Element
  // expected-note@-1 2{{protocol requires nested type 'Element'; add nested type 'Element' for conformance}}
}

extension Sequence {
  func map<Other>(_ transform: (Self.Element) -> Other) -> ConcreteSequence<Other> {
    return ConcreteSequence<Other>()
  }
}

struct ConcreteSequence<Element> : Sequence {}

protocol EquatableSequence<Element> {
  associatedtype Element : Equatable
}

struct ConcreteEquatableSequence<Element : Equatable> : EquatableSequence {}


/// Parameterized protocol in protocol inheritance clause

// CHECK-LABEL: parameterized_protocol.(file).IntSequence@
// CHECK: Requirement signature: <Self where Self : Sequence, Self.[Sequence]Element == Int>
protocol IntSequence : Sequence<Int> {}


/// Concrete types cannot inherit from a parameterized protocol

struct SillyStruct : Sequence<Int> {}
// expected-error@-1 {{cannot inherit from protocol type with generic argument 'Sequence<Int>'}}
// expected-error@-2 {{type 'SillyStruct' does not conform to protocol 'Sequence'}}


/// Parameterized protocol in generic parameter inheritance clause

// CHECK-LABEL: parameterized_protocol.(file).IntSequenceWrapper@
// CHECK: Generic signature: <S where S : Sequence, S.[Sequence]Element == Int>
struct IntSequenceWrapper<S : Sequence<Int>> {}

// CHECK-LABEL: parameterized_protocol.(file).SequenceWrapper@
// CHECK: Generic signature: <S, E where S : Sequence, E == S.[Sequence]Element>
struct SequenceWrapper<S : Sequence<E>, E> {}


/// Parameterized protocol in associated type inheritance clause

// CHECK-LABEL: parameterized_protocol.(file).IntSequenceWrapperProtocol@
// CHECK: Requirement signature: <Self where Self.[IntSequenceWrapperProtocol]S : Sequence, Self.[IntSequenceWrapperProtocol]S.[Sequence]Element == Int>
protocol IntSequenceWrapperProtocol {
  associatedtype S : Sequence<Int>
}

// CHECK-LABEL: parameterized_protocol.(file).SequenceWrapperProtocol@
// CHECK: Requirement signature: <Self where Self.[SequenceWrapperProtocol]E == Self.[SequenceWrapperProtocol]S.[Sequence]Element, Self.[SequenceWrapperProtocol]S : Sequence>
protocol SequenceWrapperProtocol {
  associatedtype S : Sequence<E>
  associatedtype E
}

// https://github.com/apple/swift/issues/58240
// the GenericSignatureBuilder doesn't like this protocol.
//
// CHECK-LABEL: .Recursive@
// CHECK-NEXT: Requirement signature: <Self where Self.[Recursive]B == Self.[Recursive]D.[Recursive]B, Self.[Recursive]C == Self.[Recursive]D.[Recursive]C, Self.[Recursive]D : Recursive>
protocol Recursive<B, C> {
  associatedtype B
  associatedtype C
  associatedtype D: Recursive<B, C> = Self
}

/// Parameterized protocol in where clause of concrete type

// CHECK-LABEL: parameterized_protocol.(file).IntSequenceWrapper2@
// CHECK: Generic signature: <S where S : Sequence, S.[Sequence]Element == Int>
struct IntSequenceWrapper2<S> where S : Sequence<Int> {}

// CHECK-LABEL: parameterized_protocol.(file).SequenceWrapper2@
// CHECK: Generic signature: <S, E where S : Sequence, E == S.[Sequence]Element>
struct SequenceWrapper2<S, E> where S : Sequence<E> {}


/// Parameterized protocol in where clause of associated type

// CHECK-LABEL: parameterized_protocol.(file).IntSequenceWrapperProtocol2@
// CHECK: Requirement signature: <Self where Self.[IntSequenceWrapperProtocol2]S : Sequence, Self.[IntSequenceWrapperProtocol2]S.[Sequence]Element == Int>
protocol IntSequenceWrapperProtocol2 {
  associatedtype S where S : Sequence<Int>
}

// CHECK-LABEL: parameterized_protocol.(file).SequenceWrapperProtocol2@
// CHECK: Requirement signature: <Self where Self.[SequenceWrapperProtocol2]E == Self.[SequenceWrapperProtocol2]S.[Sequence]Element, Self.[SequenceWrapperProtocol2]S : Sequence>
protocol SequenceWrapperProtocol2 {
  associatedtype S where S : Sequence<E>
  associatedtype E
}


/// Multiple primary associated types
protocol Collection<Element, Index> : Sequence {
  associatedtype Index
}

func testCollectionBad1<T : Collection<String>>(_: T) {}
// expected-error@-1 {{protocol type 'Collection' specialized with too few type arguments (got 1, but expected 2)}}

func testCollectionBad2<T : Collection<String, Int, Float>>(_: T) {}
// expected-error@-1 {{protocol type 'Collection' specialized with too many type arguments (got 3, but expected 2)}}

// CHECK-LABEL: .testCollectionGood@
// CHECK-NEXT: Generic signature: <T where T : Collection, T.[Sequence]Element == String, T.[Collection]Index == Int>
func testCollectionGood<T : Collection<String, Int>>(_: T) {}


/// Parameterized protocol in opaque result type

struct OpaqueTypes<E> {
  func returnSequenceOfInt() -> some Sequence<Int> {
    return ConcreteSequence<Int>()
  }

  func returnSequenceOfE() -> some Sequence<E> {
    return ConcreteSequence<E>()
  }

  // Invalid
  func returnSequenceOfIntBad() -> some Sequence<Int> {
    // expected-note@-1 {{opaque return type declared here}}
    return ConcreteSequence<E>()
    // expected-error@-1 {{return type of instance method 'returnSequenceOfIntBad()' requires the types 'E' and 'Int' be equivalent}}
  }

  // Invalid
  func returnEquatableSequenceBad() -> some Sequence<E> {
    return ConcreteEquatableSequence<E>()
    // expected-error@-1 {{type 'E' does not conform to protocol 'Equatable'}}
  }
}


/// Extensions of parameterized protocol type

// CHECK-LABEL: ExtensionDecl line={{[0-9]+}} base=Sequence<Int>
// CHECK: Generic signature: <Self where Self : Sequence, Self.[Sequence]Element == Int>
extension Sequence<Int> {

  // CHECK-LABEL: parameterized_protocol.(file).Sequence extension.doSomethingGeneric@
  // CHECK: Generic signature: <Self, E where Self : Sequence, Self.[Sequence]Element == Int>
  func doSomethingGeneric<E>(_: E) {}
}


/// Constraint aliases

typealias SequenceOfInt = Sequence<Int>
typealias SequenceOf<T> = Sequence<T>

// CHECK-LABEL: .testConstraintAlias1@
// CHECK-NEXT: Generic signature: <T where T : Sequence, T.[Sequence]Element == Int>
func testConstraintAlias1<T : SequenceOfInt>(_: T) {}

// CHECK-LABEL: .testConstraintAlias2@
// CHECK-NEXT: Generic signature: <T where T : Sequence, T.[Sequence]Element == String>
func testConstraintAlias2<T : SequenceOf<String>>(_: T) {}


/// Protocol compositions

// CHECK-LABEL: .testComposition1@
// CHECK-NEXT: Generic signature: <T where T : Sendable, T : Sequence, T.[Sequence]Element == Int>
func testComposition1<T : Sequence<Int> & Sendable>(_: T) {}

// CHECK-LABEL: .testComposition2@
// CHECK-NEXT: Generic signature:
// CHECK-NEXT: Canonical generic signature: <τ_0_0 where τ_0_0 : Sendable, τ_0_0 : Sequence, τ_0_0.[Sequence]Element == Int>
func testComposition2(_: some Sequence<Int> & Sendable) {}

// CHECK-LABEL: parameterized_protocol.(file).TestCompositionProtocol1@
// CHECK: Requirement signature: <Self where Self.[TestCompositionProtocol1]S : Sendable, Self.[TestCompositionProtocol1]S : Sequence, Self.[TestCompositionProtocol1]S.[Sequence]Element == Int>
protocol TestCompositionProtocol1 {
  associatedtype S : Sequence<Int> & Sendable
}

struct TestStructComposition : Sequence<Int> & Sendable {}
// expected-error@-1 {{cannot inherit from protocol type with generic argument 'Sequence<Int>'}}
// expected-error@-2 {{type 'TestStructComposition' does not conform to protocol 'Sequence'}}


/// Conflicts

protocol Pair<X, Y> where Self.X == Self.Y {
  associatedtype X
  associatedtype Y
}

func splay(_ x: some Pair<Int, String>) -> (Int, String) { fatalError() }
// expected-error@-1 {{no type for '(some Pair<Int, String>).X' can satisfy both '(some Pair<Int, String>).X == String' and '(some Pair<Int, String>).X == Int'}}
