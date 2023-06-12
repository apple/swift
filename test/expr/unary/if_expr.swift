// RUN: %target-typecheck-verify-swift -disable-availability-checking

// MARK: Functions

func foo() -> Int {
  if .random() { 1 } else { 2 }
}

func foo2() -> Int {
  return if .random() { 1 } else { 2 }
}

func foo3() -> Int {
  if .random() { 1 } else { 2 } as Int
}

func foo4() -> Int {
  return if .random() { 1 } else { 2 } as Int
}

func foo5() -> Int {
  // We only allow coercions as a narrow case in the parser, so attempting to
  // double them up is invalid.
  if .random() { 1 } else { 2 } as Int as Int
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-2 {{expected expression}}
}

func foo6() -> Int {
  let x = if .random() { 1 } else { 2 } as Int as Int
  return x
}

func foo7() -> String {
  if .random() { 1 } else { 2 } as String
  // expected-error@-1 {{cannot convert value of type 'Int' to specified type 'String'}}
}

func foo8() -> Int {
  return (if .random() { 1 } else { 2 } as Int)
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
}

func foo9() -> String? {
  if .random() { 1 as Any } else { 2 as Any } as? String
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
}

func foo10() -> String {
  if .random() { 1 as Any } else { 2 as Any } as! String
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
}

func foo11() -> Bool {
  // We don't parse this.
  if .random() { 1 as Any } else { 2 as Any } is String
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-2 {{expected expression}}
  // expected-error@-3 {{cannot convert value of type 'Any' to specified type 'Bool'}}
}

func foo12() -> Bool {
  let x = if .random() { 1 as Any } else { 2 as Any } is String
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
  return x
}

func bar() -> Int {
  if .random() {
    fatalError()
  } else {
    2
  }
}

func baz() -> Int {
  if .random() {
    "" // expected-error {{cannot convert value of type 'String' to specified type 'Int'}}
  } else {
    0
  }
}

func qux(_ x: Int?) -> Int {
  if let x = x { x } else { 0 }
}

func quux(_ x: Int?) -> Int {
  if case let x? = x { x } else { 0 }
}

func elseIf(_ x: Int?) -> Int {
  if .random() {
    0
  } else if let x = x {
    x
  } else if .random() {
    1
  } else {
    7 + 8
  }
}

func takesValue<T>(_ x: T) {}

// expected-error@+1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
takesValue(if .random() {
  0
} else {
  1
})
takesValue(if .random() { 0 } else { 1 })
// expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

// Cannot parse labeled if as expression.
do {
  takesValue(x: if .random() { 0 } else { 1 })
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
  // expected-error@-2 {{extraneous argument label 'x:' in call}}

  takesValue(_: x: if .random() { 0 } else { 1 })
  // expected-error@-1 {{expected expression in list of expressions}}
  // expected-error@-2 {{expected ',' separator}}
  // expected-error@-3 {{cannot find 'x' in scope}}
}
func takesValueWithLabel<T>(x: T) {}
do {
  takesValueWithLabel(x: if .random() { 1 } else { 2 })
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

  takesValueWithLabel(x: y: if .random() { 1 } else { 2 })
  // expected-error@-1 {{expected expression in list of expressions}}
  // expected-error@-2 {{expected ',' separator}}
  // expected-error@-3 {{cannot find 'y' in scope}}
}
func takesValueAndTrailingClosure<T>(_ x: T, _ fn: () -> Int) {}
takesValueAndTrailingClosure(if .random() { 0 } else { 1 }) { 2 }
// expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

func takesInOut<T>(_ x: inout T) {}
takesInOut(&if .random() { 1 } else { 2 })
// expected-error@-1 {{cannot pass immutable value of type 'Int' as inout argument}}
// expected-error@-2 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

struct HasSubscript {
  static subscript(x: Int) -> Void { () }

  subscript(x: Int...) -> Void { () }
}
HasSubscript[if .random() { 1 } else { 2 }]
// expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

func proposalExample1(isRoot: Bool, count: Int, willExpand: Bool, maxDepth: Int) -> String {
  let bullet =
      if isRoot && (count == 0 || !willExpand) { "" }
      else if count == 0 { "- " }
      else if maxDepth <= 0 { "▹ " }
      else { "▿ " }
  return bullet
}

