// RUN: %target-typecheck-verify-swift -disable-availability-checking -enable-experimental-feature NoImplicitCopy

class Klass {
    var k: Klass? = nil
}

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

func testExprFailureLet() {
    let t = 5
    // Next line is parsed as move(t) + t
    let _ = consume t + t
}

func testExprFailureVar() {
    var t = 5
    t = 5
    // Next line is parsed as move(t) + t
    let _ = consume t + t
}

func letAddressOnly<T>(_ v: T) {
    let t = v
    let _ = consume t
}

struct StructWithField {
    var k: Klass? = nil
}

func testLetStructAccessField() {
    let t = StructWithField()
    let _ = consume t.k  // expected-error {{'consume' can only be applied to a local binding ('let', 'var', or parameter)}}
}

func testVarStructAccessField() {
    var t = StructWithField()
    t = StructWithField()
    let _ = consume t.k // expected-error {{'consume' can only be applied to a local binding ('let', 'var', or parameter)}}
}

func testLetClassAccessField() {
    let t = Klass()
    let _ = consume t.k  // expected-error {{'consume' can only be applied to a local binding ('let', 'var', or parameter)}}
}

func testVarClassAccessField() {
    var t = Klass()
    t = Klass()
    let _ = consume t.k // expected-error {{'consume' can only be applied to a local binding ('let', 'var', or parameter)}}
}

func testConsumeResultImmutable() {
  class Klass {}

  struct Test {
    var k = Klass()
    mutating func mutatingTest() {}
    func borrowingTest() {}
    consuming func consumingTest() {}
  }

  var t = Test()
  t.mutatingTest()
  consume t.borrowingTest() // expected-error {{'consume' can only be applied to a local binding ('let', 'var', or parameter)}}
  (consume t).borrowingTest()
  (consume t).consumingTest()
  (consume t).mutatingTest() // expected-error {{cannot use mutating member on immutable value of type 'Test'}}
  (consume t) = Test() // expected-error {{cannot assign to immutable expression of type 'Test'}}
  consume t = Test() // expected-error {{cannot assign to immutable expression of type 'Test'}}
}
