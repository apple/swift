// RUN: %target-typecheck-verify-swift

enum E {
  case e
  case f
  case g(Int)
}

func testDotSyntax1() -> E {
  switch Bool.random() { case true: .e case false: .f }
}
func testDotSyntax2() -> E? {
  switch Bool.random() { case true: .e case false: .f }
}
func testDotSyntax3() -> E? {
  switch Bool.random() { case true: .e case false: .none }
}
func testDotSyntax4() -> Int {
  let i = switch Bool.random() { case true: 0 case false: .random() }
  // expected-error@-1 {{cannot infer contextual base in reference to member 'random'}}

  return i
}

let testVar1: E = switch Bool.random() { case true: .e case false: .f }
let testVar2: E? = switch Bool.random() { case true: .e case false: .f }
let testVar3: E? = switch Bool.random() { case true: .e case false: .none }
let testVar4: E? = switch Bool.random() { case true: nil case false: .e }

let testVar5 = switch Bool.random() { case true: 0 case false: 1.0 }
// expected-error@-1 {{branches have mismatching types 'Int' and 'Double'}}

let testVar6: Double = switch Bool.random() { case true: 0 case false: 1.0 }

let testVar7: Double = switch 0 {
  case 1: 0 + 1
  case 2: 1.0 + 3
  default: 9 + 0.0
}

let testContextualMismatch1: String = switch Bool.random() { case true: 1 case false: "" }
// expected-error@-1 {{cannot convert value of type 'Int' to specified type 'String'}}

let testContextualMismatch2: String = switch Bool.random() { case true: 1 case false: 2 }
// expected-error@-1 {{cannot convert value of type 'Int' to specified type 'String'}}

func proposalExample1(_ x: Int) -> Float {
  let y = switch x {
    case 0..<0x80: 1 // expected-error {{branches have mismatching types 'Int' and 'Double'}}
    case 0x80..<0x0800: 2.0
    case 0x0800..<0x1_0000: 3.0
    default: 4.5
  }
  return y
}

func proposalExample2(_ x: Int) -> Float {
  let y: Float = switch x {
    case 0..<0x80: 1
    case 0x80..<0x0800: 2.0
    case 0x0800..<0x1_0000: 3.0
    default: 4.5
  }
  return y
}

enum Node { case B, R }

enum Tree {
  indirect case node(Node, Tree, Tree, Tree)
  case leaf

  func proposalExample3(_ z: Tree, d: Tree) -> Tree {
    switch self {
    case let .node(.B, .node(.R, .node(.R, a, x, b), y, c), z, d):
        .node(.R, .node(.B,a,x,b),y,.node(.B,c,z,d))
    case let .node(.B, .node(.R, a, x, .node(.R, b, y, c)), z, d):
        .node(.R, .node(.B,a,x,b),y,.node(.B,c,z,d))
    case let .node(.B, a, x, .node(.R, .node(.R, b, y, c), z, d)):
        .node(.R, .node(.B,a,x,b),y,.node(.B,c,z,d))
    case let .node(.B, a, x, .node(.R, b, y, .node(.R, c, z, d))):
        .node(.R, .node(.B,a,x,b),y,.node(.B,c,z,d))
    default:
      self
    }
  }
}

enum F {
  case a(Int)
}

func overloadedWithGenericAndInt<T>(_ x: T) -> T { x }
func overloadedWithGenericAndInt(_ x: Int) -> Int { x }

struct S {
  var f: F
  mutating func foo() -> Int {
    switch f {
    case .a(let x):
      // Make sure we don't try and shrink, which would lead to trying to
      // type-check the switch early.
      overloadedWithGenericAndInt(x + x)
    }
  }
}

func testSingleCaseReturn(_ f: F) -> Int {
  switch f {
  case .a(let i): i
  }
}

func testSingleCaseReturnClosure(_ f: F) -> Int {
  let fn = {
    switch f {
    case .a(let i): i
    }
  }
  return fn()
}

func testWhereClause(_ f: F) -> Int {
  switch f {
  case let .a(x) where x.isMultiple(of: 2):
    return 0
  default:
    return 1
  }
}

func testNestedOptional() -> Int? {
  switch Bool.random() {
  case true:
    1
  case false:
    if .random() {
      1
    } else {
      nil
    }
  }
}

