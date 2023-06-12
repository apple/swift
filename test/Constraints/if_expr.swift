// RUN: %target-typecheck-verify-swift

enum E {
  case e
  case f
  case g(Int)
}

func testDotSyntax1() -> E {
  if .random() { .e } else { .f }
}
func testDotSyntax2() -> E? {
  if .random() { .e } else { .f }
}
func testDotSyntax3() -> E? {
  if .random() { .e } else { .none }
}
func testDotSyntax4() -> Int {
  let i = if .random() { 0 } else { .random() }
  // expected-error@-1 {{cannot infer contextual base in reference to member 'random'}}

  return i
}

let testVar1: E = if .random() { .e } else { .f }
let testVar2: E? = if .random() { .e } else { .f }
let testVar3: E? = if .random() { .e } else { .none }
let testVar4: E? = if .random() { nil } else { .e }

let testVar5 = if .random() { 0 } else { 1.0 }
// expected-error@-1 {{branches have mismatching types 'Int' and 'Double'}}

let testVar6: Double = if .random() { 0 } else { 1.0 }

let testVar7: Double = if .random() { 0 + 1 } else if .random() { 1.0 + 3 } else { 9 + 0.0 }

let testContextualMismatch1: String = if .random() { 1 } else { "" }
// expected-error@-1 {{cannot convert value of type 'Int' to specified type 'String'}}

let testContextualMismatch2: String = if .random() { 1 } else { 2 }
// expected-error@-1 {{cannot convert value of type 'Int' to specified type 'String'}}

let testMismatch1 = if .random() { 1 } else if .random() { "" } else { 0.0 }
// expected-error@-1 {{branches have mismatching types 'Int' and 'Double'}}
// expected-error@-2 {{branches have mismatching types 'String' and 'Double'}}

func testMismatch2() -> Double {
  let x = if .random() {
    0 // expected-error {{branches have mismatching types 'Int' and 'Double'}}
  } else if .random() {
    0
  } else {
    1.0
  }
  return x
}
func testOptionalBinding1(_ x: Int?) -> Int {
  if let x = x { x } else { 0 }
}
func testOptionalBinding2(_ x: Int?, _ y: Int?) -> Int {
  if let x = x, let y = y { x + y } else { 0 }
}
func testPatternBinding2(_ e: E) -> Int {
  if case .g(let i) = e { i } else { 0 }
}

func testTernary() -> Int {
  if .random() { .random() ? 1 : 0 } else { .random() ? 3 : 2 }
}

func testReturn() -> Int {
  if .random() {
    return 0
  } else {
    1 // expected-warning {{integer literal is unused}}
  }
}

func testNil1(_ x: Bool) {
  let _ = if x { 42 } else { nil } // expected-error {{'nil' requires a contextual type}}
}
func testNil2(_ x: Bool) {
  let _ = if x { nil } else { 42 } // expected-error {{'nil' requires a contextual type}}
}

func testNil3(_ x: Bool) {
  // In this case, we allow propagating the type from the first branch into
  // later branches.
  let _: _? = if x { 42 } else { nil }
}
func testNil4(_ x: Bool) {
  // FIXME: Bad diagnostic (#63130)
  let _: _? = if x { nil } else { 42 } // expected-error {{type of expression is ambiguous without a type annotation}}
}

enum F<T> {
  // expected-note@-1 {{arguments to generic parameter 'T' ('Double' and 'Int') are expected to be equal}}
  // expected-note@-2 {{'T' declared as parameter to type 'F'}}
  case x(T), y
}

func testUnboundGeneric1() -> F<Int> {
  let x: F = if .random() { .x(5) } else { .x(1) }
  return x
}

func testUnboundGeneric2() -> F<Int> {
  let x: F = if .random() { .x(5) } else { .y }
  return x
}

func testUnboundGeneric3() -> F<Int> {
  let x: F = if .random() { .y } else { .y }
  // expected-error@-1 {{generic parameter 'T' could not be inferred}}
  return x
}

