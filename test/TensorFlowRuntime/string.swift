// RUN: %target-run-simple-swift %swift-tensorflow-test-run-extra-options
// REQUIRES: executable_test
//
// String Tensor tests.

import TensorFlow
import StdlibUnittest

var StringTensorTests = TestSuite("String")

StringTensorTests.test("StringComparison") {
  let t1 = StringTensor("foo")
  let result1 = t1.elementsEqual(t1)
  expectEqual(ShapedArray(shape: [], scalars: [true]), result1.array)

  let t2 = StringTensor(["foo", "bar"])
  let result2 = t2.elementsEqual(t2)
  expectEqual(ShapedArray(shape: [2], scalars: [true, true]),
              result2.array)

  let t3 = StringTensor(["different", "bar"])
  let result3 = t2.elementsEqual(t3)
  expectEqual(ShapedArray(shape: [2], scalars: [false, true]),
              result3.array)
}

StringTensorTests.test("StringTFOP") {
  let encoded = StringTensor("aGVsbG8gd29ybGQ=")
  let decoded: StringTensor = Raw.decodeBase64(encoded)
  let expectedDecoded = StringTensor("hello world")
  let comparison = expectedDecoded.elementsEqual(decoded)
  expectEqual(ShapedArray(shape: [], scalars: [true]), comparison.array)
}

runAllTests()
