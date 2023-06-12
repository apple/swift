// RUN: %target-typecheck-verify-swift -disable-availability-checking

// MARK: Functions

func foo() -> Int {
  switch Bool.random() {
  case true:
    1
  case false:
    2
  }
}

func foo2() -> Int {
  return switch Bool.random() {
  case true:
    1
  case false:
    2
  }
}

func foo3() -> Int {
  switch Bool.random() {
  case true:
    1
  case false:
    2
  } as Int
}

func foo4() -> Int {
  return switch Bool.random() {
  case true:
    1
  case false:
    2
  } as Int
}

func foo5() -> Int {
  // We only allow coercions as a narrow case in the parser, so attempting to
  // double them up is invalid.
  switch Bool.random() {
  case true:
    1
  case false:
    2
  } as Int as Int
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-2 {{expected expression}}
}


func foo6() -> Int {
  let x = switch Bool.random() {
  case true:
    1
  case false:
    2
  } as Int as Int
  return x
}

func foo7() -> String {
  switch Bool.random() {
  case true:
    1 // expected-error {{cannot convert value of type 'Int' to specified type 'String'}}
  case false:
    2
  } as String
}

func foo8() -> Int {
  return (switch Bool.random() { case true: 1 case false: 2 } as Int)
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
}

func foo9() -> String? {
  switch Bool.random() { // expected-error {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
  case true:
    1 as Any
  case false:
    2 as Any
  } as? String
}

func foo10() -> String {
  switch Bool.random() { // expected-error {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
  case true:
    1 as Any
  case false:
    2 as Any
  } as! String
}

func foo11() -> Bool {
  // We don't parse this.
  switch Bool.random() {
  case true:
    1 as Any // expected-error {{cannot convert value of type 'Any' to specified type 'Bool'}}
  case false:
    2 as Any
  } is String
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-2 {{expected expression}}
}

func foo12() -> Bool {
  let x = switch Bool.random() { // expected-error {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
  case true:
    1 as Any
  case false:
    2 as Any
  } is String
  return x
}

func bar() -> Int {
  switch Bool.random() {
  case true:
    fatalError()
  case false:
    2
  }
}

func baz() -> Int {
  switch Bool.random() {
  case true where switch Bool.random() { case true: false case false: true }:
    // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
    1
  case false where if .random() { true } else { false }:
    // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
    2
  default:
    3
  }
}

func qux() -> Int {
  switch Bool.random() {
  case true:
    "" // expected-error {{cannot convert value of type 'String' to specified type 'Int'}}
  case false:
    0
  }
}

func takesValue<T>(_ x: T) {}

// expected-error@+1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
takesValue(switch Bool.random() {
case true:
  1
case false:
  2
})
takesValue(switch Bool.random() { case true: 1 case false: 2 })
// expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

// Cannot parse labeled switch as expression.
do {
  takesValue(x: switch Bool.random() { case true: 1 case false: 2 })
  // expected-error@-1 {{extraneous argument label 'x:' in call}}
  // expected-error@-2 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

  takesValue(_: x: switch Bool.random() { case true: 1 case false: 2 })
  // expected-error@-1 {{expected expression in list of expressions}}
  // expected-error@-2 {{expected ',' separator}}
  // expected-error@-3 {{cannot find 'x' in scope}}
}
func takesValueWithLabel<T>(x: T) {}
do {
  takesValueWithLabel(x: switch Bool.random() { case true: 1 case false: 2 })
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

  takesValueWithLabel(x: y: switch Bool.random() { case true: 1 case false: 2 })
  // expected-error@-1 {{expected expression in list of expressions}}
  // expected-error@-2 {{expected ',' separator}}
  // expected-error@-3 {{cannot find 'y' in scope}}
}
func takesValueAndTrailingClosure<T>(_ x: T, _ fn: () -> Int) {}
takesValueAndTrailingClosure(switch Bool.random() { case true: 0 case false: 1 }) { 2 }
// expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

