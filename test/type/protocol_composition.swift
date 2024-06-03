// RUN: %target-typecheck-verify-swift -swift-version 4

func canonical_empty_protocol() -> Any {
  return 1
}

protocol P1 {
  func p1()
  func f(_: Int) -> Int
}

protocol P2 : P1 {
  func p2()
}

protocol P3 {
  func p3()
}

protocol P4 : P3 {
  func p4()
  func f(_: Double) -> Double
}

typealias Any1 = protocol<> // expected-error {{'protocol<>' syntax has been removed; use 'Any' instead}}
typealias Any2 = protocol< > // expected-error {{'protocol<>' syntax has been removed; use 'Any' instead}}

// Okay to inherit a typealias for Any type.
protocol P5 : Any { }
protocol P6 : protocol<> { } // expected-error {{'protocol<>' syntax has been removed; use 'Any' instead}}
typealias P7 = Any & Any1

extension Int : P5 { }

typealias Bogus = P1 & Int // expected-error{{non-protocol, non-class type 'Int' cannot be used within a protocol-constrained type}}

func testEquality() {
  // Remove duplicates from protocol-conformance types.
  let x1 : (_ : P2 & P4) -> ()
  let x2 : (_ : P3 & P4 & P2 & P1) -> ()
  x1 = x2
  _ = x1

  // Singleton protocol-conformance types, after duplication, are the same as
  // simply naming the protocol type.
  let x3 : (_ : P2 & P1) -> ()
  let x4 : (_ : P2) -> ()
  x3 = x4
  _ = x3

  // Empty protocol-conformance types are empty.
  let x5 : (_ : Any) -> ()
  let x6 : (_ : Any2) -> ()
  x5 = x6
  _ = x5

  let x7 : (_ : P1 & P3) -> ()
  let x8 : (_ : P2) -> ()
  x7 = x8 // expected-error{{cannot assign value of type '(any P2) -> ()' to type '(any P1 & P3) -> ()'}}
  _ = x7
}

// Name lookup into protocol-conformance types
func testLookup() {
  let x1 : P2 & P1 & P4
  x1.p1()
  x1.p2()
  x1.p3()
  x1.p4()
  var _ : Int = x1.f(1)
  var _ : Double = x1.f(1.0)
}

protocol REPLPrintable {
  func replPrint()
}

protocol SuperREPLPrintable : REPLPrintable {
  func superReplPrint()
}

protocol FooProtocol {
  func format(_ kind: Unicode.Scalar, layout: String) -> String
}

struct SuperPrint : REPLPrintable, FooProtocol, SuperREPLPrintable {
  func replPrint() {}
  func superReplPrint() {}
  func format(_ kind: Unicode.Scalar, layout: String) -> String {}
}

struct Struct1 {}
extension Struct1 : REPLPrintable, FooProtocol {
  func replPrint() {}
  func format(_ kind: Unicode.Scalar, layout: String) -> String {}
}

func accept_manyPrintable(_: REPLPrintable & FooProtocol) {}

func return_superPrintable() -> FooProtocol & SuperREPLPrintable {}

func testConversion() {
  // Conversions for literals.
  var x : REPLPrintable & FooProtocol = Struct1()
  accept_manyPrintable(Struct1())

  // Conversions for nominal types that conform to a number of protocols.
  let sp : SuperPrint
  x = sp
  accept_manyPrintable(sp)

  // Conversions among existential types.
  var x2 : protocol<SuperREPLPrintable, FooProtocol> // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{12-53=SuperREPLPrintable & FooProtocol}}
  x2 = x // expected-error{{value of type 'any FooProtocol & REPLPrintable' does not conform to 'SuperREPLPrintable' in assignment}}
  x = x2

  // Subtyping
  var _ : () -> FooProtocol & SuperREPLPrintable = return_superPrintable

  // FIXME: closures make ABI conversions explicit. rdar://problem/19517003
  var _ : () -> protocol<FooProtocol, REPLPrintable> = { return_superPrintable() } // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{17-53=FooProtocol & REPLPrintable}}
}

// Test the parser's splitting of >= into > and =.
var x : protocol<P5>= 17 // expected-error {{'protocol<...>' composition syntax has been removed and is not needed here}} {{9-22=P5=}} expected-error {{'=' must have consistent whitespace on both sides}}
var y : protocol<P5, P7>= 17 // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{9-26=P5 & P7=}} expected-error {{'=' must have consistent whitespace on both sides}}
var z : protocol<P5, P7>?=17 // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{9-27=(P5 & P7)?=}}

