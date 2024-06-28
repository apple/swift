// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library ) | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch

// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

import Dispatch
import StdlibUnittest
import _Concurrency

// For sleep
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Android)
import Android
#endif

final class NaiveQueueExecutor: TaskExecutor {
  let queue: DispatchQueue
  let sem: DispatchSemaphore

  init(_ sem: DispatchSemaphore, _ queue: DispatchQueue) {
    print("init \(Self.self)")
    self.sem = sem
    self.queue = queue
  }

  deinit {
    print("deinit \(Self.self)")
    sem.signal()
  }

  public func enqueue(_ _job: consuming ExecutorJob) {
    print("Enqueue on \(Self.self)!")
    let job = UnownedJob(_job)
    queue.async {
      job.runSynchronously(on: self.asUnownedTaskExecutor())
    }
  }

  @inlinable
  public func asUnownedTaskExecutor() -> UnownedTaskExecutor {
    print("\(Self.self).\(#function)")
    return UnownedTaskExecutor(ordinary: self)
  }
}

nonisolated func nonisolatedFunc(expectedQueue queue: DispatchQueue) async {
  dispatchPrecondition(condition: .onQueue(queue))
  print("Invoked: \(#function)")
}

@main struct Main {

  static func main() async {
    let queue = DispatchQueue(label: "example-queue")
    let deinitSem = DispatchSemaphore(value: 0)
    var executor: NaiveQueueExecutor? = NaiveQueueExecutor(deinitSem, queue)

    // Task retains the executor, so it should never deinit before the task completes
    // CHECK: init NaiveQueueExecutor

    // The concurrency runtime invokes the...
    // CHECK: NaiveQueueExecutor.asUnownedTaskExecutor

    // And we enqueue on the task executor...
    // CHECK: Enqueue on NaiveQueueExecutor
    // CHECK: Task start

    let task = Task(executorPreference: executor!) {
      print("Task start")
      // CHECK: Invoked: nonisolatedFunc
      await nonisolatedFunc(expectedQueue: queue)
      print("Task done")
    }

    executor = nil
    print("In main: executor = nil")

    await task.value
    // The executor is ONLY released after the task has completed,
    // regardless when the reference in main() was released.
    // CHECK: Task done
    // CHECK-NEXT: deinit NaiveQueueExecutor

    deinitSem.wait()

    print("Done")
    // CHECK: Done
  }
}
