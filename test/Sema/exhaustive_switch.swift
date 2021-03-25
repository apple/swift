// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -swift-version 5 -enable-library-evolution %S/Inputs/exhaustive_switch_testable_helper.swift -emit-module -o %t
// RUN: %target-typecheck-verify-swift -swift-version 5 -enable-library-evolution -I %t
// RUN: %target-typecheck-verify-swift -swift-version 4 -enable-library-evolution -enable-nonfrozen-enum-exhaustivity-diagnostics -I %t

import exhaustive_switch_testable_helper

func foo(a: Int?, b: Int?) -> Int {
  switch (a, b) {
  case (.none, _): return 1
  case (_, .none): return 2
  case (.some(_), .some(_)): return 3
  }
    
  switch (a, b) {
  case (.none, _): return 1
  case (_, .none): return 2
  case (_?, _?): return 3
  }
  
  switch Optional<(Int?, Int?)>.some((a, b)) {
  case .none: return 1
  case let (_, x?)?: return x
  case let (x?, _)?: return x
  case (.none, .none)?: return 0
  }
}

func bar(a: Bool, b: Bool) -> Int {
  switch (a, b) {
  case (false, false):
    return 1
  case (true, _):
    return 2
  case (false, true):
    return 3
  }
}


enum Result<T> {
  case Ok(T)
  case Error(Error)

  func shouldWork<U>(other: Result<U>) -> Int {
    switch (self, other) { // No warning
    case (.Ok, .Ok): return 1
    case (.Error, .Error): return 2
    case (.Error, _): return 3
    case (_, .Error): return 4
    }
  }
}

func parenthesized() {
  // SR-7492: Space projection needs to treat extra paren-patterns explicitly.
  let x: Result<(Result<Int>, String)> = .Ok((.Ok(1), "World"))
  switch x {
  case let .Error(e):
    print(e)
  case let .Ok((.Error(e), b)):
    print(e, b)
  case let .Ok((.Ok(a), b)): // No warning here.
    print(a, b)
  }
}

enum Foo {
  case A(Int)
  case B(Int)
}
func foo() {
  switch (Foo.A(1), Foo.B(1)) {
  case (.A(_), .A(_)):
    ()
  case (.B(_), _):
    ()
  case (_, .B(_)):
    ()
  }
  
  switch (Foo.A(1), Optional<(Int, Int)>.some((0, 0))) {
  case (.A(_), _):
    break
  case (.B(_), (let q, _)?):
    print(q)
  case (.B(_), nil):
    break
  }
}

class C {}

enum Bar {
  case TheCase(C?)
}

func test(f: Bar) -> Bool {
  switch f {
  case .TheCase(_?):
    return true
  case .TheCase(nil):
    return false
  }
}

func op(this : Optional<Bool>, other : Optional<Bool>) -> Optional<Bool> {
  switch (this, other) { // No warning
  case let (.none, w):
    return w
  case let (w, .none):
    return w
  case let (.some(e1), .some(e2)):
    return .some(e1 && e2)
  }
}

enum Threepeat {
  case a, b, c
}

func test3(x: Threepeat, y: Threepeat) {
  switch (x, y) { // expected-error {{switch must be exhaustive}}
  // expected-note@-1 {{add missing case: '(.a, .c)'}}
  case (.a, .a):
    ()
  case (.b, _):
    ()
  case (.c, _):
    ()
  case (_, .b):
    ()
  }
}

enum A {
  case A(Int)
  case B(Bool)
  case C
  case D
}

enum B {
  case A
  case B
}

func s(a: A, b: B) {
  switch (a, b) {
  case (.A(_), .A):
    break
  case (.A(_), .B):
    break

  case (.B(_), let b):
    break

  case (.C, _), (.D, _):
    break
  }
}


enum Grimble {
  case A
  case B
  case C
}

enum Gromble {
  case D
  case E
}

func doSomething(foo:Grimble, bar:Gromble) {
  switch(foo, bar) { // No warning
  case (.A, .D):
    break
  case (.A, .E):
    break
  case (.B, _):
    break
  case (.C, _):
    break
  }
}

enum E {
  case A
  case B
}

func f(l: E, r: E) {
  switch (l, r) {
  case (.A, .A):
    return
  case (.A, _):
    return
  case (_, .A):
    return
  case (.B, .B):
    return
  }
}

enum TestEnum {
  case A, B
}

func switchOverEnum(testEnumTuple: (TestEnum, TestEnum)) {

  switch testEnumTuple {
  case (_,.B):
    // Matches (.A, .B) and (.B, .B)
    break
  case (.A,_):
    // Matches (.A, .A)
    // Would also match (.A, .B) but first case takes precedent
    break
  case (.B,.A):
    // Matches (.B, .A)
    break
  }

}

