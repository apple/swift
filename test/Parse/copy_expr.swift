// RUN: %target-typecheck-verify-swift -disable-availability-checking

var global: Int = 5
func testGlobal() {
    let _ = copy global
}

func testLet() {
    let t = String()
    let _ = copy t
}

func testVar() {
    var t = String()
    t = String()
    let _ = copy t
}

func copy() {}
func copy(_: String) {}
func copy(_: String, _: Int) {}
func copy(x: String, y: Int) {}

// Ensure that we can still call a function named copy.

func useCopyFunc() {
    var s = String()
    var i = global

    copy()
    copy(s)
    copy(i) // expected-error{{cannot convert value of type 'Int' to expected argument type 'String'}}
    copy(s, i)
    copy(i, s) // expected-error{{unnamed argument #2 must precede unnamed argument #1}}
    copy(x: s, y: i)
    copy(y: i, x: s) // expected-error{{argument 'x' must precede argument 'y'}}
}

// Ensure that we can still use a variable named copy.

func useCopyVar(copy: inout String) {
    let s = copy
    copy = s

    // We can copy from a variable named `copy`
    let t = copy copy
    copy = t

    // We can do member access and subscript a variable named `copy`
    let i = copy.startIndex
    let _ = copy[i]
}

@propertyWrapper
struct FooWrapper<T> {
    var value: T

    init(wrappedValue: T) { value = wrappedValue }

    var wrappedValue: T {
        get { value }
        nonmutating set {}
    }
    var projectedValue: T {
        get { value }
        nonmutating set {}
    }
}

struct Foo {
    @FooWrapper var wrapperTest: String

    func copySelf() {
        _ = copy self
    }

    func copyPropertyWrapper() {
        // Make sure that we can parse.
        _ = copy wrapperTest // expected-error {{'copy' can only be applied to lvalues}}
        _ = copy _wrapperTest // expected-error {{'copy' can only be applied to lvalues}}
        _ = copy $wrapperTest // expected-error {{'copy' can only be applied to lvalues}}
    }
}

func testParseCopyWithDollarIdentifier() {
  class Klass {}
  let f: (Klass) -> () = {
    let _ = copy $0
  }
  _ = f
}

func testParseCopySelf() {
  class Klass {
    func test() {
      let _ = copy self
    }
  }
}

func testForLoop() {
  for copy in 0..<1024 {
    _ = copy
  }
}

class ParentKlass {}
class ChildKlass : ParentKlass {}

func testAsBindingVariableInSwitch(_ x: ChildKlass) {
  switch x {
  case let copy as ParentKlass:
    _ = copy
    break
  }
}
