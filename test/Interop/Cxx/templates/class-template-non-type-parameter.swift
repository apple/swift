// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop)
//
// REQUIRES: executable_test

import ClassTemplateNonTypeParameter
import StdlibUnittest

var TemplatesTestSuite = TestSuite("TemplatesTestSuite")

TemplatesTestSuite.test("typedeffed-non-type-parameter") {
  let pair = MagicIntPair(t: (1, 2))
  expectEqual(pair.t, (1, 2))

  let triple = MagicIntTriple(t: (1, 2, 3))
  expectEqual(triple.t, (1, 2, 3))
}

// TODO: This test doesn't work because Swift doesn't support defaulted generic
// parameters (https://github.com/apple/swift/issues/55701).
// TemplatesTestSuite.test("defaulted-non-type-parameter") {
//   var intWrapper = IntWrapper(value: 5)
//   var pair = MagicArray<IntWrapper>(t: (intWrapper))
//   expectEqual(pair.t, (intWrapper))
// }

// TODO: This test doesn't work because Swift only expects types as generic
// arguments (https://github.com/apple/swift/issues/55701).
// TemplatesTestSuite.test("non-type-parameter") {
//   var pair = MagicArray<IntWrapper, 5>(
//     data: (
//       IntWrapper(value: 0), IntWrapper(value: 1), IntWrapper(value: 2), IntWrapper(value: 3),
//       IntWrapper(value: 4)
//     ))
//   expectEqual(pair.count, 5)
//   expectEqual(pair.3.value, 3)
// }

runAllTests()