func takesInOut<T>(_ x: inout T) {}
takesInOut(&switch Bool.random() { case true: 1 case false: 2 })
// expected-error@-1 {{cannot pass immutable value of type 'Int' as inout argument}}
// expected-error@-2 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

struct HasSubscript {
  static subscript(x: Int) -> Void { () }
  subscript(x: Int...) -> Void { () }
}
HasSubscript[switch Bool.random() { case true: 1 case false: 2 }]
// expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

func proposalExample1(_ x: Unicode.Scalar) -> Int {
  switch x.value {
    case 0..<0x80: 1
    case 0x80..<0x0800: 2
    case 0x0800..<0x1_0000: 3
    default: 4
  }
}

func testNeverBranches1() -> Never {
  switch Bool.random() {
  case true:
    fatalError()
  case false:
    fatalError()
  }
}

func testNeverBranches2() {
  func bar<T>(_ fn: () -> T) {}
  bar {
    switch Bool.random() {
    case true:
      fatalError()
    case false:
      fatalError()
    }
  }
}

// MARK: Outer pound if

func withPoundIf() -> Int {
  #if true
  switch Bool.random() { case true: 0 case false: 1 }
  #endif
}

func withPoundIfClosure() -> Int {
  let fn = {
    #if true
    switch Bool.random() { case true: 0 case false: 1 }
    #endif
  }
  return fn()
}

func withPoundIfElse1() -> Int {
  #if true
  switch Bool.random() { case true: 0 case false: 1 }
  #else
  0
  #endif
}

func withPoundIfElse2() -> Int {
  #if true
  0
  #else
  switch Bool.random() { case true: 0 case false: 1 }
  #endif
}

func withPoundIfElseIf1() -> Int {
  #if true
  switch Bool.random() { case true: 0 case false: 1 }
  #elseif true
  0
  #endif
}


func withPoundIfElseIf2() -> Int {
  #if true
  0
  #elseif true
  switch Bool.random() { case true: 0 case false: 1 }
  #endif
}

func withPoundIfElseIfElse1() -> Int {
  #if true
  switch Bool.random() { case true: 0 case false: 1 }
  #elseif true
  0
  #else
  0
  #endif
}

func withPoundIfElseIfElse2() -> Int {
  #if true
  0
  #elseif true
  switch Bool.random() { case true: 0 case false: 1 }
  #else
  0
  #endif
}

func withPoundIfElseIfElse3() -> Int {
  #if true
  0
  #elseif true
  0
  #else
  switch Bool.random() { case true: 0 case false: 1 }
  #endif
}

func withVeryNestedPoundIf() -> Int {
  #if true
    #if true
      #if false
      ""
      #else
      switch Bool.random() { case true: 0 case false: 1 }
      #endif
    #elseif true
    0
    #endif
  #endif
}

func withVeryNestedPoundIfClosure() -> Int {
  let fn = {
    #if true
      #if true
        #if false
            ""
        #else
            switch Bool.random() { case true: 0 case false: 1 }
        #endif
      #elseif true
          0
      #endif
    #endif
  }
  return fn()
}

// MARK: Explicit returns

func explicitReturn1() -> Int {
  print("hello")
  return switch Bool.random() { case true: 1 case false: 2 }
}

func explicitReturn2() -> Int {
  return
  switch Bool.random() { case true: 1 case false: 2 }
  // expected-warning@-1 {{expression following 'return' is treated as an argument of the 'return'}}
  // expected-note@-2 {{indent the expression to silence this warning}}
}

func explicitReturn3() -> Int {
  return
    switch Bool.random() { case true: 1 case false: 2 }
}

func explicitReturn4() {
  // This used to be legal, but is now treated as a return of the if expression.
  return
    switch Bool.random() { case true: 1 case false: 2 }
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}
}

func explicitReturn5() {
  return;
  switch Bool.random() { case true: 1 case false: 2 } // expected-warning 2{{integer literal is unused}}
}

func explicitReturn6() {
  return ()
  switch Bool.random() { case true: 1 case false: 2 } // expected-warning 2{{integer literal is unused}}
}

