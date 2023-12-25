// RUN: %target-typecheck-verify-swift

class B {
  var foo: Int
  func bar() {}

  init() {} // expected-note {{found this candidate}}
  init(x: Int) {} // expected-note {{found this candidate}}

  subscript(x: Int) -> Int {
    get {}
    set {}
  }
}

class D : B {
  override init() {
    super.init()
    super.init(42)
    // expected-error@-1 {{missing argument label 'x:' in call}}
  }

  override init(x:Int) {
    let _: () -> B = super.init // expected-error {{cannot reference 'super.init' initializer chain as function value}}
  }

  convenience init(y:Int) {
    let _: () -> D = self.init // expected-error {{cannot reference 'self.init' initializer delegation as function value}}
  }

  init(z: Int) {
    super
      .init(x: z)
  }

  func super_calls() {
    super.foo        // expected-warning {{property is accessed but result is unused}}
    super.foo.bar    // expected-error {{value of type 'Int' has no member 'bar'}}
    super.bar        // expected-error {{function is unused}}
    super.bar()
    // FIXME: should also say "'super.init' cannot be referenced outside of an initializer"
    super.init // expected-error{{no exact matches in reference to initializer}}
    super.init() // expected-error{{'super.init' cannot be called outside of an initializer}}
    super.init(0) // expected-error{{'super.init' cannot be called outside of an initializer}} // expected-error {{missing argument label 'x:' in call}}
    super[0]        // expected-warning {{subscript is accessed but result is unused}}
    super
      .bar()
  }

  func bad_super_1() {
    super.$0 // expected-error{{expected identifier or 'init'}}
  }
}
