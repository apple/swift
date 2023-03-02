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
  useString(_borrow t) // expected-error {{'borrow' can only be applied to var bindings}}
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
    (_borrow self).test() // expected-error {{'borrow' can only be applied to var bindings}}
  }

  mutating func test3() {
    (_borrow self).test()
  }
}

func useApply() {
  var s = S()
  s = S()
  (_borrow s).test()
  (((_borrow s))).test()
}

func testExprFailureLet() {
  let t = 5
  // Next line is parsed as move(t) + t
  let _ = _borrow t + t // expected-error {{'borrow' can only be applied to var bindings}}
  // Next line is parsed as move(t+t)
  let _ = _borrow (t+t) // expected-error {{'borrow' can only be applied to var bindings}}
}

func testExprFailureVar() {
  var t = 5
  t = 5
  // Next line is parsed as move(t) + t
  let _ = _borrow t + t
  // Next line is parsed as move(t+t)
  let _ = _borrow (t+t) // expected-error {{'borrow' can only be applied to var bindings}}
}

func letAddressOnly<T>(_ v: T) {
  let t = v
  let _ = _borrow t // expected-error {{'borrow' can only be applied to var bindings}}
}

struct StructWithField {
  var k: Klass = Klass()

  var computedK : Klass {
    return k
  }

  var computedK2 : Klass {
    get {
      return k
    }
    set {}
  }

  var computedK3 : Klass {
    _read {
      yield k
    }
  }

  var computedK4 : Klass {
    _read {
      yield k
    }
    set {}
  }
}

func testLetStructAccessField() {
  let t = StructWithField()
  let _ = _borrow t.k // expected-error {{'borrow' can only be applied to var bindings}}
  let _ = _borrow t.computedK // expected-error {{'borrow' can only be applied to var bindings}}
  let _ = _borrow t.computedK2 // expected-error {{'borrow' can only be applied to var bindings}}
  let _ = _borrow (_borrow t).computedK
  // expected-error @-1:11 {{'borrow' can only be applied to var bindings}}
  // expected-error @-2:20 {{'borrow' can only be applied to var bindings}}
  let _ = _borrow (_borrow t).computedK2
  // expected-error @-1:11 {{'borrow' can only be applied to var bindings}}
  // expected-error @-2:20 {{'borrow' can only be applied to var bindings}}
}

func testVarStructAccessField() {
  var t = StructWithField()
  t = StructWithField()
  let _ = _borrow t.k

  let _ = (_borrow t).computedK
  let _ = (_borrow t).computedK2
  let _ = (_borrow t).computedK3
  let _ = (_borrow t).computedK4

  let _ = _borrow t.computedK
  let _ = _borrow t.computedK2
  let _ = _borrow t.computedK3
  let _ = _borrow t.computedK4

  let _ = _borrow (_borrow t).computedK
  let _ = _borrow (_borrow t).computedK2
  let _ = _borrow (_borrow t).computedK3
  let _ = _borrow (_borrow t).computedK4

}

func testLetClassAccessField() {
  let t = Klass()
  let _ = _borrow t.k // expected-error {{'borrow' can only be applied to var bindings}}
}

func testVarClassAccessField() {
  var t = Klass()
  t = Klass()
  let _ = _borrow t.k
}