var inAComputedVar: String {
  if .random() { "a" } else { "b" }
}

// MARK: Outer pound if

func withPoundIf() -> Int {
  #if true
  if .random() { 0 } else { 1 }
  #endif
}

func withPoundIfClosure() -> Int {
  let fn = {
    #if true
    if .random() { 0 } else { 1 }
    #endif
  }
  return fn()
}

func withPoundIfElse1() -> Int {
  #if true
  if .random() { 0 } else { 1 }
  #else
  0
  #endif
}

func withPoundIfElse2() -> Int {
  #if true
  0
  #else
  if .random() { 0 } else { 1 }
  #endif
}

func withPoundIfElseIf1() -> Int {
  #if true
  if .random() { 0 } else { 1 }
  #elseif true
  0
  #endif
}


func withPoundIfElseIf2() -> Int {
  #if true
  0
  #elseif true
  if .random() { 0 } else { 1 }
  #endif
}

func withPoundIfElseIfElse1() -> Int {
  #if true
  if .random() { 0 } else { 1 }
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
  if .random() { 0 } else { 1 }
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
  if .random() { 0 } else { 1 }
  #endif
}

func withVeryNestedPoundIf() -> Int {
  #if true
    #if true
      #if false
      ""
      #else
      if .random() { 0 } else { 1 }
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
            if .random() { 0 } else { 1 }
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
  return if .random() { 0 } else { 1 }
}

func explicitReturn2() -> Int {
  return
  if .random() { 0 } else { 1 }
  // expected-warning@-1 {{expression following 'return' is treated as an argument of the 'return'}}
  // expected-note@-2 {{indent the expression to silence this warning}}
}

func explicitReturn3() -> Int {
  return
    if .random() { 0 } else { 1 }
}

func explicitReturn4() {
  // This used to be legal, but is now treated as a return of the if expression.
  return
    if .random() { 0 } else { 1 }
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}
}

func explicitReturn5() {
  return;
  if .random() { 0 } else { 1 } // expected-warning 2{{integer literal is unused}}
}

func explicitReturn6() {
  return ()
  if .random() { 0 } else { 1 } // expected-warning 2{{integer literal is unused}}
}

var explicitReturn7: String {
  return if .random() { "a" } else { "b" }
}

struct AsPropertyInit {
  var x: Int = if Bool.random() { 1 } else { 0 }
  var y = if .random() { 1 } else { 0 }
}

func testNestedAssignment() {
  var x = 0
  x = if .random() { 0 } else { 1 } // Okay
  let fn = {
    x = if .random() { 0 } else { 1 } // Also okay
  }

  // We don't allow in a nested assignment.
  // TODO: We could improve this error.
  print(x = if .random() { 0 } else { 1 })
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

  _ = x; _ = fn
}

struct TestFailableInit {
  init?(_ x: Bool) {
    let y = if x {
      0
    } else {
      return nil // expected-error {{cannot 'return' in 'if' when used as expression}}
    }
    _ = y
  }
}

struct TestFailableInitFatalError {
  init?() {
    // In this case, the if does not become an expression.
    if .random() {
      fatalError()
    } else {
      return nil
    }
  }
}

// MARK: Expressions

let a = if .random() {
  0
} else {
  1
}

let b = (if .random() { 1 } else { 2 })  // expected-error {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

let c = (if .random() { 1 } else { 2 }, k: if .random() { 1 } else { 2 })  // expected-error 2{{'if' may only be used as expression in return, throw, or as the source of an assignment}}

var d = if .random() { if .random() { 1 } else { 2 } } else { 3 }

d = if .random() { 0 } else { 1 }

let e = "\(if .random() { 1 } else { 2 })" // expected-error {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

let f = { if .random() { 1 } else { 2 } }

func throwsError() throws {
  struct E: Error {}
  throw if .random() { E() } else { E() }
}

// FIXME: If we ever support this, we need to fix the premature inference of '[Any]'/'[AnyHashable: Any]'.
// The issue is that we're attempting to bind defaults to type variables before solving the conjuction.
let g = [if .random() { "a" } else { "b" }]
// expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-2 {{heterogeneous collection literal could only be inferred to '[Any]'; add explicit type annotation if this is intentional}}

