// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library )

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
  expectedExecutor.assertIsolated()
}

actor Worker {
  let executor: NaiveQueueExecutor

  init(on executor: NaiveQueueExecutor) {
    self.executor = executor
  }

  func test(_ expectedExecutor: NaiveQueueExecutor) async {
    // we are isolated to the serial-executor (!)
    dispatchPrecondition(condition: .onQueue(expectedExecutor.queue))
    expectedExecutor.preconditionIsolated()

    // the nonisolated async func properly executes on the task-executor
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

    // we properly came back to the serial executor, just to make sure
    dispatchPrecondition(condition: .onQueue(expectedExecutor.queue))
    expectedExecutor.preconditionIsolated()
  }
}

@main struct Main {

  static func main() async {
    let queue = DispatchQueue(label: "example-queue")
    let executor = NaiveQueueExecutor(queue)

    await Task(executorPreference: executor) {
      let worker = Worker(on: executor)
      await worker.test(executor)
    }.value
  }
}
