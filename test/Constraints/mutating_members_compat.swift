// RUN: %target-swift-frontend -typecheck -verify -swift-version 4 %s

protocol P {}

struct Foo {
  mutating func boom() {}
}

let x = Foo.boom // expected-warning{{calling a 'mutating' method without an argument list is not allowed}}
var y = Foo()
let z0 = y.boom // expected-error{{calling a 'mutating' method without an argument list is not allowed}}
let z1 = Foo.boom(&y) // expected-error{{calling a 'mutating' method without an argument list is not allowed}}

func fromLocalContext() -> (inout Foo) -> () -> () {
  return Foo.boom // expected-warning{{calling a 'mutating' method without an argument list is not allowed}}
}
func fromLocalContext2(x: inout Foo, y: Bool) -> () -> () {
  if y {
    return x.boom // expected-error{{calling a 'mutating' method without an argument list is not allowed}}
  } else {
    return Foo.boom(&x) // expected-error{{calling a 'mutating' method without an argument list is not allowed}}
  }
}

func bar() -> P.Type { fatalError() }
func bar() -> Foo.Type { fatalError() }

_ = bar().boom       // expected-warning{{calling a 'mutating' method without an argument list is not allowed}}
_ = bar().boom(&y)   // expected-error{{calling a 'mutating' method without an argument list is not allowed}}
_ = bar().boom(&y)() // expected-error{{calling a 'mutating' method without an argument list is not allowed}}

func foo(_ foo: Foo.Type) {
  _ = foo.boom       // expected-warning{{calling a 'mutating' method without an argument list is not allowed}}
  _ = foo.boom(&y)   // expected-error{{calling a 'mutating' method without an argument list is not allowed}}
  _ = foo.boom(&y)() // expected-error{{calling a 'mutating' method without an argument list is not allowed}}
}
