// RUN: %target-typecheck-verify-swift

// rdar://107724970 – Make sure we don't crash.
enum E {
  case e(Int)
}
func foo(_ x: E) {
  // FIXME: We need to handle pattern arguments in a bunch of places in argument
  // list diagnostic logic.
  // https://github.com/apple/swift/issues/65062
  let fn = { // expected-error {{unable to infer closure type in the current context}}
    switch x {
    case E.e(_, _):
      break
    }
  }
}
