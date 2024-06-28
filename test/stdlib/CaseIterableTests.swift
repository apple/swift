// RUN: %target-run-simple-swift %t
// REQUIRES: executable_test
// REQUIRES: concurrency

import StdlibUnittest

var CaseIterableTests = TestSuite("CaseIterableTests")

CaseIterableTests.test("Simple Enums") {
  enum SimpleEnum: CaseIterable {
    case bar
    case baz
    case quux
  }

  expectEqual(SimpleEnum.allCases.count, 3)
  expectEqual(SimpleEnum.allCases, [.bar, .baz, .quux])
}

CaseIterableTests.test("MainActor Isolated Enums") {
  @MainActor
  enum EnumMainActor: CaseIterable {
    case a, b
  }

  expectEqual(EnumMainActor.allCases.count, 2)
  expectEqual(EnumMainActor.allCases, [.a, .b])
}

runAllTests()
