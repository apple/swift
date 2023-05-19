// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library)

// REQUIRES: concurrency
// REQUIRES: executable_test
// REQUIRES: libdispatch

// rdar://106849189 move-only types should be supported in freestanding mode
// UNSUPPORTED: freestanding

// UNSUPPORTED: back_deployment_runtime
// REQUIRES: concurrency_runtime

@preconcurrency import Dispatch
@_spi(ConcurrencyExecutors) import _Concurrency
import StdlibUnittest

final class NaiveQueueExecutor: SerialExecutor, CustomStringConvertible {
  let name: String
  let queue: DispatchQueue

  init(name: String, _ queue: DispatchQueue) {
    self.name = name
    self.queue = queue
  }

  public func enqueue(_ job: consuming ExecutorJob) {
    let unowned = UnownedJob(job)
    queue.sync {
      unowned.runSynchronously(on: self.asUnownedSerialExecutor())
    }
  }

  public func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    let ref = UnownedSerialExecutor(complexEquality: self)
    precondition(ref._isComplexEquality, "expected the ref to have complex equality")
    return ref
  }

  public func isSameExclusiveExecutionContext(other: NaiveQueueExecutor) -> Bool {
    if Set([self.name, other.name]) == Set(["one", "two"]) {
      // those we consider equal
      return true
    } else {
      return false
    }
  }

  var description: Swift.String {
    "NaiveQueueExecutor(\(name), \(queue))"
  }
}

actor MyActor {

  nonisolated let executor: NaiveQueueExecutor
  nonisolated var unownedExecutor: UnownedSerialExecutor {
    return executor.asUnownedSerialExecutor()
  }

  init(executor: NaiveQueueExecutor) {
    self.executor = executor
  }

  func test(expectedExecutor: NaiveQueueExecutor) {
    expectedExecutor.preconditionIsolated("Expected deep equality to trigger for \(expectedExecutor) and our \(self.executor)")
    print("\(Self.self): [\(self.executor.name)] on same context as [\(expectedExecutor.name)]")
  }
}

@main
struct Runner {
  static func main() async {
    let tests = TestSuite("Complex executor equality")

    let queue = DispatchQueue(label: "RootQueue")
    let one = NaiveQueueExecutor(name: "one", queue)
    let two = NaiveQueueExecutor(name: "two", queue)

    tests.test("isSameExclusiveContext=true, causes same executor checks to pass") {
      let actor = MyActor(executor: one)
      await actor.test(expectedExecutor: one)
      await actor.test(expectedExecutor: two)
    }
    tests.test("isSameExclusiveContext=false, causes same executor checks to crash") {
      expectCrashLater(withMessage: "Precondition failed: Incorrect actor executor assumption; " +
          "Expected 'NaiveQueueExecutor(unknown)' executor. " +
          "Expected deep equality to trigger for ")

      let unknown = NaiveQueueExecutor(name: "unknown", DispatchQueue(label: "unknown"))
      let actor = MyActor(executor: one)
      await actor.test(expectedExecutor: unknown)
    }

    await runAllTestsAsync()
  }
}
