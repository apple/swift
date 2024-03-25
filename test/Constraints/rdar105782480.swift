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
      // expected-error@-1 {{expression pattern of type 'Unicode.Scalar' cannot match values of type 'MyEnum'}}
      // expected-error@-2 {{type 'Unicode.Scalar' has no member 'second'}}
      break
    }
  }
}
