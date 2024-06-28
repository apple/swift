// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=swift-6)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=upcoming-swift)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=upcoming-swift -Xcc -std=c++14)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=upcoming-swift -Xcc -std=c++17)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=upcoming-swift -Xcc -std=c++20)

// REQUIRES: executable_test

// rdar://130527427
// REQUIRES: rdar130527427

import StdlibUnittest
import StdFunction
import CxxStdlib

var StdFunctionTestSuite = TestSuite("StdFunction")

StdFunctionTestSuite.test("FunctionIntToInt.init()") {
  let f = FunctionIntToInt()
  expectTrue(isEmptyFunction(f))

  let copied = f
  expectTrue(isEmptyFunction(copied))
}

StdFunctionTestSuite.test("FunctionIntToInt.callAsFunction") {
  let f = getIdentityFunction()
  expectEqual(123, f(123))
}

StdFunctionTestSuite.test("FunctionIntToInt retrieve and pass back as parameter") {
  let res = invokeFunction(getIdentityFunction(), 456)
  expectEqual(456, res)
}

#if !os(Windows) // FIXME: rdar://103979602
StdFunctionTestSuite.test("FunctionIntToInt init from closure and call") {
  let cClosure: @convention(c) (Int32) -> Int32 = { $0 + 1 }
  let f = FunctionIntToInt(cClosure)
  expectEqual(1, f(0))
  expectEqual(124, f(123))
  expectEqual(0, f(-1))

  let f2 = FunctionIntToInt({ $0 * 2 })
  expectEqual(0, f2(0))
  expectEqual(246, f2(123))
}

StdFunctionTestSuite.test("FunctionIntToInt init from closure and pass as parameter") {
  let res = invokeFunction(.init({ $0 * 2 }), 111)
  expectEqual(222, res)
}

// FIXME: assertion for address-only closure params (rdar://124501345)
//StdFunctionTestSuite.test("FunctionStringToString init from closure and pass as parameter") {
//  let res = invokeFunctionTwice(.init({ $0 + std.string("abc") }),
//                                std.string("prefix"))
//  expectEqual(std.string("prefixabcabc"), res)
//}
#endif

runAllTests()
