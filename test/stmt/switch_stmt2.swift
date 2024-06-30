// RUN: %target-typecheck-verify-swift

enum E {
  case e1
  case e2
}

func foo1(e : E) {
  switch e { // expected-error{{switch must be exhaustive}}
  // expected-note@-1 {{add missing case: '.e2'}}
  case .e1: return
  }
}

func foo2(i : Int) {
  // expected-note@+1{{add a default clause}}{{+2:3-3=default:\n<#code#>\n}}
  switch i { // expected-error{{switch must be exhaustive}}
  case 1: return
  }
}


// Treat nil as .none and do not emit false
// non-exhaustive warning.
func testSwitchEnumOptionalNil(_ x: Int?) -> Int {
  switch x { // no warning
  case .some(_):
    return 1
  case nil:
    return -1
  }
}

// Do not emit false non-exhaustive warnings if both
// true and false are covered by the switch.
func testSwitchEnumBool(_ b: Bool, xi: Int) -> Int {
  var x = xi
  let Cond = b

  switch Cond { // no warning
  default:
    x += 1
  }

  switch Cond { // expected-error{{switch must be exhaustive}}
  // expected-note@-1 {{missing case: 'false'}}
  case true:
    x += 1
  }

  switch Cond { // expected-error{{switch must be exhaustive}}
  // expected-note@-1 {{missing case: 'true'}}
  case false:
    x += 1
  }

  switch Cond { // no warning
  case true:
    x += 1
  case false:
    x -= 1
  }

  return x
}

func testSwitchOptionalBool(_ b: Bool?, xi: Int) -> Int {
  var x = xi
  switch b { // No warning
  case .some(true):
    x += 1
  case .some(false):
    x += 1
  case .none:
    x -= 1
  }

  switch b { // expected-error{{switch must be exhaustive}}
  // expected-note@-1 {{add missing case: '.some(false)'}}
  case .some(true):
    x += 1
  case .none:
    x -= 1
  }

  return xi
}

// Do not emit false non-exhaustive warnings if both
// true and false are covered for a boolean element of a tuple.
func testSwitchEnumBoolTuple(_ b1: Bool, b2: Bool, xi: Int) -> Int {
  var x = xi
  let Cond = (b1, b2)

  switch Cond { // no warning
  default:
    x += 1
  }

  switch Cond { // expected-error{{switch must be exhaustive}}
  // expected-note@-1 {{add missing case: '(false, _)'}}
  // expected-note@-2 {{add missing case: '(_, false)'}}
  case (true, true):
    x += 1
  }

  switch Cond { // expected-error{{switch must be exhaustive}}
  // expected-note@-1 {{add missing case: '(true, _)'}}
  // expected-note@-2 {{add missing case: '(_, false)'}}
  case (false, true):
    x += 1
  }

  switch Cond { // no warning
  case (true, true):
    x += 1
  case (true, false):
    x += 1
  case (false, true):
    x -= 1
  case (false, false):
    x -= 1
  }
  
  return x
}

func non_fully_covered_switch(x: Int) -> Int {
  var x = x
  switch x { // expected-error{{switch must be exhaustive}}
  // expected-note@-1{{add a default clause}}
  case 0:
    x += 1
  case 3:
    x -= 1
  }
  return x
}

// Do not crash if another switch statement follows a fallthrough.
func fallthrough_not_last(i: Int) {
  switch i {
  case 1:
    fallthrough
    switch i {
    case 1: break
    default: break
    }
  default:
    break
  }
}

// rdar://117871338 - incorrect diagnostic - type of expression is ambiguous when member is missing.
func test_invalid_optional_chaining() {
  func test(_: (E) -> Void) {
  }

  enum E {
  case a
  case b
  }

  struct S {
    var prop: E
  }

  test {
    switch $0.prop? { // expected-error {{value of type 'E' has no member 'prop'}}
    case .a: break
    case .b: break
    }
  }
}