struct AsPropertyInit {
  var x: Int = switch Bool.random() { case true: 1 case false: 0 }
  var y = switch Bool.random() { case true: 1 case false: 0 }
}

func testNestedAssignment() {
  var x = 0
  x = switch Bool.random() { case true: 0 case false: 1 } // Okay
  let fn = {
    x = switch Bool.random() { case true: 0 case false: 1 } // Also okay
  }

  // We don't allow in a nested assignment.
  // TODO: We could improve this error.
  print(x = switch Bool.random() { case true: 0 case false: 1 })
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

  _ = x; _ = fn
}

struct TestFailableInit {
  init?(_ x: Bool) {
    let y = switch x {
    case true:
      0
    case false:
      return nil // expected-error {{cannot 'return' in 'switch' when used as expression}}
    }
    _ = y
  }
}

struct TestFailableInitFatalError {
  init?(_ x: Int) {
    // In this case, the switch does not become an expression.
    switch x {
    case 0:
      fatalError()
    default:
      return nil
    }
  }
}

// MARK: Expressions

let a = switch Bool.random() {
case true:
  0
case false:
  1
}

let b = (switch Bool.random() { case true: 1 case false: 2 })
// expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

let c: (Int, k: Int) = (switch Bool.random() { case true: 1 case false: 2 },
                          k: switch Bool.random() { case true: 1 case false: 2 })
// expected-error@-2 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-2 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

let d = switch Bool.random() {
case true:
  switch Bool.random() {
  case true:
    1
  case false:
    2
  }
case false:
  3
}

let e = "\(switch Bool.random() { case true: 1 case false: 2 })"
// expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

let f = { switch Bool.random() { case true: 1 case false: 2 } }

func throwsError() throws {
  struct E: Error {}
  throw switch Bool.random() { case true: E() case false: E() }
}

// FIXME: If we ever support this, we need to fix the premature inference of '[Any]'/'[AnyHashable: Any]'.
// The issue is that we're attempting to bind defaults to type variables before solving the conjuction.
let g = [switch Bool.random() { case true: "a" case false: "b" }]
// expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-2 {{heterogeneous collection literal could only be inferred to '[Any]'; add explicit type annotation if this is intentional}}

let h = [switch Bool.random() { case true: 1 case false: 2 } : switch Bool.random() { case true: "a" case false: "b" }]
// expected-error@-1 2{{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-2 {{heterogeneous collection literal could only be inferred to '[AnyHashable : Any]'; add explicit type annotation if this is intentional}}

let i = [switch Bool.random() { case true: 1 case false: 2 }: switch Bool.random() { case true: "a" case false: "b" }]
// expected-error@-1 2{{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-2 {{heterogeneous collection literal could only be inferred to '[AnyHashable : Any]'; add explicit type annotation if this is intentional}}

let j = [switch Bool.random() { case true: 1 case false: 2 }:switch Bool.random() { case true: "a" case false: "b" }]
// expected-error@-1 2{{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-2 {{heterogeneous collection literal could only be inferred to '[AnyHashable : Any]'; add explicit type annotation if this is intentional}}

let k = [switch Bool.random() { case true: 1 case false: 2 } :switch Bool.random() { case true: "a" case false: "b" }]
// expected-error@-1 2{{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-2 {{heterogeneous collection literal could only be inferred to '[AnyHashable : Any]'; add explicit type annotation if this is intentional}}

let l = switch Bool.random() { case true: 1 case false: 2 } as Any

let _ = type(of: switch Bool.random() { case true: 1 case false: 2 })
// expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

do {
  switch Bool.random() { case true: 1 case false: 2 } = 3
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-2 {{expected expression}}
  // expected-warning@-3 2{{integer literal is unused}}
}

