// RUN: %target-typecheck-verify-swift

// rdar://109419240 – Make sure we don't crash
enum E { // expected-note {{'E' declared here}}
  case e(Int)
}

func foo(_ arr: [E]) -> Int {
  return arr.reduce(0) { (total, elem) -> Int in
    switch elem {
    case let e(x): // expected-error {{cannot find 'e' in scope; did you mean 'E'?}}
      return total + x
    }
  }
}
