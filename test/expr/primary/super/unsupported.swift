// RUN: %target-typecheck-verify-swift

class Base {}
class Derived: Base {
  override init() {
    // expected-error@+1 {{'super' may be used only to access a superclass member, subscript, or initializer}}
    let _  = super
  }

  deinit {
    // expected-error@+2 {{'super' may be used only to access a superclass member, subscript, or initializer}}
    // expected-error@+1 {{cannot call value of non-function type 'Base'}}
    super()
  }

  var unsupported: Base {
    get {
      // expected-error@+1 {{'super' may be used only to access a superclass member, subscript, or initializer}}
      super
    }
  }

  subscript(_: Base) -> Bool {
    true
  }

  func unsupported(
    // expected-error@+1 {{'super' may be used only to access a superclass member, subscript, or initializer}}
    _: Base = super
  ) {
    // expected-error@+2 {{'super' may be used only to access a superclass member, subscript, or initializer}}
    // expected-warning@+1 {{expression of type 'Base' is unused}}
    super
    // expected-error@+2 {{'super' may be used only to access a superclass member, subscript, or initializer}}
    // expected-error@+1 {{cannot call value of non-function type 'Base'}}
    super(0)
    // expected-error@+2 {{'super' may be used only to access a superclass member, subscript, or initializer}}
    // expected-warning@+1 {{expression of type 'Base' is unused}}
    super
      [1] // expected-warning{{expression of type '[Int]' is unused}}

    // expected-error@+1 {{'super' may be used only to access a superclass member, subscript, or initializer}}
    let _ = self[super]

    // FIXME: Duplicate diagnostic because expression is re-pre-checked after folding initial sequence expression.
    // expected-error@+1 2 {{'super' may be used only to access a superclass member, subscript, or initializer}}
    _ = (0, super)

    func nested() {
      let _ = [
        // expected-error@+1 {{'super' may be used only to access a superclass member, subscript, or initializer}}
        super
      ]
    }
  }
}

extension Derived {
  func unsupportedInExtension() {
    // expected-error@+1 {{'super' may be used only to access a superclass member, subscript, or initializer}}
    let _ = super
  }
}

// Do not complain about a missing/illegal parent expression unless 'super' is
// legal in context.

func unsupported() {
  // expected-error@+1 {{'super' cannot be used outside of a class computed property, method, initializer, deinitializer, or subscript}}
  super
}
protocol P: Derived {}
extension P {
  func unsupported() {
    // expected-error@+1 {{'super' cannot be used outside of a class computed property, method, initializer, deinitializer, or subscript}}
    super
  }
}
enum E {
  var unsupported: Void {
    // expected-error@+1 {{'super' cannot be used outside of a class computed property, method, initializer, deinitializer, or subscript}}
    super
  }
}
class Root {
  // expected-error@+1 {{'super' cannot be used outside of a class computed property, method, initializer, deinitializer, or subscript}}
  let sup = super

  init() {
    // expected-error@+1 {{'super' cannot be used in class 'Root' because it has no superclass}}
    super
  }

  func unsupported(
    // expected-error@+1 {{'super' cannot be used in class 'Root' because it has no superclass}}
    _: Root = super
  ) {
    // expected-error@+1 {{'super' cannot be used in class 'Root' because it has no superclass}}
    super
    struct S {
      func unsupported() {
        // expected-error@+1 {{'super' cannot be used outside of a class computed property, method, initializer, deinitializer, or subscript}}
        super
      }
    }
  }
}