// We currently prefer to parse these as trailing closures. We may want to tell
// the user to just wrap the expression in parens.
do {
  _ = (switch fatalError() {}, 1) // expected-error {{expected '{' after 'switch' subject expression}}
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
  // expected-error@-2 {{extra trailing closure passed in call}}

  _ = (switch fatalError() { #if FOO
    // expected-error@-1 {{extra trailing closure passed in call}}
    // expected-error@-2 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
  #endif
  }, 0) // expected-error {{expected '{' after 'switch' subject expression}}

  _ = (switch Bool.random() { #if FOO
    // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
    // expected-error@-2 {{cannot pass immutable value of type '() -> ()' as inout argument}}
    // expected-error@-3 {{type '() -> ()' cannot conform to 'RandomNumberGenerator'}}
    // expected-note@-4 {{required by static method 'random(using:)' where 'T' = '() -> ()'}}
    // expected-note@-5 {{only concrete types such as structs, enums and classes can conform to protocols}}
  case true: // expected-error {{'case' label can only appear inside a 'switch' statement}}
    1
  case false: // expected-error {{'case' label can only appear inside a 'switch' statement}}
    2
  #endif
  }, 0) // expected-error {{expected '{' after 'switch' subject expression}}

  _ = (switch Bool.random() { #if FOO
    // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
    // expected-error@-2 {{cannot pass immutable value of type '() -> ()' as inout argument}}
    // expected-error@-3 {{type '() -> ()' cannot conform to 'RandomNumberGenerator'}}
    // expected-note@-4 {{required by static method 'random(using:)' where 'T' = '() -> ()'}}
    // expected-note@-5 {{only concrete types such as structs, enums and classes can conform to protocols}}
  case true: // expected-error {{'case' label can only appear inside a 'switch' statement}}
    1
  case false: // expected-error {{'case' label can only appear inside a 'switch' statement}}
    2
  #else
  case true: // expected-error {{'case' label can only appear inside a 'switch' statement}}
    1
  case false: // expected-error {{'case' label can only appear inside a 'switch' statement}}
    2
  #endif
  }, 0) // expected-error {{expected '{' after 'switch' subject expression}}
}

// These are syntatically okay because the #if starts on a newline. This seems
// like the common case.
_ = (switch Bool.random() {
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
  #if FOO
  #else
case true:
  2
case false:
  3
  #endif
}, 0)

_ = (switch Bool.random() {
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
  // expected-error@-2 {{switch must be exhaustive}}
  // expected-note@-3 {{add missing case: 'true'}}
  // expected-note@-4 {{add missing case: 'false'}}
  #if FOO
case true:
  0
case false:
  1
  #else
  #endif
}, 0)

_ = (switch Bool.random() {
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
  // expected-error@-2 {{switch must be exhaustive}}
  // expected-note@-3 {{add missing case: 'true'}}
  // expected-note@-4 {{add missing case: 'false'}}
  #if FOO
case true:
  0
case false:
  1
  #endif
}, 0)

_ = (switch fatalError() {
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
  #if FOO
  #endif
}, 0)

func testEmptySwitch() {
  let x = switch fatalError() {}
  // expected-warning@-1 {{constant 'x' inferred to have type 'Void', which may be unexpected}}
  // expected-note@-2 {{add an explicit type annotation to silence this warning}}

  func takesClosure<T>(_ fn: () -> T) -> T { fn() }
  let y = takesClosure { switch fatalError() {} }
  // expected-warning@-1 {{constant 'y' inferred to have type '()', which may be unexpected}}
  // expected-note@-2 {{add an explicit type annotation to silence this warning}}

  _ = x; _ = y
}

func testConditionalBinding1(_ x: Int?) -> Int {
  if let x = switch Bool.random() { case true: 0 case false: Int?.none } { // expected-error {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
    x
  } else {
    0
  }
}

func testConditionalBinding2(_ x: Int?) -> Int {
  if case let x? = switch Bool.random() { case true: 0 case false: Int?.none } { // expected-error {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
    x
  } else {
    0
  }
}

// MARK: Operators

let m = !switch Bool.random() { case true: true case false: true }
// expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

// FIXME: Shouldn't be ambiguous
let n = switch Bool.random() { case true: 1 case false: 2 } + // expected-error {{ambiguous use of operator '+'}}
        switch Bool.random() { case true: 3 case false: 4 } +
        switch Bool.random() { case true: 5 case false: 6 }