let h = [if .random() { 1 } else { 2 } : if .random() { "a" } else { "b" }]
// expected-error@-1 2{{'if' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-2 {{heterogeneous collection literal could only be inferred to '[AnyHashable : Any]'; add explicit type annotation if this is intentional}}

let i = [if .random() { 1 } else { 2 }: if .random() { "a" } else { "b" }]
// expected-error@-1 2{{'if' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-2 {{heterogeneous collection literal could only be inferred to '[AnyHashable : Any]'; add explicit type annotation if this is intentional}}

let j = [if .random() { 1 } else { 2 }:if .random() { "a" } else { "b" }]
// expected-error@-1 2{{'if' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-2 {{heterogeneous collection literal could only be inferred to '[AnyHashable : Any]'; add explicit type annotation if this is intentional}}

let k = [if .random() { 1 } else { 2 } :if .random() { "a" } else { "b" }]
// expected-error@-1 2{{'if' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-2 {{heterogeneous collection literal could only be inferred to '[AnyHashable : Any]'; add explicit type annotation if this is intentional}}

let l = if .random() { 1 } else { 2 } as Any

let _ = type(of: if .random() { 1 } else { 2 })  // expected-error {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

let _ = (if .random() { () } else { () }) // expected-error {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

let _ = if .random() { 0 } // expected-error {{'if' must have an unconditional 'else' to be used as expression}}
let _ = if .random() { 0 } else if .random() { 1 } // expected-error {{'if' must have an unconditional 'else' to be used as expression}}

func testNonExhaustiveInFunction() {
  if .random() { 0 } // expected-warning {{integer literal is unused}}
}

func testLabelRejection1() -> Int {
  // This was legal before, so remains legal.
  x: if .random() { 0 } else { 1 }
  // expected-warning@-1 2{{integer literal is unused}}
}

func testLabelRejection2() -> Int {
  // This was never legal, so reject.
  x: if .random() { 0 } else { 1 } as Int
  // expected-error@-1 {{'if' cannot have a jump label when used as expression}}
}

do {
  if .random() { 1 } else { 2 } = 3
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-2 {{expected expression}}
  // expected-warning@-3 2{{integer literal is unused}}}
}
let m: Void = if .random() { 1 } else { 2 } // expected-error {{cannot convert value of type 'Int' to specified type 'Void'}}
let n: Never = if .random() { 1 } else { 2 } // expected-error {{cannot convert value of type 'Int' to specified type 'Never'}}

func testConditionalBinding1(_ x: Int?) -> Int {
  if let x = if .random() { 0 } else { Int?.none } { // expected-error {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
    x
  } else {
    0
  }
}

func testConditionalBinding2(_ x: Int?) -> Int {
  if case let x? = if .random() { 0 } else { Int?.none } { // expected-error {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
    x
  } else {
    0
  }
}

// MARK: Operators

let o = !if .random() { true } else { false }  // expected-error {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

// FIXME: Shouldn't be ambiguous
let p = if .random() { 1 } else { 2 } + // expected-error {{ambiguous use of operator '+'}}
        if .random() { 3 } else { 4 } +
        if .random() { 5 } else { 6 }
// expected-error@-3 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-3 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-3 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

let q = .random() ? if .random() { 1 } else { 2 }
                  : if .random() { 3 } else { 4 }
// expected-error@-2 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
// expected-error@-2 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

let r = if .random() { 1 } else { 2 }...if .random() { 1 } else { 2 }
// expected-error@-1 2{{'if' may only be used as expression in return, throw, or as the source of an assignment}}

let s = if .random() { 1 } else { 2 } ... if .random() { 1 } else { 2 }
// expected-error@-1 2{{'if' may only be used as expression in return, throw, or as the source of an assignment}}

// MARK: Lookup

do {
  let t = if .random() { t } else { 0 }
  // expected-error@-1 {{use of local variable 't' before its declaration}}
  // expected-note@-2 {{'t' declared here}}
}

// MARK: Postfix

