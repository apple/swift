// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop)
// REQUIRES: executable_test

import StdlibUnittest
import CxxStdlib
import StdSharedPtr

var StdSharedPtrTestSuite = TestSuite("StdSharedPtr")

StdSharedPtrTestSuite.test("SharedPtrOfInt.pointee") {
  var s = getSharedPtrOfInt()
  expectEqual(s.pointee, 123)

  s.pointee = 456
  expectEqual(s.pointee, 456)

  s.pointee += 543
  expectEqual(s.pointee, 999)
}

#if !os(Windows) // FIXME: enable once swiftCxxStdlib is built on Windows (https://github.com/apple/swift/issues/67649)
StdSharedPtrTestSuite.test("SharedPtrOfString.pointee") {
  var s = getSharedPtrOfString()
  expectEqual(s.pointee, std.string("abc123"))

  s.pointee = std.string()
  expectEqual(s.pointee, std.string())

  s.pointee = std.string("0123")
  expectEqual(s.pointee, std.string("0123"))
}
#endif

runAllTests()
