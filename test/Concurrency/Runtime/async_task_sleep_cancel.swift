// RUN: %target-run-simple-swift(-Xfrontend -enable-experimental-concurrency %import-libdispatch -parse-as-library) | %FileCheck %s --dump-input always
// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch

// rdar://76038845
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import _Concurrency
// FIXME: should not depend on Dispatch
import Dispatch

@available(SwiftStdlib 5.5, *)
@main struct Main {
  static let pause = 500_000_000 // 500ms

  static func main() async {
    // CHECK: Starting!
    print("Starting!")
    await testSleepFinished()
    await testSleepMomentary()
    await testSleepCancelledBeforeStarted()
    await testSleepCancelled()
  }

  static func testSleepFinished() async {
    // CHECK-NEXT: Testing sleep that completes
    print("Testing sleep that completes")
    let start = DispatchTime.now()

    // try! will fail if the task got cancelled (which shouldn't happen).
    try! await Task.sleep(nanoseconds: UInt64(pause))

    let stop = DispatchTime.now()

    // assert that at least the specified time passed since calling `sleep`
    assert(stop >= (start + .nanoseconds(pause)))

    // CHECK-NEXT: Wakey wakey!
    print("Wakey wakey!")
  }

  static func testSleepMomentary() async {
    // CHECK-NEXT: Testing sleep that completes instantly
    print("Testing sleep that completes instantly")

    // try! will fail if the task got cancelled (which shouldn't happen).
    try! await Task.sleep(nanoseconds: 0)

    // CHECK-NEXT: Wakey wakey!
    print("Wakey wakey!")
  }

  static func testSleepCancelledBeforeStarted() async {
    // CHECK-NEXT: Testing sleep that gets cancelled before it starts
    print("Testing sleep that gets cancelled before it starts")
    let sleepyTask = Task {
      try await Task.sleep(nanoseconds: UInt64(pause))
    }

    do {
      sleepyTask.cancel()
      try await sleepyTask.value

      print("Bah, weird scheduling")
    } catch is CancellationError {
      print("Caught the cancellation error")
    } catch {
      fatalError("sleep(nanoseconds:) threw some other error: \(error)")
    }

    // CHECK: Cancelled!
    print("Cancelled!")
  }

  static func testSleepCancelled() async {
    // CHECK-NEXT: Testing sleep that gets cancelled before it completes
    print("Testing sleep that gets cancelled before it completes")
    let start = DispatchTime.now()

    let sleepyTask = Task {
      try await Task.sleep(nanoseconds: UInt64(pause))
    }

    do {
      let waiterTask = Task {
        try await sleepyTask.value
      }

      let cancellerTask = Task {
        await Task.sleep(UInt64(pause / 2))
        sleepyTask.cancel()
      }

      try await waiterTask.value

      fatalError("sleep(nanoseconds:) should have thrown CancellationError")
    } catch is CancellationError {
      // CHECK-NEXT: Caught the cancellation error
      print("Caught the cancellation error")

      let stop = DispatchTime.now()

      // assert that we stopped early.
      assert(stop < (start + .nanoseconds(pause)))
    } catch {
      fatalError("sleep(nanoseconds:) threw some other error: \(error)")
    }

    // CHECK-NEXT: Cancelled!
    print("Cancelled!")
  }
}
