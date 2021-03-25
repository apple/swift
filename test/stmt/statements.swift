// RUN: %target-typecheck-verify-swift

/* block comments */
/* /* nested too */ */

func markUsed<T>(_ t: T) {}

func f1(_ a: Int, _ y: Int) {}
func f2() {}
func f3() -> Int {}

func invalid_semi() {
  ; // expected-error {{';' statements are not allowed}} {{3-5=}}
}

func nested1(_ x: Int) {
  var y : Int  
  func nested2(_ z: Int) -> Int {
    return x+y+z
  }
  
  _ = nested2(1)
}

func funcdecl5(_ a: Int, y: Int) {
  var x : Int

  // a few statements
  if (x != 0) {
    if (x != 0 || f3() != 0) {
      // while with and without a space after it.
      while(true) { 4; 2; 1 } // expected-warning 3 {{integer literal is unused}}
      while (true) { 4; 2; 1 } // expected-warning 3 {{integer literal is unused}}
    }
  }

  // Assignment statement.
  x = y
  (x) = y

  1 = x        // expected-error {{cannot assign to value: literals are not mutable}}
  (1) = x      // expected-error {{cannot assign to value: literals are not mutable}}
  "string" = "other"    // expected-error {{cannot assign to value: literals are not mutable}}
  [1, 1, 1, 1] = [1, 1] // expected-error {{cannot assign to immutable expression of type '[Int]}}
  1.0 = x               // expected-error {{cannot assign to value: literals are not mutable}}
  nil = 1               // expected-error {{cannot assign to value: literals are not mutable}}

  (x:1).x = 1 // expected-error {{cannot assign to immutable expression of type 'Int'}}
  var tup : (x:Int, y:Int)
  tup.x = 1
  _ = tup

  let B : Bool

  // if/then/else.
  if (B) {
  } else if (y == 2) {
  }

  // This diagnostic is terrible - rdar://12939553
  if x {}   // expected-error {{type 'Int' cannot be used as a boolean; test for '!= 0' instead}}

  if true {
    if (B) {
    } else {
    }
  }

  if (B) {
    f1(1,2)
  } else {
    f2()
  }

  if (B) {
    if (B) {
      f1(1,2)
    } else {
      f2()
    }
  } else {
    f2()
  }
  
  // while statement.
  while (B) {
  }

  // It's okay to leave out the spaces in these.
  while(B) {}
  if(B) {}
}

struct infloopbool {
  var boolValue: infloopbool {
    return self
  }
}

func infloopbooltest() {
  if (infloopbool()) {} // expected-error {{cannot convert value of type 'infloopbool' to expected condition type 'Bool'}}
}

// test "builder" API style
extension Int {
  static func builder() -> Int { }
  var builderProp: Int { return 0 }
  func builder2() {}
}
Int
  .builder()
  .builderProp
  .builder2()

struct SomeGeneric<T> {
  static func builder() -> SomeGeneric<T> { }
  var builderProp: SomeGeneric<T> { return .builder() }
  func builder2() {}
}
SomeGeneric<Int>
  .builder()
  .builderProp
  .builder2()


break // expected-error {{'break' is only allowed inside a loop, if, do, or switch}}
continue // expected-error {{'continue' is only allowed inside a loop}}
while true {
  func f() {
    break // expected-error {{'break' is only allowed inside a loop}}
    continue // expected-error {{'continue' is only allowed inside a loop}}
  }

  // Labeled if
  MyIf: if 1 != 2 {
    break MyIf
    continue MyIf  // expected-error {{'continue' cannot be used with if statements}}
    break          // break the while
    continue       // continue the while.
  }
}

// Labeled if
MyOtherIf: if 1 != 2 {
  break MyOtherIf
  continue MyOtherIf  // expected-error {{'continue' cannot be used with if statements}}
  break          // expected-error {{unlabeled 'break' is only allowed inside a loop or switch, a labeled break is required to exit an if}}
  continue       // expected-error {{'continue' is only allowed inside a loop}}
}

do {
  break  // expected-error {{unlabeled 'break' is only allowed inside a loop or switch, a labeled break is required to exit an if or do}}
}