// We don't allow postfix parsing.
do {
  let _ = if .random() { [1] } else { [1, 2] }.count
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-2 {{reference to member 'count' cannot be resolved without a contextual type}}

  let _ = (if .random() { [1] } else { [1, 2] }).count
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
}
do {
  let _ = if .random() { Int?.none } else { 1 as Int? }?.bitWidth
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-2 {{expected expression}}

  // FIXME: The type error is likely due to not solving the conjunction before attempting default type var bindings.
  let _ = (if .random() { Int?.none } else { 1 as Int? })?.bitWidth
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
  // expected-error@-2 {{type of expression is ambiguous without a type annotation}}
}
do {
  let _ = if .random() { Int?.none } else { 1 as Int? }!
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-2 {{expected expression}}

  let _ = (if .random() { Int?.none } else { 1 as Int? })!
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
}
do {
  func takesInts(_ x: Int...) {}
  let _ = if .random() { takesInts } else { takesInts }(1, 2, 3)
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-warning@-2 {{expression of type '(Int, Int, Int)' is unused}}

  let _ = (if .random() { takesInts } else { takesInts })(1, 2, 3)
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
}
func testSubscriptPostfix(_ x: HasSubscript) {
  if .random() { x } else { x }[1, 2, 3]
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-warning@-2 {{expression of type 'HasSubscript' is unused}}
  // expected-warning@-3 {{expression of type '[Int]' is unused}}
  // expected-warning@-4 {{expression of type 'HasSubscript' is unused}}

  (if .random() { x } else { x })[1, 2, 3]
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
}
do {
  func takesClosure(_ fn: () -> Int) {}

  let _ = if .random() { takesClosure } else { takesClosure } { 3 }
  // expected-error@-1 {{getter/setter can only be defined for a single variable}}

  let _ = (if .random() { takesClosure } else { takesClosure }) { 3 }
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
}

// MARK: Statements

func stmts() {
  if if .random() { true } else { false } {}
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

  if try if .random() { true } else { false } {}
  // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
  // expected-error@-2 {{'try' may not be used on 'if' expression}}

  // expected-error@+1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
  guard if .random() { true } else { false } else {
    return
  }

  switch if .random() { true } else { false } { // expected-error {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
  case _ where if .random() { true } else { false }: // expected-error {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
    break
  default:
    break
  }

  for b in [true] where if b { true } else { false } {} // expected-error {{'if' may only be used as expression in return, throw, or as the source of an assignment}}

  // Make sure this doesn't parse as an if expr pattern with a label.
  let x = 0
  switch 0 {
  case x: if .random() { 1 } else { 2 }
    // expected-warning@-1 2{{integer literal is unused}}
  default:
    break
  }
}

// MARK: Non-expression branches

func noElse() -> Int {
  // Not an expression because no else.
  if .random() {
    0 // expected-warning {{integer literal is unused}}
  }
  1 // expected-warning {{integer literal is unused}}
}

func returnBranches() -> Int {
  // This is not an expression because the branches are not expressions.
  if .random() {
    return 0
  } else {
    return 1
  }
}

func returnBranches1() -> Int {
  return if .random() { // expected-error {{cannot convert return expression of type 'Void' to return type 'Int'}}
    return 0 // expected-error {{cannot 'return' in 'if' when used as expression}}
  } else {
    return 1 // expected-error {{cannot 'return' in 'if' when used as expression}}
  }
}

func returnBranches2() -> Int {
  // We don't allow multiple expressions.
  if .random() {
    print("hello")
    0 // expected-warning {{integer literal is unused}}
  } else {
    1 // expected-warning {{integer literal is unused}}
  }
}

func returnBranches3() -> Int {
  if .random() {
    print("hello")
    return 0
  } else {
    1 // expected-warning {{integer literal is unused}}
  }
}

func returnBranches4() -> Int {
  if .random() { return 1 } else { 0 } // expected-warning {{integer literal is unused}}
}

struct Err: Error {}