// expected-error@-3 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-3 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-3 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

let p = .random() ? switch Bool.random() { case true: 1 case false: 2 }
                  : switch Bool.random() { case true: 3 case false: 4 }
// expected-error@-2 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-2 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

let q = switch Bool.random() { case true: 1 case false: 2 }...switch Bool.random() { case true: 1 case false: 2 }
// expected-error@-1 2{{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

let r = switch Bool.random() { case true: 1 case false: 2 } ... switch Bool.random() { case true: 1 case false: 2 }
// expected-error@-1 2{{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

// MARK: Lookup

do {
  let s = switch Bool.random() { case true: s case false: 0 }
  // expected-error@-1 {{use of local variable 's' before its declaration}}
  // expected-note@-2 {{'s' declared here}}
}

// MARK: Postfix

// We don't allow postfix parsing.
do {
  let _ = switch Bool.random() { case true: [1] case false: [1, 2] }.count
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-2 {{reference to member 'count' cannot be resolved without a contextual type}}

  let _ = (switch Bool.random() { case true: [1] case false: [1, 2] }).count
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
}
do {
  let _ = switch Bool.random() { case true: Int?.none case false: 1 }?.bitWidth
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-2 {{expected expression}}

  // FIXME: The type error is likely due to not solving the conjunction before attempting default type var bindings.
  let _ = (switch Bool.random() { case true: Int?.none case false: 1 })?.bitWidth
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
  // expected-error@-2 {{type of expression is ambiguous without a type annotation}}
}
do {
  let _ = switch Bool.random() { case true: Int?.none case false: 1 }!
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-2 {{expected expression}}

  let _ = (switch Bool.random() { case true: Int?.none case false: 1 })!
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
}
do {
  func fn(_ x: Int...) {}

  let _ = switch Bool.random() { case true: fn case false: fn }(1, 2, 3)
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-warning@-2 {{expression of type '(Int, Int, Int)' is unused}}

  let _ = (switch Bool.random() { case true: fn case false: fn })(1, 2, 3)
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
}
func takesSubscript(_ x: HasSubscript) {
  let _ = switch Bool.random() { case true: x case false: x }[1, 2, 3]
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-warning@-2 {{expression of type '[Int]' is unused}}

  let _ = (switch Bool.random() { case true: x case false: x })[1, 2, 3]
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
}
do {
  func fn(_ fn: () -> Int) {}

  let _ = switch Bool.random() { case true: fn case false: fn } { 3 }
  // expected-error@-1 {{getter/setter can only be defined for a single variable}}

  let _ = (switch Bool.random() { case true: fn case false: fn }) { 3 }
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
}

// MARK: Statements

func stmts() {
  if switch Bool.random() { case true: true case false: true } {}
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

  if try switch Bool.random() { case true: true case false: true } {}
  // expected-error@-1 {{'try' may not be used on 'switch' expression}}
  // expected-error@-2 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

  // expected-error@+1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
  guard switch Bool.random() { case true: true case false: true } else {
    return
  }

  switch switch Bool.random() { case true: true case false: true } {
    // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
  case _ where switch Bool.random() { case true: true case false: true }:
    // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}
    break
  default:
    break
  }

  for b in [true] where switch b { case true: true case false: false } {}
  // expected-error@-1 {{'switch' may only be used as expression in return, throw, or as the source of an assignment}}

  // Make sure this doesn't parse as a switch expr pattern with a label.
  let x = 0
  switch 0 {
  case x: switch Bool.random() { case true: 1 case false: 2 }
    // expected-warning@-1 2{{integer literal is unused}}
  default:
    break
  }
}

// MARK: Non-expression branches

func returnBranches() -> Int {
  // This is not an expression because the branches are not expressions.
  switch Bool.random() {
  case true:
    return 0
  case false:
    return 1
  }
}

