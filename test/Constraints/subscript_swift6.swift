// RUN: %target-typecheck-verify-swift -swift-version 6

// Note: In Swift >= 6 mode this would become an error.
func test_subscript_accepts_type_name_argument() {
  struct A {
    subscript(a: A.Type) -> Int { get { 42 } }
  }

  func test(a: A, optA: A?) {
    let _ = a[A] // expected-error {{expected member name or initializer call after type name}}
    // expected-note@-1 {{add arguments after the type to construct a value of the type}} {{16-16=()}}
    // expected-note@-2 {{use '.self' to reference the type object}} {{16-16=.self}}

    let _ = optA?[A] // expected-error {{expected member name or initializer call after type name}}
    // expected-note@-1 {{add arguments after the type to construct a value of the type}} {{20-20=()}}
    // expected-note@-2 {{use '.self' to reference the type object}} {{20-20=.self}}
  }
}