func tests(a: Int?, b: String?) {
  switch (a, b) {
  case let (.some(n), _): print("a: ", n, "?")
  case (.none, _): print("Nothing", "?")
  }

  switch (a, b) {
  case let (.some(n), .some(s)): print("a: ", n, "b: ", s)
  case let (.some(n), .none): print("a: ", n, "Nothing")
  case (.none, _): print("Nothing")
  }

  switch (a, b) {
  case let (.some(n), .some(s)): print("a: ", n, "b: ", s)
  case let (.some(n), .none): print("a: ", n, "Nothing")
  case let (.none, .some(s)): print("Nothing", "b: ", s)
  case (.none, _): print("Nothing", "?")
  }

  switch (a, b) {
  case let (.some(n), .some(s)): print("a: ", n, "b: ", s)
  case let (.some(n), .none): print("a: ", n, "Nothing")
  case let (.none, .some(s)): print("Nothing", "b: ", s)
  case (.none, .none): print("Nothing", "Nothing")
  }

}

enum X {
  case Empty
  case A(Int)
  case B(Int)
}

func f(a: X, b: X) {
  switch (a, b) {
  case (_, .Empty): ()
  case (.Empty, _): ()

  case (.A, .A): ()
  case (.B, .B): ()

  case (.A, .B): ()
  case (.B, .A): ()

  }
}

func f2(a: X, b: X) {
  switch (a, b) {

  case (.A, .A): ()
  case (.B, .B): ()

  case (.A, .B): ()
  case (.B, .A): ()

  case (_, .Empty): ()
  case (.Empty, _): ()

  case (.A, .A): () // expected-warning {{case is already handled by previous patterns; consider removing it}}
  case (.B, .B): () // expected-warning {{case is already handled by previous patterns; consider removing it}}

  case (.A, .B): () // expected-warning {{case is already handled by previous patterns; consider removing it}}
  case (.B, .A): () // expected-warning {{case is already handled by previous patterns; consider removing it}}

  default: ()
  }
}

enum XX : Int {
  case A
  case B
  case C
  case D
  case E
}

func switcheroo(a: XX, b: XX) -> Int {
  switch(a, b) { // No warning
  case (.A, _)  : return 1
  case (_, .A)  : return 2

  case (.C, _)  : return 3
  case (_, .C)  : return 4

  case (.B, .B) : return 5
  case (.B, .D) : return 6
  case (.D, .B) : return 7

  case (.B, .E) : return 8
  case (.E, .B) : return 9

  case (.E, _)  : return 10
  case (_, .E)  : return 11
  case (.D, .D) : return 12

  default:
    print("never hits this:", a, b)
    return 13
  }
}

enum PatternCasts {
  case one(Any)
  case two
  case three(String)
}

func checkPatternCasts() {
  // Pattern casts with this structure shouldn't warn about duplicate cases.
  let x: PatternCasts = .one("One")
  switch x {
  case .one(let s as String): print(s)
  case .one: break
  case .two: break
  case .three: break
  }

  // But should warn here.
  switch x {
  case .one(_): print(s)
  case .one: break // expected-warning {{case is already handled by previous patterns; consider removing it}}
  case .two: break
  case .three: break
  }

  // And not here
  switch x {
  case .one: break
  case .two: break
  case .three(let s as String?): print(s as Any)
  }
}

enum MyNever {}
func ~= (_ : MyNever, _ : MyNever) -> Bool { return true }
func myFatalError() -> MyNever { fatalError() }

@frozen public enum UninhabitedT4<A> {
  case x(A)
}

func checkUninhabited() {
  // Scrutinees of uninhabited type may match any number and kind of patterns
  // that Sema is willing to accept at will.  After all, it's quite a feat to
  // productively inhabit the type of crashing programs.
  func test1(x : Never) {
    switch x {} // No diagnostic.
  }
  
  func test2(x : Never) {
    switch (x, x) {} // No diagnostic.
  }
  
  func test3(x : MyNever) {
    switch x { // No diagnostic.
    case myFatalError(): break
    case myFatalError(): break
    case myFatalError(): break
    }
  }

  func test4(x: UninhabitedT4<Never>) {
    switch x {} // No diagnostic.
  }
}

enum Runcible {
  case spoon
  case hat
  case fork
}

func checkDiagnosticMinimality(x: Runcible?) {
  switch (x!, x!) { // expected-error {{switch must be exhaustive}}
  // expected-note@-1 {{add missing case: '(.fork, _)'}}
  // expected-note@-2 {{add missing case: '(.hat, .hat)'}}
  // expected-note@-3 {{add missing case: '(_, .fork)'}}
  case (.spoon, .spoon):
    break
  case (.spoon, .hat):
    break
  case (.hat, .spoon):
    break
  }

  switch (x!, x!) { // expected-error {{switch must be exhaustive}}
  // expected-note@-1 {{add missing case: '(.fork, _)'}}
  // expected-note@-2 {{add missing case: '(.hat, .spoon)'}}
  // expected-note@-3 {{add missing case: '(.spoon, .hat)'}}
  // expected-note@-4 {{add missing case: '(_, .fork)'}}
  case (.spoon, .spoon):
    break
  case (.hat, .hat):
    break
  }
}

indirect enum InfinitelySized {
  case one
  case two
  case recur(InfinitelySized)
  case mutualRecur(MutuallyRecursive, InfinitelySized)
}

