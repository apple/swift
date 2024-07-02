// RUN: %target-run-simple-swift %t
// REQUIRES: executable_test
// UNSUPPORTED: OS=windows-msvc
// UNSUPPORTED: OS=wasi

import StdlibUnittest
#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#elseif canImport(Android)
  import Android
  // MAP_FAILED is not available on android.
  let MAP_FAILED = UnsafeMutableRawPointer(bitPattern: -1)
#else
#error("Unsupported platform")
#endif

var MMapTests = TestSuite("MMaptests")

MMapTests.test("MAP_FAILED") {
  expectEqual(mmap(nil, 0, 0, 0, 0, 0), MAP_FAILED)
}

runAllTests()