func testUnboundGeneric4() -> F<Int> {
  let x: F = if .random() { .x(5.0) } else { .x(5.0) }
  return x
  // expected-error@-1 {{cannot convert return expression of type 'F<Double>' to return type 'F<Int>'}}
}

func testUnboundGeneric5() -> F<Int> {
  let x: F = if .random() { .x(5) } else { .x(5.0) }
  // expected-error@-1 {{cannot convert value of type 'Double' to expected argument type 'Int'}}
  return x
}

protocol Q {
  associatedtype X
}

struct SQ : Q {
  typealias X = String
}

func testAssociatedTypeReturn1() {
  func fn<T : Q>(_ fn: (T) -> T.X) {}
  fn { x in // expected-error {{cannot infer type of closure parameter 'x' without a type annotation}}
    if .random() { "" } else { "" }
  }
  fn { (x: SQ) in
    if .random() { "" } else { "" }
  }
  fn { (x: SQ) in
    if .random() { "" } else { 0 } // expected-error {{cannot convert value of type 'Int' to specified type 'SQ.X' (aka 'String')}}
  }
}

func testNeverConversion1() -> Int {
  if .random() {
    1
  } else {
    fatalError()
  }
}

func testNeverConversion2() -> Int {
  return if .random() {
    1
  } else {
    fatalError()
  }
}

func testNeverConversion3() -> Int {
  if .random() {
    1
  } else {
    if .random() {
      fatalError()
    } else {
      2
    }
  }
}

func testNeverConversion4() -> Int {
  return if .random() {
    1
  } else {
    if .random() {
      fatalError()
    } else {
      2
    }
  }
}

func testNeverConversion5() -> Int {
  {
    if .random() {
      1
    } else {
      if .random() {
        fatalError()
      } else {
        2
      }
    }
  }()
}

func testVoidConversion() {
  func foo(_ fn: () -> Void) {}
  func bar<T>(_ fn: () -> T) {}

  // Okay for an implicit return, including nested as this preserves source
  // compatibility.
  foo {
    if .random() {
      0 // expected-warning {{integer literal is unused}}
    } else {
      0 // expected-warning {{integer literal is unused}}
    }
  }
  foo {
    if .random() {
      0 // expected-warning {{integer literal is unused}}
    } else {
      if .random() {
        0 // expected-warning {{integer literal is unused}}
      } else {
        0 // expected-warning {{integer literal is unused}}
      }
    }
  }
  bar {
    if .random() {
      0
    } else {
      0
    }
  }
  bar {
    if .random() {
      0
    } else {
      if .random() {
        0
      } else {
        0
      }
    }
  }
  bar {
    if .random() {
      ()
    } else {
      0 // expected-warning {{integer literal is unused}}
    }
  }
  bar {
    if .random() {
      0 // expected-warning {{integer literal is unused}}
    } else {
      if .random() {
        ()
      } else {
        0 // expected-warning {{integer literal is unused}}
      }
    }
  }
  // We allow the branches to mismatch to preserve source compatibility.
  // (this example is silly, but this occurs in the wild for more innocuous
  //  things like branches that do set insertions and removals).
  bar {
    if .random() { 0 } else { "" }
    // expected-warning@-1 {{integer literal is unused}}
    // expected-warning@-2 {{string literal is unused}}
  }
  bar {
    if .random() {
      if .random() {
        0 // expected-warning {{integer literal is unused}}
      } else {
        [0] // expected-warning {{expression of type '[Int]' is unused}}
      }
    } else {
      "" // expected-warning {{string literal is unused}}
    }
  }
  bar { () -> Void in
    if .random() { 0 } else { "" }
    // expected-warning@-1 {{integer literal is unused}}
    // expected-warning@-2 {{string literal is unused}}
  }
  bar { () -> Void in
    if .random() {
      if .random() {
        0 // expected-warning {{integer literal is unused}}
      } else {
        [0] // expected-warning {{expression of type '[Int]' is unused}}
      }
    } else {
      "" // expected-warning {{string literal is unused}}
    }
  }
  bar { () -> Int in
    if .random() { 0 } else { "" } // expected-error {{cannot convert value of type 'String' to specified type 'Int'}}
  }
  bar { () -> Int in
    if .random() {
      if .random() {
        0
      } else {
        [0] // expected-error {{cannot convert value of type '[Int]' to specified type 'Int'}}
      }
    } else {
      ""
    }
  }

  // Not okay for an explicit return.
  foo {
    return if .random() {
      0 // expected-error {{cannot convert value of type 'Int' to specified type 'Void'}}
    } else {
      0
    }
  }
  foo {
    return if .random() {
      0 // expected-error {{cannot convert value of type 'Int' to specified type 'Void'}}
    } else {
      if .random() {
        0
      } else {
        0
      }
    }
  }
  bar {
    return if .random() {
      0
    } else {
      0
    }
  }
  bar {
    return if .random() {
      0
    } else {
      if .random() {
        0
      } else {
        0
      }
    }
  }
  bar {
    return if .random() {
      () // expected-error {{branches have mismatching types '()' and 'Int'}}
    } else {
      0
    }
  }
  bar {
    return if .random() {
      0
    } else {
      if .random() {
        () // expected-error {{branches have mismatching types '()' and 'Int'}}
      } else {
        0
      }
    }
  }
  bar {
    return if .random() { 0 } else { "" } // expected-error {{branches have mismatching types 'Int' and 'String'}}
  }
  bar {
    return if .random() {
      if .random() {
        0 // expected-error {{branches have mismatching types 'Int' and '[Int]'}}
      } else {
        [0]
      }
    } else {
      ""
    }
  }
}

