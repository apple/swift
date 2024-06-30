// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %env-SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=legacy %target-run %t/a.out

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: concurrency_runtime

// REQUIRES: libdispatch

// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: back_deploy_concurrency
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: freestanding

import StdlibUnittest

final class NaiveQueueExecutor: SerialExecutor {
  init() {}

  public func enqueue(_ job: consuming ExecutorJob) {
    job.runSynchronously(on: self.asUnownedSerialExecutor())
  }

  public func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    UnownedSerialExecutor(ordinary: self)
  }

  func checkIsolated() throws {
    print("checkIsolated: pretend it is ok!")
  }
}

actor ActorOnNaiveQueueExecutor {
  let executor: NaiveQueueExecutor

  init() {
    self.executor = NaiveQueueExecutor()
  }

  nonisolated var unownedExecutor: UnownedSerialExecutor {
    self.executor.asUnownedSerialExecutor()
  }

  // Executes on global pool, but our `checkIsolated` impl pretends
  // that it is the same executor by never crashing.
  nonisolated func checkPreconditionIsolated() async {
    print("Before preconditionIsolated")
    self.preconditionIsolated()
    print("After preconditionIsolated")
  }
}

@main struct Main {
  static func main() async {
    let tests = TestSuite("AssertPreconditionIsolationTests")

    if #available(SwiftStdlib 6.0, *) {
      tests.test("[legacy mode] expect crash since unable to invoke 'checkIsolated'") {
        expectCrashLater() // In legacy mode we do NOT invoke 'checkIsolated' and therefore will crash

        let actor = ActorOnNaiveQueueExecutor()
        await actor.checkPreconditionIsolated()
      }
    }

    await runAllTestsAsync()
  }
}
