// RUN: %target-swift-frontend -emit-sil %s -verify

func doStuff(_ fn : @escaping () -> Int) {}
func doVoidStuff(_ fn : @escaping () -> ()) {}
func doNothing() {}
func doSomething() -> Int { return 0 }

class TestCaptureUse {
  var x = 42
  func method() -> Int {
    doVoidStuff({ [y = self] in doNothing() }) // expected-warning {{capture 'y' was never used}}
    doStuff({ [y = self] in doSomething() }) // expected-warning {{capture 'y' was never used}}
    
    doVoidStuff({ [self = self.x] in doNothing() }) // expected-warning {{capture 'self' was never used}}
    doStuff({ [self = self.x] in doSomething() }) // expected-warning {{capture 'self' was never used}}
    
    // FIXME: Do these warnings make sense?
    doVoidStuff({ [weak self] in doNothing() }) // expected-warning {{variable 'self' was written to, but never read}}
    doStuff({ [weak self] in doSomething() }) // expected-warning {{variable 'self' was written to, but never read}}
    
    let y = 1
    doVoidStuff { [y] in doNothing() } // expected-warning {{capture 'y' was never used}}
    doStuff { [y] in doSomething() } // expected-warning {{capture 'y' was never used}}

    return 42
  }
}

class SomeClass {
  var field : SomeClass?
  func foo() -> Int { return 0 }
}

func testCaptureBehavior(_ ptr : SomeClass) {
  // Test normal captures.
  weak var wv : SomeClass? = ptr
  unowned let uv : SomeClass = ptr
  unowned(unsafe) let uv1 : SomeClass = ptr
  unowned(safe) let uv2 : SomeClass = ptr
  doStuff { wv!.foo() }
  doStuff { uv.foo() }
  doStuff { uv1.foo() }
  doStuff { uv2.foo() }

  
  // Capture list tests
  let v1 : SomeClass? = ptr
  let v2 : SomeClass = ptr

  doStuff { [weak v1] in v1!.foo() }
  doStuff { [weak v1] in v1!.foo() }
  doStuff { [unowned v2] in v2.foo() }
  doStuff { [unowned(unsafe) v2] in v2.foo() }
  doStuff { [unowned(safe) v2] in v2.foo() }
  doStuff { [weak v1, weak v2] in v1!.foo() + v2!.foo() }

  var i: Int?  = 42
  // expected-warning @-1 {{variable 'i' was never mutated; consider changing to 'let' constant}}
  doStuff { [i] in i! }
}

extension SomeClass {
  func bar() {
    doStuff { [unowned self] in self.foo() }
    doStuff { [unowned xyz = self.field!] in xyz.foo() }
    doStuff { [weak xyz = self.field] in xyz!.foo() }
    doStuff { [weak self] in 42 } // expected-warning {{variable 'self' was written to, but never read}}
  }
}

// <rdar://problem/16955318> Observed variable in a closure triggers an assertion
var closureWithObservedProperty: () -> () = {
  var a: Int = 42 { // expected-warning {{variable 'a' was never used; consider replacing with '_' or removing it}}
  willSet {
    _ = "Will set a to \(newValue)"
  }
  didSet {
    _ = "Did set a with old value of \(oldValue)"
  }
  }
}

// <rdar://problem/22344208> QoI: Warning for unused capture list variable should be customized
class r22344208 {
  func f() {
    let q = 42
    let _: () -> Int = {
      [unowned self,  // expected-warning {{capture 'self' was never used}}
       q] in       // expected-warning {{capture 'q' was never used}}
      1 }
  }
}

func r21375863() {
  var width = 0 // expected-warning {{variable 'width' was never mutated}}
  var height = 0 // expected-warning {{variable 'height' was never mutated}}
  var bufs: [[UInt8]] = (0..<4).map { _ -> [UInt8] in  // expected-warning {{variable 'bufs' was never used}}
    [UInt8](repeating: 0, count: width*height)
  }
}

func sr3186<T, U>(_ f: (@escaping (@escaping (T) -> U) -> ((T) -> U))) -> ((T) -> U) {
    return { x in return f(sr3186(f))(x) }
}

class SR3186 {
  init() {
    // expected-warning@+1{{capture 'self' was never used}}
    let v = sr3186 { f in { [unowned self, f] x in x != 1000 ? f(x + 1) : "success" } }(0)
    print("\(v)")
  }
}
