// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library )

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch

// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

import Dispatch
import StdlibUnittest
import _Concurrency

// for sleep
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Android)
import Android
#endif

final class QueueSerialExecutor: SerialExecutor {
  let queue: DispatchQueue

  init(_ queue: DispatchQueue) {
    self.queue = queue
  }

  public func enqueue(_ _job: consuming ExecutorJob) {
    let job = UnownedJob(_job)
    queue.async {
      job.runSynchronously(on: self.asUnownedSerialExecutor())
    }
  }

  func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    UnownedSerialExecutor(ordinary: self)
  }
}

final class QueueTaskExecutor: TaskExecutor {
  let queue: DispatchQueue

  init(_ queue: DispatchQueue) {
    self.queue = queue
  }

  public func enqueue(_ _job: consuming ExecutorJob) {
    let job = UnownedJob(_job)
    queue.async {
      job.runSynchronously(on: self.asUnownedTaskExecutor())
    }
  }
}

actor ThreaddyTheDefaultActor {
  func actorIsolated(expectedExecutor: QueueTaskExecutor) async {
    self.assertIsolated()
    dispatchPrecondition(condition: .onQueue(expectedExecutor.queue))
  }

  var phase: Phase = .start
  enum Phase: String {
    case start = "Start"
    case hello = "Hello"
    case world = "World"
  }

  func printBlockingSleepPrint(name: String) -> String {
    print("Enter: \(phase) - \(name)")
    precondition(phase == .start, "Must start method call after previous had completed!")
    defer { phase = .start }

    phase = .hello
    print("- Before sleep: \(phase) - \(name)")
    var reply = "\(phase)"

    sleep(3)

    phase = .world
    print("- After sleep: \(phase) - \(name)")
    reply = "\(reply) \(phase)"

    return reply
  }
}

actor CharlieTheCustomExecutorActor {
  let executor: QueueSerialExecutor

  init(executor: QueueSerialExecutor) {
    self.executor = executor
  }

  nonisolated var unownedExecutor: UnownedSerialExecutor {
    self.executor.asUnownedSerialExecutor()
  }

  func actorIsolated(expectedExecutor: QueueSerialExecutor,
                     notExpectedExecutor: QueueTaskExecutor) async {
    self.assertIsolated()
    dispatchPrecondition(condition: .onQueue(expectedExecutor.queue))
    dispatchPrecondition(condition: .notOnQueue(notExpectedExecutor.queue))
  }
}

@main struct Main {
  static func main() async {
    let tests = TestSuite("\(#fileID)")

    let serialExecutor = QueueSerialExecutor(DispatchQueue(label: "serial-exec-queue"))
    let taskExecutor = QueueTaskExecutor(DispatchQueue(label: "task-executor-queue"))

    tests.test("'default actor' should execute on present task executor preference, and keep isolation") {
      let defaultActor = ThreaddyTheDefaultActor()

      await Task(executorPreference: taskExecutor) {
        dispatchPrecondition(condition: .onQueue(taskExecutor.queue))
        await defaultActor.actorIsolated(expectedExecutor: taskExecutor)
      }.value

      await withTaskExecutorPreference(taskExecutor) {
        await defaultActor.actorIsolated(expectedExecutor: taskExecutor)
      }
    }

    tests.test("'custom executor actor' should NOT execute on present task executor preference, and keep isolation") {
      let customActor = CharlieTheCustomExecutorActor(executor: serialExecutor)

      await Task(executorPreference: taskExecutor) {
        dispatchPrecondition(condition: .onQueue(taskExecutor.queue))
        await customActor.actorIsolated(
          expectedExecutor: serialExecutor,
          notExpectedExecutor: taskExecutor)
      }.value

      await withTaskExecutorPreference(taskExecutor) {
        await customActor.actorIsolated(
          expectedExecutor: serialExecutor,
          notExpectedExecutor: taskExecutor)
      }
    }

    tests.test("'default actor' must not be entered concurrently from non-task executor and task executor") {
      // The serial executor must be used to serialize the actor execution properly.
      let defaultActor = ThreaddyTheDefaultActor()

      let t1 = Task {
        let reply = await defaultActor.printBlockingSleepPrint(name: "Task()")
        print("= Task() got reply: \(reply)")
      }

      let t2 = Task(executorPreference: taskExecutor) {
        let reply = await defaultActor.printBlockingSleepPrint(name: "Task(executorPreference:)")
        print("= Task(executorPreference:) got reply: \(reply)")
      }

      _ = await [t1.value, t2.value]
    }

    await runAllTestsAsync()
  }
}
