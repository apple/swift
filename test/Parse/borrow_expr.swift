// RUN: %target-typecheck-verify-swift  -disable-availability-checking -enable-experimental-move-only

var global: Int = 5
func testGlobal() {
    let _ = _borrow global
}

func testLet() {
    let t = String()
    let _ = _borrow t // expected-error {{}}
}

func testVar() {
    var t = String()
    t = String()
    let _ = _borrow t
}


