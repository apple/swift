// RUN: %target-swift-emit-sil %s -verify | %FileCheck %s

func testUnreachableAfterReturn() -> Int {
  var x: Int = 3 //expected-warning {{variable 'x' was never mutated; consider changing to 'let' constant}}
  return x
  x += 1 //expected-warning {{code after 'return' will never be executed}}
}

func testUnreachableAfterIfReturn(a: Bool) -> Int {
  if a {
    return 1
  } else {
    return 0
  }
  var _: Int = testUnreachableAfterReturn() // expected-warning {{will never be executed}}
}

func testUnreachableForAfterContinue(b: Bool) {
  for _ in 0..<10 {
    var y: Int = 300 // expected-warning {{variable 'y' was written to, but never read}}
    y += 1
    if b {
      break
      y += 1 // expected-warning {{code after 'break' will never be executed}}
    }
    continue
    y -= 1 // expected-warning {{code after 'continue' will never be executed}}
  }
}

func testUnreachableWhileAfterContinue(b: Bool) {
  var i:Int = 0 // expected-warning {{variable 'i' was never mutated; consider changing to 'let' constant}}
  while (i<10) { 
    var y: Int = 300 // expected-warning {{variable 'y' was written to, but never read}}
    y += 1
    if b {
      break
      y += 1 // expected-warning {{code after 'break' will never be executed}}
    }
    continue
    i += 1 // expected-warning {{code after 'continue' will never be executed}}
  }
}

func testBreakAndContinue() {
  var m = 0
  for _ in 0 ..< 10 {
    m += 1
    if m == 15 {
      break
    } else {
      continue
    }
    m += 1 // expected-warning {{will never be executed}}
  }
}


// <rdar://problem/20253447> `case let Case` without bindings incorrectly matches other cases
enum Tree {
  case Leaf(Int)
  case Branch(Int)
}

func testUnreachableCase1(a : Tree) {
  switch a {
  case let Leaf: // expected-warning {{immutable value 'Leaf' was never used; consider replacing with '_' or removing it}}
    return
  case .Branch(_): // expected-warning {{case is already handled by previous patterns; consider removing it}}
    return
  }
}

func testUnreachableCase2(a : Tree) {
  switch a {
  case let Leaf: // expected-warning {{immutable value 'Leaf' was never used; consider replacing with '_' or removing it}}
    fallthrough
  case .Branch(_): // expected-warning {{case is already handled by previous patterns; consider removing it}}
    return
  }
}

func testUnreachableCase3(a : Tree) {
  switch a {
  case _:
    break
  case .Branch(_): // expected-warning {{case is already handled by previous patterns; consider removing it}}
    return
  }
}

func testUnreachableCase4(a : Tree) {
  switch a {
  case .Leaf(_):
    return
  case .Branch(_):
    return
  }
}

func testUnreachableCase5(a : Tree) {
  switch a {
  case _:
    break
  default:  // expected-warning {{default will never be executed}}
    return
  }
}

func testOptionalEvaluationBreak(a : Tree) {
  class SR5763 { func foo() {} }
  func createOptional() -> SR5763? { return SR5763() }
  switch a {
  case _:
    break
    createOptional()?.foo() // expected-warning {{code after 'break' will never be executed}}
  }
}

func testUnreachableAfterThrow(e: Error) throws {
  throw e
  return   // expected-warning {{code after 'throw' will never be executed}}
}

class TestThrowInInit {
  required init(e: Error) throws {
    throw e  // no unreachable code diagnostic for the implicit return.
  }
}

func sr6141() {
  var bar: String? = "" // expected-warning{{variable 'bar' was never used; consider replacing with '_' or removing it}}
  return;
  bar?.append("x")  // expected-warning{{code after 'return' will never be executed}}
}

func testUnreachableCatchClause() {
  enum ErrorEnum: Error { case someError }
  do {
    throw ErrorEnum.someError
  } catch let error {
    print(error)
  } catch ErrorEnum.someError { // expected-warning {{case will never be executed}}
    print("some error")
  }
}

func sr13639() -> Int {
  return Foo.bar
  struct Foo { // no-warning
    static var bar = 0
    // CHECK: sil private @$s16unreachable_code7sr13639SiyF3FooL_V7fooFuncyyF : $@convention(method) (Foo) -> ()
    func fooFunc() {}
  }
}
