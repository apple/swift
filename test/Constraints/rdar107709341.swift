// RUN: %target-typecheck-verify-swift

// rdar://107709341 – Make sure we don't crash.
func foo(_ x: Int) {
  let _ = {
    switch x {
    case Optional<Int>.some(x): // expected-error {{pattern of type 'Optional<Int>' cannot match 'Int'}} {{none}}
      break
    default:
      break
    }
  }
}
