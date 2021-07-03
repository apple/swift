// RUN: %target-typecheck-verify-swift

//
// Type aliases that reference unbound generic types -- not really generic,
// but they behave as such, in the sense that you can apply generic
// arguments to them.
//

struct MyType<TyA, TyB> {
  var a : TyA, b : TyB
}

typealias OurType = MyType

typealias YourType = Swift.Optional

struct Container {
  typealias YourType = Swift.Optional
}

let _: OurType<Int, String>
let _: YourType<Int>
let _: Container.YourType<Int>

typealias WeirdType = Array where Element : Equatable
// expected-error@-1 {{'where' clause cannot be applied to a non-generic top-level declaration}}
// expected-error@-2 {{reference to generic type 'Array' requires arguments in <...>}}

struct Generic<T> {
  typealias WeirdType = Array where Element : Equatable
  // expected-error@-1 {{cannot find type 'Element' in scope}}
  // expected-error@-2 {{type '<<error type>>' in conformance requirement does not refer to a generic parameter or associated type}}
  // expected-error@-3 {{reference to generic type 'Array' requires arguments in <...>}}

  typealias WeirdestType = Array where T : Equatable
  // expected-error@-1 {{reference to generic type 'Array' requires arguments in <...>}}

  typealias SelfType = Generic
}

struct NotEquatable {}

let _: Generic<NotEquatable>.WeirdestType = [1, 2, 3]
// expected-error@-1 {{type 'NotEquatable' does not conform to protocol 'Equatable'}}

// FIXME: Diagnose this
let _: Generic<NotEquatable>.WeirdestType<Int> = [1, 2, 3]

let _: Generic<Int>.SelfType = Generic<Int>()