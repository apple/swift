 // RUN: %target-run-simple-swift
// REQUIRES: executable_test

import StdlibUnittest
import StdlibCollectionUnittest

var StrideTestSuite = TestSuite("Stride")

StrideTestSuite.test("to") {
  checkSequence(Array(0...4), stride(from: 0, to: 5, by: 1))
  checkSequence(Array(1...5).reversed(), stride(from: 5, to: 0, by: -1))
}

StrideTestSuite.test("through") {
  checkSequence(Array(0...5), stride(from: 0, through: 5, by: 1))
  checkSequence(Array(0...5).reversed(), stride(from: 5, through: 0, by: -1))
  checkSequence(Array(Int8.min...Int8.max), stride(from: Int8.min, through: Int8.max, by: 1))
  checkSequence(Array(0...Int8.max), stride(from: 0, through: Int8.max, by: 1))
  checkSequence(Array(UInt8.min...UInt8.max), stride(from: UInt8.min, through: UInt8.max, by: 1))
  checkSequence([65530, 65531, 65532, 65533, 65534, 65535] as [UInt16], stride(from: 65530, through: UInt16.max, by: 1))
  checkSequence([65530, 65532, 65534] as [UInt16], stride(from: 65530, through: UInt16.max, by: 2))
  checkSequence([5, 4, 3, 2, 1, 0] as [UInt16], stride(from: 5, through: UInt16.min, by: -1))
  checkSequence([5, 3, 1] as [UInt16], stride(from: 5, through: UInt16.min, by: -2))
  checkSequence([5, 2] as [UInt16], stride(from: 5, through: UInt16.min, by: -3))
  checkSequence([5, 1] as [UInt16], stride(from: 5, through: UInt16.min, by: -4))
  checkSequence([5, 0] as [UInt16], stride(from: 5, through: UInt16.min, by: -5))
  checkSequence([5] as [UInt16], stride(from: 5, through: UInt16.min, by: -6))
  checkSequence([5] as [UInt16], stride(from: 5, through: UInt16.min, by: -7))
  checkSequence([-32765, -32766, -32767, -32768] as [Int16], stride(from: -32765, through: Int16.min, by: -1))
  checkSequence([-32765, -32767] as [Int16], stride(from: -32765, through: Int16.min, by: -2))
  checkSequence([-32765, -32768] as [Int16], stride(from: -32765, through: Int16.min, by: -3))
  checkSequence([-32765] as [Int16], stride(from: -32765, through: Int16.min, by: -4))
  checkSequence([-32765] as [Int16], stride(from: -32765, through: Int16.min, by: -5))
  checkSequence([-32765] as [Int16], stride(from: -32765, through: Int16.min, by: -6))
  checkSequence([Int16.min], stride(from: Int16.min, through: Int16.min, by: -5))
  checkSequence(Array(Int8.min...Int8.max), stride(from: Int8.min, through: Int8.max, by: 1))
}

runAllTests()