func testNestedOptionalSwitch() -> Int? {
  switch Bool.random() {
  case true:
    1
  case false:
    switch Bool.random() {
    case true:
      1
    case false:
      nil
    }
  }
}

func testNestedOptionalMismatch1() -> Int? {
  switch Bool.random() {
  case true:
    1
  case false:
    if .random() {
      1
    } else {
      "" // expected-error {{cannot convert value of type 'String' to specified type 'Int'}}
    }
  }
}


func testNestedOptionalMismatch2() -> Int {
  switch Bool.random() {
  case true:
    1
  case false:
    if .random() {
      1
    } else {
      // FIXME: Seems like we could do better here
      nil // expected-error {{cannot convert value of type 'ExpressibleByNilLiteral' to specified type 'Int'}}
    }
  }
}

func testAssignment() {
  var d: Double = switch Bool.random() { case true: 0 case false: 1.0 }
  d = switch Bool.random() { case true: 0 case false: 1 }
  _ = d
}

struct TestBadReturn {
  var y = switch Bool.random() { case true: return case false: 0 } // expected-error {{return invalid outside of a func}}
}

func testNil1(_ x: Bool) {
  let _ = switch x { case true: 42 case false: nil } // expected-error {{'nil' requires a contextual type}}
}
func testNil2(_ x: Bool) {
  let _ = switch x { case true: nil case false: 42 } // expected-error {{'nil' requires a contextual type}}
}

func testNil3(_ x: Bool) {
  // In this case, we allow propagating the type from the first branch into
  // later branches.
  let _: _? = switch x { case true: 42 case false: nil }
}
func testNil4(_ x: Bool) {
  // FIXME: Bad diagnostic (#63130)
  let _: _? = switch x { case true: nil case false: 42 }  // expected-error {{type of expression is ambiguous without a type annotation}}
}

enum G<T> {
  // expected-note@-1 {{arguments to generic parameter 'T' ('Double' and 'Int') are expected to be equal}}
  // expected-note@-2 {{'T' declared as parameter to type 'G'}}
  case x(T), y
}

func testUnboundGeneric1() -> G<Int> {
  let x: G = switch Bool.random() { case true: .x(5) case false: .x(1) }
  return x
}

func testUnboundGeneric2() -> G<Int> {
  let x: G = switch Bool.random() { case true: .x(5) case false: .y }
  return x
}

func testUnboundGeneric3() -> G<Int> {
  let x: G = switch Bool.random() { case true: .y case false: .y }
  // expected-error@-1 {{generic parameter 'T' could not be inferred}}
  return x
}

func testUnboundGeneric4() -> G<Int> {
  let x: G = switch Bool.random() { case true: .x(5.0) case false: .x(5.0) }
  return x
  // expected-error@-1 {{cannot convert return expression of type 'G<Double>' to return type 'G<Int>'}}
}

func testUnboundGeneric5() -> G<Int> {
  let x: G = switch Bool.random() { case true: .x(5) case false: .x(5.0) }
  // expected-error@-1 {{cannot convert value of type 'Double' to expected argument type 'Int'}}
  return x
}

func testNeverConversion1() -> Int {
  switch Bool.random() {
  case true:
    1
  case false:
    fatalError()
  }
}

func testNeverConversion2() -> Int {
  return switch Bool.random() {
  case true:
    1
  case false:
    fatalError()
  }
}

func testNeverConversion3() -> Int {
  switch Bool.random() {
  case true:
    1
  case false:
    if .random() {
      fatalError()
    } else {
      2
    }
  }
}

func testNeverConversion4() -> Int {
  return switch Bool.random() {
  case true:
    1
  case false:
    if .random() {
      fatalError()
    } else {
      2
    }
  }
}

func testNeverConversion5() -> Int {
  {
    switch Bool.random() {
    case true:
      1
    case false:
      if .random() {
        fatalError()
      } else {
        2
      }
    }
  }()
}

func testNeverConversion6(_ e: E) -> String {
  switch e {
  default:
    fatalError()
  }
}

