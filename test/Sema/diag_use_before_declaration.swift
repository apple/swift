// RUN: %target-typecheck-verify-swift

// SR-5163
func sr5163() {
  func foo(_ x: Int) -> Int? { return 1 }
  
  func fn() {
    let a = foo(c) // expected-error {{use of local variable 'c' before its declaration}}
    guard let b = a else { return }
    let c = b // expected-note {{'c' declared here}}
  }
}

// SR-7660
class C {
  var variable: Int?
  func f() {
    guard let _ = variable else { return }
    let variable = 1
  }
}

// SR-7517
func testExample() {
  let app = app2 // expected-error {{use of local variable 'app2' before its declaration}}
  let app2 = app // expected-note {{'app2' declared here}}
}

// SR-8447
func test_circular() {
  let obj = sr8447 // expected-error {{use of local variable 'sr8447' before its declaration}}
  let _ = obj.prop, sr8447 // expected-note {{'sr8447' declared here}} expected-error {{type annotation missing in pattern}}
}

//===----------------------------------------------------------------------===//
// Nested scope
//===----------------------------------------------------------------------===//

func nested_scope_1() {
  do {
    do {
      let _ = x // expected-error {{use of local variable 'x' before its declaration}}
      let x = 111 // expected-note {{'x' declared here}}
    }
    let x = 11
  }
  let x = 1
}

func nested_scope_3() {
  let x = 1
  do {
    do {
      let _ = x
      let x = 111
    }
    let x = 11
  }
}

//===----------------------------------------------------------------------===//
// Type scope
//===----------------------------------------------------------------------===//

class Ty {
  var v : Int?

  func fn() {
    let _ = v
    let v = 1
  }
}

//===----------------------------------------------------------------------===//
// File scope
//===----------------------------------------------------------------------===//

let g = 0

func file_scope_1() {
  let _ = g
  let g = 1
}

func file_scope_2() {
  let _ = gg // expected-error {{use of local variable 'gg' before its declaration}}
  let gg = 1 // expected-note {{'gg' declared here}}
}

//===----------------------------------------------------------------------===//
// Module scope
//===----------------------------------------------------------------------===//

func module_scope_1() {
  let _ = print // Legal use of func print declared in Swift Standard Library
  let print = "something"
}

func module_scope_2() {
  let _ = another_print // expected-error {{use of local variable 'another_print' before its declaration}}
  let another_print = "something" // expected-note {{'another_print' declared here}}
}