func returnBranches5() throws -> Int {
  let i = if .random() {
    // expected-warning@-1 {{constant 'i' inferred to have type 'Void', which may be unexpected}}
    // expected-note@-2 {{add an explicit type annotation to silence this warning}}
    return 0 // expected-error {{cannot 'return' in 'if' when used as expression}}
  } else {
    return 1 // expected-error {{cannot 'return' in 'if' when used as expression}}
  }
  let j = if .random() {
    // expected-warning@-1 {{constant 'j' inferred to have type 'Void', which may be unexpected}}
    // expected-note@-2 {{add an explicit type annotation to silence this warning}}
    throw Err()
  } else {
    throw Err()
  }
  return i // expected-error {{cannot convert return expression of type 'Void' to return type 'Int'}}
}

func returnBranches6() -> Int {
  // We don't allow multiple expressions.
  let i = if .random() {
    print("hello")
    0 // expected-warning {{integer literal is unused}}
  } else { // expected-error {{non-expression branch of 'if' expression may only end with a 'throw'}}
    1
  }
  return i
}

func returnBranches6PoundIf() -> Int {
  // We don't allow multiple expressions.
  let i = if .random() {
    #if true
    print("hello")
    0 // expected-warning {{integer literal is unused}}
    #endif
  } else { // expected-error {{non-expression branch of 'if' expression may only end with a 'throw'}}
    1
  }
  return i
}

func returnBranches6PoundIf2() -> Int {
  // We don't allow multiple expressions.
  let i = if .random() {
    #if false
    print("hello")
    0
    #endif
  } else { // expected-error {{non-expression branch of 'if' expression may only end with a 'throw'}}
    1
  }
  return i
}

func returnBranches7() -> Int {
  let i = if .random() {
    print("hello")
    return 0  // expected-error {{cannot 'return' in 'if' when used as expression}}
  } else {
    1
  }
  return i
}

func returnBranches8() -> Int {
  let i = if .random() { return 1 } else { 0 }  // expected-error {{cannot 'return' in 'if' when used as expression}}
  return i
}

func returnBranches9() -> Int {
  let i = if .random() {
    print("hello")
    if .random() {}
  } else { // expected-error {{non-expression branch of 'if' expression may only end with a 'throw'}}
    1
  }
  return i
}

func returnBranches10() -> Int {
  let i = if .random() {
    print("hello")
    if .random() {
      0 // expected-warning {{integer literal is unused}}
    } else {
      2 // expected-warning {{integer literal is unused}}
    }
  } else { // expected-error {{non-expression branch of 'if' expression may only end with a 'throw'}}
    1
  }
  return i
}

func returnBranches11() -> Int {
  let i = if .random() {
    print("hello")
    if .random() {
      "" // expected-warning {{string literal is unused}}
    } else {
      0 // expected-warning {{integer literal is unused}}
    }
  } else { // expected-error {{non-expression branch of 'if' expression may only end with a 'throw'}}
    1
  }
  return i
}

func returnBranches12() -> Int {
  if .random() {
    print("hello")
    if .random() {}
  } else {
    1 // expected-warning {{integer literal is unused}}
  }
}

func returnBranches13() -> Int {
  if .random() {
    print("hello")
    if .random() {
      0 // expected-warning {{integer literal is unused}}
    } else {
      2 // expected-warning {{integer literal is unused}}
    }
  } else {
    1 // expected-warning {{integer literal is unused}}
  }
}

func returnBranches14() -> Int {
  if .random() {
    print("hello")
    if .random() {
      "" // expected-warning {{string literal is unused}}
    } else {
      0 // expected-warning {{integer literal is unused}}
    }
  } else {
    1 // expected-warning {{integer literal is unused}}
  }
}

func nestedType() -> Int {
  if .random() {
    struct S {
      var x: Int
    }
    return S(x: 0).x
  } else {
    0 // expected-warning {{integer literal is unused}}
  }
}

func testEmptyBranch() -> Int {
  let x = if .random() {} else { 0 }
  // expected-error@-1:24 {{expected expression in branch of 'if' expression}}
  return x
}

// MARK: Pound if branches

func testPoundIfBranch1() -> Int {
  if .random() {
    #if true
    0
    #endif
  } else {
    0
  }
}

func testPoundIfBranch2() -> Int {
  if .random() {
    #if false
    0
    #endif
  } else {
    0 // expected-warning {{integer literal is unused}}
  }
}

