// RUN: %target-typecheck-verify-swift -disable-availability-checking

let t1: (_: Int) = (_: 3)

// FIXME: diag::tuple_single_element should be diagnosed in the constraint system
// instead of MiscDiagnostics, which will allow the compiler to emit that
// error instead of bogus type mismatches for tuple expressions.
let t2: (x: Int) = (x: 3)
// expected-error@-1{{cannot create a single-element tuple with an element label}}
// expected-error@-2 {{cannot convert value of type '(x: Int)' to specified type 'Int'}}

let i1: Int = t1.0
// expected-error@-1 {{value of type 'Int' has no member '0'}}

let i2: Int = t2.x
// expected-error@-1 {{value of type 'Int' has no member 'x'}}

let m1: (_: Int).Type = (_: Int).self
let m2: (x: Int).Type = (x: Int).self
// expected-error@-1 2 {{cannot create a single-element tuple with an element label}}

struct S<each T> {
  var t: (repeat each T)
}

func packSubstitution() -> Int {
  let s = S<Int>(t: 1)
  return s.t
}