func tuple_assign() {
  var a,b,c,d : Int
  (a,b) = (1,2)
  func f() -> (Int,Int) { return (1,2) }
  ((a,b), (c,d)) = (f(), f())
  _ = (a,b,c,d)
}

func missing_semicolons() {
  var w = 321
  func g() {}
  g() w += 1             // expected-error{{consecutive statements}} {{6-6=;}}
  var z = w"hello"    // expected-error{{consecutive statements}} {{12-12=;}} expected-warning {{string literal is unused}}
  class  C {}class  C2 {} // expected-error{{consecutive statements}} {{14-14=;}}
  struct S {}struct S2 {} // expected-error{{consecutive statements}} {{14-14=;}}
  func j() {}func k() {}  // expected-error{{consecutive statements}} {{14-14=;}}
}

//===--- Return statement.

return 42 // expected-error {{return invalid outside of a func}}

return // expected-error {{return invalid outside of a func}}

func NonVoidReturn1() -> Int {
  _ = 0
  return // expected-error {{non-void function should return a value}}
}

func NonVoidReturn2() -> Int {
  return + // expected-error {{unary operator cannot be separated from its operand}} {{11-1=}} expected-error {{expected expression in 'return' statement}}
}

func VoidReturn1() {
  if true { return }
  // Semicolon should be accepted -- rdar://11344875
  return; // no-error
}

func VoidReturn2() {
  return () // no-error
}

func VoidReturn3() {
  return VoidReturn2() // no-error
}

//===--- If statement.

func IfStmt1() {
  if 1 > 0 // expected-error {{expected '{' after 'if' condition}}
  _ = 42
}

func IfStmt2() {
  if 1 > 0 {
  } else // expected-error {{expected '{' or 'if' after 'else'}}
  _ = 42
}
func IfStmt3() {
  if 1 > 0 {
  } else 1 < 0 { // expected-error {{expected '{' or 'if' after 'else'; did you mean to write 'if'?}} {{9-9= if}}
    _ = 42
  } else {
  }
}

//===--- While statement.

func WhileStmt1() {
  while 1 > 0 // expected-error {{expected '{' after 'while' condition}}
  _ = 42
}

//===-- Do statement.
func DoStmt() {
  // This is just a 'do' statement now.
  do {
  }
}


func DoWhileStmt1() {
  do { // expected-error {{'do-while' statement is not allowed}}
  // expected-note@-1 {{did you mean 'repeat-while' statement?}} {{3-5=repeat}}
  // expected-note@-2 {{did you mean separate 'do' and 'while' statements?}} {{5-5=\n}}
  } while true
}

func DoWhileStmt2() {
  do {

  }
  while true {

  }
}

func LabeledDoStmt() {
  LABEL: { // expected-error {{labeled block needs 'do'}} {{10-10=do }}
  }
}

//===--- Repeat-while statement.

func RepeatWhileStmt1() {
  repeat {} while true

  repeat {} while false

  repeat { break } while true
  repeat { continue } while true
}

func RepeatWhileStmt2() {
  repeat // expected-error {{expected '{' after 'repeat'}} expected-error {{expected 'while' after body of 'repeat' statement}}
}

func RepeatWhileStmt4() {
  repeat {
  } while + // expected-error {{unary operator cannot be separated from its operand}} {{12-1=}} expected-error {{expected expression in 'repeat-while' condition}}
}

func brokenSwitch(_ x: Int) -> Int {
  switch x {
  case .Blah(var rep): // expected-error{{type 'Int' has no member 'Blah'}}
    return rep
  }
}

func switchWithVarsNotMatchingTypes(_ x: Int, y: Int, z: String) -> Int {
  switch (x,y,z) {
  case (let a, 0, _), (0, let a, _): // OK
    return a
  case (let a, _, _), (_, _, let a): // expected-error {{pattern variable bound to type 'String', expected type 'Int'}}
  // expected-warning@-1 {{case is already handled by previous patterns; consider removing it}}
    return a
  }
}

