// RUN: %target-typecheck-verify-swift -disable-availability-checking

protocol P<A> {
  associatedtype A
}

func f1(x: any P) -> any P<Int> {
  // FIXME: Bad diagnostic
  return x // expected-error {{type of expression is ambiguous without a type annotation}}
}

func f2(x: any P<Int>) -> any P {
  return x // okay
}

func f3(x: any P<Int>) -> any P<String> {
  // FIXME: Misleading diagnostic
  return x // expected-error {{return expressions for types 'String' and 'Int' are not interconvertible}}
}

struct G<T> {}
// expected-note@-1 {{arguments to generic parameter 'T' ('any P<Int>' and 'any P') are expected to be equal}}
// expected-note@-2 {{arguments to generic parameter 'T' ('any P' and 'any P<Int>') are expected to be equal}}
// expected-note@-3 {{arguments to generic parameter 'T' ('any P<Int>' and 'any P<String>') are expected to be equal}}

func g1(x: G<any P>) -> G<any P<Int>> {
  return x // expected-error {{return expressions for types 'G<any P>' and 'G<any P<Int>>' are not interconvertible}}
}

func g2(x: G<any P<Int>>) -> G<any P> {
  return x // expected-error {{return expressions for types 'G<any P<Int>>' and 'G<any P>' are not interconvertible}}
}

func g3(x: G<any P<Int>>) -> G<any P<String>> {
  return x // expected-error {{return expressions for types 'G<any P<Int>>' and 'G<any P<String>>' are not interconvertible}}
}

func h1(x: (any P)?) -> (any P<Int>)? {
  return x // expected-error {{return expressions for types 'any P' and 'any P<Int>' are not interconvertible}}
}

func h2(x: (any P<Int>)?) -> (any P)? {
  return x // okay
}

func h3(x: (any P<Int>)?) -> (any P<String>)? {
  return x // expected-error {{return expressions for types 'any P<Int>' and 'any P<String>' are not interconvertible}
  // expected-note@-1 {{arguments to generic parameter 'T' ('any P<Int>' and 'any P<String>') are expected to be equal}}
}
