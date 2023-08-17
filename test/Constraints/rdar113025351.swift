// RUN: %target-typecheck-verify-swift

struct S {
  enum E: Error {
    case e(Int)
    case f
  }
}

func foo() throws {}

// rdar://113025351: Avoid emitting a separate diagnostic complaining that a
// 'let' cannot be nested in an expression, as it just adds noise.
func bar() throws {
  do {
    try foo()
  } catch S.E(let x) {} // expected-error {{'S.E' cannot be constructed because it has no accessible initializers}}
}

func baz(_ x: S.E) {
  if case S.E(let y) = x {} // expected-error {{'S.E' cannot be constructed because it has no accessible initializers}}
  if case S.E(S.E.e(let y)) = x {} // expected-error {{'S.E' cannot be constructed because it has no accessible initializers}}
}
