// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %env-SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=swift6 %target-run %t/a.out
// RUN: %env-SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=legacy not --crash %target-run %t/a.out

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: concurrency_runtime

// REQUIRES: libdispatch

// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: back_deploy_concurrency
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: freestanding

// Disable this test on simulators
// UNSUPPORTED: DARWIN_SIMULATOR=watchos
// UNSUPPORTED: DARWIN_SIMULATOR=ios
// UNSUPPORTED: DARWIN_SIMULATOR=tvos
// UNSUPPORTED: single_threaded_concurrency

import Dispatch

@available(SwiftStdlib 6.0, *)
final class NaiveOnMainQueueExecutor: SerialExecutor {
  init() {}

  let mainQueue = DispatchQueue.main

  public func enqueue(_ _job: consuming ExecutorJob) {
    let job = UnownedJob(_job)
    mainQueue.async {
      job.runSynchronously(on: self.asUnownedSerialExecutor())
    }
  }

  public func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    UnownedSerialExecutor(complexEquality: self)
  }

  public func checkIsolated() throws {
    print("\(Self.self).checkIsolated...")
    dispatchPrecondition(condition: .onQueue(self.mainQueue))
  }
}

actor ActorOnNaiveOnMainQueueExecutor {
  let executor: NaiveOnMainQueueExecutor

  init() {
    self.executor = NaiveOnMainQueueExecutor()
  }

  nonisolated var unownedExecutor: UnownedSerialExecutor {
    self.executor.asUnownedSerialExecutor()
  }

  @MainActor
  func checkPreconditionIsolated() async {
    print("Before preconditionIsolated")
    self.preconditionIsolated()
    print("After preconditionIsolated")
  }
}

@main struct Main {
  static func main() async {
    if #available(SwiftStdlib 6.0, *) {
      let actor = ActorOnNaiveOnMainQueueExecutor()
      await actor.checkPreconditionIsolated()
      // CHECK: Before preconditionIsolated
      // CHECK-NEXT: checkIsolated: pretend it is ok!
      // CHECK-NEXT: After preconditionIsolated
    }
  }
}
