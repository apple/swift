// RUN: %target-typecheck-verify-swift -disable-typo-correction

struct X { 
  var b = true, i = 17

  var d : Dictionary = [0 : "Zero", 1 : "One", 2 : "Two"]
}

func testX(_ x: inout X) {
  x.b = false
  x.i = 5
  x.d[3] = "Three"
}

struct Broken {
  var b = True // expected-error{{cannot find 'True' in scope}} 
}

