// RUN: %target-typecheck-verify-swift

func takeIntToInt(_ f: (Int) -> Int) { }
func takeIntIntToInt(_ f: (Int, Int) -> Int) { }

// Simple closures
func simple() {
  takeIntToInt({(x: Int) -> Int in
    return x + 1
  })
  takeIntIntToInt({(x: Int, y: Int) -> Int in
    return x + y
  })
}

// Closures with variadic argument lists
func variadic() {
  var f = {(start: Int, rest: Int...) -> Int in
    var result = start
    for x in rest {
      result += x
    }
    return result
  }
  _ = f(1)
  _ = f(1, 2)
  _ = f(1, 3)

  let D = { (Ss ...) in 1 } // expected-error{{'...' cannot be applied to a subpattern which is not explicitly typed}}, expected-error{{cannot infer type of closure parameter 'Ss' without a type annotation}}
}

// Closures with attributes in the parameter list.
func attrs() {
  _ = {(z: inout Int) -> Int in z }
}

// Closures with argument and parameter names.
func argAndParamNames() -> Int {
  let _: (_ x: Int, _ y: Int) -> Int = { (a x, b y) in x + y }  // expected-error 2 {{closure cannot have keyword arguments}}
  let f1: (_ x: Int, _ y: Int) -> Int = { (x, y) in x + y }
  _ = f1(1, 2)
  return f1(1, 2)
}
