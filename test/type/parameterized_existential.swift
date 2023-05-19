// RUN: %target-typecheck-verify-swift -disable-availability-checking

protocol Sequence<Element> { // expected-note {{'Sequence' declared here}}
  associatedtype Element
}

// 'any' is required here

func takesSequenceOfInt1(_: Sequence<Int>) {}
// expected-error@-1 {{use of protocol 'Sequence<Int>' as a type must be written 'any Sequence<Int>'}}

func returnsSequenceOfInt1() -> Sequence<Int> {}
// expected-error@-1 {{use of protocol 'Sequence<Int>' as a type must be written 'any Sequence<Int>'}}

struct ConcreteSequence<Element> : Sequence {}

extension Sequence {
  func map<Other>(_ transform: (Self.Element) -> Other) -> ConcreteSequence<Other> {
    return ConcreteSequence<Other>()
  }
}

protocol DoubleWide<X, Y> {
  associatedtype X
  associatedtype Y

  var x: X { get }
  var y: Y { get }
}

extension Int: DoubleWide {
  typealias X = Int
  typealias Y = Int

  var x: X { 0 }
  var y: X { 0 }
}

struct Collapse<T: DoubleWide>: DoubleWide {
  typealias X = T
  typealias Y = T

  var x: X
  var y: X { self.x }
}

func test() -> any DoubleWide<some DoubleWide<Int, Int>, some DoubleWide<Int, Int>> { return Collapse<Int>(x: 42) }
// expected-error@-1 {{'some' types cannot be used in constraints on existential types}}

func diagonalizeAny(_ x: any Sequence<Int>) -> any Sequence<(Int, Int)> {
  return x.map { ($0, $0) }
}

func erase<T>(_ x: ConcreteSequence<T>) -> any Sequence<T> {
  return x as any Sequence<T>
}

protocol Sponge<A, B> {
  associatedtype A
  associatedtype B
}

func saturation(_ dry: any Sponge, _ wet: any Sponge<Int, Int>) {
  _ = dry as any Sponge<Int, Int>
  // expected-error@-1 {{'any Sponge' is not convertible to 'any Sponge<Int, Int>'}}
  // expected-note@-2 {{did you mean to use 'as!' to force downcast?}}
  _ = dry as any Sponge

  _ = wet as any Sponge<Int, Int> // Ok
  _ = wet as any Sponge // Ok
  _ = wet as any Sponge<String, String> // expected-error {{'any Sponge<Int, Int>' is not convertible to 'any Sponge<String, String>'}}
  // expected-note@-1 {{did you mean to use 'as!' to force downcast?}}
}

func typeExpr() {
  _ = Sequence<Int>.self
  // expected-error@-1 {{use of protocol 'Sequence<Int>' as a type must be written 'any Sequence<Int>'}}

  _ = any Sequence<Int>.self
  // expected-error@-1 {{'self' is not a member type of protocol 'parameterized_existential.Sequence<Swift.Int>'}}

  _ = (any Sequence<Int>).self
}

/// Not supported as a protocol composition term for now

protocol SomeProto {}

func protocolCompositionNotSupported1(_: SomeProto & Sequence<Int>) {}
// expected-error@-1 {{non-protocol, non-class type 'Sequence<Int>' cannot be used within a protocol-constrained type}}

func protocolCompositionNotSupported2(_: any SomeProto & Sequence<Int>) {}
// expected-error@-1 {{non-protocol, non-class type 'Sequence<Int>' cannot be used within a protocol-constrained type}}

func increment(_ n : any Collection<Float>) {
  for value in n {
      _ = value + 1
  }
}

func genericIncrement<T: Numeric>(_ n : any Collection<T>) {
  for value in n {
    _ = value + 1
  }
}