func testPoundIfBranch3() -> Int {
  let x = if .random() {
    #if false
    0
    #endif
  } else { // expected-error {{non-expression branch of 'if' expression may only end with a 'throw'}}
    0
  }
  return x
}

func testPoundIfBranch4() -> Int {
  if .random() {
    #if true
    0
    #endif
  } else {
    #if true
    0
    #endif
  }
}

func testPoundIfBranch5() -> Int {
  // Not allowed (matches the behavior of implict expression returns)
  if .random() {
    #if false
    0
    #endif
    0 // expected-warning {{integer literal is unused}}
  } else {
    1 // expected-warning {{integer literal is unused}}
  }
}

func testPoundIfBranch6() -> Int {
  // Not allowed (matches the behavior of implict expression returns)
  let x = if .random() {
    #if false
    0
    #endif
    0 // expected-warning {{integer literal is unused}}
  } else {  // expected-error {{non-expression branch of 'if' expression may only end with a 'throw'}}
    1
  }
  return x
}

func testPoundIfBranch7() -> Int {
  if .random() {
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
  } else {
    0
  }
}

func testPoundIfBranch8() -> Int {
  if .random() {
    #if false
    0
    #else
    #if true
    if .random() { 0 } else { 1 }
    #endif
    #endif
  } else {
    #if true
    if .random() { 0 } else { 1 }
    #endif
  }
}

// MARK: Jumping

func break1() -> Int {
  switch true {
  case true:
    let j = if .random() {
      break // expected-error {{cannot 'break' in 'if' when used as expression}}
    } else {
      0
    }
    return j
  case false:
    return 0
  }
}

func continue1() -> Int {
  for _ in 0 ... 5 {
    let i = if true { continue } else { 1 }
    // expected-error@-1 {{cannot 'continue' in 'if' when used as expression}}
    return i
  }
}

func return1() -> Int {
  // Make sure we always reject a return.
  let i = if .random() {
    do {
      for _ in [0] {
        while true {
          switch 0 {
          default:
            return 0 // expected-error {{cannot 'return' in 'if' when used as expression}}
          }
        }
      }
    }
  } else {
    0
  }
  return i
}

func return2() throws -> Int {
  // In a nested function is okay though.
  let i = if .random() {
    func foo() { return }
    throw Err()
  } else {
    0
  }
  return i
}

func return3() throws -> Int {
  // A nested type is also okay.
  let i = if .random() {
    struct Nested {
      func foo() { return }
    }
    throw Err()
  } else {
    0
  }
  return i
}

func return4() throws -> Int {
  // A nested closure is also okay.
  let i = if .random() {
    let _ = { return }
    throw Err()
  } else {
    0
  }
  return i
}

// MARK: Effect specifiers

func tryIf1() -> Int {
  try if .random() { 0 } else { 1 }
  // expected-error@-1 {{'try' may not be used on 'if' expression}}
}

func tryIf2() -> Int {
  let x = try if .random() { 0 } else { 1 }
  // expected-error@-1 {{'try' may not be used on 'if' expression}}
  return x
}

func tryIf3() -> Int {
  return try if .random() { 0 } else { 1 }
  // expected-error@-1 {{'try' may not be used on 'if' expression}}
}

func awaitIf1() async -> Int {
  await if .random() { 0 } else { 1 }
  // expected-error@-1 {{'await' may not be used on 'if' expression}}
}

func awaitIf2() async -> Int {
  let x = await if .random() { 0 } else { 1 }
  // expected-error@-1 {{'await' may not be used on 'if' expression}}
  return x
}

func awaitIf3() async -> Int {
  return await if .random() { 0 } else { 1 }
  // expected-error@-1 {{'await' may not be used on 'if' expression}}
}

func tryAwaitIf1() async throws -> Int {
  try await if .random() { 0 } else { 1 }
  // expected-error@-1 {{'try' may not be used on 'if' expression}}
  // expected-error@-2 {{'await' may not be used on 'if' expression}}
}

func tryAwaitIf2() async throws -> Int {
  try await if .random() { 0 } else { 1 } as Int
  // expected-error@-1 {{'try' may not be used on 'if' expression}}
  // expected-error@-2 {{'await' may not be used on 'if' expression}}
}
