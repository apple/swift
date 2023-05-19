// RUN: %target-run-simple-swift( -Xfrontend -enable-experimental-move-only -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library) | %FileCheck %s

// REQUIRES: concurrency
// REQUIRES: executable_test
// REQUIRES: libdispatch

// rdar://106849189 move-only types should be supported in freestanding mode
// UNSUPPORTED: freestanding

// UNSUPPORTED: back_deployment_runtime
// REQUIRES: concurrency_runtime

@preconcurrency import Dispatch

protocol WithSpecifiedExecutor: Actor {
  nonisolated var executor: any SpecifiedExecutor { get }
}

protocol SpecifiedExecutor: SerialExecutor {}

extension WithSpecifiedExecutor {
  /// Establishes the WithSpecifiedExecutorExecutor as the serial
  /// executor that will coordinate execution for the actor.
  nonisolated var unownedExecutor: UnownedSerialExecutor {
    executor.asUnownedSerialExecutor()
  }
}

final class NaiveQueueExecutor: SpecifiedExecutor, CustomStringConvertible {
  let name: String
  let queue: DispatchQueue

  init(name: String, _ queue: DispatchQueue) {
    self.name = name
    self.queue = queue
  }

  public func enqueue(_ job: consuming ExecutorJob) {
    print("\(self): enqueue")
    let unowned = UnownedJob(job)
    queue.sync {
      unowned.runSynchronously(on: self.asUnownedSerialExecutor())
    }
    print("\(self): after run")
  }

  var description: Swift.String {
    "NaiveQueueExecutor(\(name))"
  }
}

actor MyActor: WithSpecifiedExecutor {

  nonisolated let executor: any SpecifiedExecutor

  // Note that we don't have to provide the unownedExecutor in the actor itself.
  // We obtain it from the extension on `WithSpecifiedExecutor`.

  init(executor: any SpecifiedExecutor) {
    self.executor = executor
  }

  func test(expectedExecutor: some SerialExecutor, expectedQueue: DispatchQueue) {
    // FIXME(waiting on preconditions to merge): expectedExecutor.preconditionIsolated("Expected to be on: \(expectedExecutor)")
    dispatchPrecondition(condition: .onQueue(expectedQueue))
    print("\(Self.self): on executor \(expectedExecutor)")
  }
}

@main struct Main {
  static func main() async {
    print("begin")
    let name = "CustomQueue"
    let queue = DispatchQueue(label: name)
    let one = NaiveQueueExecutor(name: name, queue)
    let actor = MyActor(executor: one)
    await actor.test(expectedExecutor: one, expectedQueue: queue)
    await actor.test(expectedExecutor: one, expectedQueue: queue)
    await actor.test(expectedExecutor: one, expectedQueue: queue)
    print("end")
  }
}

// CHECK:      begin
// CHECK-NEXT: NaiveQueueExecutor(CustomQueue): enqueue
// CHECK-NEXT: MyActor: on executor NaiveQueueExecutor(CustomQueue)
// CHECK-NEXT: MyActor: on executor NaiveQueueExecutor(CustomQueue)
// CHECK-NEXT: MyActor: on executor NaiveQueueExecutor(CustomQueue)
// CHECK-NEXT: NaiveQueueExecutor(CustomQueue): after run
// CHECK-NEXT: end
