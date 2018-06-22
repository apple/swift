// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -o %t/a.out4 -swift-version 4 && %target-run %t/a.out4

// REQUIRES: executable_test

import StdlibUnittest


public typealias ExpectedResultType = [Character]
let swiftVersion = "4"

var Tests = TestSuite("StringFlatMap")

Tests.test("DefaultReturnType/\(swiftVersion)") {
  var result = ["hello", "world"].flatMap { $0 }
  expectType(ExpectedResultType.self, &result)
}

Tests.test("ExplicitTypeContext/\(swiftVersion)") {
  expectEqualSequence(["hello", "world"],
    ["hello", "world"].flatMap { $0 } as [String])
  expectEqualSequence("helloworld".characters,
    ["hello", "world"].flatMap { $0 } as [Character])
}

Tests.test("inference/\(swiftVersion)") {
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
