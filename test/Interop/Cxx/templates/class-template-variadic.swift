// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop)
//
// REQUIRES: executable_test

import ClassTemplateVariadic
import StdlibUnittest

var TemplatesTestSuite = TestSuite("TemplatesTestSuite")

TemplatesTestSuite.test("typedeffed-variadic-class-template") {
  let a = IntWrapper(value: 10)
  let b = IntWrapper(value: 20)

  var pair = Pair()
  pair.set(a, b)

  var pairA = pair.first()
  var restB = pair.rest()
  var pairB = restB.first()
  expectEqual(pairA.getValue(), 10)
  expectEqual(pairB.getValue(), 20)
}

// TODO: This test doesn't work because Swift sees this as passing in too many
// generic arguments (https://github.com/apple/swift/issues/55701).
// TemplatesTestSuite.test("variadic-class-template") {
//   let a = IntWrapper(value: 10)
//   let b = IntWrapper(value: 20)

//   var pair = Tuple<IntWrapper, IntWrapper>()
//   pair.set(a, b)

//   var pairA = pair.first()
//   var restB = pair.rest()
//   var pairB = restB.first()
//   expectEqual(pairA.getValue(), 10)
//   expectEqual(pairB.getValue(), 20)
// }

runAllTests()