func returnBranches1() -> Int {
  return switch Bool.random() { // expected-error {{cannot convert return expression of type 'Void' to return type 'Int'}}
  case true:
    return 0 // expected-error {{cannot 'return' in 'switch' when used as expression}}
  case false:
    return 1 // expected-error {{cannot 'return' in 'switch' when used as expression}}
  }
}

func returnBranches2() -> Int {
  // We don't allow multiple expressions.
  switch Bool.random() {
  case true:
    print("hello")
    0 // expected-warning {{integer literal is unused}}
  case false:
    1 // expected-warning {{integer literal is unused}}
  }
}

func returnBranches3() -> Int {
  switch Bool.random() {
  case true:
    print("hello")
    return 0
  case false:
    1 // expected-warning {{integer literal is unused}}
  }
}

func returnBranches4() -> Int {
  switch Bool.random() {
  case true:
    return 1
  case false:
    0 // expected-warning {{integer literal is unused}}
  }
}

func returnBranches5() -> Int {
  let i = switch Bool.random() {
    // expected-warning@-1 {{constant 'i' inferred to have type 'Void', which may be unexpected}}
    // expected-note@-2 {{add an explicit type annotation to silence this warning}}
  case true:
    return 0 // expected-error {{cannot 'return' in 'switch' when used as expression}}
  case false:
    return 1 // expected-error {{cannot 'return' in 'switch' when used as expression}}
  }
  return i // expected-error {{cannot convert return expression of type 'Void' to return type 'Int'}}
}

func returnBranches6() -> Int {
  // We don't allow multiple expressions.
  let i = switch Bool.random() {
  case true:
    print("hello")
    0 // expected-warning {{integer literal is unused}}
    // expected-error@-1 {{non-expression branch of 'switch' expression may only end with a 'throw'}}
  case false:
    1
  }
  return i
}

func returnBranches6PoundIf() -> Int {
  // We don't allow multiple expressions.
  let i = switch Bool.random() {
  case true:
    #if true
    print("hello")
    0 // expected-warning {{integer literal is unused}}
    #endif
    // expected-error@-1 {{non-expression branch of 'switch' expression may only end with a 'throw'}}
  case false:
    1
  }
  return i
}

func returnBranches6PoundIf2() -> Int {
  // We don't allow multiple expressions.
  let i = switch Bool.random() {
  case true:
    #if false
    print("hello")
    0
    #endif
    // expected-error@-1 {{non-expression branch of 'switch' expression may only end with a 'throw'}}
  case false:
    1
  }
  return i
}

func returnBranches7() -> Int {
  let i = switch Bool.random() {
  case true:
    print("hello")
    return 0 // expected-error {{cannot 'return' in 'switch' when used as expression}}
  case false:
    1
  }
  return i
}

func returnBranches8() -> Int {
  let i = switch Bool.random() {
  case true:
    return 1 // expected-error {{cannot 'return' in 'switch' when used as expression}}
  case false:
    0
  }
  return i
}

func returnBranches9() -> Int {
  let i = switch Bool.random() {
  case true:
    print("hello")
    if .random() {} // expected-error {{non-expression branch of 'switch' expression may only end with a 'throw'}}
  case false:
    1
  }
  return i
}

func returnBranches10() -> Int {
  let i = switch Bool.random() {
  case true:
    print("hello")
    switch Bool.random() {
    case true:
      0 // expected-warning {{integer literal is unused}}
    case false:
      2 // expected-warning {{integer literal is unused}}
    } // expected-error {{non-expression branch of 'switch' expression may only end with a 'throw'}}
  case false:
    1
  }
  return i
}

func returnBranches11() -> Int {
  let i = switch Bool.random() {
  case true:
    print("hello")
    switch Bool.random() {
    case true:
      "" // expected-warning {{string literal is unused}}
    case false:
      2 // expected-warning {{integer literal is unused}}
    } // expected-error {{non-expression branch of 'switch' expression may only end with a 'throw'}}
  case false:
    1
  }
  return i
}

func returnBranches12() -> Int {
  switch Bool.random() {
  case true:
    print("hello")
    if .random() {}
  case false:
    1 // expected-warning {{integer literal is unused}}
  }
}

