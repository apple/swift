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
  return x // expected-error {{cannot convert return expression of type 'String' to return type 'Int'}}
}

struct G<T> {}
// expected-note@-1 {{arguments to generic parameter 'T' ('any P<Int>' and 'any P') are expected to be equal}}
// expected-note@-2 {{arguments to generic parameter 'T' ('any P' and 'any P<Int>') are expected to be equal}}
// expected-note@-3 {{arguments to generic parameter 'T' ('any P<Int>' and 'any P<String>') are expected to be equal}}

func g1(x: G<any P>) -> G<any P<Int>> {
  return x // expected-error {{cannot convert return expression of type 'G<any P>' to return type 'G<any P<Int>>'}}
}

func g2(x: G<any P<Int>>) -> G<any P> {
  return x // expected-error {{cannot convert return expression of type 'G<any P<Int>>' to return type 'G<any P>'}}
}

func g3(x: G<any P<Int>>) -> G<any P<String>> {
  return x // expected-error {{cannot convert return expression of type 'G<any P<Int>>' to return type 'G<any P<String>>'}}
}

func h1(x: (any P)?) -> (any P<Int>)? {
  return x // expected-error {{cannot convert return expression of type '(any P)?' to return type '(any P<Int>)?'}}
}

func h2(x: (any P<Int>)?) -> (any P)? {
  return x // okay
}

func h3(x: (any P<Int>)?) -> (any P<String>)? {
  return x // expected-error {{cannot convert return expression of type '(any P<Int>)?' to return type '(any P<String>)?'}}
}
