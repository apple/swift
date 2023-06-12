// RUN: %target-typecheck-verify-swift -swift-version 4 -experimental-one-way-closure-params

func testBasic() {
  let _: (Float) -> Float = { $0 + 1 }

  let _ = { $0 + 1 } // expected-error{{cannot infer type of closure parameter '$0' without a type annotation}}
}