indirect enum MutuallyRecursive {
  case one
  case two
  case recur(MutuallyRecursive)
  case mutualRecur(InfinitelySized, MutuallyRecursive)
}

func infinitelySized() -> Bool {
  switch (InfinitelySized.one, InfinitelySized.one) { // expected-error {{switch must be exhaustive}}
  // expected-note@-1 8 {{add missing case:}}
  case (.one, .one): return true
  case (.two, .two): return true
  }
  
  switch (MutuallyRecursive.one, MutuallyRecursive.one) { // expected-error {{switch must be exhaustive}}
  // expected-note@-1 8 {{add missing case:}}
  case (.one, .one): return true
  case (.two, .two): return true
  }
}

func sr6316() {
  let bool1 = false
  let bool2 = false
  let bool3 = false
  let bool4 = true
  let bool5 = false
  let bool6 = true
  let bool7 = true
  let bool8 = false
  let bool9 = false

  switch (bool1, (bool2, bool4, bool6, bool8), (bool3, bool5, bool7, bool9)) {
  // expected-error@-1 {{switch must be exhaustive}}
  // expected-note@-2 {{add missing case: '(false, (_, false, true, _), (_, true, _, _))'}}
  // expected-note@-3 {{add missing case: '(_, (_, true, _, _), (_, false, true, _))'}}
  case (true, (_, _, _, _), (_, true, true, _)):
    break
  case (true, (_, _, _, _), (_, _, false, _)):
    break
  case (_, (_, true, true, _), (_, _, false, _)):
    break
  case (_, (_, _, false, _), (_, true, true, _)):
    break
  case (_, (_, true, true, _), (_, true, true, _)):
    break
  case (_, (_, _, false, _), (_, _, false, _)):
    break
  case (_, (_, false, _, _), (_, false, _, _)):
    break
  }
}

func sr6652() {
  enum A {
    indirect case a([A], foo: Bool)
    indirect case b(Dictionary<String, Int>)
    indirect case c(A, foo: [A])
    indirect case d(if: A, then: A, else: A)
    indirect case e(A, A, foo: Bool)
    indirect case f(A)
    case g(String, foo: Bool)
    case string(String)
    case `nil`

    static func eke(_ lhs: A, _ rhs: A) -> Bool { return false }
  }

  enum B {
    static func eke(_ lhs: B, _ rhs: B) -> Bool { return false }
  }

  enum C {
    static func eke(_ lhs: C, _ rhs: C) -> Bool { return false }
  }

  enum D {
    case a(A)
    case b(B)
    case c(C)
    indirect case d([D])

    // No diagnostic
    static func eke(_ lhs: D, _ rhs: D) -> Bool {
      switch (lhs, rhs) {
      case (.a(let r1), .a(let r2)): return A.eke(r1, r2)
      case (.a, _): return false
      case (.b(let r1), .b(let r2)): return B.eke(r1, r2)
      case (.b, _): return false
      case (.c(let r1),  .c(let r2)): return C.eke(r1, r2)
      case (.c, _): return false
      case (.d(let r1),  .d(let r2)): return zip(r1, r2).allSatisfy(D.eke)
      case (.d, _): return false
      }
    }
  }
}

