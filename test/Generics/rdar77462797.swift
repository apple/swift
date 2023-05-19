// RUN: %target-typecheck-verify-swift -warn-redundant-requirements
// RUN: %target-swift-frontend -typecheck %s -debug-generic-signatures 2>&1 | %FileCheck %s

protocol P {}

protocol Q {
  associatedtype T : P
}

class S<T : P> : Q {}

struct G<X, Y> where X : Q {
  // no redundancies
  func foo1() where X == S<Y> {} // expected-warning {{redundant conformance constraint 'S<Y>' : 'Q'}}
  // CHECK: Generic signature: <X, Y where X == S<Y>, Y : P>

  // 'Y : P' is redundant, but only via an inferred requirement from S<Y>,
  // so we should not diagnose it
  func foo2() where X == S<Y>, Y : P {} // expected-warning {{redundant conformance constraint 'S<Y>' : 'Q'}}
  // CHECK: Generic signature: <X, Y where X == S<Y>, Y : P>

  // 'X : Q' is redundant
  func foo3() where X : Q, X == S<Y>, Y : P {} // expected-warning {{redundant conformance constraint 'S<Y>' : 'Q'}}
  // expected-warning@-1 {{redundant conformance constraint 'S<Y>' : 'Q'}}
  // CHECK: Generic signature: <X, Y where X == S<Y>, Y : P>

  // 'T.T : P' is redundant
  func foo4<T : Q>(_: T) where X == S<Y>, T.T : P {} // expected-warning {{redundant conformance constraint 'S<Y>' : 'Q'}}
  // expected-warning@-1 {{redundant conformance constraint 'T.T' : 'P'}}
  // CHECK: Generic signature: <X, Y, T where X == S<Y>, Y : P, T : Q>
}

func foo<X, Y>(_: X, _: Y) where X : Q, X : S<Y>, Y : P {}
// expected-warning@-1 {{redundant conformance constraint 'S<Y>' : 'Q'}}
// CHECK: Generic signature: <X, Y where X : S<Y>, Y : P>
