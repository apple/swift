// RUN: %target-run-simple-swift(-I %S/Inputs/ -Xfrontend -enable-experimental-cxx-interop)
//
// REQUIRES: executable_test

import StdlibUnittest
import Methods

var CxxMethodTestSuite = TestSuite("CxxMethods")

CxxMethodTestSuite.test("() -> Void") {
  var instance = HasMethods()

  instance.nonConstMethod()
  instance.constMethod()
}

CxxMethodTestSuite.test("(Int) -> Int") {
  var instance = HasMethods()

  expectEqual(42, instance.nonConstPassThrough(42))
  expectEqual(42, instance.constPassThrough(42))
}

CxxMethodTestSuite.test("(Int, Int) -> Int") {
  var instance = HasMethods()

  expectEqual(42, instance.nonConstSum(40, 2))
  expectEqual(42, instance.constSum(40, 2))
}

CxxMethodTestSuite.test("(NonTrivialInWrapper, NonTrivialInWrapper) -> Int") {
  var instance = HasMethods()

  expectEqual(42, instance.nonConstSum(NonTrivialInWrapper(40), NonTrivialInWrapper(2)))
  expectEqual(42, instance.constSum(NonTrivialInWrapper(40), NonTrivialInWrapper(2)))
}

CxxMethodTestSuite.test("(NonTrivialInWrapper, NonTrivialInWrapper) -> NonTrivialInWrapper") {
  var instance = HasMethods()

  expectEqual(42, instance.nonConstSumAsWrapper(NonTrivialInWrapper(40), NonTrivialInWrapper(2)).value)
  expectEqual(42, instance.constSumAsWrapper(NonTrivialInWrapper(40), NonTrivialInWrapper(2)).value)
}

CxxMethodTestSuite.test("(Int) -> NonTrivialInWrapper") {
  var instance = HasMethods()

  expectEqual(42, instance.nonConstPassThroughAsWrapper(42).value)
  expectEqual(42, instance.constPassThroughAsWrapper(42).value)
}

CxxMethodTestSuite.test("Constructor with ref params") {
  let a = CInt(42)
  let b = CInt(11)
  var instance = ReferenceParams(a, b)

  expectEqual(42, instance.a)
  expectEqual(11, instance.b)
}

// Just make sure we don't crash
CxxMethodTestSuite.test("Static method with ref params") {
  let a = CInt(42)
  let b = CInt(11)
  ReferenceParams.staticMethod(a, b)
}

runAllTests()