func diagnoseDuplicateLiterals() {
  let str = "def"
  let int = 2
  let dbl = 2.5

  // No Diagnostics
  switch str {
  case "abc": break
  case "def": break
  case "ghi": break
  default: break
  }

  switch str {
  case "abc": break
  case "def": break // expected-note {{first occurrence of identical literal pattern is here}}
  case "def": break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case "ghi": break
  default: break
  }
  
  switch str {
  case "abc", "def": break // expected-note 2 {{first occurrence of identical literal pattern is here}}
  case "ghi", "jkl": break
  case "abc", "def": break // expected-warning 2 {{literal value is already handled by previous pattern; consider removing it}}
  default: break
  }

  switch str {
  case "xyz": break // expected-note {{first occurrence of identical literal pattern is here}}
  case "ghi": break
  case "def": break
  case "abc": break
  case "xyz": break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  default: break
  }

  func someStr() -> String { return "sdlkj" }
  let otherStr = "ifnvbnwe"
  switch str {
  case "sdlkj": break
  case "ghi": break // expected-note {{first occurrence of identical literal pattern is here}}
  case someStr(): break
  case "def": break
  case otherStr: break
  case "xyz": break // expected-note {{first occurrence of identical literal pattern is here}}
  case "ifnvbnwe": break
  case "ghi": break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case "xyz": break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  default: break
  }

  // No Diagnostics
  switch int {
  case -2: break
  case -1: break
  case 0: break
  case 1: break
  case 2: break
  case 3: break
  default: break
  }

  switch int {
  case -2: break // expected-note {{first occurrence of identical literal pattern is here}}
  case -2: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 1: break
  case 2: break // expected-note {{first occurrence of identical literal pattern is here}}
  case 2: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 3: break
  default: break
  }
    
  switch int {
  case -2, -2: break // expected-note {{first occurrence of identical literal pattern is here}} expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 1, 2: break // expected-note 3 {{first occurrence of identical literal pattern is here}}
  case 2, 3: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 1, 2: break // expected-warning 2 {{literal value is already handled by previous pattern; consider removing it}}
  case 4, 5: break
  case 7, 7: break // expected-note {{first occurrence of identical literal pattern is here}}
                   // expected-warning@-1 {{literal value is already handled by previous pattern; consider removing it}}
  default: break
  }

  switch int {
  case 1: break // expected-note {{first occurrence of identical literal pattern is here}}
  case 2: break // expected-note 2 {{first occurrence of identical literal pattern is here}}
  case 3: break
  case 17: break // expected-note {{first occurrence of identical literal pattern is here}}
  case 4: break
  case 2: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 001: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 5: break
  case 0x11: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 0b10: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  default: break
  }

  switch int {
  case 10: break
  case 0b10: break // expected-note {{first occurrence of identical literal pattern is here}}
  case -0b10: break // expected-note {{first occurrence of identical literal pattern is here}}
  case 3000: break
  case 0x12: break // expected-note {{first occurrence of identical literal pattern is here}}
  case 400: break
  case 2: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case -2: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 18: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  default: break
  }

  func someInt() -> Int { return 0x1234 }
  let otherInt = 13254
  switch int {
  case 13254: break
  case 3000: break
  case 00000002: break // expected-note {{first occurrence of identical literal pattern is here}}
  case 0x1234: break
  case someInt(): break
  case 400: break
  case 2: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 18: break
  case otherInt: break
  case 230: break
  default: break
  }

  // No Diagnostics
  switch dbl {
  case -3.5: break
  case -2.5: break
  case -1.5: break
  case 1.5: break
  case 2.5: break
  case 3.5: break
  default: break
  }
  
  switch dbl {
  case -3.5: break
  case -2.5: break // expected-note {{first occurrence of identical literal pattern is here}}
  case -2.5: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case -1.5: break
  case 1.5: break
  case 2.5: break // expected-note {{first occurrence of identical literal pattern is here}}
  case 2.5: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 3.5: break
  default: break
  }
  
  switch dbl {
  case 1.5, 4.5, 7.5, 6.9: break // expected-note 2 {{first occurrence of identical literal pattern is here}}
  case 3.4, 1.5: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 7.5, 2.3: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  default: break
  }
  
  switch dbl {
  case 1: break
  case 1.5: break // expected-note 2 {{first occurrence of identical literal pattern is here}}
  case 2.5: break
  case 3.5: break // expected-note {{first occurrence of identical literal pattern is here}}
  case 5.3132: break
  case 1.500: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 46.2395: break
  case 1.5000: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 0003.50000: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 23452.43: break
  default: break
  }
  
  func someDouble() -> Double { return 324.4523 }
  let otherDouble = 458.2345
  switch dbl {
  case 1: break // expected-note {{first occurrence of identical literal pattern is here}}
  case 1.5: break
  case 2.5: break
  case 3.5: break // expected-note {{first occurrence of identical literal pattern is here}}
  case 5.3132: break
  case 46.2395: break
  case someDouble(): break
  case 0003.50000: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case otherDouble: break
  case 2.50505: break // expected-note {{first occurrence of identical literal pattern is here}}
  case 23452.43: break
  case 00001: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  case 123453: break
  case 2.50505000000: break // expected-warning {{literal value is already handled by previous pattern; consider removing it}}
  default: break
  }
}

func checkLiteralTuples() {
  let str1 = "abc"
  let str2 = "def"
  let int1 = 23
  let int2 = 7
  let dbl1 = 4.23
  let dbl2 = 23.45
  
  // No Diagnostics
  switch (str1, str2) {
  case ("abc", "def"): break
  case ("def", "ghi"): break
  case ("ghi", "def"): break
  case ("abc", "def"): break // We currently don't catch this
  default: break
  }
  
  // No Diagnostics
  switch (int1, int2) {
  case (94, 23): break
  case (7, 23): break
  case (94, 23): break // We currently don't catch this
  case (23, 7): break
  default: break
  }
  
  // No Diagnostics
  switch (dbl1, dbl2) {
  case (543.21, 123.45): break
  case (543.21, 123.45): break // We currently don't catch this
  case (23.45, 4.23): break
  case (4.23, 23.45): break
  default: break
  }
}

func sr6975() {
  enum E {
    case a, b
  }

  let e = E.b
  switch e {
  case .a as E: // expected-warning {{'as' test is always true}}
    print("a")
  case .b: // Valid!
    print("b")
  case .a: // expected-warning {{case is already handled by previous patterns; consider removing it}}
    print("second a")
  }

  func foo(_ str: String) -> Int {
    switch str { // expected-error {{switch must be exhaustive}}
    // expected-note@-1 {{do you want to add a default clause?}}
    case let (x as Int) as Any:
      return x
    }
  }
  _ = foo("wtf")
}

public enum NonExhaustive {
  case a, b
}

public enum NonExhaustivePayload {
  case a(Int), b(Bool)
}

