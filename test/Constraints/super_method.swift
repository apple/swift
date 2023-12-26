// RUN: %target-typecheck-verify-swift -parse-as-library

class D : B {
  func b_foo() -> Int { return super.foo }

  override func bar(_ a: Float) -> Int { return super.bar(a) }

  func bas() -> (Int, UnicodeScalar, String) {
    return (super.zim(), super.zang(), super.zung())
  }

  override var zippity : Int { return super.zippity }
}

extension D {
  func d_ext_foo() -> Int {
    return super.foo
  }
}

class B {
  var foo : Int = 0
  func bar(_ a: Float) -> Int {}

  func zim() -> Int {}
  func zang() -> UnicodeScalar {}
  func zung() -> String {}

  var zippity : Int { return 123 }
}

class X<T> {
  func method<U>(_ x: T, y: U) { }
}

class Y<U> : X<Int> {
  func otherMethod<V>(_ x: Int, y: V) {
    super.method(x, y: y)
  }
}

func use_d(_ d: D) -> Int {
  _ = d.b_foo()
  _ = d.bar(1.0)
  _ = d.bas()

  return d.zippity
}

func not_method() {
  super.foo() // expected-error{{'super' cannot be used outside of a class computed property, method, initializer, deinitializer, or subscript}}
}

// rdar://problem/50819554 - inability to properly resolve superclass shouldn't crash the solver
func test_that_invalid_supertype_ref_doesnt_crash() {
  final class Node: ManagedBuffer<AnyObject, Undefined> { // expected-error {{cannot find type 'Undefined' in scope}}
    static func create() {
      super.create()
    }
  }
}
