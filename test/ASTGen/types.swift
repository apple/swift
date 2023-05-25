// RUN: %target-typecheck-verify-swift -enable-experimental-feature ASTGenTypes

// -enable-experimental-feature requires an asserts build
// REQUIRES: asserts

protocol P { }
protocol Q { }
typealias PQ = P & Q

func test7(_ b: inout Bool) {
  b = true
}

struct X { struct `Protocol` { } }

func test10(_: X.`Protocol`) { }

func test11(_: Int...) { }
func test11a() {
  test11(1, 2, 3, 4, 5)
}

typealias VAFunc = (Int, Int...) -> Int
func testVAFunc(a: Int, f: VAFunc) {
  _ = f(a, a, a, a, a)
}

func test12(_ producer: @escaping @autoclosure () -> Int) {
  _ = producer()
}
func test12a(i: Int) {
  test12(i)
}

func test13(body: (_ value: Int) -> Void, i: Int) {
  body(i)
}

func test14() {
  _ = Array<Array<Array<Int>>>().count
}
