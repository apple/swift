// RUN: %target-typecheck-verify-swift -parse-as-library 

class D : B {
  func foo() {
    super.init() // expected-error{{'super.init' cannot be called outside of an initializer}}
  }

  init(a:Int) {
    super.init()
  }

  init(f:Int) {
    super.init(a: "x")
  }

  init(g:Int) {
    super.init("aoeu") // expected-error{{no exact matches in call to initializer}}
  }

  init(h:Int) {
    var _ : B = super.init() // expected-error{{cannot convert value of type '()' to specified type 'B'}}
  }

  init(d:Double) {
    super.init()
  }
}

class B {
  var foo : Int
  func bar() {}

  init() {
  }

  init(x:Int) { // expected-note{{candidate expects value of type 'Int' for parameter #1}}
  }

  init(a:UnicodeScalar) { // expected-note {{candidate expects value of type 'UnicodeScalar' (aka 'Unicode.Scalar') for parameter #1}}
  }
  init(b:UnicodeScalar) { // expected-note {{candidate expects value of type 'UnicodeScalar' (aka 'Unicode.Scalar') for parameter #1}}
  }
}

/// https://github.com/apple/swift/issues/45089
/// Bad diagnostic for incorrectly calling private `init`

class C_45089 {
  private init() {} // expected-note {{'init()' declared here}}
  private init(a: Int) {}
}

class Impl_45089 : C_45089 {
  init() {
    super.init() // expected-error {{'C_45089' initializer is inaccessible due to 'private' protection level}}
  }
}

class A_Priv<T> {
  private init(_ foo: T) {}
}

class B_Override<U> : A_Priv<[U]> {
  init(_ foo: [U]) { fatalError() } // Ok, because effectively overrides init from parent which is invisible
}