func breakContinue(_ x : Int) -> Int {

Outer:
  for _ in 0...1000 {

  Switch: // expected-error {{switch must be exhaustive}} expected-note{{do you want to add a default clause?}}
  switch x {
    case 42: break Outer
    case 97: continue Outer
    case 102: break Switch
    case 13: continue
    case 139: break   // <rdar://problem/16563853> 'break' should be able to break out of switch statements
    }
  }
  
  // <rdar://problem/16692437> shadowing loop labels should be an error
Loop:  // expected-note {{previously declared here}}
  for _ in 0...2 {
  Loop:  // expected-error {{label 'Loop' cannot be reused on an inner statement}}
    for _ in 0...2 {
    }
  }


  // <rdar://problem/16798323> Following a 'break' statement by another statement on a new line result in an error/fit-it
  switch 5 {
  case 5:
    markUsed("before the break")
    break
    markUsed("after the break")    // 'markUsed' is not a label for the break.
  default:
    markUsed("")
  }
  
  let x : Int? = 42
  
  // <rdar://problem/16879701> Should be able to pattern match 'nil' against optionals
  switch x { // expected-error {{switch must be exhaustive}}
  // expected-note@-1 {{missing case: '.some(_)'}}
  case .some(42): break
  case nil: break
  
  }
  
}


enum MyEnumWithCaseLabels {
  case Case(one: String, two: Int)
}

func testMyEnumWithCaseLabels(_ a : MyEnumWithCaseLabels) {
  // <rdar://problem/20135489> Enum case labels are ignored in "case let" statements
  switch a {
  case let .Case(one: _, two: x): break // ok
  case let .Case(xxx: _, two: x): break // expected-error {{tuple pattern element label 'xxx' must be 'one'}}
  // TODO: In principle, reordering like this could be supported.
  case let .Case(two: _, one: x): break // expected-error {{tuple pattern element label}}
  }
}


func test_guard(_ x : Int, y : Int??, cond : Bool) {
  
  // These are all ok.
  guard let a = y else {}
  markUsed(a)
  guard let b = y, cond else {}
  guard case let c = x, cond else {}
  guard case let Optional.some(d) = y else {}
  guard x != 4, case _ = x else { }


  guard let e, cond else {}    // expected-error {{variable binding in a condition requires an initializer}}
  guard case let f? : Int?, cond else {}    // expected-error {{variable binding in a condition requires an initializer}}

  // FIXME: Bring back the tailored diagnostic
  guard let g = y else {
    markUsed(g)  // expected-error {{cannot find 'g' in scope}}
  }

  guard let h = y, cond {}  // expected-error {{expected 'else' after 'guard' condition}} {{25-25=else }}


  guard case _ = x else {}  // expected-warning {{'guard' condition is always true, body is unreachable}}

  // SR-7567
  guard let outer = y else {
    // FIXME: Bring back the tailored diagnostic
    guard true else {
      print(outer) // expected-error {{cannot find 'outer' in scope}}
    }
  }
}

func test_is_as_patterns() {
  switch 4 {
  case is Int: break        // expected-warning {{'is' test is always true}}
  case _ as Int: break  // expected-warning {{'as' test is always true}}
  // expected-warning@-1 {{case is already handled by previous patterns; consider removing it}}
  case _: break // expected-warning {{case is already handled by previous patterns; consider removing it}}
  }
}

// <rdar://problem/21387308> Fuzzing SourceKit: crash in Parser::parseStmtForEach(...)
func matching_pattern_recursion() {
  switch 42 {
  case {  // expected-error {{expression pattern of type '() -> ()' cannot match values of type 'Int'}}
      for i in zs {
      }
  }: break
  }
}

// <rdar://problem/18776073> Swift's break operator in switch should be indicated in errors
func r18776073(_ a : Int?) {
  switch a {
    case nil:   // expected-error {{'case' label in a 'switch' should have at least one executable statement}} {{14-14= break}}
    case _?: break
  }
}

// <rdar://problem/22491782> unhelpful error message from "throw nil"
func testThrowNil() throws {
  throw nil  // expected-error {{cannot infer concrete Error for thrown 'nil' value}}
}


// rdar://problem/23684220
// Even if the condition fails to typecheck, save it in the AST anyway; the old
// condition may have contained a SequenceExpr.
func r23684220(_ b: Any) {
  if let _ = b ?? b {} // expected-warning {{left side of nil coalescing operator '??' has non-optional type 'Any', so the right side is never used}}
  // expected-error@-1 {{initializer for conditional binding must have Optional type, not 'Any'}}
}


