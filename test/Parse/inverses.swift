// RUN: %target-typecheck-verify-swift -enable-experimental-feature SuppressedAssociatedTypes

protocol U {}

enum Maybe<Thing: ~Copyable> : ~Copyable {}

func more() {
  let _: any ~Copyable = 19

  let _: any ~Equatable = 19  // expected-error@:14 {{type 'Equatable' cannot be suppressed}}

  let _: any (~Copyable & ~Equatable) // expected-error{{type 'Equatable' cannot be suppressed}}

  let _: ~Any // expected-error {{type 'Any' cannot be suppressed}}
  let _: ~AnyObject // expected-error {{type 'AnyObject' cannot be suppressed}}
}

struct S4: ~(Copyable & Equatable) {} // expected-error {{conformance to 'Equatable' cannot be suppressed}}

func blah<T>(_ t: borrowing T) where T: ~Copyable,
                                     T: ~Hashable {}  // expected-error@:41 {{type 'Hashable' cannot be suppressed}}

func foo<T: ~Copyable>(x: borrowing T) {}

struct Buurap<T: ~Copyable> where T: ~Copyable {}

struct ExtraNoncopyStruct: ~Copyable, ~Copyable {}
struct ExtraNoncopyEnum: ~Copyable, ~Copyable {}

protocol ExtraNoncopyProto: ~Copyable, ~Copyable {}

protocol Foo: ~Copyable
         where Self: ~Copyable {

    associatedtype Touch : ~Copyable,
                           ~Copyable

    func test<T>(_ t: T) where T: ~Self  // expected-error {{type 'Self' cannot be suppressed}}
}

protocol Sando { func make() }

class C: ~Copyable,  // expected-error {{classes cannot be '~Copyable'}}
         ~Sando // expected-error {{type 'Sando' cannot be suppressed}}
         {}

public struct MoveOnlyS1<T> : ~Copyable { /*deinit {}*/ }
public struct MoveOnlyS2<T: Equatable> : ~Copyable { /*deinit {}*/ }
public struct MoveOnlyS3<T: ~Copyable> : ~Copyable { /*deinit {}*/ }

protocol Rope<Element>: Hashable, ~Copyable {  // expected-error {{'Self' required to be 'Copyable' but is marked with '~Copyable'}}
  associatedtype Element: ~Copyable
}

extension S: ~Copyable {} // expected-error {{cannot suppress 'Copyable' in extension}}

struct S: ~U, // expected-error {{type 'U' cannot be suppressed}}
          ~Copyable {}

func greenBay<each T: ~Copyable>(_ r: repeat each T) {} // expected-error{{cannot suppress '~Copyable' on type 'each T'}}
// expected-error@-1 {{'each T' required to be 'Copyable' but is marked with '~Copyable'}}

typealias Clone = Copyable
func dup<D: ~Clone>(_ d: D) {}
// expected-error@-1 {{parameter of noncopyable type 'D' must specify ownership}}
// expected-note@-2 {{add 'borrowing'}}
// expected-note@-3 {{add 'inout'}}
// expected-note@-4 {{add 'consuming'}}

// expected-error@+2 {{parameter of noncopyable type 'some ~Copyable' must specify ownership}}
// expected-error@+1 {{parameter of noncopyable type 'some ~Clone' must specify ownership}}
func superb(_ thing: some ~Copyable, thing2: some ~Clone) {}
// expected-note@-1 2{{add 'borrowing'}}
// expected-note@-2 2{{add 'inout'}}
// expected-note@-3 2{{add 'consuming'}}

func ownership1(_ t: borrowing any ~Equatable) {} // expected-error {{type 'Equatable' cannot be suppressed}}

func ownership2(_ t: ~ borrowing Int) {} // expected-error {{cannot find type 'borrowing' in scope}}
                                         // expected-error@-1 {{unnamed parameters must be written with the empty name '_'}}
                                         // expected-error@-2 {{expected ',' separator}}

func ownership3(_ t: consuming some ~Clone) {}

func what(one: ~Copyable..., // expected-error {{noncopyable type '~Copyable' cannot be used within a variadic type yet}}
          two: ~(Copyable...) // expected-error {{variadic parameter cannot appear outside of a function parameter list}}
                              // expected-error@-1 {{parameter of noncopyable type '~Copyable' must specify ownership}}
              // expected-note@-2{{add 'borrowing' for an immutable reference}}
              // expected-note@-3{{add 'inout' for a mutable reference}}
              // expected-note@-4{{add 'consuming' to take the value from the caller}}
          ) {}

struct A { struct B { struct C {} } }

typealias Z2 = ~A.B.C // expected-error {{type 'A.B.C' cannot be suppressed}}
typealias Z3 = ~A? // expected-error {{type 'A?' cannot be suppressed}}
typealias Z4 = ~Rope<Int> // expected-error {{type 'Rope<Int>' cannot be suppressed}}
typealias Z5 = (~Int) -> Void // expected-error {{type 'Int' cannot be suppressed}}
typealias Z6 = ~() -> () // expected-error {{single argument function types require parentheses}}
                         // expected-error@-1 {{type '()' cannot be suppressed}}
typealias Z7 = ~(Copyable & Hashable) // expected-error {{type 'Hashable' cannot be suppressed}}
typealias Z8 = ~Copyable & Hashable // expected-error {{composition cannot contain '~Copyable' when another member requires 'Copyable'}}

struct NotAProtocol {}

struct Bad: ~NotAProtocol {} // expected-error {{type 'NotAProtocol' cannot be suppressed}}


struct X<T: ~Copyable>: ~Copyable { }

func typeInExpression() {
  _ = [~Copyable]()  // expected-error{{type '~Copyable' does not conform to protocol 'Copyable'}}
  _ = X<any ~Copyable>()

  _ = X<any ~Copyable & Foo>()
  _ = X<any Foo & ~Copyable>()

  _ = X<(borrowing any ~Copyable) -> Void>()

  _ = ~Copyable.self // expected-error{{unary operator '~' cannot be applied to an operand of type '(any Copyable).Type'}}
  _ = (any ~Copyable).self
}

func param3(_ t: borrowing any ~Copyable) {}
func param4(_ t: any ~Copyable.Type) {}

protocol P: ~Copyable {}
protocol Q: ~Copyable {}
protocol R: ~Copyable {}
struct Blooper<T: ~Copyable>: ~Copyable {}
extension Blooper: (Q & (R & (~Copyable & P))) {} // expected-error {{cannot suppress 'Copyable' in extension}}

protocol Edible {}
protocol Portable {}
typealias Alias = Portable & Copyable

struct Burrito<Filling: ~Copyable>: ~Copyable {}
extension Burrito: Alias {} // expected-error {{conformance to 'Copyable' must be declared in a separate extension}}
// expected-note@-1 {{'Burrito<Filling>' declares conformance to protocol 'Copyable' here}}

extension Burrito: Copyable & Edible & P {} // expected-error {{redundant conformance of 'Burrito<Filling>' to protocol 'Copyable'}}

struct Blah<T: ~Copyable>: ~Copyable {}
extension Blah: P, Q, Copyable, R {} // expected-error {{generic struct 'Blah' required to be 'Copyable' but is marked with '~Copyable'}}
// expected-error@-1 {{conformance to 'Copyable' must be declared in a separate extension}}

enum Hello<Gesture: ~Copyable>: ~Copyable {}
extension Hello: Copyable & Edible & P {} // expected-error {{conformance to 'Copyable' must be declared in a separate extension}}
