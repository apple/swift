// RUN: %target-swift-frontend %s -o /dev/null -verify -emit-sil

struct Box<T> {
  var value: T
}

class Klass {}

func myPrint(_ x: inout Box<Klass>) -> () { print(x) }

func testError() -> (() -> ()) {
  // We emit this error twice since we run allocbox to stack twice in the pipeline for now. This is not an official feature, so it is ok for now.
  @_semantics("boxtostack.mustbeonstack")
  var x = Box<Klass>(value: Klass()) // expected-error {{Can not promote value from heap to stack due to value escaping}}
  let result = { // expected-note {{value escapes here}}
    myPrint(&x)
  }
  return result
}

func main() {
  testError()()
}

main()
