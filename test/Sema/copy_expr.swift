// RUN: %target-typecheck-verify-swift -disable-availability-checking -enable-experimental-feature NoImplicitCopy

class Klass {
    var k: Klass? = nil
}

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

func testExprFailureLet() {
    let t = 5
    // Next line is parsed as move(t) + t
    let _ = copy t + t
}

func testExprFailureVar() {
    var t = 5
    t = 5
    // Next line is parsed as move(t) + t
    let _ = copy t + t
}

func letAddressOnly<T>(_ v: T) {
    let t = v
    let _ = copy t
}

struct StructWithField {
  var k: Klass? = nil
  var computedK: Klass? { nil }
}

func testLetStructAccessField() {
  let t = StructWithField()
  let _ = copy t.k // expected-error {{'copy' can only be applied to lvalues}}
}

func testLetStructAccessComputedField() {
    let t = StructWithField()
    let _ = copy t.computedK  // expected-error {{'copy' can only be applied to lvalues}}
}

func testVarStructAccessField() {
    var t = StructWithField()
    t = StructWithField()
    let _ = copy t.k // expected-error {{'copy' can only be applied to lvalues}}
}

func testLetClassAccessField() {
    let t = Klass()
    let _ = copy t.k  // expected-error {{'copy' can only be applied to lvalues}}
}

func testVarClassAccessField() {
    var t = Klass()
    t = Klass()
    let _ = copy t.k // expected-error {{'copy' can only be applied to lvalues}}
}

struct MoveOnly : ~Copyable {}

func testNoMoveOnlyCopy(_ x: borrowing MoveOnly) {
  let _ = copy x // expected-error {{'copy' cannot be applied to noncopyable types}}
}

func testCopyResultImmutable() {
  class Klass {}

  struct Test {
    var k = Klass()
    mutating func mutatingTest() {}
    func borrowingTest() {}
    consuming func consumingTest() {}
  }

  var t = Test()
  t.mutatingTest()
  copy t.borrowingTest() // expected-error {{'copy' can only be applied to lvalues}}
  (copy t).borrowingTest()
  (copy t).consumingTest()
  (copy t).mutatingTest() // expected-error {{cannot use mutating member on immutable value of type 'Test'}}
  (copy t) = Test() // expected-error {{cannot assign to immutable expression of type 'Test'}}
  copy t = Test() // expected-error {{cannot assign to immutable expression of type 'Test'}}
}
