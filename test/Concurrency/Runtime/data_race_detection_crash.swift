// RUN: %empty-directory(%t)
// RUN: %target-build-swift %import-libdispatch -Xfrontend -disable-availability-checking -enable-actor-data-race-checks -parse-as-library %s -o %t/a.out -module-name main
// RUN: %target-codesign %t/a.out

// NOTE: This test specifically tests the crashing behavior of `checkIsolated`,
// because this behavior is currently disabled
// RUN: env %env-SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=crash %target-run %t/a.out

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch

// rdar://76038845
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: single_threaded_concurrency

import StdlibUnittest
import _Concurrency
import Dispatch

// For sleep
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif

@MainActor func onMainActor() {
  print("I'm on the main actor!")
}

func promiseMainThread(_ fn: @escaping @MainActor () -> Void) -> (() -> Void) {
  typealias Fn = () -> Void
  return unsafeBitCast(fn, to: Fn.self)
}

func launchTask(_ fn: @escaping () -> Void) {
  if #available(macOS 10.10, iOS 8.0, watchOS 2.0, tvOS 8.0, *) {
    DispatchQueue.global().async {
      fn()
    }
  }
}

func launchFromMainThread() {
  launchTask(promiseMainThread(onMainActor))
}

actor MyActor {
  var counter = 0

  func onMyActor() {
    dispatchPrecondition(condition: .notOnQueue(DispatchQueue.main))
    counter = counter + 1
  }

  func getTaskOnMyActor() -> (() -> Void) {
    return {
      self.onMyActor()
    }
  }
}

/// These tests now eventually end up calling `dispatch_assert_queue`,
/// after the introduction of checkIsolated in `swift_task_isCurrentExecutorImpl`
@main struct Main {
  static func main() {
    if #available(SwiftStdlib 5.9, *) {
      let tests = TestSuite("data_race_detection")

      tests.test("Expect MainActor") {
        expectCrashLater()
        print("Launching a main-actor task")
        launchFromMainThread()
        sleep(1)
      }

      tests.test("Expect same executor") {
        expectCrashLater(withMessage: "Incorrect actor executor assumption")
        Task.detached {
          let actor = MyActor()
          let actorFn = await actor.getTaskOnMyActor()
          print("Launching an actor-instance task")
          launchTask(actorFn)
        }

        sleep(2)
      }

      runAllTests()
    }
  }
}