@frozen public enum TemporalProxy {
  case seconds(Int)
  case milliseconds(Int)
  case microseconds(Int)
  case nanoseconds(Int)
  case never
}

// Inlinable code is considered "outside" the module and must include a default
// case.
@inlinable
public func testNonExhaustive(_ value: NonExhaustive, _ payload: NonExhaustivePayload, for interval: TemporalProxy, flag: Bool) {
  switch value { // expected-error {{switch must be exhaustive}} {{none}} expected-note {{add missing case: '.b'}} {{none}} expected-note {{handle unknown values using "@unknown default"}} {{none}}
  case .a: break
  }

  switch value { // expected-warning {{switch covers known cases, but 'NonExhaustive' may have additional unknown values}} {{none}} expected-note {{handle unknown values using "@unknown default"}} {{3-3=@unknown default:\n<#fatalError#>()\n}}
  case .a: break
  case .b: break
  }
  
  switch value {
  case .a: break
  case .b: break
  default: break // no-warning
  }

  switch value {
  case .a: break
  case .b: break
  @unknown case _: break // no-warning
  }

  switch value { // expected-warning {{switch must be exhaustive}} {{none}} expected-note {{add missing case: '.b'}} {{none}}
  case .a: break
  @unknown case _: break
  }

  switch value { // expected-warning {{switch must be exhaustive}} {{none}} expected-note {{add missing case: '.a'}} {{none}} expected-note {{add missing case: '.b'}} {{none}}
  @unknown case _: break
  }

  switch value {
  case _: break
  @unknown case _: break // expected-warning {{case is already handled by previous patterns; consider removing it}}
  }

  // Test being part of other spaces.
  switch value as Optional { // expected-warning {{switch covers known cases, but 'Optional<NonExhaustive>' may have additional unknown values}} {{none}} expected-note {{add missing case: '.some(_)'}}
  case .a?: break
  case .b?: break
  case nil: break
  }

  switch value as Optional {
  case .a?: break
  case .b?: break
  case nil: break
  @unknown case _: break
  } // no-warning

  switch value as Optional {
  case _?: break
  case nil: break
  } // no-warning

  switch (value, flag) { // expected-warning {{switch covers known cases, but '(NonExhaustive, Bool)' may have additional unknown values}} {{none}} expected-note {{add missing case: '(_, false)'}}
  case (.a, _): break
  case (.b, false): break
  case (_, true): break
  }

  switch (value, flag) {
  case (.a, _): break
  case (.b, false): break
  case (_, true): break
  @unknown case _: break
  } // no-warning

  switch (flag, value) { // expected-warning {{switch covers known cases, but '(Bool, NonExhaustive)' may have additional unknown values}} {{none}} expected-note {{add missing case: '(false, _)'}}
  case (_, .a): break
  case (false, .b): break
  case (true, _): break
  }

  switch (flag, value) {
  case (_, .a): break
  case (false, .b): break
  case (true, _): break
  @unknown case _: break
  } // no-warning

  switch (value, value) { // expected-warning {{switch covers known cases, but '(NonExhaustive, NonExhaustive)' may have additional unknown values}} {{none}} expected-note {{add missing case: '(_, _)'}}
  case (.a, _), (_, .a): break
  case (.b, _), (_, .b): break
  }

  switch (value, value) {
  case (.a, _), (_, .a): break
  case (.b, _), (_, .b): break
  @unknown case _: break
  } // no-warning

  // Test payloaded enums.
  switch payload { // expected-error {{switch must be exhaustive}} {{none}} expected-note {{add missing case: '.b(_)'}} {{none}} expected-note {{handle unknown values using "@unknown default"}} {{none}}
  case .a: break
  }

  switch payload { // expected-warning {{switch covers known cases, but 'NonExhaustivePayload' may have additional unknown values}} {{none}} expected-note {{handle unknown values using "@unknown default"}} {{3-3=@unknown default:\n<#fatalError#>()\n}}
  case .a: break
  case .b: break
  }
  
  switch payload {
  case .a: break
  case .b: break
  default: break // no-warning
  }

  switch payload {
  case .a: break
  case .b: break
  @unknown case _: break // no-warning
  }

  switch payload { // expected-warning {{switch must be exhaustive}} {{none}} expected-note {{add missing case: '.b(_)'}} {{none}}
  case .a: break
  @unknown case _: break
  }

  switch payload { // expected-error {{switch must be exhaustive}} {{none}} expected-note {{add missing case: '.b(true)'}} {{none}} expected-note {{handle unknown values using "@unknown default"}} {{none}}
  case .a: break
  case .b(false): break
  }

  switch payload { // expected-warning {{switch must be exhaustive}} {{none}} expected-note {{add missing case: '.b(true)'}} {{none}}
  case .a: break
  case .b(false): break
  @unknown case _: break
  }

  // Test fully-covered switches.
  switch interval {
  case .seconds, .milliseconds, .microseconds, .nanoseconds: break
  case .never: break
  @unknown case _: break // expected-warning {{case is already handled by previous patterns; consider removing it}}
  }

  switch flag {
  case true: break
  case false: break
  @unknown case _: break // expected-warning {{case is already handled by previous patterns; consider removing it}}
  }

  switch flag as Optional {
  case _?: break
  case nil: break
  @unknown case _: break // expected-warning {{case is already handled by previous patterns; consider removing it}}
  }

  switch (flag, value) {
  case (true, _): break
  case (false, _): break
  @unknown case _: break // expected-warning {{case is already handled by previous patterns; consider removing it}}
  }
}

