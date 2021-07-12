// RUN: %target-run-simple-swift(-Xfrontend -enable-cxx-interop)

// REQUIRES: executable_test
// REQUIRES: OS=linux-gnu || OS=linux-android

import Glibc
import StdlibUnittest

var GlibcTests = TestSuite("GlibcTests")

GlibcTests.test("abs") {
  expectEqual(42, abs(-42))
}

runAllTests()
