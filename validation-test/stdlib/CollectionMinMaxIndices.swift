// RUN: %target-run-stdlib-swift
// REQUIRES: executable_test

import StdlibUnittest
import StdlibCollectionUnittest

let CollectionMinMaxIndices = TestSuite("CollectionMinMaxIndices")

CollectionMinMaxIndices.test("[1,2,3,4,5,-9,6,7,8,9,0].minIndex()") {
  expectEqual([1,2,3,4,5,-9,6,7,8,9,0].minIndex()!, 5)
}

CollectionMinMaxIndices.test("[3.14].minIndex()") {
    expectEqual([3.14].minIndex()!, 0)
}

CollectionMinMaxIndices.test("Array<Int>().minIndex()") {
  expectNil(Array<Int>().minIndex())
}

CollectionMinMaxIndices.test("[1,2,3,4,5,-9,6,7,8,9,0].maxIndex()") {
  expectEqual([1,2,3,4,5,-9,6,7,8,9,0].maxIndex()!, 9)
}

CollectionMinMaxIndices.test("[3.14].maxIndex()") {
    expectEqual([3.14].maxIndex()!, 0)
}

CollectionMinMaxIndices.test("Array<Int>().maxIndex()") {
  expectNil(Array<Int>().maxIndex())
}

CollectionMinMaxIndices.test("[1,2,3,4,5,-9,6,7,8,9,0].minmaxIndices()") {
  expectEqual([1,2,3,4,5,-9,6,7,8,9,0].minmaxIndices()!, (minIndex: 5, maxIndex: 9))
}

CollectionMinMaxIndices.test("[3.14].minmaxIndices()") {
  expectEqual([3.14].minmaxIndices()!, (minIndex: 0, maxIndex: 0))
}

CollectionMinMaxIndices.test("Array<Int>().minmaxIndices()") {
  expectNil(Array<Int>().minmaxIndices())
}

runAllTests()
