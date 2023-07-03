// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library)
//
// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch
// REQUIRES: concurrency_runtime

import StdlibUnittest

import CxxStdlib
import Cxx

import _Concurrency
import Dispatch

@main struct Main {
  static func main() async {
    var ConcurrencyTestSuite = TestSuite("Concurrency")

    ConcurrencyTestSuite.test("Task.sleep") {
      let start = DispatchTime.now()
      await Task.sleep(100_000_000)
      let stop = DispatchTime.now()
      expectTrue(stop >= (start + .nanoseconds(100_000_000)))
    }

    ConcurrencyTestSuite.test("Task.sleep (non-blocking)") {
      let task = detach {
        std.string("Hello, Swift!")
      }

      await Task.sleep(100_000_000)
      expectEqual(await task.get(), "Hello, Swift!")
    }
    
    await runAllTestsAsync()
  }
}

