// RUN: %target-typecheck-verify-swift

// An inout parameter can be captured.
func foo(x: inout Int) {
  func bar() -> Int {
    return x
  }
}

// But not partially applied.
struct C {
  mutating func f(x: Int) {}
}

var c = C()
let x = c.f // expected-error{{calling a 'mutating' method without an argument list is not allowed}}