func returnBranches13() -> Int {
  switch Bool.random() {
  case true:
    print("hello")
    switch Bool.random() {
    case true:
      0 // expected-warning {{integer literal is unused}}
    case false:
      2 // expected-warning {{integer literal is unused}}
    }
  case false:
    1 // expected-warning {{integer literal is unused}}
  }
}

func returnBranches14() -> Int {
  switch Bool.random() {
  case true:
    print("hello")
    switch Bool.random() {
    case true:
      "" // expected-warning {{string literal is unused}}
    case false:
      2 // expected-warning {{integer literal is unused}}
    }
  case false:
    1 // expected-warning {{integer literal is unused}}
  }
}

func doStatementBranch() -> Int {
  switch Bool.random() {
  case true:
    0 // expected-warning {{integer literal is unused}}
  case false:
    do {}
  }
}

func genericReturnWhileTrueBranch() {
  enum E<T> {
    case x(T), y

    func foo() -> E<T> {
      switch self {
      case .x:
        while true {}
      case .y:
        fatalError()
      }
    }
  }
}

struct NestedInFailableInit {
  init?(_ b: Bool) {
    // This is okay, it's treated as a statement.
    switch b {
    case true:
      switch b {
      case true:
        return nil
      case false:
        fatalError()
      }
    case false:
      fatalError()
    }
  }
}

func nestedType() -> Int {
  switch Bool.random() {
  case true:
    struct S {
      var x: Int
    }
    return S(x: 0).x
  case false:
    0 // expected-warning {{integer literal is unused}}
  }
}

func testEmptyBranch() -> Int {
  // TODO: Ideally we wouldn't emit both diagnostics, the latter is the better
  // one, but the former is currently emitted by the parser. Ideally the former
  // one should become semantic, and we'd just avoid it for
  // SingleValueStmtExprs.
  let x = switch Bool.random() {
    case true:
    // expected-error@-1 {{'case' label in a 'switch' must have at least one executable statement}}
    // expected-error@-2:14 {{expected expression in branch of 'switch' expression}}
    case false:
    0
  }
  return x
}

// MARK: Pound if branches

func testPoundIfBranch1() -> Int {
  switch Bool.random() {
  case true:
    #if true
    0
    #endif
  case false:
    0
  }
}

func testPoundIfBranch2() -> Int {
  switch Bool.random() {
  case true:
    #if false
    0
    #endif
  case false:
    0 // expected-warning {{integer literal is unused}}
  }
}

func testPoundIfBranch3() -> Int {
  let x = switch Bool.random() {
  case true:
    #if false
    0
    #endif
  // expected-error@-1 {{non-expression branch of 'switch' expression may only end with a 'throw'}}
  case false:
    0
  }
  return x
}

func testPoundIfBranch4() -> Int {
  switch Bool.random() {
  case true:
    #if true
    0
    #endif
  case false:
    #if true
    0
    #endif
  }
}

func testPoundIfBranch5() -> Int {
  // Not allowed (matches the behavior of implict expression returns)
  switch Bool.random() {
  case true:
    #if false
    0
    #endif
    0 // expected-warning {{integer literal is unused}}
  case false:
    1 // expected-warning {{integer literal is unused}}
  }
}

func testPoundIfBranch6() -> Int {
  // Not allowed (matches the behavior of implict expression returns)
  let x = switch Bool.random() {
  case true:
    #if false
    0
    #endif
    0 // expected-warning {{integer literal is unused}}
    // expected-error@-1 {{non-expression branch of 'switch' expression may only end with a 'throw'}}
  case false:
    1
  }
  return x
}

func testPoundIfBranch7() -> Int {
  switch Bool.random() {
  case true:
    #if true
      #if true
        #if false
        ""
        #else
        0
        #endif
      #elseif true
      ""
      #endif
    #endif
  case false:
    0
  }
}

func testPoundIfBranch8() -> Int {
  switch Bool.random() {
  case true:
    #if false
    0
    #else
    #if true
    switch Bool.random() { case true: 0 case false: 1 }
    #endif
    #endif
  case false:
    #if true
    switch Bool.random() { case true: 0 case false: 1 }
    #endif
  }
}

