// RUN: %target-swiftc_driver -emit-sil %s -o /dev/null -Xfrontend -verify

// TODO: Change this back to using target-swift-frontend once we move errors to
// type checker and SILGen.

class Klass {}

class B {
  init(x: inout Int) {}
  init(x: inout Klass) {}
}

class A : B {
  let x: Int // expected-note {{change 'let' to 'var' to make it mutable}}

  init() {
    self.x = 12
    super.init(x: &x) // expected-error {{cannot pass immutable value as inout argument: 'x' is a 'let' constant}}
  }
}

class C : B {
  let x: Klass // expected-note {{change 'let' to 'var' to make it mutable}}

  init() {
    self.x = Klass()
    super.init(x: &x) // expected-error {{cannot pass immutable value as inout argument: 'x' is a 'let' constant}}
  }
}

class D : B {
  var x: Int

  init() {
    self.x = 12
    super.init(x: &x)
  }
}

class E : B {
  var x: Klass

  init() {
    self.x = Klass()
    super.init(x: &x)
  }
}
