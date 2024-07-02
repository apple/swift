// RUN: %target-run-simple-swift 2>&1 | %FileCheck %s
// REQUIRES: executable_test
// FIXME: this test is failing for watchos <rdar://problem/29997111>
// UNSUPPORTED: OS=watchos

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

_setTestSuiteFailedCallback() { print("abort()") }

//
// Test that harness aborts when a test crashes if a child process crashes
// after all tests have finished running.
//

var TestSuiteChildCrashes = TestSuite("TestSuiteChildCrashes")

TestSuiteChildCrashes.test("passes") {
  atexit {
    fatalError("Crash at exit")
  }
}

// CHECK: [ RUN      ] TestSuiteChildCrashes.passes
// CHECK: [       OK ] TestSuiteChildCrashes.passes
// CHECK: TestSuiteChildCrashes: All tests passed
// CHECK: stderr>>> {{.*}}Fatal error: Crash at exit
// CHECK: stderr>>> CRASHED: SIG
// CHECK: The child process failed during shutdown, aborting.
// CHECK: abort()

runAllTests()

