// RUN: %target-run-simple-swift(-Xfrontend -enable-experimental-cxx-interop)

// REQUIRES: executable_test
// REQUIRES: OS=linux-gnu || OS=linux-android

#if canImport(Glibc)
import Glibc
#elseif canImport(Android)
import Android
#else
#error ("unsupported platform")
#endif
import StdlibUnittest

var LibcTests = TestSuite("LibcTests")

LibcTests.test("abs") {
  expectEqual(42, abs(-42))
}

runAllTests()