public func testNonExhaustiveWithinModule(_ value: NonExhaustive, _ payload: NonExhaustivePayload, flag: Bool) {
  switch value { // expected-error {{switch must be exhaustive}} {{none}} expected-note {{add missing case: '.b'}}
  case .a: break
  }

  switch value { // no-warning
  case .a: break
  case .b: break
  }
  
  switch value {
  case .a: break
  case .b: break
  default: break // no-warning
  }

  switch value {
  case .a: break
  case .b: break
  @unknown case _: break // no-warning
  }

  switch value { // expected-warning {{switch must be exhaustive}} {{none}} expected-note {{add missing case: '.b'}} {{none}}
  case .a: break
  @unknown case _: break
  }

  switch value { // expected-warning {{switch must be exhaustive}} {{none}} expected-note {{add missing case: '.a'}} {{none}} expected-note {{add missing case: '.b'}} {{none}}
  @unknown case _: break
  }

  switch value {
  case _: break
  @unknown case _: break // expected-warning {{case is already handled by previous patterns; consider removing it}}
  }

  // Test being part of other spaces.
  switch value as Optional { // no-warning
  case .a?: break
  case .b?: break
  case nil: break
  }

  switch value as Optional {
  case _?: break
  case nil: break
  } // no-warning

  switch (value, flag) { // no-warning
  case (.a, _): break
  case (.b, false): break
  case (_, true): break
  }

  switch (flag, value) { // no-warning
  case (_, .a): break
  case (false, .b): break
  case (true, _): break
  }

  switch (value, value) { // no-warning
  case (.a, _): break
  case (.b, _): break
  case (_, .a): break
  case (_, .b): break
  }

  switch (value, value) { // no-warning
  case (.a, _): break
  case (.b, _): break
  case (_, .a): break
  case (_, .b): break
  @unknown case _: break
  }

  // Test payloaded enums.
  switch payload { // expected-error {{switch must be exhaustive}} {{none}} expected-note {{add missing case: '.b(_)'}} {{none}}
  case .a: break
  }

  switch payload { // no-warning
  case .a: break
  case .b: break
  }
  
  switch payload {
  case .a: break
  case .b: break
  default: break // no-warning
  }

  switch payload {
  case .a: break
  case .b: break
  @unknown case _: break // no-warning
  }

  switch payload { // expected-warning {{switch must be exhaustive}} {{none}} expected-note {{add missing case: '.b(_)'}} {{none}}
  case .a: break
  @unknown case _: break
  }

  switch payload { // expected-error {{switch must be exhaustive}} {{none}} expected-note {{add missing case: '.b(true)'}} {{none}}
  case .a: break
  case .b(false): break
  }

  switch payload { // expected-warning {{switch must be exhaustive}} {{none}} expected-note {{add missing case: '.b(true)'}} {{none}}
  case .a: break
  case .b(false): break
  @unknown case _: break
  }
}

enum UnavailableCase {
  case a
  case b
  @available(*, unavailable)
  case oopsThisWasABadIdea
}

enum UnavailableCaseOSSpecific {
  case a
  case b

#if canImport(Darwin)
  @available(macOS, unavailable)
  @available(iOS, unavailable)
  @available(tvOS, unavailable)
  @available(watchOS, unavailable)
  case unavailableOnAllTheseApplePlatforms
#else
  @available(*, unavailable)
  case dummyCaseForOtherPlatforms
#endif
}

enum UnavailableCaseOSIntroduced {
  case a
  case b

  @available(macOS 50, iOS 50, tvOS 50, watchOS 50, *)
  case notYetIntroduced
}

func testUnavailableCases(_ x: UnavailableCase, _ y: UnavailableCaseOSSpecific, _ z: UnavailableCaseOSIntroduced) {
  switch x {
  case .a: break
  case .b: break
  } // no-error

  switch y {
  case .a: break
  case .b: break
  } // no-error

  switch z {
  case .a: break
  case .b: break
  case .notYetIntroduced: break
  } // no-error
}

// The following test used to behave differently when the uninhabited enum was
// defined in the same module as the function (as opposed to using Swift.Never).
enum NoError {}
extension Result where T == NoError {
  func testUninhabited() {
    switch self {
    case .Error(_):
      break
    // No .Ok case possible because of the 'NoError'.
    }

    switch self {
    case .Error(_):
      break
    case .Ok(_):
      break // But it's okay to write one.
    }
  }
}

enum SR10301<T,E> {
  case value(T)
  case error(E)
}
enum SR10301Error: Error {
  case bad
}