func testReturnMismatch() {
  let _ = if .random() {
    return 1 // expected-error {{unexpected non-void return value in void function}}
    // expected-note@-1 {{did you mean to add a return type?}}
    // expected-error@-2 {{cannot 'return' in 'if' when used as expression}}
  } else {
    0
  }
}

func testOptionalGeneric() {
  func bar<T>(_ fn: () -> T?) -> T? { fn() }
  bar {
    if .random() {
      ()
    } else {
      ()
    }
  }
}

func testNestedOptional() -> Int? {
  if .random() {
    1
  } else {
    if .random() {
      1
    } else {
      nil
    }
  }
}

let neverVar = if .random() { fatalError() } else { fatalError() }
// expected-warning@-1 {{constant 'neverVar' inferred to have type 'Never'}}
// expected-note@-2 {{add an explicit type annotation to silence this warning}}

func testNonVoidToVoid() {
  if .random() { 0 } else { 1 } // expected-warning 2{{integer literal is unused}}
}

func uninferableNil() {
  let _ = if .random() { nil } else { 2.0 } // expected-error {{'nil' requires a contextual type}}
}

func testAssignment() {
  var d: Double = if .random() { 0 } else { 1.0 }
  d = if .random() { 0 } else { 1 }
  _ = d
}

struct TestBadReturn {
  var y = if .random() { return } else { 0 } // expected-error {{return invalid outside of a func}}
}

struct SomeError: Error {}

func testThrowInference() {
  func notThrowing(_ fn: () -> Int) {}
  notThrowing { // expected-error {{invalid conversion from throwing function of type '() throws -> Int' to non-throwing function type '() -> Int'}}
    if .random() {
      0
    } else {
      throw SomeError()
    }
  }

  @discardableResult
  func rethrowing<T>(_ fn: () throws -> T) rethrows -> T { try fn() }
  rethrowing {
    // expected-error@-1 {{call can throw, but it is not marked with 'try' and the error is not handled}}
    // expected-note@-2 {{call is to 'rethrows' function, but argument function can throw}}
    if .random() {
      0
    } else {
      throw SomeError()
    }
  }
}

// MARK: Pound if

