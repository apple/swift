// RUN: %target-swift-emit-sil %s -verify | %FileCheck %s

func testUnreachableAfterReturn() -> Int {
  var x: Int = 3
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
    var y: Int = 300
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
  var i:Int = 0
  while (i<10) { 
    var y: Int = 300
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
  case let Leaf:
    _ = Leaf
    return
  case .Branch(_): // expected-warning {{case is already handled by previous patterns; consider removing it}}
    return
  }
}

func testUnreachableCase2(a : Tree) {
  switch a {
  case let Leaf:
    _ = Leaf
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

// https://github.com/apple/swift/issues/48333
func testOptionalEvaluationBreak(a : Tree) {
  class C { func foo() {} }
  func createOptional() -> C? { return C() }
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

// https://github.com/apple/swift/issues/48696
func f_48696() {
  var bar: String? = ""
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

// https://github.com/apple/swift/issues/56075
func f_56075() -> Int {
  return Foo.bar
  struct Foo { // no-warning
    static var bar = 0
    // CHECK: sil private @$s16unreachable_code7f_56075SiyF3FooL_V7fooFuncyyF : $@convention(method) (Foo) -> ()
    func fooFunc() {}
  }
  func appendix() {} // no-warning
}

// https://github.com/apple/swift/issues/73649
func testUnreachableExistential() {
  protocol P {
    func run()
  }

  func unreachableExistential(_ it: any P) -> Bool {
    return true
    it.run() // expected-warning {{code after 'return' will never be executed}}
  }

  func unreachableOptionalChainedExistential(_ it: (any P)?) -> Bool {
    return false
    it?.run() // expected-warning {{code after 'return' will never be executed}}
  }
}
