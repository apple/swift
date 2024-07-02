// RUN: %target-typecheck-verify-swift -enable-experimental-feature StaticExclusiveOnly

@_staticExclusiveOnly // expected-error {{@_staticExclusiveOnly can only be applied to noncopyable types}}
struct A {}

@_staticExclusiveOnly // OK
struct B: ~Copyable { // expected-note {{'B' is a non-mutable type}}
                      // expected-note@-1 {{'B' is a non-mutable type}}
                      // expected-note@-2 {{'B' is a non-mutable type}}
                      // expected-note@-3 {{'B' is a non-mutable type}}
                      // expected-note@-4 {{'B' is a non-mutable type}}
                      // expected-note@-5 {{'B' is a non-mutable type}}
                      // expected-note@-6 {{'B' is a non-mutable type}}
  mutating func change() { // expected-error {{type 'B' cannot have mutating function 'change()'}}
    print("123")
  }
}

let b0 = B() // OK

var b1 = B() // expected-error {{variable of type 'B' must be declared with a 'let'}}

class C {
  var b2 = B() // expected-error {{variable of type 'B' must be declared with a 'let'}}
  let b3 = B() // OK
}

struct D: ~Copyable {
  var b4 = B() // expected-error {{variable of type 'B' must be declared with a 'let'}}
  let b5 = B() // OK
}

func e(_: borrowing B) {} // OK

func f(_: inout B) {} // expected-error {{parameter of type 'B' must be declared as either 'borrowing' or 'consuming'}}

func g(_: (inout B) -> ()) {} // expected-error {{parameter of type 'B' must be declared as either 'borrowing' or 'consuming'}}

func h(_: (borrowing B) -> ()) {} // OK

func i() {
  let _: (Int, Int) -> Int = {
    var b6 = B() // expected-error {{variable of type 'B' must be declared with a 'let'}}
                 // expected-warning@-1 {{initialization of variable 'b6' was never used; consider replacing with assignment to '_' or removing it}}

    return $0 + $1
  }
}

@_staticExclusiveOnly // expected-error {{@_staticExclusiveOnly may only be used on 'struct' declarations}}
enum J {}

@_staticExclusiveOnly // expected-error {{@_staticExclusiveOnly may only be used on 'struct' declarations}}
class K {}

@_staticExclusiveOnly // expected-error {{@_staticExclusiveOnly may only be used on 'struct' declarations}}
func l() {}

@_staticExclusiveOnly // expected-error {{@_staticExclusiveOnly may only be used on 'struct' declarations}}
let m = 123

@_staticExclusiveOnly // expected-error {{@_staticExclusiveOnly may only be used on 'struct' declarations}}
protocol N {}

func o(_: consuming B) {} // OK

func p(_: (consuming B) -> ()) {} // OK

@_staticExclusiveOnly
struct Q<T>: ~Copyable {} // expected-note {{'Q<T>' is a non-mutable type}}

var r0 = Q<Int>() // expected-error {{variable of type 'Q<Int>' must be declared with a 'let'}}

protocol S {
  var t0: B { get } // OK

  var t1: B { get set } // expected-error {{varaible of type 'B' must not have a setter}}
}

protocol U: ~Copyable {
  var v: B { get } // OK
}

struct W: ~Copyable {}

extension W: U {
  var v: B { // expected-error {{variable of type 'B' must be declared with a 'let'}}
    B()
  }
}

struct X: ~Copyable, U {
  let v: B // OK
}