typealias A1 = protocol<> // expected-error {{'protocol<>' syntax has been removed; use 'Any' instead}} {{16-26=Any}}
typealias A2 = protocol<>? // expected-error {{'protocol<>' syntax has been removed; use 'Any' instead}} {{16-27=Any?}}
typealias B1 = protocol<P1,P2> // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{16-31=P1 & P2}}
typealias B2 = protocol<P1, P2> // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{16-32=P1 & P2}}
typealias B3 = protocol<P1 ,P2> // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{16-32=P1 & P2}}
typealias B4 = protocol<P1 , P2> // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{16-33=P1 & P2}}
typealias C1 = protocol<Any, P1> // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{16-33=Any & P1}}
typealias C2 = protocol<P1, Any> // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{16-33=P1 & Any}}
typealias D = protocol<P1> // expected-error {{'protocol<...>' composition syntax has been removed and is not needed here}} {{15-27=P1}}
typealias E = protocol<Any> // expected-error {{'protocol<...>' composition syntax has been removed and is not needed here}} {{15-28=Any}}
typealias F = protocol<Any, Any> // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{15-33=Any & Any}}
typealias G = protocol<P1>.Type // expected-error {{'protocol<...>' composition syntax has been removed and is not needed here}} {{15-27=P1}}
typealias H = protocol<P1>! // expected-error {{'protocol<...>' composition syntax has been removed and is not needed here}} {{15-28=P1!}}
// expected-warning@-1 {{using '!' is not allowed here; treating this as '?' instead}}
typealias J = protocol<P1, P2>.Protocol // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{15-31=(P1 & P2)}}
typealias K = protocol<P1, P2>? // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{15-32=(P1 & P2)?}}
typealias L = protocol<(P1), P2> // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{15-33=(P1) & P2}}

// Deprecated protocol composition syntax in expression context.
do {
  func typesAreEqual<T>(_: T.Type, _: T.Type) {}

  typesAreEqual(Optional<P1 & P2>.self,
                Optional<protocol<P1, P2>>.self)
  // expected-error@-1 {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{26-43=P1 & P2>}}

  // Test that we parse non-identifier components.
  typesAreEqual(Optional<P1 & P2>.self,
                Optional<protocol<P1, (P2)>>.self)
  // expected-error@-1 {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{26-45=P1 & (P2)>}}
}

typealias T01 = P1.Protocol & P2 // expected-error {{non-protocol, non-class type '(any P1).Type' cannot be used within a protocol-constrained type}}
typealias T02 = P1.Type & P2 // expected-error {{non-protocol, non-class type 'any P1.Type' cannot be used within a protocol-constrained type}}
typealias T03 = P1? & P2 // expected-error {{non-protocol, non-class type '(any P1)?' cannot be used within a protocol-constrained type}}
typealias T04 = P1 & P2! // expected-error {{non-protocol, non-class type '(any P2)?' cannot be used within a protocol-constrained type}}
// expected-warning@-1 {{using '!' is not allowed here; treating this as '?' instead}}
typealias T05 = P1 & P2 -> P3 // expected-error {{single argument function types require parentheses}} {{17-17=(}} {{24-24=)}}
typealias T06 = P1 -> P2 & P3 // expected-error {{single argument function types require parentheses}} {{17-17=(}} {{19-19=)}}
typealias T07 = P1 & protocol<P2, P3> // expected-error {{protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{22-38=P2 & P3}}
func fT07(x: T07) -> P1 & P2 & P3 { return x } // OK, 'P1 & protocol<P2, P3>' is parsed as 'P1 & P2 & P3'.
let _: P1 & P2 & P3 -> P1 & P2 & P3 = fT07 // expected-error {{single argument function types require parentheses}} {{8-8=(}} {{20-20=)}}

struct S01: P5 & P6 {}
struct S02: P5? & P6 {} // expected-error {{non-protocol, non-class type '(any P5)?' cannot be used within a protocol-constrained type}}
struct S03: Optional<P5> & P6 {} // expected-error {{non-protocol, non-class type 'Optional<any P5>' cannot be used within a protocol-constrained type}}
struct S04<T : P5 & (P6)> {}
struct S05<T> where T : P5? & P6 {} // expected-error {{non-protocol, non-class type '(any P5)?' cannot be used within a protocol-constrained type}}

// https://github.com/apple/swift/issues/45712
// Protocol Composition Often Migrated Incorrectly
struct S_45712<T: protocol<P1, P3>> {} // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{19-36=P1 & P3>}}
func f1_45712<U where U: protocol<P1, P3>>(_: U)  {} // expected-error {{'protocol<...>' composition syntax has been removed; join the type constraints using '&'}} {{26-43=P1 & P3>}} // expected-error {{'where' clause}}
func f2_45712<U : protocol<P1>>(_: U)  {} // expected-error {{'protocol<...>' composition syntax has been removed and is not needed here}} {{19-32=P1>}}

// Make sure we correctly form compositions in expression context
func takesP1AndP2(_: [AnyObject & P1 & P2]) {}

takesP1AndP2([AnyObject & P1 & P2]())
takesP1AndP2([Swift.AnyObject & P1 & P2]())
takesP1AndP2([AnyObject & protocol_composition.P1 & P2]())
takesP1AndP2([AnyObject & P1 & protocol_composition.P2]())
takesP1AndP2([DoesNotExist & P1 & P2]()) // expected-error {{cannot find 'DoesNotExist' in scope}}
// expected-error@-1 {{cannot call value of non-function type '[UInt8]'}}
takesP1AndP2([Swift.DoesNotExist & P1 & P2]()) // expected-error {{module 'Swift' has no member named 'DoesNotExist'}}
// expected-error@-1 {{cannot call value of non-function type '[UInt8]'}}

typealias T08 = P1 & inout P2 // expected-error {{'inout' may only be used on parameters}}
typealias T09 = P1 & __shared P2 // expected-error {{'__shared' may only be used on parameters}}
typealias T10 = P1 & __owned P2 // expected-error {{'__owned' may only be used on parameters}}
