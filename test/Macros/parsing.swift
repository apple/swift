// REQUIRES: swift_swift_parser

// RUN: %target-typecheck-verify-swift -swift-version 5

protocol P { }
protocol Q { associatedtype Assoc }

@freestanding(expression) macro m1() -> Int = #externalMacro(module: "A", type: "M1")
// expected-warning@-1{{external macro implementation type 'A.M1' could not be found for macro 'm1()'}}
// expected-note@-2{{'m1()' declared here}}
@freestanding(expression) macro m2(_: Int) = #externalMacro(module: "A", type: "M2")
// expected-warning@-1{{external macro implementation type 'A.M2' could not be found for macro 'm2'}}
@freestanding(expression) macro m3(a b: Int) -> Int = #externalMacro(module: "A", type: "M3")
// expected-warning@-1{{external macro implementation type 'A.M3' could not be found for macro 'm3(a:)'}}
@freestanding(expression) macro m4<T: Q>() -> T = #externalMacro(module: "A", type: "M4") where T.Assoc: P
// expected-warning@-1{{external macro implementation type 'A.M4' could not be found for macro 'm4()'}}
@freestanding(expression) macro m5<T: P>(_: T) = #externalMacro(module: "A", type: "M4")
// expected-warning@-1{{external macro implementation type 'A.M4' could not be found for macro 'm5'}}

@freestanding(expression) macro m6 = A // expected-error{{expected '(' for macro parameters or ':' for a value-like macro}}
// expected-error@-1{{by a macro expansion}}

// expected-error @+2 {{expected '('}}
// expected-error @+1 {{macro 'm7' must declare its applicable roles}}
@freestanding macro m7(_: String) = #externalMacro(module: "A", type: "M4")
// expected-warning@-1{{external macro implementation type 'A.M4' could not be found for macro 'm7'}}

// expected-error @+2 {{expected a freestanding macro role such as 'expression'}}
// expected-error @+1 {{macro 'm8' must declare its applicable roles}}
@freestanding(abc) macro m8(_: String) = #externalMacro(module: "A", type: "M4")
// expected-warning@-1{{external macro implementation type 'A.M4' could not be found for macro 'm8'}}
@freestanding(declaration, names: arbitrary) macro m9(_: String) = #externalMacro(module: "A", type: "M4")
// expected-warning@-1{{external macro implementation type 'A.M4' could not be found for macro 'm9'}}

@freestanding(expression) @freestanding(declaration, names: named(Foo)) @attached(accessor)
macro m10(_: String) = #externalMacro(module: "A", type: "M4")
// expected-warning@-1{{external macro implementation type 'A.M4' could not be found for macro 'm10'}}
// expected-error@-2{{macro can only have a single freestanding role}}

@attached(
  accessor,
  names: overloaded, arbitrary, named(hello), prefixed(_), suffixed(_favorite)
)
macro am1()
// expected-error@-1{{macro 'am1()' requires a definition}}

@attached(
  accessor,
  overloaded, // expected-error{{@attached argument is missing label 'names'}}
  unknown, // expected-error{{unknown introduced name kind 'unknown'}}
  named, // expected-error{{introduced name kind 'named' requires a single argument '(name)'}}
  arbitrary(a) // expected-error{{introduced name kind 'arbitrary' must not have an argument}}
)
macro am2()
// expected-error@-1{{macro 'am2()' requires a definition}}

#m1 + 1
// expected-warning @-1 {{result of operator '+' is unused}}
// expected-error @-2 {{external macro implementation type 'A.M1' could not be found for macro 'm1()'}}
