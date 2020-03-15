// RUN: %target-typecheck-verify-swift

protocol P {
  @_requiresConcreteImplementation
  func f(_: Int) -> Bool // expected-note {{requirement 'f' declared here}}
  // expected-note@-1 {{overridden declaration is here}}
  func f<T: SignedInteger>(_: T) -> Bool

  func g(_: Int) -> Bool
}

extension P {
  func f<T: SignedInteger>(_ x: T) -> Bool { // expected-note {{'f' declared here}}
    return f(Int(x))
  }

  func g(_ x: Int) -> Bool { // expected-note {{'g' declared here}}
    return f(x)
  }
}

protocol Q: P {
  override func f(_: Int) -> Bool // expected-error {{override of protocol requirement marked '@_requiresConcreteImplementation' should also be marked }}
}

protocol R: P {
  @_requiresConcreteImplementation
  override func g(_: Int) -> Bool // expected-note {{requirement 'g' declared here}}
}

struct S: P { // expected-error {{type 'S' does not conform to protocol 'P'}}
  // expected-error@-1 {{protocol requirement marked '@_requiresConcreteImplementation' cannot be satisfied }}
}

struct T: P, R { // expected-error {{type 'T' does not conform to protocol 'R'}}
  // expected-error@-1 {{protocol requirement marked '@_requiresConcreteImplementation' cannot be satisfied }}
  func f(_: Int) -> Bool { true }
}

struct U: R {
  func f(_: Int) -> Bool { true }
  func g(_: Int) -> Bool { false }
}

class C {
  @_requiresConcreteImplementation // expected-error {{'@_requiresConcreteImplementation' can only be specified on protocol requirements}}
  var x: Int = 42
}
