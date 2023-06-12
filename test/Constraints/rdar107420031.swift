// RUN: %target-typecheck-verify-swift

enum E {
  case e
}

func ~= (lhs: any Error, rhs: E) -> Bool { true }

// rdar://107420031 – Make sure we don't crash.
// TODO: This ought to compile.
func foo(_ error: any Error) {
  switch error {
  case E.e: // expected-error {{pattern of type 'E' cannot match 'any Error'}}
    break
  default:
    break
  }
}
