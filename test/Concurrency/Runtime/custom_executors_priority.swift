// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library) | %FileCheck %s --dump-input=always

// REQUIRES: concurrency
// REQUIRES: executable_test

// rdar://106849189 move-only types should be supported in freestanding mode
// UNSUPPORTED: freestanding

// UNSUPPORTED: back_deployment_runtime
// REQUIRES: concurrency_runtime

final class InlineExecutor: SerialExecutor {
  public func enqueue(_ job: consuming ExecutorJob) {
    print("\(self): enqueue (priority: \(TaskPriority(job.priority)!))")
    job.runSynchronously(on: self.asUnownedSerialExecutor())
  }
}

let inlineExecutor = InlineExecutor()

actor Custom {
  var count = 0

  @available(SwiftStdlib 5.1, *)
  nonisolated var unownedExecutor: UnownedSerialExecutor {
    print("custom unownedExecutor")
    return inlineExecutor.asUnownedSerialExecutor()
  }

  func report() async {
    print("custom.count == \(count)")
    count += 1
  }
}

@main struct Main {
  static func main() async {
    print("begin")
    let actor = Custom()
    await Task(priority: .high) {
      await actor.report()
    }.value
    await Task() {
      await actor.report()
    }.value
    print("end")
  }
}

// CHECK:      begin
// CHECK-NEXT: custom unownedExecutor
// CHECK-NEXT: main.InlineExecutor: enqueue (priority: TaskPriority.high)
// CHECK-NEXT: custom.count == 0
// CHECK-NEXT: custom unownedExecutor
// CHECK-NEXT: main.InlineExecutor: enqueue (priority: TaskPriority.medium)
// CHECK-NEXT: custom.count == 1
// CHECK-NEXT: end
