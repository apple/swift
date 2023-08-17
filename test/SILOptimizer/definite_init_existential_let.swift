// RUN: %target-swift-frontend -emit-sil -verify %s

// rdar://problem/29716016 - Check that we properly enforce DI on `let`
// variables and class properties.

protocol P { }

extension P {
  mutating func foo() {}
  var bar: Int { get { return 0 } set {} }
}

class ImmutableP {
  let field: P // expected-note* {{}}

  init(field: P) {
    self.field = field
    self.field.foo() // expected-error{{}}
    self.field.bar = 4 // expected-error{{}}
  }
}

func immutableP(field: P) {
  let x: P // expected-note* {{}}

  x = field
  x.foo() // expected-error{{}}
  x.bar = 4 // expected-error{{}}
}
