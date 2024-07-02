// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import StdlibUnittest
#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#elseif os(Windows)
  import MSVCRT
#elseif canImport(Android)
  import Android
#else
#error("Unsupported platform")
#endif

//
// Test that harness correctly handles the case when there is no child process
// to terminate during shutdown because it crashed during test execution.
//

var TestSuiteChildCrashes = TestSuite("TestSuiteChildCrashes")

TestSuiteChildCrashes.test("passes") {
  expectCrashLater()
  fatalError("crash")
}

runAllTests()