func sr10301(_ foo: SR10301<String,(Int,Error)>) {
  switch foo {
  case .value: return
  case .error((_, SR10301Error.bad)): return
  case .error((_, let err)):
    _ = err
    return
  }
}

func sr10301_is(_ foo: SR10301<String,(Int,Error)>) {
  switch foo {
  case .value: return
  case .error((_, is SR10301Error)): return
  case .error((_, let err)):
    _ = err
    return
  }
}

func sr10301_as(_ foo: SR10301<String,(Int,Error)>) {
  switch foo {
  case .value: return
  case .error((_, let err as SR10301Error)):
    _ = err
    return
  case .error((_, let err)):
    _ = err
    return
  }
}

func sr11160() {
  switch Optional<(Int, Int)>((5, 6)) {
  case .some((let a, let b)): print(a, b)
  case nil:                   print(0)
  }

  switch Optional<(Int, Int)>((5, 6)) {
  case let b?: print(b)
  case nil:    print(0)
  }
}

enum Z {
  case z1(a: Int)
  case z2(a: Int, b: Int)
  case z3((c: Int, d: Int))
}

func sr11160_extra() {
  switch Z.z1(a: 1) { // expected-error {{switch must be exhaustive}}
                      // expected-note@-1 {{add missing case: '.z1(a: let a)'}}
  case .z2(_, _): ()
  case .z3(_): ()
  }

  switch Z.z1(a: 1) { // expected-error {{switch must be exhaustive}}
                      // expected-note@-1 {{add missing case: '.z2(a: let a, b: let b)'}}
  case .z1(_): ()
  case .z3(_): ()
  }

  switch Z.z1(a: 1) { // expected-error {{switch must be exhaustive}}
                      // expected-note@-1 {{add missing case: '.z3((let c, let d))'}}
  case .z1(_):    ()
  case .z2(_, _): ()
  }
}

public enum SR11672Tests {
  
  @frozen public enum FrozenSameModule {
    case a, b
  }
  
  func testNotRequired(_ value: NonExhaustive, _ value2: FrozenEnum, _ value3: FrozenSameModule) {
    switch value {
      // expected-error@-1 {{switch must be exhaustive}}
      // expected-note@-2 {{add missing case: '.a'}}
      // expected-note@-3 {{add missing case: '.b'}}
      // Do not suggest adding '@unknown default'
    }
    
    switch value2 {
      // expected-error@-1 {{switch must be exhaustive}}
      // expected-note@-2 {{add missing case: '.a'}}
      // expected-note@-3 {{add missing case: '.b'}}
      // expected-note@-4 {{add missing case: '.c'}}
    }
    
    switch value3 {
      // expected-error@-1 {{switch must be exhaustive}}
      // expected-note@-2 {{add missing case: '.a'}}
      // expected-note@-3 {{add missing case: '.b'}}
    }
  }
  
  @inlinable public func testNotRequired2(_ value: FrozenSameModule) {
    switch value {
      // expected-error@-1 {{switch must be exhaustive}}
      // expected-note@-2 {{add missing case: '.a'}}
      // expected-note@-3 {{add missing case: '.b'}}
    }
  }
  
  // Inlinable code is considered "outside" the module and must include a default
  // case.
  @inlinable public func testRequired(_ value: NonExhaustive) {
    switch value {
      // expected-error@-1 {{switch must be exhaustive}}
      // expected-note@-2 {{add missing case: '.a'}}
      // expected-note@-3 {{add missing case: '.b'}}
      // expected-note@-4 {{handle unknown values using "@unknown default"}}
    }
  }
}

// SR-11212 tests: Some of the tests here rely on compiler bugs related to
// implicit (un)tupling in patterns.
//
// Related codegen test: Compatibility/implicit_tupling_untupling_codegen.swift
enum SR11212Tests {

  enum Untupled {
    case upair(Int, Int)
  }

  func sr11212_content_untupled_pattern_tupled1(u: Untupled) -> (Int, Int) {
    switch u {
    case .upair((let x, let y)): return (x, y)
    // expected-warning@-1 {{enum case 'upair' has 2 associated values}}{{16-17=}}{{31-32=}}
    // expected-note@-7 {{'upair' declared here}}
    }
  }

  func sr11212_content_untupled_pattern_tupled2(u: Untupled) -> (Int, Int) {
    switch u {
    case .upair(let (x, y)): return (x, y)
    // expected-warning@-1 {{enum case 'upair' has 2 associated values}} // No fix-it as that would require us to peek inside the 'let' :-/
    // expected-note@-15 {{'upair' declared here}}
    }
  }

  func sr11212_content_untupled_pattern_tupled3(u: Untupled) -> (Int, Int) {
    switch u {
    case let .upair((x, y)): return (x, y)
    // expected-warning@-1 {{enum case 'upair' has 2 associated values}}{{20-21=}}{{27-28=}}
    // expected-note@-23 {{'upair' declared here}}
    }
  }

  func sr11212_content_untupled_pattern_untupled1(u: Untupled) -> (Int, Int) {
    switch u {
    case .upair(let x, let y): return (x, y)
    }
  }