// <rdar://problem/21080671> QoI: try/catch (instead of do/catch) creates silly diagnostics
func f21080671() {
  try {  // expected-error {{the 'do' keyword is used to specify a 'catch' region}} {{3-6=do}}
  } catch { }
  
  
  try {  // expected-error {{the 'do' keyword is used to specify a 'catch' region}} {{3-6=do}}
    f21080671()
  } catch let x as Int {
  } catch {
  }
}

// <rdar://problem/24467411> QoI: Using "&& #available" should fixit to comma
// https://twitter.com/radexp/status/694561060230184960
func f(_ x : Int, y : Int) {
  if x == y && #available(iOS 52, *) {}  // expected-error {{expected ',' joining parts of a multi-clause condition}} {{12-15=,}}
  if #available(iOS 52, *) && x == y {}  // expected-error {{expected ',' joining parts of a multi-clause condition}} {{27-30=,}}

  // https://twitter.com/radexp/status/694790631881883648
  if x == y && let _ = Optional(y) {}  // expected-error {{expected ',' joining parts of a multi-clause condition}} {{12-15=,}}
  if x == y&&let _ = Optional(y) {}  // expected-error {{expected ',' joining parts of a multi-clause condition}} {{12-14=,}}
}



// <rdar://problem/25178926> QoI: Warn about cases where switch statement "ignores" where clause
enum Type {
  case Foo
  case Bar
}
func r25178926(_ a : Type) {
  switch a { // expected-error {{switch must be exhaustive}}
  // expected-note@-1 {{missing case: '.Bar'}}
  case .Foo, .Bar where 1 != 100:
    // expected-warning @-1 {{'where' only applies to the second pattern match in this case}}
    // expected-note @-2 {{disambiguate by adding a line break between them if this is desired}} {{14-14=\n       }}
    // expected-note @-3 {{duplicate the 'where' on both patterns to check both patterns}} {{12-12= where 1 != 100}}
    break
  }

  switch a { // expected-error {{switch must be exhaustive}}
  // expected-note@-1 {{missing case: '.Bar'}}
  case .Foo: break
  case .Bar where 1 != 100: break
  }

  switch a { // expected-error {{switch must be exhaustive}}
  // expected-note@-1 {{missing case: '.Bar'}}
  case .Foo,  // no warn
       .Bar where 1 != 100:
    break
  }

  switch a { // expected-error {{switch must be exhaustive}}
  // expected-note@-1 {{missing case: '.Foo'}}
  // expected-note@-2 {{missing case: '.Bar'}}
  case .Foo where 1 != 100, .Bar where 1 != 100:
    break
  }
}

do {
  guard 1 == 2 else {
    break // expected-error {{unlabeled 'break' is only allowed inside a loop or switch, a labeled break is required to exit an if or do}}
  }
}

func fn(a: Int) {
  guard a < 1 else {
    break // expected-error {{'break' is only allowed inside a loop, if, do, or switch}}
  }
}

func fn(x: Int) {
  if x >= 0 {
    guard x < 1 else {
      guard x < 2 else {
        break // expected-error {{unlabeled 'break' is only allowed inside a loop or switch, a labeled break is required to exit an if or do}}
      }
      return
    }
  }
}

func bad_if() {
  if 1 {} // expected-error {{type 'Int' cannot be used as a boolean; test for '!= 0' instead}}
  if (x: false) {} // expected-error {{cannot convert value of type '(x: Bool)' to expected condition type 'Bool'}}
  if (x: 1) {} // expected-error {{cannot convert value of type '(x: Int)' to expected condition type 'Bool'}}
  if nil {} // expected-error {{'nil' is not compatible with expected condition type 'Bool'}}
}

// Typo correction for loop labels
for _ in [1] {
  break outerloop // expected-error {{cannot find label 'outerloop' in scope}}
  continue outerloop // expected-error {{cannot find label 'outerloop' in scope}}
}
while true {
  break outerloop // expected-error {{cannot find label 'outerloop' in scope}}
  continue outerloop // expected-error {{cannot find label 'outerloop' in scope}}
}
repeat {
  break outerloop // expected-error {{cannot find label 'outerloop' in scope}}
  continue outerloop // expected-error {{cannot find label 'outerloop' in scope}}
} while true

