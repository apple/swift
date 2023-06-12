// RUN: %target-typecheck-verify-swift  -disable-availability-checking -enable-experimental-move-only

class Klass {
    var k: Klass? = nil
}

func useString(_ x: String) {}

var global: String = "5"
func testGlobal() {
    useString(_borrow global)
}

func testLet() {
    let t = String()
    useString(_borrow t)
}

func testVar() {
    var t = String()
    t = String()
    useString(_borrow t)
}

struct S {
    var k = Klass()

    func test() {}

    func test2() {
        (_borrow self).test()
    }
}

func useApply() {
    var s = S()
    s = S()
    (_borrow s).test()
}

func testExprFailureLet() {
    let t = 5
    // Next line is parsed as move(t) + t
    let _ = _borrow t + t
    // Next line is parsed as move(t+t)
    let _ = _borrow (t+t) // expected-error {{'borrow' can only be applied to a local binding ('let', 'var', or parameter)}}
}

func testExprFailureVar() {
    var t = 5
    t = 5
    // Next line is parsed as move(t) + t
    let _ = _borrow t + t
    // Next line is parsed as move(t+t)
    let _ = _borrow (t+t) // expected-error {{'borrow' can only be applied to a local binding ('let', 'var', or parameter)}}
}

func letAddressOnly<T>(_ v: T) {
    let t = v
    let _ = _borrow t
}

struct StructWithField {
    var k: Klass? = nil
}

func testLetStructAccessField() {
    let t = StructWithField()
    let _ = _borrow t.k
}

func testVarStructAccessField() {
    var t = StructWithField()
    t = StructWithField()
    let _ = _borrow t.k
}

func testLetClassAccessField() {
    let t = Klass()
    let _ = _borrow t.k
}

func testVarClassAccessField() {
    var t = Klass()
    t = Klass()
    let _ = _borrow t.k // expected-error {{'borrow' can only be applied to a local binding ('let', 'var', or parameter)}}
}
