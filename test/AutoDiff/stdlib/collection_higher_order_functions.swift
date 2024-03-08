// RUN: %target-run-simple-swift
// REQUIRES: executable_test


import _Differentiation
import StdlibUnittest

// Test differentiable collection higher order functions:
// `differentiableMap(_:)` and `differentiableReduce(_:_:)`.

var CollectionHOFTests = TestSuite("CollectionHigherOrderFunctions")

let array: [Float] = [1, 2, 3, 4, 5]

CollectionHOFTests.test("differentiableMap(_:)") {
  func double(_ array: [Float]) -> [Float] {
    array.differentiableMap { $0 * $0 }
  }
  expectEqual([], pullback(at: array, of: double)([]))
  expectEqual([0], pullback(at: array, of: double)([0]))
  expectEqual([2], pullback(at: array, of: double)([1]))
  expectEqual([2, 4, 6, 8, 10], pullback(at: array, of: double)([1, 1, 1, 1, 1]))
}

CollectionHOFTests.test("differentiableReduce(_:_:)") {
  func product(_ array: [Float]) -> Float {
    array.differentiableReduce(1) { $0 * $1 }
  }
  expectEqual([1], gradient(at: [0], of: product))
  expectEqual([1], gradient(at: [1], of: product))
  expectEqual([120, 60, 40, 30, 24], gradient(at: array, of: product))
}

runAllTests()
