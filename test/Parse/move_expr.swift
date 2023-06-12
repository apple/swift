// RUN: %target-typecheck-verify-swift -disable-availability-checking

var global: Int = 5
func testGlobal() {
    let _ = consume global
}

func testLet() {
    let t = String()
    let _ = consume t
}

func testVar() {
    var t = String()
    t = String()
    let _ = consume t
}

func consume() {}
func consume(_: String) {}
func consume(_: String, _: Int) {}
func consume(x: String, y: Int) {}

// Ensure that we can still call a function named consume.

func useConsumeFunc() {
    var s = String()
    var i = global

    consume()
    consume(s)
    consume(i) // expected-error{{cannot convert value of type 'Int' to expected argument type 'String'}}
    consume(s, i)
    consume(i, s) // expected-error{{unnamed argument #2 must precede unnamed argument #1}}
    consume(x: s, y: i)
    consume(y: i, x: s) // expected-error{{argument 'x' must precede argument 'y'}}
}

// Ensure that we can still use a variable named consume.

func useConsumeVar(consume: inout String) {
    let s = consume
    consume = s

    // We can consume from a variable named `consume`
    let t = consume consume
    consume = t

    // We can do member access and subscript a variable named `consume`
    let i = consume.startIndex
    let _ = consume[i]
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
    
    func consumeSelf() {
        _ = consume self
    }

    func consumePropertyWrapper() {
        // should still parse, even if it doesn't semantically work out
        _ = consume wrapperTest // expected-error{{can only be applied to a local binding ('let', 'var', or parameter)}}
        _ = consume _wrapperTest // expected-error{{can only be applied to a local binding ('let', 'var', or parameter)}}
        _ = consume $wrapperTest // expected-error{{can only be applied to a local binding ('let', 'var', or parameter)}}
    }
}

func testParseConsumeWithDollarIdentifier() {
  class Klass {}
  let f: (Klass) -> () = {
    let _ = consume $0
  }
  _ = f
}

class ParentKlass {}
class ChildKlass : ParentKlass {}

func testAsBindingVariableInSwitch(_ x: ChildKlass) {
  switch x {
  case let consume as ParentKlass:
    _ = consume
    break
  }
}
