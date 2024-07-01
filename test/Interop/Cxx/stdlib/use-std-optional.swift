// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -cxx-interoperability-mode=swift-5.9)
// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -cxx-interoperability-mode=swift-6)
// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -cxx-interoperability-mode=upcoming-swift)
// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -cxx-interoperability-mode=upcoming-swift -Xcc -std=c++20)
//
// REQUIRES: executable_test

import StdlibUnittest
import StdOptional
import CxxStdlib

var StdOptionalTestSuite = TestSuite("StdOptional")

StdOptionalTestSuite.test("pointee") {
  let nonNilOpt = getNonNilOptional()
  let pointee = nonNilOpt.pointee
  expectEqual(123, pointee)

#if !os(Linux) // crashes on Ubuntu 18.04 (rdar://113414160)
  var modifiedOpt = getNilOptional()
  modifiedOpt.pointee = 777
  expectEqual(777, modifiedOpt.pointee)
#endif
}

StdOptionalTestSuite.test("std::optional => Swift.Optional") {
  let nonNilOpt = getNonNilOptional()
  let swiftOptional = Optional(fromCxx: nonNilOpt)
  expectNotNil(swiftOptional)
  expectEqual(123, swiftOptional!)

  let nilOpt = getNilOptional()
  let swiftNil = Optional(fromCxx: nilOpt)
  expectNil(swiftNil)
}

StdOptionalTestSuite.test("std::optional hasValue/value") {
  let nonNilOpt = getNonNilOptional()
  expectTrue(nonNilOpt.hasValue)
  expectEqual(123, nonNilOpt.value!)

  let nilOpt = getNilOptional()
  expectFalse(nilOpt.hasValue)
  expectNil(nilOpt.value)
}

StdOptionalTestSuite.test("std::optional as ExpressibleByNilLiteral") {
  let res1 = takesOptionalInt(nil)
  expectFalse(res1)

  let res2 = takesOptionalString(nil)
  expectFalse(res2)
}

StdOptionalTestSuite.test("std::optional init(_:Wrapped)") {
  let optInt = StdOptionalInt(123)
  expectEqual(123, optInt.pointee)

  let optBoolT = StdOptionalBool(true)
  let optBoolF = StdOptionalBool(false)
  expectTrue(optBoolT.pointee)
  expectFalse(optBoolF.pointee)

  let optString = StdOptionalString(std.string("abc"))
  expectEqual(std.string("abc"), optString.pointee)

  let optOptInt = StdOptionalOptionalInt(StdOptionalInt(456))
  expectEqual(456, optOptInt.pointee.pointee)

  let optConstexprCtor = StdOptionalHasConstexprCtor(HasConstexprCtor(321))
  expectEqual(321, optConstexprCtor.pointee.value)
}

runAllTests()