outerLoop: for _ in [1] { // expected-note {{'outerLoop' declared here}}
  break outerloop // expected-error {{cannot find label 'outerloop' in scope; did you mean 'outerLoop'?}} {{9-18=outerLoop}}
}
outerLoop: for _ in [1] { // expected-note {{'outerLoop' declared here}}
  continue outerloop // expected-error {{cannot find label 'outerloop' in scope; did you mean 'outerLoop'?}} {{12-21=outerLoop}}
}
outerLoop: while true { // expected-note {{'outerLoop' declared here}}
  break outerloop // expected-error {{cannot find label 'outerloop' in scope; did you mean 'outerLoop'?}} {{9-18=outerLoop}}
}
outerLoop: while true { // expected-note {{'outerLoop' declared here}}
  continue outerloop // expected-error {{cannot find label 'outerloop' in scope; did you mean 'outerLoop'?}} {{12-21=outerLoop}}
}
outerLoop: repeat { // expected-note {{'outerLoop' declared here}}
  break outerloop // expected-error {{cannot find label 'outerloop' in scope; did you mean 'outerLoop'?}} {{9-18=outerLoop}}
} while true
outerLoop: repeat { // expected-note {{'outerLoop' declared here}}
  continue outerloop // expected-error {{cannot find label 'outerloop' in scope; did you mean 'outerLoop'?}} {{12-21=outerLoop}}
} while true

outerLoop1: for _ in [1] { // expected-note {{did you mean 'outerLoop1'?}} {{11-20=outerLoop1}}
  outerLoop2: for _ in [1] { // expected-note {{did you mean 'outerLoop2'?}} {{11-20=outerLoop2}}
    break outerloop // expected-error {{cannot find label 'outerloop' in scope}}
  }
}
outerLoop1: for _ in [1] { // expected-note {{did you mean 'outerLoop1'?}} {{14-23=outerLoop1}}
  outerLoop2: for _ in [1] { // expected-note {{did you mean 'outerLoop2'?}} {{14-23=outerLoop2}}
    continue outerloop // expected-error {{cannot find label 'outerloop' in scope}}
  }
}
outerLoop1: while true { // expected-note {{did you mean 'outerLoop1'?}} {{11-20=outerLoop1}}
  outerLoop2: while true { // expected-note {{did you mean 'outerLoop2'?}} {{11-20=outerLoop2}}
    break outerloop // expected-error {{cannot find label 'outerloop' in scope}}
  }
}
outerLoop1: while true { // expected-note {{did you mean 'outerLoop1'?}} {{14-23=outerLoop1}}
  outerLoop2: while true { // expected-note {{did you mean 'outerLoop2'?}} {{14-23=outerLoop2}}
    continue outerloop // expected-error {{cannot find label 'outerloop' in scope}}
  }
}
outerLoop1: repeat { // expected-note {{did you mean 'outerLoop1'?}} {{11-20=outerLoop1}}
  outerLoop2: repeat { // expected-note {{did you mean 'outerLoop2'?}} {{11-20=outerLoop2}}
    break outerloop // expected-error {{cannot find label 'outerloop' in scope}}
  } while true
} while true
outerLoop1: repeat { // expected-note {{did you mean 'outerLoop1'?}} {{14-23=outerLoop1}}
  outerLoop2: repeat { // expected-note {{did you mean 'outerLoop2'?}} {{14-23=outerLoop2}}
    continue outerloop // expected-error {{cannot find label 'outerloop' in scope}}
  } while true
} while true

// Errors in case syntax
class
case, // expected-error {{expected identifier in enum 'case' declaration}} expected-error {{expected identifier after comma in enum 'case' declaration}} expected-error {{enum 'case' is not allowed outside of an enum}}
case  // expected-error {{expected identifier in enum 'case' declaration}} expected-error {{enum 'case' is not allowed outside of an enum}}
// NOTE: EOF is important here to properly test a code path that used to crash the parser
