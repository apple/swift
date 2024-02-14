// RUN: %target-typecheck-verify-swift

class Base {
  var foo: Int

  init() {}
}

class Derived : Base {
  func captureWeak() {
    let g = { [weak self] () -> Void in // expected-note * {{'self' explicitly captured here}}
      super.foo() // expected-error {{using 'super' in a closure where 'self' is explicitly captured is not yet supported}}
    }
    g()
  }

  func captureUnowned() {
    let g = { [unowned self] () -> Void in // expected-note * {{'self' explicitly captured here}}
      super.foo() // expected-error {{using 'super' in a closure where 'self' is explicitly captured is not yet supported}}
    }
    g()
  }

  func nestedInner() {
    let g = { () -> Void in
      let h = { [weak self] () -> Void in // expected-note * {{'self' explicitly captured here}}
        super.foo() // expected-error {{using 'super' in a closure where 'self' is explicitly captured is not yet supported}}
        nil ?? super.foo() // expected-error {{using 'super' in a closure where 'self' is explicitly captured is not yet supported}}
      }
      h()
    }
    g()
  }

  func nestedOuter() {
    let g = { [weak self] () -> Void in // expected-note * {{'self' explicitly captured here}}
      let h = { () -> Void in
        super.foo() // expected-error {{using 'super' in a closure where 'self' is explicitly captured is not yet supported}}
        nil ?? super.foo() // expected-error {{using 'super' in a closure where 'self' is explicitly captured is not yet supported}}
      }
      h()
    }
    g()
  }
}
