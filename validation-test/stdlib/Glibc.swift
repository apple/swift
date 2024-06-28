// RUN: %target-run-simple-swift
// REQUIRES: executable_test
//
// UNSUPPORTED: OS=macosx
// UNSUPPORTED: OS=ios
// UNSUPPORTED: OS=tvos
// UNSUPPORTED: OS=watchos

// REQUIRES: OS=linux-gnu

import Swift
import StdlibUnittest


import Glibc

var GlibcTestSuite = TestSuite("Glibc")

GlibcTestSuite.test("errno") {
  errno = 0
  expectEqual(0, errno)
  close(-1)
  expectEqual(EBADF, errno)
}

GlibcTestSuite.test("sendfile") {
  // Check that `sendfile` is available.  Don't actually call it, because doing that is non-trivial.
  _ = sendfile
}

var GlibcIoctlConstants = TestSuite("GlibcIoctlConstants")

GlibcIoctlConstants.test("tty ioctl constants availability") {
  let aConstant = TIOCSTI
}

runAllTests()