func testVoidConversion() {
  func foo(_ fn: () -> Void) {}
  func bar<T>(_ fn: () -> T) {}

  // Okay for an implicit return, including nested as this preserves source
  // compatibility.
  foo {
    switch Bool.random() {
    case true:
      0 // expected-warning {{integer literal is unused}}
    case false:
      0 // expected-warning {{integer literal is unused}}
    }
  }
  foo {
    switch Bool.random() {
    case true:
      0 // expected-warning {{integer literal is unused}}
    case false:
      if .random() {
        0 // expected-warning {{integer literal is unused}}
      } else {
        0 // expected-warning {{integer literal is unused}}
      }
    }
  }
  foo {
    switch Bool.random() {
    default:
      0 // expected-warning {{integer literal is unused}}
    }
  }
  bar {
    switch Bool.random() {
    case true:
      0
    case false:
      0
    }
  }
  bar {
    switch Bool.random() {
    case true:
      0
    case false:
      if .random() {
        0
      } else {
        0
      }
    }
  }
  bar {
    switch Bool.random() {
    case true:
      ()
    case false:
      0 // expected-warning {{integer literal is unused}}
    }
  }
  bar {
    switch Bool.random() {
    case true:
      0 // expected-warning {{integer literal is unused}}
    case false:
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
    switch Bool.random() { case true: 0 case false: "" }
    // expected-warning@-1 {{integer literal is unused}}
    // expected-warning@-2 {{string literal is unused}}
  }
  bar {
    switch Bool.random() {
    case true:
      switch Bool.random() {
      case true:
        0 // expected-warning {{integer literal is unused}}
      case false:
        [0] // expected-warning {{expression of type '[Int]' is unused}}
      }
    case false:
      "" // expected-warning {{string literal is unused}}
    }
  }
  bar { () -> Void in
    switch Bool.random() { case true: 0 case false: "" }
    // expected-warning@-1 {{integer literal is unused}}
    // expected-warning@-2 {{string literal is unused}}
  }
  bar { () -> Void in
    switch Bool.random() {
    case true:
      switch Bool.random() {
      case true:
        0 // expected-warning {{integer literal is unused}}
      case false:
        [0] // expected-warning {{expression of type '[Int]' is unused}}
      }
    case false:
      "" // expected-warning {{string literal is unused}}
    }
  }
  bar { () -> Int in
    switch Bool.random() { case true: 0 case false: "" } // expected-error {{cannot convert value of type 'String' to specified type 'Int'}}
  }
  bar { () -> Int in
    switch Bool.random() {
    case true:
      switch Bool.random() {
      case true:
        0
      case false:
        [0] // expected-error {{cannot convert value of type '[Int]' to specified type 'Int'}}
      }
    case false:
      ""
    }
  }

  // Not okay for an explicit return.
  foo {
    return switch Bool.random() {
    case true:
      0 // expected-error {{cannot convert value of type 'Int' to specified type 'Void'}}
    case false:
      0
    }
  }
  foo {
    return switch Bool.random() {
    case true:
      0 // expected-error {{cannot convert value of type 'Int' to specified type 'Void'}}
    case false:
      if .random() {
        0
      } else {
        0
      }
    }
  }
  bar {
    return switch Bool.random() {
    case true:
      0
    case false:
      0
    }
  }
  bar {
    return switch Bool.random() {
    case true:
      0
    case false:
      if .random() {
        0
      } else {
        0
      }
    }
  }
  bar {
    return switch Bool.random() {
    case true:
      () // expected-error {{branches have mismatching types '()' and 'Int'}}
    case false:
      0
    }
  }
  bar {
    return switch Bool.random() {
    case true:
      0
    case false:
      if .random() {
        () // expected-error {{branches have mismatching types '()' and 'Int'}}
      } else {
        0
      }
    }
  }
  bar {
    return switch Bool.random() { case true: 0 case false: "" } // expected-error {{branches have mismatching types 'Int' and 'String'}}
  }
  bar {
    return switch Bool.random() {
    case true:
      switch Bool.random() {
      case true:
        0 // expected-error {{branches have mismatching types 'Int' and '[Int]'}}
      case false:
        [0]
      }
    case false:
      ""
    }
  }
}

struct SomeError: Error {}

func testThrowInference() {
  func notThrowing(_ fn: () -> Int) {}
  notThrowing { // expected-error {{invalid conversion from throwing function of type '() throws -> Int' to non-throwing function type '() -> Int'}}
    switch Bool.random() {
    case true:
      0
    case false:
      throw SomeError()
    }
  }

  @discardableResult
  func rethrowing<T>(_ fn: () throws -> T) rethrows -> T { try fn() }
  rethrowing {
    // expected-error@-1 {{call can throw, but it is not marked with 'try' and the error is not handled}}
    // expected-note@-2 {{call is to 'rethrows' function, but argument function can throw}}
    switch Bool.random() {
    case true:
      0
    case false:
      throw SomeError()
    }
  }
}

// MARK: Pound if

func testPoundIf1() -> Int {
  switch Bool.random() {
  case true:
    #if true
    0
    #else
    ""
    #endif
  case false:
    0
  }
}

func testPoundIf2() -> String {
  switch Bool.random() {
  case true:
    #if true
    0 // expected-error {{cannot convert value of type 'Int' to specified type 'String'}}
    #else
    ""
    #endif
  case false:
    ""
  }
}

func testPoundIf3() -> String {
  switch Bool.random() {
  case true:
    #if false
    0
    #else
    ""
    #endif
  case false:
    ""
  }
}

func testPoundIf4() -> String {
  let x = switch Bool.random() {
  case true:
    #if true
    0 // expected-error {{branches have mismatching types 'Int' and 'String'}}
    #else
    ""
    #endif
  case false:
    ""
  }
  return x
}

func testPoundIf5() -> String {
  let x = switch Bool.random() {
  case true:
    #if false
    0
    #else
    ""
    #endif
  case false:
    ""
  }
  return x
}

func testPoundIfClosure1() -> Int {
  let fn = {
    switch Bool.random() {
    case true:
      #if true
        0
      #else
        ""
      #endif
    case false:
      0
    }
  }
  return fn()
}

func testPoundIfClosure2() -> String {
  let fn: () -> String = {
    switch Bool.random() {
    case true:
      #if true
        0 // expected-error {{cannot convert value of type 'Int' to specified type 'String'}}
      #else
        ""
      #endif
    case false:
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
  let x = switch Bool.random() {
    case true:
      B()
    case false:
      C()
  }
  let y = .random() ? B() : C()
  if .random() {
    return x
  } else {
    return y
  }
}

// MARK: Opaque result types

protocol P {}
extension P {
  @available(macOS 10.15, iOS 13.0, tvOS 13.0, watchOS 6.0, *)
  func foo() -> some P { 0 }
}
extension Int : P {}
extension Never : P {}

@available(macOS 10.15, iOS 13.0, tvOS 13.0, watchOS 6.0, *)
func testOpaqueReturn1() -> some P {
  switch Bool.random() {
  case true:
    0
  case false:
    1
  }
}

@available(macOS 10.15, iOS 13.0, tvOS 13.0, watchOS 6.0, *)
func testOpaqueReturn2() -> some P {
  switch Bool.random() {
  case true:
    0
  case false:
    fatalError()
  }
}

@available(macOS 10.15, iOS 13.0, tvOS 13.0, watchOS 6.0, *)
func testOpaqueReturn3(_ x: Int) -> some P {
  switch Bool.random() {
  case true:
    return x.foo()
  case false:
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
  switch Bool.random() {
  case true:
    ""
  case false:
    1
  }
}

@Builder
func builderStaticMember() -> (Either<String, Int>, Double) {
  switch Bool.random() {
  case true:
    ""
  case false:
    1
  }
  .pi // This becomes a static member ref, not a member on an if expression.
}

@Builder
func builderNotPostfix() -> (Either<String, Int>, Bool) {
  switch Bool.random() { case true: "" case false: 1 } !.random() // expected-error {{consecutive statements on a line must be separated by ';'}}
}

@Builder
func builderWithBinding() -> Either<String, Int> {
  // Make sure the binding gets type-checked as an if expression, but the
  // other if block gets type-checked as a stmt.
  let str = switch Bool.random() {
    case true: "a"
    case false: "b"
  }
  if .random() {
    str
  } else {
    1
  }
}

func builderInClosure() {
  func build(@Builder _ fn: () -> Either<String, Int>) {}
  build {
    switch Bool.random() {
    case true:
      ""
    case false:
      1
    }
  }
}