  func sr11212_content_untupled_pattern_untupled2(u: Untupled) -> (Int, Int) {
      switch u {
      case let .upair(x, y): return (x, y)
      }
  }

  func sr11212_content_untupled_pattern_ambiguous1(u: Untupled) -> (Int, Int) {
    switch u {
    case .upair(let u_): return u_
    // expected-warning@-1 {{enum case 'upair' has 2 associated values; matching them as a tuple is deprecated}}
    // expected-note@-43 {{'upair' declared here}}
    }
  }

  func sr11212_content_untupled_pattern_ambiguous2(u: Untupled) -> (Int, Int) {
    switch u {
    case let .upair(u_): return u_
    // expected-warning@-1 {{enum case 'upair' has 2 associated values; matching them as a tuple is deprecated}}
    // expected-note@-51 {{'upair' declared here}}
    }
  }

  enum Tupled {
    case tpair((Int, Int))
  }

  func sr11212_content_tupled_pattern_tupled1(t: Tupled) -> (Int, Int) {
    switch t {
    case .tpair((let x, let y)): return (x, y)
    }
  }

  func sr11212_content_tupled_pattern_tupled2(t: Tupled) -> (Int, Int) {
    switch t {
    case .tpair(let (x, y)): return (x, y)
    }
  }

  func sr11212_content_tupled_pattern_tupled3(t: Tupled) -> (Int, Int) {
    switch t {
    case let .tpair((x, y)): return (x, y)
    }
  }

  func sr11212_content_tupled_pattern_untupled1(t: Tupled) -> (Int, Int) {
    switch t {
    case .tpair(let x, let y): return (x, y)
    // expected-warning@-1 {{enum case 'tpair' has one associated value that is a tuple of 2 elements}}{{16-16=(}}{{30-30=)}}
    // expected-note@-25 {{'tpair' declared here}}
    }
  }

  func sr11212_content_tupled_pattern_untupled2(t: Tupled) -> (Int, Int) {
    switch t {
    case let .tpair(x, y): return (x, y)
    // expected-warning@-1 {{enum case 'tpair' has one associated value that is a tuple of 2 elements}}{{20-20=(}}{{26-26=)}}
    // expected-note@-33 {{'tpair' declared here}}
    }
  }

  func sr11212_content_tupled_pattern_ambiguous1(t: Tupled) -> (Int, Int) {
    switch t {
    case .tpair(let t_): return t_
    }
  }

  func sr11212_content_tupled_pattern_ambiguous2(t: Tupled) -> (Int, Int) {
    switch t {
    case let .tpair(t_): return t_
    }
  }

  enum Box<T> {
    case box(T)
  }

  func sr11212_content_generic_pattern_tupled1(b: Box<(Int, Int)>) -> (Int, Int) {
    switch b {
    case .box((let x, let y)): return (x, y)
    }
  }

  func sr11212_content_generic_pattern_tupled2(b: Box<(Int, Int)>) -> (Int, Int) {
    switch b {
    case .box(let (x, y)): return (x, y)
    }
  }

  func sr11212_content_generic_pattern_tupled3(b: Box<(Int, Int)>) -> (Int, Int) {
   switch b {
   case let .box((x, y)): return (x, y)
   }
  }

  func sr11212_content_generic_pattern_untupled1(b: Box<(Int, Int)>) -> (Int, Int) {
    switch b {
    case .box(let x, let y): return (x, y)
    // expected-warning@-1 {{enum case 'box' has one associated value that is a tuple of 2 elements}}{{14-14=(}}{{28-28=)}}
    // expected-note@-25 {{'box' declared here}}
    }
  }

  func sr11212_content_generic_pattern_untupled2(b: Box<(Int, Int)>) -> (Int, Int) {
    switch b {
    case let .box(x, y): return (x, y)
    // expected-warning@-1 {{enum case 'box' has one associated value that is a tuple of 2 elements}}{{18-18=(}}{{24-24=)}}
    // expected-note@-33 {{'box' declared here}}
    }
  }

  // rdar://problem/58578342
  func sr11212_content_generic_pattern_untupled3(b: Box<((Int, Int), Int)>) -> (Int, Int, Int) {
    switch b {
    case let .box((x, y), z): return (x, y, z)
    // expected-warning@-1 {{enum case 'box' has one associated value that is a tuple of 2 elements}}{{18-18=(}}{{29-29=)}}
    // expected-note@-42 {{'box' declared here}}
    }
  }

  func sr11212_content_generic_pattern_ambiguous1(b: Box<(Int, Int)>) -> (Int, Int) {
    switch b {
    case .box(let b_): return b_
    }
  }

  func sr11212_content_generic_pattern_ambiguous2(b: Box<(Int, Int)>) -> (Int, Int) {
    switch b {
    case let .box(b_): return b_
    }
  }

} // end SR11212Tests

func sr12412() {
  enum E {
    case x
    case y
  }
  switch (E.x, true) as Optional<(e: E, b: Bool)> {
      case nil, (e: .x, b: _)?: break
      case (e: .y, b: false)?: break
      case (e: .y, b: true)?: break
  }
}
