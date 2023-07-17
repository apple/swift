// REQUIRES: executable_test
// RUN: %target-run-simple-swift

import StdlibUnittest

var suite = TestSuite("ParameterPackTestSuite")

suite.test("operator precedence") {
  // Test 'a * each b + c' is parsed and operator-folded as '(a * (each b)) + c'
  func _test<each T: Numeric>(args arg: repeat each T) -> (repeat each T) {
    (repeat 2 * each arg + 3)
  }

  let result = _test(args: 12, 12.3)
  expectEqual(result.0, 2 * 12 + 3)
  expectEqual(result.1, 2 * 12.3 + 3)
}

runAllTests()
