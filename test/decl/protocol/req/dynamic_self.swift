// RUN: %target-typecheck-verify-swift

protocol P {
  var p: Self { get }
  // expected-note@-1{{protocol requires property 'p' with type 'Self'}}
  // expected-note@-2{{protocol requires property 'p' with type 'EError'}}
  // expected-note@-3{{protocol requires property 'p' with type 'SError'}}
  subscript() -> Self { get }
  // expected-note@-1{{protocol requires subscript with type '() -> Self'}}
  // expected-note@-2{{protocol requires subscript with type '() -> EError'}}
  // expected-note@-3{{protocol requires subscript with type '() -> SError'}}
  func f() -> Self
  // expected-note@-1{{protocol requires function 'f()' with type '() -> Self'}}
  // expected-note@-2{{protocol requires function 'f()' with type '() -> EError'}}
  // expected-note@-3{{protocol requires function 'f()' with type '() -> SError'}}
}

func takesP(_: P) {} // OK

// Error: Missing witnesses.
class W : P {} // expected-error{{type 'W' does not conform to protocol 'P'}}

// Okay: Self method in class.
class X : P {
  var p: Self { self }
  subscript() -> Self { self }
  func f() -> Self { self }
}

class Y {
  var p: Self { self }
  subscript() -> Self { self }
  func f() -> Self { self }
}

class GX<T> : P {
  var p: Self { self }
  subscript() -> Self { self }
  func f() -> Self { self }
}

// Okay: dynamic Self method in superclass.
class Z : Y, P { }

// Error: Z2 conforms, but subclass would not.
class Z2 : P {
  var p: Z2 { self } //expected-error{{property 'p' in non-final class 'Z2' must specify type 'Self' to conform to protocol 'P'}}
  subscript() -> Z2 { self } //expected-error{{subscript 'subscript()' in non-final class 'Z2' must return 'Self' to conform to protocol 'P'}}
  func f() -> Z2 { self } // expected-error{{method 'f()' in non-final class 'Z2' must return 'Self' to conform to protocol 'P'}}
}

// Okay: struct conforms by returning itself
struct S : P {
  var p: S { self }
  subscript() -> S { self }
  func f() -> S { self }
}

struct GS<T> : P {
  var p: GS { self }
  subscript() -> GS { self }
  func f() -> GS { self }
}

struct SError : P { // expected-error{{type 'SError' does not conform to protocol 'P'}}
  var p: Int { 0 } // expected-note{{candidate has non-matching type 'Int'}}
  subscript() -> Int { 0 } // expected-note{{candidate has non-matching type '() -> Int'}}
  func f() -> Int { 0 } // expected-note{{candidate has non-matching type '() -> Int'}}
}

// Okay: enum conforms by returning itself
enum E : P {
  var p: E { self }
  subscript() -> E { self }
  func f() -> E { self }
}

enum GE<T> : P {
  var p: GE { self }
  subscript() -> GE { self }
  func f() -> GE { self }
}

enum EError : P { // expected-error{{type 'EError' does not conform to protocol 'P'}}
  var p: Int { 0 } // expected-note{{candidate has non-matching type 'Int'}}
  subscript() -> Int { 0 } // expected-note{{candidate has non-matching type '() -> Int'}}
  func f() -> Int { 0 } // expected-note{{candidate has non-matching type '() -> Int'}}
}
