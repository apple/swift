// RUN: %target-run-simple-swift(-enable-experimental-feature SwiftParser -enable-experimental-feature ParserASTGen)
// RUN: %target-run-simple-swift(-enable-experimental-feature ASTGenTypes)

// REQUIRES: executable_test

// -enable-experimental-feature requires an asserts build
// REQUIRES: asserts

func test1(x: Int, fn: (Int) -> Int) -> Int {
  let xx = fn(42)
  return fn(x)
}

func test2(b: Bool) {
  if b {
    print("TRUE")
  } else {
    print("FALSE")
  }

  let x = true
}

func test3(y: Int) -> Int {
  let x = y
  return x
}

func test4(_ b: [Bool]) -> Int {
  if b.isEmpty { 0 } else { 1 }
}

func test5(_ b: Swift.Bool) -> Int {
  return if b { 0 } else { 1 }
}

func test6(_ b: Bool) -> Int {
  let x = if b { 0 } else { 1 }
  return x
}

func test7(_ b: inout Bool) {
  // b = true
}

func test8(_ i: _const Int) {
}

func test9(_ value: Any) { }