// MARK: Jumping

func break1() -> Int {
  switch true {
  case true:
    break
  case false:
    0 // expected-warning {{integer literal is unused}}
  }
  return 1
}

func fallthrough1() -> Int {
  switch true {
  case true:
    if .random() {
      fallthrough
    }
    return 1
  case false:
    0 // expected-warning {{integer literal is unused}}
  }
}

func fallthrough2() -> Int {
  let x = switch true {
  case true:
    if .random() {
      fallthrough
    }
    return 1 // expected-error {{cannot 'return' in 'switch' when used as expression}}
  case false:
    0
  }
  return x
}

func breakAfterNeverExpr() -> String {
  // We avoid turning this into a switch expression because of the 'break'.
  switch Bool.random() {
  case true:
    if .random() {
      fatalError() // or while true {}
      break
    }
    return ""
  case false:
    fatalError()
  }
}

func nonExhaustive() -> String {
  switch Bool.random() {
    // expected-error@-1 {{switch must be exhaustive}}
    // expected-note@-2 {{add missing case: 'false'}}
  case true:
    "hello"
  }
}

func nonExhaustiveInClosure() -> String {
  let fn = {
    switch Bool.random() {
      // expected-error@-1 {{switch must be exhaustive}}
      // expected-note@-2 {{add missing case: 'false'}}
    case true:
      "hello"
    }
  }
  return fn()
}

func breakToInner() -> Int {
  switch Bool.random() {
  case true:
    // These are fine, they're inner breaks.
    y: switch Bool.random() {
    case true:
      break
    case false:
      switch Bool.random() {
      case true: break y
      case false: break
      }
    }
    return 0
  case false:
    1 // expected-warning {{integer literal is unused}}
  }
}

func continueToInner() -> Int {
  switch Bool.random() {
  case true:
    // These are fine, they're inner breaks/continues.
    y: for x in [0] {
      for y in [""] {
        if x == 0 {
          break y
        } else if y == "hello" {
          continue y
        } else if .random() {
          continue
        } else {
          break
        }
      }
    }
    return 0
  case false:
    1  // expected-warning {{integer literal is unused}}
  }
}

// MARK: Effect specifiers

func trySwitch1() -> Int {
  try switch Bool.random() { case true: 0 case false: 1 }
  // expected-error@-1 {{'try' may not be used on 'switch' expression}}
}

func trySwitch2() -> Int {
  let x = try switch Bool.random() { case true: 0 case false: 1 }
  // expected-error@-1 {{'try' may not be used on 'switch' expression}}
  return x
}

func trySwitch3() -> Int {
  return try switch Bool.random() { case true: 0 case false: 1 }
  // expected-error@-1 {{'try' may not be used on 'switch' expression}}
}

func awaitSwitch1() async -> Int {
  await switch Bool.random() { case true: 0 case false: 1 }
  // expected-error@-1 {{'await' may not be used on 'switch' expression}}
}

func awaitSwitch2() async -> Int {
  let x = await switch Bool.random() { case true: 0 case false: 1 }
  // expected-error@-1 {{'await' may not be used on 'switch' expression}}
  return x
}

func awaitSwitch3() async -> Int {
  return await switch Bool.random() { case true: 0 case false: 1 }
  // expected-error@-1 {{'await' may not be used on 'switch' expression}}
}

func tryAwaitSwitch1() async throws -> Int {
  try await switch Bool.random() { case true: 0 case false: 1 }
  // expected-error@-1 {{'try' may not be used on 'switch' expression}}
  // expected-error@-2 {{'await' may not be used on 'switch' expression}}
}

func tryAwaitSwitch2() async throws -> Int {
  try await switch Bool.random() { case true: 0 case false: 1 } as Int
  // expected-error@-1 {{'try' may not be used on 'switch' expression}}
  // expected-error@-2 {{'await' may not be used on 'switch' expression}}
}
