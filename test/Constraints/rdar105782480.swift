// RUN: %target-typecheck-verify-swift

// rdar://105782480
enum MyEnum {
  case second(Int?)
}

func takeClosure(_ x: () -> Void) {}

func foo(value: MyEnum) {
  takeClosure {
    switch value {
    case .second(let drag).invalid:
      // expected-error@-1 {{value of type 'MyEnum' has no member 'invalid'}}
      break
    }
  }
}
