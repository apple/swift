// RUN: %target-swift-frontend -emit-sil -primary-file %s -o /dev/null -verify

import Swift

// Tests for definite initialization of globals.

var x: Int         // expected-note {{variable defined here}}
                   // expected-note@-1 {{variable defined here}}
                   // expected-note@-2 {{variable defined here}}
                   // expected-note@-3 {{variable defined here}}

let y: Int         // expected-note {{constant defined here}}
                   // expected-note@-1 {{constant defined here}}
                   // expected-note@-2 {{constant defined here}}

// Test top-level defer.
defer { print(x) } // expected-error {{variable 'x' used in defer before being initialized}}

defer { print(y) } // expected-error {{constant 'y' used in defer before being initialized}}

// Test top-level functions.

func testFunc() {       // expected-error {{variable 'x' used by function definition before being initialized}}
  defer { print(x) }    // expected-warning {{'defer' statement at end of scope always executes immediately}}{{3-8=do}}
}

// Test top-level closures.

let clo: () -> Void = { print(y) } // expected-error {{constant 'y' captured by a closure before being initialized}}
clo()

({ () -> Void in print(x) })() // expected-error {{variable 'x' captured by a closure before being initialized}}

let clo2: () -> Void = { [x] in print(x) } // expected-error {{variable 'x' used before being initialized}}
clo2()

class C {
  var f = 0
}

var c: C  // expected-note {{variable defined here}}

let clo3: () -> Void = { [weak c] in  // expected-error {{variable 'c' used before being initialized}}
  guard let cref = c else{ return }
  print(cref)
}
clo3()

// Test inner functions.

func testFunc2() {    // expected-error {{constant 'y' used by function definition before being initialized}}
  func innerFunc() {
    print(y)
  }
}

// Test class initialization and methods.

var w: String  // expected-note {{variable defined here}}
               // expected-note@-1 {{variable defined here}}
               // expected-note@-2 {{variable defined here}}

class TestClass1 {
  let fld = w // expected-error {{variable 'w' used by function definition before being initialized}}
}

class TestClass2 {
  init() {        // expected-error {{variable 'w' used by function definition before being initialized}}
    print(w)
  }

  func bar() {    // expected-error {{variable 'w' used by function definition before being initialized}}
    print(w)
  }
}

// Test initialization of global variables of protocol type.

protocol P {
  var f: Int { get }
}

var p: P    // expected-note {{variable defined here}}

defer { print(p.f) } // expected-error {{variable 'p' used in defer before being initialized}}
