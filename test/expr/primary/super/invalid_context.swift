// RUN: %target-typecheck-verify-swift

class Root {
  let x = 0

  // expected-error@+1 {{'super' cannot be used outside of a class computed property, method, initializer, deinitializer, or subscript}}
  let testStoredRoot = super.x

  var testComputedRoot: Int {
    // expected-error@+1 {{'super' cannot be used in class 'Root' because it has no superclass}}
    let _ = super.x
  }

  init(root: Void) {
    // expected-error@+1 {{'super' cannot be used in class 'Root' because it has no superclass}}
    super.x
    // expected-error@+1 {{'super' cannot be used in class 'Root' because it has no superclass}}
    super.init()
  }

  func testMethodRoot(
    // expected-error@+1 {{'super' cannot be used in class 'Root' because it has no superclass}}
    _: Int = super.x
  ) {
    // expected-error@+1 {{'super' cannot be used in class 'Root' because it has no superclass}}
    super.testMethodRoot()
  }

  deinit {
    // expected-error@+1 {{'super' cannot be used in class 'Root' because it has no superclass}}
    super.x
  }
}

extension Root {
  func testMethodRootExtension() {
    // expected-error@+1 {{'super' cannot be used in extension of class 'Root' because it has no superclass}}
    super.x
  }
}

class Derived: Root {
  // expected-error@+1 {{'super' cannot be used outside of a class computed property, method, initializer, deinitializer, or subscript}}
  let testStoredDerived = super.x

  var testComputedDerived: Int {
    super.x
  }

  init(derived: Void) {
    let _ = super.x
  }

  func testMethodDerived(_: Int = super.x) -> Int {
    super.x
  }

  deinit {
    let _ = super.x
  }
}

protocol P: Derived {}
extension P {
  func test() {
    // expected-error@+1 {{'super' cannot be used in non-class type 'P'}}
    super.x
  }
}

enum E {
  case a(
    // expected-error@+1 {{'super' cannot be used in non-class type 'E'}}
    _: Int = super.undef
  )

  func test() {
    // expected-error@+1 {{'super' cannot be used in non-class type 'E'}}
    super.undef
  }
}

struct S {
  // expected-error@+1 {{'super' cannot be used in non-class type 'S'}}
  let testStoredRoot = super.undef

  init() {
    // expected-error@+1 {{'super' cannot be used in non-class type 'S'}}
    super.init()
  }

  func test() {
    // expected-error@+1 {{'super' cannot be used in non-class type 'S'}}
    super.undef
  }
}

func test() {
  // expected-error@+1 {{'super' cannot be used outside of a class computed property, method, initializer, deinitializer, or subscript}}
  super.undef
}

// expected-error@+1 {{'super' cannot be used outside of a class computed property, method, initializer, deinitializer, or subscript}}
super.init()
