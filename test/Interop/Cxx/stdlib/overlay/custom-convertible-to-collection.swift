// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop)
//
// REQUIRES: executable_test
// REQUIRES: OS=macosx || OS=linux-gnu

import StdlibUnittest
import CustomSequence
import Cxx

var CxxSequenceTestSuite = TestSuite("CxxConvertibleToCollection")

CxxSequenceTestSuite.test("SimpleSequence to Swift.Array") {
  let seq = SimpleSequence()
  let array = Array(seq)
  expectEqual([1, 2, 3, 4] as [Int32], array)
}

#if !os(Linux) // this test crashes on Linux (https://github.com/apple/swift/issues/66363)
CxxSequenceTestSuite.test("SimpleCopyAwareSequence to Swift.Array") {
  copiesCount = 0

  let seq = SimpleCopyAwareSequence()
  let array = Array(seq)

  expectEqual(0, copiesCount) // make sure we don't copy the C++ sequence value unnecessarily
}
#endif

CxxSequenceTestSuite.test("SimpleSequenceWithOutOfLineEqualEqual to Swift.Array") {
  let seq = SimpleSequenceWithOutOfLineEqualEqual()
  let array = Array(seq)
  expectEqual([1, 2, 3, 4] as [Int32], array)
}

CxxSequenceTestSuite.test("SimpleSequence to Swift.Set") {
  let seq = SimpleSequence()
  let set = Set(seq)
  expectEqual(Set([1, 2, 3, 4] as [Int32]), set)
}

CxxSequenceTestSuite.test("SimpleEmptySequence to Swift.Array") {
  let seq = SimpleEmptySequence()
  let array = Array(seq)
  expectTrue(array.isEmpty)
}

CxxSequenceTestSuite.test("SimpleEmptySequence to Swift.Set") {
  let seq = SimpleEmptySequence()
  let set = Set(seq)
  expectTrue(set.isEmpty)
}

CxxSequenceTestSuite.test("SimpleSequence.forEach") {
  let seq = SimpleSequence()
  var array: [Int32] = []
  seq.forEach {
    array.append($0)
  }
  expectEqual([1, 2, 3, 4] as [Int32], array)
}

runAllTests()
