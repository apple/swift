// RUN: %target-typecheck-verify-swift

protocol Sando { func make() } // expected-note {{protocol requires function 'make()'}}

struct BuggerView: ~Escapable {} // expected-error {{type '~Escapable' requires -enable-experimental-feature NonescapableTypes}}

struct S: ~U, // expected-error {{type 'U' is not invertible}}
          ~Copyable {}

struct U: // expected-error {{noncopyable struct 'U' cannot conform to 'Sando'}}
          // expected-error@-1 {{type 'U' does not conform to protocol 'Sando'}}
          ~Copyable,
          Sando,
          ~Copyable
          {}

class C: ~Copyable, // expected-error {{cannot suppress conformances here}}
         ~Sando // expected-error {{type 'Sando' is not invertible}}
         {}

protocol Rope<Element>: ~Copyable { // expected-error {{cannot suppress conformances here}}

  associatedtype Element: ~Copyable // expected-error {{cannot suppress conformances here}}
}

extension S: ~Copyable {} // expected-error {{cannot suppress conformances here}}
                          // expected-error@-1 {{noncopyable struct 'S' cannot conform to '~Copyable'}}

func takeNoncopyableGeneric<T: ~Copyable>(_ t: T) {} // expected-error {{cannot suppress conformances here}}

@_moveOnly struct ExtraNonCopyable: ~Copyable {}

// basic tests to ensure it's viewed as a noncopyable struct, by using 
// capabilities only available to them
struct HasADeinit: ~Copyable { deinit {} }

public struct MoveOnlyS1<T> : ~Copyable { deinit {} }
public struct MoveOnlyS2<T: Equatable> : ~Copyable { deinit {} }
public struct MoveOnlyS3<T: ~Copyable> : ~Copyable { deinit {} } // expected-error {{cannot suppress conformances here}}

public enum MoveOnlyE1<T> : ~Copyable { 
  case holding(s: MoveOnlyS1<T>)
  consuming func x() {}
}

public enum MoveOnlyE2<T: Equatable> : ~Copyable { 
  case holding(s: MoveOnlyS1<T>)
  consuming func x() {}
}

func more() {
  let _: any ~Copyable = 19  // expected-error@:14 {{cannot suppress conformances here}}

  let _: any ~Equatable = 19  // expected-error@:14 {{type 'Equatable' is not invertible}}
}

func blah<T>(_ t: T) where T: ~Copyable,    // expected-error@:31 {{cannot suppress conformances here}}

                           T: ~Hashable {}  // expected-error@:31 {{type 'Hashable' is not invertible}}

func foo<T: ~Copyable>(x: T) {} // expected-error {{cannot suppress conformances here}}

struct Buurap<T: ~Copyable> {} // expected-error {{cannot suppress conformances here}}

protocol Foo where Self: ~Copyable {} // expected-error {{cannot suppress conformances here}}
