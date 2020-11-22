// RUN: %empty-directory(%t)
// RUN: %target-build-swift -swift-version 4 %s -o %t/a.out && %target-codesign %t/a.out && %target-run %t/a.out

// REQUIRES: executable_test

import StdlibUnittest


public typealias ExpectedResultType = [Character]

var Tests = TestSuite("StringFlatMap")

Tests.test("DefaultReturnType") {
  var result = ["hello", "world"].flatMap { $0 }
  expectType(ExpectedResultType.self, &result)
}

Tests.test("ExplicitTypeContext") {
  expectEqualSequence(["hello", "world"],
    ["hello", "world"].flatMap { $0 } as [String])
  expectEqualSequence("helloworld",
    ["hello", "world"].flatMap { $0 } as [Character])
}

Tests.test("inference") {
  let result = [1, 2].flatMap { x in
    if String(x) == "foo" {
      return "bar"
    } else {
      return nil
    }
  }
  expectEqualSequence([], result)
}

runAllTests()
