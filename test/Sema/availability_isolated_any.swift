// RUN: %target-typecheck-verify-swift -target %target-cpu-apple-macosx10.50

// REQUIRES: OS=macosx

// Concrete uses of @isolated(any) are fine.
func expectIsolatedAny(fn: @isolated(any) () -> ()) {}

func testConcrete1(fn: @isolated(any) () -> ()) {
  expectIsolatedAny(fn: fn)
}
func testConcrete2(fn: @isolated(any) () -> ()) {
  expectIsolatedAny {}
}
func testConcrete3(fn: @MainActor () -> ()) {
  expectIsolatedAny {}
}

func testErasure(fn: @escaping @isolated(any) () -> ()) -> Any {
  return fn // expected-error {{runtime support for @isolated(any) function types is only available in}}
  // expected-note @-2 {{add @available attribute to enclosing global function}}
  // expected-note @-2 {{add 'if #available' version check}}
}

func testCovariantErasure(fn: @escaping () -> @isolated(any) () -> Void) -> (() -> Any) {
  return fn // expected-error {{runtime support for @isolated(any) function types is only available in}}
  // expected-note @-2 {{add @available attribute to enclosing global function}}
  // expected-note @-2 {{add 'if #available' version check}}
}

func testContravariantErasure(fn: @escaping (Any) -> Void) -> ((@escaping @isolated(any) () -> Void) -> Void) {
  return fn // expected-error {{runtime support for @isolated(any) function types is only available in}}
  // expected-note @-2 {{add @available attribute to enclosing global function}}
  // expected-note @-2 {{add 'if #available' version check}}
}

protocol P {
  associatedtype A
}

struct S: P { // expected-note {{add @available attribute to enclosing struct}}
  typealias A = @isolated(any) () -> () // expected-error {{runtime support for @isolated(any) function types is only available in}}
  // expected-note @-1 {{add @available attribute to enclosing type alias}}
}
