// RUN: %target-run-stdlib-swift
// REQUIRES: executable_test

import StdlibUnittest
import StdlibCollectionUnittest

let SequenceAlgorithmTests = TestSuite("SequenceAlgorithm")

SequenceAlgorithmTests.test("[1,2,3,4,5,-9,6,7,8,9,0].min()") {
  expectEqual([1,2,3,4,5,-9,6,7,8,9,0].min()!, -9)
}

SequenceAlgorithmTests.test("Array<Int>().min()") {
  expectNil(Array<Int>().min())
}

SequenceAlgorithmTests.test("[1,2,3,4,5,-9,6,7,8,9,0].max()") {
  expectEqual([1,2,3,4,5,-9,6,7,8,9,0].max()!, 9)
}

SequenceAlgorithmTests.test("Array<Int>().max()") {
  expectNil(Array<Int>().max())
}

SequenceAlgorithmTests.test("[1,2,3,4,5,-9,6,7,8,9,0].minmax()") {
    expectEqual([1,2,3,4,5,-9,6,7,8,9,0].minmax()!, (min: -9, max: 9))
}

runAllTests()