func testPoundIf1() -> Int {
  if .random() {
    #if true
    0
    #else
    ""
    #endif
  } else {
    0
  }
}

func testPoundIf2() -> String {
  if .random() {
    #if true
    0 // expected-error {{cannot convert value of type 'Int' to specified type 'String'}}
    #else
    ""
    #endif
  } else {
    ""
  }
}

func testPoundIf3() -> String {
  if .random() {
    #if false
    0
    #else
    ""
    #endif
  } else {
    ""
  }
}

func testPoundIf4() -> String {
  let x = if .random() {
    #if true
    0 // expected-error {{branches have mismatching types 'Int' and 'String'}}
    #else
    ""
    #endif
  } else {
    ""
  }
  return x
}

func testPoundIf5() -> String {
  let x = if .random() {
    #if false
    0
    #else
    ""
    #endif
  } else {
    ""
  }
  return x
}

func testPoundIfClosure1() -> Int {
  let fn = {
    if .random() {
      #if true
        0
      #else
        ""
      #endif
    } else {
      0
    }
  }
  return fn()
}

func testPoundIfClosure2() -> String {
  let fn: () -> String = {
    if .random() {
      #if true
        0 // expected-error {{cannot convert value of type 'Int' to specified type 'String'}}
      #else
        ""
      #endif
    } else {
      ""
    }
  }
  return fn()
}

// MARK: Subtyping

class A {}
class B : A {}
class C : A {}

func testSubtyping1() -> A {
  // We can join to A.
  let x = if .random() { B() } else { C() }
  let y = .random() ? B() : C()
  if .random() {
    return x
  } else {
    return y
  }
}

// MARK: Opaque result types

protocol P {}
extension Int : P {}

@available(macOS 10.15, iOS 13.0, tvOS 13.0, watchOS 6.0, *)
func testOpaqueReturn1() -> some P {
  if .random() {
    0
  } else {
    1
  }
}

@available(macOS 10.15, iOS 13.0, tvOS 13.0, watchOS 6.0, *)
func testOpaqueReturn2() -> some P {
  if .random() {
    0
  } else {
    fatalError()
  }
}

// MARK: Result builders

enum Either<T, U> {
  case first(T), second(U)
}

@resultBuilder
struct Builder {
  static func buildBlock<T>(_ x: T) -> T { x }
  static func buildBlock<T, U>(_ x: T, _ y: U) -> (T, U) { (x, y) }

  static func buildEither<T, U>(first x: T) -> Either<T, U> { .first(x) }
  static func buildEither<T, U>(second x: U) -> Either<T, U> { .second(x) }

  static func buildExpression(_ x: Double) -> Double { x }
  static func buildExpression<T>(_ x: T) -> T { x }
}

@Builder
func singleExprBuilder() -> Either<String, Int> {
  if .random() {
    ""
  } else {
    1
  }
}

@Builder
func builderStaticMember() -> (Either<String, Int>, Double) {
  if .random() {
    ""
  } else {
    1
  }
  .pi // This becomes a static member ref, not a member on an if expression.
}

@Builder
func builderNotPostfix() -> (Either<String, Int>, Bool) {
  if .random() { "" } else { 1 } !.random() // expected-error {{consecutive statements on a line must be separated by ';'}}
}

@Builder
func builderWithBinding() -> Either<String, Int> {
  // Make sure the binding gets type-checked as an if expression, but the
  // other if block gets type-checked as a stmt.
  let str = if .random() { "a" } else { "b" }
  if .random() {
    str
  } else {
    1
  }
}

func builderInClosure() {
  func build(@Builder _ fn: () -> Either<String, Int>) {}
  build {
    if .random() {
      ""
    } else {
      1
    }
  }
}

// https://github.com/apple/swift/issues/63796
func testInvalidOptionalChainingInIfContext() {
  let v63796 = 1
  if v63796? {} // expected-error{{cannot use optional chaining on non-optional value of type 'Int'}}
}
