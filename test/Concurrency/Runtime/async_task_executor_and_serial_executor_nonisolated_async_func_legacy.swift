// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %env-SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=legacy %target-run %t/a.out

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch

// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

import Dispatch
import StdlibUnittest
import _Concurrency

final class NaiveQueueExecutor: TaskExecutor, SerialExecutor {
  let queue: DispatchQueue

  init(_ queue: DispatchQueue) {
    self.queue = queue
  }

  public func enqueue(_ _job: consuming ExecutorJob) {
    let job = UnownedJob(_job)
    queue.async {
      job.runSynchronously(
        isolatedTo: self.asUnownedSerialExecutor(),
        taskExecutor: self.asUnownedTaskExecutor())
    }
  }

  public func checkIsolated() {
    print("\(Self.self).\(#function)")
    dispatchPrecondition(condition: .onQueue(self.queue))
  }

  @inlinable
  public func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    UnownedSerialExecutor(complexEquality: self)
  }

  @inlinable
  public func asUnownedTaskExecutor() -> UnownedTaskExecutor {
    UnownedTaskExecutor(ordinary: self)
  }
}

nonisolated func nonisolatedFunc(expectedExecutor: NaiveQueueExecutor) async {
  dispatchPrecondition(condition: .onQueue(expectedExecutor.queue))
  expectedExecutor.preconditionIsolated()
}

actor DefaultActor {

  func testWithTaskExecutorPreferenceTask(_ expectedExecutor: NaiveQueueExecutor) async {
    withUnsafeCurrentTask { task in
      precondition(task?.unownedTaskExecutor != nil, "This test expects to be called with a task executor preference")
    }

    // we always must be on the "self" isolation context
    self.preconditionIsolated() // baseline soundness check

    // we are on this queue because we were invoked with a task executor preference
    dispatchPrecondition(condition: .onQueue(expectedExecutor.queue))

    // The following precondition would, we are isolated to the 'default actor',
    // and without calling 'checkIsolated' in this legacy mode,
    // we can't know that it's "actually the same queue"
    //     expectedExecutor.preconditionIsolated()

    // calling a nonisolated async func properly executes on the task-executor
    await nonisolatedFunc(expectedExecutor: expectedExecutor)

    // the task-executor preference is inherited properly:
    async let val = {
      dispatchPrecondition(condition: .onQueue(expectedExecutor.queue))
      expectedExecutor.preconditionIsolated()
      return 12
    }()
    _ = await val

    // as expected not-inheriting
    _ = await Task.detached {
      dispatchPrecondition(condition: .notOnQueue(expectedExecutor.queue))
    }.value

    _ = await Task.detached {
      dispatchPrecondition(condition: .notOnQueue(expectedExecutor.queue))
    }.value

    // we properly came back to the serial executor, just to make sure
    dispatchPrecondition(condition: .onQueue(expectedExecutor.queue))

    // The following precondition would, we are isolated to the 'default actor',
    // and without calling 'checkIsolated' in this legacy mode,
    // we can't know that it's "actually the same queue"
    //     expectedExecutor.preconditionIsolated()
  }
}

@main struct Main {

  static func main() async {
    let executor = NaiveQueueExecutor(DispatchQueue(label: "example-queue"))

    let actor = DefaultActor()

    await Task(executorPreference: executor) {
      await actor.testWithTaskExecutorPreferenceTask(executor)
    }.value
  }
}
