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
// Test that a test runs in its own child process if asked.
//

enum Globals {
  static var modifiedByChildProcess = false
}

var TestSuiteRequireNewProcess = TestSuite("TestSuiteRequireNewProcess")

TestSuiteRequireNewProcess.test("RequireOwnProcessBefore")
  .code {
  Globals.modifiedByChildProcess = true
}

TestSuiteRequireNewProcess.test("RequireOwnProcess")
  .requireOwnProcess()
  .code {
  expectEqual(false, Globals.modifiedByChildProcess)
  Globals.modifiedByChildProcess = true
}

TestSuiteRequireNewProcess.test("ShouldNotReusePreviousProcess") {
  expectEqual(false, Globals.modifiedByChildProcess)
}

runAllTests()
